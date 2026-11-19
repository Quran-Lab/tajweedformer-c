/* The install's own days, for the automatic analytics of SDK 4.0 (SDK_32_CONTRACT_DRAFT.md section D: 17.8 learner,
 * learner_gap, streak; 17.9 judged_tenure and the weekly trend), and the open analytics session (17.8 session_s).
 *
 * The days live in the Core's journal (Core request R10, quranlab-core cd24aa3 on; qlcore.h qlc_days_*), written
 * whole and committed with the recited counters, never sent: the install's first active UTC day, its active UTC days
 * and local days of the last weeks (40 of each), the streak at each recent active day, the days whose learner facts
 * are in a delta, six weeks of (judged, correct) by the Monday of the UTC week, the last week whose trend was sent.
 * Each change is one call into the Core (qlc_days_mark, _judged, _seal, _trend_sent), and the view the rules below
 * read is qlc_days_read's. An install without keys (no model was ever installed) has no Core and no days: nothing is
 * marked, and no learner facts go out.
 *
 * Atomicity: the days are in one write with the Core's recited counters. A delta that carries learner facts or a
 * trend is written first (report-*.json) and the Core marks them sent after; a kill in between is repaired from the
 * delta itself before anything else is sealed (tf_client.c finish_seal), so facts go out once. The day's counters
 * are the SDK's own file: a kill between the Core's write and the day file's can shift one take's marks.
 *
 * The open session (17.8 session_s: the session belongs to the SDK, not the Core) is <storage>/session.json,
 * {"audio_ms":n} while one is open, written whole and atomically.
 *
 * The rules are the Apple SDK's (Cloud/RecitedAnalytics.swift LearnerJournal, the same tests): an active day is a UTC
 * day with a take with audio; learner: active, new (the first active day), d1, d7, d30 (1, 7 or 30 UTC days after
 * it); learner_gap: UTC days since the previous active day (new, 1 to 30, gt_30); streak: consecutive active LOCAL
 * days up to today (1, 2_3, 4_6, 7_13, 14_29, ge_30); tenure: days since the first active day (w1 0 to 6, w2_4 7 to
 * 27, m2_3 28 to 89, later); trend: the last UTC week (Monday to Sunday) against the four before, both at least 20
 * judged, a move of 5 points or more better or worse, else same; not_enough otherwise; none when no judged ayah. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tf_internal.h"

long long tf__learner_test_tz_s = -999999; /* tests: the local time is UTC plus this many seconds; the default: the device's */

/* ------------------------------------------------------------------ days as yyyy-mm-dd */
static long long days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const long long yoe = y - era * 400;
  const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
static void civil_from_days(long long z, int *y, int *m, int *d) {
  z += 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const long long doe = z - era * 146097;
  const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const long long mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yoe + era * 400 + (*m <= 2));
}
/* the day number (days since 1970-01-01) of a yyyy-mm-dd, or -1000000 when it is not one */
long long tf__day_num(const char *day) {
  int y, m, d;
  if (!day || strlen(day) != 10 || sscanf(day, "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return -1000000;
  return days_from_civil(y, m, d);
}
void tf__day_str(long long n, char out[11]) {
  int y, m, d;
  civil_from_days(n, &y, &m, &d);
  snprintf(out, 11, "%04d-%02d-%02d", y, m, d);
}
/* the Monday of the UTC week of a day (1970-01-01 was a Thursday) */
void tf__day_monday(const char *day, char out[11]) {
  const long long n = tf__day_num(day);
  const long long wd = ((n % 7) + 7 + 3) % 7; /* 0 Monday ... 6 Sunday */
  tf__day_str(n - wd, out);
}
static long long now_s(void) { return tf__now_override ? tf__now_override : (long long)time(NULL); }
/* the learner's local day and hour now (the device's time zone; a test sets an offset from UTC) */
void tf__local_now(char day[11], int *hour) {
  const long long t = now_s();
  struct tm g;
  if (tf__learner_test_tz_s != -999999) {
    const time_t u = (time_t)(t + tf__learner_test_tz_s);
#ifdef _WIN32
    gmtime_s(&g, &u);
#else
    gmtime_r(&u, &g);
#endif
  } else {
    const time_t u = (time_t)t;
#ifdef _WIN32
    localtime_s(&g, &u);
#else
    localtime_r(&u, &g);
#endif
  }
  if (day) strftime(day, 11, "%Y-%m-%d", &g);
  if (hour) *hour = g.tm_hour;
}

/* ------------------------------------------------------------------ the juz of an ayah (Hafs, the 30 parts) */
static const short kJuz[30][2] = {{1, 1},   {2, 142}, {2, 253}, {3, 93},  {4, 24},  {4, 148}, {5, 83},  {6, 111}, {7, 88},  {8, 41},
                                  {9, 93},  {11, 6},  {12, 53}, {15, 1},  {17, 1},  {18, 75}, {21, 1},  {23, 1},  {25, 21}, {27, 56},
                                  {29, 46}, {33, 31}, {36, 28}, {39, 32}, {41, 47}, {46, 1},  {51, 31}, {58, 1},  {67, 1},  {78, 1}};
int tf__juz_of(int surah, int ayah) {
  int j = 1;
  for (int i = 0; i < 30; ++i)
    if (surah > kJuz[i][0] || (surah == kJuz[i][0] && ayah >= kJuz[i][1])) j = i + 1;
  return j;
}

/* ------------------------------------------------------------------ the days, from the Core's journal */
static int has(const char (*days)[11], int n, const char *day) {
  for (int i = 0; i < n; ++i)
    if (!strcmp(days[i], day)) return 1;
  return 0;
}

/* the Core that keeps the days, locked; NULL (and nothing locked) when this install has none */
static TfPm *core_of(TfClient *c) {
  TfPm *p = tf__pm_existing(c);
  if (!p) return NULL;
  tf__pm_lock(p);
  if (tf__pm_core(p)) return p;
  tf__pm_unlock(p);
  return NULL;
}

static void days_from_core(TfClient *c, TfLearner *j) {
  qlc_days d;
  TfPm *p = core_of(c);
  const int ok = p && qlc_days_read(tf__pm_core(p), &d) == QLC_OK;
  if (p) tf__pm_unlock(p);
  if (!ok) return;
  if (d.first_day >= 0) tf__day_str(d.first_day, j->first);
  for (int i = 0; i < d.n_active && i < TF_LEARNER_DAYS; ++i) tf__day_str(d.active[i], j->active[j->n_active++]);
  for (int i = 0; i < d.n_local && i < TF_LEARNER_DAYS; ++i) tf__day_str(d.local[i], j->local[j->n_local++]);
  for (int i = 0; i < d.n_sealed && i < 10; ++i) tf__day_str(d.sealed[i], j->sent[j->n_sent++]);
  for (int i = 0; i < d.n_streak && i < 10; ++i) {
    tf__day_str(d.streak_day[i], j->streak_day[j->n_streaks]);
    j->streak_n[j->n_streaks++] = d.streak[i];
  }
  for (int i = 0; i < d.n_weeks && i < 6; ++i) {
    tf__day_str(d.week[i], j->week[j->n_weeks]);
    j->week_judged[j->n_weeks] = d.judged[i];
    j->week_correct[j->n_weeks++] = d.correct[i];
  }
  if (d.trend_week >= 0) tf__day_str(d.trend_week, j->trend_week);
}

static char *spath(TfClient *c) { return tf_path_join(tf__client_dir(c), "session.json"); }

void tf__learner_load(TfClient *c, TfLearner *j) {
  memset(j, 0, sizeof *j);
  days_from_core(c, j);
  char *p = spath(c), *text = NULL;
  size_t n = 0;
  TfJson *o = p && !tf_read_file(p, &text, &n) ? tf_json_parse(text, n, NULL) : NULL;
  free(p);
  free(text);
  const TfJson *ms = tf_json_get(o, "audio_ms");
  if (ms && ms->type == TJ_NUM) {
    j->session_open = 1;
    j->session_ms = (long long)ms->num;
  }
  tf_json_free(o);
}

/* the open session: session.json while one is open, none after */
int tf__learner_save_session(TfClient *c, const TfLearner *j) {
  char *p = spath(c);
  int rc = -1;
  if (p && !j->session_open) {
    rc = remove(p) == 0 || !tf_file_exists(p) ? 0 : -1;
  } else if (p) {
    char b[64];
    const int n = snprintf(b, sizeof b, "{\"audio_ms\":%lld}\n", j->session_ms);
    rc = tf_write_file(p, b, (size_t)n);
  }
  free(p);
  return rc;
}

/* one change to the days in the Core, then the view read again */
static void days_op(TfClient *c, TfLearner *j, int op, const char *day, long long b, int x, int y) {
  TfPm *p = core_of(c);
  if (!p) return;
  qlc_core *k = tf__pm_core(p);
  const long long a = tf__day_num(day);
  if (op == 0) qlc_days_mark(k, a, b);
  else if (op == 1) qlc_days_judged(k, a, x, y);
  else if (op == 2) qlc_days_seal(k, a);
  else qlc_days_trend_sent(k, a);
  tf__pm_unlock(p);
  const int open = j->session_open;
  const long long ms = j->session_ms;
  memset(j, 0, sizeof *j);
  days_from_core(c, j);
  j->session_open = open;
  j->session_ms = ms;
}

void tf__learner_mark_active(TfClient *c, TfLearner *j, const char *utc, const char *local) {
  days_op(c, j, 0, utc, tf__day_num(local), 0, 0);
}
void tf__learner_mark_sent(TfClient *c, TfLearner *j, const char *day) { days_op(c, j, 2, day, 0, 0, 0); }
void tf__learner_judged(TfClient *c, TfLearner *j, const char *day, int judged, int correct) {
  if (judged > 0) days_op(c, j, 1, day, 0, judged, correct);
}
void tf__learner_trend_sent(TfClient *c, TfLearner *j, const char *week) { days_op(c, j, 3, week, 0, 0, 0); }

int tf__learner_facts(const TfLearner *j, const char *day, int learner[5], char gap[8], char streak[8]) {
  if (!has(j->active, j->n_active, day) || has(j->sent, j->n_sent, day) || !j->first[0]) return 0;
  const long long since = tf__day_num(day) - tf__day_num(j->first);
  learner[0] = 1;
  learner[1] = since == 0;
  learner[2] = since == 1;
  learner[3] = since == 7;
  learner[4] = since == 30;
  const char *prev = NULL;
  for (int i = 0; i < j->n_active; ++i)
    if (strcmp(j->active[i], day) < 0 && (!prev || strcmp(j->active[i], prev) > 0)) prev = j->active[i];
  if (prev) {
    const long long n = tf__day_num(day) - tf__day_num(prev);
    if (n > 30) snprintf(gap, 8, "gt_30");
    else snprintf(gap, 8, "%lld", n);
  } else {
    /* no earlier day among the kept ones: the first day itself, or one more than 30 days back */
    snprintf(gap, 8, "%s", !strcmp(j->first, day) ? "new" : "gt_30");
  }
  int s = 1;
  for (int i = 0; i < j->n_streaks; ++i)
    if (!strcmp(j->streak_day[i], day)) s = j->streak_n[i];
  snprintf(streak, 8, "%s", s >= 30 ? "ge_30" : s >= 14 ? "14_29" : s >= 7 ? "7_13" : s >= 4 ? "4_6" : s >= 2 ? "2_3" : "1");
  return 1;
}
int tf__learner_trend(const TfLearner *j, const char *day, char out[16], char last_week[11]) {
  char mon[11];
  tf__day_monday(day, mon);
  tf__day_str(tf__day_num(mon) - 7, last_week);
  if (j->trend_week[0] && strcmp(j->trend_week, last_week) >= 0) return 0;
  long long lj = 0, lc = 0, pj = 0, pc = 0;
  for (int i = 0; i < j->n_weeks; ++i) {
    const long long back = (tf__day_num(last_week) - tf__day_num(j->week[i])) / 7;
    if (back == 0) lj += j->week_judged[i], lc += j->week_correct[i];
    else if (back >= 1 && back <= 4) pj += j->week_judged[i], pc += j->week_correct[i];
  }
  if (lj + pj == 0) return 0; /* an app that judges nothing sends no trend */
  if (lj < 20 || pj < 20) snprintf(out, 16, "not_enough");
  else {
    const double move = 100.0 * ((double)lc / (double)lj - (double)pc / (double)pj);
    snprintf(out, 16, "%s", move >= 5 ? "better" : move <= -5 ? "worse" : "same");
  }
  return 1;
}

int tf__learner_tenure(const TfLearner *j, const char *day) {
  const long long n = j->first[0] ? tf__day_num(day) - tf__day_num(j->first) : 0;
  return n < 7 ? 0 : n < 28 ? 1 : n < 90 ? 2 : 3;
}
