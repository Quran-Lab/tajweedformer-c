/* The decoded layer: per 40 ms frame the top-1 and runner-up, the greedy tokens with peak log-prob, runner-up,
 * margin and conf-head confidence, the words between word bars, and (3.0) how many leading tokens are settled.
 * The numbers are pinned by the shared layers vectors (layers_ref.py). The SDK never compares any of it with an
 * expected ayah. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

#define BLANK 0
#define BAR 1

typedef struct {
  TfDecoded pub; /* first: the public pointer is this struct's */
  const TfModel *m;
  TfFrame *frames;
  TfToken *tokens;
  TfWord *words;
  TfTopK *ranked; /* log_probs ranked to depth k per frame */
  char *text;
  char *json;
} Decoded;

/* Greedy CTC over one head: runs of the same best token, blank removed. top2: the engine's per-frame top-1 and
 * runner-up (zf_tajweed_stream_top2 for a take, zf_tajweed_top2 otherwise; ties to the lower id). The margin is
 * recomputed in double from the two float log-probs, as the layers vectors define it (the engine's own margin is
 * float). */
int tf__greedy(const TfModel *m, const float *log_probs, int T, int frame_ms, const float *conf, int n20, const ZfTajweedTop2 *top2,
               TfFrame *frames_out, TfToken **out) {
  if (!log_probs || T < 0 || !out) return tf__fail(TF_ERR_INVALID_ARGUMENT, "the heads need log_probs");
  ZfTajweedTop2 *own = NULL;
  TfFrame *fr = frames_out ? frames_out : (TfFrame *)calloc((size_t)T + 1, sizeof(TfFrame));
  TfToken *tokens = (TfToken *)calloc((size_t)T + 1, sizeof(TfToken));
  int n = 0;
  if (!fr || !tokens) goto oom;
  if (!top2 && T > 0) {
    own = (ZfTajweedTop2 *)malloc(sizeof(ZfTajweedTop2) * (size_t)T);
    if (!own) goto oom;
    if (zf_tajweed_top2(log_probs, T, ZF_TAJWEED_VOCAB, own) != T) {
      free(own);
      if (!frames_out) free(fr);
      free(tokens);
      return tf__fail(TF_ERR_ENGINE_FAILED, "zf_tajweed_top2 failed");
    }
    top2 = own;
  }
  for (int t = 0; t < T; ++t) {
    TfFrame *f = &fr[t];
    f->top1 = top2[t].token;
    f->top1_log_prob = top2[t].log_prob;
    f->runner_up = top2[t].runner_up;
    f->runner_up_log_prob = top2[t].runner_up_log_prob;
    f->margin = (double)top2[t].log_prob - (double)top2[t].runner_up_log_prob;
  }
  free(own);
  for (int t = 0; t < T;) {
    const int a = fr[t].top1;
    if (a == BLANK) {
      ++t;
      continue;
    }
    int e = t;
    while (e + 1 < T && fr[e + 1].top1 == a) ++e;
    int pk = t;
    for (int k = t + 1; k <= e; ++k)
      if (fr[k].top1_log_prob > fr[pk].top1_log_prob) pk = k;
    TfToken *tk = &tokens[n++];
    tk->id = a;
    tk->name = m ? tf_model_token_name(m, a) : NULL;
    tk->first_frame = t;
    tk->last_frame = e;
    tk->peak_frame = pk;
    tk->first_ms = frame_ms * t;
    tk->peak_ms = frame_ms * pk;
    tk->last_ms = frame_ms * e;
    tk->start_ms = frame_ms * t;
    tk->end_ms = frame_ms * (e + 1);
    tk->log_prob = fr[pk].top1_log_prob;
    tk->runner_up = fr[pk].runner_up;
    tk->runner_up_log_prob = fr[pk].runner_up_log_prob;
    tk->margin = fr[pk].margin;
    if (conf && frame_ms == 40 && n20 >= 2 * (e + 1)) {
      double s = 0;
      for (int k = 2 * t; k < 2 * e + 2; ++k) s += 1.0 / (1.0 + exp(-(double)conf[k]));
      tk->confidence = s / (2 * (e - t + 1));
    } else {
      tk->confidence = NAN;
    }
    t = e + 1;
  }
  if (!frames_out) free(fr);
  *out = tokens;
  return n;
oom:
  if (!frames_out) free(fr);
  free(tokens);
  return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
}

TfDecoded *tf__decode(const TfModel *m, const ZfTajweedHeads *h, const ZfTajweedTop2 *top2, int k) {
  if (!h || !h->log_probs || h->n40 < 0) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "the heads need log_probs");
    return NULL;
  }
  if (k < 0 || k > ZF_TAJWEED_VOCAB) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "top_k must be 0 to %d", ZF_TAJWEED_VOCAB);
    return NULL;
  }
  const int T = h->n40;
  Decoded *d = (Decoded *)calloc(1, sizeof *d);
  if (!d) goto oom;
  d->m = m;
  d->frames = (TfFrame *)calloc((size_t)T + 1, sizeof(TfFrame));
  d->words = (TfWord *)calloc((size_t)T + 1, sizeof(TfWord));
  if (!d->frames || !d->words) goto oom;
  const int nt = tf__greedy(m, h->log_probs, T, 40, h->conf, h->n20, top2, d->frames, &d->tokens);
  if (nt < 0) {
    tf__decoded_free(&d->pub);
    return NULL;
  }
  /* words: the tokens between "|" tokens */
  int nw = 0, first = -1;
  for (int i = 0; i <= nt; ++i) {
    const int bar = i == nt || d->tokens[i].id == BAR;
    if (!bar) {
      if (first < 0) first = i;
      continue;
    }
    if (first >= 0) {
      TfWord *s = &d->words[nw++];
      s->first_token = first;
      s->n_tokens = i - first;
      s->start_ms = d->tokens[first].start_ms;
      s->end_ms = d->tokens[i - 1].end_ms;
      s->confidence = d->tokens[first].confidence;
      s->margin = d->tokens[first].margin;
      for (int j = first + 1; j < i; ++j) {
        if (d->tokens[j].confidence != d->tokens[j].confidence || s->confidence != s->confidence) s->confidence = NAN;
        else if (d->tokens[j].confidence < s->confidence) s->confidence = d->tokens[j].confidence;
        if (d->tokens[j].margin < s->margin) s->margin = d->tokens[j].margin;
      }
    }
    first = -1;
  }
  /* the text: the token names (ids without a model), separated by spaces */
  {
    TfBuf tr;
    tf_buf_init(&tr);
    tf_buf_puts(&tr, "");
    for (int i = 0; i < nt; ++i) {
      if (i) tf_buf_puts(&tr, " ");
      if (d->tokens[i].name) tf_buf_puts(&tr, d->tokens[i].name);
      else tf_buf_printf(&tr, "%d", d->tokens[i].id);
    }
    d->text = tf_buf_take(&tr);
    if (!d->text) goto oom;
  }
  if (k >= 1) {
    /* depth 1 is the frames' top-1 again, depth 2 its runner-up too, through the ranking call */
    d->ranked = tf__topk(h, TF_HEAD_LOG_PROBS, k);
    if (!d->ranked) {
      tf__decoded_free(&d->pub);
      return NULL;
    }
    d->pub.top_k = k;
    d->pub.ranked_ids = d->ranked->ids;
    d->pub.ranked_log_probs = d->ranked->values;
    d->pub.ranked_margins = d->ranked->margins;
  }
  d->pub.text = d->text;
  d->pub.n_tokens = nt;
  d->pub.tokens = d->tokens;
  d->pub.n_words = nw;
  d->pub.words = d->words;
  d->pub.n_frames = T;
  d->pub.frames = d->frames;
  tf__decoded_mark(&d->pub, -1, 0);
  return &d->pub;
oom:
  tf__decoded_free(d ? &d->pub : NULL);
  tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  return NULL;
}

/* settled < 0: by the frames so far. A token is settled once its run of frames has ended: a later frame exists
 * and holds another best token (blank included). The last token is still open while its run reaches the last
 * frame so far: the next chunk may extend it and move its peak, last frame and confidence. The frames of a chunk
 * never change afterwards (the engine is causal), so a settled token stays as it is for the rest of the take. */
void tf__decoded_mark(TfDecoded *pub, int settled, int is_final) {
  if (!pub) return;
  Decoded *d = (Decoded *)pub;
  if (is_final) settled = pub->n_tokens;
  if (settled < 0) {
    settled = pub->n_tokens;
    if (pub->n_tokens > 0 && d->tokens[pub->n_tokens - 1].last_frame == pub->n_frames - 1) settled--;
  }
  pub->settled = settled > pub->n_tokens ? pub->n_tokens : settled;
  pub->is_final = is_final ? 1 : 0;
}

void tf__decoded_free(TfDecoded *pub) {
  if (!pub) return;
  Decoded *d = (Decoded *)pub;
  free(d->frames);
  free(d->tokens);
  free(d->words);
  tf__topk_free(d->ranked);
  free(d->text);
  free(d->json);
  free(d);
}

static void unit(TfBuf *b, const Decoded *d, int id) {
  const char *u = d->m ? tf_model_token_name(d->m, id) : NULL;
  if (u) tf_buf_json_str(b, u);
  else tf_buf_puts(b, "null");
}

const char *tf_decoded_json(const TfDecoded *pub, int with_frames) {
  if (!pub) return NULL;
  Decoded *d = (Decoded *)pub;
  free(d->json);
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{");
  if (with_frames) {
    tf_buf_puts(&b, "\"frames\":[");
    for (int t = 0; t < pub->n_frames; ++t) {
      const TfFrame *f = &d->frames[t];
      if (t) tf_buf_puts(&b, ",");
      tf_buf_printf(&b, "{\"top1\":%d,\"top1LogProb\":", f->top1);
      tf_buf_num(&b, f->top1_log_prob);
      tf_buf_printf(&b, ",\"runnerUp\":%d,\"runnerUpLogProb\":", f->runner_up);
      tf_buf_num(&b, f->runner_up_log_prob);
      tf_buf_puts(&b, ",\"margin\":");
      tf_buf_num(&b, f->margin);
      if (d->ranked && t < d->ranked->frames) {
        /* the frame's k best tokens: id, unit, log-prob, margin to the next rank */
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
  for (int i = 0; i < pub->n_tokens; ++i) {
    const TfToken *t = &d->tokens[i];
    if (i) tf_buf_puts(&b, ",");
    tf_buf_printf(&b, "{\"id\":%d,\"unit\":", t->id);
    unit(&b, d, t->id);
    tf_buf_printf(&b, ",\"frame0\":%d,\"frame1\":%d,\"startMs\":%d,\"endMs\":%d,\"peakFrame\":%d,\"logProb\":", t->first_frame, t->last_frame,
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
  for (int i = 0; i < pub->n_words; ++i) {
    const TfWord *s = &d->words[i];
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
  tf_buf_json_str(&b, d->text ? d->text : "");
  tf_buf_printf(&b, ",\"settled\":%d,\"isFinal\":%s}", pub->settled, pub->is_final ? "true" : "false");
  d->json = tf_buf_take(&b);
  return d->json;
}
