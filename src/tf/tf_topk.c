/* Top-K: per frame the k best entries of any token or class head, by tf__topk_rows (tf_rank.c, the
 * 3.x engine's ranking), on heads from anywhere or on a stream's take (qlc_stream_heads). The ranking is a
 * stable argsort of the negated row (equal values by the lower id). Margins are recomputed here as double
 * differences of the float values (API_LAYERS.md, layers_ref.py topk): the engine is asked for k + 1 ranks
 * when k < cols so the k-th margin has its partner, and the last rank of a row has margin 0. */
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

typedef struct {
  TfTopK pub;
  int *ids;
  float *values;
  double *margins;
  char *json;
} TopK;

static int rankable(int head) {
  const TfHeadInfo *hi = tf_head_info(head);
  return hi && strcmp(hi->kind, "logit") != 0;
}

/* from k2 = min(k + 1, cols) ranks per frame (ids2, vals2), keep k and compute the margins */
static TfTopK *finish(int head, int frames, int cols, int k, int k2, const int *ids2, const float *vals2) {
  TopK *t = (TopK *)calloc(1, sizeof *t);
  if (!t) goto oom;
  const size_t n = (size_t)frames * (size_t)k + 1;
  t->ids = (int *)malloc(n * sizeof(int));
  t->values = (float *)malloc(n * sizeof(float));
  t->margins = (double *)malloc(n * sizeof(double));
  if (!t->ids || !t->values || !t->margins) goto oom;
  for (int f = 0; f < frames; ++f) {
    const int *ri = ids2 + (size_t)f * k2;
    const float *rv = vals2 + (size_t)f * k2;
    for (int j = 0; j < k; ++j) {
      t->ids[(size_t)f * k + j] = ri[j];
      t->values[(size_t)f * k + j] = rv[j];
      t->margins[(size_t)f * k + j] = j + 1 < k2 ? (double)rv[j] - (double)rv[j + 1] : 0.0;
    }
  }
  t->pub.head = head;
  t->pub.frames = frames;
  t->pub.k = k;
  t->pub.cols = cols;
  t->pub.ids = t->ids;
  t->pub.values = t->values;
  t->pub.margins = t->margins;
  return &t->pub;
oom:
  tf__topk_free(t ? &t->pub : NULL);
  tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  return NULL;
}

TfTopK *tf__topk(const TfRawHeads *h, int head, int k) {
  tf__clear_error();
  if (!h || !rankable(head)) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "top-K needs a token or class head");
    return NULL;
  }
  int frames = 0;
  const float *v = tf__heads_array(h, head, &frames);
  const int cols = tf_head_info(head)->width;
  if (!v) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "the heads have no %s", tf_head_info(head)->name);
    return NULL;
  }
  if (k < 1 || k > cols) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "k must be 1 to %d for %s", cols, tf_head_info(head)->name);
    return NULL;
  }
  const int k2 = k < cols ? k + 1 : k;
  int *ids2 = (int *)malloc(((size_t)frames * k2 + 1) * sizeof(int));
  float *vals2 = (float *)malloc(((size_t)frames * k2 + 1) * sizeof(float));
  TfTopK *t = NULL;
  if (!ids2 || !vals2) tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  else if (frames > 0 && tf__topk_rows(v, frames, cols, k2, ids2, vals2, NULL) != frames) tf__fail(TF_ERR_ENGINE_FAILED, "the top-K ranking failed");
  else t = finish(head, frames, cols, k, k2, ids2, vals2);
  free(ids2);
  free(vals2);
  return t;
}

TfTopK *tf__session_topk(const TfSession *s, int head, int k) {
  tf__clear_error();
  if (!s || !rankable(head)) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "top-K needs a session and a token or class head");
    return NULL;
  }
  const int cols = tf_head_info(head)->width;
  if (k < 1 || k > cols) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "k must be 1 to %d for %s", cols, tf_head_info(head)->name);
    return NULL;
  }
  const int k2 = k < cols ? k + 1 : k;
  /* the take so far as the Core keeps it (a head the stream does not compute is absent) */
  TfRawHeads all;
  int frames = 0;
  /* 4.0.2: read under the streams' lock and copied: another Model's take, or this Model's next, may destroy the
   * stream once the lock is let go */
  tf__model_stream_lock();
  const float *in = tf__session_take_heads(s, &all) ? NULL : tf__heads_array(&all, head, &frames);
  float *v = in ? (float *)malloc(((size_t)frames * cols + 1) * sizeof(float)) : NULL;
  if (v) memcpy(v, in, (size_t)frames * cols * sizeof(float));
  tf__model_stream_unlock();
  if (!v) {
    if (in) tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
    else tf__fail(TF_ERR_INVALID_ARGUMENT, "%s is not computed by this stream (see TfSessionOptions.heads)", tf_head_info(head)->name);
    return NULL;
  }
  int *ids2 = (int *)malloc(((size_t)frames * k2 + 1) * sizeof(int));
  float *vals2 = (float *)malloc(((size_t)frames * k2 + 1) * sizeof(float));
  TfTopK *t = NULL;
  if (!ids2 || !vals2) tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  else if (frames > 0 && tf__topk_rows(v, frames, cols, k2, ids2, vals2, NULL) != frames) tf__fail(TF_ERR_ENGINE_FAILED, "the top-K ranking failed");
  else t = finish(head, frames, cols, k, k2, ids2, vals2);
  free(ids2);
  free(vals2);
  free(v);
  return t;
}

void tf__topk_free(TfTopK *p) {
  if (!p) return;
  TopK *t = (TopK *)p;
  free(t->ids);
  free(t->values);
  free(t->margins);
  free(t->json);
  free(t);
}

const char *tf__topk_json(TfTopK *p) {
  if (!p) return NULL;
  TopK *t = (TopK *)p;
  free(t->json);
  const TfHeadInfo *hi = tf_head_info(p->head);
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"head\":");
  tf_buf_json_str(&b, hi->name);
  tf_buf_printf(&b, ",\"k\":%d,\"cols\":%d", p->k, p->cols);
  if (hi->n_classes) {
    tf_buf_puts(&b, ",\"classes\":[");
    for (int c = 0; c < hi->n_classes; ++c) {
      if (c) tf_buf_puts(&b, ",");
      tf_buf_json_str(&b, hi->classes[c]);
    }
    tf_buf_puts(&b, "]");
  }
  for (int part = 0; part < 3; ++part) {
    tf_buf_puts(&b, part == 0 ? ",\"ids\":[" : part == 1 ? "],\"values\":[" : "],\"margins\":[");
    for (int f = 0; f < p->frames; ++f) {
      tf_buf_puts(&b, f ? ",[" : "[");
      for (int j = 0; j < p->k; ++j) {
        const size_t i = (size_t)f * p->k + j;
        if (j) tf_buf_puts(&b, ",");
        if (part == 0) tf_buf_printf(&b, "%d", p->ids[i]);
        else tf_buf_num(&b, part == 1 ? (double)p->values[i] : p->margins[i]);
      }
      tf_buf_puts(&b, "]");
    }
  }
  tf_buf_puts(&b, "]}");
  t->json = tf_buf_take(&b);
  return t->json;
}
