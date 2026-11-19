/* When SDK 4.0 asks a learner to share (SHARE_ASK_40.md, section 20): the contract's reference (tools/share_ask.py
 * resolve_config, local_day, normalize, decide) ported line for line, as a pure function of the stored state, this app
 * session's part, the event, the signed config's `share_ask` and the clock. tf_sharing.c keeps the state, feeds the
 * events from their hooks and acts on the action; the vectors (share_ask.json) replay every case through this file.
 *
 * The rule (20.1): the first ask is the full sheet, never within 24 hours of the install or before the 3rd completed
 * session, only at the end of a good session (the take ended normally with speech, and at least 60 s of speech in this
 * app session); after "Not now" the next ask needs both 14 days and 7 sessions since the last, the one after it 30
 * days and 10; one ask per app session and per calendar day; a re-ask is a card, whose "See how it helps" opens the
 * full sheet; three asks at most, ever; a yes, or sharing turned off, ends asking. The signed config can only make it
 * rarer (resolve_config clamps every field into bounds whose floors are the defaults). Integers only: times are unix
 * milliseconds, days whole local days. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tf_internal.h"

#define HOUR_MS 3600000LL
#define DAY_MS 86400000LL
#define MINUTE_MS 60000LL
#define HARD_MAX_ASKS 3 /* Quran Lab's permanent rule: three asks per install, ever */

static const int kDefaults[7] = {3, 24, 14, 30, 7, 10, 60}; /* first_min_sessions, first_min_hours, reask_days x2, reask_sessions x2, good_session_min_s */

/* a JSON number with an integral value (21 and 21.0), else 0 (a boolean, a string, 14.5, null, a list) */
static int integral(const TfJson *v, long long *out) {
  if (!v || v->type != TJ_NUM || !isfinite(v->num) || v->num != floor(v->num) || fabs(v->num) > 9.0e15) return 0;
  *out = (long long)v->num;
  return 1;
}
static long long clamp(long long v, long long lo, long long hi) { return v < lo ? lo : v > hi ? hi : v; }

/* 20.3 */
void tf__ask_config(const TfJson *raw, TfAskConfig *c) {
  c->first_min_sessions = kDefaults[0];
  c->first_min_hours = kDefaults[1];
  c->reask_days[0] = kDefaults[2], c->reask_days[1] = kDefaults[3];
  c->reask_sessions[0] = kDefaults[4], c->reask_sessions[1] = kDefaults[5];
  c->good_session_min_s = kDefaults[6];
  c->max_asks = HARD_MAX_ASKS;
  if (!raw || raw->type != TJ_OBJ) return;
  long long n;
  if (integral(tf_json_get(raw, "first_min_sessions"), &n)) c->first_min_sessions = (int)clamp(n, 3, 30);
  if (integral(tf_json_get(raw, "first_min_hours"), &n)) c->first_min_hours = (int)clamp(n, 24, 720);
  if (integral(tf_json_get(raw, "good_session_min_s"), &n)) c->good_session_min_s = (int)clamp(n, 60, 600);
  if (integral(tf_json_get(raw, "max_asks"), &n)) c->max_asks = (int)clamp(n, 1, HARD_MAX_ASKS);
  static const long long dlo[2] = {14, 30}, dhi[2] = {180, 365}, slo[2] = {7, 10}, shi[2] = {100, 100};
  const TfJson *d = tf_json_get(raw, "reask_days"), *s = tf_json_get(raw, "reask_sessions");
  for (int i = 0; d && d->type == TJ_ARR && d->n == 2 && i < 2; ++i)
    if (integral(&d->kids[i], &n)) c->reask_days[i] = (int)clamp(n, dlo[i], dhi[i]);
  for (int i = 0; s && s->type == TJ_ARR && s->n == 2 && i < 2; ++i)
    if (integral(&s->kids[i], &n)) c->reask_sessions[i] = (int)clamp(n, slo[i], shi[i]);
}

/* 20.2: the device's calendar day at `ms` with the UTC offset `tz_min` (floor division) */
long long tf__ask_local_day(long long ms, int tz_min) {
  const long long t = ms + (long long)tz_min * MINUTE_MS;
  return t >= 0 ? t / DAY_MS : -((-t + DAY_MS - 1) / DAY_MS);
}

/* 20.4: the stored fields as decide reads them (a 3.x state, a declined state without a count, the clock rule) */
static void normalize(TfAskState *s, long long now_ms, int tz_min) {
  if (s->consent < 0 || s->consent > 3) s->consent = TF_ASK_UNKNOWN;
  if (s->asks < 0) s->asks = 0;
  if (s->sessions < 0) s->sessions = 0;
  if (s->last_ask_session < 0) s->last_ask_session = 0;
  if (s->consent == TF_ASK_DECLINED && s->asks < 1) s->asks = 1;
  if (s->last_ask_session > s->sessions) s->last_ask_session = s->sessions;
  if (!s->has_installed_at || s->installed_at > now_ms) {
    s->installed_at = now_ms;
    s->has_installed_at = 1;
  }
  if (s->asks == 0) {
    s->has_last_ask_at = s->has_last_ask_day = 0;
  } else if (!s->has_last_ask_at || s->last_ask_at > now_ms) {
    s->last_ask_at = now_ms;
    s->last_ask_day = tf__ask_local_day(now_ms, tz_min);
    s->has_last_ask_at = s->has_last_ask_day = 1;
  } else if (!s->has_last_ask_day) {
    s->last_ask_day = tf__ask_local_day(s->last_ask_at, tz_min);
    s->has_last_ask_day = 1;
  }
  if (!s->has_last_ask_at) s->last_ask_at = 0;
  if (!s->has_last_ask_day) s->last_ask_day = 0;
}

static const char *blocked(const TfAskState *s, const TfAskConfig *cfg, long long now_ms, int tz_min, int moment) {
  if (s->consent == TF_ASK_GRANTED) return "granted";
  if (s->consent == TF_ASK_FINAL) return "final";
  if (s->asks >= cfg->max_asks) return "cap";
  if (s->app.ask.kind) return "ask_open";
  if (s->app.asked) return "app_session";
  if (s->has_last_ask_day && tf__ask_local_day(now_ms, tz_min) <= s->last_ask_day) return "same_day";
  if (moment && s->app.recited_ms < (long long)cfg->good_session_min_s * 1000) return "short";
  if (s->asks == 0) {
    if (now_ms - s->installed_at < (long long)cfg->first_min_hours * HOUR_MS) return "first_hours";
    if (s->sessions < cfg->first_min_sessions) return "first_sessions";
  } else {
    const int k = (int)(s->asks - 1 < 1 ? s->asks - 1 : 1);
    if (now_ms - s->last_ask_at < (long long)cfg->reask_days[k] * DAY_MS) return "reask_days";
    if (s->sessions - s->last_ask_session < cfg->reask_sessions[k]) return "reask_sessions";
  }
  return NULL;
}

static int pend(TfAskState *s) {
  const int kind = s->asks == 0 ? TF_ASK_SHEET : TF_ASK_CARD;
  s->app.ask.kind = kind;
  s->app.ask.number = (int)s->asks + 1;
  s->app.ask.shown = 0;
  return kind;
}
/* the ask in hand goes on screen now: counted (one of the three) unless it is stale */
static const char *show(TfAskState *s, const TfAskConfig *cfg, long long now_ms, int tz_min) {
  TfAskHand *a = &s->app.ask;
  if ((s->consent != TF_ASK_UNKNOWN && s->consent != TF_ASK_DECLINED) || s->asks + 1 != a->number || s->asks >= cfg->max_asks) {
    memset(a, 0, sizeof *a);
    return "stale";
  }
  s->asks++;
  s->last_ask_at = now_ms;
  s->last_ask_day = tf__ask_local_day(now_ms, tz_min);
  s->has_last_ask_at = s->has_last_ask_day = 1;
  s->last_ask_session = s->sessions;
  s->app.asked = 1;
  a->shown = 1;
  return NULL;
}
/* "Not now" on the sheet or the card, the sheet closed without an answer, a card dismissed or gone (20.6) */
static void not_now(TfAskState *s) {
  if (s->consent == TF_ASK_UNKNOWN || s->consent == TF_ASK_DECLINED) s->consent = s->asks >= HARD_MAX_ASKS ? TF_ASK_FINAL : TF_ASK_DECLINED;
  memset(&s->app.ask, 0, sizeof s->app.ask);
}

/* 20.5: the decision. `s` is the state in and out (the stored fields as read, raw or not, and this app session's
 * part); `cfg_raw` the signed config's share_ask as it came (NULL when absent). Returns the action (TF_ASK_NONE,
 * TF_ASK_SHEET, TF_ASK_CARD); *why the first rule that held an ask back, or why an event was ignored (NULL else). */
int tf__ask_decide(TfAskState *s, const TfAskEvent *ev, const TfJson *cfg_raw, long long now_ms, int tz_min, const char **why) {
  TfAskConfig cfg;
  tf__ask_config(cfg_raw, &cfg);
  normalize(s, now_ms, tz_min);
  TfAskApp *app = &s->app;
  int action = TF_ASK_NONE;
  const char *w = NULL;
  switch (ev->type) {
  case TF_ASK_EV_LAUNCH: memset(app, 0, sizeof *app); break;
  case TF_ASK_EV_TAKE_START:
    app->take_open = 1;
    if (app->ask.kind && !app->ask.shown) memset(&app->ask, 0, sizeof app->ask); /* not yet on screen: nothing counted */
    else if (app->ask.kind == TF_ASK_CARD) not_now(s);                           /* a card on screen goes: Not now */
    break;
  case TF_ASK_EV_TAKE_END: {
    app->take_open = 0;
    const long long speech = ev->speech_ms;
    const int completed = ev->end == TF_ASK_END_NORMAL && speech > 0;
    if (completed) {
      s->sessions++;
      app->takes++;
      app->recited_ms += speech;
    }
    if (!completed) w = "not_completed";
    else if (!ev->offered) w = "not_offered";
    else if (!(w = blocked(s, &cfg, now_ms, tz_min, 1))) action = pend(s);
    break;
  }
  case TF_ASK_EV_ASK_NOW:
    if (!ev->offered) w = "not_offered";
    else if (app->take_open) w = "take_open";
    else if (app->takes == 0) w = "no_take";
    else if (!(w = blocked(s, &cfg, now_ms, tz_min, 0))) action = pend(s);
    break;
  case TF_ASK_EV_SHOWN:
    if (!app->ask.kind) w = "no_ask";
    else if (app->ask.shown) w = "already_shown";
    else {
      const int kind = app->ask.kind;
      if (!(w = show(s, &cfg, now_ms, tz_min))) action = kind;
    }
    break;
  case TF_ASK_EV_ANSWER: {
    TfAskHand *a = &app->ask;
    if (!a->kind) w = "no_ask";
    else if (s->consent != TF_ASK_UNKNOWN && s->consent != TF_ASK_DECLINED) {
      memset(a, 0, sizeof *a);
      w = "answered";
    } else if (ev->answer == TF_ASK_YES && a->kind != TF_ASK_SHEET) w = "card_has_no_yes";
    else if (ev->answer == TF_ASK_OPEN && a->kind != TF_ASK_CARD) w = "not_a_card";
    else if (ev->answer != TF_ASK_YES && ev->answer != TF_ASK_NOT_NOW && ev->answer != TF_ASK_OPEN) w = "unknown_answer";
    else {
      if (!a->shown) w = show(s, &cfg, now_ms, tz_min); /* an answer means the ask was on screen */
      if (!w) {
        if (ev->answer == TF_ASK_YES) {
          s->consent = TF_ASK_GRANTED;
          memset(a, 0, sizeof *a);
        } else if (ev->answer == TF_ASK_NOT_NOW) {
          not_now(s);
        } else {
          a->kind = TF_ASK_SHEET; /* the full sheet for the same ask, not counted again */
          a->shown = 1;
          action = TF_ASK_SHEET;
        }
      }
    }
    break;
  }
  case TF_ASK_EV_WITHDRAWN:
    if (!app->ask.kind) w = "no_ask";
    else if (!app->ask.shown) memset(&app->ask, 0, sizeof app->ask);
    else if (app->ask.kind == TF_ASK_CARD) not_now(s);
    else memset(&app->ask, 0, sizeof app->ask); /* a sheet taken back after it was shown: used, no answer */
    break;
  case TF_ASK_EV_SETTING:
    s->consent = ev->on ? TF_ASK_GRANTED : TF_ASK_FINAL;
    memset(&app->ask, 0, sizeof app->ask);
    break;
  default: w = "unknown_event";
  }
  if (why) *why = w;
  return action;
}

/* ------------------------------------------------------------------ the state as JSON (tests, and sharing.json's names) */
static const char *const kConsent[4] = {"unknown", "granted", "declined", "final"};
const char *tf__ask_consent_name(int c) { return c >= 0 && c <= 3 ? kConsent[c] : "unknown"; }
int tf__ask_consent_of(const char *name) {
  for (int i = 0; name && i < 4; ++i)
    if (!strcmp(name, kConsent[i])) return i;
  return TF_ASK_UNKNOWN;
}
/* one stored field, as the reference reads it: an integral number, else absent (counts: 0 when not one above 0) */
void tf__ask_read_field(TfAskState *s, const char *key, const TfJson *v) {
  long long n = 0;
  const int ok = integral(v, &n);
  if (!strcmp(key, "consent")) s->consent = tf__ask_consent_of(tf_json_str(v));
  else if (!strcmp(key, "asks")) s->asks = ok && n > 0 ? n : 0;
  else if (!strcmp(key, "sessions")) s->sessions = ok && n > 0 ? n : 0;
  else if (!strcmp(key, "last_ask_session") || !strcmp(key, "lastAskSession")) s->last_ask_session = ok && n > 0 ? n : 0;
  else if (!strcmp(key, "installed_at") || !strcmp(key, "installedAt")) s->installed_at = n, s->has_installed_at = ok;
  else if (!strcmp(key, "last_ask_at") || !strcmp(key, "lastAskAt")) s->last_ask_at = n, s->has_last_ask_at = ok;
  else if (!strcmp(key, "last_ask_day") || !strcmp(key, "lastAskDay")) s->last_ask_day = n, s->has_last_ask_day = ok;
}
/* the whole state in the reference's names (share_ask.json's `state`) */
void tf__ask_state_json(const TfAskState *s, TfBuf *b) {
  tf_buf_printf(b, "{\"consent\":\"%s\",\"asks\":%lld,\"sessions\":%lld,\"last_ask_session\":%lld,\"installed_at\":", tf__ask_consent_name(s->consent),
                s->asks, s->sessions, s->last_ask_session);
  if (s->has_installed_at) tf_buf_printf(b, "%lld", s->installed_at);
  else tf_buf_puts(b, "null");
  tf_buf_puts(b, ",\"last_ask_at\":");
  if (s->has_last_ask_at) tf_buf_printf(b, "%lld", s->last_ask_at);
  else tf_buf_puts(b, "null");
  tf_buf_puts(b, ",\"last_ask_day\":");
  if (s->has_last_ask_day) tf_buf_printf(b, "%lld", s->last_ask_day);
  else tf_buf_puts(b, "null");
  tf_buf_printf(b, ",\"app\":{\"takes\":%lld,\"recited_ms\":%lld,\"asked\":%s,\"take_open\":%s,\"ask\":", s->app.takes, s->app.recited_ms,
                s->app.asked ? "true" : "false", s->app.take_open ? "true" : "false");
  if (s->app.ask.kind)
    tf_buf_printf(b, "{\"kind\":\"%s\",\"number\":%d,\"shown\":%s}", s->app.ask.kind == TF_ASK_SHEET ? "sheet" : "card", s->app.ask.number,
                  s->app.ask.shown ? "true" : "false");
  else tf_buf_puts(b, "null");
  tf_buf_puts(b, "}}");
}
/* an event as share_ask.json writes it */
void tf__ask_event_of(const TfJson *e, TfAskEvent *ev) {
  memset(ev, 0, sizeof *ev);
  const char *t = tf_json_str(tf_json_get(e, "type"));
  static const char *const types[] = {"launch", "take_start", "take_end", "ask_now", "shown", "answer", "withdrawn", "setting"};
  ev->type = TF_ASK_EV_UNKNOWN;
  for (int i = 0; t && i < 8; ++i)
    if (!strcmp(t, types[i])) ev->type = TF_ASK_EV_LAUNCH + i;
  const char *end = tf_json_str(tf_json_get(e, "end"));
  ev->end = end && !strcmp(end, "normal") ? TF_ASK_END_NORMAL : TF_ASK_END_OTHER;
  long long n = 0;
  ev->speech_ms = integral(tf_json_get(e, "speech_ms"), &n) ? n : 0;
  const TfJson *o = tf_json_get(e, "offered"), *on = tf_json_get(e, "on");
  ev->offered = o && o->type == TJ_BOOL && o->boolean;
  ev->on = on && on->type == TJ_BOOL && on->boolean;
  const char *a = tf_json_str(tf_json_get(e, "answer"));
  ev->answer = !a ? TF_ASK_ANSWER_OTHER : !strcmp(a, "yes") ? TF_ASK_YES : !strcmp(a, "not_now") ? TF_ASK_NOT_NOW : !strcmp(a, "open") ? TF_ASK_OPEN : TF_ASK_ANSWER_OTHER;
}
