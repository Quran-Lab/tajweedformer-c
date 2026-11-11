/* Whole-session recitation sharing for desktop (PLATFORM_CLIENT.md sections 8 to 10, the 2.0.0 design): every session
 * the SDK listens to while the learner's consent is on is encoded as one Ogg Opus stream, cut into segments of
 * 10, 20, 40, then 60 s at page boundaries, and uploaded in the background with presigned PUTs straight to R2, the
 * same wire as the Swift reference (Cloud/Contributions.swift, Cloud/Outbox.swift) and the web SDK (web/sharing).
 *
 * On disk, one folder per session under <storage>/outbox/<id>/:
 *   session.json   the POST /v1/uploads body (closed schema, sorted keys, contributor_key and recovery_hash)
 *   capture.json   written by the capture: segments closed, ended, completed, duration, created (unix s), pre_skip
 *   item.json      written by the uploader: started, URLs with expiry, segments uploaded, attempts, next attempt;
 *                  "done" once every segment went up and only the take list is still to send
 *   seg-NNNNN.opus the segment files (each deleted once its PUT succeeded)
 *   seg-NNNNN.json the segment's marker: {"n", "bitrate"} written when the segment opens, plus "granule" (the
 *                  granule position of its last page) when it closes; kept until the session is removed
 *   takes.json     the takes of a recording of two or more takes (2.0.3, below): the take list call's body
 * A crash or restart leaves the folders; the next launch resumes them (a session whose capture died is closed at
 * its last whole page, with the bitrate its marker recorded when it opened). Sessions older than 7 days are
 * deleted unsent. Turning sharing off deletes them at once and aborts the PUT in flight.
 *
 * Takes in a row share one recording (SDK 2.0.3, the shared vectors' takes_join.json). A take is one
 * tf__sharing_session_begin .. finish (one take of a TfStream); the RECORDING (the capture: its Ogg stream, outbox
 * folder and session id) is decoupled from it:
 * - When a take ends while sharing is on, its recording stays open for the JOIN WINDOW: the signed config's
 *   flags.share_join_ms (a whole number 0 to 10000; absent or anything else 3000; 0 never joins).
 * - A take that starts within the window (wall ms since the previous take with audio ended <= the window) with the
 *   same input rate, contributor, consent version and model revision continues the same stream: no new headers,
 *   the resampler and the encoder keep their state (a partial Opus frame is completed by the next take's first
 *   samples), granules and page sequence continue, segments are cut by the same plan counted across takes, the
 *   bitrate ladder applies as before. The gap gets no audio.
 * - Otherwise the recording closes as a 2.0.2 session ends (its last page with EOS, capture.json ended) and the
 *   take starts a new recording. Closed at once as well: the window expiring, tf__client_background, sharing going
 *   off (its files deleted with the queue), tf__sharing_recording_close, 256 takes.
 * - A recording never holds more than 20 minutes of audio (1200 x input rate samples; SDK 3.0.0). Where a take
 *   reaches that, the recording closes at once as a whole recording (completed), and the take's next sample
 *   opens a new one: the next take_seq, gap_ms_prev 0, no pre-roll, a take list that starts {0, 0}. In 2.0.3 a
 *   take that had joined with more than 30 s of room ran past the cap and stayed whole. That left recordings
 *   of unbounded length (one long take, or an app that never ends a take), which the server's segment cap and
 *   the people who cut the audio could not rely on, so the cap now holds for every recording; the two parts
 *   are stitched by take_seq and gap_ms_prev 0, and no audio of the take is lost. A take that starts with 30 s
 *   or less of room is still held in memory and joins only when it fits (in samples), so short takes at the
 *   end of a recording are never cut.
 * - `completed` is the last take's that had audio; the 2 s minimum of a cancelled session applies to the
 *   recording's total audio; the session JSON is written once, at the first take.
 * - The takes: [{"start_ms","gap_ms"}], start_ms the stream position of the take's first sample (floor(1000 x
 *   input samples before it / input rate)), gap_ms the wall gap rounded to the nearest 100 (0 to 10000), the first
 *   {0, 0}. Kept in takes.json from the second take on (rewritten atomically as a take joins, so recovery keeps
 *   it) and sent once the recording is closed and all its segments are uploaded: POST /v1/uploads/{id}/takes. A
 *   recording of one take has no list. The call never touches the session's uploaded state or the ladder.
 * Tried and rejected: silence in the gap (made-up audio in training data); holding the upload until the recording
 * closes so the session JSON could list the takes (a long recording would wait to upload and its backlog would
 * push the ladder down); flushing a page when a take ends (the stream would no longer be the bytes of one
 * continuous encode); dropping the rest of a take at the cap (audio the learner agreed to share would be lost).
 *
 * The feed (SDK 3.0.0, the shared vectors' sharing_30.json): tf_sharing_feed takes audio the app captures outside
 * takes. The last second of it waits in a ring in memory and goes in front of the next take (the pre-roll); the
 * first half second after a take goes to its recording (the tail), for which the recording stays open up to
 * 1500 ms after the take's end, also with a join window of 0. Nothing else is ever added to a recording after
 * its take ended. Each recording carries take_seq and gap_ms_prev in its session JSON.
 *
 * Bitrate (SDK 2.0.1, the shared vectors' bitrate_ladder.json): one ladder per client, in memory, rungs 192000,
 * 96000, 64000, 32000 bit/s, starting at the top. A network failure of an upload request (no connection, a
 * timeout, HTTP 408 or 5xx on POST /v1/uploads, POST /v1/uploads/{id}/urls or a segment PUT) steps down; a closed
 * segment while 120 s or more of closed audio waits steps down; three segment PUTs in a row answered 2xx while at
 * most 30 s waits step up. The capture takes the ladder's rung when a segment opens (OPUS_SET_BITRATE before the
 * segment's first packet), so each segment has one bitrate, recorded in its marker. Tried and rejected: changing
 * mid-segment (no single bitrate to record), persisting the rung across launches (a stale bad network would cost
 * quality on a good one), rungs below 32 kbit/s (recitation detail lost for little size saved).
 *
 * Trust: desktop uploads at the `capped` tier (unattested token; per-key and per-contributor
 * caps on the server; sessions marked attested=false there). flags.upload_trust[windows|linux]: absent or "capped"
 * = upload, "off" or "attested" = no upload. The uploader runs on its own thread, one PUT at a time (the spec's
 * "at most 2 in parallel" is met). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "tf_internal.h"
#include "tf_ogg.h"
#include "tf_share.h"
#include "tf_thread.h"

/* tf_client.c / tf_sharing.c / tf_devicekey.c */
const char *tf__client_dir(const TfClient *c);
const char *tf__client_platform(const TfClient *c);
void **tf__client_share_slot(TfClient *c);
int tf__client_call(TfClient *c, const char *method, const char *path, const char *body, int auth, const char *extra,
                    TfHttpResponse *r);
int tf__client_os_major(void);
int tf__sharing_may_upload(const TfClient *c);
void tf__sharing_uploaded(TfClient *c);
void tf__sharing_grant(TfClient *c, char version[40], char hash[65]);
int tf__contributor(const char *dir, int create, char id[33], char contributor_key[90], char recovery_hash[65]);
void tf__contributor_forget(const char *dir);
int tf__client_flag_int(const TfClient *c, const char *name, int lo, int hi, int dflt);
void tf__client_token_forget(TfClient *c);
void tf__client_log(TfClient *c, int level, const char *message);
extern long long tf__now_override;
extern int tf__client_no_timer;
void tf__share_window_check(TfClient *c);

#define MAX_AGE_S (7LL * 86400)
#define URL_MARGIN_S 300
#define MIN_PARTIAL_S 2.0
/* back-to-back takes (2.0.3; the same numbers in every SDK) */
#define JOIN_MS_DEFAULT TF_SHARING_JOIN_MS_DEFAULT /* 3000: the join window when the signed config sets none */
#define JOIN_MS_MAX TF_SHARING_JOIN_MS_MAX         /* 10000: the longest window, and the largest gap recorded */
#define REC_CAP_MS (TF_SHARING_RECORDING_MAX_S * 1000LL) /* 20 minutes: such a recording takes no further take */
#define MAX_TAKES 256                  /* takes per recording (the take list call takes no more) */
#define HOLD_MS TF_SHARING_HOLD_MS     /* with this little room under the cap a joining take's audio is held */
#define PRE_SKIP_DEFAULT 312           /* libopus's lookahead at 48 kHz: a capture.json written before 2.0.3 */
#ifdef _WIN32
#define SLEEP_SHORT() Sleep(20)
#else
#define SLEEP_SHORT() usleep(20000)
#endif

static long long now_s(void) { return tf__now_override ? tf__now_override : (long long)time(NULL); }

/* The wall clock in ms: what the join window is measured with (a take's start minus the previous take's end). The
 * wall clock, not a monotonic one: a computer that slept between two takes must not join them. Tests set
 * tf__share_now_ms (0: the system's clock). */
long long tf__share_now_ms;
static long long now_ms(void) {
  if (tf__share_now_ms) return tf__share_now_ms;
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  const unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 ns since 1601 */
  return (long long)(t / 10000ULL) - 11644473600000LL;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* ------------------------------------------------------------------ per-client state */
typedef struct Rec Rec;
typedef struct {
  TfClient *c;
  TfMutex m;
  TfCond cv;
  TfThread th;
  int thread_on, quit, wake;
  volatile int cancel;
  int rung, streak;    /* the bitrate ladder (under m) */
  char active[16][33]; /* sessions being captured in this process */
  int n_active;
  int passes;          /* finished upload passes (tests) */
  int in_pass;         /* the uploader is inside a pass: it may hold a segment file open and will write the
                          session's state when it leaves (under m) */
  char outbox[1024];
  /* 2.0.3, the recorder. rm guards it: takes begin, push and end on the app's audio thread, the window ends on the
   * timer's, the app's life and the settings switch arrive on others. Lock order: rm, then m or tm; never back. */
  TfMutex rm;
  Rec *open;           /* the recording takes join: one with a take running, or waiting in its join window */
  /* the window's timer: it only prompts tf__share_window_check, where the clock decides */
  TfMutex tm;
  TfCond tcv;
  TfThread tth;
  int t_on, t_quit, t_armed, t_fire;
  double t_due;        /* on the monotonic clock, ms */
  int takes_404;       /* the missing take list route was logged (once per client) */
  /* 3.0, the feed (under rm): audio the app captures outside takes */
  float *ring;         /* the last TF_SHARING_RING_MS of it while no take runs; NULL when empty */
  int ring_cap, ring_n, ring_at, ring_rate; /* its room (one second at ring_rate), samples held, write position */
  long long ring_wall; /* the wall ms of the last feed call that reached the ring */
  long long last_audio;   /* the wall ms the last audio ended: a feed call, or a take; 0: none yet */
  long long st_tail, st_ring, st_dropped, st_pre; /* the last feed call and take start (tests) */
} Share;

static Share *sh(TfClient *c) { return c ? (Share *)*tf__client_share_slot(c) : NULL; }

static void share_thread(void *arg);
static void recorder_free(Share *s);

void tf__share_init(TfClient *c) {
  Share *s = (Share *)calloc(1, sizeof *s);
  if (!s) return;
  s->c = c;
  tf_mutex_init(&s->m);
  tf_cond_init(&s->cv);
  tf_mutex_init(&s->rm);
  tf_mutex_init(&s->tm);
  tf_cond_init(&s->tcv);
  char *p = tf_path_join(tf__client_dir(c), "outbox");
  if (p) snprintf(s->outbox, sizeof s->outbox, "%s", p);
  free(p);
  *tf__client_share_slot(c) = s;
}
void tf__share_free(TfClient *c) {
  Share *s = sh(c);
  if (!s) return;
  /* the window's timer first, then the recording it would have closed: it ends here, as when its window ends */
  tf_mutex_lock(&s->tm);
  s->t_quit = 1;
  tf_cond_signal(&s->tcv);
  tf_mutex_unlock(&s->tm);
  if (s->t_on) tf_thread_join(s->tth);
  recorder_free(s);
  if (s->thread_on) {
    tf_mutex_lock(&s->m);
    s->quit = 1;
    tf_cond_signal(&s->cv);
    tf_mutex_unlock(&s->m);
    tf__http_cancel(&s->cancel);
    tf_thread_join(s->th);
  }
  tf_cond_destroy(&s->tcv);
  tf_mutex_destroy(&s->tm);
  tf_mutex_destroy(&s->rm);
  tf_cond_destroy(&s->cv);
  tf_mutex_destroy(&s->m);
  free(s);
  *tf__client_share_slot(c) = NULL;
}
static int is_active(Share *s, const char *id) {
  for (int i = 0; i < s->n_active; ++i)
    if (!strcmp(s->active[i], id)) return 1;
  return 0;
}
static void set_active(Share *s, const char *id, int on) {
  tf_mutex_lock(&s->m);
  if (on && s->n_active < 16 && !is_active(s, id)) snprintf(s->active[s->n_active++], 33, "%s", id);
  if (!on)
    for (int i = 0; i < s->n_active; ++i)
      if (!strcmp(s->active[i], id)) {
        memmove(s->active[i], s->active[i + 1], sizeof s->active[0] * (size_t)(s->n_active - i - 1));
        s->n_active--;
        break;
      }
  tf_mutex_unlock(&s->m);
}
static void wake(Share *s) {
  tf_mutex_lock(&s->m);
  s->wake = 1;
  tf_cond_signal(&s->cv);
  tf_mutex_unlock(&s->m);
}

/* ------------------------------------------------------------------ files */
static char *sess_path(const Share *s, const char *id, const char *name) {
  char *d = tf_path_join(s->outbox, id);
  char *p = d && name ? tf_path_join(d, name) : NULL;
  if (!name) return d;
  free(d);
  return p;
}
static TfJson *read_json(const Share *s, const char *id, const char *name) {
  char *p = sess_path(s, id, name), *t = NULL;
  size_t n;
  TfJson *j = p && !tf_read_file(p, &t, &n) ? tf_json_parse(t, n, NULL) : NULL;
  free(p);
  free(t);
  return j;
}
static int write_text(const Share *s, const char *id, const char *name, const char *text) {
  char *p = sess_path(s, id, name);
  const int rc = p ? tf_write_file(p, text, strlen(text)) : -1;
  free(p);
  return rc;
}
static int list_dir(const char *dir, char (*names)[64], int max) {
  int n = 0;
#ifdef _WIN32
  char pat[1100];
  snprintf(pat, sizeof pat, "%s\\*", dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..") && strlen(fd.cFileName) < 64 && n < max)
      snprintf(names[n++], 64, "%s", fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR *d = opendir(dir);
  if (!d) return 0;
  struct dirent *e;
  while ((e = readdir(d)))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strlen(e->d_name) < 64 && n < max) snprintf(names[n++], 64, "%s", e->d_name);
  closedir(d);
#endif
  return n;
}
static void remove_session(const Share *s, const char *id) {
  char *d = sess_path(s, id, NULL);
  if (!d) return;
  char names[256][64];
  /* in rounds: a long session has more files (segments and their markers) than one listing holds */
  for (int round = 0; round < 64; ++round) {
    const int n = list_dir(d, names, 256);
    if (!n) break;
    int removed = 0;
    for (int i = 0; i < n; ++i) {
      char *f = tf_path_join(d, names[i]);
      if (f) removed += remove(f) == 0;
      free(f);
    }
    if (!removed) break;
  }
#ifdef _WIN32
  RemoveDirectoryA(d);
#else
  rmdir(d);
#endif
  free(d);
}
static int is_id(const char *x) {
  if (strlen(x) != 32) return 0;
  for (const char *p = x; *p; ++p)
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0;
  return 1;
}

/* ------------------------------------------------------------------ the bitrate ladder (make_bitrate_ladder.py) */
static const int kRungs[4] = {192000, 96000, 64000, 32000};
#define UP_STREAK 3
#define UP_MAX_BACKLOG_S 30.0
#define DOWN_BACKLOG_S 120.0

static int ladder_bitrate(Share *s) {
  tf_mutex_lock(&s->m);
  const int b = kRungs[s->rung];
  tf_mutex_unlock(&s->m);
  return b;
}
static void ladder_failed(Share *s) {
  tf_mutex_lock(&s->m);
  if (s->rung < 3) s->rung++;
  s->streak = 0;
  tf_mutex_unlock(&s->m);
}
static void ladder_ok(Share *s, double backlog_s) {
  tf_mutex_lock(&s->m);
  if (s->streak < UP_STREAK) s->streak++;
  if (s->streak >= UP_STREAK && backlog_s <= UP_MAX_BACKLOG_S && s->rung > 0) {
    s->rung--;
    s->streak = 0;
  }
  tf_mutex_unlock(&s->m);
}
static void ladder_closed(Share *s, double backlog_s) {
  tf_mutex_lock(&s->m);
  if (backlog_s >= DOWN_BACKLOG_S) {
    if (s->rung < 3) s->rung++;
    s->streak = 0;
  }
  tf_mutex_unlock(&s->m);
}

/* ------------------------------------------------------------------ segment markers and the backlog */
static void marker_write(const Share *s, const char *id, int n, int bitrate, long long granule) {
  char name[32], t[128];
  snprintf(name, sizeof name, "seg-%05d.json", n);
  if (granule >= 0) snprintf(t, sizeof t, "{\"n\":%d,\"bitrate\":%d,\"granule\":%lld}", n, bitrate, granule);
  else snprintf(t, sizeof t, "{\"n\":%d,\"bitrate\":%d}", n, bitrate);
  write_text(s, id, name, t);
}
/* a marker's bitrate (0 when unknown) and granule (-1 when the segment has not closed) */
static int marker_read(const Share *s, const char *id, int n, int *bitrate, long long *granule) {
  char name[32];
  snprintf(name, sizeof name, "seg-%05d.json", n);
  TfJson *j = read_json(s, id, name);
  const TfJson *b = tf_json_get(j, "bitrate"), *g = tf_json_get(j, "granule");
  if (bitrate) *bitrate = b && b->type == TJ_NUM ? (int)b->num : 0;
  if (granule) *granule = g && g->type == TJ_NUM ? (long long)g->num : -1;
  const int ok = j != NULL;
  tf_json_free(j);
  return ok ? 0 : -1;
}
/* seconds of segment n: (granule_n - granule_{n-1}) / 48000, segment 1 from 0; the plan's length when a marker
 * cannot be read */
static double segment_seconds(const Share *s, const char *id, int n) {
  long long g1 = -1, g0 = 0;
  marker_read(s, id, n, NULL, &g1);
  if (n > 1) marker_read(s, id, n - 1, NULL, &g0);
  if (g1 >= 0 && g0 >= 0 && g1 >= g0) return (double)(g1 - g0) / 48000.0;
  return (double)(tf_ogg_segment_start(n + 1) - tf_ogg_segment_start(n)) * 960.0 / 48000.0;
}
static int uploaded_list_has(const TfJson *item, int n) {
  const TfJson *up = tf_json_get(item, "uploaded");
  for (int i = 0; up && up->type == TJ_ARR && i < up->n; ++i)
    if (up->kids[i].type == TJ_NUM && (int)up->kids[i].num == n) return 1;
  return 0;
}
/* seconds of audio in closed segments not uploaded yet, over every session in the outbox */
static double backlog_s(const Share *s) {
  char names[256][64];
  const int ns = list_dir(s->outbox, names, 256);
  double sum = 0;
  for (int i = 0; i < ns; ++i) {
    if (!is_id(names[i])) continue;
    TfJson *cap = read_json(s, names[i], "capture.json"), *item = read_json(s, names[i], "item.json");
    const TfJson *cl = tf_json_get(cap, "closed");
    const int closed = cl && cl->type == TJ_NUM ? (int)cl->num : 0;
    for (int n = 1; n <= closed; ++n) {
      char name[32];
      snprintf(name, sizeof name, "seg-%05d.opus", n);
      char *p = sess_path(s, names[i], name);
      const int waiting = p && tf_file_exists(p) && !uploaded_list_has(item, n);
      free(p);
      if (waiting) sum += segment_seconds(s, names[i], n);
    }
    tf_json_free(cap);
    tf_json_free(item);
  }
  return sum;
}

/* ------------------------------------------------------------------ the recording and its takes */
typedef struct {
  long long start_ms;
  int gap_ms;
} Take;
/* One recording: the capture of 2.0.2 (one Ogg stream written into one outbox folder), fed by one take or by
 * several in a row. */
struct Rec {
  TfClient *c;
  char id[33];
  int input_rate, resample;
  TfResampler rs;
  TfEncoder enc;
  TfOgg ogg;
  int opened, failed, closed;
  long long samples, created; /* input samples of every take so far; unix s */
  int seg_bitrate;      /* the bitrate of the segment being written */
  int next_seg;         /* the next segment to open */
  long long next_start; /* its first packet */
  /* what a take must have in common with the recording to join it (the session JSON is written once) */
  char contributor[33], consent_version[40], consent_hash[65], revision[80];
  Take takes[MAX_TAKES]; /* the first is {0, 0} */
  int n_takes;
  int completed;        /* of the last take that had audio */
  long long last_end;   /* when that take ended (wall ms) */
  int close_at_end;     /* the app went to the background during the take: no waiting when it ends */
  int standalone;       /* begun while another take of the client was running: never joined, closed with its take */
  long long tail_left;  /* fed samples the recording still takes after its last take ended (the tail) */
  TfShareSession *take; /* the take writing into it now */
};
/* One take: what tf__sharing_session_begin hands out. */
struct TfShareSession {
  TfClient *c;          /* NULL once the client is gone */
  Rec *rec;             /* NULL: nothing is recorded (any more): sharing went off, a write failed */
  int input_rate;
  char route[16], revision[80];
  long long samples;    /* this take's audio */
  int gap_ms;           /* a joining take: the rounded wall gap before it; -1: the first take of its recording */
  int entered;          /* its boundary is decided (at its first audio) */
  /* The 20-minute cap, live (a take's length is not known when it starts): with at most HOLD_MS of room left
   * under the cap (counted in samples), a joining take's audio is held here until the take ends (it joins if the
   * recording's samples plus the take's, its pre-roll included, stay within the cap) or outgrows the room (the
   * recording closes without it and the take starts a new recording with the held audio). With more room the
   * take is written as it comes, and where the recording reaches the cap it closes and the take goes on in a
   * new one (push_locked). Tried and rejected: holding every joining take (a crash would lose it, and its
   * segments would wait). */
  int holding;
  float *hold;
  long long hold_n, hold_cap;
  /* the pre-roll: the feed's last second before the take, written with the take's first audio (in front of it:
   * the take's entry in the take list starts after it) */
  float *pre;
  long long pre_n;
  long long hold_pre;   /* of the held audio, how much is pre-roll */
  long long start_wall; /* when the take began (wall ms) */
};

static long long ms_of(long long samples, int rate) { return rate > 0 ? samples * 1000 / rate : 0; }
static long long rec_ms(const Rec *o) { return ms_of(o->samples, o->input_rate); }
/* a wall gap in ms, rounded to the nearest 100 (half up) within 0 to 10000 (a clock set back gives 0) */
static int round_gap(long long gap) {
  if (gap < 0) gap = 0;
  if (gap > JOIN_MS_MAX) gap = JOIN_MS_MAX;
  return (int)((gap + 50) / 100 * 100);
}
/* the join window in force: flags.share_join_ms, a whole number 0 to 10000, else 3000 */
static int join_window(const TfClient *c) { return tf__client_flag_int(c, "share_join_ms", 0, JOIN_MS_MAX, JOIN_MS_DEFAULT); }

static void capture_save(Rec *x, int ended, int completed) {
  char t[320];
  snprintf(t, sizeof t, "{\"v\":1,\"created\":%lld,\"closed\":%d,\"ended\":%s,\"completed\":%s,\"duration_ms\":%lld,\"pre_skip\":%d,\"samples\":%lld}",
           x->created, x->closed, ended ? "true" : "false", completed ? "true" : "false", rec_ms(x), x->enc.pre_skip, x->samples);
  write_text(sh(x->c), x->id, "capture.json", t);
}
/* a segment closed: its marker gets the granule of its last page, capture.json the count, and the ladder hears
 * of the backlog (this segment included) */
static void on_segment(void *user, int n, int last) {
  Rec *x = (Rec *)user;
  Share *s = sh(x->c);
  (void)last;
  marker_write(s, x->id, n, x->seg_bitrate, x->ogg.last_granule);
  x->closed = n;
  capture_save(x, 0, 0);
  ladder_closed(s, backlog_s(s));
  wake(s);
}
/* before each packet: at a segment boundary the closing segment is paged and closed first (so the ladder hears of
 * it), then the next segment opens at the ladder's bitrate, recorded in its marker before its first packet */
static int before_packet(void *user, long long packet) {
  Rec *x = (Rec *)user;
  if (packet != x->next_start) return 0;
  Share *s = sh(x->c);
  if (tf_ogg_flush(&x->ogg)) return -1;
  const int br = ladder_bitrate(s);
  if (tf_enc_set_bitrate(&x->enc, br)) return -1;
  x->seg_bitrate = br;
  marker_write(s, x->id, x->next_seg, br, -1);
  x->next_seg++;
  x->next_start = tf_ogg_segment_start(x->next_seg);
  return 0;
}
static int put_packet(void *user, const uint8_t *p, int n) { return tf_ogg_packet(&((Rec *)user)->ogg, p, n); }
static int put_samples(void *user, const float *x, int n) {
  Rec *s = (Rec *)user;
  return tf_enc_encode(&s->enc, x, n, put_packet, s);
}

/* ---- the take list ---- */
/* takes.json: exactly the take list call's body. Written when a second take adds its entry, and again when a later
 * one does (tf_write_file replaces the file atomically, so a crash leaves the old list or the new one). */
static void takes_write(const Share *s, const Rec *o) {
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"takes\":[");
  for (int i = 0; i < o->n_takes; ++i)
    tf_buf_printf(&b, "%s{\"start_ms\":%lld,\"gap_ms\":%d}", i ? "," : "", o->takes[i].start_ms, o->takes[i].gap_ms);
  tf_buf_puts(&b, "]}");
  if (b.p) write_text(s, o->id, "takes.json", b.p);
  tf_buf_free(&b);
}
/* The take list of a session folder as the server takes it (the bytes takes.json was written with), or NULL: 2 to
 * 256 entries, the first {0, 0}, start_ms whole and strictly increasing, gap_ms 0 to 10000 in steps of 100.
 * limit_ms >= 0 (a recording closed at its last whole page): the takes that start at or beyond it are left out,
 * none of their audio was kept. *count gets the number of takes (1 without a list). The caller frees. */
static char *takes_body(const Share *s, const char *id, long long limit_ms, int *count) {
  if (count) *count = 1;
  TfJson *j = read_json(s, id, "takes.json");
  const TfJson *a = tf_json_get(j, "takes");
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"takes\":[");
  int n = 0, ok = a && a->type == TJ_ARR;
  long long prev = -1;
  for (int i = 0; ok && i < a->n; ++i) {
    const TfJson *st = tf_json_get(&a->kids[i], "start_ms"), *g = tf_json_get(&a->kids[i], "gap_ms");
    if (!tf_json_is_int(st) || !tf_json_is_int(g)) {
      ok = 0;
      break;
    }
    const long long start = (long long)st->num, gap = (long long)g->num;
    if (i == 0 ? (start < 0 || gap != 0) : start <= prev) ok = 0;
    if (gap < 0 || gap > JOIN_MS_MAX || gap % 100) ok = 0;
    if (!ok) break;
    if (i > 0 && limit_ms >= 0 && start >= limit_ms) break;
    tf_buf_printf(&b, "%s{\"start_ms\":%lld,\"gap_ms\":%lld}", n ? "," : "", start, gap);
    prev = start;
    n++;
  }
  tf_buf_puts(&b, "]}");
  tf_json_free(j);
  if (!ok || n < 2 || n > MAX_TAKES) {
    tf_buf_free(&b);
    return NULL;
  }
  if (count) *count = n;
  return tf_buf_take(&b);
}
/* a recording closed short of its end (its capture died): the list keeps the takes that begin before
 * `duration_ms`; with fewer than 2 left there is no list */
static void takes_trim(const Share *s, const char *id, long long duration_ms) {
  char *p = sess_path(s, id, "takes.json");
  if (p && tf_file_exists(p)) {
    char *body = takes_body(s, id, duration_ms, NULL);
    if (body) write_text(s, id, "takes.json", body);
    else remove(p);
    free(body);
  }
  free(p);
}

/* ---- the window's timer ---- */
static void join_timer(void *arg) {
  TfClient *c = (TfClient *)arg;
  Share *s = sh(c);
  tf_mutex_lock(&s->tm);
  while (!s->t_quit) {
    if (!s->t_armed && !s->t_fire) {
      tf_cond_wait_ms(&s->tcv, &s->tm, 600000);
      continue;
    }
    const double left = s->t_due - tf__now_s() * 1000.0;
    if (left > 0 && !s->t_fire) {
      tf_cond_wait_ms(&s->tcv, &s->tm, (long)left + 1);
      continue;
    }
    s->t_armed = s->t_fire = 0;
    tf_mutex_unlock(&s->tm);
    tf__share_window_check(c);
    tf_mutex_lock(&s->tm);
  }
  tf_mutex_unlock(&s->tm);
}
/* prompt a window check in `ms` (a thread of the client, started at the first use; none in the tests' one-thread
 * mode, which call tf__share_window_check themselves) */
static void timer_arm(Share *s, long long ms) {
  if (tf__client_no_timer) return;
  tf_mutex_lock(&s->tm);
  if (!s->t_on && !s->t_quit && !tf_thread_start(&s->tth, join_timer, s->c)) s->t_on = 1;
  s->t_armed = 1;
  s->t_due = tf__now_s() * 1000.0 + (double)(ms < 0 ? 0 : ms) + 1.0;
  tf_cond_signal(&s->tcv);
  tf_mutex_unlock(&s->tm);
}
static void timer_disarm(Share *s) {
  tf_mutex_lock(&s->tm);
  s->t_armed = 0;
  tf_mutex_unlock(&s->tm);
}
/* tests: the timer's thread checks the window now, whatever its own wait says */
void tf__share_timer_poke(TfClient *c) {
  Share *s = sh(c);
  if (!s) return;
  tf_mutex_lock(&s->tm);
  s->t_fire = 1;
  tf_cond_signal(&s->tcv);
  tf_mutex_unlock(&s->tm);
}

/* ------------------------------------------------------------------ the feed (3.0; vectors README 10.1) */
/* The ring: at most TF_SHARING_RING_MS of the feed's audio, at one rate, in memory only. Emptying it frees it:
 * nothing of the feed is held once sharing is off, the app is in the background or a take has taken it. */
static void ring_empty(Share *s) {
  free(s->ring);
  s->ring = NULL;
  s->ring_cap = s->ring_n = s->ring_at = s->ring_rate = 0;
  s->ring_wall = 0;
}
static void ring_put(Share *s, const float *x, long long n, int rate) {
  if (s->ring_rate != rate || !s->ring) {
    ring_empty(s);
    const int cap = (int)((long long)rate * TF_SHARING_RING_MS / 1000);
    s->ring = (float *)malloc((size_t)cap * sizeof(float));
    if (!s->ring) return;
    s->ring_cap = cap;
    s->ring_rate = rate;
  }
  const long long kept = n > s->ring_cap ? s->ring_cap : n;
  x += n - kept;
  for (long long i = 0; i < kept; ++i) {
    s->ring[s->ring_at] = x[i];
    s->ring_at = (s->ring_at + 1) % s->ring_cap;
  }
  s->ring_n = (int)(s->ring_n + kept > s->ring_cap ? s->ring_cap : s->ring_n + kept);
}
/* the ring's audio, oldest first, handed to a take that starts (the caller frees); the ring is empty afterwards */
static float *ring_take(Share *s, long long *n) {
  *n = 0;
  float *x = s->ring_n ? (float *)malloc((size_t)s->ring_n * sizeof(float)) : NULL;
  if (x) {
    const int from = (s->ring_at - s->ring_n + s->ring_cap) % s->ring_cap;
    for (int i = 0; i < s->ring_n; ++i) x[i] = s->ring[(from + i) % s->ring_cap];
    *n = s->ring_n;
  }
  ring_empty(s);
  return x;
}
/* F8: the tail of a take can take fed audio until TF_SHARING_TAIL_WAIT_MS of wall time after the take's end,
 * for every take, whether or not the app has fed before. Only fed audio ever goes into it. */
static int tail_pending(const Rec *o, long long now) {
  const long long since = now - o->last_end;
  return o->tail_left > 0 && since >= 0 && since <= TF_SHARING_TAIL_WAIT_MS;
}
static long long cap_samples(const Rec *o) { return (long long)TF_SHARING_RECORDING_MAX_S * o->input_rate; }

/* take_seq and gap_ms_prev (vectors README 10.2): this install's count of shared recordings and the wall time
 * its last recorded take ended, in a small file beside the client's state. "Delete what I shared" removes it. */
static void seq_read(TfClient *c, long long *seq, long long *prev_end) {
  *seq = *prev_end = 0;
  char *p = tf_path_join(tf__client_dir(c), "share_seq.json"), *text = NULL;
  size_t n = 0;
  TfJson *j = p && !tf_read_file(p, &text, &n) ? tf_json_parse(text, n, NULL) : NULL;
  if (tf_json_is_int(tf_json_get(j, "seq"))) *seq = (long long)tf_json_get(j, "seq")->num;
  if (tf_json_is_int(tf_json_get(j, "prev_end"))) *prev_end = (long long)tf_json_get(j, "prev_end")->num;
  if (*seq < 0) *seq = 0;
  tf_json_free(j);
  free(text);
  free(p);
}
static void seq_write(TfClient *c, long long seq, long long prev_end) {
  char t[96], *p = tf_path_join(tf__client_dir(c), "share_seq.json");
  const int n = snprintf(t, sizeof t, "{\"seq\":%lld,\"prev_end\":%lld}", seq, prev_end);
  if (p) tf_write_file(p, t, (size_t)n);
  free(p);
}
static void seq_take_ended(TfClient *c, long long now) {
  long long seq, prev;
  seq_read(c, &seq, &prev);
  seq_write(c, seq, now);
}
void tf__share_forget_seq(TfClient *c) {
  char *p = tf_path_join(tf__client_dir(c), "share_seq.json");
  if (p) remove(p);
  free(p);
}
/* a recording starts at wall ms `start`: its take_seq, and its gap_ms_prev (-1: left out) */
static void seq_next(TfClient *c, long long start, long long *take_seq, long long *gap_prev) {
  long long seq, prev;
  seq_read(c, &seq, &prev);
  *take_seq = seq + 1;
  *gap_prev = -1;
  if (prev > 0 && start - prev >= 0 && start - prev <= TF_SHARING_GAP_PREV_MAX_MS) {
    *gap_prev = (start - prev + 50) / 100 * 100;
    if (*gap_prev > TF_SHARING_GAP_PREV_MAX_MS) *gap_prev = TF_SHARING_GAP_PREV_MAX_MS;
  }
  seq_write(c, *take_seq, prev);
}

/* ---- a recording's life ---- */
/* the session JSON, sorted keys (as the Swift and web SDKs write it) */
static char *session_json(const char *contributor, const char *key, const char *rhash, const char *cv, const char *ch, int input_rate,
                          const char *route, const char *revision, long long take_seq, long long gap_prev) {
  TfBuf b;
  tf_buf_init(&b);
  char day[16];
  const time_t t = (time_t)now_s();
  struct tm g;
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  strftime(day, sizeof day, "%Y-%m-%d", &g);
  tf_buf_puts(&b, "{\"audio\":{\"channels\":1,\"codec\":\"opus\",\"container\":\"ogg\"");
  if (input_rate >= 8000 && input_rate <= 192000) tf_buf_printf(&b, ",\"input_sample_rate\":%d", input_rate);
  tf_buf_puts(&b, ",\"input_route\":");
  tf_buf_json_str(&b, route);
  tf_buf_puts(&b, ",\"sample_rate\":48000}");
  /* the version the learner agreed to; with the app's own wording also the SHA-256 of that text (sorted keys:
   * consent_text_sha256 before consent_version). Without own wording the field is absent: the 2.0.0 schema is
   * closed. */
  if (ch[0]) {
    tf_buf_puts(&b, ",\"consent_text_sha256\":");
    tf_buf_json_str(&b, ch);
  }
  if (cv[0]) {
    tf_buf_puts(&b, ",\"consent_version\":");
    tf_buf_json_str(&b, cv);
  }
  tf_buf_printf(&b, ",\"contributor\":\"%s\",\"contributor_key\":\"%s\",\"day\":\"%s\"", contributor, key, day);
  /* SPEC 2a: model_revision carries the model version, model_build the build id (the take's "revision" here is
   * "<version>" or "<version>	<build>"; sorted keys) */
  tf_buf_puts(&b, ",\"decode\":{\"backend\":\"exact\"");
  const char *tab = revision ? strchr(revision, '	') : NULL;
  if (tab && tab[1]) {
    tf_buf_puts(&b, ",\"model_build\":");
    tf_buf_json_str(&b, tab + 1);
  }
  tf_buf_puts(&b, ",\"model_revision\":");
  if (revision && *revision) {
    char ver[80];
    snprintf(ver, sizeof ver, "%.*s", tab ? (int)(tab - revision) : (int)strlen(revision), revision);
    tf_buf_json_str(&b, ver);
  } else {
    tf_buf_puts(&b, "null");
  }
  tf_buf_printf(&b, ",\"sdk_version\":\"%s\",\"weights_sha256\":null}", TF_SDK_VERSION);
  /* 3.0: this install's count of shared recordings, and the time since its previous one (sorted keys) */
  tf_buf_puts(&b, ",\"device_class\":\"x86_64\",\"format\":1");
  if (gap_prev >= 0) tf_buf_printf(&b, ",\"gap_ms_prev\":%lld", gap_prev);
  tf_buf_printf(&b, ",\"os_major\":%d,\"recovery_hash\":\"%s\",\"take_seq\":%lld}", tf__client_os_major(), rhash, take_seq);
  return tf_buf_take(&b);
}

static const char *const kRoutes[] = {"built_in", "wired", "usb", "bluetooth", "car", "continuity", "other", "unknown", NULL};

/* the recording is over (closed, dropped or lost): nothing points at it any more */
static void rec_release(Share *s, Rec *o) {
  if (s->open == o) {
    s->open = NULL;
    timer_disarm(s);
  }
  if (o->take) {
    o->take->rec = NULL;
    o->take->holding = 0;
    o->take = NULL;
  }
  if (o->resample) tf_rs_free(&o->rs);
  tf_enc_free(&o->enc);
  set_active(s, o->id, 0);
  wake(s);
  free(o);
}
/* nothing of the recording is kept: sharing went off, what was shared was deleted, or a write failed */
static void rec_drop(Share *s, Rec *o) {
  tf_ogg_abandon(&o->ogg);
  remove_session(s, o->id);
  rec_release(s, o);
}
/* The end of a recording, as a 2.0.2 session ends: the resampler and the encoder are flushed, the last page gets
 * EOS, capture.json says ended. Nothing is kept when sharing went off, a write failed, or the last take with audio
 * was cancelled and the recording holds under 2 s with no segment closed. Returns the milliseconds kept. */
static int rec_close(Share *s, Rec *o) {
  int ms = 0;
  const double dur = o->input_rate ? (double)o->samples / o->input_rate : 0;
  const int keep = o->opened && !o->failed && tf__client_sharing_state(o->c) && dur > 0 &&
                   (o->completed || dur >= MIN_PARTIAL_S || o->closed > 0);
  int rc = keep ? 0 : -1;
  if (keep) {
    rc = o->resample ? tf_rs_flush(&o->rs, put_samples, o) : 0;
    if (!rc) rc = tf_enc_flush(&o->enc, put_packet, o);
    if (!rc) rc = tf_ogg_finish(&o->ogg, tf_enc_final_granule(&o->enc));
  }
  if (!rc) {
    capture_save(o, 1, o->completed);
    ms = (int)(dur * 1000 + 0.5);
  } else {
    tf_ogg_abandon(&o->ogg);
    remove_session(s, o->id);
  }
  rec_release(s, o);
  return ms;
}
/* tests: the process "dies" (the files stay as they are, nothing is finished) */
static void rec_crash(Share *s, Rec *o) {
  tf_ogg_abandon(&o->ogg);
  rec_release(s, o);
}

uint32_t tf__share_serial; /* tests: the Ogg serial of the next recordings (0: random) */

/* a new recording: its folder, session JSON, capture.json and first marker, the encoder and the stream's headers */
static Rec *rec_open(TfClient *c, Share *s, int rate, const char *route, const char *revision, const char *contributor,
                     const char *key, const char *rhash, const char *cv, const char *ch, long long start_wall) {
  Rec *x = (Rec *)calloc(1, sizeof *x);
  if (!x) {
    tf__fail(TF__ERR_NO_MEMORY, "out of memory");
    return NULL;
  }
  uint8_t r[16];
  tf_random_bytes(r, sizeof r);
  tf_hex_encode(r, 16, x->id);
  x->c = c;
  x->input_rate = rate;
  x->resample = rate != 48000;
  x->created = now_s();
  snprintf(x->contributor, sizeof x->contributor, "%s", contributor);
  snprintf(x->consent_version, sizeof x->consent_version, "%s", cv);
  snprintf(x->consent_hash, sizeof x->consent_hash, "%s", ch);
  snprintf(x->revision, sizeof x->revision, "%s", revision);
  x->n_takes = 1; /* the first take: {0, 0} */
  char *dir = sess_path(s, x->id, NULL);
  long long take_seq, gap_prev;
  seq_next(c, start_wall, &take_seq, &gap_prev);
  char *body = session_json(contributor, key, rhash, cv, ch, rate, route, revision, take_seq, gap_prev);
  if (!dir || !body || tf_mkdirs(dir) || write_text(s, x->id, "session.json", body)) {
    free(dir);
    free(body);
    free(x);
    tf__fail(TF__ERR_IO, "cannot write the sharing outbox");
    return NULL;
  }
  free(body);
  set_active(s, x->id, 1);
  /* segment 1 opens with the rung in force when the capture starts */
  const int bitrate = ladder_bitrate(s);
  x->seg_bitrate = bitrate;
  x->next_seg = 2;
  x->next_start = tf_ogg_segment_start(2);
  uint32_t serial;
  tf_random_bytes((uint8_t *)&serial, sizeof serial);
  if (tf__share_serial) serial = tf__share_serial;
  char vendor[96];
  snprintf(vendor, sizeof vendor, "QuranLab SDK %s %s", TF_SDK_VERSION, tf_opus_version());
  int bad = (x->resample && tf_rs_init(&x->rs, rate)) || tf_enc_init(&x->enc, bitrate);
  if (!bad) {
    capture_save(x, 0, 0); /* after the encoder: it carries the pre-skip, which recovery needs for the duration */
    marker_write(s, x->id, 1, bitrate, -1);
    bad = tf_ogg_open(&x->ogg, dir, serial, x->enc.pre_skip, rate, vendor);
  }
  free(dir);
  if (bad) {
    rec_drop(s, x);
    tf__fail(TF_ERR_ENGINE_FAILED, "the Opus encoder or the Ogg writer could not start");
    return NULL;
  }
  x->ogg.on_segment = on_segment;
  x->ogg.user = x;
  x->enc.before = before_packet;
  x->enc.before_user = x;
  x->opened = 1;
  return x;
}

/* audio into the recording's stream; a failed write ends the recording for good (it is deleted when it closes) */
static int rec_encode(Rec *o, const float *pcm, long long n) {
  while (n > 0) {
    const int k = n > 48000 ? 48000 : (int)n;
    o->samples += k;
    const int rc = o->resample ? tf_rs_process(&o->rs, pcm, k, put_samples, o) : tf_enc_encode(&o->enc, pcm, k, put_packet, o);
    if (rc) {
      o->failed = 1;
      return -1;
    }
    pcm += k;
    n -= k;
  }
  return 0;
}

/* The take's first audio is about to go into the recording: a joining take adds its boundary there, so a take
 * without audio adds none. A boundary that would not be after the previous one (a take of under a millisecond
 * before it) is left out too, its audio still joins: the list stays strictly increasing. */
static void take_enter(Share *s, Rec *o, TfShareSession *x) {
  x->entered = 1;
  if (x->gap_ms < 0) return; /* the first take of the recording */
  const long long start = rec_ms(o);
  if (o->n_takes >= MAX_TAKES || start <= o->takes[o->n_takes - 1].start_ms) return;
  o->takes[o->n_takes].start_ms = start;
  o->takes[o->n_takes].gap_ms = x->gap_ms;
  o->n_takes++;
  takes_write(s, o);
}

TfShareSession *tf__sharing_session_begin(TfClient *c, int input_sample_rate, const char *input_route) {
  return tf__share_begin(c, input_sample_rate, input_route, NULL, NULL);
}

static TfShareSession *begin_locked(TfClient *c, Share *s, int rate, const char *input_route, const char *revision) {
  Rec *o = s->open;
  if (!tf__client_sharing_state(c) || !tf__sharing_may_upload(c)) {
    /* sharing went off while a recording waited for its next take: it ends now (nothing of it is kept without the
     * learner's yes or the config's offer; with uploads only paused for this platform it waits in the queue) */
    if (o && !o->take) rec_close(s, o);
    tf__fail(TF__ERR_NOT_AVAILABLE, "sharing is off (consent, the contributions option, flags.contributions or upload_trust)");
    return NULL;
  }
  if (rate < 8000 || rate > 192000) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "input sample rate %d is not 8000 to 192000", rate);
    return NULL;
  }
  const char *route = "unknown";
  for (int i = 0; input_route && kRoutes[i]; ++i)
    if (!strcmp(input_route, kRoutes[i])) route = kRoutes[i];
  char contributor[33], key[90], rhash[65];
  if (tf__contributor(tf__client_dir(c), 1, contributor, key, rhash)) {
    tf__fail(TF__ERR_NOT_AVAILABLE, "the device key cannot be created");
    return NULL;
  }
  char cv[40], ch[65];
  tf__sharing_grant(c, cv, ch);
  TfShareSession *x = (TfShareSession *)calloc(1, sizeof *x);
  if (!x) {
    tf__fail(TF__ERR_NO_MEMORY, "out of memory");
    return NULL;
  }
  x->c = c;
  x->input_rate = rate;
  x->gap_ms = -1;
  /* the pre-roll (F6): the ring, when it has the take's rate and its last buffer came at most 500 ms ago; the
   * ring is empty afterwards either way */
  x->start_wall = now_ms();
  if (s->ring_n && s->ring_rate == rate && x->start_wall - s->ring_wall <= TF_SHARING_FEED_STALE_MS) x->pre = ring_take(s, &x->pre_n);
  ring_empty(s);
  s->st_pre = x->pre_n;
  snprintf(x->route, sizeof x->route, "%s", route);
  snprintf(x->revision, sizeof x->revision, "%s", revision ? revision : "");
  if (o && !o->take) {
    /* a recording waits in its join window: this take continues it, or ends it */
    const long long gap = now_ms() - o->last_end;
    const int win = join_window(c);
    /* the uploader may have dropped the session under its capture (the server refused the upload, capped it or
     * does not know it): nothing joins such a recording, and nothing of it is kept */
    char *alive = sess_path(s, o->id, "capture.json");
    if (!alive || !tf_file_exists(alive)) o->failed = 1;
    free(alive);
    if (win > 0 && gap <= win && !o->failed && o->input_rate == rate && rec_ms(o) < REC_CAP_MS && o->n_takes < MAX_TAKES &&
        !strcmp(o->contributor, contributor) && !strcmp(o->consent_version, cv) && !strcmp(o->consent_hash, ch) &&
        !strcmp(o->revision, x->revision)) {
      timer_disarm(s);
      x->rec = o;
      o->take = x;
      x->gap_ms = round_gap(gap);
      x->holding = cap_samples(o) - o->samples <= (long long)HOLD_MS * o->input_rate / 1000;
      return x;
    }
    rec_close(s, o);
    o = NULL;
  }
  Rec *n = rec_open(c, s, rate, route, x->revision, contributor, key, rhash, cv, ch, x->start_wall);
  if (!n) {
    free(x->pre);
    free(x);
    return NULL;
  }
  /* A take begun while another take of this client runs (the calls allow it) records on its own, as before 2.0.3:
   * its recording closes when it ends and no take joins it. */
  n->standalone = o != NULL;
  if (!n->standalone) s->open = n;
  n->take = x;
  x->rec = n;
  x->entered = 1;
  return x;
}

TfShareSession *tf__share_begin(TfClient *c, int input_sample_rate, const char *input_route, const char *version, const char *build) {
  tf__clear_error();
  char revision[80] = "";
  if (version && *version && build && *build) snprintf(revision, sizeof revision, "%s	%s", version, build);
  else if (version) snprintf(revision, sizeof revision, "%s", version);
  Share *s = sh(c);
  if (!s) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
    return NULL;
  }
  tf_mutex_lock(&s->rm);
  TfShareSession *x = begin_locked(c, s, input_sample_rate, input_route, revision);
  tf_mutex_unlock(&s->rm);
  return x;
}

static void hold_free(TfShareSession *x) {
  free(x->hold);
  x->hold = NULL;
  x->hold_n = x->hold_cap = x->hold_pre = 0;
  x->holding = 0;
  free(x->pre);
  x->pre = NULL;
  x->pre_n = 0;
}

/* the recording a take goes on in when its own is full or closed under it: the next take_seq, counted from
 * `start_wall` (NULL when sharing may not go on) */
static Rec *rec_next(Share *s, TfShareSession *x, long long start_wall, int standalone) {
  char contributor[33], key[90], rhash[65], cv[40], ch[65];
  if (!tf__sharing_may_upload(x->c) || tf__contributor(tf__client_dir(x->c), 1, contributor, key, rhash)) return NULL;
  tf__sharing_grant(x->c, cv, ch);
  Rec *nr = rec_open(x->c, s, x->input_rate, x->route, x->revision, contributor, key, rhash, cv, ch, start_wall);
  if (!nr) return NULL;
  nr->standalone = standalone;
  if (!standalone) s->open = nr;
  nr->take = x;
  x->rec = nr;
  x->gap_ms = -1;
  x->entered = 1;
  return nr;
}

static int push_locked(Share *s, TfShareSession *x, const float *pcm, int n) {
  Rec *o = x->rec;
  if (!o || o->failed) return 0;
  /* consent withdrawn during the take: nothing more is written and what the recording holds is deleted at once */
  if (!tf__client_sharing_state(x->c)) {
    hold_free(x);
    rec_drop(s, o);
    return 0;
  }
  if (x->holding) {
    const long long pre = x->pre ? x->pre_n : 0; /* held in front of the take's first audio */
    if (x->hold_n + pre + n > x->hold_cap) {
      long long cap = x->hold_cap ? x->hold_cap : 65536;
      while (cap < x->hold_n + pre + n) cap *= 2;
      float *p = (float *)realloc(x->hold, sizeof(float) * (size_t)cap);
      if (!p) { /* no memory to hold it: the recording closes without this take, which is not shared */
        hold_free(x);
        o->take = NULL;
        x->rec = NULL;
        rec_close(s, o);
        return tf__fail(TF__ERR_NO_MEMORY, "out of memory");
      }
      x->hold = p;
      x->hold_cap = cap;
    }
    if (pre) {
      memcpy(x->hold, x->pre, sizeof(float) * (size_t)pre);
      x->hold_n = x->hold_pre = pre;
      free(x->pre);
      x->pre = NULL;
      x->pre_n = 0;
    }
    memcpy(x->hold + x->hold_n, pcm, sizeof(float) * (size_t)n);
    x->hold_n += n;
    x->samples += n;
    if (o->samples + x->hold_n <= cap_samples(o)) return 0; /* the fit is in samples: a held take that joins is never cut */
    /* the held take outgrew the room under the cap: the recording closes without it, and the take starts a new
     * recording with the held audio */
    o->take = NULL;
    x->rec = NULL;
    x->holding = 0;
    rec_close(s, o);
    char contributor[33], key[90], rhash[65], cv[40], ch[65];
    Rec *nr = NULL;
    if (tf__sharing_may_upload(x->c) && !tf__contributor(tf__client_dir(x->c), 1, contributor, key, rhash)) {
      tf__sharing_grant(x->c, cv, ch);
      nr = rec_open(x->c, s, x->input_rate, x->route, x->revision, contributor, key, rhash, cv, ch, x->start_wall);
    }
    int rc = 0;
    if (nr) {
      s->open = nr;
      nr->take = x;
      x->rec = nr;
      x->gap_ms = -1;
      x->entered = 1;
      nr->takes[0].start_ms = ms_of(x->hold_pre, nr->input_rate); /* the take starts after its pre-roll */
      rc = rec_encode(nr, x->hold, x->hold_n);
    }
    hold_free(x);
    tf__clear_error(); /* a recording that could not start is not an error of the take's audio */
    return rc ? tf__fail(TF__ERR_IO, "the shared session could not be written") : 0;
  }
  if (x->pre) {
    /* the pre-roll goes in first; the take's own first sample comes after it (its start in the take list) */
    const int bad = rec_encode(o, x->pre, x->pre_n);
    if (x->gap_ms < 0 && o->n_takes == 1) o->takes[0].start_ms = rec_ms(o);
    free(x->pre);
    x->pre = NULL;
    x->pre_n = 0;
    if (bad) return tf__fail(TF__ERR_IO, "the shared session could not be written");
  }
  if (!x->entered) take_enter(s, o, x);
  x->samples += n;
  /* A recording never holds more than TF_SHARING_RECORDING_MAX_S of audio: where a take reaches the cap the
   * recording closes as a whole one, and the take goes on in a new recording at its next sample (the next
   * take_seq, gap_ms_prev 0, no pre-roll). */
  const long long cap = cap_samples(o);
  while (o->samples + n > cap) {
    const long long room = cap > o->samples ? cap - o->samples : 0;
    if (room && rec_encode(o, pcm, room)) return tf__fail(TF__ERR_IO, "the shared session could not be written");
    pcm += room;
    n -= (int)room;
    const long long now = now_ms();
    const int alone = o->standalone;
    o->completed = 1;
    o->last_end = now;
    o->take = NULL;
    x->rec = NULL;
    rec_close(s, o);
    seq_take_ended(x->c, now);
    o = rec_next(s, x, now, alone);
    tf__clear_error();
    if (!o) return 0; /* sharing may not go on: the rest of the take is not recorded */
  }
  if (n > 0 && rec_encode(o, pcm, n)) return tf__fail(TF__ERR_IO, "the shared session could not be written");
  return 0;
}

int tf__sharing_session_push(TfShareSession *x, const float *pcm, int n) {
  if (!x || n < 0 || (n && !pcm)) return tf__fail(TF_ERR_INVALID_ARGUMENT, "bad arguments");
  Share *s = sh(x->c);
  if (!s || !n) return 0;
  tf_mutex_lock(&s->rm);
  const int rc = push_locked(s, x, pcm, n);
  tf_mutex_unlock(&s->rm);
  return rc;
}

/* The end of a take. Its recording stays open for the join window (the next take may continue it) unless nothing
 * could join it any more: no audio, a window of 0, the app in the background, the cap or the take limit reached,
 * a take that recorded on its own. Returns the take's milliseconds that went into a recording (0: nothing, also
 * when the recording was dropped here: sharing off, a failed write, a cancelled take under 2 s with nothing to
 * join). A cancelled take (completed 0) that waits in the window may still be dropped when its recording closes. */
static int finish_locked(Share *s, TfShareSession *x, int completed) {
  Rec *o = x->rec;
  if (!o) return 0;
  const int on = tf__client_sharing_state(x->c);
  if (x->holding) {
    /* the held take ended within the room under the cap: it joins */
    x->holding = 0;
    if (x->hold_n > 0 && on && !o->failed) {
      if (x->hold_pre) rec_encode(o, x->hold, x->hold_pre);
      take_enter(s, o, x);
      rec_encode(o, x->hold + x->hold_pre, x->hold_n - x->hold_pre);
    }
  }
  free(x->pre); /* a take without audio: its pre-roll is not recorded */
  x->pre = NULL;
  x->pre_n = 0;
  const long long now = now_ms();
  if (x->samples > 0) {
    o->completed = completed;
    o->last_end = now;
    /* the tail (F4): the feed's next half second belongs to this recording; the time since the previous
     * recording counts from here */
    o->tail_left = (long long)o->input_rate * TF_SHARING_TAIL_MS / 1000;
    s->last_audio = now;
    if (on && !o->failed) seq_take_ended(x->c, now);
  }
  o->take = NULL;
  x->rec = NULL;
  const int take_ms = (int)((double)x->samples / x->input_rate * 1000 + 0.5);
  if (o->failed || !on) {
    rec_drop(s, o);
    return 0;
  }
  const int win = join_window(x->c);
  const long long waited = now - o->last_end;
  if (!o->standalone && o->samples > 0 && win > 0 && !o->close_at_end && rec_ms(o) < REC_CAP_MS && o->n_takes < MAX_TAKES &&
      waited <= win) {
    timer_arm(s, win - (waited > 0 ? waited : 0));
    return take_ms;
  }
  /* nothing can join it, but its tail may still come (also with a join window of 0); a full recording takes none */
  if (!o->standalone && o->samples > 0 && !o->close_at_end && o->samples < cap_samples(o) && tail_pending(o, now)) {
    timer_arm(s, TF_SHARING_TAIL_WAIT_MS - (waited > 0 ? waited : 0) + 1);
    return take_ms;
  }
  return rec_close(s, o) ? take_ms : 0;
}

int tf__sharing_session_finish(TfShareSession *x, int completed) {
  if (!x) return 0;
  Share *s = sh(x->c);
  int ms = 0;
  if (s) {
    tf_mutex_lock(&s->rm);
    ms = finish_locked(s, x, completed);
    tf_mutex_unlock(&s->rm);
  }
  free(x->pre);
  free(x->hold);
  free(x);
  return ms;
}

/* The join window's end, and every other moment something may have ended the wait (the timer's prompt, a new
 * signed config): the open recording closes once the clock says the window passed, or sharing went off, or the
 * window is now 0; otherwise the timer is set for the rest. A clock that went backwards since the take ended
 * closes it too: the wait cannot be measured any more. */
void tf__share_window_check(TfClient *c) {
  Share *s = sh(c);
  if (!s) return;
  tf_mutex_lock(&s->rm);
  Rec *o = s->open;
  if (o && !o->take) {
    const int win = join_window(c);
    const long long waited = now_ms() - o->last_end;
    const long long now = o->last_end + waited;
    const int over = win <= 0 || waited > win || waited < 0;
    if (!tf__client_sharing_state(c) || !tf__sharing_may_upload(c)) rec_close(s, o);
    else if (!over) timer_arm(s, win - waited);
    else if (o->samples < cap_samples(o) && tail_pending(o, now)) timer_arm(s, TF_SHARING_TAIL_WAIT_MS - waited + 1); /* its tail first */
    else rec_close(s, o);
  }
  tf_mutex_unlock(&s->rm);
}

/* The open recording ends now instead of at the end of its join window (the app goes to the background, or knows
 * that no take follows); with a take running, when that take ends. */
static void recorder_end(Share *s) {
  tf_mutex_lock(&s->rm);
  Rec *o = s->open;
  if (o && o->take) o->close_at_end = 1;
  else if (o) rec_close(s, o);
  ring_empty(s);
  tf_mutex_unlock(&s->rm);
}
void tf__share_background(TfClient *c) {
  Share *s = sh(c);
  if (s) recorder_end(s);
}
int tf__sharing_recording_close(TfClient *c) {
  Share *s = sh(c);
  if (!s) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  recorder_end(s);
  return 0;
}
/* sharing went off, or what was shared was deleted: the open recording ends at once, whatever its take does, and
 * nothing of it is kept (the take's later pushes record nothing) */
static void recorder_stop(Share *s) {
  tf_mutex_lock(&s->rm);
  if (s->open) {
    if (s->open->take) hold_free(s->open->take);
    rec_drop(s, s->open);
  }
  ring_empty(s);
  tf_mutex_unlock(&s->rm);
}
/* the client is closing: the open recording ends as when its window ends; a take still running on it is over for
 * sharing (and counts as not completed) */
static void recorder_free(Share *s) {
  tf_mutex_lock(&s->rm);
  Rec *o = s->open;
  if (o) {
    if (o->take) {
      if (o->take->samples > 0) o->completed = 0;
      o->take->c = NULL;
      hold_free(o->take);
    }
    rec_close(s, o);
  }
  tf_mutex_unlock(&s->rm);
}

/* tests: the process "dies" during a take, or between two takes (files stay as they are, nothing is finished) */
void tf__share_crash(TfShareSession *x) {
  if (!x) return;
  Share *s = sh(x->c);
  if (s) {
    tf_mutex_lock(&s->rm);
    if (x->rec) rec_crash(s, x->rec);
    tf_mutex_unlock(&s->rm);
  }
  free(x->pre);
  free(x->hold);
  free(x);
}
void tf__share_crash_open(TfClient *c) {
  Share *s = sh(c);
  if (!s) return;
  tf_mutex_lock(&s->rm);
  if (s->open) rec_crash(s, s->open);
  tf_mutex_unlock(&s->rm);
}
/* tests: the join window in force; the open recording's id, takes and input samples (0 when none is open) */
int tf__share_join_ms(TfClient *c) { return join_window(c); }
int tf__share_open(TfClient *c, char id[33], int *takes, long long *samples) {
  Share *s = sh(c);
  if (!s) return 0;
  tf_mutex_lock(&s->rm);
  const Rec *o = s->open;
  if (o) {
    if (id) snprintf(id, 33, "%s", o->id);
    if (takes) *takes = o->n_takes;
    if (samples) *samples = o->samples;
  }
  tf_mutex_unlock(&s->rm);
  return o != NULL;
}

/* ------------------------------------------------------------------ recovery of a session whose capture died */
static uint32_t crc_tab[256];
static uint32_t page_crc(const uint8_t *d, size_t n) {
  if (!crc_tab[1])
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t r = i << 24;
      for (int k = 0; k < 8; ++k) r = r & 0x80000000u ? (r << 1) ^ 0x04c11db7u : r << 1;
      crc_tab[i] = r;
    }
  uint32_t c = 0;
  for (size_t i = 0; i < n; ++i) c = (c << 8) ^ crc_tab[((c >> 24) & 0xff) ^ d[i]];
  return c;
}
/* bytes of whole, valid pages at the start of `d`, how many of them carry audio (granule > 0), and the granule of
 * the last one */
static size_t whole_pages(const uint8_t *d, size_t n, int *audio, long long *last_granule) {
  size_t at = 0;
  *audio = 0;
  *last_granule = 0;
  while (at + 27 <= n && !memcmp(d + at, "OggS", 4)) {
    const int ns = d[at + 26];
    if (at + 27 + (size_t)ns > n) break;
    size_t body = 0;
    for (int i = 0; i < ns; ++i) body += d[at + 27 + i];
    const size_t len = 27 + (size_t)ns + body;
    if (at + len > n) break;
    uint8_t *tmp = (uint8_t *)malloc(len);
    if (!tmp) break;
    memcpy(tmp, d + at, len);
    const uint32_t want = (uint32_t)tmp[22] | (uint32_t)tmp[23] << 8 | (uint32_t)tmp[24] << 16 | (uint32_t)tmp[25] << 24;
    memset(tmp + 22, 0, 4);
    const int ok = page_crc(tmp, len) == want;
    free(tmp);
    if (!ok) break;
    long long g = 0;
    for (int i = 7; i >= 0; --i) g = (g << 8) | d[at + 6 + i];
    if (g > 0) (*audio)++;
    *last_granule = g;
    at += len;
  }
  return at;
}
static void recover(Share *s, const char *id, const TfJson *cap) {
  const int closed = tf_json_get(cap, "closed") ? (int)tf_json_get(cap, "closed")->num : 0;
  const long long created = tf_json_get(cap, "created") ? (long long)tf_json_get(cap, "created")->num : now_s();
  /* the stream's pre-skip, as the capture recorded it (a capture.json from before 2.0.3 has none: libopus's) */
  const TfJson *ps = tf_json_get(cap, "pre_skip");
  const long long pre_skip = ps && ps->type == TJ_NUM && ps->num > 0 ? (long long)ps->num : PRE_SKIP_DEFAULT;
  char name[32];
  snprintf(name, sizeof name, "seg-%05d.opus", closed + 1);
  char *p = sess_path(s, id, name), *data = NULL;
  size_t n = 0;
  int kept = closed;
  long long last_granule = -1;
  if (p && !tf_read_file(p, &data, &n)) {
    int audio = 0;
    long long granule = 0;
    const size_t good = whole_pages((const uint8_t *)data, n, &audio, &granule);
    if (audio > 0) {
      tf_write_file(p, data, good);
      kept = closed + 1;
      last_granule = granule;
      /* the segment closes at its last whole page, with the bitrate its marker recorded when it opened */
      int br = 0;
      marker_read(s, id, kept, &br, NULL);
      marker_write(s, id, kept, br, granule);
    } else {
      remove(p);
    }
  }
  free(data);
  free(p);
  if (!kept) {
    remove_session(s, id);
    return;
  }
  /* what was kept: up to the last whole page. The take list (2.0.3) keeps the takes that begin before it. */
  if (last_granule < 0) marker_read(s, id, kept, NULL, &last_granule);
  const long long duration_ms = last_granule > pre_skip ? (last_granule - pre_skip) / 48 : 0;
  takes_trim(s, id, duration_ms);
  char t[320];
  snprintf(t, sizeof t,
           "{\"v\":1,\"created\":%lld,\"closed\":%d,\"ended\":true,\"completed\":false,\"duration_ms\":%lld,\"pre_skip\":%lld,\"recovered\":true}",
           created, kept, duration_ms, pre_skip);
  write_text(s, id, "capture.json", t);
}

/* ------------------------------------------------------------------ the uploader */
typedef struct {
  int n;
  char *url;
  long long exp;
} Url;
typedef struct {
  int started, attempts, done_n, n_urls;
  long long next_at;
  unsigned char *done; /* done[n] for segments 1..cap */
  int cap;
  Url *urls;
  char error[160];
  /* 2.0.3: every segment went up (the session is uploaded and counted); only the take list is still to send. The
   * folder then holds this file and takes.json alone, so the recording's contributor and age are kept here. */
  int uploaded;
  int refused; /* 3.0: the server answered the take list with 400 once and the issue was reported */
  char contributor[33];
  long long created;
} Item;

static void item_free(Item *it) {
  for (int i = 0; i < it->n_urls; ++i) free(it->urls[i].url);
  free(it->urls);
  free(it->done);
  memset(it, 0, sizeof *it);
}
static int item_done(const Item *it, int n) { return n < it->cap && it->done[n]; }
static void item_mark(Item *it, int n) {
  if (n >= it->cap) {
    const int nc = n + 64;
    unsigned char *d = (unsigned char *)realloc(it->done, (size_t)nc);
    if (!d) return;
    memset(d + it->cap, 0, (size_t)(nc - it->cap));
    it->done = d;
    it->cap = nc;
  }
  it->done[n] = 1;
}
static void item_drop_url(Item *it, int n) {
  for (int i = 0; i < it->n_urls; ++i)
    if (it->urls[i].n == n) {
      free(it->urls[i].url);
      it->urls[i] = it->urls[--it->n_urls];
      return;
    }
}
static const Url *item_url(const Item *it, int n) {
  for (int i = 0; i < it->n_urls; ++i)
    if (it->urls[i].n == n) return &it->urls[i];
  return NULL;
}
static void item_add_url(Item *it, int n, const char *url, long long exp) {
  item_drop_url(it, n);
  Url *u = (Url *)realloc(it->urls, sizeof(Url) * (size_t)(it->n_urls + 1));
  if (!u) return;
  it->urls = u;
  char *d = (char *)malloc(strlen(url) + 1);
  if (!d) return;
  strcpy(d, url);
  it->urls[it->n_urls].n = n;
  it->urls[it->n_urls].url = d;
  it->urls[it->n_urls].exp = exp;
  it->n_urls++;
}
static void item_load(Share *s, const char *id, Item *it) {
  memset(it, 0, sizeof *it);
  TfJson *j = read_json(s, id, "item.json");
  if (!j) return;
  const TfJson *v;
  if ((v = tf_json_get(j, "started")) && v->type == TJ_BOOL) it->started = v->boolean;
  if ((v = tf_json_get(j, "attempts")) && v->type == TJ_NUM) it->attempts = (int)v->num;
  if ((v = tf_json_get(j, "next_at")) && v->type == TJ_NUM) it->next_at = (long long)v->num;
  const char *e = tf_json_str(tf_json_get(j, "error"));
  if (e) snprintf(it->error, sizeof it->error, "%s", e);
  if ((v = tf_json_get(j, "done")) && v->type == TJ_BOOL) it->uploaded = v->boolean;
  if ((v = tf_json_get(j, "refused")) && v->type == TJ_BOOL) it->refused = v->boolean;
  if ((v = tf_json_get(j, "created")) && v->type == TJ_NUM) it->created = (long long)v->num;
  const char *ct = tf_json_str(tf_json_get(j, "contributor"));
  if (ct && is_id(ct)) snprintf(it->contributor, sizeof it->contributor, "%s", ct);
  const TfJson *up = tf_json_get(j, "uploaded");
  for (int i = 0; up && up->type == TJ_ARR && i < up->n; ++i) item_mark(it, (int)up->kids[i].num);
  const TfJson *us = tf_json_get(j, "urls");
  for (int i = 0; us && us->type == TJ_ARR && i < us->n; ++i) {
    const TfJson *u = &us->kids[i];
    const char *url = tf_json_str(tf_json_get(u, "url"));
    if (url && tf_json_get(u, "n") && tf_json_get(u, "exp")) item_add_url(it, (int)tf_json_get(u, "n")->num, url, (long long)tf_json_get(u, "exp")->num);
  }
  tf_json_free(j);
}
static void item_save(Share *s, const char *id, const Item *it) {
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "{\"v\":1,\"started\":%s,\"attempts\":%d,\"next_at\":%lld,\"error\":", it->started ? "true" : "false", it->attempts,
                it->next_at);
  tf_buf_json_str(&b, it->error);
  if (it->uploaded) tf_buf_printf(&b, ",\"done\":true,\"contributor\":\"%s\",\"created\":%lld", it->contributor, it->created);
  if (it->refused) tf_buf_puts(&b, ",\"refused\":true");
  tf_buf_puts(&b, ",\"uploaded\":[");
  int first = 1;
  for (int n = 0; n < it->cap; ++n)
    if (it->done[n]) {
      tf_buf_printf(&b, "%s%d", first ? "" : ",", n);
      first = 0;
    }
  tf_buf_puts(&b, "],\"urls\":[");
  for (int i = 0; i < it->n_urls; ++i) {
    tf_buf_printf(&b, "%s{\"n\":%d,\"exp\":%lld,\"url\":", i ? "," : "", it->urls[i].n, it->urls[i].exp);
    tf_buf_json_str(&b, it->urls[i].url);
    tf_buf_puts(&b, "}");
  }
  tf_buf_puts(&b, "]}");
  if (b.p) write_text(s, id, "item.json", b.p);
  tf_buf_free(&b);
}

static double frand(void) {
  uint8_t r[2];
  tf_random_bytes(r, 2);
  return (r[0] | r[1] << 8) / 65535.0;
}
static void retry(Item *it, const char *why, int immediately, int lng) {
  snprintf(it->error, sizeof it->error, "%s", why);
  const int n = it->attempts > 1 ? it->attempts : 1;
  double base = 30.0;
  for (int i = 1; i < n && i <= 16; ++i) base *= 2;
  if (base > 6 * 3600.0 || lng) base = 6 * 3600.0;
  it->next_at = now_s() + (immediately ? 0 : (long long)(base * (0.8 + 0.4 * frand())));
}

/* outcome of a Worker answer: 1 URLs taken; 0 retry scheduled / paused; -1 the session is gone (deleted) */
enum { ANS_URLS = 1, ANS_WAIT = 0, ANS_GONE = -1, ANS_OFFLINE = -2 };
static int answer(TfClient *c, Share *s, const char *id, Item *it, int call_rc, TfHttpResponse *r) {
  if (call_rc) {
    /* no connection or a timeout: the bitrate steps down (not when the client stopped its calls or has no key) */
    if (call_rc == TF__ERR_NETWORK) ladder_failed(s);
    retry(it, "offline", 0, 0);
    return ANS_OFFLINE;
  }
  if (r->status == 408 || r->status >= 500) ladder_failed(s);
  if (r->status >= 200 && r->status < 300) {
    TfJson *j = tf_json_parse(r->body, r->body_n, NULL);
    const char *rid = tf_json_str(tf_json_get(j, "id"));
    const TfJson *us = tf_json_get(j, "urls");
    int got = 0;
    if (rid && !strcmp(rid, id) && us && us->type == TJ_ARR)
      for (int i = 0; i < us->n; ++i) {
        const TfJson *u = &us->kids[i];
        const TfJson *n = tf_json_get(u, "n"), *e = tf_json_get(u, "expires_at");
        const char *url = tf_json_str(tf_json_get(u, "url"));
        if (n && n->type == TJ_NUM && n->num > 0 && url && e && e->type == TJ_NUM) {
          item_add_url(it, (int)n->num, url, (long long)e->num);
          got++;
        }
      }
    tf_json_free(j);
    if (got) return ANS_URLS;
    retry(it, "no upload URLs in the answer", 0, 0);
    return ANS_WAIT;
  }
  char code[64] = "";
  TfJson *j = tf_json_parse(r->body ? r->body : "", r->body_n, NULL);
  const char *e = tf_json_str(tf_json_get(j, "error"));
  snprintf(code, sizeof code, "%s", e ? e : "");
  tf_json_free(j);
  char why[160];
  snprintf(why, sizeof why, "HTTP %d %s", r->status, code);
  if (!strcmp(code, "contributor_revoked") || !strcmp(code, "contributor_key_mismatch")) {
    /* this contributor id can never upload again: a new contributor, device key and recovery code at the next
     * session; this session (recorded under the old id) is dropped */
    tf__contributor_forget(tf__client_dir(c));
    remove_session(s, id);
    return ANS_GONE;
  }
  if (!strcmp(code, "volume_limit") || !strcmp(code, "capped_limit")) {
    snprintf(it->error, sizeof it->error, "%.120s: retry tomorrow", why);
    it->next_at = (now_s() / 86400 + 1) * 86400 + (long long)(frand() * 3600);
    return ANS_WAIT;
  }
  if (!strcmp(code, "segment_cap")) {
    remove_session(s, id); /* the capped tier's last allowed segment went: what arrived is valid audio */
    return ANS_GONE;
  }
  if (r->status == 404 || r->status == 410) {
    remove_session(s, id);
    return ANS_GONE;
  }
  if (r->status == 401) retry(it, why, it->attempts < 3, 0);
  else if (r->status >= 500 || r->status == 408 || r->status == 425 || r->status == 429) retry(it, why, 0, 0);
  else if (!strcmp(code, "contributions_disabled") || !strcmp(code, "attestation_required")) retry(it, why, 0, 1);
  else {
    remove_session(s, id); /* a refused body (an SDK bug) is never retried */
    return ANS_GONE;
  }
  return ANS_WAIT;
}

static int fetch_urls(TfClient *c, Share *s, const char *id, Item *it, int from) {
  char path[128], hdr[80], contributor[33];
  if (tf__contributor(tf__client_dir(c), 0, contributor, NULL, NULL)) {
    remove_session(s, id);
    return ANS_GONE;
  }
  snprintf(path, sizeof path, "/v1/uploads/%s/urls?from=%d", id, from);
  snprintf(hdr, sizeof hdr, "x-qlab-contributor: %s", contributor);
  TfHttpResponse r;
  memset(&r, 0, sizeof r);
  const int rc = tf__client_call(c, "POST", path, NULL, 1, hdr, &r);
  const int a = answer(c, s, id, it, rc, &r);
  tf_http_response_free(&r);
  return a;
}

/* one PUT of one segment, content-type audio/ogg only (signed into the URL), no credential */
static int put_segment(Share *s, const char *id, int n, const char *url, int *status) {
  char name[32];
  snprintf(name, sizeof name, "seg-%05d.opus", n);
  char *p = sess_path(s, id, name);
  const char *hdr[] = {"content-type: audio/ogg", NULL};
  TfHttpRequest q;
  memset(&q, 0, sizeof q);
  q.method = "PUT";
  q.url = url;
  q.headers = hdr;
  q.upload_file = p;
  q.timeout_s = 120;
  q.cancel = &s->cancel;
  TfHttpResponse r;
  const int rc = p ? tf_http(&q, &r) : -1;
  *status = rc ? 0 : r.status;
  if (!rc) tf_http_response_free(&r);
  free(p);
  return rc;
}

/* The take list of an uploaded recording (2.0.3): POST /v1/uploads/{id}/takes with the token and the recording's
 * own contributor, body {"takes":[...]} (takes.json). Idempotent on the server (the list replaces an earlier one),
 * so every retry sends the same body. 2xx: done. 404 (a server without the route, or one that does not know the
 * upload): the list is dropped, logged once per client. 400: dropped, and the bad-request issue. 410 (the upload
 * was deleted): dropped. 401: a new token, then again (at once, a few times). Anything else (429, 5xx, no
 * connection): again later, with the outbox's backoff. The session itself is uploaded whatever happens here, and
 * the bitrate ladder never hears of this call. Returns 1 when the pass should stop (no connection). */
static int step_takes(TfClient *c, Share *s, const char *id, Item *it) {
  if (now_s() - it->created > MAX_AGE_S) { /* a list that could not be sent for 7 days */
    remove_session(s, id);
    return 0;
  }
  if (it->next_at > now_s()) return 0;
  char *body = takes_body(s, id, -1, NULL);
  if (!body || !it->contributor[0]) {
    free(body);
    remove_session(s, id);
    return 0;
  }
  char path[96], hdr[80];
  snprintf(path, sizeof path, "/v1/uploads/%s/takes", id);
  snprintf(hdr, sizeof hdr, "x-qlab-contributor: %s", it->contributor);
  it->attempts++;
  TfHttpResponse r;
  memset(&r, 0, sizeof r);
  const int rc = tf__client_call(c, "POST", path, body, 1, hdr, &r);
  free(body);
  if (rc) {
    retry(it, "takes: offline", 0, 0);
    item_save(s, id, it);
    return 1;
  }
  const int st = r.status;
  tf_http_response_free(&r);
  if (st == 400) {
    /* 3.0: a refused list is not dropped. It stays and is tried again on the outbox's backoff until the server
     * takes it, sharing goes off or the folder passes the seven days; the issue is reported once per list (kept
     * in item.json, so a restart does not report it again). */
    if (!it->refused) {
      it->refused = 1;
      tf__client_report_issue(c, "other", "bad_request:takes", NULL);
    }
    retry(it, "takes: HTTP 400", 0, 0);
    item_save(s, id, it);
    return 0;
  }
  if ((st >= 200 && st < 300) || st == 404 || st == 410) {
    if (st == 404 && !s->takes_404) {
      s->takes_404 = 1;
      tf__client_log(c, TF_LOG_INFO, "the server has no take list for this upload (HTTP 404): the take boundaries are not sent");
    }
    remove_session(s, id);
    return 0;
  }
  char why[48];
  snprintf(why, sizeof why, "takes: HTTP %d", st);
  if (st == 401) tf__client_token_forget(c);
  retry(it, why, st == 401 && it->attempts < 3, 0);
  item_save(s, id, it);
  return 0;
}
/* every file of a session folder but item.json and takes.json */
static void keep_item_and_takes(const Share *s, const char *id) {
  char *d = sess_path(s, id, NULL);
  if (!d) return;
  char names[256][64];
  for (int round = 0; round < 64; ++round) {
    const int n = list_dir(d, names, 256);
    int removed = 0;
    for (int i = 0; i < n; ++i) {
      if (!strcmp(names[i], "item.json") || !strcmp(names[i], "takes.json")) continue;
      char *f = tf_path_join(d, names[i]);
      if (f) removed += remove(f) == 0;
      free(f);
    }
    if (!removed) break;
  }
  free(d);
}

/* moves one session on as far as it goes now; 0 continue with the next session, 1 stop the pass (offline) */
static int step(TfClient *c, Share *s, const char *id) {
  tf_mutex_lock(&s->m);
  const int active = is_active(s, id);
  tf_mutex_unlock(&s->m);
  {
    /* an uploaded recording whose take list is still to send (2.0.3) */
    Item list;
    item_load(s, id, &list);
    const int waiting = list.uploaded;
    const int stop_now = waiting ? step_takes(c, s, id, &list) : 0;
    item_free(&list);
    if (waiting) return stop_now;
  }
  TfJson *cap = read_json(s, id, "capture.json");
  if (!cap && !active) {
    SLEEP_SHORT(); /* a replace in progress on Windows can fail a read for a moment */
    cap = read_json(s, id, "capture.json");
  }
  if (!cap) {
    if (!active) {
      char *p = sess_path(s, id, "session.json");
      const int has_session = p && tf_file_exists(p);
      free(p);
      if (!has_session) remove_session(s, id); /* a folder with no session at all */
    }
    return 0;
  }
  const long long created = tf_json_get(cap, "created") ? (long long)tf_json_get(cap, "created")->num : now_s();
  const TfJson *ev = tf_json_get(cap, "ended");
  int ended = ev && ev->type == TJ_BOOL && ev->boolean;
  if (!ended && !active) { /* the capture died (crash, restart): close the session at its last whole page */
    recover(s, id, cap);
    tf_json_free(cap);
    cap = read_json(s, id, "capture.json");
    if (!cap) return 0;
    ended = 1;
  }
  const int closed = tf_json_get(cap, "closed") ? (int)tf_json_get(cap, "closed")->num : 0;
  tf_json_free(cap);
  if (now_s() - created > MAX_AGE_S) { /* unsent sessions older than 7 days are deleted */
    remove_session(s, id);
    return 0;
  }
  Item it;
  item_load(s, id, &it);
  int stop = 0;
  if (it.next_at > now_s() || !closed) goto out;
  if (!it.started) {
    char *body = NULL;
    size_t bn;
    char *p = sess_path(s, id, "session.json");
    const int rd = p ? tf_read_file(p, &body, &bn) : -1;
    free(p);
    if (rd) {
      remove_session(s, id);
      item_free(&it);
      return 0;
    }
    char hdr[64];
    snprintf(hdr, sizeof hdr, "x-qlab-id: %s", id);
    it.attempts++;
    TfHttpResponse r;
    memset(&r, 0, sizeof r);
    const int rc = tf__client_call(c, "POST", "/v1/uploads", body, 1, hdr, &r);
    free(body);
    const int a = answer(c, s, id, &it, rc, &r);
    tf_http_response_free(&r);
    if (a == ANS_GONE) {
      item_free(&it);
      return 0;
    }
    if (a != ANS_URLS) {
      stop = a == ANS_OFFLINE;
      goto out;
    }
    it.started = 1;
    it.attempts = 0;
    item_save(s, id, &it);
  }
  for (int n = 1; n <= closed; ++n) {
    if (item_done(&it, n)) continue;
    if (s->cancel || !tf__client_sharing_state(c)) goto out;
    const Url *u = item_url(&it, n);
    if (!u || u->exp - now_s() <= URL_MARGIN_S) {
      const int a = fetch_urls(c, s, id, &it, n);
      if (a == ANS_GONE) {
        item_free(&it);
        return 0;
      }
      if (a != ANS_URLS) {
        stop = a == ANS_OFFLINE;
        goto out;
      }
      u = item_url(&it, n);
      if (!u) {
        retry(&it, "no URL for the segment", 0, 0);
        goto out;
      }
    }
    char *url = (char *)malloc(strlen(u->url) + 1);
    if (!url) goto out;
    strcpy(url, u->url);
    int status = 0;
    const int rc = put_segment(s, id, n, url, &status);
    free(url);
    if (s->cancel || !tf__client_sharing_state(c)) goto out; /* sharing went off during the PUT */
    if (rc) {
      ladder_failed(s); /* no connection, a timeout or a transport error */
      retry(&it, "PUT: offline", 0, 0);
      stop = 1;
      goto out;
    }
    if (status >= 200 && status < 300) {
      item_mark(&it, n);
      item_drop_url(&it, n);
      it.error[0] = 0;
      it.attempts = 0;
      item_save(s, id, &it);
      char name[32];
      snprintf(name, sizeof name, "seg-%05d.opus", n);
      char *p = sess_path(s, id, name);
      if (p) remove(p);
      free(p);
      ladder_ok(s, backlog_s(s));
      continue;
    }
    if (status == 408 || status >= 500) ladder_failed(s); /* 403 (an expired URL) and other refusals are not the network */
    it.attempts++;
    char why[64];
    snprintf(why, sizeof why, "PUT seg %d: HTTP %d", n, status);
    if (status == 403) { /* an expired or wrong URL: ask again from this segment (at once, a few times) */
      item_drop_url(&it, n);
      if (it.attempts < 5) {
        item_save(s, id, &it);
        n--; /* the same segment again, with a fresh URL */
        continue;
      }
    }
    retry(&it, why, 0, 0);
    goto out;
  }
  /* while the session runs: the next batch once 5 unused URLs remain */
  if (!ended) {
    int last = 0;
    for (int i = 0; i < it.n_urls; ++i)
      if (it.urls[i].n > last) last = it.urls[i].n;
    if (last > 0 && last - closed <= 5) {
      const int a = fetch_urls(c, s, id, &it, last + 1);
      if (a == ANS_GONE) {
        item_free(&it);
        return 0;
      }
      if (a == ANS_OFFLINE) stop = 1;
    }
    goto out;
  }
  {
    int all = 1;
    for (int n = 1; n <= closed; ++n) all = all && item_done(&it, n);
    if (all) { /* uploaded: nothing is kept, except a take list still to send (2.0.3) */
      char *list = takes_body(s, id, -1, NULL);
      TfJson *sj = list ? read_json(s, id, "session.json") : NULL;
      const char *ctr = tf_json_str(tf_json_get(sj, "contributor"));
      if (!list || !ctr || !is_id(ctr)) { /* one take (or a list that does not read): the folder goes */
        tf_json_free(sj);
        free(list);
        item_free(&it);
        remove_session(s, id);
        tf__sharing_uploaded(c);
        return 0;
      }
      free(list);
      /* the session is uploaded now, whatever becomes of the list: item.json says so first (a crash after this
       * line sends the list at the next launch), then the session's other files go and it is counted */
      it.uploaded = 1;
      it.created = created;
      snprintf(it.contributor, sizeof it.contributor, "%s", ctr);
      tf_json_free(sj);
      it.attempts = 0;
      it.next_at = 0;
      it.error[0] = 0;
      while (it.n_urls) item_drop_url(&it, it.urls[0].n);
      item_save(s, id, &it);
      keep_item_and_takes(s, id);
      tf__sharing_uploaded(c);
      stop = step_takes(c, s, id, &it);
      item_free(&it);
      return stop;
    }
  }
out:
  item_save(s, id, &it);
  item_free(&it);
  return stop;
}

/* one upload pass over every session, oldest first; returns the next time anything is due (unix s), 0 none */
static long long pass(TfClient *c, Share *s) {
  char names[256][64];
  const int n = list_dir(s->outbox, names, 256);
  /* oldest first by capture time */
  long long created[256];
  for (int i = 0; i < n; ++i) {
    created[i] = 0;
    if (!is_id(names[i])) continue;
    TfJson *cap = read_json(s, names[i], "capture.json");
    created[i] = cap && tf_json_get(cap, "created") ? (long long)tf_json_get(cap, "created")->num : 0;
    tf_json_free(cap);
  }
  for (int i = 1; i < n; ++i)
    for (int k = i; k > 0 && created[k] < created[k - 1]; --k) {
      char t[64];
      memcpy(t, names[k], 64);
      memcpy(names[k], names[k - 1], 64);
      memcpy(names[k - 1], t, 64);
      const long long x = created[k];
      created[k] = created[k - 1];
      created[k - 1] = x;
    }
  const int may = tf__client_sharing_state(c) && tf__sharing_may_upload(c);
  for (int i = 0; i < n; ++i) {
    if (!is_id(names[i])) continue;
    if (!may) break;
    if (s->cancel || s->quit) break;
    if (step(c, s, names[i])) break;
  }
  long long next = 0;
  for (int i = 0; i < n; ++i) {
    if (!is_id(names[i])) continue;
    Item it;
    item_load(s, names[i], &it);
    if (it.next_at && (!next || it.next_at < next)) next = it.next_at;
    item_free(&it);
  }
  return next;
}

static void share_thread(void *arg) {
  TfClient *c = (TfClient *)arg;
  Share *s = sh(c);
  tf_mutex_lock(&s->m);
  for (;;) {
    if (s->quit) break;
    s->wake = 0;
    s->cancel = 0;
    s->in_pass = 1;
    tf_mutex_unlock(&s->m);
    const long long next = pass(c, s);
    tf_mutex_lock(&s->m);
    s->in_pass = 0;
    s->passes++;
    tf_cond_signal(&s->cv);
    if (s->quit) break;
    if (s->wake) continue;
    long ms = 300000;
    if (next) {
      const long long d = next - now_s();
      ms = d <= 0 ? 1000 : d * 1000 > 300000 ? 300000 : (long)(d * 1000);
    }
    tf_cond_wait_ms(&s->cv, &s->m, ms);
  }
  tf_mutex_unlock(&s->m);
}

/* ------------------------------------------------------------------ public */
int tf__sharing_outbox_resume(TfClient *c) {
  Share *s = sh(c);
  if (!s) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  tf_mkdirs(s->outbox);
  tf_mutex_lock(&s->m);
  if (!s->thread_on) {
    if (tf_thread_start(&s->th, share_thread, c)) {
      tf_mutex_unlock(&s->m);
      return tf__fail(TF__ERR_NOT_AVAILABLE, "cannot start the uploader thread");
    }
    s->thread_on = 1;
  }
  s->wake = 1;
  tf_cond_signal(&s->cv);
  tf_mutex_unlock(&s->m);
  return 0;
}

/* an uploaded recording that only waits for its take list to go (2.0.3) */
static int list_waiting(const Share *s, const char *id) {
  TfJson *j = read_json(s, id, "item.json");
  const TfJson *d = tf_json_get(j, "done");
  const int v = d && d->type == TJ_BOOL && d->boolean;
  tf_json_free(j);
  return v;
}

int tf__sharing_outbox_pending(TfClient *c) {
  Share *s = sh(c);
  if (!s) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  char names[256][64];
  const int n = list_dir(s->outbox, names, 256);
  int k = 0;
  for (int i = 0; i < n; ++i) k += is_id(names[i]) && !list_waiting(s, names[i]);
  return k;
}
/* tests: upload passes finished so far */
int tf__share_passes(TfClient *c) {
  Share *s = sh(c);
  if (!s) return 0;
  tf_mutex_lock(&s->m);
  const int n = s->passes;
  tf_mutex_unlock(&s->m);
  return n;
}
/* tests: uploaded recordings whose take list is still to send */
int tf__share_lists_waiting(TfClient *c) {
  Share *s = sh(c);
  if (!s) return 0;
  char names[256][64];
  const int n = list_dir(s->outbox, names, 256);
  int k = 0;
  for (int i = 0; i < n; ++i) k += is_id(names[i]) && list_waiting(s, names[i]);
  return k;
}

/* tests and apps that want to know: waits until the uploader has finished a pass that started after this call
 * (at most timeout_ms); returns the sessions still pending */
int tf__sharing_outbox_wait(TfClient *c, int timeout_ms) {
  Share *s = sh(c);
  if (!s) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  if (tf__sharing_outbox_resume(c)) return tf__error_code();
  tf_mutex_lock(&s->m);
  const int target = s->passes + 2; /* the pass running now may have started before the call */
  const double t0 = tf__now_s();
  while (s->passes < target && (tf__now_s() - t0) * 1000 < timeout_ms) tf_cond_wait_ms(&s->cv, &s->m, 200);
  tf_mutex_unlock(&s->m);
  return tf__sharing_outbox_pending(c);
}

/* Since 2.0.1 the adaptive ladder decides every segment's bitrate (the same in every SDK); the call stays for
 * binary compatibility, checks its argument and changes nothing. */
int tf__sharing_set_bitrate(TfClient *c, int bits_per_second) {
  Share *s = sh(c);
  if (!s) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  if (bits_per_second < 6000 || bits_per_second > 510000) return tf__fail(TF_ERR_INVALID_ARGUMENT, "Opus bitrates are 6000 to 510000 bit/s");
  return 0;
}

/* tests: one ladder event (-1 a fresh ladder, 0 upload_failed, 1 upload_ok, 2 segment_closed); returns the bitrate
 * the next segment opens with */
int tf__ladder_event(TfClient *c, int event, double backlog_s) {
  Share *s = sh(c);
  if (!s) return -1;
  if (event == -1) {
    tf_mutex_lock(&s->m);
    s->rung = s->streak = 0;
    tf_mutex_unlock(&s->m);
  } else if (event == 0) {
    ladder_failed(s);
  } else if (event == 1) {
    ladder_ok(s, backlog_s);
  } else if (event == 2) {
    ladder_closed(s, backlog_s);
  }
  return ladder_bitrate(s);
}

int tf__sharing_bitrate(TfClient *c) {
  Share *s = sh(c);
  if (!s) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  return ladder_bitrate(s);
}

/* the outbox's items, oldest first: [{"id","created","ended","completed","segments","uploaded","segmentBitrates":
 * [...],"takes","error"}] (segmentBitrates: one per closed segment, in segment order; takes: how many takes the
 * recording holds so far, 1 for a single take) */
char *tf__sharing_outbox_json(TfClient *c) {
  Share *s = sh(c);
  if (!s) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
    return NULL;
  }
  char names[256][64];
  const int n = list_dir(s->outbox, names, 256);
  long long created[256];
  for (int i = 0; i < n; ++i) {
    TfJson *cap = is_id(names[i]) ? read_json(s, names[i], "capture.json") : NULL;
    created[i] = cap && tf_json_get(cap, "created") ? (long long)tf_json_get(cap, "created")->num : 0;
    tf_json_free(cap);
  }
  int order[256];
  for (int i = 0; i < n; ++i) order[i] = i;
  for (int i = 1; i < n; ++i)
    for (int k = i; k > 0 && created[order[k]] < created[order[k - 1]]; --k) {
      const int t = order[k];
      order[k] = order[k - 1];
      order[k - 1] = t;
    }
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "[");
  int first = 1;
  for (int oi = 0; oi < n; ++oi) {
    const char *id = names[order[oi]];
    if (!is_id(id)) continue;
    TfJson *cap = read_json(s, id, "capture.json");
    if (!cap) continue;
    Item it;
    item_load(s, id, &it);
    const TfJson *cl = tf_json_get(cap, "closed"), *en = tf_json_get(cap, "ended"), *co = tf_json_get(cap, "completed");
    const int closed = cl && cl->type == TJ_NUM ? (int)cl->num : 0;
    int up = 0;
    for (int k = 1; k <= closed; ++k) up += item_done(&it, k);
    tf_buf_printf(&b, "%s{\"id\":\"%s\",\"created\":%lld,\"ended\":%s,\"completed\":%s,\"segments\":%d,\"uploaded\":%d,\"segmentBitrates\":[",
                  first ? "" : ",", id, created[order[oi]], en && en->type == TJ_BOOL && en->boolean ? "true" : "false",
                  co && co->type == TJ_BOOL && co->boolean ? "true" : "false", closed, up);
    for (int k = 1; k <= closed; ++k) {
      int br = 0;
      marker_read(s, id, k, &br, NULL);
      tf_buf_printf(&b, "%s%d", k > 1 ? "," : "", br);
    }
    int n_takes = 1;
    free(takes_body(s, id, -1, &n_takes));
    tf_buf_printf(&b, "],\"takes\":%d,\"error\":", n_takes);
    tf_buf_json_str(&b, it.error);
    tf_buf_puts(&b, "}");
    first = 0;
    item_free(&it);
    tf_json_free(cap);
  }
  tf_buf_puts(&b, "]");
  return tf_buf_take(&b);
}

/* sharing went off (or what was shared was deleted): the open recording ends at once (also between two takes,
 * when no push would notice), the PUT in flight is aborted and every session not being captured is deleted, a
 * take list waiting to be sent with them (a take that records on its own sees consent gone at its next push and
 * deletes its own) */
void tf__share_wipe(TfClient *c) {
  Share *s = sh(c);
  if (!s) return;
  recorder_stop(s);
  /* The uploader may be in the middle of a PUT: it holds the segment file open (Windows cannot delete an open
   * file) and writes the session's state when it leaves. Deleting under its feet could leave the recording on
   * disk after the learner turned sharing off. So: abort under the uploader's lock (its own reset of the flag
   * cannot lose the abort), wait until it has left its pass (it signals; at most 3 s), then delete. A new pass
   * cannot start while this holds the lock, and the next one sees that sharing is off. */
  tf_mutex_lock(&s->m);
  tf__http_cancel(&s->cancel);
  const double t0 = tf__now_s();
  while (s->in_pass && tf__now_s() - t0 < 3.0) {
    tf__http_cancel(&s->cancel);
    tf_cond_wait_ms(&s->cv, &s->m, 20);
  }
  char names[256][64];
  const int n = list_dir(s->outbox, names, 256);
  for (int i = 0; i < n; ++i)
    if (is_id(names[i]) && !is_active(s, names[i])) remove_session(s, names[i]);
  tf_mutex_unlock(&s->m);
}

/* bytes of the sharing queue on disk */
long long tf__share_bytes(TfClient *c) {
  char *p = tf_path_join(tf__client_dir(c), "outbox");
  const long long n = p ? tf__dir_bytes(p) : 0;
  free(p);
  return n;
}

/* The feed (vectors README 10.1): audio the app captures outside takes. A call's time is the wall time of the
 * call, taken as the end of its audio.
 *   F1  without the learner's yes, or with sharing off or not offered: dropped at once, and the ring is empty
 *   F2  during a take: dropped
 *   F3  the feed continues when the call comes no later than its own audio plus 500 ms after the audio before
 *       it (the previous call, or the end of the take); else it paused: the ring is emptied, an open tail ends
 *   F4  the tail: after a take, the first continuing samples go to its recording, up to 0.5 s, at its rate
 *   F5  everything else goes to the ring: the last second, one rate
 * The model never sees any of it. */
int tf_sharing_feed(TfClient *c, const float *samples, int n, int sample_rate) {
  if (!c) return tf__raise(tf__null_handle("client"));
  tf__clear_error();
  if (n < 0 || (n > 0 && !samples) || sample_rate < 8000 || sample_rate > 192000)
    return tf__raise(tf__fail(TF_ERR_INVALID_ARGUMENT, "the feed takes mono float samples at a rate from 8000 to 192000"));
  Share *s = sh(c);
  if (!s) return 0;
  tf_mutex_lock(&s->rm);
  Rec *o = s->open;
  s->st_tail = s->st_ring = 0;
  s->st_dropped = n;
  if (!tf__client_sharing_state(c) || !tf__sharing_may_upload(c)) {
    ring_empty(s);
  } else if (!(o && o->take)) {
    const long long now = now_ms(), dur = 1000LL * n / sample_rate;
    const int cont = s->last_audio && now - s->last_audio <= dur + TF_SHARING_FEED_STALE_MS;
    if (!cont) {
      ring_empty(s);
      if (o) o->tail_left = 0;
    }
    if (o && o->tail_left > 0 && now - o->last_end > TF_SHARING_TAIL_WAIT_MS) o->tail_left = 0; /* F8 */
    long long to_tail = 0;
    if (o && o->tail_left > 0) {
      const long long room = cap_samples(o) - o->samples; /* never past the recording's cap */
      if (sample_rate == o->input_rate && room > 0 && !o->failed) {
        to_tail = n < o->tail_left ? n : o->tail_left;
        if (to_tail > room) to_tail = room;
        o->tail_left -= to_tail;
        rec_encode(o, samples, to_tail);
        if (o->samples >= cap_samples(o)) { /* the tail filled the recording: it closes at the cap */
          rec_close(s, o);
          o = NULL;
        }
      } else {
        o->tail_left = 0;
      }
    }
    const long long rest = n - to_tail;
    if (rest) {
      ring_put(s, samples + to_tail, rest, sample_rate);
      s->ring_wall = now;
    }
    s->last_audio = now;
    s->st_tail = to_tail;
    s->st_ring = rest;
    s->st_dropped = 0;
    /* a recording whose window is over and whose tail is complete (or ended) has nothing to wait for */
    if (o && o->tail_left == 0 && now - o->last_end > join_window(c)) rec_close(s, o);
  }
  tf_mutex_unlock(&s->rm);
  return 0;
}

void tf__share_feed_stats(TfClient *c, long long *to_tail, long long *to_ring, long long *dropped, long long *ring_samples,
                          long long *ring_bytes, long long *pre_roll) {
  Share *s = sh(c);
  if (!s) return;
  tf_mutex_lock(&s->rm);
  if (to_tail) *to_tail = s->st_tail;
  if (to_ring) *to_ring = s->st_ring;
  if (dropped) *dropped = s->st_dropped;
  if (ring_samples) *ring_samples = s->ring_n;
  if (ring_bytes) *ring_bytes = s->ring ? (long long)s->ring_cap * (long long)sizeof(float) : 0;
  if (pre_roll) *pre_roll = s->st_pre;
  tf_mutex_unlock(&s->rm);
}
