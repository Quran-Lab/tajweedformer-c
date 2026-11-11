/* Heads: the twelve outputs of the network (their names, shapes and classes) and the Heads object the public API
 * hands out: a view of one chunk's heads, or the copies a finished take keeps. */
#include <math.h>
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

/* tf's head bits are the engine's ZF_TAJWEED_HEAD_* bits (the same order as ZfTajweedHeads) */
_Static_assert(TF_HEAD_BIT(TF_HEAD_LOG_PROBS) == ZF_TAJWEED_HEAD_LOG_PROBS && TF_HEAD_BIT(TF_HEAD_INTER1) == ZF_TAJWEED_HEAD_INTER1 &&
               TF_HEAD_BIT(TF_HEAD_INTER4) == ZF_TAJWEED_HEAD_INTER4 && TF_HEAD_BIT(TF_HEAD_ID) == ZF_TAJWEED_HEAD_ID &&
               TF_HEAD_BIT(TF_HEAD_TIMING) == ZF_TAJWEED_HEAD_TIMING && TF_HEAD_BIT(TF_HEAD_GEM) == ZF_TAJWEED_HEAD_GEM &&
               TF_HEAD_BIT(TF_HEAD_TAFKHEEM) == ZF_TAJWEED_HEAD_TAFKHEEM && TF_HEAD_BIT(TF_HEAD_GHUNNA) == ZF_TAJWEED_HEAD_GHUNNA &&
               TF_HEAD_BIT(TF_HEAD_QALQALAH) == ZF_TAJWEED_HEAD_QALQALAH && TF_HEAD_BIT(TF_HEAD_FACE) == ZF_TAJWEED_HEAD_FACE &&
               TF_HEAD_BIT(TF_HEAD_BOUNDARY) == ZF_TAJWEED_HEAD_BOUNDARY && TF_HEAD_BIT(TF_HEAD_CONF) == ZF_TAJWEED_HEAD_CONF &&
               TF_HEADS_ALL == ZF_TAJWEED_HEADS_ALL,
               "tf head bits must equal the engine's");

const TfHeadInfo *tf_head_info(int h) { return h >= 0 && h < TF_HEAD_COUNT ? &kHeads[h] : NULL; }
int tf_head_from_name(const char *name) {
  if (!name) return -1;
  for (int h = 0; h < TF_HEAD_COUNT; ++h)
    if (!strcmp(name, kHeads[h].name)) return h;
  return -1;
}

unsigned tf__heads_mask(const char *list) {
  if (!list || !*list) return TF_HEADS_DEFAULT;
  if (!strcmp(list, "all")) return TF_HEADS_ALL;
  unsigned m = 0;
  for (const char *s = list; *s;) {
    while (*s == ' ' || *s == ',') ++s;
    const char *e = s;
    while (*e && *e != ',' && *e != ' ') ++e;
    if (e == s) break;
    int found = -1;
    for (int h = 0; h < TF_HEAD_COUNT; ++h)
      if (strlen(kHeads[h].name) == (size_t)(e - s) && !strncmp(kHeads[h].name, s, (size_t)(e - s))) found = h;
    if (found < 0) {
      tf__fail(TF_ERR_INVALID_ARGUMENT, "%.*s is not a head of the model", (int)(e - s), s);
      return 0;
    }
    m |= TF_HEAD_BIT(found);
    s = e;
  }
  if (!m) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "the head list is empty");
    return 0;
  }
  return m | TF_HEAD_BIT(TF_HEAD_LOG_PROBS); /* log_probs is always computed */
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
const float *tf__heads_array(const ZfTajweedHeads *h, int head, int *frames) {
  if (!h || head < 0 || head >= TF_HEAD_COUNT) return NULL;
  const float *p = *slot((ZfTajweedHeads *)h, head);
  if (frames) *frames = kHeads[head].rate_ms == 40 ? h->n40 : h->n20;
  return p;
}
void tf__heads_select(ZfTajweedHeads *h, unsigned mask) {
  for (int k = 0; k < TF_HEAD_COUNT; ++k)
    if (!(mask & TF_HEAD_BIT(k))) *slot(h, k) = NULL;
}

/* ------------------------------------------------------------------ the Heads object */

static void name_list(TfHeads *out) {
  out->n_names = 0;
  for (int k = 0; k < TF_HEAD_COUNT; ++k)
    if (*slot(&out->z, k)) out->names[out->n_names++] = kHeads[k].name;
}

void tf__heads_view(TfHeads *out, const ZfTajweedHeads *z, unsigned mask, const TfModel *m) {
  memset(out, 0, sizeof *out);
  out->z = *z;
  tf__heads_select(&out->z, mask);
  out->mask = 0;
  for (int k = 0; k < TF_HEAD_COUNT; ++k)
    if (*slot(&out->z, k)) out->mask |= TF_HEAD_BIT(k);
  out->m = m;
  name_list(out);
}

int tf__heads_own(TfHeads *out, const ZfTajweedHeads *z, unsigned mask, const TfModel *m) {
  tf__heads_view(out, z, mask, m);
  for (int k = 0; k < TF_HEAD_COUNT; ++k) {
    const float **p = slot(&out->z, k);
    if (!*p) continue;
    const size_t n = (size_t)(kHeads[k].rate_ms == 40 ? z->n40 : z->n20) * (size_t)kHeads[k].width;
    out->own[k] = (float *)malloc((n + 1) * sizeof(float));
    if (!out->own[k]) {
      tf__heads_release(out);
      return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
    }
    memcpy(out->own[k], *p, n * sizeof(float));
    *p = out->own[k];
  }
  return 0;
}

void tf__heads_release(TfHeads *h) {
  if (!h) return;
  for (int k = 0; k < TF_HEAD_COUNT; ++k) {
    free(h->own[k]);
    free(h->greedy[k]);
    h->own[k] = NULL;
    h->greedy[k] = NULL;
  }
  if (!h->decoded_borrowed) tf__decoded_free(h->decoded);
  h->decoded = NULL;
}

int tf_heads_count(const TfHeads *h) { return h ? h->n_names : 0; }
const char *tf_heads_name(const TfHeads *h, int i) { return h && i >= 0 && i < h->n_names ? h->names[i] : NULL; }

int tf_heads_get(const TfHeads *h, const char *name, TfHead *out) {
  if (!h || !out) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no heads");
  const int k = tf_head_from_name(name);
  int frames = 0;
  const float *v = k >= 0 ? tf__heads_array(&h->z, k, &frames) : NULL;
  if (!v) return tf__fail(TF_ERR_INVALID_ARGUMENT, "these heads do not hold %s", name ? name : "(no name)");
  out->name = kHeads[k].name;
  out->kind = !strcmp(kHeads[k].kind, "log_softmax") ? "ctc" : "attribute";
  out->frame_ms = kHeads[k].rate_ms;
  out->frames = frames;
  out->classes = kHeads[k].width;
  out->values = v;
  out->class_names = kHeads[k].n_classes ? kHeads[k].classes : NULL;
  return 0;
}

int tf_heads_greedy(const TfHeads *hc, const char *name, const TfToken **tokens) {
  TfHeads *h = (TfHeads *)hc; /* the token arrays are built on demand and kept with the Heads */
  if (!h) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no heads");
  const int k = name ? tf_head_from_name(name) : TF_HEAD_LOG_PROBS;
  int frames = 0;
  const float *v = k >= 0 ? tf__heads_array(&h->z, k, &frames) : NULL;
  if (!v || strcmp(kHeads[k].kind, "log_softmax"))
    return tf__fail(TF_ERR_INVALID_ARGUMENT, "%s is not a ctc head these heads hold", name ? name : "log_probs");
  if (!h->greedy[k]) {
    const int with_conf = k == TF_HEAD_LOG_PROBS && h->z.conf;
    const int n = tf__greedy(h->m, v, frames, kHeads[k].rate_ms, with_conf ? h->z.conf : NULL, with_conf ? h->z.n20 : 0, NULL, NULL,
                             &h->greedy[k]);
    if (n < 0) return tf__error_code();
    h->n_greedy[k] = n;
  }
  if (tokens) *tokens = h->greedy[k];
  return h->n_greedy[k];
}

const TfDecoded *tf_heads_decoded(const TfHeads *hc) {
  TfHeads *h = (TfHeads *)hc;
  if (!h) {
    tf__raise(tf__null_handle("heads"));
    return NULL;
  }
  if (!h->decoded) {
    h->decoded = tf__decode(h->m, &h->z, NULL, h->top_k);
    if (h->decoded) tf__decoded_mark(h->decoded, h->is_final ? h->decoded->n_tokens : -1, h->is_final);
  }
  return h->decoded;
}
