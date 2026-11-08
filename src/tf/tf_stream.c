/* Heads metadata and streams with per-head selection. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

static const char *const kGem[] = {"single", "held"};
static const char *const kTafkheem[] = {"thin", "heavy", "low_heavy"};
static const char *const kGhunna[] = {"none", "plain", "geminate", "ikhfa", "idgham"};
static const char *const kQalqalah[] = {"none", "sughra", "kubra"};
static const char *const kFace[] = {"none", "imala", "tasheel", "sakt"};

static const TfHeadInfo kHeads[TF_HEAD_COUNT] = {
    {"log_probs", 38, 40, "log_softmax", 0, NULL},
    {"inter1_log_probs", 38, 40, "log_softmax", 0, NULL},
    {"inter4_log_probs", 38, 40, "log_softmax", 0, NULL},
    {"id_log_probs", 38, 20, "log_softmax", 0, NULL},
    {"timing_log_probs", 38, 20, "log_softmax", 0, NULL},
    {"gem", 2, 20, "softmax", 2, kGem},
    {"attr_tafkheem", 3, 20, "softmax", 3, kTafkheem},
    {"attr_ghunna", 5, 20, "softmax", 5, kGhunna},
    {"attr_qalqalah", 3, 20, "softmax", 3, kQalqalah},
    {"attr_face", 4, 20, "softmax", 4, kFace},
    {"boundary", 1, 20, "logit", 0, NULL},
    {"conf", 1, 20, "logit", 0, NULL},
};

const TfHeadInfo *tf_head_info(int h) { return h >= 0 && h < TF_HEAD_COUNT ? &kHeads[h] : NULL; }
int tf_head_from_name(const char *name) {
  if (!name) return -1;
  for (int h = 0; h < TF_HEAD_COUNT; ++h)
    if (!strcmp(name, kHeads[h].name)) return h;
  return -1;
}

static const float **slot(ZfTajweedHeads *h, int head) {
  switch (head) {
    case TF_HEAD_LOG_PROBS: return &h->log_probs;
    case TF_HEAD_INTER1: return &h->inter1_log_probs;
    case TF_HEAD_INTER4: return &h->inter4_log_probs;
    case TF_HEAD_ID: return &h->id_log_probs;
    case TF_HEAD_TIMING: return &h->timing_log_probs;
    case TF_HEAD_GEM: return &h->gem;
    case TF_HEAD_TAFKHEEM: return &h->attr_tafkheem;
    case TF_HEAD_GHUNNA: return &h->attr_ghunna;
    case TF_HEAD_QALQALAH: return &h->attr_qalqalah;
    case TF_HEAD_FACE: return &h->attr_face;
    case TF_HEAD_BOUNDARY: return &h->boundary;
    case TF_HEAD_CONF: return &h->conf;
    default: return NULL;
  }
}
const float *tf_heads_get(const ZfTajweedHeads *h, int head, int *frames) {
  if (!h || head < 0 || head >= TF_HEAD_COUNT) return NULL;
  const float *p = *slot((ZfTajweedHeads *)h, head);
  if (frames) *frames = kHeads[head].rate_ms == 40 ? h->n40 : h->n20;
  return p;
}
/* keep only the heads in `mask` */
void tf__heads_select(ZfTajweedHeads *h, unsigned mask) {
  for (int k = 0; k < TF_HEAD_COUNT; ++k)
    if (!(mask & TF_HEAD_BIT(k))) *slot(h, k) = NULL;
}

/* ------------------------------------------------------------------ streams */

int tf__model_has_engine(const TfModel *m);
static volatile int g_live; /* the engine allows one live stream per process */

int tf__model_sample_rate(const TfModel *m);
TfShareSession *tf__share_begin(TfClient *c, int input_sample_rate, const char *input_route, const char *revision);

struct TfStream {
  TfClient *client;     /* recitation sharing: the take is shared while the learner's consent is on */
  char route[16];
  TfShareSession *share;
  TfModel *m;
  ZfTajweedStream *zs;
  unsigned mask;
  TfChunkCallback cb;
  void *user;
  int chunk;
  int frames40;
  int chunk_ms;
};

TfStreamOptions tf_stream_default_options(void) {
  TfStreamOptions o;
  memset(&o, 0, sizeof o);
  o.struct_size = sizeof o;
  return o;
}

static void on_chunk(void *user, const ZfTajweedHeads *h) {
  TfStream *s = (TfStream *)user;
  if (s->cb) {
    ZfTajweedHeads sel = *h;
    tf__heads_select(&sel, s->mask);
    TfChunk c;
    c.index = s->chunk;
    c.frame0 = s->frames40;
    c.start_ms = 40 * s->frames40;
    c.end_ms = 40 * (s->frames40 + h->n40);
    c.heads = &sel;
    s->cb(s->user, &c);
  }
  s->chunk++;
  s->frames40 += h->n40;
}

/* tf's head bits are the engine's ZF_TAJWEED_HEAD_* bits (the same order as ZfTajweedHeads) */
_Static_assert(TF_HEAD_BIT(TF_HEAD_LOG_PROBS) == ZF_TAJWEED_HEAD_LOG_PROBS && TF_HEAD_BIT(TF_HEAD_INTER1) == ZF_TAJWEED_HEAD_INTER1 &&
               TF_HEAD_BIT(TF_HEAD_INTER4) == ZF_TAJWEED_HEAD_INTER4 && TF_HEAD_BIT(TF_HEAD_ID) == ZF_TAJWEED_HEAD_ID &&
               TF_HEAD_BIT(TF_HEAD_TIMING) == ZF_TAJWEED_HEAD_TIMING && TF_HEAD_BIT(TF_HEAD_GEM) == ZF_TAJWEED_HEAD_GEM &&
               TF_HEAD_BIT(TF_HEAD_TAFKHEEM) == ZF_TAJWEED_HEAD_TAFKHEEM && TF_HEAD_BIT(TF_HEAD_GHUNNA) == ZF_TAJWEED_HEAD_GHUNNA &&
               TF_HEAD_BIT(TF_HEAD_QALQALAH) == ZF_TAJWEED_HEAD_QALQALAH && TF_HEAD_BIT(TF_HEAD_FACE) == ZF_TAJWEED_HEAD_FACE &&
               TF_HEAD_BIT(TF_HEAD_BOUNDARY) == ZF_TAJWEED_HEAD_BOUNDARY && TF_HEAD_BIT(TF_HEAD_CONF) == ZF_TAJWEED_HEAD_CONF &&
               TF_HEADS_SCORER == ZF_TAJWEED_HEADS_SCORER && TF_HEADS_ALL == ZF_TAJWEED_HEADS_ALL,
               "tf head bits must equal the engine's");

/* the heads the engine computes for this take (API 3, lazy heads): the selection plus the scorer's; heads
 * nobody asked for are not computed at all. Called at the start of every take (create, reset). */
static void select_heads(TfStream *s) { zf_tajweed_stream_set_heads(s->zs, s->mask | TF_HEADS_SCORER); }

TfStream *tf_stream_create(TfModel *m, const TfStreamOptions *opt) {
  tf__clear_error();
  if (!m) {
    tf__fail(TF_ERR_ARGUMENT, "no model");
    return NULL;
  }
  if (!tf__model_has_engine(m)) {
    tf__fail(TF_ERR_NOT_AVAILABLE, "the model was opened without the engine (load_engine 0)");
    return NULL;
  }
  if (g_live) {
    tf__fail(TF_ERR_BUSY, "one live stream per process (the engine's rule): destroy the other stream first");
    return NULL;
  }
  TfStreamOptions o = tf_stream_default_options();
  if (opt) memcpy(&o, opt, opt->struct_size && opt->struct_size < sizeof o ? opt->struct_size : sizeof o);
  TfStream *s = (TfStream *)calloc(1, sizeof *s);
  if (!s) {
    tf__fail(TF_ERR_NO_MEMORY, "out of memory");
    return NULL;
  }
  const char *err = NULL;
  s->zs = zf_tajweed_stream_create(tf_model_engine(m), &err);
  if (!s->zs) {
    free(s);
    tf__fail(TF_ERR_ENGINE, "engine: %s", err ? err : "cannot create a stream");
    return NULL;
  }
  g_live = 1;
  s->m = m;
  s->mask = (o.heads ? o.heads : TF_HEADS_DEFAULT) & TF_HEADS_ALL;
  s->cb = o.on_chunk;
  s->user = o.user;
  s->chunk_ms = tf_model_chunk_ms(m);
  if (o.struct_size >= offsetof(TfStreamOptions, input_route) + sizeof(o.input_route) && o.client) {
    s->client = o.client;
    snprintf(s->route, sizeof s->route, "%s", o.input_route ? o.input_route : "unknown");
  }
  zf_tajweed_stream_set_callback(s->zs, on_chunk, s);
  select_heads(s);
  return s;
}

/* the shared copy of the take: begun at its first push (when sharing is on), finished with the take */
static void share_push(TfStream *s, const float *x, int n) {
  if (!s->client || n <= 0) return;
  if (!s->share) {
    char rev[80] = "";
    const char *info = tf_model_info_json(s->m);
    TfJson *j = info ? tf_json_parse(info, strlen(info), NULL) : NULL;
    const char *id = tf_json_str(tf_json_get(j, "model_id")), *r = tf_json_str(tf_json_get(j, "revision"));
    if (id && r) snprintf(rev, sizeof rev, "%s+%s", id, r);
    tf_json_free(j);
    s->share = tf__share_begin(s->client, tf__model_sample_rate(s->m), s->route, rev);
    if (!s->share) {
      tf__clear_error(); /* sharing off is not an error of the stream */
      return;
    }
  }
  tf_sharing_session_push(s->share, x, n);
}
static void share_end(TfStream *s, int completed) {
  if (s->share) tf_sharing_session_finish(s->share, completed);
  s->share = NULL;
}

void tf_stream_destroy(TfStream *s) {
  if (!s) return;
  share_end(s, 0);
  zf_tajweed_stream_destroy(s->zs);
  g_live = 0;
  free(s);
}

int tf_stream_push_audio(TfStream *s, const float *samples, int n) {
  if (!s) return tf__fail(TF_ERR_ARGUMENT, "no stream");
  const int r = zf_tajweed_stream_push_audio(s->zs, samples, n);
  if (r < 0) {
    const char *e = zf_tajweed_stream_last_error(s->zs);
    return tf__fail(TF_ERR_ENGINE, "engine: %s", e ? e : "push failed");
  }
  share_push(s, samples, n);
  return r;
}
int tf_stream_finish(TfStream *s) {
  if (!s) return tf__fail(TF_ERR_ARGUMENT, "no stream");
  const int r = zf_tajweed_stream_finish(s->zs);
  share_end(s, 1);
  if (r < 0) {
    const char *e = zf_tajweed_stream_last_error(s->zs);
    return tf__fail(TF_ERR_ENGINE, "engine: %s", e ? e : "finish failed");
  }
  return r;
}
void tf_stream_reset(TfStream *s) {
  if (!s) return;
  share_end(s, 0); /* a take reset before its end: kept only from 2 s */
  zf_tajweed_stream_reset(s->zs);
  select_heads(s);
  s->chunk = 0;
  s->frames40 = 0;
}
int tf_stream_heads(const TfStream *s, ZfTajweedHeads *out) {
  if (!s || !out) return tf__fail(TF_ERR_ARGUMENT, "no stream");
  if (zf_tajweed_stream_heads(s->zs, out) < 0) return tf__fail(TF_ERR_ENGINE, "engine: no heads");
  tf__heads_select(out, s->mask | TF_HEADS_SCORER);
  return 0;
}
ZfTajweedStream *tf__stream_engine(const TfStream *s) { return s ? s->zs : NULL; }
/* the decoded layer's top-1 and runner-up of the take, kept by the engine as chunks arrive (API 3) */
int tf__stream_top2(const TfStream *s, const ZfTajweedTop2 **out) { return zf_tajweed_stream_top2(s->zs, out); }
/* every head the engine kept, whatever the selection (for scoring) */
int tf__stream_all_heads(const TfStream *s, ZfTajweedHeads *out) { return zf_tajweed_stream_heads(s->zs, out); }
TfModel *tf__stream_model(TfStream *s) { return s->m; }
int tf_stream_greedy(const TfStream *s, int *ids, int max) {
  if (!s) return tf__fail(TF_ERR_ARGUMENT, "no stream");
  return zf_tajweed_stream_greedy(s->zs, ids, max);
}
