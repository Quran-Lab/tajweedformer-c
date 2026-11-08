/* The decoded layer (API_LAYERS.md section 2): per 40 ms frame top-1 and runner-up, the greedy tokens with
 * peak log-prob, runner-up, margin and conf-head confidence, and the "|"-separated segments. The numbers
 * are pinned by the reference layers_ref.py. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

#define BLANK 0
#define BAR 1

struct TfDecoded {
  const TfModel *m;
  TfFrame *frames;
  int n_frames;
  TfToken *tokens;
  int n_tokens;
  TfSegment *segs;
  int n_segs;
  TfTopK *ranked; /* log_probs ranked to depth k per frame */
  char *json;
};

/* top2: the engine's per-frame top-1 and runner-up (zf_tajweed_stream_top2 for a stream, zf_tajweed_top2
 * otherwise; ties to the lower id). The margin is recomputed in double from the two float log-probs, as
 * API_LAYERS.md and layers_ref.py define it (the engine's own margin is float). */
static TfDecoded *decode(const TfModel *m, const ZfTajweedHeads *h, const ZfTajweedTop2 *top2) {
  tf__clear_error();
  if (!h || !h->log_probs || h->n40 < 0) {
    tf__fail(TF_ERR_ARGUMENT, "the heads need log_probs");
    return NULL;
  }
  const int T = h->n40, V = ZF_TAJWEED_VOCAB;
  TfDecoded *d = (TfDecoded *)calloc(1, sizeof *d);
  if (!d) goto oom;
  d->m = m;
  d->frames = (TfFrame *)calloc((size_t)T + 1, sizeof(TfFrame));
  d->tokens = (TfToken *)calloc((size_t)T + 1, sizeof(TfToken));
  d->segs = (TfSegment *)calloc((size_t)T + 1, sizeof(TfSegment));
  if (!d->frames || !d->tokens || !d->segs) goto oom;
  d->n_frames = T;
  ZfTajweedTop2 *own = NULL;
  if (!top2 && T > 0) {
    own = (ZfTajweedTop2 *)malloc(sizeof(ZfTajweedTop2) * (size_t)T);
    if (!own) goto oom;
    if (zf_tajweed_top2(h->log_probs, T, V, own) != T) {
      free(own);
      tf_decoded_free(d);
      tf__fail(TF_ERR_ENGINE, "engine: zf_tajweed_top2 failed");
      return NULL;
    }
    top2 = own;
  }
  for (int t = 0; t < T; ++t) {
    TfFrame *f = &d->frames[t];
    f->top1 = top2[t].token;
    f->top1_log_prob = top2[t].log_prob;
    f->runner_up = top2[t].runner_up;
    f->runner_up_log_prob = top2[t].runner_up_log_prob;
    f->margin = (double)top2[t].log_prob - (double)top2[t].runner_up_log_prob;
  }
  free(own);
  for (int t = 0; t < T;) {
    const int a = d->frames[t].top1;
    if (a == BLANK) {
      ++t;
      continue;
    }
    int e = t;
    while (e + 1 < T && d->frames[e + 1].top1 == a) ++e;
    int pk = t;
    for (int k = t + 1; k <= e; ++k)
      if (d->frames[k].top1_log_prob > d->frames[pk].top1_log_prob) pk = k;
    TfToken *tk = &d->tokens[d->n_tokens++];
    tk->id = a;
    tk->frame0 = t;
    tk->frame1 = e;
    tk->start_ms = 40 * t;
    tk->end_ms = 40 * (e + 1);
    tk->peak_frame = pk;
    tk->log_prob = d->frames[pk].top1_log_prob;
    tk->runner_up = d->frames[pk].runner_up;
    tk->runner_up_log_prob = d->frames[pk].runner_up_log_prob;
    tk->margin = d->frames[pk].margin;
    if (h->conf && h->n20 >= 2 * (e + 1)) {
      double s = 0;
      for (int k = 2 * t; k < 2 * e + 2; ++k) s += 1.0 / (1.0 + exp(-(double)h->conf[k]));
      tk->confidence = s / (2 * (e - t + 1));
    } else {
      tk->confidence = NAN;
    }
    t = e + 1;
  }
  /* segments between "|" tokens */
  int first = -1;
  for (int i = 0; i <= d->n_tokens; ++i) {
    const int bar = i == d->n_tokens || d->tokens[i].id == BAR;
    if (!bar) {
      if (first < 0) first = i;
      continue;
    }
    if (first >= 0) {
      TfSegment *s = &d->segs[d->n_segs++];
      s->first_token = first;
      s->n_tokens = i - first;
      s->start_ms = d->tokens[first].start_ms;
      s->end_ms = d->tokens[i - 1].end_ms;
      s->confidence = d->tokens[first].confidence;
      s->margin = d->tokens[first].margin;
      for (int k = first + 1; k < i; ++k) {
        if (d->tokens[k].confidence != d->tokens[k].confidence || s->confidence != s->confidence) s->confidence = NAN;
        else if (d->tokens[k].confidence < s->confidence) s->confidence = d->tokens[k].confidence;
        if (d->tokens[k].margin < s->margin) s->margin = d->tokens[k].margin;
      }
    }
    first = -1;
  }
  return d;
oom:
  tf_decoded_free(d);
  tf__fail(TF_ERR_NO_MEMORY, "out of memory");
  return NULL;
}

static TfDecoded *with_ranks(TfDecoded *d, TfTopK *r) {
  if (!d || !r) {
    tf_topk_free(r);
    tf_decoded_free(d);
    return NULL;
  }
  d->ranked = r;
  return d;
}
TfDecoded *tf_decode_k(const TfModel *m, const ZfTajweedHeads *h, int k) {
  if (k < 2 || k > ZF_TAJWEED_VOCAB) {
    tf__fail(TF_ERR_ARGUMENT, "k must be 2 to %d", ZF_TAJWEED_VOCAB);
    return NULL;
  }
  TfDecoded *d = decode(m, h, NULL);
  return d ? with_ranks(d, tf_topk(h, TF_HEAD_LOG_PROBS, k)) : NULL;
}
TfDecoded *tf_decode(const TfModel *m, const ZfTajweedHeads *h) { return tf_decode_k(m, h, 2); }
int tf_decoded_ranked(const TfDecoded *d, const int **ids, const float **log_probs, const double **margins) {
  if (!d || !d->ranked) return -1;
  if (ids) *ids = d->ranked->ids;
  if (log_probs) *log_probs = d->ranked->values;
  if (margins) *margins = d->ranked->margins;
  return d->ranked->k;
}

TfModel *tf__stream_model(TfStream *s);
int tf__stream_all_heads(const TfStream *s, ZfTajweedHeads *out);
int tf__stream_top2(const TfStream *s, const ZfTajweedTop2 **out);
TfDecoded *tf_stream_decode(const TfStream *s) { return tf_stream_decode_k(s, 2); }
TfDecoded *tf_stream_decode_k(const TfStream *s, int k) {
  if (k < 2 || k > ZF_TAJWEED_VOCAB) {
    tf__fail(TF_ERR_ARGUMENT, "k must be 2 to %d", ZF_TAJWEED_VOCAB);
    return NULL;
  }
  ZfTajweedHeads h;
  if (!s || tf__stream_all_heads(s, &h) < 0) {
    tf__fail(TF_ERR_ARGUMENT, "no stream heads");
    return NULL;
  }
  const ZfTajweedTop2 *top2 = NULL;
  const int n = tf__stream_top2(s, &top2);
  if (n != h.n40) top2 = NULL; /* never expected; the engine's kept top-2 covers every frame of the take */
  TfDecoded *d = decode(tf__stream_model((TfStream *)s), &h, top2);
  return d ? with_ranks(d, tf_stream_topk(s, TF_HEAD_LOG_PROBS, k)) : NULL;
}

void tf_decoded_free(TfDecoded *d) {
  if (!d) return;
  free(d->frames);
  free(d->tokens);
  free(d->segs);
  tf_topk_free(d->ranked);
  free(d->json);
  free(d);
}
int tf_decoded_frames(const TfDecoded *d, const TfFrame **f) {
  if (!d) return -1;
  if (f) *f = d->frames;
  return d->n_frames;
}
int tf_decoded_tokens(const TfDecoded *d, const TfToken **t) {
  if (!d) return -1;
  if (t) *t = d->tokens;
  return d->n_tokens;
}
int tf_decoded_segments(const TfDecoded *d, const TfSegment **s) {
  if (!d) return -1;
  if (s) *s = d->segs;
  return d->n_segs;
}

static void unit(TfBuf *b, const TfDecoded *d, int id) {
  const char *u = d->m ? tf_model_token_name(d->m, id) : NULL;
  if (u) tf_buf_json_str(b, u);
  else tf_buf_puts(b, "null");
}

const char *tf_decoded_json(TfDecoded *d, int with_frames) {
  if (!d) return NULL;
  free(d->json);
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{");
  if (with_frames) {
    tf_buf_puts(&b, "\"frames\":[");
    for (int t = 0; t < d->n_frames; ++t) {
      const TfFrame *f = &d->frames[t];
      if (t) tf_buf_puts(&b, ",");
      tf_buf_printf(&b, "{\"top1\":%d,\"top1LogProb\":", f->top1);
      tf_buf_num(&b, f->top1_log_prob);
      tf_buf_printf(&b, ",\"runnerUp\":%d,\"runnerUpLogProb\":", f->runner_up);
      tf_buf_num(&b, f->runner_up_log_prob);
      tf_buf_puts(&b, ",\"margin\":");
      tf_buf_num(&b, f->margin);
      if (d->ranked && t < d->ranked->frames) {
        /* the frame's k best tokens (tf_decode_k): id, unit, log-prob, margin to the next rank */
        tf_buf_puts(&b, ",\"ranked\":[");
        for (int j = 0; j < d->ranked->k; ++j) {
          const size_t i = (size_t)t * d->ranked->k + j;
          tf_buf_printf(&b, "%s{\"id\":%d,\"unit\":", j ? "," : "", d->ranked->ids[i]);
          unit(&b, d, d->ranked->ids[i]);
          tf_buf_puts(&b, ",\"logProb\":");
          tf_buf_num(&b, d->ranked->values[i]);
          tf_buf_puts(&b, ",\"margin\":");
          tf_buf_num(&b, d->ranked->margins[i]);
          tf_buf_puts(&b, "}");
        }
        tf_buf_puts(&b, "]");
      }
      tf_buf_puts(&b, "}");
    }
    tf_buf_puts(&b, "],");
  }
  tf_buf_puts(&b, "\"tokens\":[");
  for (int i = 0; i < d->n_tokens; ++i) {
    const TfToken *t = &d->tokens[i];
    if (i) tf_buf_puts(&b, ",");
    tf_buf_printf(&b, "{\"id\":%d,\"unit\":", t->id);
    unit(&b, d, t->id);
    tf_buf_printf(&b, ",\"frame0\":%d,\"frame1\":%d,\"startMs\":%d,\"endMs\":%d,\"peakFrame\":%d,\"logProb\":", t->frame0, t->frame1,
                  t->start_ms, t->end_ms, t->peak_frame);
    tf_buf_num(&b, t->log_prob);
    tf_buf_printf(&b, ",\"runnerUp\":%d,\"runnerUpUnit\":", t->runner_up);
    unit(&b, d, t->runner_up);
    tf_buf_puts(&b, ",\"runnerUpLogProb\":");
    tf_buf_num(&b, t->runner_up_log_prob);
    tf_buf_puts(&b, ",\"margin\":");
    tf_buf_num(&b, t->margin);
    tf_buf_puts(&b, ",\"confidence\":");
    tf_buf_num(&b, t->confidence);
    tf_buf_puts(&b, "}");
  }
  tf_buf_puts(&b, "],\"words\":[");
  for (int i = 0; i < d->n_segs; ++i) {
    const TfSegment *s = &d->segs[i];
    if (i) tf_buf_puts(&b, ",");
    tf_buf_printf(&b, "{\"index\":%d,\"tokens\":[", i);
    for (int k = 0; k < s->n_tokens; ++k) tf_buf_printf(&b, k ? ",%d" : "%d", s->first_token + k);
    tf_buf_puts(&b, "],\"units\":[");
    for (int k = 0; k < s->n_tokens; ++k) {
      if (k) tf_buf_puts(&b, ",");
      unit(&b, d, d->tokens[s->first_token + k].id);
    }
    tf_buf_printf(&b, "],\"startMs\":%d,\"endMs\":%d,\"confidence\":", s->start_ms, s->end_ms);
    tf_buf_num(&b, s->confidence);
    tf_buf_puts(&b, ",\"margin\":");
    tf_buf_num(&b, s->margin);
    tf_buf_puts(&b, "}");
  }
  tf_buf_puts(&b, "],\"transcript\":");
  TfBuf tr;
  tf_buf_init(&tr);
  for (int i = 0; i < d->n_tokens; ++i) {
    const char *u = d->m ? tf_model_token_name(d->m, d->tokens[i].id) : NULL;
    if (i) tf_buf_puts(&tr, " ");
    if (u) tf_buf_puts(&tr, u);
    else tf_buf_printf(&tr, "%d", d->tokens[i].id);
  }
  char *trs = tf_buf_take(&tr);
  tf_buf_json_str(&b, trs ? trs : "");
  free(trs);
  tf_buf_puts(&b, "}");
  d->json = tf_buf_take(&b);
  return d->json;
}
