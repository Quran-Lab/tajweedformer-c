/* The tajweed layer (API_LAYERS.md section 3): the engine's verdicts plus per-word scores, flags, timings
 * and issues, and one JSON with everything. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

const char *tf__model_backend(const TfModel *m);
char *tf__settings_json_alloc(TfModel *m);
TfModel *tf__stream_model(TfStream *s);
int tf__stream_all_heads(const TfStream *s, ZfTajweedHeads *out);

struct TfResult {
  TfModel *m;
  ZfTajweedResult *zr;
  int surah, ayah;
  char backend[8];
  char *settings;
  TfWordScore *words;
  int n_words;
  char *json;
};

static const char *status_name(int s) { return s == 2 ? "validated" : s == 1 ? "experimental" : "off"; }

static int compute_words(TfResult *r) {
  const ZfTajweedSite *sites;
  const ZfTajweedMadd *madd;
  const ZfTajweedLetter *q, *g;
  const int ns = zf_tajweed_result_sites(r->zr, &sites);
  const int nm = zf_tajweed_result_madd(r->zr, &madd);
  const int nq = zf_tajweed_result_qalqalah(r->zr, &q);
  const int ng = zf_tajweed_result_gemination(r->zr, &g);
  int nw = 0;
  for (int i = 0; i < ns; ++i) nw = sites[i].word + 1 > nw ? sites[i].word + 1 : nw;
  for (int i = 0; i < nm; ++i) nw = madd[i].word + 1 > nw ? madd[i].word + 1 : nw;
  for (int i = 0; i < nq; ++i) nw = q[i].word + 1 > nw ? q[i].word + 1 : nw;
  for (int i = 0; i < ng; ++i) nw = g[i].word + 1 > nw ? g[i].word + 1 : nw;
  /* the expected row's word count when the table has the ayah */
  {
    const int *wot;
    int n_words = 0;
    if (zf_tajweed_expected_words(tf_model_engine(r->m), r->surah, r->ayah, &wot, &n_words) >= 0 && n_words > nw) nw = n_words;
  }
  r->words = (TfWordScore *)calloc((size_t)nw + 1, sizeof(TfWordScore));
  if (!r->words) return -1;
  r->n_words = nw;
  int en;
  double th;
  char preset[32], source[16];
  tf__model_word_effective(r->m, &en, &th, preset, sizeof preset, source, sizeof source);
  int *lo = (int *)malloc(sizeof(int) * ((size_t)nw + 1)), *hi = (int *)malloc(sizeof(int) * ((size_t)nw + 1));
  if (!lo || !hi) {
    free(lo);
    free(hi);
    return -1;
  }
  for (int w = 0; w < nw; ++w) {
    TfWordScore *x = &r->words[w];
    x->word = w;
    x->evidence_site = -1;
    x->score = NAN;
    x->has_threshold = en;
    x->threshold = th;
    x->evidence_class = x->evidence_edit = x->evidence_expected = x->evidence_alt = -1;
    x->evidence_llr_final = x->evidence_llr_inter1 = NAN;
    lo[w] = -1;
    hi[w] = -1;
  }
  for (int i = 0; i < ns; ++i) {
    const ZfTajweedSite *s = &sites[i];
    if (s->word < 0 || s->word >= nw) continue;
    TfWordScore *x = &r->words[s->word];
    if (!x->has_score || s->llr > x->score) {
      x->has_score = 1;
      x->score = s->llr;
      x->evidence_site = i;
      x->evidence_class = s->cls;
      x->evidence_edit = s->expected < 0 ? 2 : s->alt < 0 ? 1 : 0;
      x->evidence_expected = s->expected;
      x->evidence_alt = s->alt;
      x->evidence_llr_final = s->llr_final;
      x->evidence_llr_inter1 = s->llr_inter1;
    }
    if (s->flagged) x->n_flagged_sites++;
    if (s->frame_lo >= 0 && (lo[s->word] < 0 || s->frame_lo < lo[s->word])) lo[s->word] = s->frame_lo;
    if (s->frame_hi >= 0 && s->frame_hi > hi[s->word]) hi[s->word] = s->frame_hi;
  }
  for (int i = 0; i < nm; ++i)
    if (madd[i].flagged && madd[i].word >= 0 && madd[i].word < nw) r->words[madd[i].word].n_flagged_madd++;
  for (int i = 0; i < nq; ++i)
    if (q[i].flagged && q[i].word >= 0 && q[i].word < nw) r->words[q[i].word].n_flagged_qalqalah++;
  for (int i = 0; i < ng; ++i)
    if (g[i].flagged && g[i].word >= 0 && g[i].word < nw) r->words[g[i].word].n_flagged_gemination++;
#if ZF_TAJWEED_API_VERSION >= 3
  /* API 3: the engine's word readout (the reference's tajweed_ref.word_scores: every single edit inside the word), its
   * threshold and flag; the site-based maximum above is only the fallback for an API 2 engine */
  {
    const ZfTajweedWord *ew;
    const int new_ = zf_tajweed_result_words(r->zr, &ew);
    for (int i = 0; i < new_; ++i) {
      if (ew[i].word < 0 || ew[i].word >= nw) continue;
      TfWordScore *x = &r->words[ew[i].word];
      x->has_score = ew[i].has_score;
      x->score = ew[i].has_score ? ew[i].score : NAN;
      if (en) x->threshold = ew[i].threshold;
      x->evidence_class = ew[i].has_score ? ew[i].cls : -1;
      x->evidence_edit = ew[i].has_score ? ew[i].edit : -1;
      x->evidence_expected = ew[i].has_score ? ew[i].expected : -1;
      x->evidence_alt = ew[i].has_score ? ew[i].alt : -1;
      x->evidence_llr_final = ew[i].has_score ? ew[i].llr_final : NAN;
      x->evidence_llr_inter1 = ew[i].has_score ? ew[i].llr_inter1 : NAN;
    }
  }
#endif
  for (int w = 0; w < nw; ++w) {
    TfWordScore *x = &r->words[w];
    x->flagged = !en ? -1 : (x->has_score && x->score >= x->threshold) ? 1 : 0; /* words flag at or above tau */
    x->start_ms = lo[w] >= 0 ? 40 * lo[w] : -1;
    x->end_ms = hi[w] >= 0 ? 40 * (hi[w] + 1) : -1;
  }
  free(lo);
  free(hi);
  return 0;
}

TfResult *tf_score(TfModel *m, const ZfTajweedHeads *h, int surah, int ayah, const char *backend) {
  tf__clear_error();
  if (!m || !h) {
    tf__fail(TF_ERR_ARGUMENT, "no model or heads");
    return NULL;
  }
  if (surah < 1 || surah > 114 || ayah < 1) {
    tf__fail(TF_ERR_ARGUMENT, "%d:%d is not an ayah", surah, ayah);
    return NULL;
  }
  char before[8];
  snprintf(before, sizeof before, "%s", tf__model_backend(m) ? tf__model_backend(m) : "");
  ZfTajweedConfig *c = tf_model_engine_config(m);
  if (backend && zf_tajweed_config_set_backend(c, backend)) {
    tf__fail(TF_ERR_SETTINGS, "backend must be int8 or fp32");
    return NULL;
  }
  TfResult *r = (TfResult *)calloc(1, sizeof *r);
  if (!r) {
    tf__fail(TF_ERR_NO_MEMORY, "out of memory");
    goto restore;
  }
  r->m = m;
  r->surah = surah;
  r->ayah = ayah;
  r->settings = tf__settings_json_alloc(m);
  {
    TfJson *sj = r->settings ? tf_json_parse(r->settings, strlen(r->settings), NULL) : NULL;
    const char *b = tf_json_str(tf_json_get(sj, "backend"));
    snprintf(r->backend, sizeof r->backend, "%s", b ? b : "");
    tf_json_free(sj);
  }
  const char *err = NULL;
  r->zr = zf_tajweed_score(tf_model_engine(m), c, h, surah, ayah, &err);
  if (!r->zr) {
    tf__fail(TF_ERR_ENGINE, "score %d:%d: %s", surah, ayah, err ? err : "failed");
    tf_result_free(r);
    r = NULL;
    goto restore;
  }
  if (compute_words(r)) {
    tf__fail(TF_ERR_NO_MEMORY, "out of memory");
    tf_result_free(r);
    r = NULL;
  }
restore:
  if (backend) zf_tajweed_config_set_backend(c, before[0] ? before : NULL);
  return r;
}

TfResult *tf_stream_score(TfStream *s, int surah, int ayah) {
  ZfTajweedHeads h;
  if (!s || tf__stream_all_heads(s, &h) < 0) {
    tf__fail(TF_ERR_ARGUMENT, "no stream heads");
    return NULL;
  }
  return tf_score(tf__stream_model(s), &h, surah, ayah, NULL);
}

void tf_result_free(TfResult *r) {
  if (!r) return;
  if (r->zr) zf_tajweed_result_free(r->zr);
  free(r->settings);
  free(r->words);
  free(r->json);
  free(r);
}
const ZfTajweedResult *tf_result_engine(const TfResult *r) { return r ? r->zr : NULL; }
int tf_result_words(const TfResult *r, const TfWordScore **w) {
  if (!r) return -1;
  if (w) *w = r->words;
  return r->n_words;
}

static void unit(TfBuf *b, const TfModel *m, int id) {
  const char *u = tf_model_token_name(m, id);
  if (u) tf_buf_json_str(b, u);
  else tf_buf_puts(b, "null");
}
static void letters(TfBuf *b, const TfModel *m, const ZfTajweedLetter *x, int n) {
  tf_buf_puts(b, "[");
  for (int i = 0; i < n; ++i) {
    if (i) tf_buf_puts(b, ",");
    tf_buf_printf(b, "{\"pos\":%d,\"word\":%d,\"token\":%d,\"unit\":", x[i].pos, x[i].word, x[i].token);
    unit(b, m, x[i].token);
    tf_buf_puts(b, ",\"status\":");
    tf_buf_json_str(b, status_name(x[i].status));
    tf_buf_puts(b, ",\"score\":");
    tf_buf_num(b, x[i].score);
    tf_buf_puts(b, ",\"threshold\":");
    tf_buf_num(b, x[i].threshold);
    tf_buf_printf(b, ",\"flagged\":%s,\"frameLo\":%d,\"frameHi\":%d}", x[i].flagged ? "true" : "false", x[i].frame_lo, x[i].frame_hi);
  }
  tf_buf_puts(b, "]");
}

const char *tf_result_json(TfResult *r) {
  if (!r) return NULL;
  free(r->json);
  const TfModel *m = r->m;
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "{\"surah\":%d,\"ayah\":%d,\"context\":", r->surah, r->ayah);
  tf_buf_json_str(&b, tf_model_context(m));
  tf_buf_puts(&b, ",\"backend\":");
  tf_buf_json_str(&b, r->backend);
  tf_buf_puts(&b, ",\"settings\":");
  tf_buf_puts(&b, r->settings ? r->settings : "null");
  const ZfTajweedSite *s;
  const int ns = zf_tajweed_result_sites(r->zr, &s);
  tf_buf_puts(&b, ",\"sites\":[");
  for (int i = 0; i < ns; ++i) {
    if (i) tf_buf_puts(&b, ",");
    tf_buf_puts(&b, "{\"class\":");
    tf_buf_json_str(&b, zf_tajweed_class_name(s[i].cls));
    tf_buf_puts(&b, ",\"check\":");
    tf_buf_json_str(&b, zf_tajweed_check_name(zf_tajweed_class_check(s[i].cls)));
    tf_buf_puts(&b, ",\"status\":");
    tf_buf_json_str(&b, status_name(s[i].status));
    tf_buf_printf(&b, ",\"pos\":%d,\"word\":%d,\"expected\":%d,\"expectedUnit\":", s[i].pos, s[i].word, s[i].expected);
    unit(&b, m, s[i].expected);
    tf_buf_printf(&b, ",\"alternative\":%d,\"alternativeUnit\":", s[i].alt);
    unit(&b, m, s[i].alt);
    tf_buf_puts(&b, ",\"llr\":{\"final\":");
    tf_buf_num(&b, s[i].llr_final);
    tf_buf_puts(&b, ",\"inter1\":");
    tf_buf_num(&b, s[i].llr_inter1);
    tf_buf_puts(&b, "},\"fusedLlr\":");
    tf_buf_num(&b, s[i].llr);
    tf_buf_puts(&b, ",\"threshold\":");
    tf_buf_num(&b, s[i].threshold);
    tf_buf_printf(&b, ",\"flagged\":%s,\"frameLo\":%d,\"frameHi\":%d}", s[i].flagged ? "true" : "false", s[i].frame_lo, s[i].frame_hi);
  }
  const ZfTajweedMadd *md;
  const int nm = zf_tajweed_result_madd(r->zr, &md);
  tf_buf_puts(&b, "],\"madd\":[");
  for (int i = 0; i < nm; ++i) {
    if (i) tf_buf_puts(&b, ",");
    tf_buf_printf(&b, "{\"pos\":%d,\"word\":%d,\"token\":%d,\"unit\":", md[i].pos, md[i].word, md[i].token);
    unit(&b, m, md[i].token);
    tf_buf_puts(&b, ",\"maddClass\":");
    tf_buf_json_str(&b, zf_tajweed_madd_class_name(md[i].madd_class));
    tf_buf_puts(&b, ",\"status\":");
    tf_buf_json_str(&b, status_name(md[i].status));
    tf_buf_puts(&b, ",\"ratio\":");
    tf_buf_num(&b, md[i].ratio);
    tf_buf_printf(&b, ",\"count\":%d,\"displayCount\":", md[i].count);
    tf_buf_num(&b, md[i].display_count);
    tf_buf_printf(&b, ",\"allowedLo\":%d,\"allowedHi\":%d,\"flagged\":%s,\"frameLo\":%d,\"frameHi\":%d,\"durationMs\":%d}", md[i].allowed_lo,
                  md[i].allowed_hi, md[i].flagged ? "true" : "false", md[i].frame_lo, md[i].frame_hi,
                  md[i].frame_lo >= 0 && md[i].frame_hi >= 0 ? 20 * (md[i].frame_hi - md[i].frame_lo + 1) : -1);
  }
  const ZfTajweedLetter *q, *g;
  const int nq = zf_tajweed_result_qalqalah(r->zr, &q);
  const int ng = zf_tajweed_result_gemination(r->zr, &g);
  tf_buf_puts(&b, "],\"qalqalah\":");
  letters(&b, m, q, nq < 0 ? 0 : nq);
  tf_buf_puts(&b, ",\"gemination\":");
  letters(&b, m, g, ng < 0 ? 0 : ng);
  tf_buf_printf(&b, ",\"wordReadout\":\"%s\",\"words\":[",
                ZF_TAJWEED_API_VERSION >= 3 ? "engine" : "sites (an API 2 engine: the largest site LLR, not every single edit)");
  for (int w = 0; w < r->n_words; ++w) {
    const TfWordScore *x = &r->words[w];
    if (w) tf_buf_puts(&b, ",");
    tf_buf_printf(&b, "{\"word\":%d,\"score\":", x->word);
    if (x->has_score) tf_buf_num(&b, x->score);
    else tf_buf_puts(&b, "null");
    tf_buf_puts(&b, ",\"threshold\":");
    if (x->has_threshold) tf_buf_num(&b, x->threshold);
    else tf_buf_puts(&b, "null");
    tf_buf_printf(&b, ",\"flagged\":%s,\"evidence\":", x->flagged < 0 ? "null" : x->flagged ? "true" : "false");
    if (x->evidence_class >= 0) {
      static const char *const edits[] = {"sub", "del", "ins"};
      tf_buf_puts(&b, "{\"class\":");
      tf_buf_json_str(&b, zf_tajweed_class_name(x->evidence_class));
      tf_buf_printf(&b, ",\"edit\":\"%s\",\"expectedUnit\":", x->evidence_edit >= 0 && x->evidence_edit < 3 ? edits[x->evidence_edit] : "sub");
      unit(&b, m, x->evidence_expected);
      tf_buf_puts(&b, ",\"alternativeUnit\":");
      unit(&b, m, x->evidence_alt);
      tf_buf_puts(&b, ",\"llr\":{\"final\":");
      tf_buf_num(&b, x->evidence_llr_final);
      tf_buf_puts(&b, ",\"inter1\":");
      tf_buf_num(&b, x->evidence_llr_inter1);
      tf_buf_puts(&b, "},\"fusedLlr\":");
      tf_buf_num(&b, x->score);
      tf_buf_puts(&b, "}");
    } else {
      tf_buf_puts(&b, "null");
    }
    tf_buf_puts(&b, ",\"startMs\":");
    if (x->start_ms >= 0) tf_buf_printf(&b, "%d", x->start_ms);
    else tf_buf_puts(&b, "null");
    tf_buf_puts(&b, ",\"endMs\":");
    if (x->end_ms >= 0) tf_buf_printf(&b, "%d", x->end_ms);
    else tf_buf_puts(&b, "null");
    tf_buf_printf(&b, ",\"issues\":{\"sites\":%d,\"madd\":%d,\"qalqalah\":%d,\"gemination\":%d}}", x->n_flagged_sites, x->n_flagged_madd,
                  x->n_flagged_qalqalah, x->n_flagged_gemination);
  }
  ZfTajweedSummary sum;
  memset(&sum, 0, sizeof sum);
  zf_tajweed_result_summary(r->zr, &sum);
  int wf = 0;
  for (int w = 0; w < r->n_words; ++w) wf += r->words[w].flagged == 1;
  tf_buf_printf(&b,
                "],\"summary\":{\"sites\":%d,\"flagged\":%d,\"madd\":%d,\"maddFlagged\":%d,\"qalqalah\":%d,\"qalqalahFlagged\":%d,"
                "\"gemination\":%d,\"geminationFlagged\":%d,\"words\":%d,\"wordsFlagged\":%d,\"shortVowelFrames\":",
                sum.n_sites, sum.n_flagged, sum.n_madd, sum.n_madd_flagged, sum.n_qalqalah, sum.n_qalqalah_flagged, sum.n_gemination,
                sum.n_gemination_flagged, r->n_words, wf);
  tf_buf_num(&b, sum.short_vowel_frames);
  tf_buf_printf(&b, ",\"maddUnitOk\":%s},\"raw\":", sum.madd_unit_ok ? "true" : "false");
  const char *raw = zf_tajweed_result_json(r->zr);
  tf_buf_puts(&b, raw ? raw : "null");
  tf_buf_puts(&b, "}");
  r->json = tf_buf_take(&b);
  return r->json;
}
