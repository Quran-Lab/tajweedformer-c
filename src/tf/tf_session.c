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
 * Order for one chunk: speechStarted or speechEnded of its frames, then the chunk, then ended.
 *
 * SDK 4.0: the engine's stream is the Core's (qlc_stream_*). The take's top-2 is computed here from its log_probs
 * (tf_rank.c, the same ranking), since the Core gives heads and greedy ids only. The take's recited range is found
 * by the Core's detector (tf_recited.c): begun with the take, fed after each push that ran a chunk, finished with
 * the take; the app's report is the fallback for a take the detector counted nothing for. */
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
  qlc_stream *zs;
  unsigned zs_gen; /* 4.0.2: the generation of the Model's stream this Session began on (tf__model_stream_of) */
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
  int failed_memory, failed_status; /* 24.3: the engine's status when it stopped (QLC_E_NOMEM: memory) */
  char failed_status_text[12];
  uint64_t take_id; /* the take's id in the Core's recited journal; 0 when the detector is off */
  int measured;     /* the detector counted this take: the app's report is not used */
  qlc_recited_result range; /* what it counted (never given to the app): a late report's outcomes pair with it */
  long long hour_ms[24];    /* ms of speech by the learner's local hour at each chunk (17.8 hour_speech_s) */
};

qlc_stream *tf__session_engine(const TfSession *s) { return s ? s->zs : NULL; }
qlc_stream *tf__session_stream(const TfSession *s) { return s ? s->zs : NULL; }
/* the take's heads as the Core keeps them, called under tf__model_stream_lock: only while the Model's stream is still
 * the one this Session began on (4.0.2: a later Session, the Model's close or another Model's take may have destroyed
 * it) */
int tf__session_take_heads(const TfSession *s, TfRawHeads *out) {
  qlc_stream *zs = s ? tf__model_stream_of(s->m, s->zs_gen) : NULL;
  return zs && zs == s->zs && qlc_stream_heads(zs, out) == QLC_OK ? 0 : -1;
}
/* 4.0.2: the Session's Client, held while a call uses it (tf__client_unhold), NULL once its Model is not the Client's */
static TfClient *client_hold(const TfSession *s) { return tf__model_client_hold(s->m); }

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
  for (int i = 1; i < TF_VOCAB; ++i)
    if (row[i] > row[a]) a = i;
  return a;
}

/* Hands one chunk over: the speech events of its frames, then the chunk (its Heads, the decoded layer of the take
 * up to its end, whether it holds speech). Returns 1 when the chunk ended the take on silence. */
static int deliver(TfSession *s, TfHeads *chunk, int n40, const TfRawHeads *all, const TfTop2 *top2, int is_final) {
  TfRawHeads upto = *all;
  upto.n40 = s->frames40 + n40;
  upto.n20 = 2 * upto.n40;
  /* the timing head is computed for every take (the speech rule needs it); the app gets it only when selected */
  const float *timing = all->timing_log_probs;
  tf__heads_select(&upto, s->mask);
  int speech = 0, ended = 0, active_now = 0;
  for (int t = s->frames40; t < upto.n40 && upto.log_probs; ++t) {
    const int id = top2 ? top2[t].token : greedy_id(upto.log_probs + (size_t)t * TF_VOCAB);
    const int ta = timing ? greedy_id(timing + (size_t)(2 * t) * TF_VOCAB) : 0;
    const int tb = timing ? greedy_id(timing + (size_t)(2 * t + 1) * TF_VOCAB) : 0;
    const int active = tf__speech_active(id, ta, tb);
    TfSpeechEvent ev;
    if (id != TF_BLANK) s->any_token = 1;
    if (tf__speech_frame(&s->speech, active, &ev)) emit(s, ev.type, ev.at_ms, ev.silence_ms, 0, 0);
    speech |= active;
    s->active_frames += active;
    active_now += active;
  }
  if (active_now) {
    int hour = 0;
    tf__local_now(NULL, &hour);
    s->hour_ms[hour] += 40LL * active_now; /* only the bucket leaves the device, never the zone or a time */
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
static int keep_take(TfSession *s, const TfRawHeads *all, const TfTop2 *top2) {
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
static void on_engine_chunk(void *user, const TfRawHeads *h) {
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
  TfRawHeads all;
  const TfTop2 *top2 = NULL; /* computed from log_probs by the decoded layer (tf_rank.c) */
  if (qlc_stream_heads(s->zs, &all) != QLC_OK) return;
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
  if (tf__model_closed(m)) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "the model is closed");
    return NULL;
  }
  TfSessionOptions o;
  memset(&o, 0, sizeof o);
  if (opt) o = *opt;
  if (o.backend && strcmp(o.backend, "auto") && strcmp(o.backend, "cpu")) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "backend %s is not auto or cpu", o.backend);
    return NULL;
  }
  if (o.top_k < 0 || o.top_k > TF_VOCAB) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "top_k %d is not 0 to %d", o.top_k, TF_VOCAB);
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
  qlc_stream *zs = tf__model_stream(m, context, rate);
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
  s->zs_gen = tf__model_stream_gen(m);
  tf__model_session_add(m);
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
  qlc_stream_reset(zs);
  qlc_stream_set_heads(zs, mask | TF_HEAD_BIT(TF_HEAD_TIMING)); /* timing always: the speech rule */
  qlc_stream_set_callback(zs, (qlc_chunk_fn)on_engine_chunk, s);
  /* the recited journal (17.6): the take begins there (off without an index, without analytics) */
  TfClient *c = client_hold(s);
  if (c) tf__recited_begin(c, tf__model_pm(m), &s->take_id);
  if (c) tf__sharing_take_start(c); /* 4.0: an ask not yet on screen waits for this take's end */
  tf__client_unhold(c);
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
  if (n <= 0) return;
  /* 4.0.2: under the Model's link, so the Client's destroy (which ends this copy there) is never in between */
  tf__model_link_lock(s->m);
  TfClient *c = s->client ? tf__model_client(s->m) : NULL;
  if (c && !s->share) {
    s->share = tf__share_begin(c, s->rate, s->route, tf_model_version(s->m), tf_model_build(s->m));
    if (!s->share) tf__clear_error(); /* sharing off is not an error of the take */
  }
  if (c && s->share) tf__sharing_session_push(s->share, x, n);
  tf__model_link_unlock(s->m);
}
static void share_end_locked(TfSession *s, int completed) {
  if (s->share) tf__sharing_session_finish(s->share, completed);
  s->share = NULL;
}
static void share_end(TfSession *s, int completed) {
  tf__model_link_lock(s->m);
  share_end_locked(s, completed);
  tf__model_link_unlock(s->m);
}

/* the Client is being destroyed (under the Model's link): this take's shared copy ends now, as the Client goes */
void tf__session_detach(TfSession *s) {
  share_end_locked(s, 0);
  s->client = NULL;
}

static int g_takes; /* takes started in this process: the first one is the launch's cold start */

/* the take for the day's telemetry (SPEC section 6): every take is a session; one that ended with its Heads also
 * counts in takes, audio_s, speech_s, silence_s, tokens and compute_s, and counts its recited report */
static void count_take(TfSession *s, int aborted, int end) {
  TfClient *const c = client_hold(s);
  if (!c) return;
  TfSessionStats st;
  memset(&st, 0, sizeof st);
  st.end = end;
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
    memcpy(st.hour_ms, s->hour_ms, sizeof st.hour_ms);
    for (int h = 0; h < TF_HEAD_COUNT; ++h) {
      if (!(s->mask & TF_HEAD_BIT(h)) || strcmp(tf_head_info(h)->kind, "log_softmax")) continue;
      const int n = tf_heads_greedy(&s->take, tf_head_info(h)->name, NULL);
      st.tokens[h] = n > 0 ? n : 0;
    }
    tf__clear_error();
  }
  tf__client_session_finished(c, &st);
  /* the recited range (17.1): measured by the Core's detector, else the app's report; a take that did not end
   * with its Heads counts neither, and its journaled units are dropped */
  if (!aborted && s->have_take) {
    s->measured = tf__recited_finish(c, tf__model_pm(s->m), s->zs, s->take_id, s->recited.surah ? &s->recited : NULL, &s->range);
    if (s->recited.surah) s->recited.counted = 1;
    s->take_id = 0;
  } else {
    tf__recited_cancel(c, tf__model_pm(s->m), s->take_id);
    s->take_id = 0;
  }
  tf__recited_end(&s->recited, c, 0);
  tf__client_unhold(c);
}

/* 24.3 session: the cause of a take's failure (the engine's status when it stopped) */
static void take_cause(const TfSession *s) {
  const char *qn = s->failed_status == QLC_E_NOMEM ? "QLC_E_NOMEM" : s->failed_status == QLC_E_ENGINE ? "QLC_E_ENGINE" : NULL;
  tf__cause("session", s->failed_memory ? "memory" : "engine", 0, qn, s->failed_status ? s->failed_status_text : NULL);
}
/* the engine reported an error: the take is over, with nothing kept */
static int fail_take(TfSession *s, int code) {
  char text[256];
  snprintf(text, sizeof text, "%s", tf__error_text());
  s->state = FAILED;
  s->failed_code = code;
  share_end(s, 0);
  tf__model_clear_session(s->m, s);
  count_take(s, 1, 2);
  emit(s, TF_EVENT_ENDED, 0, 0, TF_ENDED_ERROR, code);
  /* the callbacks may have used the thread's error: put this one back, with its cause (24.3 session) */
  take_cause(s);
  if (code == TF_ERR_ENGINE_FAILED) tf__fail(code, "%s", "the engine stopped");
  else tf__fail(code, "%s", "the take's heads could not be kept");
  return code;
}
/* the engine refused audio or the flush (its status: QLC_E_NOMEM is memory) */
static int engine_failed(TfSession *s, int status) {
  s->failed_memory = status == QLC_E_NOMEM;
  s->failed_status = status;
  snprintf(s->failed_status_text, sizeof s->failed_status_text, "%d", status);
  return fail_take(s, tf__fail(TF_ERR_ENGINE_FAILED, "%s", "the engine stopped"));
}

/* the take ended with its Heads: the shared copy is closed, the take is counted, and when it ended by itself it
 * says why */
static void wrap_up(TfSession *s, int reason) {
  s->state = ENDED;
  share_end(s, 1);
  tf__model_clear_session(s->m, s);
  /* the app's finish, silence, the limit, the clip's end: normal; the background reasons (never raised here) not */
  count_take(s, 0, reason == TF_ENDED_BACKGROUND || reason == TF_ENDED_HIDDEN || reason == TF_ENDED_INTERRUPTED ? 1 : 0);
  if (reason) emit(s, TF_EVENT_ENDED, 0, 0, reason, 0);
}

/* The end of the take by finish (reason 0) or because its source ran out: the engine's flush and the chunks it
 * completes (the last one final), then the take's Heads. Returns the chunks run, or a negative TfError. */
static int end_take(TfSession *s, int reason) {
  s->finishing = 1;
  TfQosSave qos;
  tf__qos_enter(&qos);
  const double t0 = tf__now_s();
  const int r = qlc_stream_finish(s->zs);
  const double dt = tf__now_s() - t0;
  tf__qos_leave(&qos);
  s->compute_s += dt;
  s->compute_us += (long long)(dt * 1e6);
  if (r < 0) {
    for (int i = 0; i < s->n_kept; ++i) tf__heads_release(&s->kept[i].heads);
    s->n_kept = 0;
    return engine_failed(s, r);
  }
  TfRawHeads all;
  const TfTop2 *top2 = NULL;
  int rc = 0;
  if (qlc_stream_heads(s->zs, &all) != QLC_OK) rc = tf__fail(TF_ERR_ENGINE_FAILED, "the engine kept no heads");
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
  /* 4.0.1, Windows: the engine runs on this thread too; it is never power-throttled while it does (tf_qos.c) */
  TfQosSave qos;
  tf__qos_enter(&qos);
  for (int at = 0; at < n && s->state == OPEN && !s->silence_end; at += piece) {
    const int k = n - at < piece ? n - at : piece;
    const double t0 = tf__now_s();
    const int r = qlc_stream_push(s->zs, samples + at, k);
    const double dt = tf__now_s() - t0;
    s->compute_s += dt;
    if (r < 0) {
      tf__qos_leave(&qos);
      return engine_failed(s, r);
    }
    if (r > 0) s->compute_us += (long long)(dt * 1e6);
    chunks += r;
    s->samples += k;
    share_push(s, samples + at, k);
    if (r > 0 && s->take_id) tf__recited_feed(tf__model_pm(s->m), s->zs); /* the take's units so far, off the audio path */
  }
  tf__qos_leave(&qos);
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
  else if (s->state == FAILED) {
    /* the take's failure, reported when it happened: handed over again with its cause (24.13), not reported again */
    take_cause(s);
    tf__fail(s->failed_code == TF_ERR_ENGINE_FAILED || !s->failed_code ? TF_ERR_ENGINE_FAILED : s->failed_code, "the take ended on an error");
    tf__raise_again(-1);
    return NULL;
  } else if (!s->have_take) tf__fail(TF_ERR_INVALID_ARGUMENT, "the take has not ended: call tf_session_finish first");
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
  qlc_stream_reset(s->zs);
  tf__model_clear_session(s->m, s);
  count_take(s, 1, 3);
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
/* state: 0 the take is open, 1 it ended with its Heads, 2 it was cancelled or failed. 4.0: a report after the take
 * ended counts at once, as the app's report, only when the detector counted nothing for the take (17.1) */
int tf__recited_report(TfRecited *r, TfClient *c, int state, int surah, int ayah_from, int ayah_to) {
  if (surah < 1 || surah > 114) return tf__fail(TF_ERR_INVALID_ARGUMENT, "surah %d is not 1 to 114", surah);
  if (!tf__recited_ok(surah, ayah_from, ayah_to))
    return tf__fail(TF_ERR_INVALID_ARGUMENT, "ayahs %d to %d are not inside surah %d (1 to %d)", ayah_from, ayah_to, surah, kAyahs[surah]);
  if (state == 2 || r->counted) return 0;
  r->surah = surah;
  r->ayah_from = ayah_from;
  r->ayah_to = ayah_to;
  if (state == 1) {
    if (c) {
      TfRecitedCount k;
      memset(&k, 0, sizeof k);
      k.source = 2;
      k.surah = surah;
      k.first = ayah_from;
      k.last = ayah_to;
      k.app = r;
      tf__client_count_take_range(c, &k);
    }
    r->counted = 1;
  }
  return 0;
}
/* the take ended without counting its report (a take that did not end with its Heads): the report is spent */
void tf__recited_end(TfRecited *r, TfClient *c, int counts) {
  (void)c;
  if (!counts || !r->surah || r->counted) return;
  r->counted = 1;
}
void tf__recited_clear(TfRecited *r) {
  free(r->per);
  memset(r, 0, sizeof *r);
}

/* one outcome of 17.9, checked: correct, or incorrect with 0 (none given) or 1 to 1,000 mistakes and at most 8 rule
 * ids ^[a-z][a-z0-9_]{0,23}$; correct with mistakes or rules is refused */
static int outcome_in(const TfRecitedOutcome *o, TfOutcomeX *out) {
  memset(out, 0, sizeof *out);
  if (o->correct != 0 && o->correct != 1) return tf__fail(TF_ERR_INVALID_ARGUMENT, "outcome.correct is not 0 or 1");
  if (o->correct && (o->mistakes || o->n_rules)) return tf__fail(TF_ERR_INVALID_ARGUMENT, "a correct outcome has no mistakes and no rules");
  if (o->mistakes < 0 || o->mistakes > 1000) return tf__fail(TF_ERR_INVALID_ARGUMENT, "outcome.mistakes %d is not 1 to 1000 (0: not given)", o->mistakes);
  if (o->n_rules < 0 || o->n_rules > 8 || (o->n_rules && !o->rules)) return tf__fail(TF_ERR_INVALID_ARGUMENT, "outcome.rules: at most 8 rule ids");
  for (int i = 0; i < o->n_rules; ++i)
    if (!tf__rule_name_ok(o->rules[i]))
      return tf__fail(TF_ERR_INVALID_ARGUMENT, "rule %s is not a lower-case letter followed by at most 23 lower-case letters, digits or underscores",
                      o->rules[i] ? o->rules[i] : "(none)");
  out->set = 1;
  out->correct = o->correct;
  out->mistakes = o->mistakes;
  out->n_rules = o->n_rules;
  for (int i = 0; i < o->n_rules; ++i) snprintf(out->rules[i], sizeof out->rules[i], "%s", o->rules[i]);
  return 0;
}
int tf__recited_outcome(TfRecited *r, int surah, int ayah_from, int ayah_to, const TfRecitedOutcome *whole, const TfRecitedAyah *per, int n) {
  if (surah < 1 || surah > 114) return tf__fail(TF_ERR_INVALID_ARGUMENT, "surah %d is not 1 to 114", surah);
  if (!tf__recited_ok(surah, ayah_from, ayah_to))
    return tf__fail(TF_ERR_INVALID_ARGUMENT, "ayahs %d to %d are not inside surah %d (1 to %d)", ayah_from, ayah_to, surah, kAyahs[surah]);
  if (n < 0 || (n && !per)) return tf__fail(TF_ERR_INVALID_ARGUMENT, "per_ayah: %d entries", n);
  TfRecited x;
  memset(&x, 0, sizeof x);
  int rc = whole ? outcome_in(whole, &x.whole) : 0;
  if (!rc && n) {
    x.per = (TfPerAyah *)calloc((size_t)n, sizeof(TfPerAyah));
    if (!x.per) rc = tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  }
  for (int i = 0; !rc && i < n; ++i) {
    if (per[i].ayah < ayah_from || per[i].ayah > ayah_to) rc = tf__fail(TF_ERR_INVALID_ARGUMENT, "per_ayah: ayah %d is outside %d to %d", per[i].ayah, ayah_from, ayah_to);
    for (int k = 0; !rc && k < i; ++k)
      if (per[k].ayah == per[i].ayah) rc = tf__fail(TF_ERR_INVALID_ARGUMENT, "per_ayah: ayah %d is given twice", per[i].ayah);
    if (!rc) rc = outcome_in(&per[i].outcome, &x.per[i].o);
    if (!rc) x.per[i].ayah = per[i].ayah, x.n_per++;
  }
  if (rc) {
    free(x.per);
    return rc;
  }
  /* a valid report replaces the take's earlier one (at most one report per take) */
  free(r->per);
  r->whole = x.whole;
  r->per = x.per;
  r->n_per = x.n_per;
  return 0;
}

int tf_session_report_recited(TfSession *s, int surah, int ayah_from, int ayah_to) {
  return tf_session_report_recited_outcome(s, surah, ayah_from, ayah_to, NULL, NULL, 0);
}

/* 17.9: the same report with the app's own judgement of the range (whole) and of single ayahs (per_ayah). Nothing
 * here judges: the counts are the app's. Counted, as the report, only where the detector counted nothing; where it
 * counted the take, the outcomes are paired with the measured ayahs. */
int tf_session_report_recited_outcome(TfSession *s, int surah, int ayah_from, int ayah_to, const TfRecitedOutcome *outcome,
                                      const TfRecitedAyah *per_ayah, int n_per_ayah) {
  if (!s) return tf__raise(tf__null_handle("session"));
  tf__clear_error();
  const int state = s->state == OPEN ? 0 : s->state == ENDED ? 1 : 2;
  if (state == 2 || s->recited.counted) {
    /* checked all the same (a wrong call is an error whenever it comes), then nothing more */
    TfRecited tmp;
    memset(&tmp, 0, sizeof tmp);
    const int rc = tf__recited_outcome(&tmp, surah, ayah_from, ayah_to, outcome, per_ayah, n_per_ayah);
    free(tmp.per);
    return tf__raise(rc);
  }
  if (state == 1 && s->measured) {
    /* the detector counted this take: a report after it adds only its outcomes, paired with the measured ayahs */
    TfRecited tmp;
    memset(&tmp, 0, sizeof tmp);
    const int rc = tf__recited_outcome(&tmp, surah, ayah_from, ayah_to, outcome, per_ayah, n_per_ayah);
    if (!rc) {
      tmp.surah = surah, tmp.ayah_from = ayah_from, tmp.ayah_to = ayah_to;
      TfRecitedCount k;
      memset(&k, 0, sizeof k);
      k.source = 1;
      k.late = 1;
      k.surah = s->range.surah, k.first = s->range.first, k.last = s->range.last;
      k.ties = s->range.ties > 0 ? s->range.ties : 0; /* a tie take's outcomes: not toward first or repeat */
      k.app = &tmp;
      TfClient *c = outcome || n_per_ayah ? client_hold(s) : NULL;
      if (c) tf__client_count_take_range(c, &k);
      tf__client_unhold(c);
      s->recited.counted = 1;
    }
    free(tmp.per);
    return tf__raise(rc);
  }
  int rc = tf__recited_outcome(&s->recited, surah, ayah_from, ayah_to, outcome, per_ayah, n_per_ayah);
  TfClient *c = client_hold(s); /* 4.0.2: none once the Client is destroyed or the Model closed */
  if (!rc) rc = tf__recited_report(&s->recited, state == 1 ? NULL : c, state, surah, ayah_from, ayah_to);
  if (!rc && state == 0) tf__recited_report_pending(c, s->take_id, &s->recited);
  if (!rc && state == 1 && c) {
    /* after the end of a take the detector did not count: the app's report, with its outcomes, counts now */
    TfRecited app = s->recited;
    s->recited.counted = 0;
    tf__recited_finish(c, NULL, NULL, 0, &app, NULL);
    s->recited.counted = 1;
  }
  tf__client_unhold(c);
  return tf__raise(rc);
}

/* tests (tf40_tests lifetime): an ended Session of m with no engine stream, as a Session is after its take when the
 * stand-in Core opens no engine */
TfSession *tf__session_test_ended(TfModel *m) {
  TfSession *s = m && !tf__model_closed(m) ? (TfSession *)calloc(1, sizeof *s) : NULL;
  if (!s) return NULL;
  s->m = m;
  s->client = tf__model_client(m);
  s->zs_gen = tf__model_stream_gen(m);
  tf__model_session_add(m);
  s->state = ENDED;
  s->rate = 16000;
  snprintf(s->context, sizeof s->context, "%s", tf__model_default_context(m));
  snprintf(s->route, sizeof s->route, "unknown");
  return s;
}

void tf_session_destroy(TfSession *s) {
  if (!s) return;
  if (s->state == OPEN) tf_session_cancel(s);
  tf__recited_clear(&s->recited);
  tf__model_session_gone(s->m);
  tf__decoded_free(s->so_far);
  if (s->have_take) tf__heads_release(&s->take);
  for (int i = 0; i < s->n_kept; ++i) tf__heads_release(&s->kept[i].heads);
  free(s->kept);
  TfModel *const m = s->m;
  free(s);
  tf__model_session_freed(m); /* 4.0.2: a Model its holders closed goes with its last Session */
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
    char st[12];
    snprintf(st, sizeof st, "%s", s->failed_status_text);
    const int memory = s->failed_memory;
    tf_session_destroy(s);
    tf__cause("session", memory ? "memory" : "engine", 0, NULL, st[0] ? st : NULL); /* the take's failure (24.3) */
    tf__raise(tf__fail(code ? code : TF_ERR_ENGINE_FAILED, "the take ended on an error"));
    return NULL;
  }
  tf__clear_error();
  return s;
}
