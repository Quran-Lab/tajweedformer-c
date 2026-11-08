/* Recitation sharing: the consent state machine, the embedded copy decks, the settings calls, and whether this install may upload at all
 * (PLATFORM_CLIENT.md section 9: flags.contributions and flags.upload_trust[windows|linux], default
 * "attested", which no desktop install can satisfy, so a Windows or Linux app shares nothing unless Quran Lab\1opens the "capped" tier). State: sharing.json in the client's storage directory. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "tf_internal.h"

typedef struct {
  const char *lang;
  const char *json;
} TfCopyDeck;
extern const TfCopyDeck tf__copy_decks[];

const char *tf__client_dir(const TfClient *c);
const TfClientOptions *tf__client_options(const TfClient *c);
void **tf__client_sharing_slot(TfClient *c);
int tf__client_flag(const TfClient *c, const char *name, int dflt);
void tf__client_upload_trust(const TfClient *c, char *out, size_t n);

int tf_random_bytes(uint8_t *out, size_t n) {
#ifdef _WIN32
  return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
#else
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) return -1;
  size_t got = 0;
  while (got < n) {
    const ssize_t k = read(fd, out + got, n - got);
    if (k <= 0) {
      close(fd);
      return -1;
    }
    got += (size_t)k;
  }
  close(fd);
  return 0;
#endif
}

/* ------------------------------------------------------------------ copy */
const char *tf_sharing_copy_json(const char *language) {
  const char *want = language && *language ? language : "en";
  for (int i = 0; tf__copy_decks[i].lang; ++i)
    if (!_tf_strnicmp(tf__copy_decks[i].lang, want, 2) && (want[2] == 0 || want[2] == '-' || want[2] == '_'))
      return tf__copy_decks[i].json;
  for (int i = 0; tf__copy_decks[i].lang; ++i)
    if (!strcmp(tf__copy_decks[i].lang, "en")) return tf__copy_decks[i].json;
  return NULL;
}
static void consent_version(const char *lang, char *out, size_t n) {
  const char *d = tf_sharing_copy_json(lang);
  TfJson *j = d ? tf_json_parse(d, strlen(d), NULL) : NULL;
  const char *v = tf_json_str(tf_json_get(j, "consent_version"));
  snprintf(out, n, "%s", v ? v : "");
  tf_json_free(j);
}

/* ------------------------------------------------------------------ state */
typedef struct {
  char consent[12]; /* unknown, granted, declined, final */
  int asks, last_ask_session, sessions, thanked, guardian, shared_sessions, pending_ask;
  long long decided_at;
  char consent_version[40];
} Share;

static char *state_path(const TfClient *c) { return tf_path_join(tf__client_dir(c), "sharing.json"); }

static void share_save(const TfClient *c, const Share *s) {
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "{\"v\":1,\"consent\":\"%s\",\"asks\":%d,\"lastAskSession\":%d,\"sessions\":%d,\"thanked\":%s,\"decidedAt\":", s->consent,
                s->asks, s->last_ask_session, s->sessions, s->thanked ? "true" : "false");
  if (s->decided_at) tf_buf_printf(&b, "%lld", s->decided_at);
  else tf_buf_puts(&b, "null");
  tf_buf_puts(&b, ",\"consentVersion\":");
  if (s->consent_version[0]) tf_buf_json_str(&b, s->consent_version);
  else tf_buf_puts(&b, "null");
  tf_buf_printf(&b, ",\"guardian\":%s,\"sharedSessions\":%d,\"pendingAsk\":%d}", s->guardian ? "true" : "false", s->shared_sessions,
                s->pending_ask);
  char *p = state_path(c);
  if (p && b.p) tf_write_file(p, b.p, b.n);
  free(p);
  tf_buf_free(&b);
}

void tf__sharing_init(TfClient *c) {
  Share *s = (Share *)calloc(1, sizeof *s);
  if (!s) return;
  snprintf(s->consent, sizeof s->consent, "unknown");
  char *p = state_path(c), *text = NULL;
  size_t n;
  if (p && !tf_read_file(p, &text, &n)) {
    TfJson *j = tf_json_parse(text, n, NULL);
    const char *cs = tf_json_str(tf_json_get(j, "consent"));
    if (cs && (!strcmp(cs, "unknown") || !strcmp(cs, "granted") || !strcmp(cs, "declined") || !strcmp(cs, "final")))
      snprintf(s->consent, sizeof s->consent, "%s", cs);
#define GETI(k, f)                                                \
  do {                                                            \
    const TfJson *v = tf_json_get(j, k);                          \
    if (v && v->type == TJ_NUM) s->f = (int)v->num;               \
    if (v && v->type == TJ_BOOL) s->f = v->boolean;               \
  } while (0)
    GETI("asks", asks);
    GETI("lastAskSession", last_ask_session);
    GETI("sessions", sessions);
    GETI("thanked", thanked);
    GETI("guardian", guardian);
    GETI("sharedSessions", shared_sessions);
    GETI("pendingAsk", pending_ask);
#undef GETI
    const TfJson *d = tf_json_get(j, "decidedAt");
    if (d && d->type == TJ_NUM) s->decided_at = (long long)d->num;
    const char *cv = tf_json_str(tf_json_get(j, "consentVersion"));
    if (cv) snprintf(s->consent_version, sizeof s->consent_version, "%s", cv);
    tf_json_free(j);
  }
  free(text);
  free(p);
  /* a new consent_version (new disclosure) asks again, once */
  char cv[40];
  consent_version(tf__client_options(c)->language, cv, sizeof cv);
  if (!strcmp(s->consent, "granted") && s->consent_version[0] && strcmp(s->consent_version, cv)) {
    snprintf(s->consent, sizeof s->consent, "unknown");
    s->asks = 0;
  }
  *tf__client_sharing_slot(c) = s;
}
void tf__sharing_free(TfClient *c) {
  free(*tf__client_sharing_slot(c));
  *tf__client_sharing_slot(c) = NULL;
}
static Share *share(TfClient *c) { return (Share *)*tf__client_sharing_slot(c); }

/* enabled = the app's contributions option AND the signed config's flags.contributions not false */
static int sharing_enabled(const TfClient *c) { return tf__client_options(c)->contributions && tf__client_flag(c, "contributions", 1); }

/* may this install upload (web and desktop upload at the lower trust tier): flags.upload_trust
 * [windows|linux] absent or "capped" = yes (unattested token, per-key and per-contributor caps, sessions marked
 * attested=false by the server); "off", or "attested" (which no desktop install can satisfy) = no */
int tf__sharing_may_upload(const TfClient *c) {
  char t[16];
  tf__client_upload_trust(c, t, sizeof t);
  return sharing_enabled(c) && (!t[0] || !strcmp(t, "capped"));
}
/* the copy deck version the learner agreed to (the upload's consent_version) */
const char *tf__sharing_consent_version(TfClient *c) { return share(c) ? share(c)->consent_version : NULL; }
/* a session went up completely: counted for the status (the thank-you after the first) */
void tf__sharing_uploaded(TfClient *c) {
  Share *s = share(c);
  if (!s) return;
  s->shared_sessions++;
  share_save(c, s);
}
void tf__share_wipe(TfClient *c);

static void ask(TfClient *c, int reask) {
  Share *s = share(c);
  const TfClientOptions *o = tf__client_options(c);
  s->asks++;
  s->last_ask_session = s->sessions;
  share_save(c, s);
  if (o->on_consent) {
    TfConsentRequest r;
    r.reask = reask ? s->asks - 1 : 0;
    r.guardian = o->children;
    r.copy_json = tf_sharing_copy_json(o->language);
    o->on_consent(o->user, &r);
  } else {
    s->pending_ask = 1; /* the app can read it from the status and show the sheet itself */
    share_save(c, s);
  }
}

void tf__sharing_session(TfClient *c, int feedback, double audio_s) {
  Share *s = share(c);
  if (!s) return;
  (void)audio_s;
  s->sessions++;
  share_save(c, s);
  if (!sharing_enabled(c) || !feedback) return;
  if (!strcmp(s->consent, "unknown")) ask(c, 0);
  else if (!strcmp(s->consent, "declined") && s->asks < 3 && s->sessions - s->last_ask_session >= 7) ask(c, 1);
}

int tf_client_sharing_ask_now(TfClient *c) {
  if (!c || !share(c)) return tf__fail(TF_ERR_ARGUMENT, "no client");
  if (!sharing_enabled(c)) return tf__fail(TF_ERR_NOT_AVAILABLE, "sharing is not enabled (contributions option or flags.contributions)");
  if (!strcmp(share(c)->consent, "granted") || !strcmp(share(c)->consent, "final")) return 0;
  ask(c, share(c)->asks > 0);
  return 0;
}

int tf_client_sharing_answer(TfClient *c, int yes) {
  Share *s = c ? share(c) : NULL;
  if (!s) return tf__fail(TF_ERR_ARGUMENT, "no client");
  s->pending_ask = 0;
  if (yes) {
    snprintf(s->consent, sizeof s->consent, "granted");
    s->decided_at = (long long)time(NULL);
    s->guardian = tf__client_options(c)->children;
    consent_version(tf__client_options(c)->language, s->consent_version, sizeof s->consent_version);
  } else {
    snprintf(s->consent, sizeof s->consent, "%s", s->asks >= 3 ? "final" : "declined");
  }
  share_save(c, s);
  return 0;
}

int tf_client_sharing_set(TfClient *c, int on) {
  Share *s = c ? share(c) : NULL;
  if (!s) return tf__fail(TF_ERR_ARGUMENT, "no client");
  if (on) return tf_client_sharing_answer(c, 1);
  snprintf(s->consent, sizeof s->consent, "final");
  s->pending_ask = 0;
  share_save(c, s);
  /* off stops uploads at once: the PUT in flight is aborted, the unsent queue is deleted */
  tf__share_wipe(c);
  return 0;
}

int tf_client_sharing_state(TfClient *c) { return c && share(c) && !strcmp(share(c)->consent, "granted") && sharing_enabled(c); }

const char *tf_client_sharing_status_json(TfClient *c) {
  static _Thread_local char buf[640];
  Share *s = c ? share(c) : NULL;
  if (!s) return NULL;
  char trust[16];
  tf__client_upload_trust(c, trust, sizeof trust);
  snprintf(buf, sizeof buf,
           "{\"consent\":\"%s\",\"enabled\":%s,\"asks\":%d,\"sessions\":%d,\"pendingAsk\":%s,\"thanked\":%s,\"guardian\":%s,"
           "\"sharedSessions\":%d,\"uploadTrust\":\"%s\",\"mayUpload\":%s,\"consentVersion\":\"%s\"}",
           s->consent, sharing_enabled(c) ? "true" : "false", s->asks, s->sessions, s->pending_ask ? "true" : "false",
           s->thanked ? "true" : "false", s->guardian ? "true" : "false", s->shared_sessions, trust[0] ? trust : "capped",
           tf__sharing_may_upload(c) ? "true" : "false", s->consent_version);
  return buf;
}

int tf__client_call(TfClient *c, const char *method, const char *path, const char *body, int auth, const char *extra,
                    TfHttpResponse *r);
const char *tf__client_api(const TfClient *c);
const char *tf__client_app(const TfClient *c);
const char *tf__client_platform(const TfClient *c);
int tf__contributor(const char *dir, int create, char id[33], char contributor_key[90], char recovery_hash[65]);
int tf__contributor_recovery(const char *dir, char out[33]);
int tf__contributor_sign(const char *dir, const char *app, const char *nonce, long long ts, char sig_b64url[90]);
void tf__contributor_forget(const char *dir);
extern long long tf__now_override;

int tf_client_sharing_recovery_code(TfClient *c, char out[33]) {
  if (!c) return tf__fail(TF_ERR_ARGUMENT, "no client");
  return tf__contributor_recovery(tf__client_dir(c), out) ? tf__fail(TF_ERR_NOT_AVAILABLE, "nothing was shared from this install yet") : 0;
}

/* deletion routes take NO app key or token (403 credential_not_accepted): plain JSON POSTs */
static int post_plain(TfClient *c, const char *path, const char *body, TfHttpResponse *r) {
  char url[1024];
  snprintf(url, sizeof url, "%s%s", tf__client_api(c), path);
  const char *hdr[] = {"content-type: application/json", NULL};
  TfHttpRequest q;
  memset(&q, 0, sizeof q);
  q.method = "POST";
  q.url = url;
  q.headers = hdr;
  q.body = body;
  q.body_n = strlen(body);
  q.timeout_s = 30;
  return tf_http(&q, r);
}

/* "Delete what I shared": challenge, the device key's signature, delete (PLATFORM_CLIENT.md 9.1). On 202 the local
 * key, contributor id, recovery code and unsent queue are forgotten. */
int tf_client_sharing_delete(TfClient *c) {
  tf__clear_error();
  if (!c) return tf__fail(TF_ERR_ARGUMENT, "no client");
  char id[33];
  if (tf__contributor(tf__client_dir(c), 0, id, NULL, NULL)) return 0; /* nothing was ever shared from this install */
  char app[600];
  snprintf(app, sizeof app, "%s:%s", tf__client_platform(c), tf__client_app(c));
  int rc = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    TfBuf b;
    tf_buf_init(&b);
    tf_buf_puts(&b, "{\"app\":");
    tf_buf_json_str(&b, app);
    tf_buf_printf(&b, ",\"contributor\":\"%s\"}", id);
    TfHttpResponse r;
    rc = post_plain(c, "/v1/contributions/challenge", b.p, &r);
    tf_buf_free(&b);
    if (rc) return rc;
    TfJson *j = r.status == 200 ? tf_json_parse(r.body, r.body_n, NULL) : NULL;
    char nonce[200] = "";
    if (tf_json_str(tf_json_get(j, "nonce"))) snprintf(nonce, sizeof nonce, "%s", tf_json_get(j, "nonce")->str);
    tf_json_free(j);
    const int st = r.status;
    tf_http_response_free(&r);
    if (!*nonce) return tf__fail(TF_ERR_HTTP, "deletion challenge: HTTP %d", st);
    const long long ts = tf__now_override ? tf__now_override : (long long)time(NULL);
    char sig[90];
    if (tf__contributor_sign(tf__client_dir(c), app, nonce, ts, sig)) return tf__fail(TF_ERR_NOT_AVAILABLE, "the device key cannot sign");
    tf_buf_init(&b);
    tf_buf_puts(&b, "{\"app\":");
    tf_buf_json_str(&b, app);
    tf_buf_printf(&b, ",\"contributor\":\"%s\",\"nonce\":", id);
    tf_buf_json_str(&b, nonce);
    tf_buf_printf(&b, ",\"timestamp\":%lld,\"action\":\"delete-all\",\"signature\":\"%s\"}", ts, sig);
    rc = post_plain(c, "/v1/contributions/delete", b.p, &r);
    tf_buf_free(&b);
    if (rc) return rc;
    const int ds = r.status;
    const int retry = ds == 401 && r.body && (strstr(r.body, "nonce_invalid") || strstr(r.body, "timestamp_invalid"));
    tf_http_response_free(&r);
    if (ds == 202) {
      tf__contributor_forget(tf__client_dir(c));
      share(c)->shared_sessions = 0;
      share_save(c, share(c));
      tf__share_wipe(c);
      return 0;
    }
    if (!retry) return tf__fail(TF_ERR_HTTP, "deletion: HTTP %d", ds);
  }
  return tf__fail(TF_ERR_HTTP, "deletion: the challenge was refused twice");
}

const char *tf_client_sharing_summary_json(TfClient *c) {
  static _Thread_local char out[1024];
  char id[33], hdr[64];
  if (!c || tf__contributor(tf__client_dir(c), 0, id, NULL, NULL)) return NULL;
  snprintf(hdr, sizeof hdr, "x-qlab-contributor: %s", id);
  TfHttpResponse r;
  if (tf__client_call(c, "GET", "/v1/contributions/summary", NULL, 1, hdr, &r)) return NULL;
  const int ok = r.status == 200 && r.body && r.body_n < sizeof out;
  if (ok) memcpy(out, r.body, r.body_n + 1);
  tf_http_response_free(&r);
  return ok ? out : NULL;
}
