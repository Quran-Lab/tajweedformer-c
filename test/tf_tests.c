/* tf_tests: drives the tajweedformer library through its public API and writes what it produced, for the
 * checkers in test/ (check_*.py) to compare with the reference files. Built against the static library;
 * the internal JSON and crypto helpers are used only to read inputs and to run the crypto vectors.
 *
 *   tf_tests crypto
 *   tf_tests conformance <folder> <key_hex|-> <clips.txt> <out.jsonl> [heads_mask [context]]
 *   tf_tests vectors <fixture_dir> <c16 folder> <c24 folder> <out.jsonl>     CAL vectors: engine JSON + words + decoded
 *   tf_tests settings <config_vectors.json> <folder> <out.jsonl>
 *   tf_tests folder <folder> <key_hex|->        open a folder, print the info JSON or the error code name
 *   tf_tests ogg | codec | sharing | upload | stream_share | client | download   (tf_client_tests.inc, tf_share_tests.inc)
 *   tf_tests bitrate | consent | telemetry | sdk_latest                          (2.0.1: tf_201_tests.inc) */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* sched_getaffinity, CPU_COUNT (the pinning test) */
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tajweedformer.h"
#include "tf_internal.h"
#include "zf/stream.h"
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

static const char *const kOut[12] = {"attr_face", "attr_ghunna", "attr_qalqalah", "attr_tafkheem", "boundary", "conf",
                                     "gem", "id_log_probs", "inter1_log_probs", "inter4_log_probs", "log_probs",
                                     "timing_log_probs"};
#define FNV0 1469598103934665603ull
static uint64_t fnv1a(uint64_t h, const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

static int fails;
#define CHECK(c, ...)                                  \
  do {                                                 \
    if (!(c)) {                                        \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                    \
      fprintf(stderr, "\n");                           \
      fails++;                                         \
    }                                                  \
  } while (0)

/* ------------------------------------------------------------------ crypto */
static void hex(const char *h, uint8_t *o, size_t n) {
  if (tf_hex_decode(h, o, n)) {
    fprintf(stderr, "bad hex in test\n");
    exit(2);
  }
}
static int crypto_tests(void) {
  uint8_t d[64];
  char x[129];
  tf_sha256("abc", 3, d);
  tf_hex_encode(d, 32, x);
  CHECK(!strcmp(x, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc) %s", x);
  tf_sha512("abc", 3, d);
  tf_hex_encode(d, 64, x);
  CHECK(!strcmp(x, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"),
        "sha512(abc) %s", x);
  {
    /* 1,000,000 x 'a' across block boundaries */
    char *a = (char *)malloc(1000000);
    memset(a, 'a', 1000000);
    TfSha256 c;
    tf_sha256_init(&c);
    for (int i = 0; i < 1000000; i += 777) tf_sha256_update(&c, a + i, i + 777 > 1000000 ? 1000000 - i : 777);
    tf_sha256_final(&c, d);
    tf_hex_encode(d, 32, x);
    CHECK(!strcmp(x, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256(1M a) %s", x);
    free(a);
  }
  /* RFC 8032 section 7.1, tests 1, 2 and 3 */
  static const struct { const char *pk, *msg, *sig; } v[] = {
      {"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
       "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
      {"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
       "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
      {"fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
       "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
  };
  for (size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
    uint8_t pk[32], sig[64], msg[8];
    hex(v[i].pk, pk, 32);
    hex(v[i].sig, sig, 64);
    const size_t n = strlen(v[i].msg) / 2;
    hex(v[i].msg, msg, n);
    CHECK(tf_ed25519_verify(sig, msg, n, pk) == 1, "RFC 8032 vector %d does not verify", (int)i + 1);
    sig[5] ^= 1;
    CHECK(tf_ed25519_verify(sig, msg, n, pk) == 0, "RFC 8032 vector %d verifies with a flipped bit", (int)i + 1);
    sig[5] ^= 1;
    if (n) {
      msg[0] ^= 0x80;
      CHECK(tf_ed25519_verify(sig, msg, n, pk) == 0, "RFC 8032 vector %d verifies a changed message", (int)i + 1);
    }
  }
  /* base64 */
  uint8_t b[8];
  CHECK(tf_base64_decode("aGVsbG8=", b, sizeof b) == 5 && !memcmp(b, "hello", 5), "base64");
  CHECK(tf_base64_decode("aGVsbG8", b, sizeof b) == 5, "base64 unpadded");
  /* JSON round trip */
  const char *j = "{\"a\":[1,2.5,-3e-2,true,null,\"x\\u00e9\\n\"],\"b\":{\"c\":\"d\"}}";
  TfJson *p = tf_json_parse(j, strlen(j), NULL);
  CHECK(p != NULL, "json parse");
  if (p) {
    TfBuf o;
    tf_buf_init(&o);
    tf_json_write(&o, p);
    CHECK(o.p && !strcmp(o.p, "{\"a\":[1,2.5,-0.03,true,null,\"x\xc3\xa9\\n\"],\"b\":{\"c\":\"d\"}}"), "json write %s", o.p);
    tf_buf_free(&o);
    tf_json_free(p);
  }
  CHECK(tf_json_parse("{\"a\":}", 6, NULL) == NULL, "bad json accepted");
  printf("crypto: %s\n", fails ? "FAIL" : "ok (sha256, sha512, RFC 8032 x3, base64, json)");
  return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ conformance */
typedef struct {
  uint64_t *fnv; /* per chunk x 12 */
  int chunks, cap;
  uint64_t logp;
  unsigned mask;
  int bad_mask;
} ConfRun;

static void on_chunk(void *user, const TfChunk *c) {
  ConfRun *r = (ConfRun *)user;
  if (r->chunks == r->cap) {
    r->cap = r->cap ? 2 * r->cap : 64;
    r->fnv = (uint64_t *)realloc(r->fnv, (size_t)r->cap * 12 * sizeof(uint64_t));
  }
  for (int k = 0; k < 12; ++k) {
    const int h = tf_head_from_name(kOut[k]);
    int frames = 0;
    const float *p = tf_heads_get(c->heads, h, &frames);
    const int want = (r->mask >> h) & 1;
    if (!!p != want) r->bad_mask++;
    const size_t n = (size_t)frames * tf_head_info(h)->width;
    r->fnv[(size_t)r->chunks * 12 + k] = p ? fnv1a(FNV0, p, n * sizeof(float)) : 0;
    if (h == TF_HEAD_LOG_PROBS && p) r->logp = fnv1a(r->logp, p, n * sizeof(float));
  }
  r->chunks++;
}

static TfModel *open_folder(const char *dir, const char *key, int load_engine, const char *ctx) {
  TfModelOptions o = tf_model_default_options();
  o.trusted_key_hex = strcmp(key, "-") ? key : NULL;
  o.require_signature = strcmp(key, "-") != 0;
  o.load_engine = load_engine;
  o.context = ctx;
  TfModel *m = tf_model_open(dir, &o);
  if (!m) fprintf(stderr, "open %s: %s (%s)\n", dir, tf_last_error(), tf_error_name(tf_last_error_code()));
  return m;
}

static int conformance(const char *dir, const char *key, const char *list, const char *outp, unsigned mask, const char *ctx) {
  TfModel *m = open_folder(dir, key, 1, ctx);
  if (!m) return 2;
  ConfRun r;
  memset(&r, 0, sizeof r);
  r.mask = mask ? mask : TF_HEADS_DEFAULT;
  TfStreamOptions so = tf_stream_default_options();
  so.heads = mask;
  so.on_chunk = on_chunk;
  so.user = &r;
  TfStream *s = tf_stream_create(m, &so);
  if (!s) {
    fprintf(stderr, "stream: %s\n", tf_last_error());
    return 2;
  }
  CHECK(tf_stream_create(m, &so) == NULL && tf_last_error_code() == TF_ERR_BUSY, "a second live stream was allowed");
  FILE *lf = fopen(list, "r"), *out = fopen(outp, "w");
  if (!lf || !out) return 2;
  char line[4096];
  int clips = 0, greedy_ok = 0, top2_ok = 0, topk_ok = 0, topk_checks = 0;
  double audio_s = 0, wall = 0;
  while (fgets(line, sizeof line, lf)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
    if (!len) continue;
    char *pcm = NULL;
    size_t bytes = 0;
    if (tf_read_file(line, &pcm, &bytes)) {
      fprintf(stderr, "cannot read %s\n", line);
      return 2;
    }
    const int n = (int)(bytes / 4);
    tf_stream_reset(s);
    r.chunks = 0;
    r.logp = FNV0;
    const double t0 = tf__now_s();
    for (int at = 0; at < n; at += 320)
      if (tf_stream_push_audio(s, (const float *)pcm + at, n - at < 320 ? n - at : 320) < 0) {
        fprintf(stderr, "push: %s\n", tf_last_error());
        return 3;
      }
    if (tf_stream_finish(s) < 0) {
      fprintf(stderr, "finish: %s\n", tf_last_error());
      return 3;
    }
    wall += tf__now_s() - t0;
    audio_s += n / 16000.0;
    free(pcm);
    /* tokens = the decoded layer's token ids; they must equal the engine's greedy */
    TfDecoded *d = tf_stream_decode(s);
    const TfToken *tk;
    const int nt = tf_decoded_tokens(d, &tk);
    int g[4096];
    const int ng = tf_stream_greedy(s, g, 4096);
    int same = ng == nt;
    for (int i = 0; same && i < nt; ++i) same = g[i] == tk[i].id;
    greedy_ok += same;
    CHECK(same, "%s: decoded tokens (%d) differ from the engine's greedy (%d)", line, nt, ng);
    /* the take's heads equal the chunks joined */
    ZfTajweedHeads h;
    tf_stream_heads(s, &h);
    /* the decoded layer's top-2 comes from the engine (zf_tajweed_stream_top2): it must equal a plain C scan
     * of log_probs (lower id on ties), and its margin the double difference of the two float log-probs */
    {
      const TfFrame *fr;
      const int nf = tf_decoded_frames(d, &fr);
      int bad = nf != h.n40;
      for (int t = 0; !bad && t < nf; ++t) {
        const float *row = h.log_probs + (size_t)t * ZF_TAJWEED_VOCAB;
        int a1 = 0;
        for (int i = 1; i < ZF_TAJWEED_VOCAB; ++i)
          if (row[i] > row[a1]) a1 = i;
        int b1 = a1 == 0 ? 1 : 0;
        for (int i = 0; i < ZF_TAJWEED_VOCAB; ++i)
          if (i != a1 && row[i] > row[b1]) b1 = i;
        bad = fr[t].top1 != a1 || fr[t].runner_up != b1 || fr[t].top1_log_prob != row[a1] || fr[t].runner_up_log_prob != row[b1] ||
              fr[t].margin != (double)row[a1] - (double)row[b1];
      }
      top2_ok += !bad;
      CHECK(!bad, "%s: the engine's top-2 differs from the C scan of log_probs", line);
    }
    /* top-K from the stream (zf_tajweed_stream_topk) == a C stable argsort of the kept heads, for every
     * ranked head at k 2 and k = cols, on the first 5 clips of an all-heads run */
    if (r.mask == TF_HEADS_ALL && clips < 5) {
      for (int hd = 0; hd < TF_HEAD_COUNT; ++hd) {
        const TfHeadInfo *hi = tf_head_info(hd);
        if (!strcmp(hi->kind, "logit")) continue;
        int frames = 0;
        const float *v = tf_heads_get(&h, hd, &frames);
        const int cols = hi->width;
        for (int kk = 0; kk < 2; ++kk) {
          const int k = kk == 0 ? 2 : cols;
          TfTopK *tk = tf_stream_topk(s, hd, k);
          int bad = !tk || !v || tk->frames != frames || tk->k != k;
          int order[40];
          for (int f = 0; !bad && f < frames; ++f) {
            const float *row = v + (size_t)f * cols;
            for (int i = 0; i < cols; ++i) order[i] = i;
            for (int i = 1; i < cols; ++i) { /* insertion sort: stable, highest first */
              const int x = order[i];
              int j = i - 1;
              while (j >= 0 && row[order[j]] < row[x]) {
                order[j + 1] = order[j];
                --j;
              }
              order[j + 1] = x;
            }
            for (int j = 0; !bad && j < k; ++j) {
              const size_t i = (size_t)f * k + j;
              const double m = j + 1 < cols ? (double)row[order[j]] - (double)row[order[j + 1]] : 0.0;
              bad = tk->ids[i] != order[j] || tk->values[i] != row[order[j]] || tk->margins[i] != m;
            }
          }
          topk_checks++;
          topk_ok += !bad;
          CHECK(!bad, "%s: stream top-K of %s at k %d differs from a C argsort", line, hi->name, k);
          tf_topk_free(tk);
        }
      }
    }
    const char *base = strrchr(line, '/');
    const char *b2 = strrchr(line, '\\');
    if (b2 > base) base = b2;
    base = base ? base + 1 : line;
    char clip[256];
    snprintf(clip, sizeof clip, "%s", base);
    char *dot = strrchr(clip, '.');
    if (dot) *dot = 0;
    fprintf(out, "{\"clip\": \"%s\", \"chunks\": %d, \"tokens\": [", clip, r.chunks);
    for (int i = 0; i < nt; ++i) fprintf(out, i ? ", %d" : "%d", tk[i].id);
    fprintf(out, "], \"logp_fnv\": \"%016llx\", \"out_fnv\": {", (unsigned long long)r.logp);
    for (int k = 0; k < 12; ++k) {
      fprintf(out, "%s\"%s\": [", k ? ", " : "", kOut[k]);
      for (int c = 0; c < r.chunks; ++c) fprintf(out, c ? ", \"%016llx\"" : "\"%016llx\"", (unsigned long long)r.fnv[(size_t)c * 12 + k]);
      fprintf(out, "]");
    }
    fprintf(out, "}}\n");
    tf_decoded_free(d);
    clips++;
  }
  fclose(lf);
  fclose(out);
  CHECK(r.bad_mask == 0, "head selection: %d chunk heads present/absent against the mask", r.bad_mask);
  printf("conformance run: %d clips, engine top-2 == C scan on %d\n", clips, top2_ok);
  if (topk_checks)
    printf("conformance run: stream top-K == C argsort on %d/%d (each ranked head at k 2 and k = cols, first 5 clips)\n", topk_ok,
           topk_checks);
  printf("conformance run: %d clips, greedy == decoded on %d, head mask 0x%03x %s, RTF %.3f (%.1f s audio)\n", clips, greedy_ok,
         r.mask, r.bad_mask ? "VIOLATED" : "respected", audio_s > 0 ? wall / audio_s : 0, audio_s);
  tf_stream_destroy(s);
  tf_model_close(m);
  free(r.fnv);
  return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ CAL vectors + layers */
static const char *const kOrder[12] = {"log_probs", "inter1_log_probs", "inter4_log_probs", "id_log_probs", "timing_log_probs",
                                       "gem", "attr_tafkheem", "attr_ghunna", "attr_qalqalah", "attr_face", "boundary", "conf"};

static int vectors(const char *fx, const char *dir16, const char *dir24, const char *outp) {
  char *path = tf_path_join(fx, "vectors.json"), *text = NULL;
  size_t n = 0;
  if (tf_read_file(path, &text, &n)) return 2;
  free(path);
  TfJson *v = tf_json_parse(text, n, NULL);
  free(text);
  if (!v) return 2;
  TfModel *m16 = open_folder(dir16, "-", 0, "c16_l64"), *m24 = open_folder(dir24, "-", 0, "c24_l72");
  if (!m16 || !m24) return 2;
  tf_settings_set_backend(m16, "fp32");
  tf_settings_set_backend(m24, "fp32");
  const TfJson *presets = tf_json_get(v, "presets"), *vec = tf_json_get(v, "vectors");
  FILE *out = fopen(outp, "w");
  int done = 0;
  for (int i = 0; i < vec->n; ++i) {
    const TfJson *x = &vec->kids[i];
    const char *ctx = tf_json_str(tf_json_get(x, "context"));
    TfModel *m = !strcmp(ctx, "c16_l64") ? m16 : m24;
    const int n40 = (int)tf_json_get(x, "n40")->num, n20 = (int)tf_json_get(x, "n20")->num;
    char *hp = tf_path_join(fx, tf_json_str(tf_json_get(x, "heads"))), *raw = NULL;
    size_t bytes;
    if (tf_read_file(hp, &raw, &bytes)) return 2;
    free(hp);
    ZfTajweedHeads h;
    memset(&h, 0, sizeof h);
    h.n40 = n40;
    h.n20 = n20;
    h.vocab = 38;
    const float *at = (const float *)raw;
    for (int k = 0; k < 12; ++k) {
      const int hd = tf_head_from_name(kOrder[k]);
      const TfHeadInfo *hi = tf_head_info(hd);
      const float **slot = NULL;
      switch (hd) {
        case TF_HEAD_LOG_PROBS: slot = &h.log_probs; break;
        case TF_HEAD_INTER1: slot = &h.inter1_log_probs; break;
        case TF_HEAD_INTER4: slot = &h.inter4_log_probs; break;
        case TF_HEAD_ID: slot = &h.id_log_probs; break;
        case TF_HEAD_TIMING: slot = &h.timing_log_probs; break;
        case TF_HEAD_GEM: slot = &h.gem; break;
        case TF_HEAD_TAFKHEEM: slot = &h.attr_tafkheem; break;
        case TF_HEAD_GHUNNA: slot = &h.attr_ghunna; break;
        case TF_HEAD_QALQALAH: slot = &h.attr_qalqalah; break;
        case TF_HEAD_FACE: slot = &h.attr_face; break;
        case TF_HEAD_BOUNDARY: slot = &h.boundary; break;
        case TF_HEAD_CONF: slot = &h.conf; break;
      }
      *slot = at;
      at += (size_t)(hi->rate_ms == 40 ? n40 : n20) * hi->width;
    }
    CHECK((size_t)((const char *)at - raw) == bytes, "%s: heads file size", tf_json_str(tf_json_get(x, "name")));
    TfDecoded *d = tf_decode(m, &h);
    fprintf(out, "{\"name\":\"%s\",\"context\":\"%s\",\"decoded\":%s,", tf_json_str(tf_json_get(x, "name")), ctx,
            tf_decoded_json(d, 1));
    tf_decoded_free(d);
    /* top-K through tf_topk (layers_ref.py "topk": k 3 for the token heads, the full ranking of the class heads,
     * and log_probs_full8 = all 38 on the first 8 frames) */
    fprintf(out, "\"topk\":{");
    for (int hd = 0, first = 1; hd < TF_HEAD_COUNT; ++hd) {
      const TfHeadInfo *hi = tf_head_info(hd);
      if (!strcmp(hi->kind, "logit")) continue;
      TfTopK *tk = tf_topk(&h, hd, hi->n_classes ? hi->width : 3);
      CHECK(tk != NULL, "tf_topk %s: %s", hi->name, tf_last_error());
      if (tk) fprintf(out, "%s\"%s\":%s", first ? "" : ",", hi->name, tf_topk_json(tk));
      first = 0;
      tf_topk_free(tk);
    }
    {
      ZfTajweedHeads h8 = h;
      h8.n40 = n40 < 8 ? n40 : 8;
      h8.n20 = 2 * h8.n40;
      TfTopK *tk = tf_topk(&h8, TF_HEAD_LOG_PROBS, 38);
      CHECK(tk != NULL, "tf_topk full: %s", tf_last_error());
      if (tk) fprintf(out, ",\"log_probs_full8\":%s", tf_topk_json(tk));
      tf_topk_free(tk);
    }
    fprintf(out, "},\"results\":{");
    const TfJson *refs = tf_json_get(x, "refs");
    for (int p = 0; p < refs->n; ++p) {
      const char *preset = refs->kids[p].key;
      TfBuf rb;
      tf_buf_init(&rb);
      tf_json_write(&rb, tf_json_get(presets, preset));
      tf_settings_set_remote_json(m, rb.p, rb.n);
      tf_buf_free(&rb);
      TfResult *r = tf_score(m, &h, (int)tf_json_get(x, "surah")->num, (int)tf_json_get(x, "ayah")->num, NULL);
      if (!r) {
        fprintf(stderr, "score: %s\n", tf_last_error());
        return 3;
      }
      fprintf(out, "%s\"%s\":%s", p ? "," : "", preset, tf_result_json(r));
      tf_result_free(r);
      done++;
    }
    fprintf(out, "}}\n");
    free(raw);
  }
  fclose(out);
  tf_model_close(m16);
  tf_model_close(m24);
  tf_json_free(v);
  printf("vectors run: %d scores\n", done);
  return 0;
}

/* ------------------------------------------------------------------ settings vectors */
static int settings(const char *vp, const char *dir, const char *outp) {
  char *text = NULL;
  size_t n;
  if (tf_read_file(vp, &text, &n)) return 2;
  TfJson *v = tf_json_parse(text, n, NULL);
  free(text);
  if (!v) return 2;
  FILE *out = fopen(outp, "w");
  const TfJson *cases = tf_json_get(v, "cases"), *inv = tf_json_get(v, "app_invalid");
  for (int i = 0; i < cases->n; ++i) {
    const TfJson *c = &cases->kids[i];
    TfModelOptions o = tf_model_default_options();
    o.require_signature = 0;
    o.load_engine = 0;
    o.debug_build = tf_json_get(c, "debug") && tf_json_get(c, "debug")->boolean;
    TfModel *m = tf_model_open(dir, &o);
    if (!m) {
      fprintf(stderr, "open: %s\n", tf_last_error());
      return 2;
    }
    tf_settings_set_backend(m, tf_json_str(tf_json_get(c, "backend")));
    const TfJson *remote = tf_json_get(c, "remote");
    if (remote && remote->type != TJ_NULL) {
      TfBuf rb;
      tf_buf_init(&rb);
      if (remote->type == TJ_STR) tf_buf_puts(&rb, remote->str); /* a block as text */
      else tf_json_write(&rb, remote);
      tf_settings_set_remote_json(m, rb.p, rb.n);
      tf_buf_free(&rb);
    }
    const TfJson *app = tf_json_get(c, "app");
    int rc = 0;
    if (app) {
      TfBuf ab;
      tf_buf_init(&ab);
      tf_json_write(&ab, app);
      rc = tf_settings_set_app_json(m, ab.p);
      tf_buf_free(&ab);
    }
    fprintf(out, "{\"name\":");
    TfBuf nb;
    tf_buf_init(&nb);
    tf_buf_json_str(&nb, tf_json_str(tf_json_get(c, "name")));
    fprintf(out, "%s,\"rc\":%d,\"settings\":%s}\n", nb.p, rc, tf_settings_json(m));
    tf_buf_free(&nb);
    tf_model_close(m);
  }
  int refused = 0;
  for (int i = 0; inv && i < inv->n; ++i) {
    TfModelOptions o = tf_model_default_options();
    o.require_signature = 0;
    o.load_engine = 0;
    TfModel *m = tf_model_open(dir, &o);
    TfBuf ab;
    tf_buf_init(&ab);
    tf_json_write(&ab, tf_json_get(&inv->kids[i], "app"));
    const int rc = tf_settings_set_app_json(m, ab.p);
    tf_buf_free(&ab);
    CHECK(rc == TF_ERR_SETTINGS, "invalid app case %s was accepted", tf_json_str(tf_json_get(&inv->kids[i], "name")));
    refused += rc == TF_ERR_SETTINGS;
    tf_model_close(m);
  }
  fclose(out);
  printf("settings run: %d cases; invalid app options refused %d/%d\n", cases->n, refused, inv ? inv->n : 0);
  tf_json_free(v);
  return fails ? 1 : 0;
}

#include "tf_client_tests.inc"
#include "tf_share_tests.inc"
#include "tf_201_tests.inc"

static int folder(const char *dir, const char *key) {
  TfModel *m = open_folder(dir, key, 0, NULL);
  if (!m) {
    printf("%s\n", tf_error_name(tf_last_error_code()));
    return 1;
  }
  printf("%s\n", tf_model_info_json(m));
  tf_model_close(m);
  return 0;
}

/* ------------------------------------------------------------------ pinning (engine de88490) */
/* Streams made and destroyed one after another on ONE thread with pinning ON (no_pin 0 explicitly) keep their
 * full engine team (zf_stream_threads_active == min(threads, the process's cores) every time) and leave the
 * calling thread's affinity mask as they found it. The bug this guards: a pinned caller's mask was never
 * restored, so every later stream got a one-core team. Checked at the engine's stream level (the team) and
 * through tf_stream (the mask, and the same tokens every take). */
#if defined(_WIN32)
typedef DWORD_PTR PinMask;
static PinMask pin_thread_mask(void) {
  DWORD_PTR proc = 0, sys = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
  const DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), proc);
  if (old) SetThreadAffinityMask(GetCurrentThread(), old);
  return old;
}
static int pin_process_cores(void) {
  DWORD_PTR proc = 0, sys = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
  int n = 0;
  for (; proc; proc >>= 1) n += (int)(proc & 1);
  return n;
}
#elif defined(__linux__)
typedef cpu_set_t PinMask;
static PinMask pin_thread_mask(void) {
  PinMask m;
  CPU_ZERO(&m);
  sched_getaffinity(0, sizeof m, &m);
  return m;
}
static int pin_process_cores(void) {
  PinMask m;
  CPU_ZERO(&m);
  sched_getaffinity(getpid(), sizeof m, &m);
  return CPU_COUNT(&m);
}
#else
typedef int PinMask;
static PinMask pin_thread_mask(void) { return 0; }
static int pin_process_cores(void) { return 1; }
#endif

static int pin_tests(const char *dir, const char *key, const char *list, int threads, int rounds) {
  /* the first clip of the list */
  FILE *lf = fopen(list, "r");
  char line[4096] = "";
  if (!lf || !fgets(line, sizeof line, lf)) return 2;
  fclose(lf);
  size_t len = strlen(line);
  while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
  char *pcm = NULL;
  size_t bytes = 0;
  if (tf_read_file(line, &pcm, &bytes)) return 2;
  const int n = (int)(bytes / 4);
  const float *x = (const float *)pcm;
  const int cores = pin_process_cores();
  const int expect = threads < cores ? threads : cores;
  const PinMask original = pin_thread_mask();
  int bad = 0;

  /* 1. the engine's stream (zf_stream), pinned, `rounds` in a row, then two alive at once */
  char *model = tf_path_join(dir, "c16_l64/model_engine2.onnx");
  ZfToken toks[512];
  ZfStream *both[2] = {NULL, NULL};
  for (int r = 0; r < rounds + 2; ++r) {
    ZfStreamOptions o = zf_stream_default_options();
    o.threads = threads;
    o.no_pin = 0; /* pinned, whatever the platform's default */
    const char *err = NULL;
    ZfStream *s = zf_stream_create(model, &o, &err);
    if (!s) {
      fprintf(stderr, "zf_stream_create: %s\n", err ? err : "?");
      return 3;
    }
    for (int at = 0; at < n; at += 320) zf_stream_push_audio(s, x + at, n - at < 320 ? n - at : 320, toks, 512);
    zf_stream_finish_audio(s, toks, 512);
    const int team = zf_stream_threads_active(s);
    if (r < rounds) {
      zf_stream_destroy(s);
      const PinMask after = pin_thread_mask();
      const int restored = memcmp(&after, &original, sizeof after) == 0;
      const int ok = team == expect && restored;
      bad += !ok;
      printf("engine stream %d (pinned, %d threads): team %d of %d expected, caller's mask %s: %s\n", r, threads, team, expect,
             restored ? "restored" : "NOT restored", ok ? "ok" : "FAIL");
    } else {
      both[r - rounds] = s;
      bad += team != expect;
      printf("engine stream %d alive with %d other: team %d of %d expected: %s\n", r, r - rounds, team, expect,
             team == expect ? "ok" : "FAIL");
    }
  }
  zf_stream_destroy(both[1]);
  zf_stream_destroy(both[0]);
  {
    const PinMask after = pin_thread_mask();
    const int restored = memcmp(&after, &original, sizeof after) == 0;
    bad += !restored;
    printf("two engine streams at once, destroyed: caller's mask %s\n", restored ? "restored" : "NOT restored");
  }
  free(model);

  /* 2. through the SDK: a model opened pinned (no_pin 0) and `threads` threads; streams made, used and
   * destroyed in a row on this thread give the same tokens and leave the mask as it was */
  TfModelOptions mo = tf_model_default_options();
  mo.trusted_key_hex = strcmp(key, "-") ? key : NULL;
  mo.require_signature = strcmp(key, "-") != 0;
  mo.threads = threads;
  mo.no_pin = 0;
  TfModel *m = tf_model_open(dir, &mo);
  if (!m) {
    fprintf(stderr, "open: %s\n", tf_last_error());
    return 3;
  }
  int first[1024], nfirst = -1;
  for (int r = 0; r < rounds; ++r) {
    TfStream *s = tf_stream_create(m, NULL);
    if (!s) return 3;
    const double t0 = tf__now_s();
    for (int at = 0; at < n; at += 320) tf_stream_push_audio(s, x + at, n - at < 320 ? n - at : 320);
    tf_stream_finish(s);
    const double dt = tf__now_s() - t0;
    int ids[1024];
    const int ni = tf_stream_greedy(s, ids, 1024);
    tf_stream_destroy(s);
    const PinMask after = pin_thread_mask();
    const int restored = memcmp(&after, &original, sizeof after) == 0;
    int same = 1;
    if (nfirst < 0) {
      nfirst = ni;
      memcpy(first, ids, sizeof(int) * (size_t)(ni < 1024 ? ni : 1024));
    } else {
      same = ni == nfirst && !memcmp(first, ids, sizeof(int) * (size_t)(ni < 1024 ? ni : 1024));
    }
    const int ok = restored && same;
    bad += !ok;
    printf("tf stream %d (pinned, %d threads): %.0f ms, %d tokens%s, caller's mask %s: %s\n", r, threads, dt * 1e3, ni,
           same ? "" : " (DIFFERENT)", restored ? "restored" : "NOT restored", ok ? "ok" : "FAIL");
  }
  tf_model_close(m);
  free(pcm);
  printf("pinning: %s\n", bad ? "FAIL" : "all ok");
  return bad ? 1 : 0;
}

int main(int argc, char **argv) {
  tf__client_no_timer = 1; /* the replayed exchanges and stubs run on the test's thread; the telemetry test starts the timer itself */
  if (argc >= 2 && !strcmp(argv[1], "crypto")) return crypto_tests();
  if (argc >= 5 && !strcmp(argv[1], "pin"))
    return pin_tests(argv[2], argv[3], argv[4], argc > 5 ? atoi(argv[5]) : 4, argc > 6 ? atoi(argv[6]) : 6);
  if (argc >= 6 && !strcmp(argv[1], "conformance"))
    return conformance(argv[2], argv[3], argv[4], argv[5], argc > 6 ? (unsigned)strtoul(argv[6], NULL, 0) : 0,
                       argc > 7 ? argv[7] : NULL);
  if (argc >= 6 && !strcmp(argv[1], "vectors")) return vectors(argv[2], argv[3], argv[4], argv[5]);
  if (argc >= 5 && !strcmp(argv[1], "settings")) return settings(argv[2], argv[3], argv[4]);
  if (argc >= 4 && !strcmp(argv[1], "folder")) return folder(argv[2], argv[3]);
  if (argc >= 5 && !strcmp(argv[1], "download")) return download_tests(argv[2], argv[3], argv[4]);
  if (argc >= 5 && !strcmp(argv[1], "sharing")) return sharing_tests(argv[2], argv[3], argv[4]);
  if (argc >= 4 && !strcmp(argv[1], "ogg")) return ogg_tests(argv[2], argv[3]);
  if (argc >= 4 && !strcmp(argv[1], "codec")) return codec_tests(argv[2], argv[3]);
  if (argc >= 3 && !strcmp(argv[1], "upload")) return upload_tests(argv[2]);
  if (argc >= 6 && !strcmp(argv[1], "stream_share")) return stream_share_tests(argv[2], argv[3], argv[4], argv[5]);
  if (argc >= 4 && !strcmp(argv[1], "client")) return client_tests(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
  if (argc >= 4 && !strcmp(argv[1], "bitrate")) return bitrate_tests(argv[2], argv[3]);
  if (argc >= 5 && !strcmp(argv[1], "consent")) return consent_tests(argv[2], argv[3], argv[4]);
  if (argc >= 4 && !strcmp(argv[1], "sdk_latest")) return sdk_latest_tests(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
  if (argc >= 3 && !strcmp(argv[1], "telemetry")) return telemetry_tests(argv[2], argc > 4 ? argv[3] : NULL, argc > 4 ? argv[4] : NULL);
  fprintf(stderr, "usage: see the comment at the top of test/tf_tests.c\n");
  return 2;
}
