/* The Session: one take. Audio in (pushed by the app, or a sample clip through the same path), chunks and events
 * out, the take's Heads at the end. The engine's stream belongs to the Model and is reused from take to take; the
 * Session selects the heads, turns the engine's chunks into TfChunk (the chunk's Heads, the decoded layer of the
 * take so far, whether the chunk holds speech), raises the speech events from the model's own output
 * (tf_speech.c), shares the take when the learner agreed, and counts it for the telemetry.
 *
 * The last chunk's decoded layer carries is_final. The engine only says that a take is over when its finish call
 * returns, so the chunks that call completes are kept (their heads copied, a few frames each) and handed over
 * right after it, the last one marked. Tried and rejected: holding every chunk back until the next one arrives
 * (a whole chunk of delay in a streaming UI for the sake of one flag).
 *
 * With end_on_silence_ms the take ends at the chunk that carries the frame of speechEnded: that chunk is
 * delivered whole and is the last (is_final), later audio is not decoded (no flush), ended(silence) follows.
 * Order for one chunk: speechStarted or speechEnded of its frames, then the chunk, then ended. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

enum { OPEN = 0, ENDED = 1, CANCELLED = 2, FAILED = 3 };

typedef struct {
  TfHeads heads; /* an owned copy of one chunk's heads */
  int n40;
} Kept;

struct TfSession {
  TfModel *m;
  TfClient *client;
  ZfTajweedStream *zs;
  unsigned mask;
  char context[32];
  char route[16];
  int rate, top_k, end_ms, pace;
  void (*on_chunk)(void *user, const TfChunk *chunk);
  void (*on_event)(void *user, const TfEvent *event);
  void *user;
  int state;
  int chunk;       /* chunks handed over so far */
  int frames40;    /* 40 ms frames of the take so far */
  TfSpeech speech;
  int any_token;   /* the model emitted a token in this take */
  int active_frames; /* the take's speech frames so far (tf_speech.c) */
  int silence_end; /* the chunk that ended the take on silence was delivered: the take is wrapped up after the push */
  int finishing;   /* inside the app's finish: the flush's chunks are kept, and a silence in them ends nothing */
  Kept *kept;
  int n_kept, cap_kept, kept_peak;
  TfDecoded *so_far; /* the decoded layer handed over with the last chunk; replaced at the next */
  TfHeads take;      /* the take's Heads once it has ended */
  int have_take;
  TfShareSession *share;
  double compute_s;
  long long compute_us;
  long long samples;
  int from_clip;
  TfRecited recited;
  int failed_code;
};

ZfTajweedStream *tf__session_engine(const TfSession *s) { return s ? s->zs : NULL; }

static void backend_cpu(TfBackend *b) {
  b->kind = "cpu";
  b->name = "cpu";
  b->stage = "cpu";
  b->reason = NULL;
}

/* the names of the events and of the reasons a take ends by itself: the same in every SDK (NAMING.md section 4) */
const char *tf__event_name(int type) {
  static const char *const names[] = {"", "speechStarted", "speechEnded", "interrupted", "resumed", "backendChanged", "ended"};
  return type >= TF_EVENT_SPEECH_STARTED && type <= TF_EVENT_ENDED ? names[type] : NULL;
}
const char *tf__ended_reason_name(int reason) {
  static const char *const reasons[] = {"", "silence", "background", "hidden", "interrupted", "sourceEnd", "limit", "error"};
  return reason >= TF_ENDED_SILENCE && reason <= TF_ENDED_ERROR ? reasons[reason] : NULL;
}

int tf__take_max_s = TF_TAKE_MAX_S;
static long long take_max_samples(const TfSession *s) {
  return (long long)tf__take_max_s * s->rate;
}
/* end_on_silence_ms: off (0), or 100 to 60000 */
int tf__end_on_silence_ok(int ms) { return ms == 0 || (ms >= 500 && ms <= 60000); }

static void emit(TfSession *s, int type, int at_ms, int silence_ms, int reason, int error) {
  if (!s->on_event) return;
  TfEvent e;
  memset(&e, 0, sizeof e);
  e.type = type;
  e.name = tf__event_name(type);
  e.at_ms = at_ms;
  e.silence_ms = silence_ms;
  e.reason = reason;
  e.reason_name = type == TF_EVENT_ENDED ? tf__ended_reason_name(reason) : NULL;
  e.error = error;
  s->on_event(s->user, &e);
}

/* the greedy id of one frame of log_probs: the largest log-prob, the lowest id on ties */
static int greedy_id(const float *row) {
  int a = 0;
  for (int i = 1; i < ZF_TAJWEED_VOCAB; ++i)
    if (row[i] > row[a]) a = i;
  return a;
}

/* Hands one chunk over: the speech events of its frames, then the chunk (its Heads, the decoded layer of the take
 * up to its end, whether it holds speech). Returns 1 when the chunk ended the take on silence. */
static int deliver(TfSession *s, TfHeads *chunk, int n40, const ZfTajweedHeads *all, const ZfTajweedTop2 *top2, int is_final) {
  ZfTajweedHeads upto = *all;
  upto.n40 = s->frames40 + n40;
  upto.n20 = 2 * upto.n40;
  /* the timing head is computed for every take (the speech rule needs it); the app gets it only when selected */
  const float *timing = all->timing_log_probs;
  tf__heads_select(&upto, s->mask);
  int speech = 0, ended = 0;
  for (int t = s->frames40; t < upto.n40 && upto.log_probs; ++t) {
    const int id = top2 ? top2[t].token : greedy_id(upto.log_probs + (size_t)t * ZF_TAJWEED_VOCAB);
    const int ta = timing ? greedy_id(timing + (size_t)(2 * t) * ZF_TAJWEED_VOCAB) : 0;
    const int tb = timing ? greedy_id(timing + (size_t)(2 * t + 1) * ZF_TAJWEED_VOCAB) : 0;
    const int active = tf__speech_active(id, ta, tb);
    TfSpeechEvent ev;
    if (id != ZF_TAJWEED_BLANK) s->any_token = 1;
    if (tf__speech_frame(&s->speech, active, &ev)) emit(s, ev.type, ev.at_ms, ev.silence_ms, 0, 0);
    speech |= active;
    s->active_frames += active;
  }
  if (s->speech.ended && !s->finishing) ended = 1; /* this chunk is whole and is the last */
  if (s->on_chunk) {
    tf__decoded_free(s->so_far);
    s->so_far = tf__decode(s->m, &upto, top2, s->top_k);
    if (s->so_far) tf__decoded_mark(s->so_far, -1, is_final || ended);
    TfChunk c;
    memset(&c, 0, sizeof c);
    c.index = s->chunk;
    c.first_frame = s->frames40;
    c.start_ms = 40 * s->frames40;
    c.end_ms = 40 * (s->frames40 + n40);
    backend_cpu(&c.backend);
    c.speech = speech;
    c.heads = chunk;
    c.decoded = s->so_far;
    s->on_chunk(s->user, &c);
  }
  s->chunk++;
  s->frames40 += n40;
  return ended;
}

/* the take's Heads, copied out of the engine's stream (which the next take will reuse), and its final decoded
 * layer; `all` holds exactly the take's frames */
static int keep_take(TfSession *s, const ZfTajweedHeads *all, const ZfTajweedTop2 *top2) {
  const int rc = tf__heads_own(&s->take, all, s->mask, s->m);
  if (rc) return rc;
  s->take.top_k = s->top_k;
  s->take.is_final = 1;
  s->take.decoded = tf__decode(s->m, &s->take.z, top2, s->top_k);
  if (s->take.decoded) tf__decoded_mark(s->take.decoded, -1, 1);
  s->have_take = 1;
  return 0;
}

/* What a Session holds of a take: no audio (the engine takes each pushed buffer as it comes) and, only inside
 * finish, the chunks the engine's flush completes (the last one is not known to be the last until the flush
 * returns): the rest of one chunk and the model's lookahead, a handful at most. The tests read the peak. */
int tf__session_kept_peak(const TfSession *s) { return s ? s->kept_peak : 0; }

/* the engine's callback, once per chunk, inside a push or the finish call */
static void on_engine_chunk(void *user, const ZfTajweedHeads *h) {
  TfSession *s = (TfSession *)user;
  if (s->state != OPEN || s->silence_end) return; /* audio after the end of the take is not decoded */
  if (s->finishing) {
    if (s->n_kept == s->cap_kept) {
      const int cap = s->cap_kept ? 2 * s->cap_kept : 8;
      Kept *k = (Kept *)realloc(s->kept, (size_t)cap * sizeof(Kept));
      if (!k) return;
      s->kept = k;
      s->cap_kept = cap;
    }
    Kept *k = &s->kept[s->n_kept];
    if (tf__heads_own(&k->heads, h, s->mask, s->m)) return;
    k->n40 = h->n40;
    s->n_kept++;
    if (s->n_kept > s->kept_peak) s->kept_peak = s->n_kept;
    return;
  }
  ZfTajweedHeads all;
  const ZfTajweedTop2 *top2 = NULL;
  if (zf_tajweed_stream_heads(s->zs, &all) < 0) return;
  if (zf_tajweed_stream_top2(s->zs, &top2) < s->frames40 + h->n40) top2 = NULL;
  TfHeads view;
  tf__heads_view(&view, h, s->mask, s->m);
  const int ended = deliver(s, &view, h->n40, &all, top2, 0);
  tf__heads_release(&view);
  if (ended) {
    /* the take ends here, on silence: its Heads are the frames up to this chunk */
    all.n40 = s->frames40;
    all.n20 = 2 * s->frames40;
    s->failed_code = keep_take(s, &all, top2);
    s->silence_end = 1;
  }
}

static TfSession *session_new(TfModel *m, const TfSessionOptions *opt, int rate_override) {
  if (!m) {
    tf__null_handle("model");
    return NULL;
  }
  tf__clear_error();
  TfSessionOptions o;
  memset(&o, 0, sizeof o);
  if (opt) o = *opt;
  if (o.backend && strcmp(o.backend, "auto") && strcmp(o.backend, "cpu")) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "backend %s is not auto or cpu", o.backend);
    return NULL;
  }
  if (o.top_k < 0 || o.top_k > ZF_TAJWEED_VOCAB) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "top_k %d is not 0 to %d", o.top_k, ZF_TAJWEED_VOCAB);
    return NULL;
  }
  if (!tf__end_on_silence_ok(o.end_on_silence_ms)) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "end_on_silence_ms %d is not 0 (off) or 500 to 60000", o.end_on_silence_ms);
    return NULL;
  }
  if (o.pace != TF_PACE_REAL_TIME && o.pace != TF_PACE_FAST) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "pace %d is not TF_PACE_REAL_TIME or TF_PACE_FAST", o.pace);
    return NULL;
  }
  const unsigned mask = tf__heads_mask(o.heads);
  if (!mask) return NULL;
  /* the context: the app's choice (an unknown one is an error, never replaced silently), else the signed
   * config's inference.context, else the manifest's default; settings apply at a take's start */
  char context[32];
  if (o.context) {
    if (!tf__model_has_context(m, o.context)) {
      tf__fail(TF_ERR_INVALID_ARGUMENT, "context %s is not one this model has", o.context);
      return NULL;
    }
    snprintf(context, sizeof context, "%s", o.context);
  } else {
    tf__model_context_in_force(m, context, sizeof context);
  }
  const int rate = rate_override ? rate_override : o.sample_rate > 0 ? o.sample_rate : 16000;
  ZfTajweedStream *zs = tf__model_stream(m, context, rate);
  if (!zs) return NULL;
  TfSession *s = (TfSession *)calloc(1, sizeof *s);
  if (!s) {
    tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
    return NULL;
  }
  if (tf__model_set_session(m, s)) {
    free(s);
    tf__fail(TF_ERR_SESSION_OPEN, NULL);
    return NULL;
  }
  s->m = m;
  s->client = tf__model_client(m);
  s->zs = zs;
  s->mask = mask;
  s->rate = rate;
  s->top_k = o.top_k;
  s->end_ms = o.end_on_silence_ms;
  s->pace = o.pace;
  s->on_chunk = o.on_chunk;
  s->on_event = o.on_event;
  s->user = o.user;
  tf__speech_init(&s->speech, o.end_on_silence_ms);
  snprintf(s->context, sizeof s->context, "%s", context);
  snprintf(s->route, sizeof s->route, "%s", o.input_route ? o.input_route : "unknown");
  /* a new take: every cache and every kept head back to the start, then the heads this take computes (a head
   * nobody asked for is not computed at all) */
  zf_tajweed_stream_reset(zs);
  zf_tajweed_stream_set_heads(zs, mask | TF_HEAD_BIT(TF_HEAD_TIMING)); /* timing always: the speech rule */
  zf_tajweed_stream_set_callback(zs, on_engine_chunk, s);
  return s;
}

TfSession *tf_session_create(TfModel *m, const TfSessionOptions *options) {
  TfSession *s = session_new(m, options, 0);
  if (!s) tf__raise(-1);
  return s;
}

/* the shared copy of the take: begun at its first samples (when sharing is on), finished with the take. Takes in
 * a row continue the client's open recording (tf_share.c): the Session only says where its take begins and ends. */
static void share_push(TfSession *s, const float *x, int n) {
  if (!s->client || n <= 0) return;
  if (!s->share) {
    s->share = tf__share_begin(s->client, s->rate, s->route, tf_model_version(s->m), tf_model_build(s->m));
    if (!s->share) {
      tf__clear_error(); /* sharing off is not an error of the take */
      return;
    }
  }
  tf__sharing_session_push(s->share, x, n);
}
static void share_end(TfSession *s, int completed) {
  if (s->share) tf__sharing_session_finish(s->share, completed);
  s->share = NULL;
}

void tf__session_detach(TfSession *s) {
  share_end(s, 0);
  s->client = NULL;
}

static int g_takes; /* takes started in this process: the first one is the launch's cold start */

/* the take for the day's telemetry (SPEC section 6): every take is a session; one that ended with its Heads also
 * counts in takes, audio_s, speech_s, silence_s, tokens and compute_s, and counts its recited report */
static void count_take(TfSession *s, int aborted) {
  if (!s->client) return;
  TfSessionStats st;
  memset(&st, 0, sizeof st);
  st.model = s->m;
  st.context = s->context;
  st.audio_s = (double)s->samples / s->rate;
  st.compute_s = s->compute_s;
  st.chunks = s->chunk;
  st.model_load_ms = tf__model_load_ms(s->m);
  st.cold_start = __atomic_fetch_add(&g_takes, 1, __ATOMIC_SEQ_CST) == 0;
  st.aborted = aborted;
  if (!aborted && s->have_take) {
    const TfDecoded *d = tf_heads_decoded(&s->take);
    st.decoded = d;
    st.feedback_shown = s->any_token; /* a finished take in which the model heard something */
    st.audio_ms = 1000LL * s->samples / s->rate;
    st.compute_us = s->compute_us;
    if (d) { /* the same frames as the events and chunk.speech */
      st.speech_frames = s->active_frames;
      st.silence_frames = d->n_frames - s->active_frames;
    }
    for (int h = 0; h < TF_HEAD_COUNT; ++h) {
      if (!(s->mask & TF_HEAD_BIT(h)) || strcmp(tf_head_info(h)->kind, "log_softmax")) continue;
      const int n = tf_heads_greedy(&s->take, tf_head_info(h)->name, NULL);
      st.tokens[h] = n > 0 ? n : 0;
    }
    tf__clear_error();
  }
  tf__client_session_finished(s->client, &st);
  tf__recited_end(&s->recited, s->client, !aborted && s->have_take);
}

/* the engine reported an error: the take is over, with nothing kept */
static int fail_take(TfSession *s, int code) {
  char text[256];
  snprintf(text, sizeof text, "%s", tf__error_text());
  s->state = FAILED;
  s->failed_code = code;
  share_end(s, 0);
  tf__model_clear_session(s->m, s);
  count_take(s, 1);
  emit(s, TF_EVENT_ENDED, 0, 0, TF_ENDED_ERROR, code);
  /* the callbacks may have used the thread's error: put this one back */
  const char *e = zf_tajweed_stream_last_error(s->zs);
  if (code == TF_ERR_ENGINE_FAILED) tf__fail(code, "%s", e ? e : "the engine stopped");
  else tf__fail(code, "%s", "the take's heads could not be kept");
  return code;
}
static int engine_failed(TfSession *s) {
  const char *e = zf_tajweed_stream_last_error(s->zs);
  return fail_take(s, tf__fail(TF_ERR_ENGINE_FAILED, "%s", e ? e : "the engine stopped"));
}

/* the take ended with its Heads: the shared copy is closed, the take is counted, and when it ended by itself it
 * says why */
static void wrap_up(TfSession *s, int reason) {
  s->state = ENDED;
  share_end(s, 1);
  tf__model_clear_session(s->m, s);
  count_take(s, 0);
  if (reason) emit(s, TF_EVENT_ENDED, 0, 0, reason, 0);
}

/* The end of the take by finish (reason 0) or because its source ran out: the engine's flush and the chunks it
 * completes (the last one final), then the take's Heads. Returns the chunks run, or a negative TfError. */
static int end_take(TfSession *s, int reason) {
  s->finishing = 1;
  const double t0 = tf__now_s();
  const int r = zf_tajweed_stream_finish(s->zs);
  const double dt = tf__now_s() - t0;
  s->compute_s += dt;
  s->compute_us += (long long)(dt * 1e6);
  if (r < 0) {
    for (int i = 0; i < s->n_kept; ++i) tf__heads_release(&s->kept[i].heads);
    s->n_kept = 0;
    return engine_failed(s);
  }
  ZfTajweedHeads all;
  const ZfTajweedTop2 *top2 = NULL;
  int rc = 0;
  if (zf_tajweed_stream_heads(s->zs, &all) < 0) rc = tf__fail(TF_ERR_ENGINE_FAILED, "the engine kept no heads");
  if (!rc && zf_tajweed_stream_top2(s->zs, &top2) < all.n40) top2 = NULL;
  for (int i = 0; i < s->n_kept; ++i) {
    if (!rc) deliver(s, &s->kept[i].heads, s->kept[i].n40, &all, top2, i == s->n_kept - 1);
    tf__heads_release(&s->kept[i].heads);
  }
  s->n_kept = 0;
  if (!rc) rc = keep_take(s, &all, top2);
  if (rc) return fail_take(s, rc);
  wrap_up(s, reason);
  return r;
}

static int closed(const TfSession *s) {
  if (s->state == CANCELLED) return tf__fail(TF_ERR_CANCELLED, NULL);
  return tf__fail(TF_ERR_SESSION_CLOSED, NULL);
}

static int end_take(TfSession *s, int reason);

static int push(TfSession *s, const float *samples, int n) {
  /* a take holds at most TF_TAKE_MAX_S of audio: what fits is taken, then the take ends by itself */
  const long long room = take_max_samples(s) - s->samples;
  const int at_limit = n >= room;
  if (at_limit) n = room > 0 ? (int)room : 0;
  /* with end_on_silence_ms the audio goes in pieces of 10 ms, so at most one chunk runs per piece and the take
   * ends at the chunk that reached the silence, whatever the app pushed in one call (the same bits either way) */
  const int piece = s->end_ms > 0 ? (s->rate / 100 > 0 ? s->rate / 100 : 1) : (n > 0 ? n : 1);
  int chunks = 0;
  for (int at = 0; at < n && s->state == OPEN && !s->silence_end; at += piece) {
    const int k = n - at < piece ? n - at : piece;
    const double t0 = tf__now_s();
    const int r = zf_tajweed_stream_push_audio(s->zs, samples + at, k);
    const double dt = tf__now_s() - t0;
    s->compute_s += dt;
    if (r < 0) return engine_failed(s);
    if (r > 0) s->compute_us += (long long)(dt * 1e6);
    chunks += r;
    s->samples += k;
    share_push(s, samples + at, k);
  }
  if (s->state == OPEN && s->silence_end) {
    if (s->failed_code) return fail_take(s, s->failed_code);
    wrap_up(s, TF_ENDED_SILENCE);
  }
  if (s->state == OPEN && at_limit) {
    const int r = end_take(s, TF_ENDED_LIMIT);
    return r < 0 ? r : chunks + r;
  }
  return chunks;
}

int tf_session_push(TfSession *s, const float *samples, int n) {
  if (!s) return tf__raise(tf__null_handle("session"));
  tf__clear_error();
  if (s->state != OPEN) return tf__raise(closed(s));
  if (s->from_clip) return tf__raise(tf__fail(TF_ERR_INVALID_ARGUMENT, "this session is fed by tf_session_listen"));
  if (n < 0 || (n > 0 && !samples)) return tf__raise(tf__fail(TF_ERR_INVALID_ARGUMENT, "no samples"));
  return tf__raise(push(s, samples, n));
}

int tf_session_finish(TfSession *s) {
  if (!s) return tf__raise(tf__null_handle("session"));
  tf__clear_error();
  if (s->state == ENDED) return 0;
  if (s->state != OPEN) return tf__raise(closed(s));
  return tf__raise(end_take(s, 0));
}

const TfHeads *tf_session_heads(TfSession *s) {
  if (!s) {
    tf__raise(tf__null_handle("session"));
    return NULL;
  }
  tf__clear_error();
  if (s->state == CANCELLED) tf__fail(TF_ERR_CANCELLED, NULL);
  else if (s->state == FAILED) tf__fail(s->failed_code == TF_ERR_ENGINE_FAILED || !s->failed_code ? TF_ERR_ENGINE_FAILED : s->failed_code, "the take ended on an error");
  else if (!s->have_take) tf__fail(TF_ERR_INVALID_ARGUMENT, "the take has not ended: call tf_session_finish first");
  else return &s->take;
  tf__raise(-1);
  return NULL;
}

int tf_session_cancel(TfSession *s) {
  tf__clear_error();
  if (!s) return tf__raise(tf__fail(TF_ERR_INVALID_ARGUMENT, "no session"));
  if (s->state != OPEN) return 0;
  s->state = CANCELLED;
  share_end(s, 0); /* a cancelled take is kept in a recording only from 2 s */
  zf_tajweed_stream_reset(s->zs);
  tf__model_clear_session(s->m, s);
  count_take(s, 1);
  return 0;
}

int tf_session_backend(const TfSession *s, TfBackend *out) {
  if (!s || !out) return tf__raise(tf__fail(TF_ERR_INVALID_ARGUMENT, "no session"));
  backend_cpu(out);
  return 0;
}

/* ayahs per surah (the 114 counts of the mushaf, Hafs numbering; 6236 in all) */
static const short kAyahs[115] = {0,   7,   286, 200, 176, 120, 165, 206, 75,  129, 109, 123, 111, 43,  52,  99,  128, 111, 110, 98,  135, 112, 78,
                                  118, 64,  77,  227, 93,  88,  69,  60,  34,  30,  73,  54,  45,  83,  182, 88,  75,  85,  54,  53,  89,  59,  37,
                                  35,  38,  29,  18,  45,  60,  49,  62,  55,  78,  96,  29,  22,  24,  13,  14,  11,  11,  18,  12,  12,  30,  52,
                                  52,  44,  28,  28,  20,  56,  40,  31,  50,  40,  46,  42,  29,  19,  36,  25,  22,  17,  19,  26,  30,  20,  15,
                                  21,  11,  8,   8,   19,  5,   8,   8,   11,  11,  8,   3,   9,   5,   4,   7,   3,   6,   3,   5,   4,   5,   6};
int tf__ayah_count(int surah) { return surah >= 1 && surah <= 114 ? kAyahs[surah] : 0; }

/* SPEC 6.3 and the telemetry vector: the report belongs to its take. Before the take ends a valid call replaces
 * the take's earlier report, and the report is counted when the take ends, if the take counts (it ended with its
 * Heads). After a take that counts has ended, the first valid call is counted at once; once a take's report is
 * counted, later valid calls return normally and count nothing. A cancelled or failed take counts no report, then
 * or later. A refused call changes nothing, an earlier report included. Counts only: never in a shared recording. */
int tf__recited_ok(int surah, int ayah_from, int ayah_to) {
  return surah >= 1 && surah <= 114 && ayah_from >= 1 && ayah_from <= ayah_to && ayah_to <= kAyahs[surah];
}
/* state: 0 the take is open, 1 it ended with its Heads, 2 it was cancelled or failed */
int tf__recited_report(TfRecited *r, TfClient *c, int state, int surah, int ayah_from, int ayah_to) {
  if (surah < 1 || surah > 114) return tf__fail(TF_ERR_INVALID_ARGUMENT, "surah %d is not 1 to 114", surah);
  if (!tf__recited_ok(surah, ayah_from, ayah_to))
    return tf__fail(TF_ERR_INVALID_ARGUMENT, "ayahs %d to %d are not inside surah %d (1 to %d)", ayah_from, ayah_to, surah, kAyahs[surah]);
  if (state == 2 || r->counted) return 0;
  r->surah = surah;
  r->ayah_from = ayah_from;
  r->ayah_to = ayah_to;
  if (state == 1) {
    if (c) tf__client_count_recited(c, surah, ayah_from, ayah_to);
    r->counted = 1;
  }
  return 0;
}
/* the take ended: its report, if it has one, is counted when the take counts */
void tf__recited_end(TfRecited *r, TfClient *c, int counts) {
  if (!counts || !r->surah || r->counted) return;
  if (c) tf__client_count_recited(c, r->surah, r->ayah_from, r->ayah_to);
  r->counted = 1;
}

int tf_session_report_recited(TfSession *s, int surah, int ayah_from, int ayah_to) {
  if (!s) return tf__raise(tf__null_handle("session"));
  tf__clear_error();
  return tf__raise(tf__recited_report(&s->recited, s->client, s->state == OPEN ? 0 : s->state == ENDED ? 1 : 2, surah, ayah_from, ayah_to));
}

void tf_session_destroy(TfSession *s) {
  if (!s) return;
  if (s->state == OPEN) tf_session_cancel(s);
  tf__model_session_gone(s->m);
  tf__decoded_free(s->so_far);
  if (s->have_take) tf__heads_release(&s->take);
  for (int i = 0; i < s->n_kept; ++i) tf__heads_release(&s->kept[i].heads);
  free(s->kept);
  free(s);
}

/* ------------------------------------------------------------------ a sample clip through the same path */

TfSession *tf_session_listen(TfModel *m, const TfClip *clip, const TfSessionOptions *options) {
  if (!m) {
    tf__raise(tf__null_handle("model"));
    return NULL;
  }
  tf__clear_error();
  if (!clip || !clip->name) {
    tf__raise(tf__fail(TF_ERR_INVALID_ARGUMENT, "no clip"));
    return NULL;
  }
  float *x = NULL;
  int rate = 0;
  const int n = tf__model_clip_samples(m, clip->name, &x, &rate);
  TfSession *s = n < 0 ? NULL : session_new(m, options, rate);
  if (!s) {
    free(x);
    tf__raise(-1);
    return NULL;
  }
  s->from_clip = 1;
  const int step = rate / 50; /* 20 ms */
  const double t0 = tf__now_s();
  for (int at = 0; at < n && s->state == OPEN; at += step) {
    if (s->pace == TF_PACE_REAL_TIME) {
      /* the clip's pace: these samples are due once the audio before them has played */
      const double due = (double)at / rate - (tf__now_s() - t0);
      if (due > 0) tf__sleep_ms((int)(due * 1000.0));
    }
    if (push(s, x + at, n - at < step ? n - at : step) < 0) break;
  }
  free(x);
  if (s->state == OPEN) end_take(s, TF_ENDED_SOURCE_END);
  if (s->state == FAILED) {
    const int code = s->failed_code;
    tf_session_destroy(s);
    tf__raise(tf__fail(code ? code : TF_ERR_ENGINE_FAILED, "the take ended on an error"));
    return NULL;
  }
  tf__clear_error();
  return s;
}
