/* The recited range of a take (SDK_32_CONTRACT_DRAFT.md 17.1, 17.5, 17.6, 17.9; the 4.0.0 analytics chapter), on
 * the Core's detector and journal (qlcore.h qlc_recited_*).
 *
 *   index    the build's prebuilt index (role recited), installed with the model, handed to the Core when a model
 *            opens (qlc_recited_index). Without it the detector is off and the app's reports apply.
 *   a take   qlc_recited_begin at its start (a random 64-bit id that never leaves the device), the stream's greedy
 *            units after every push that ran a chunk (qlc_recited_feed_stream; the Core journals them on its tick,
 *            every 10 s, so a kill loses at most that), qlc_recited_finish at its end. A cancelled or failed take
 *            begins an empty take in its place, so its units are never counted.
 *   counted  exactly one source per take (17.1 "17.1 changes"): `measured` when the detector found a range, else
 *            `app` when the app reported the take (tf_session_report_recited, or _outcome), else nothing. The
 *            measured range is never given to the app or logged; only its counts go into the day's telemetry.
 *   once     <storage>/core/recited.pending names the open take (its id, its UTC day, the app's report): written at
 *            begin and at a report, removed once the take's counts are in the day's file, which also keeps the
 *            ids it counted. A kill anywhere in between is finished at the next start, from the Core's journal
 *            (qlc_recited_open, or the stored result of a take id finished twice) into the take's own day.
 *   switches analytics off (the signed config's flags.analytics), or flags.recited false: no take is begun,
 *            nothing is journaled. flags.recited_twins false: QLC_RECITED_NO_CROSS_SURAH. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tf_internal.h"

static int g_index_ok; /* the Core took an index in this process (one Client, one Core) */

static int detector_on(TfClient *c) { return c && tf__client_telemetry_on(c) && tf__client_flag(c, "recited", 1); }

static char *pending_path(TfClient *c) {
  char *d = tf_path_join(tf__client_dir(c), "core"), *p = d ? tf_path_join(d, "recited.pending") : NULL;
  if (d) tf_mkdirs(d);
  free(d);
  return p;
}
static void day_now(char out[11]) {
  time_t t = (time_t)(tf__now_override ? tf__now_override : (long long)time(NULL));
  struct tm g;
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  strftime(out, 11, "%Y-%m-%d", &g);
}

/* ------------------------------------------------------------------ the pending record */
static void outcome_json(TfBuf *b, const TfOutcomeX *o) {
  tf_buf_printf(b, "{\"correct\":%d,\"mistakes\":%d,\"rules\":[", o->correct, o->mistakes);
  for (int i = 0; i < o->n_rules; ++i) {
    if (i) tf_buf_puts(b, ",");
    tf_buf_json_str(b, o->rules[i]);
  }
  tf_buf_puts(b, "]}");
}
static void outcome_read(const TfJson *j, TfOutcomeX *o) {
  memset(o, 0, sizeof *o);
  if (!j || j->type != TJ_OBJ) return;
  o->set = 1;
  const TfJson *c = tf_json_get(j, "correct"), *m = tf_json_get(j, "mistakes"), *r = tf_json_get(j, "rules");
  o->correct = c && c->type == TJ_NUM && c->num != 0;
  o->mistakes = m && m->type == TJ_NUM ? (int)m->num : 0;
  for (int i = 0; r && r->type == TJ_ARR && i < r->n && o->n_rules < 8; ++i)
    if (tf__rule_name_ok(tf_json_str(&r->kids[i]))) snprintf(o->rules[o->n_rules++], 25, "%s", r->kids[i].str);
}
static void pending_write(TfClient *c, uint64_t id, const char *day, int cancelled, const TfRecited *r) {
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "{\"take\":\"%016llx\",\"day\":\"%s\",\"cancelled\":%s", (unsigned long long)id, day, cancelled ? "true" : "false");
  if (r && r->surah) {
    tf_buf_printf(&b, ",\"report\":{\"surah\":%d,\"from\":%d,\"to\":%d", r->surah, r->ayah_from, r->ayah_to);
    if (r->whole.set) {
      tf_buf_puts(&b, ",\"outcome\":");
      outcome_json(&b, &r->whole);
    }
    if (r->n_per) {
      tf_buf_puts(&b, ",\"per\":[");
      for (int i = 0; i < r->n_per; ++i) {
        tf_buf_printf(&b, "%s{\"ayah\":%d,\"outcome\":", i ? "," : "", r->per[i].ayah);
        outcome_json(&b, &r->per[i].o);
        tf_buf_puts(&b, "}");
      }
      tf_buf_puts(&b, "]");
    }
    tf_buf_puts(&b, "}");
  }
  tf_buf_puts(&b, "}");
  char *p = pending_path(c);
  if (p && b.p) tf_write_file(p, b.p, b.n);
  free(p);
  tf_buf_free(&b);
}
static void pending_remove(TfClient *c) {
  char *p = pending_path(c);
  if (p) remove(p);
  free(p);
}

/* ------------------------------------------------------------------ counting one take */
/* the take's counts into the day: the measured range, else the app's report; the app's outcomes paired with the
 * ayahs counted (17.9, tf_client.c) */
static void count_take(TfClient *c, uint64_t id, const char *day, const qlc_recited_result *res, const TfRecited *app, int recovered) {
  TfRecitedCount k;
  memset(&k, 0, sizeof k);
  k.take_id = id;
  k.recovered = recovered;
  k.day = day;
  /* R5: the take's own UTC day, as the Core counted it (17.6: a take finalised after a restart counts on its day) */
  char core_day[11];
  if (res && res->day > 0) {
    tf__day_str(res->day, core_day);
    k.day = core_day;
  }
  k.app = app && app->surah ? app : NULL;
  if (res && res->found && res->surah >= 1 && res->surah <= 114 && res->first >= 1 && res->first <= res->last) {
    k.source = 1;
    k.surah = res->surah;
    k.first = res->first;
    k.last = res->last;
    k.context = res->context ? 1 : 0; /* R5: the session context decided it (recited_takes.context) */
    k.ties = res->ties > 0 ? res->ties : 0;
  } else if (k.app) {
    k.source = 2;
    k.surah = app->surah;
    k.first = app->ayah_from;
    k.last = app->ayah_to;
  }
  tf__client_count_take_range(c, &k);
}

/* ------------------------------------------------------------------ the index and a take left open */
void tf__recited_index(TfClient *c, TfPm *p, const char *build_dir, const char *manifest, size_t n) {
  if (!detector_on(c) || !p) return;
  tf__pm_lock(p);
  tf__plat_set_build(tf__pm_plat(p), build_dir);
  const int st = qlc_recited_index(tf__pm_core(p), manifest, n);
  tf__pm_unlock(p);
  if (st != QLC_OK) return; /* no index in this build, or not installed: the app's reports apply */
  g_index_ok = 1;
  /* a take an earlier run left (killed mid-take, or between the Core's journal and the day's file) */
  char *pp = pending_path(c), *text = NULL;
  size_t tn = 0;
  TfJson *j = pp && !tf_read_file(pp, &text, &tn) ? tf_json_parse(text, tn, NULL) : NULL;
  free(pp);
  free(text);
  const char *ids = tf_json_str(tf_json_get(j, "take")), *day = tf_json_str(tf_json_get(j, "day"));
  const TfJson *cx = tf_json_get(j, "cancelled");
  if (ids && strlen(ids) == 16 && day && strlen(day) == 10) {
    const uint64_t id = strtoull(ids, NULL, 16);
    TfRecited app;
    memset(&app, 0, sizeof app);
    const TfJson *rep = tf_json_get(j, "report");
    if (rep) {
      app.surah = (int)tf_json_get(rep, "surah")->num;
      app.ayah_from = (int)tf_json_get(rep, "from")->num;
      app.ayah_to = (int)tf_json_get(rep, "to")->num;
      outcome_read(tf_json_get(rep, "outcome"), &app.whole);
      const TfJson *per = tf_json_get(rep, "per");
      app.per = per && per->type == TJ_ARR && per->n > 0 ? (TfPerAyah *)calloc((size_t)per->n, sizeof(TfPerAyah)) : NULL;
      for (int i = 0; app.per && i < per->n; ++i) {
        app.per[app.n_per].ayah = (int)tf_json_get(&per->kids[i], "ayah")->num;
        outcome_read(tf_json_get(&per->kids[i], "outcome"), &app.per[app.n_per].o);
        app.n_per++;
      }
      if (!tf__recited_ok(app.surah, app.ayah_from, app.ayah_to)) app.surah = 0;
    }
    if (!tf__client_take_counted(c, id)) {
      qlc_recited_result res;
      memset(&res, 0, sizeof res);
      tf__pm_lock(p);
      const int fr = qlc_recited_finish(tf__pm_core(p), id, tf__recited_flags(c), &res);
      tf__pm_unlock(p);
      if (cx && cx->type == TJ_BOOL && cx->boolean) {
        /* a cancelled take counts nothing */
      } else if (fr == QLC_OK || (app.surah && fr != QLC_E_NOMEM)) {
        count_take(c, id, day, fr == QLC_OK ? &res : NULL, &app, 1);
      }
    }
    free(app.per);
    pending_remove(c);
  }
  tf_json_free(j);
  /* an open take of the Core's journal that the SDK never recorded (killed before the pending record) */
  uint64_t open_id = 0;
  tf__pm_lock(p);
  if (qlc_recited_open(tf__pm_core(p), &open_id) == 1) {
    qlc_recited_result res;
    if (!tf__client_take_counted(c, open_id) && qlc_recited_finish(tf__pm_core(p), open_id, tf__recited_flags(c), &res) == QLC_OK) {
      tf__pm_unlock(p);
      char today[11];
      day_now(today);
      count_take(c, open_id, today, &res, NULL, 1);
      tf__pm_lock(p);
    }
  }
  tf__pm_unlock(p);
}

unsigned tf__recited_flags(TfClient *c) { return tf__client_flag(c, "recited_twins", 1) ? 0u : QLC_RECITED_NO_CROSS_SURAH; }

/* ------------------------------------------------------------------ a take */
int tf__recited_begin(TfClient *c, TfPm *p, uint64_t *id) {
  *id = 0;
  if (!g_index_ok || !detector_on(c) || !p) return -1;
  uint8_t r[8];
  if (tf_random_bytes(r, 8)) return -1;
  for (int i = 0; i < 8; ++i) *id = *id << 8 | r[i];
  if (!*id) *id = 1;
  char day[11];
  day_now(day);
  pending_write(c, *id, day, 0, NULL);
  tf__pm_lock(p);
  const int st = qlc_recited_begin(tf__pm_core(p), *id);
  tf__pm_unlock(p);
  if (st < 0) {
    pending_remove(c);
    *id = 0;
    return -1;
  }
  return 0;
}
void tf__recited_feed(TfPm *p, const qlc_stream *s) {
  if (!p || !s) return;
  tf__pm_lock(p);
  qlc_recited_feed_stream(tf__pm_core(p), s);
  tf__pm_unlock(p);
}
/* the app's report of the open take, kept with it for a finish after a restart */
void tf__recited_report_pending(TfClient *c, uint64_t id, const TfRecited *r) {
  if (!id) return;
  char day[11];
  char *pp = pending_path(c), *text = NULL;
  size_t tn = 0;
  TfJson *j = pp && !tf_read_file(pp, &text, &tn) ? tf_json_parse(text, tn, NULL) : NULL;
  free(pp);
  free(text);
  const char *d = tf_json_str(tf_json_get(j, "day"));
  if (d && strlen(d) == 10) snprintf(day, sizeof day, "%s", d);
  else day_now(day);
  tf_json_free(j);
  pending_write(c, id, day, 0, r);
}
/* The end of a take with its Heads: the Core's answer, then the day's counts. 1 when the detector counted it (the
 * app's report of this take is then ignored), 0 when not. */
int tf__recited_finish(TfClient *c, TfPm *p, const qlc_stream *s, uint64_t id, const TfRecited *app, qlc_recited_result *out) {
  qlc_recited_result res;
  memset(&res, 0, sizeof res);
  if (out) memset(out, 0, sizeof *out);
  int st = -1;
  if (id && p) {
    tf__pm_lock(p);
    qlc_recited_feed_stream(tf__pm_core(p), s);
    st = qlc_recited_finish(tf__pm_core(p), id, tf__recited_flags(c), &res);
    tf__pm_unlock(p);
  }
  char day[11];
  day_now(day);
  if (id || (app && app->surah)) count_take(c, id, day, st == QLC_OK ? &res : NULL, app, 0);
  if (id) pending_remove(c);
  if (out && st == QLC_OK) *out = res;
  return st == QLC_OK && res.found;
}
/* a cancelled or failed take: its units are dropped (an empty take takes its place in the journal) */
void tf__recited_cancel(TfClient *c, TfPm *p, uint64_t id) {
  if (!id || !p) return;
  uint64_t blank = id ^ 0x9e3779b97f4a7c15ull;
  char day[11];
  day_now(day);
  pending_write(c, blank, day, 1, NULL);
  tf__pm_lock(p);
  qlc_recited_begin(tf__pm_core(p), blank);
  tf__pm_unlock(p);
}
