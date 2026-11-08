/* Whole-session recitation sharing for desktop (PLATFORM_CLIENT.md sections 8 to 10, the 2.0.0 design): every session
 * the SDK listens to while the learner's consent is on is encoded as one Ogg Opus stream, cut into segments of
 * 10, 20, 40, then 60 s at page boundaries, and uploaded in the background with presigned PUTs straight to R2, the
 * same wire as the Swift reference (Cloud/Contributions.swift, Cloud/Outbox.swift) and the web SDK (web/sharing).
 *
 * On disk, one folder per session under <storage>/outbox/<id>/:
 *   session.json   the POST /v1/uploads body (closed schema, sorted keys, contributor_key and recovery_hash)
 *   capture.json   written by the capture: segments closed, ended, completed, duration, created (unix s)
 *   item.json      written by the uploader: started, URLs with expiry, segments uploaded, attempts, next attempt
 *   seg-NNNNN.opus the segment files (each deleted once its PUT succeeded)
 * A crash or restart leaves the folders; the next launch resumes them (a session whose capture died is closed at
 * its last whole page). Sessions older than 7 days are deleted unsent. Turning sharing off deletes them at once and
 * aborts the PUT in flight.
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
const char *tf__sharing_consent_version(TfClient *c);
int tf__contributor(const char *dir, int create, char id[33], char contributor_key[90], char recovery_hash[65]);
void tf__contributor_forget(const char *dir);
extern long long tf__now_override;
TfShareSession *tf__share_begin(TfClient *c, int input_sample_rate, const char *input_route, const char *revision);

#define MAX_AGE_S (7LL * 86400)
#define URL_MARGIN_S 300
#define MIN_PARTIAL_S 2.0
#ifdef _WIN32
#define SLEEP_SHORT() Sleep(20)
#else
#define SLEEP_SHORT() usleep(20000)
#endif

static long long now_s(void) { return tf__now_override ? tf__now_override : (long long)time(NULL); }

/* ------------------------------------------------------------------ per-client state */
typedef struct {
  TfMutex m;
  TfCond cv;
  TfThread th;
  int thread_on, quit, wake;
  volatile int cancel;
  int bitrate;
  char active[16][33]; /* sessions being captured in this process */
  int n_active;
  int passes;          /* finished upload passes (tests) */
  char outbox[1024];
} Share;

static Share *sh(TfClient *c) { return c ? (Share *)*tf__client_share_slot(c) : NULL; }

static void share_thread(void *arg);

void tf__share_init(TfClient *c) {
  Share *s = (Share *)calloc(1, sizeof *s);
  if (!s) return;
  tf_mutex_init(&s->m);
  tf_cond_init(&s->cv);
  s->bitrate = TF_SHARING_BITRATE_DEFAULT;
  char *p = tf_path_join(tf__client_dir(c), "outbox");
  if (p) snprintf(s->outbox, sizeof s->outbox, "%s", p);
  free(p);
  *tf__client_share_slot(c) = s;
}
void tf__share_free(TfClient *c) {
  Share *s = sh(c);
  if (!s) return;
  if (s->thread_on) {
    tf_mutex_lock(&s->m);
    s->quit = 1;
    tf_cond_signal(&s->cv);
    tf_mutex_unlock(&s->m);
    tf__http_cancel(&s->cancel);
    tf_thread_join(s->th);
  }
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
  const int n = list_dir(d, names, 256);
  for (int i = 0; i < n; ++i) {
    char *f = tf_path_join(d, names[i]);
    if (f) remove(f);
    free(f);
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

/* ------------------------------------------------------------------ the capture */
struct TfShareSession {
  TfClient *c;
  char id[33];
  int input_rate, resample;
  TfResampler rs;
  TfEncoder enc;
  TfOgg ogg;
  int opened, failed, closed, finished;
  long long samples, created;
};

static void capture_save(TfShareSession *x, int ended, int completed) {
  char t[256];
  snprintf(t, sizeof t, "{\"v\":1,\"created\":%lld,\"closed\":%d,\"ended\":%s,\"completed\":%s,\"duration_ms\":%lld}", x->created, x->closed,
           ended ? "true" : "false", completed ? "true" : "false", x->samples * 1000 / (x->input_rate ? x->input_rate : 48000));
  write_text(sh(x->c), x->id, "capture.json", t);
}
static void on_segment(void *user, int n, int last) {
  TfShareSession *x = (TfShareSession *)user;
  (void)last;
  x->closed = n;
  capture_save(x, 0, 0);
  wake(sh(x->c));
}
static int put_packet(void *user, const uint8_t *p, int n) { return tf_ogg_packet(&((TfShareSession *)user)->ogg, p, n); }
static int put_samples(void *user, const float *x, int n) {
  TfShareSession *s = (TfShareSession *)user;
  return tf_enc_encode(&s->enc, x, n, put_packet, s);
}

/* the session JSON, sorted keys (as the Swift and web SDKs write it) */
static char *session_json(TfClient *c, const char *contributor, const char *key, const char *rhash, int input_rate, const char *route,
                          const char *revision) {
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
  const char *cv = tf__sharing_consent_version(c);
  if (cv && *cv) {
    tf_buf_puts(&b, ",\"consent_version\":");
    tf_buf_json_str(&b, cv);
  }
  tf_buf_printf(&b, ",\"contributor\":\"%s\",\"contributor_key\":\"%s\",\"day\":\"%s\"", contributor, key, day);
  tf_buf_puts(&b, ",\"decode\":{\"backend\":\"exact\",\"model_revision\":");
  if (revision && *revision) tf_buf_json_str(&b, revision);
  else tf_buf_puts(&b, "null");
  tf_buf_printf(&b, ",\"sdk_version\":\"%s\",\"weights_sha256\":null}", TF_SDK_VERSION);
  tf_buf_printf(&b, ",\"device_class\":\"x86_64\",\"format\":1,\"os_major\":%d,\"recovery_hash\":\"%s\"}", tf__client_os_major(), rhash);
  return tf_buf_take(&b);
}

static const char *const kRoutes[] = {"built_in", "wired", "usb", "bluetooth", "car", "continuity", "other", "unknown", NULL};

TfShareSession *tf_sharing_session_begin(TfClient *c, int input_sample_rate, const char *input_route) {
  return tf__share_begin(c, input_sample_rate, input_route, NULL);
}

TfShareSession *tf__share_begin(TfClient *c, int input_sample_rate, const char *input_route, const char *revision) {
  tf__clear_error();
  Share *s = sh(c);
  if (!s) {
    tf__fail(TF_ERR_ARGUMENT, "no client");
    return NULL;
  }
  if (!tf_client_sharing_state(c) || !tf__sharing_may_upload(c)) {
    tf__fail(TF_ERR_NOT_AVAILABLE, "sharing is off (consent, the contributions option, flags.contributions or upload_trust)");
    return NULL;
  }
  if (input_sample_rate < 8000 || input_sample_rate > 192000) {
    tf__fail(TF_ERR_ARGUMENT, "input sample rate %d is not 8000 to 192000", input_sample_rate);
    return NULL;
  }
  const char *route = "unknown";
  for (int i = 0; input_route && kRoutes[i]; ++i)
    if (!strcmp(input_route, kRoutes[i])) route = kRoutes[i];
  char contributor[33], key[90], rhash[65];
  if (tf__contributor(tf__client_dir(c), 1, contributor, key, rhash)) {
    tf__fail(TF_ERR_NOT_AVAILABLE, "the device key cannot be created");
    return NULL;
  }
  TfShareSession *x = (TfShareSession *)calloc(1, sizeof *x);
  if (!x) {
    tf__fail(TF_ERR_NO_MEMORY, "out of memory");
    return NULL;
  }
  uint8_t r[16];
  tf_random_bytes(r, sizeof r);
  tf_hex_encode(r, 16, x->id);
  x->c = c;
  x->input_rate = input_sample_rate;
  x->resample = input_sample_rate != 48000;
  x->created = now_s();
  char *dir = sess_path(s, x->id, NULL);
  char *body = session_json(c, contributor, key, rhash, input_sample_rate, route, revision);
  if (!dir || !body || tf_mkdirs(dir) || write_text(s, x->id, "session.json", body)) {
    free(dir);
    free(body);
    free(x);
    tf__fail(TF_ERR_IO, "cannot write the sharing outbox");
    return NULL;
  }
  free(body);
  set_active(s, x->id, 1);
  capture_save(x, 0, 0);
  int bitrate;
  tf_mutex_lock(&s->m);
  bitrate = s->bitrate;
  tf_mutex_unlock(&s->m);
  uint32_t serial;
  tf_random_bytes((uint8_t *)&serial, sizeof serial);
  char vendor[96];
  snprintf(vendor, sizeof vendor, "QuranLab SDK %s %s", TF_SDK_VERSION, tf_opus_version());
  if ((x->resample && tf_rs_init(&x->rs, input_sample_rate)) || tf_enc_init(&x->enc, bitrate) ||
      tf_ogg_open(&x->ogg, dir, serial, x->enc.pre_skip, input_sample_rate, vendor)) {
    free(dir);
    x->failed = 1;
    tf_sharing_session_finish(x, 0);
    tf__fail(TF_ERR_ENGINE, "the Opus encoder or the Ogg writer could not start");
    return NULL;
  }
  free(dir);
  x->ogg.on_segment = on_segment;
  x->ogg.user = x;
  x->opened = 1;
  return x;
}

int tf_sharing_session_push(TfShareSession *x, const float *pcm, int n) {
  if (!x || n < 0 || (n && !pcm)) return tf__fail(TF_ERR_ARGUMENT, "bad arguments");
  if (x->failed || !n) return 0;
  /* consent withdrawn mid-session: nothing more is written; finish() deletes what is unsent */
  if (!tf_client_sharing_state(x->c)) {
    x->failed = 1;
    return 0;
  }
  x->samples += n;
  const int rc = x->resample ? tf_rs_process(&x->rs, pcm, n, put_samples, x) : tf_enc_encode(&x->enc, pcm, n, put_packet, x);
  if (rc) {
    x->failed = 1;
    return tf__fail(TF_ERR_IO, "the shared session could not be written");
  }
  return 0;
}

/* the end of the session: the last segment and capture.json; a failed, withdrawn, or cancelled session (completed
 * 0) shorter than 2 s with no segment closed is deleted. Returns the milliseconds shared (0: nothing). */
int tf_sharing_session_finish(TfShareSession *x, int completed) {
  if (!x) return 0;
  Share *s = sh(x->c);
  int ms = 0;
  const double dur = x->input_rate ? (double)x->samples / x->input_rate : 0;
  const int keep = x->opened && !x->failed && tf_client_sharing_state(x->c) && dur > 0 && (completed || dur >= MIN_PARTIAL_S || x->closed > 0);
  if (keep) {
    int rc = x->resample ? tf_rs_flush(&x->rs, put_samples, x) : 0;
    if (!rc) rc = tf_enc_flush(&x->enc, put_packet, x);
    if (!rc) rc = tf_ogg_finish(&x->ogg, tf_enc_final_granule(&x->enc));
    if (!rc) {
      capture_save(x, 1, completed);
      ms = (int)(dur * 1000 + 0.5);
    } else {
      tf_ogg_abandon(&x->ogg);
      remove_session(s, x->id);
    }
  } else {
    if (x->opened) tf_ogg_abandon(&x->ogg);
    if (x->closed > 0 && tf_client_sharing_state(x->c) && !x->failed) {
      capture_save(x, 1, 0); /* closed segments stay with the outbox */
    } else {
      remove_session(s, x->id);
    }
  }
  if (x->resample) tf_rs_free(&x->rs);
  tf_enc_free(&x->enc);
  set_active(s, x->id, 0);
  wake(s);
  free(x);
  return ms;
}

/* tests: the process "dies" mid-session (files stay as they are, nothing is finished) */
void tf__share_crash(TfShareSession *x) {
  if (!x) return;
  if (x->ogg.file) fclose(x->ogg.file);
  x->ogg.file = NULL;
  for (int i = 0; i < x->ogg.n_q; ++i) free(x->ogg.q[i]);
  if (x->resample) tf_rs_free(&x->rs);
  tf_enc_free(&x->enc);
  set_active(sh(x->c), x->id, 0);
  free(x);
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
/* bytes of whole, valid pages at the start of `d`, and how many of them carry audio (granule > 0) */
static size_t whole_pages(const uint8_t *d, size_t n, int *audio) {
  size_t at = 0;
  *audio = 0;
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
    at += len;
  }
  return at;
}
static void recover(Share *s, const char *id, const TfJson *cap) {
  const int closed = tf_json_get(cap, "closed") ? (int)tf_json_get(cap, "closed")->num : 0;
  const long long created = tf_json_get(cap, "created") ? (long long)tf_json_get(cap, "created")->num : now_s();
  char name[32];
  snprintf(name, sizeof name, "seg-%05d.opus", closed + 1);
  char *p = sess_path(s, id, name), *data = NULL;
  size_t n = 0;
  int kept = closed;
  if (p && !tf_read_file(p, &data, &n)) {
    int audio = 0;
    const size_t good = whole_pages((const uint8_t *)data, n, &audio);
    if (audio > 0) {
      tf_write_file(p, data, good);
      kept = closed + 1;
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
  char t[256];
  snprintf(t, sizeof t, "{\"v\":1,\"created\":%lld,\"closed\":%d,\"ended\":true,\"completed\":false,\"duration_ms\":0,\"recovered\":true}",
           created, kept);
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
    retry(it, "offline", 0, 0);
    return ANS_OFFLINE;
  }
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

/* moves one session on as far as it goes now; 0 continue with the next session, 1 stop the pass (offline) */
static int step(TfClient *c, Share *s, const char *id) {
  tf_mutex_lock(&s->m);
  const int active = is_active(s, id);
  tf_mutex_unlock(&s->m);
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
    if (s->cancel || !tf_client_sharing_state(c)) goto out;
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
    if (s->cancel || !tf_client_sharing_state(c)) goto out; /* sharing went off during the PUT */
    if (rc) {
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
      continue;
    }
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
    if (all) { /* uploaded: nothing is kept */
      item_free(&it);
      remove_session(s, id);
      tf__sharing_uploaded(c);
      return 0;
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
  const int may = tf_client_sharing_state(c) && tf__sharing_may_upload(c);
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
    tf_mutex_unlock(&s->m);
    const long long next = pass(c, s);
    tf_mutex_lock(&s->m);
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
int tf_sharing_outbox_resume(TfClient *c) {
  Share *s = sh(c);
  if (!s) return tf__fail(TF_ERR_ARGUMENT, "no client");
  tf_mkdirs(s->outbox);
  tf_mutex_lock(&s->m);
  if (!s->thread_on) {
    if (tf_thread_start(&s->th, share_thread, c)) {
      tf_mutex_unlock(&s->m);
      return tf__fail(TF_ERR_NOT_AVAILABLE, "cannot start the uploader thread");
    }
    s->thread_on = 1;
  }
  s->wake = 1;
  tf_cond_signal(&s->cv);
  tf_mutex_unlock(&s->m);
  return 0;
}

int tf_sharing_outbox_pending(TfClient *c) {
  Share *s = sh(c);
  if (!s) return tf__fail(TF_ERR_ARGUMENT, "no client");
  char names[256][64];
  const int n = list_dir(s->outbox, names, 256);
  int k = 0;
  for (int i = 0; i < n; ++i) k += is_id(names[i]);
  return k;
}

/* tests and apps that want to know: waits until the uploader has finished a pass that started after this call
 * (at most timeout_ms); returns the sessions still pending */
int tf_sharing_outbox_wait(TfClient *c, int timeout_ms) {
  Share *s = sh(c);
  if (!s) return tf__fail(TF_ERR_ARGUMENT, "no client");
  if (tf_sharing_outbox_resume(c)) return tf_last_error_code();
  tf_mutex_lock(&s->m);
  const int target = s->passes + 2; /* the pass running now may have started before the call */
  const double t0 = tf__now_s();
  while (s->passes < target && (tf__now_s() - t0) * 1000 < timeout_ms) tf_cond_wait_ms(&s->cv, &s->m, 200);
  tf_mutex_unlock(&s->m);
  return tf_sharing_outbox_pending(c);
}

int tf_sharing_set_bitrate(TfClient *c, int bits_per_second) {
  Share *s = sh(c);
  if (!s) return tf__fail(TF_ERR_ARGUMENT, "no client");
  if (bits_per_second < 6000 || bits_per_second > 510000) return tf__fail(TF_ERR_ARGUMENT, "Opus bitrates are 6000 to 510000 bit/s");
  tf_mutex_lock(&s->m);
  s->bitrate = bits_per_second;
  tf_mutex_unlock(&s->m);
  return 0;
}

/* sharing went off (or what was shared was deleted): the PUT in flight is aborted and every session not being
 * captured is deleted at once (a capture sees consent gone at its next push and deletes its own) */
void tf__share_wipe(TfClient *c) {
  Share *s = sh(c);
  if (!s) return;
  tf__http_cancel(&s->cancel);
  char names[256][64];
  const int n = list_dir(s->outbox, names, 256);
  tf_mutex_lock(&s->m);
  for (int i = 0; i < n; ++i)
    if (is_id(names[i]) && !is_active(s, names[i])) remove_session(s, names[i]);
  tf_mutex_unlock(&s->m);
}
