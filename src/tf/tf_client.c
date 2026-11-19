/* The platform client: token, register, the signed
 * config (Ed25519 over the exact body, pinned key id, expiry, ETag), telemetry v2 (anonymous daily
 * aggregates, always on, flags.analytics the only kill switch), issues (no free text). State lives in the
 * storage directory: client.json (bucket, install flag, token), config.body / config.meta, telemetry-<day>.json,
 * issues.json. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#include "tf_internal.h"
#include "tf_thread.h"

extern const char *const TF_QURANLAB_KEY_HEX;
void tf__sharing_init(TfClient *c);
void tf__sharing_free(TfClient *c);
void tf__share_init(TfClient *c);
void tf__share_free(TfClient *c);
void tf__share_cancel(TfClient *c);
void tf__share_sweep(TfClient *c);
void tf__share_background(TfClient *c);
void tf__share_window_check(TfClient *c);
int tf__sharing_outbox_resume(TfClient *c);
void tf__telemetry_add(TfClient *c, const char *field, long long n);
int tf__client_telemetry_on(const TfClient *c);

struct TfClient {
  TfClientConfig o;
  char *key, *app_id, *platform, *app_name, *app_version, *api, *cdn, *dir, *language;
  char trusted_hex[65]; /* the config and manifest signing key in force */
  char pin[24];         /* the model version the app pinned (sent as model_version), "" for none */
  char config_pin[24];  /* the pin the stored config was asked with */
  char stop_code[48];   /* the server's error code that stopped the network calls of this launch */
  char stop_detail[48]; /* sdk_too_old: the version the answer names */
  char gate[48];        /* SPEC 15.3: the refusal that stops the model on this device; kept in client.json until a
                           later answer accepts the key */
  int gate_cdn;         /* that refusal came from a model request (the CDN), not from the API */
  int purge;            /* SPEC 15.3: a refusal stopped the model and the models on this device
                           are to be removed; kept in client.json until a removal has left none */
  int purge_try;        /* a refusal has just arrived: the call that got it tries the removal before it returns */
  int last_error;       /* the catalogue code of the last failure a public call raised */
  void *api_state;      /* tf_api.c's state */
  uint8_t pk[32];
  char kid[17];
  int bucket;
  int new_install;
  char token[1024];
  long long token_exp;
  int stopped;         /* a stop code this launch: no more network calls */
  volatile int cancel; /* the client is being destroyed: requests in flight are abandoned */
  char download_protocol[12]; /* 3.1.3: what the last model request ran on ("h2"), "" before the first; in
                                 memory only, for the diagnosis */
  char *config;        /* the last verified config body */
  char etag[80];
  long long config_exp;
  char register_day[11]; /* the UTC day of the last register answered 200 (once per day per install) */
  void *sharing;       /* tf_sharing.c's state */
  void *share;         /* tf_share.c's uploader */
  TfMutex lock;        /* the uploader thread and the app's thread share the token and the config */
  /* telemetry deltas: tlock guards the day's counts and the sealing (disk only), send_lock one sender at a time */
  TfMutex tlock, send_lock;
  long long last_seal; /* when the last delta was sealed (the 15-minute send point) */
  long long next_try;  /* backoff of the timer's sends after a failure (in memory) */
  int fails;
  /* the timer: a thread started by tf__client_launch */
  TfMutex tm;
  TfCond tcv;
  TfThread tth;
  int t_on, t_quit, t_send;
  long long next_tick; /* SDK 4.0: the next Core tick (every TF_CORE_TICK_S) */
  int latest_logged;   /* the sdk_latest line was logged (once per client) */
  char latest[24];     /* the SDK release the config in force names (sdk_latest.version), "" when none */
  void *pm;            /* SDK 4.0: the protected model's host (tf_lease.c): the Core, the install keys */
  int answered;        /* the server answered an authenticated request of this client with a success */
  uint8_t sess_counted[4320], sess_judged[4320]; /* the analytics session's measured and judged ayahs (surah * 300 + ayah) */
  int config_fresh;    /* the config in force was fetched or confirmed (304) by this client */
};

/* ------------------------------------------------------------------ small helpers */
static char *dupstr(const char *s) {
  if (!s) return NULL;
  char *p = (char *)malloc(strlen(s) + 1);
  if (p) strcpy(p, s);
  return p;
}
static char *spath(const TfClient *c, const char *name) { return tf_path_join(c->dir, name); }
long long tf__now_override; /* tests: a fixed clock */
static long long now_unix(void) { return tf__now_override ? tf__now_override : (long long)time(NULL); }
static void day_utc(char out[11], long long offset_s) {
  time_t t = (time_t)(now_unix() + offset_s);
  struct tm g;
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  strftime(out, 11, "%Y-%m-%d", &g);
}

const char *tf__client_dir(const TfClient *c) { return c->dir; }
const char *tf__client_app(const TfClient *c) { return c->app_id; }
const char *tf__client_platform(const TfClient *c) { return c->platform; }
const char *tf__client_api(const TfClient *c) { return c->api; }
const char *tf__client_cdn(const TfClient *c) { return c->cdn; }
volatile int *tf__client_cancel_flag(TfClient *c) { return &c->cancel; }
void tf__client_set_download_protocol(TfClient *c, const char *protocol) {
  tf_mutex_lock(&c->lock);
  snprintf(c->download_protocol, sizeof c->download_protocol, "%s", protocol ? protocol : "");
  tf_mutex_unlock(&c->lock);
}
void tf__client_download_protocol(TfClient *c, char out[12]) {
  tf_mutex_lock(&c->lock);
  snprintf(out, 12, "%s", c->download_protocol);
  tf_mutex_unlock(&c->lock);
}
const char *tf__client_key(const TfClient *c) { return c->key; }
const char *tf__client_trusted_hex(const TfClient *c) { return c->trusted_hex; }
const char *tf__model_key_override;
const char *tf__client_model_key(const TfClient *c) { return tf__model_key_override ? tf__model_key_override : c->trusted_hex; }
void **tf__client_api_slot(TfClient *c) { return &c->api_state; }
void **tf__client_pm_slot(TfClient *c) { return &c->pm; }
void tf__client_note_error(TfClient *c, int code) {
  if (c) c->last_error = code;
}
int tf__client_last_error(const TfClient *c) { return c->last_error; }
/* why the network calls of this launch stopped: the server's code ("invalid_key", "sdk_too_old", ...), or "" */
const char *tf__client_stop_code(const TfClient *c) { return c->stop_code; }
const char *tf__client_latest(const TfClient *c) { return c->latest[0] ? c->latest : NULL; }
int tf__client_stopped(const TfClient *c) { return c->stopped; }
/* One list (SPEC 15.3, vectors/model_gate.json stop_reasons): the `reason` of an answer that refuses the key or
 * the app. An answer whose body carries one of these stops a model on the device; an answer without a reason
 * never does, whatever its status and its error. */
int tf__stop_reason(const char *code) {
  static const char *const stop[] = {"key_revoked", "key_unknown", "app_not_allowed", "origin_not_allowed",
                                     "key_type_mismatch", "app_blocked", "developer_not_approved"};
  for (size_t i = 0; code && i < sizeof stop / sizeof stop[0]; ++i)
    if (!strcmp(code, stop[i])) return 1;
  return 0;
}
/* the reasons whose error is invalid_key (the catalogue has no refusal of their own for them) */
int tf__key_reason(const char *code) { return code && (!strcmp(code, "key_revoked") || !strcmp(code, "key_unknown")); }
/* the stop reason an answer's body carries, copied to out; 1 when it has one */
int tf__body_stop_reason(const char *body, size_t n, char out[48]) {
  out[0] = 0;
  TfJson *j = body ? tf_json_parse(body, n, NULL) : NULL;
  const char *why = tf_json_str(tf_json_get(j, "reason"));
  if (why && strlen(why) < 48 && tf__stop_reason(why)) snprintf(out, 48, "%s", why);
  tf_json_free(j);
  return out[0] != 0;
}
/* the remembered refusal as the catalogue error the app gets */
int tf__fail_gate(const TfClient *c) {
  return tf__key_reason(c->gate) ? tf__fail(TF_ERR_INVALID_KEY, NULL) : tf__fail_rejected(c->gate, c->app_id);
}
const char *tf__client_gate(const TfClient *c) { return c->gate; }
int tf__client_purge_owed(const TfClient *c) { return c->purge; }
int tf__client_answered(const TfClient *c) { return c->answered; }
const char *tf__client_stop_detail(const TfClient *c) { return c->stop_detail; }
void tf__client_set_pin(TfClient *c, const char *version) { snprintf(c->pin, sizeof c->pin, "%s", version ? version : ""); }
const TfClientConfig *tf__client_options(const TfClient *c) { return &c->o; }
void **tf__client_sharing_slot(TfClient *c) { return &c->sharing; }
void **tf__client_share_slot(TfClient *c) { return &c->share; }
int tf__client_os_major(void);
const char *tf__client_config(const TfClient *c) { return c->config; }

int tf__client_os_major(void);
static int os_major(void);
int tf__client_os_major(void) { return os_major(); }
static int os_major(void) {
#ifdef _WIN32
  /* Windows 10 and 11 report 10 through the compatibility layer; the build decides 11 */
  OSVERSIONINFOW v;
  memset(&v, 0, sizeof v);
  v.dwOSVersionInfoSize = sizeof v;
  typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
  RtlGetVersionFn f = (RtlGetVersionFn)(void (*)(void))GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
  if (f && f(&v) == 0) return v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? 11 : (int)v.dwMajorVersion;
  return 10;
#else
  struct utsname u;
  if (uname(&u) == 0) {
    const int k = atoi(u.release);
    return k >= 1 && k <= 99 ? k : 1;
  }
  return 1;
#endif
}
static const char *ram_tier(void) {
  double gb = 0;
#ifdef _WIN32
  MEMORYSTATUSEX m;
  m.dwLength = sizeof m;
  if (GlobalMemoryStatusEx(&m)) gb = (double)m.ullTotalPhys / (1024.0 * 1024 * 1024);
#else
  long pages = sysconf(_SC_PHYS_PAGES), ps = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && ps > 0) gb = (double)pages * ps / (1024.0 * 1024 * 1024);
#endif
  return gb < 2 ? "lt2" : gb < 4 ? "2_4" : gb < 8 ? "4_8" : gb < 16 ? "8_16" : "ge16";
}
const char *tf__client_ram_tier(void) { return ram_tier(); }
/* the error reports' 30 s send is due: the timer looks again (tf_report.c, under its own lock) */
void tf__client_rep_wake(TfClient *c) {
  tf_mutex_lock(&c->tm);
  tf_cond_signal(&c->tcv);
  tf_mutex_unlock(&c->tm);
}

/* ------------------------------------------------------------------ requests */
typedef struct {
  const char *method, *path, *body;
  int auth; /* 0 key, 1 token */
  const char *extra_header;
} Call;

static int classify_stop(TfClient *c, const TfHttpResponse *r);
/* A 403 on a route a model needs (token, registration, config) whose code this SDK does not know (a newer
 * server): it is a refusal all the same, so the app gets server_rejected with the code in the message, and the
 * launch makes no more network calls. Codes the error table gives another meaning are not refusals of the app. */
static void note_refusal(TfClient *c, const TfHttpResponse *r) {
  if (r->status != 403 || c->stopped || !r->body) return;
  static const char *const other[] = {"app_mismatch", "attestation_required", "contributions_disabled", "turnstile_required",
                                      "turnstile_failed", "segment_cap", "contributor_revoked", "credential_not_accepted"};
  TfJson *j = tf_json_parse(r->body, r->body_n, NULL);
  const char *e = tf_json_str(tf_json_get(j, "error"));
  int ok = e && *e && strlen(e) < sizeof c->stop_code;
  for (const char *q = e; ok && *q; ++q) ok = (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_';
  for (size_t i = 0; ok && i < sizeof other / sizeof other[0]; ++i) ok = strcmp(e, other[i]) != 0;
  if (ok) {
    snprintf(c->stop_code, sizeof c->stop_code, "%s", e);
    c->stop_detail[0] = 0;
    c->stopped = 1;
  }
  tf_json_free(j);
}

static int classify_stop(TfClient *c, const TfHttpResponse *r) {
  if (!r->body) return 0;
  /* a key Quran Lab revoked or never issued, whatever the error beside the reason (invalid_key, or invalid_token
   * for a token whose key was revoked after it was issued): the app is told invalid_key */
  {
    char why[48];
    if (tf__body_stop_reason(r->body, r->body_n, why) && tf__key_reason(why)) {
      snprintf(c->stop_code, sizeof c->stop_code, "invalid_key");
      c->stop_detail[0] = 0;
      return 1;
    }
  }
  /* The answer's "reason" first: the server answers an origin, key-type or allow-list refusal as a bad key (401
   * invalid_key, the code released SDKs stop on) and names the check that refused in "reason". When the
   * catalogue has that refusal, it is the code the app is told; a bad key has no reason and reads as before. */
  {
    TfJson *j = tf_json_parse(r->body, r->body_n, NULL);
    const char *why = tf_json_str(tf_json_get(j, "reason"));
    const int known = why && strlen(why) < sizeof c->stop_code && tf__reason_known(why);
    if (known) {
      snprintf(c->stop_code, sizeof c->stop_code, "%s", why);
      c->stop_detail[0] = 0;
    }
    tf_json_free(j);
    if (known) return 1;
  }
  static const char *const stops[] = {"invalid_key", "developer_not_approved", "not_a_member", "key_type_mismatch",
                                      "origin_not_allowed", "app_not_allowed", "app_blocked", "environment_mismatch",
                                      "sdk_too_old", "unauthorized"};
  for (size_t i = 0; i < sizeof stops / sizeof stops[0]; ++i) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"error\":\"%s\"", stops[i]);
    if (!strstr(r->body, pat)) continue;
    snprintf(c->stop_code, sizeof c->stop_code, "%s", stops[i]);
    c->stop_detail[0] = 0;
    if (!strcmp(stops[i], "sdk_too_old")) {
      /* the answer names the version this key needs (min_sdk, or min_sdk_version) */
      TfJson *j = tf_json_parse(r->body, r->body_n, NULL);
      const char *v = tf_json_str(tf_json_get(j, "min_sdk"));
      if (!v) v = tf_json_str(tf_json_get(j, "min_sdk_version"));
      if (!v) v = tf_json_str(tf_json_get(j, "required"));
      if (v && tf__version_at_least(v, "0")) snprintf(c->stop_detail, sizeof c->stop_detail, "%s", v);
      tf_json_free(j);
    }
    return 1;
  }
  return 0;
}

static int ensure_token(TfClient *c);
static void save_state(TfClient *c);
/* the server refused the key or the app with a reason that stops the model: remembered on the device */
static void gate_set(TfClient *c, const char *code, int cdn) {
  if (!tf__stop_reason(code)) return;
  /* a stop removes the models from the device, and every refusal tries that again */
  c->purge_try = 1;
  if (!strcmp(c->gate, code) && c->gate_cdn == cdn && c->purge) return;
  snprintf(c->gate, sizeof c->gate, "%s", code);
  c->gate_cdn = cdn;
  c->purge = 1;
  save_state(c);
}
void tf__client_refuse(TfClient *c, const char *code) {
  tf_mutex_lock(&c->lock);
  gate_set(c, code, 1);
  const int purge = c->purge_try;
  c->purge_try = 0;
  tf_mutex_unlock(&c->lock);
  if (purge) tf__client_purge_models(c);
}
/* 0: a removal has left no model on this device, nothing is owed any more. 1: a removal failed, it is owed */
void tf__client_purge_set(TfClient *c, int owed) {
  tf_mutex_lock(&c->lock);
  if (c->purge != (owed != 0)) {
    c->purge = owed != 0;
    save_state(c);
  }
  tf_mutex_unlock(&c->lock);
}
/* A refusal that came from a model request: the API has just accepted the key (a fresh config), so the model
 * request is asked again and says for itself whether it still refuses. Without a network it holds. */
void tf__client_gate_retry(TfClient *c) {
  tf_mutex_lock(&c->lock);
  if (c->gate[0] && c->gate_cdn) {
    c->gate[0] = 0;
    c->gate_cdn = 0;
    save_state(c);
  }
  tf_mutex_unlock(&c->lock);
}

/* 3.0.1: a thread may name its own abort flag for the calls it makes (the sharing uploader does: turning sharing
 * off aborts its request in flight, whichever it is, without touching the client's other calls). Without one a
 * call carries the client's flag, as before. */
static _Thread_local volatile int *t_cancel;
void tf__client_thread_cancel(volatile int *flag) { t_cancel = flag; }
/* SDK 4.0, the lease route: its stops count only when the Core verifies their signature (tf_lease.c), so the
 * answer's reason is not taken here */
static _Thread_local int t_no_gate;
/* 18.4.7.1: a model open past its lease's exp waits at most 10 s for the lease attempt (tf_lease.c): until then each
 * request of this thread gets at most the time left, and none starts after it (no answer) */
static _Thread_local double t_deadline;
void tf__client_deadline(double at) { t_deadline = at; }

static int call_locked(TfClient *c, const Call *k, TfHttpResponse *r);
static const char *route_stage(const char *path);
/* The client's lock, taken for a call's whole request. Under a model open's deadline (t_deadline, 18.4.7.1) it is waited
 * for no longer than the deadline: another thread's request in flight (the background lease refresh, a config refresh)
 * holds it for up to its own 30 s, and until 11-18 an open past exp behind such a request waited for it, 32 s on staging
 * (Stage B) where the contract says 10. Past the deadline the call fails as a request that never started (no answer). */
static int call_lock(TfClient *c, const Call *k) {
  if (t_deadline <= 0) {
    tf_mutex_lock(&c->lock);
    return 0;
  }
  while (!tf_mutex_trylock(&c->lock)) {
    if (tf__now_s() >= t_deadline) {
      tf__http_answer_none();
      const int rc = tf__fail(TF__ERR_NETWORK, "no answer within the time a model open waits for a lease");
      const char *stage = route_stage(k->path);
      if (stage) tf__cause(stage, "timeout", 0, NULL, NULL);
      return rc;
    }
    tf__sleep_ms(20);
  }
  return 0;
}
static int call(TfClient *c, const Call *k, TfHttpResponse *r) {
  const int lrc = call_lock(c, k);
  if (lrc) return lrc;
  const int rc = call_locked(c, k, r);
  const int purge = c->purge_try;
  c->purge_try = 0;
  tf_mutex_unlock(&c->lock);
  /* SPEC 15.3: the answer refused the key or the app: the models go in this call, before it returns */
  if (purge) tf__client_purge_models(c);
  return rc;
}
/* 24.3: the stage of a route (NULL: a route whose failure is never reported, the error reports' own) */
static const char *route_stage(const char *path) {
  if (!strncmp(path, "/v1/config", 10)) return "config";
  if (!strcmp(path, "/v1/register")) return "register";
  if (!strcmp(path, "/v1/attest")) return "attest";
  if (!strcmp(path, "/v1/attest/challenge") || !strcmp(path, "/v1/model-key")) return "lease";
  if (!strcmp(path, "/v1/events")) return "events";
  if (!strncmp(path, "/v1/uploads", 11) || !strncmp(path, "/v1/contributions", 17)) return "sharing";
  return NULL;
}
/* 24.1 item 2: the launch's calls (config, register, the token) that fail where no app call will report it; the
 * lease's background refresh is reported where it ends (tf_lease.c), after the renewals 18.4 asks for */
static void route_failed(const char *stage, int attempts) {
  tf__cause_retries(attempts);
  if (stage && (!strcmp(stage, "config") || !strcmp(stage, "register") || !strcmp(stage, "attest"))) tf__cause_silent();
}
static int call_locked(TfClient *c, const Call *k, TfHttpResponse *r) {
  if (c->stopped) return tf__fail(TF__ERR_NOT_AVAILABLE, "network calls are stopped for this launch");
  if (!c->key) return tf__fail(TF__ERR_NOT_AVAILABLE, "no developer key");
  const char *stage = route_stage(k->path);
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (k->auth && ensure_token(c)) return tf__error_code();
    char url[2048], auth[1200];
    snprintf(url, sizeof url, "%s%s", c->api, k->path);
    const char *hdr[6];
    int nh = 0;
    if (k->auth) snprintf(auth, sizeof auth, "authorization: Bearer %s", c->token);
    else snprintf(auth, sizeof auth, "x-qlab-key: %s", c->key);
    hdr[nh++] = auth;
    if (k->body) hdr[nh++] = "content-type: application/json";
    if (k->extra_header) hdr[nh++] = k->extra_header;
    hdr[nh] = NULL;
    TfHttpRequest q;
    memset(&q, 0, sizeof q);
    q.method = k->method;
    q.url = url;
    q.headers = hdr;
    q.body = k->body;
    q.body_n = k->body ? strlen(k->body) : 0;
    q.timeout_s = 30;
    if (t_deadline > 0) {
      const double left = t_deadline - tf__now_s();
      if (left <= 0) {
        tf__http_answer_none();
        const int rc = tf__fail(TF__ERR_NETWORK, "no answer within the time a model open waits for a lease");
        if (stage) tf__cause(stage, "timeout", 0, NULL, NULL);
        return rc;
      }
      if (left < q.timeout_s) q.timeout_s = left < 1 ? 1 : (int)(left + 0.999);
    }
    q.cancel = t_cancel ? t_cancel : &c->cancel;
    /* The request itself runs without the client's lock (everything it reads is copied above): until 11-18 a request
     * in flight held it for up to its 30 s, and every other thread that reads the client (a config flag, the counters,
     * another call, a model open past exp under its 10 s) waited for the network with it (Stage B, staging: an open
     * past exp behind the background refresh's hanging request took 32 s). Calls of several threads may now overlap;
     * each still sees and changes the client's state under the lock. */
    tf_mutex_unlock(&c->lock);
    const int hrc = tf_http(&q, r);
    tf_mutex_lock(&c->lock);
    if (hrc) {
      const int rc = tf__error_code() ? tf__error_code() : TF__ERR_NETWORK;
      if (stage) {
        tf__cause_transport(stage, r->status);
        route_failed(stage, attempt);
      }
      return rc;
    }
    char why[48];
    /* invalid_token without a reason: renew and repeat once. With a reason of the list the key is refused */
    if (r->status == 401 && k->auth && attempt == 0 && r->body && strstr(r->body, "invalid_token") &&
        !tf__body_stop_reason(r->body, r->body_n, why)) {
      c->token[0] = 0;
      c->token_exp = 0;
      tf_http_response_free(r);
      continue;
    }
    /* an answer that failed is the cause of what follows (24.3); 304 is the config's "not changed" */
    if (stage && r->status >= 300 && r->status != 304) {
      tf__cause_answer(stage, r->status, r->body, r->body_n);
      route_failed(stage, attempt);
    }
    break;
  }
  if (r->status >= 400 && !t_no_gate) {
    char why[48];
    if (classify_stop(c, r)) c->stopped = 1;
    /* SPEC 15.3: only a reason of the list stops the model on this device */
    if (tf__body_stop_reason(r->body, r->body_n, why)) gate_set(c, why, 0);
  }
  /* an answer that accepts the key: a 2xx, or the config's 304 (not changed). A redirect is neither a refusal
   * nor an acceptance: it is not followed (SPEC 15.4), it is the call's failure, and it clears nothing */
  if ((r->status >= 200 && r->status < 300) || r->status == 304) {
    c->answered = 1;
    /* an answer that accepts the key clears a remembered refusal of the API (SPEC 15.3); one that came from
     * a model request is asked again by the next model call (tf__client_gate_retry) */
    if (c->gate[0] && !c->gate_cdn) {
      c->gate[0] = 0;
      save_state(c);
    }
  }
  return 0;
}

int tf__client_model_token(TfClient *c, int renew, char *out, size_t n) {
  tf_mutex_lock(&c->lock);
  if (renew) {
    c->token[0] = 0;
    c->token_exp = 0;
  }
  int rc = c->stopped ? tf__fail(TF__ERR_NOT_AVAILABLE, "network calls are stopped for this launch")
           : !c->key ? tf__fail(TF__ERR_NOT_AVAILABLE, "no developer key") : ensure_token(c);
  if (!rc) snprintf(out, n, "%s", c->token);
  tf_mutex_unlock(&c->lock);
  return rc;
}

int tf__client_call(TfClient *c, const char *method, const char *path, const char *body, int auth, const char *extra,
                    TfHttpResponse *r) {
  Call k = {method, path, body, auth, extra};
  return call(c, &k, r);
}
/* POST /v1/model-key with the app token (18.4.4). A 401 invalid_token without a reason renews the token and repeats
 * once, as every token route; no stop reason of the answer is taken here (18.4.10: the caller asks the Core). */
int tf__client_call_lease(TfClient *c, const char *body, TfHttpResponse *r) {
  Call k = {"POST", "/v1/model-key", body, 1, NULL};
  t_no_gate = 1;
  const int rc = call(c, &k, r);
  t_no_gate = 0;
  return rc;
}
/* the token of an attest made elsewhere (tf_lease.c, with the install keys) */
void tf__client_set_token(TfClient *c, const char *token, long long expires_in) {
  if (!c || !token || strlen(token) >= sizeof c->token) return;
  tf_mutex_lock(&c->lock);
  snprintf(c->token, sizeof c->token, "%s", token);
  c->token_exp = now_unix() + (expires_in > 0 ? expires_in : 3600);
  save_state(c);
  tf_mutex_unlock(&c->lock);
}
/* a stop the Core verified on the lease route (18.4.10): remembered and the models removed, as an API refusal */
void tf__client_refuse_api(TfClient *c, const char *code) {
  tf_mutex_lock(&c->lock);
  gate_set(c, code, 0);
  snprintf(c->stop_code, sizeof c->stop_code, "%s", tf__key_reason(code) ? "invalid_key" : code);
  c->stopped = 1;
  const int purge = c->purge_try;
  c->purge_try = 0;
  tf_mutex_unlock(&c->lock);
  if (purge) tf__client_purge_models(c);
}
/* the app token was refused (a 401 whatever its error code): the next call with a token asks for a new one */
void tf__client_token_forget(TfClient *c) {
  tf_mutex_lock(&c->lock);
  c->token[0] = 0;
  c->token_exp = 0;
  tf_mutex_unlock(&c->lock);
}

/* ------------------------------------------------------------------ state */
static void save_state(TfClient *c) {
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "{\"v\":1,\"bucket\":%d,\"installed\":true,\"pending_new_install\":%s,\"token\":", c->bucket,
                c->new_install ? "true" : "false");
  tf_buf_json_str(&b, c->token);
  tf_buf_printf(&b, ",\"token_exp\":%lld,\"etag\":", c->token_exp);
  tf_buf_json_str(&b, c->etag);
  tf_buf_printf(&b, ",\"config_exp\":%lld,\"register_day\":", c->config_exp);
  tf_buf_json_str(&b, c->register_day);
  tf_buf_puts(&b, ",\"config_pin\":");
  tf_buf_json_str(&b, c->config_pin);
  tf_buf_puts(&b, ",\"refused\":");
  tf_buf_json_str(&b, c->gate);
  tf_buf_printf(&b, ",\"refused_by_cdn\":%s,\"remove_models\":%s}", c->gate_cdn ? "true" : "false", c->purge ? "true" : "false");
  char *p = spath(c, "client.json");
  if (p && b.p) tf_write_file(p, b.p, b.n);
  free(p);
  tf_buf_free(&b);
}

static void load_state(TfClient *c) {
  char *p = spath(c, "client.json"), *text = NULL;
  size_t n;
  int have = p && !tf_read_file(p, &text, &n);
  free(p);
  TfJson *j = have ? tf_json_parse(text, n, NULL) : NULL;
  free(text);
  if (j) {
    const TfJson *b = tf_json_get(j, "bucket");
    c->bucket = b && b->type == TJ_NUM ? (int)b->num : -1;
    const TfJson *pn = tf_json_get(j, "pending_new_install");
    c->new_install = pn && pn->boolean;
    if (tf_json_str(tf_json_get(j, "token"))) snprintf(c->token, sizeof c->token, "%s", tf_json_str(tf_json_get(j, "token")));
    const TfJson *te = tf_json_get(j, "token_exp");
    c->token_exp = te && te->type == TJ_NUM ? (long long)te->num : 0;
    if (tf_json_str(tf_json_get(j, "etag"))) snprintf(c->etag, sizeof c->etag, "%s", tf_json_str(tf_json_get(j, "etag")));
    const TfJson *ce = tf_json_get(j, "config_exp");
    c->config_exp = ce && ce->type == TJ_NUM ? (long long)ce->num : 0;
    const char *rd = tf_json_str(tf_json_get(j, "register_day"));
    if (rd && strlen(rd) == 10) snprintf(c->register_day, sizeof c->register_day, "%s", rd);
    const char *cp0 = tf_json_str(tf_json_get(j, "config_pin"));
    if (cp0) snprintf(c->config_pin, sizeof c->config_pin, "%s", cp0);
    const char *gr = tf_json_str(tf_json_get(j, "refused"));
    if (gr && tf__stop_reason(gr)) {
      snprintf(c->gate, sizeof c->gate, "%s", gr);
      const TfJson *gc = tf_json_get(j, "refused_by_cdn");
      c->gate_cdn = gc && gc->type == TJ_BOOL && gc->boolean;
    }
    const TfJson *po = tf_json_get(j, "remove_models");
    c->purge = po && po->type == TJ_BOOL && po->boolean;
    tf_json_free(j);
  } else {
    c->bucket = -1;
    c->new_install = 1; /* no state yet: the first launch after install */
  }
  if (c->bucket < 0 || c->bucket > 99) {
    uint8_t r[8];
    tf_random_bytes(r, sizeof r);
    c->bucket = (int)(((uint32_t)r[0] << 8 | r[1]) % 100);
  }
  /* the stored verified config, if still valid */
  char *cp = spath(c, "config.body"), *body = NULL;
  size_t bn;
  if (cp && !tf_read_file(cp, &body, &bn)) {
    if (c->config_exp > now_unix()) c->config = body;
    else free(body);
  }
  free(cp);
}

TfClientConfig tf__client_default_config(void) {
  TfClientConfig o;
  memset(&o, 0, sizeof o);
  return o;
}

TfClient *tf__client_open(const TfClientConfig *opt) {
  tf__clear_error();
  TfClientConfig o = opt ? *opt : tf__client_default_config();
  if (!o.storage_dir || !o.app_id) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "a storage folder and an app id are required");
    return NULL;
  }
  TfClient *c = (TfClient *)calloc(1, sizeof *c);
  if (!c) return NULL;
  tf_mutex_init(&c->lock);
  tf_mutex_init(&c->tlock);
  tf_mutex_init(&c->send_lock);
  tf_mutex_init(&c->tm);
  tf_cond_init(&c->tcv);
  c->o = o;
  c->key = dupstr(o.key);
  c->app_id = dupstr(o.app_id);
  c->platform = dupstr(o.platform ? o.platform : tf__platform_name());
  c->app_name = dupstr(o.app_name);
  c->app_version = dupstr(o.app_version ? o.app_version : "0");
  c->api = dupstr(o.api_url ? o.api_url : "https://api.quranlab.ai");
  c->cdn = dupstr(o.cdn_url ? o.cdn_url : "https://cdn.quranlab.ai/models");
  c->dir = dupstr(o.storage_dir);
  c->language = dupstr(o.language ? o.language : "en");
  c->o.key = c->key;
  c->o.app_id = c->app_id;
  c->o.platform = c->platform;
  c->o.app_name = c->app_name;
  c->o.app_version = c->app_version;
  c->o.api_url = c->api;
  c->o.cdn_url = c->cdn;
  c->o.storage_dir = c->dir;
  c->o.language = c->language;
  c->o.trusted_key_hex = NULL;
  {
    uint8_t h[32];
    snprintf(c->trusted_hex, sizeof c->trusted_hex, "%s", opt && opt->trusted_key_hex ? opt->trusted_key_hex : TF_QURANLAB_KEY_HEX);
    if (strlen(opt && opt->trusted_key_hex ? opt->trusted_key_hex : TF_QURANLAB_KEY_HEX) != 64 || tf_hex_decode(c->trusted_hex, c->pk, 32)) {
      tf__fail(TF_ERR_INVALID_ARGUMENT, "trusted_key_hex is not 64 hex digits");
      tf__client_close(c);
      return NULL;
    }
    tf_sha256(c->pk, 32, h);
    tf_hex_encode(h, 8, c->kid);
  }
  size_t n = strlen(c->api);
  while (n && c->api[n - 1] == '/') c->api[--n] = 0;
  if (tf_mkdirs(c->dir)) {
    tf__fail(TF_ERR_STORAGE_FAILED, "cannot create %s", c->dir);
    tf__client_close(c);
    return NULL;
  }
  /* 24.2 first_launch: the install's first process, when no earlier start left its state (PLATFORM_CLIENT.md 2) */
  char *cj = spath(c, "client.json");
  const int first_process = cj && !tf_file_exists(cj);
  free(cj);
  load_state(c);
  save_state(c);
  tf__rep_attach(c, first_process); /* the error reports of this process are this Client's */
  /* SPEC 15.3: a start that finds a remembered refusal, or a removal an earlier run owes,
   * removes the models before the Client does anything else */
  if (c->gate[0] || c->purge) tf__store_purge(c);
  tf__sharing_init(c);
  tf__share_init(c);
  tf__share_sweep(c); /* 3.0.1: recordings an earlier run's stop left behind (the process died, a file was in use) */
  /* telemetry: opening a client is a launch; the 15 minutes run from here */
  c->last_seal = now_unix();
  tf__telemetry_add(c, "launches", 1);
  tf__client_session_close(c); /* 17.8: a session an earlier process left open (it ended without closing it) */
  return c;
}

static void timer_stop(TfClient *c);
static int telemetry_send_point(TfClient *c);

void tf__client_cancel_calls(TfClient *c, int on) {
  if (on) {
    tf__http_cancel(&c->cancel);
    tf__share_cancel(c); /* the uploader's calls carry its own flag (3.0.1) */
  } else {
    c->cancel = 0;
  }
}

void tf__client_close(TfClient *c) {
  if (!c) return;
  timer_stop(c);
  c->cancel = 0; /* the last send point below is a request of its own */
  /* closing the client is a send point (what is not sent stays on disk for the next launch) */
  if (!tf__learner_test_kill) tf__client_session_close(c); /* 17.8: the Client's end closes the analytics session */
  if (c->key && tf__client_telemetry_on(c)) telemetry_send_point(c);
  else if (c->key) tf__rep_send(c); /* the error reports' send point all the same (24.6) */
  tf__rep_detach(c);
  tf__pm_client_gone(c); /* the Core stays while a Model of it is open */
  tf__share_free(c);
  tf__sharing_free(c);
  free(c->key);
  free(c->app_id);
  free(c->platform);
  free(c->app_name);
  free(c->app_version);
  free(c->api);
  free(c->cdn);
  free(c->dir);
  free(c->language);
  free(c->config);
  tf_cond_destroy(&c->tcv);
  tf_mutex_destroy(&c->tm);
  tf_mutex_destroy(&c->send_lock);
  tf_mutex_destroy(&c->tlock);
  tf_mutex_destroy(&c->lock);
  free(c);
}

/* ------------------------------------------------------------------ token (unattested: native has no attestation) */
static int ensure_token(TfClient *c) {
  if (c->token[0] && c->token_exp - now_unix() > 3600) return 0;
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"platform\":");
  tf_buf_json_str(&b, c->platform);
  tf_buf_puts(&b, ",\"bundle_id\":");
  tf_buf_json_str(&b, c->app_id);
  tf_buf_puts(&b, "}");
  Call k = {"POST", "/v1/attest", b.p, 0, NULL};
  TfHttpResponse r;
  int rc = call(c, &k, &r);
  tf_buf_free(&b);
  if (rc) return rc;
  if (r.status != 200) {
    note_refusal(c, &r);
    rc = tf__fail(TF__ERR_HTTP, "token: HTTP %d %s", r.status, r.body ? r.body : "");
    tf_http_response_free(&r);
    return rc;
  }
  TfJson *j = tf_json_parse(r.body, r.body_n, NULL);
  const char *t = tf_json_str(tf_json_get(j, "token"));
  const TfJson *e = tf_json_get(j, "expires_in");
  if (!t || strlen(t) >= sizeof c->token) {
    tf__cause("attest", "invalid_answer", r.status, NULL, NULL); /* a 200 that is not what was asked for */
    tf__cause_silent();
    rc = tf__fail(TF__ERR_HTTP, "token: no token in the answer");
  } else {
    snprintf(c->token, sizeof c->token, "%s", t);
    c->token_exp = now_unix() + (e && e->type == TJ_NUM ? (long long)e->num : 3600);
    save_state(c);
  }
  tf_json_free(j);
  tf_http_response_free(&r);
  return rc;
}

/* ------------------------------------------------------------------ register */
/* Once per UTC day per install (2.0.1: the server counts daily active installs; launches go in the telemetry): the
 * day stamp is written only after a 200, so a failed attempt is tried again (and new_install survives it). */
int tf__client_register(TfClient *c) {
  tf__clear_error();
  if (!c) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  char today[11];
  day_utc(today, 0);
  if (!strcmp(c->register_day, today)) return 0;
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"platform\":");
  tf_buf_json_str(&b, c->platform);
  tf_buf_puts(&b, ",\"bundle_id\":");
  tf_buf_json_str(&b, c->app_id);
  if (c->app_name && *c->app_name) {
    tf_buf_puts(&b, ",\"app_name\":");
    tf_buf_json_str(&b, c->app_name);
  }
  tf_buf_puts(&b, ",\"app_version\":");
  tf_buf_json_str(&b, c->app_version);
  tf_buf_printf(&b, ",\"sdk_version\":\"%s\",\"new_install\":%s}", TF_SDK_VERSION, c->new_install ? "true" : "false");
  Call k = {"POST", "/v1/register", b.p, 1, NULL};
  TfHttpResponse r;
  int rc = call(c, &k, &r);
  tf_buf_free(&b);
  if (rc) return rc;
  if (r.status == 200) {
    tf_mutex_lock(&c->lock);
    snprintf(c->register_day, sizeof c->register_day, "%s", today);
    c->new_install = 0;
    save_state(c);
    tf_mutex_unlock(&c->lock);
  } else {
    note_refusal(c, &r);
    rc = tf__fail(TF__ERR_HTTP, "register: HTTP %d %s", r.status, r.body ? r.body : "");
  }
  tf_http_response_free(&r);
  return rc;
}

/* ------------------------------------------------------------------ config */
/* Verify a config body as received: kid pinned, Ed25519 over the exact bytes, v 1, expires_at in the future.
 * Returns 0 and the expiry, or TF__ERR_CONFIG. Exposed for the tests (the recorded fixtures). */
/* a config that does not verify: the cause (24.3 signature, parse, expired) and the internal error */
static int config_bad(const char *reason, const char *text, const char *arg) {
  tf__cause("config", reason, 200, NULL, NULL);
  return tf__fail(TF__ERR_CONFIG, text, arg);
}
int tf__verify_config(const uint8_t pk[32], const char *kid_pinned, const char *body, size_t n, const char *sig_header,
                      const char *kid, long long now, long long *expires) {
  if (!kid || _tf_strnicmp(kid, kid_pinned, 17)) return config_bad("signature", "config: unknown key id %s", kid ? kid : "");
  if (!sig_header || strncmp(sig_header, "ed25519=", 8)) return config_bad("signature", "config: no ed25519 signature%s", "");
  uint8_t sig[70];
  if (tf_base64_decode(sig_header + 8, sig, sizeof sig) != 64) return config_bad("signature", "config: malformed signature%s", "");
  if (!tf_ed25519_verify(sig, (const uint8_t *)body, n, pk)) return config_bad("signature", "config: the signature does not verify%s", "");
  TfJson *j = tf_json_parse(body, n, NULL);
  const TfJson *v = tf_json_get(j, "v"), *e = tf_json_get(j, "expires_at");
  int rc = 0;
  if (!v || v->type != TJ_NUM || v->num != 1) rc = config_bad("parse", "config: v is not 1%s", "");
  else if (!e || e->type != TJ_NUM || (long long)e->num <= now) rc = config_bad("expired", "config: expired%s", "");
  else if (expires) *expires = (long long)e->num;
  tf_json_free(j);
  return rc;
}

void tf__client_check_latest(TfClient *c);
static void check_pin(TfClient *c);
int tf__client_refresh_config(TfClient *c) {
  tf__clear_error();
  if (!c) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  char path[256], inm[120];
  /* SPEC 2a: a pinned model version goes with the request; the server answers that version's build for this SDK */
  if (c->pin[0]) snprintf(path, sizeof path, "/v1/config?bucket=%d&sdk=%s&model_version=%s", c->bucket, TF_SDK_VERSION, c->pin);
  else snprintf(path, sizeof path, "/v1/config?bucket=%d&sdk=%s", c->bucket, TF_SDK_VERSION);
  /* within 1 h of expiry: ask without If-None-Match for a freshly signed body */
  const int conditional = c->config && c->etag[0] && c->config_exp - now_unix() > 3600 && !strcmp(c->pin, c->config_pin);
  snprintf(inm, sizeof inm, "if-none-match: %s", c->etag);
  Call k = {"GET", path, NULL, 1, conditional ? inm : NULL};
  TfHttpResponse r;
  int rc = call(c, &k, &r);
  if (rc) return rc;
  if (r.status == 304 && c->config) {
    tf_http_response_free(&r);
    c->config_fresh = 1;
    return 0;
  }
  if (r.status != 200) {
    note_refusal(c, &r);
    if (r.status < 300 || r.status == 304) { /* a 2xx that is not the config, a 304 for none held (call: the rest) */
      tf__cause("config", "invalid_answer", r.status, NULL, NULL);
      tf__cause_silent();
    }
    rc = tf__fail(TF__ERR_HTTP, "config: HTTP %d", r.status);
    tf_http_response_free(&r);
    return rc;
  }
  long long exp = 0;
  rc = tf__verify_config(c->pk, c->kid, r.body, r.body_n, r.signature, r.key_id, now_unix(), &exp);
  if (rc) {
    tf__cause_silent(); /* 24.1: the launch goes on with the stored config, or none */
    tf__client_report_issue(c, "signature_invalid", "config", NULL);
    tf_http_response_free(&r);
    return rc; /* the last good config stays */
  }
  tf_mutex_lock(&c->lock);
  free(c->config);
  c->config = r.body;
  r.body = NULL;
  c->config_exp = exp;
  c->config_fresh = 1;
  snprintf(c->etag, sizeof c->etag, "%s", r.etag);
  snprintf(c->config_pin, sizeof c->config_pin, "%s", c->pin);
  char *p = spath(c, "config.body");
  if (p) tf_write_file(p, c->config, strlen(c->config));
  free(p);
  save_state(c);
  tf_mutex_unlock(&c->lock);
  tf_http_response_free(&r);
  tf__client_check_latest(c);
  check_pin(c);
  /* 2.0.3: a config that turns sharing off, or sets another join window, reaches a recording that waits for its
   * next take now, not at the end of the window it started with */
  tf__share_window_check(c);
  tf__rep_config(c); /* 24.5: flags.issues */
  return 0;
}

/* ------------------------------------------------------------------ the developer's log and sdk_latest */
/* debug builds only (the app's TfClientConfig.debug_build: the library itself is always a release build, so its
 * own NDEBUG says nothing about the app); the app's callback, or stderr without one */
void tf__client_log(TfClient *c, int level, const char *message) {
  if (!c || !c->o.debug_build || !message) return;
  if (c->o.on_log) c->o.on_log(c->o.log_user, level, message);
  else fprintf(stderr, "tajweedformer: %s\n", message);
}
/* dotted numbers only ("2", "2.0", "2.0.1", up to 4 parts of up to 9 digits); 0 for anything else */
static int parse_version(const char *s, long part[4], int *n) {
  *n = 0;
  if (!s || !*s) return 0;
  while (*n < 4) {
    int digits = 0;
    long v = 0;
    while (*s >= '0' && *s <= '9' && digits < 10) v = v * 10 + (*s++ - '0'), digits++;
    if (!digits || digits > 9) return 0;
    part[(*n)++] = v;
    if (!*s) return 1;
    if (*s != '.') return 0;
    s++;
  }
  return 0;
}
/* 1 when a is a newer version than b; junk is never newer */
static int version_newer(const char *a, const char *b) {
  long pa[4] = {0}, pb[4] = {0};
  int na, nb;
  if (!parse_version(a, pa, &na) || !parse_version(b, pb, &nb)) return 0;
  for (int i = 0; i < 4; ++i) {
    const long x = i < na ? pa[i] : 0, y = i < nb ? pb[i] : 0;
    if (x != y) return x > y;
  }
  return 0;
}
/* The verified config's optional "sdk_latest": {"version","severity","url"} for the app's platform. A newer version
 * than this SDK's is logged once per client, at warning level, in debug builds only; a missing or malformed field
 * (not an object, a version that is not dotted numbers, severity not a short lower-case word, url not https) is
 * ignored without an error. Tried and rejected: an error code or a callback of its own (the app needs nothing
 * more than the line in its debug log). */
void tf__client_check_latest(TfClient *c) {
  tf_mutex_lock(&c->lock);
  TfJson *j = c->config && c->config_exp > now_unix() ? tf_json_parse(c->config, strlen(c->config), NULL) : NULL;
  tf_mutex_unlock(&c->lock);
  const TfJson *l = tf_json_get(j, "sdk_latest");
  const char *v = tf_json_str(tf_json_get(l, "version")), *sev = tf_json_str(tf_json_get(l, "severity"));
  const char *url = tf_json_str(tf_json_get(l, "url"));
  int ok = l && l->type == TJ_OBJ && v && sev && url && *sev && strlen(sev) <= 32 && !strncmp(url, "https://", 8) && strlen(url) < 512;
  for (const char *p = sev; ok && *p; ++p) ok = (*p >= 'a' && *p <= 'z') || *p == '_' || *p == '-';
  for (const char *p = url; ok && *p; ++p) ok = (unsigned char)*p > 0x20 && (unsigned char)*p < 0x7f;
  if (ok && version_newer(v, TF_SDK_VERSION) && !c->latest_logged && c->o.debug_build) {
    char line[700];
    snprintf(line, sizeof line, "Quran Lab SDK %s is available (%s), you have %s: %s", v, sev, TF_SDK_VERSION, url);
    tf__client_log(c, TF_LOG_WARNING, line);
    c->latest_logged = 1;
  }
  /* kept for tf_client_diagnose, whatever the build */
  snprintf(c->latest, sizeof c->latest, "%s", ok && tf__is_model_version(v) ? v : "");
  tf_json_free(j);
}

const char *tf__client_config_json(TfClient *c) {
  if (!c || !c->config) return NULL;
  if (c->config_exp <= now_unix()) return NULL;
  return c->config;
}

/* SPEC 2a: when the server cannot give the pinned version to this SDK it answers its normal choice plus
 * "pin": {"version", "status": "unavailable"}; a debug build logs that once per answer. */
static void check_pin(TfClient *c) {
  if (!c->pin[0] || !c->o.debug_build) return;
  char status[32];
  if (tf__client_pin_status(c, status, sizeof status) && strcmp(status, "ok")) {
    char line[160];
    snprintf(line, sizeof line, "the pinned model version %s is %s for this SDK; Quran Lab's choice is used", c->pin, status);
    tf__client_log(c, TF_LOG_WARNING, line);
  }
}
/* the config's answer to the pin: its status ("unavailable", ...) copied into out and 1; 0 when the config has no
 * "pin" entry (the pin was honoured, or none was sent) */
int tf__client_pin_status(TfClient *c, char *out, size_t n) {
  tf_mutex_lock(&c->lock);
  TfJson *j = c->config && c->config_exp > now_unix() ? tf_json_parse(c->config, strlen(c->config), NULL) : NULL;
  tf_mutex_unlock(&c->lock);
  const char *st = tf_json_str(tf_json_get(tf_json_get(j, "pin"), "status"));
  if (st) snprintf(out, n, "%s", st);
  tf_json_free(j);
  return st != NULL;
}
/* the signed config's inference.context (SPEC section 3): the portal's Response time setting. Any string here; the
 * Model ignores one its build does not have. */
const char *tf__client_inference_context(TfClient *c, char *out, size_t n) {
  tf_mutex_lock(&c->lock);
  TfJson *j = c->config && c->config_exp > now_unix() ? tf_json_parse(c->config, strlen(c->config), NULL) : NULL;
  tf_mutex_unlock(&c->lock);
  const char *ctx = tf_json_str(tf_json_get(tf_json_get(j, "inference"), "context"));
  const int ok = ctx && *ctx && strlen(ctx) < n;
  if (ok) snprintf(out, n, "%s", ctx);
  tf_json_free(j);
  return ok ? out : NULL;
}
const char *tf__client_config_state(TfClient *c) {
  if (c->config && c->config_exp > now_unix()) return "fresh"; /* a verified config that has not expired is in force */
  return c->config_exp > 0 ? "stale" : "none";                 /* the stored config is past its expires_at */
}

/* flags.<name> of the verified config; `dflt` when absent */
int tf__client_flag(const TfClient *c, const char *name, int dflt) {
  tf_mutex_lock((TfMutex *)&c->lock);
  if (!c->config || c->config_exp <= now_unix()) {
    tf_mutex_unlock((TfMutex *)&c->lock);
    return dflt;
  }
  TfJson *j = tf_json_parse(c->config, strlen(c->config), NULL);
  tf_mutex_unlock((TfMutex *)&c->lock);
  const TfJson *f = tf_json_get(tf_json_get(j, "flags"), name);
  const int v = f && f->type == TJ_BOOL ? f->boolean : dflt;
  tf_json_free(j);
  return v;
}
/* flags.<name> of the verified config when it is a whole number lo to hi; `dflt` when it is absent, anything else (a
 * boolean, a string, a fraction, a number outside the range) or no config is in force */
int tf__client_flag_int(const TfClient *c, const char *name, int lo, int hi, int dflt) {
  tf_mutex_lock((TfMutex *)&c->lock);
  if (!c->config || c->config_exp <= now_unix()) {
    tf_mutex_unlock((TfMutex *)&c->lock);
    return dflt;
  }
  TfJson *j = tf_json_parse(c->config, strlen(c->config), NULL);
  tf_mutex_unlock((TfMutex *)&c->lock);
  const TfJson *f = tf_json_get(tf_json_get(j, "flags"), name);
  const int v = tf_json_is_int(f) && f->num >= lo && f->num <= hi ? (int)f->num : dflt;
  tf_json_free(j);
  return v;
}
/* flags.upload_trust[platform]: "off", "capped" or "attested"; "" when the config has no entry (desktop then uploads at the capped tier) */
void tf__client_upload_trust(const TfClient *c, char *out, size_t n) {
  snprintf(out, n, "%s", "");
  tf_mutex_lock((TfMutex *)&c->lock);
  if (!c->config) {
    tf_mutex_unlock((TfMutex *)&c->lock);
    return;
  }
  TfJson *j = tf_json_parse(c->config, strlen(c->config), NULL);
  tf_mutex_unlock((TfMutex *)&c->lock);
  const char *t = tf_json_str(tf_json_get(tf_json_get(tf_json_get(j, "flags"), "upload_trust"), c->platform));
  if (t) snprintf(out, n, "%s", t);
  tf_json_free(j);
}

/* ------------------------------------------------------------------ telemetry v2 */
/* SDK 3.0.0 (SPEC section 6). What the SDK measures itself, per cohort (one per model version, build, context and
 * backend): sessions, minutes, chunks, aborted, fallbacks, self_test, latency_ms, rtf, cold_start_ms,
 * model_load_ms, thermal, margin, confidence (6.1, kept from 2.x), and takes, audio_s, speech_s, silence_s, tokens
 * per head and compute_s (6.2, each left out when zero). Nothing derived from scoring or settings is left: no
 * preset, word level, classes, madd or words per cohort. What the app reports through tf_session_report_recited
 * and tf_client_report_checks (6.3): ayahs, surah_ayahs, coverage, words, rule_checks, rule_flags, named in the
 * body's "reported" list so the portal labels them. Never audio, text, ids or times. */
#define NB(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const char *const kFb[] = {"non_finite", "deadline", "ane_unavailable", "npu_unavailable", "model_load", "thermal", "memory", "other"};
static const char *const kLat[] = {"le_5", "le_10", "le_20", "le_40", "le_80", "le_160", "le_320", "gt_320"};
static const double kLatV[] = {5, 10, 20, 40, 80, 160, 320};
static const char *const kRtf[] = {"le_0_1", "le_0_25", "le_0_5", "le_1", "gt_1"};
static const double kRtfV[] = {0.1, 0.25, 0.5, 1};
static const char *const kMs[] = {"le_250", "le_500", "le_1000", "le_2000", "le_4000", "gt_4000"};
static const double kMsV[] = {250, 500, 1000, 2000, 4000};
static const char *const kMargin[] = {"le_0_5", "le_1", "le_2", "le_4", "le_8", "gt_8"};
static const double kMarginV[] = {0.5, 1, 2, 4, 8};
static const char *const kConf[] = {"le_0_5", "le_0_7", "le_0_8", "le_0_9", "le_0_95", "gt_0_95"};
static const double kConfV[] = {0.5, 0.7, 0.8, 0.9, 0.95};

typedef struct {
  char revision[32], build[41], context[16], backend[16];
  long long sessions, chunks, aborted, takes;
  double minutes;
  /* 6.2, whole units so that sums stay exact: ms of audio, of speech and of silence; microseconds of compute */
  long long audio_ms, speech_ms, silence_ms, compute_us;
  long long fb[8], st[2], lat[8], rtf[5], cold[6], load[6], thermal[2], margin[6], conf[6];
  long long tokens[TF_HEAD_COUNT];
} Cohort;
#define MAX_RULES 32 /* distinct rules a day; later ones are dropped (SPEC 6.3) */
#define MAX_DOWNLOADS 8 /* builds with a download a day (the server takes at most 8 entries) */
typedef struct {
  char name[25];
  long long checks, flags;
} Rule;
#define MAX_TAKE_IDS 32 /* the recited take ids a day keeps (local only: a take is counted once, 17.6) */
typedef struct {
  Cohort co[16];
  int n;
  long long launches, share_asked, share_yes, share_no;
  /* 4.0 (17.1): measured by the SDK; the app's fallback in app_ayahs and app_surah_ayahs */
  long long ayahs, words, surah_ayahs[115];
  long long app_ayahs, app_surah_ayahs[115];
  long long rt[6]; /* recited_takes: measured, context, none, app, recovered, dropped (17.8; flags.metrics.health false: off) */
  /* 17.9, the app's judgement (flags.metrics.judged false: off) */
  long long j_ayahs, j_correct, j_incorrect, j_mistakes, j_measured, j_unpaired, j_surah[115], c_surah[115];
  long long j_first, j_first_correct, j_repeat, j_repeat_correct, tenure[4][2];
  /* 17.8: measured ayahs per juz, measured ayahs again in a session, finished takes and closed sessions by their
   * seconds of audio, speech by the learner's local hour (ms), the lease's requests */
  long long juz[31], repeat_ayahs, take_n[8], session_n[7], hour_ms[24], lease_ok, lease_failed[6];
  unsigned long long ids[MAX_TAKE_IDS];
  int n_ids;
  Rule rules[MAX_RULES];
  int n_rules;
  /* model downloads: per build, the attempts that ended and the bytes fetched */
  struct {
    char revision[32], build[41];
    long long bytes, success, failed, resumed;
    long long fr[6]; /* 24.8 failure_reasons, kFailure's order */
  } dl[MAX_DOWNLOADS];
  int n_dl;
} Day;
static int day_empty(const Day *d) {
  long long any = d->n + d->launches + d->ayahs + d->words + d->share_asked + d->share_yes + d->share_no + d->n_rules + d->n_dl + d->app_ayahs + d->j_ayahs +
                  d->j_unpaired + d->repeat_ayahs + d->lease_ok;
  for (int s = 1; s <= 114; ++s) any += d->surah_ayahs[s] + d->app_surah_ayahs[s];
  for (int i = 0; i < 6; ++i) any += d->rt[i];
  for (int i = 0; i < 31; ++i) any += d->juz[i];
  for (int i = 0; i < 24; ++i) any += d->hour_ms[i];
  for (int i = 0; i < 8; ++i) any += d->take_n[i];
  for (int i = 0; i < 7; ++i) any += d->session_n[i];
  for (int i = 0; i < 6; ++i) any += d->lease_failed[i];
  return any == 0;
}
static const char *const kRt[6] = {"measured", "context", "none", "app", "recovered", "dropped"};
/* 17.8's histograms and causes, 17.9's keys and tenure buckets (the Apple SDK's RecitedDay, the same names) */
static const char *const kTakeS[8] = {"le_5", "le_15", "le_30", "le_60", "le_120", "le_300", "le_600", "gt_600"};
static const double kTakeSV[7] = {5, 15, 30, 60, 120, 300, 600};
static const char *const kSessionS[7] = {"le_60", "le_300", "le_600", "le_1200", "le_1800", "le_3600", "gt_3600"};
static const double kSessionSV[6] = {60, 300, 600, 1200, 1800, 3600};
static const char *const kLeaseCause[6] = {"offline", "refused", "expired", "clock", "attestation", "other"};
static const char *const kFailure[6] = {"network", "http", "hash_mismatch", "signature", "storage", "other"}; /* 24.8 */
static const char *const kTenure[4] = {"w1", "w2_4", "m2_3", "later"};
static const char *const kJudged[10] = {"ayahs", "correct", "incorrect", "mistakes", "first", "first_correct", "repeat", "repeat_correct", "measured", "unpaired"};
static long long *judged_slot(Day *d, int k) {
  long long *const s[10] = {&d->j_ayahs, &d->j_correct, &d->j_incorrect, &d->j_mistakes, &d->j_first, &d->j_first_correct, &d->j_repeat,
                            &d->j_repeat_correct, &d->j_measured, &d->j_unpaired};
  return s[k];
}
/* the facts sealed into a day's first delta (17.8 learner, learner_gap, streak) and once per week (17.9 trend) */
typedef struct {
  int learner, l[5];
  char gap[8], streak[8];
  int trend;
  char trend_v[16];
} Facts;
static _Thread_local const Facts *t_facts; /* day_body's, while finish_seal writes a delta */

static int bucket(double v, const double *lim, int n) {
  for (int i = 0; i < n; ++i)
    if (v <= lim[i]) return i;
  return n;
}
static void read_arr(const TfJson *o, const char *const *names, int n, long long *out) {
  for (int i = 0; i < n; ++i) {
    const TfJson *v = tf_json_get(o, names[i]);
    out[i] = v && v->type == TJ_NUM ? (long long)v->num : 0;
  }
}
static void write_arr(TfBuf *b, const char *key, const char *const *names, int n, const long long *v) {
  tf_buf_printf(b, ",\"%s\":{", key);
  for (int i = 0; i < n; ++i) tf_buf_printf(b, "%s\"%s\":%lld", i ? "," : "", names[i], v[i]);
  tf_buf_puts(b, "}");
}
static void copy_str(char *dst, size_t n, const TfJson *o, const char *k) {
  const char *s = tf_json_str(tf_json_get(o, k));
  snprintf(dst, n, "%s", s ? s : "");
}
static long long num(const TfJson *o, const char *k) {
  const TfJson *v = tf_json_get(o, k);
  return v && v->type == TJ_NUM ? (long long)v->num : 0;
}
static double numd(const TfJson *o, const char *k) {
  const TfJson *v = tf_json_get(o, k);
  return v && v->type == TJ_NUM ? v->num : 0;
}
/* 1 when name is a rule id: ^[a-z][a-z0-9_]{0,23}$ */
int tf__rule_name_ok(const char *name) {
  if (!name || !(name[0] >= 'a' && name[0] <= 'z')) return 0;
  size_t n = 1;
  for (; name[n]; ++n)
    if (n >= 24 || !((name[n] >= 'a' && name[n] <= 'z') || (name[n] >= '0' && name[n] <= '9') || name[n] == '_')) return 0;
  return 1;
}
static Rule *day_rule(Day *d, const char *name, int add) {
  for (int i = 0; i < d->n_rules; ++i)
    if (!strcmp(d->rules[i].name, name)) return &d->rules[i];
  if (!add || d->n_rules >= MAX_RULES || !tf__rule_name_ok(name)) return NULL;
  Rule *r = &d->rules[d->n_rules++];
  memset(r, 0, sizeof *r);
  snprintf(r->name, sizeof r->name, "%s", name);
  return r;
}

static void day_load_file(const TfClient *c, const char *name, Day *d) {
  memset(d, 0, sizeof *d);
  char *p = spath(c, name), *text = NULL;
  size_t n;
  if (!p || tf_read_file(p, &text, &n)) {
    free(p);
    return;
  }
  free(p);
  TfJson *j = tf_json_parse(text, n, NULL);
  free(text);
  const TfJson *cs = tf_json_get(j, "cohorts");
  for (int i = 0; cs && cs->type == TJ_ARR && i < cs->n && d->n < 16; ++i) {
    const TfJson *o = &cs->kids[i];
    Cohort *x = &d->co[d->n++];
    copy_str(x->revision, sizeof x->revision, o, "revision");
    copy_str(x->build, sizeof x->build, o, "build");
    copy_str(x->context, sizeof x->context, o, "context");
    copy_str(x->backend, sizeof x->backend, o, "backend");
    x->sessions = num(o, "sessions");
    x->chunks = num(o, "chunks");
    x->aborted = num(o, "aborted");
    x->takes = num(o, "takes");
    x->minutes = numd(o, "minutes");
    x->audio_ms = num(o, "audio_ms");
    x->speech_ms = num(o, "speech_ms");
    x->silence_ms = num(o, "silence_ms");
    x->compute_us = num(o, "compute_us");
    read_arr(tf_json_get(o, "fallbacks"), kFb, 8, x->fb);
    static const char *const stn[] = {"pass", "fail"};
    read_arr(tf_json_get(o, "self_test"), stn, 2, x->st);
    read_arr(tf_json_get(o, "latency_ms"), kLat, 8, x->lat);
    read_arr(tf_json_get(o, "rtf"), kRtf, 5, x->rtf);
    read_arr(tf_json_get(o, "cold_start_ms"), kMs, 6, x->cold);
    read_arr(tf_json_get(o, "model_load_ms"), kMs, 6, x->load);
    static const char *const thn[] = {"transitions", "throttled_sessions"};
    read_arr(tf_json_get(o, "thermal"), thn, 2, x->thermal);
    read_arr(tf_json_get(o, "margin"), kMargin, 6, x->margin);
    read_arr(tf_json_get(o, "confidence"), kConf, 6, x->conf);
    for (int h = 0; h < TF_HEAD_COUNT; ++h) x->tokens[h] = num(tf_json_get(o, "tokens"), tf_head_info(h)->name);
  }
  d->launches = num(j, "launches");
  d->ayahs = num(j, "ayahs");
  d->words = num(j, "words");
  d->share_asked = num(j, "share_asked");
  const TfJson *dls = tf_json_get(j, "downloads");
  for (int i = 0; dls && dls->type == TJ_ARR && i < dls->n && d->n_dl < MAX_DOWNLOADS; ++i) {
    const TfJson *x = &dls->kids[i];
    const char *rv = tf_json_str(tf_json_get(x, "revision")), *bd = tf_json_str(tf_json_get(x, "build"));
    if (!rv || !bd) continue;
    memset(&d->dl[d->n_dl], 0, sizeof d->dl[0]);
    snprintf(d->dl[d->n_dl].revision, sizeof d->dl[0].revision, "%s", rv);
    snprintf(d->dl[d->n_dl].build, sizeof d->dl[0].build, "%s", bd);
    d->dl[d->n_dl].bytes = num(x, "bytes");
    d->dl[d->n_dl].success = num(x, "success");
    d->dl[d->n_dl].failed = num(x, "failed");
    d->dl[d->n_dl].resumed = num(x, "resumed");
    read_arr(tf_json_get(x, "failure_reasons"), kFailure, 6, d->dl[d->n_dl].fr);
    d->n_dl++;
  }
  d->share_yes = num(j, "share_yes");
  d->share_no = num(j, "share_no");
  static const char *const smaps[4] = {"surah_ayahs", "app_surah_ayahs", "judged_surah_ayahs", "correct_surah_ayahs"};
  long long *const sdst[4] = {d->surah_ayahs, d->app_surah_ayahs, d->j_surah, d->c_surah};
  for (int m = 0; m < 4; ++m) {
    const TfJson *sa = tf_json_get(j, smaps[m]);
    for (int i = 0; sa && sa->type == TJ_OBJ && i < sa->n; ++i) {
      const int s = atoi(sa->kids[i].key);
      if (s >= 1 && s <= 114 && sa->kids[i].type == TJ_NUM) sdst[m][s] = (long long)sa->kids[i].num;
    }
  }
  d->app_ayahs = num(j, "app_ayahs");
  read_arr(tf_json_get(j, "recited_takes"), kRt, 6, d->rt);
  const TfJson *jd = tf_json_get(j, "judged");
  d->j_ayahs = num(jd, "ayahs");
  d->j_correct = num(jd, "correct");
  d->j_incorrect = num(jd, "incorrect");
  d->j_mistakes = num(jd, "mistakes");
  d->j_measured = num(jd, "measured");
  d->j_unpaired = num(jd, "unpaired");
  {
    const TfJson *jd2 = tf_json_get(j, "judged");
    for (int k = 4; k < 8; ++k) *judged_slot(d, k) = num(jd2, kJudged[k]);
    const TfJson *tn = tf_json_get(j, "judged_tenure");
    for (int k = 0; k < 4; ++k) {
      d->tenure[k][0] = num(tf_json_get(tn, kTenure[k]), "judged");
      d->tenure[k][1] = num(tf_json_get(tn, kTenure[k]), "correct");
    }
    const TfJson *jz = tf_json_get(j, "juz_ayahs"), *hm = tf_json_get(j, "hour_speech_ms");
    for (int i = 0; jz && jz->type == TJ_OBJ && i < jz->n; ++i) {
      const int k = atoi(jz->kids[i].key);
      if (k >= 1 && k <= 30 && jz->kids[i].type == TJ_NUM) d->juz[k] = (long long)jz->kids[i].num;
    }
    for (int i = 0; hm && hm->type == TJ_OBJ && i < hm->n; ++i) {
      const int k = atoi(hm->kids[i].key);
      if (k >= 0 && k <= 23 && hm->kids[i].type == TJ_NUM) d->hour_ms[k] = (long long)hm->kids[i].num;
    }
    d->repeat_ayahs = num(j, "repeat_ayahs");
    read_arr(tf_json_get(j, "take_s"), kTakeS, 8, d->take_n);
    read_arr(tf_json_get(j, "session_s"), kSessionS, 7, d->session_n);
    const TfJson *ls = tf_json_get(j, "lease");
    d->lease_ok = num(ls, "ok");
    read_arr(tf_json_get(ls, "failed"), kLeaseCause, 6, d->lease_failed);
  }
  const TfJson *ti = tf_json_get(j, "take_ids");
  for (int i = 0; ti && ti->type == TJ_ARR && i < ti->n && d->n_ids < MAX_TAKE_IDS; ++i)
    if (tf_json_str(&ti->kids[i])) d->ids[d->n_ids++] = strtoull(ti->kids[i].str, NULL, 16);
  const TfJson *rc = tf_json_get(j, "rule_checks"), *rf = tf_json_get(j, "rule_flags");
  for (int i = 0; rc && rc->type == TJ_OBJ && i < rc->n; ++i) {
    Rule *r = rc->kids[i].type == TJ_NUM ? day_rule(d, rc->kids[i].key, 1) : NULL;
    if (!r) continue;
    r->checks = (long long)rc->kids[i].num;
    r->flags = num(rf, r->name);
  }
  tf_json_free(j);
}

static void day_load(const TfClient *c, const char *day, Day *d) {
  char name[64];
  snprintf(name, sizeof name, "telemetry-%s.json", day);
  day_load_file(c, name, d);
}

static void write_count(TfBuf *b, const char *key, long long v) {
  if (v) tf_buf_printf(b, ",\"%s\":%lld", key, v);
}
/* whole milliseconds sent as seconds (ms / 1000); left out when zero */
static void write_ms_as_s(TfBuf *b, const char *key, long long ms) {
  if (ms <= 0) return;
  tf_buf_printf(b, ",\"%s\":", key);
  tf_buf_num(b, (double)ms / 1000.0);
}
/* the v2 body for a day (closed schema: exactly these fields; zero counts and empty objects are left out, the
 * vectors' telemetry_30.json). report_id NULL: the pending counts on disk, not a delta to send; there the 6.2
 * sums are kept in their whole units (audio_ms, speech_ms, silence_ms, compute_us) so that a day's deltas add up
 * exactly, and a sealed delta carries them as seconds. */
static char *day_body(const TfClient *c, const char *day, const Day *d, const char *report_id) {
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "{\"v\":2,\"day\":\"%s\",\"product\":\"native\",\"platform\":", day);
  tf_buf_json_str(&b, c->platform);
  tf_buf_printf(&b, ",\"sdk_version\":\"%s\",\"app_version\":", TF_SDK_VERSION);
  tf_buf_json_str(&b, c->app_version);
  tf_buf_printf(&b, ",\"device\":{\"class\":\"x86_64\",\"soc\":\"x86_64\",\"os_major\":%d,\"ram_tier\":\"%s\"},\"cohorts\":[", os_major(),
                ram_tier());
  for (int i = 0; i < d->n; ++i) {
    const Cohort *x = &d->co[i];
    if (i) tf_buf_puts(&b, ",");
    /* SPEC 2a: "revision" carries the model version, "build" the build id */
    tf_buf_puts(&b, "{\"revision\":");
    tf_buf_json_str(&b, x->revision);
    if (x->build[0]) {
      tf_buf_puts(&b, ",\"build\":");
      tf_buf_json_str(&b, x->build);
    }
    tf_buf_puts(&b, ",\"context\":");
    tf_buf_json_str(&b, x->context);
    tf_buf_puts(&b, ",\"backend\":");
    tf_buf_json_str(&b, x->backend);
    tf_buf_printf(&b, ",\"sessions\":%lld,\"minutes\":", x->sessions);
    tf_buf_num(&b, round(x->minutes * 100) / 100);
    tf_buf_printf(&b, ",\"chunks\":%lld,\"aborted\":%lld", x->chunks, x->aborted);
    write_arr(&b, "fallbacks", kFb, 8, x->fb);
    static const char *const stn[] = {"pass", "fail"};
    write_arr(&b, "self_test", stn, 2, x->st);
    write_arr(&b, "latency_ms", kLat, 8, x->lat);
    write_arr(&b, "rtf", kRtf, 5, x->rtf);
    write_arr(&b, "cold_start_ms", kMs, 6, x->cold);
    write_arr(&b, "model_load_ms", kMs, 6, x->load);
    static const char *const thn[] = {"transitions", "throttled_sessions"};
    write_arr(&b, "thermal", thn, 2, x->thermal);
    write_arr(&b, "margin", kMargin, 6, x->margin);
    write_arr(&b, "confidence", kConf, 6, x->conf);
    write_count(&b, "takes", x->takes);
    if (report_id) {
      write_ms_as_s(&b, "audio_s", x->audio_ms);
      write_ms_as_s(&b, "speech_s", x->speech_ms);
      write_ms_as_s(&b, "silence_s", x->silence_ms);
    } else {
      write_count(&b, "audio_ms", x->audio_ms);
      write_count(&b, "speech_ms", x->speech_ms);
      write_count(&b, "silence_ms", x->silence_ms);
      write_count(&b, "compute_us", x->compute_us);
    }
    int first = 1;
    for (int h = 0; h < TF_HEAD_COUNT; ++h)
      if (x->tokens[h]) {
        tf_buf_printf(&b, "%s\"%s\":%lld", first ? ",\"tokens\":{" : ",", tf_head_info(h)->name, x->tokens[h]);
        first = 0;
      }
    if (!first) tf_buf_puts(&b, "}");
    if (report_id) write_ms_as_s(&b, "compute_s", (x->compute_us + 500) / 1000); /* microseconds, rounded to ms */
    tf_buf_puts(&b, "}");
  }
  tf_buf_puts(&b, "]");
  int first = 1, any_surah = 0;
  /* downloads: revision carries the model version, build the build id; left out when there was none */
  for (int i = 0; i < d->n_dl; ++i) {
    tf_buf_printf(&b, "%s{\"revision\":\"%s\",\"build\":\"%s\",\"profile\":\"%s-cpu\",\"bytes\":%lld,\"success\":%lld,\"failed\":%lld,\"resumed\":%lld",
                  i ? "," : ",\"downloads\":[", d->dl[i].revision, d->dl[i].build, tf__client_platform(c), d->dl[i].bytes, d->dl[i].success, d->dl[i].failed,
                  d->dl[i].resumed);
    long long any_fr = 0;
    for (int k = 0; k < 6; ++k) any_fr += d->dl[i].fr[k];
    if (any_fr) write_arr(&b, "failure_reasons", kFailure, 6, d->dl[i].fr); /* 24.8, the reasons with a count */
    tf_buf_puts(&b, "}");
  }
  if (d->n_dl) tf_buf_puts(&b, "]");
  if (report_id) tf_buf_printf(&b, ",\"report_id\":\"%s\"", report_id);
  write_count(&b, "launches", d->launches);
  write_count(&b, "ayahs", d->ayahs > 0 ? d->ayahs : 0);
  write_count(&b, "app_ayahs", d->app_ayahs > 0 ? d->app_ayahs : 0);
  write_count(&b, "words", d->words > 0 ? d->words : 0);
  /* 4.0: surah_ayahs measured by the SDK (never listed in reported), app_surah_ayahs the app's fallback (listed);
   * coverage is not sent any more (17.1) */
  int any_app_surah = 0;
  for (int pass = 0; pass < 2; ++pass) {
    const long long *v = pass ? d->app_surah_ayahs : d->surah_ayahs;
    first = 1;
    for (int s = 1; s <= 114; ++s)
      if (v[s] > 0) {
        tf_buf_printf(&b, "%s\"%d\":%lld", first ? (pass ? ",\"app_surah_ayahs\":{" : ",\"surah_ayahs\":{") : ",", s, v[s]);
        first = 0;
        if (pass) any_app_surah = 1;
        else any_surah = 1;
      }
    if (!first) tf_buf_puts(&b, "}");
  }
  (void)any_surah;
  /* 17.8 and 17.9, by group (the draft: absent = on; the signed config's flags.metrics.<group> false turns one off,
   * and then nothing of it is sent): juz (juz_ayahs), hours (hour_speech_s), lengths (take_s, session_s), learner
   * (learner, learner_gap, streak), repeats (repeat_ayahs), health (recited_takes, lease), judged (judged,
   * judged_surah_ayahs, correct_surah_ayahs, judged_tenure, trend). On disk every count is kept as counted (speech in
   * ms); the learner facts and the trend exist only in a sealed delta (finish_seal). recited_takes has its counts
   * that are not zero; `context` from the Core's result (R5), `dropped` never (the Core does not count it). */
  const int disk = report_id == NULL;
#define ON(g) (disk || tf__client_metric_on(c, g))
  if (ON("juz")) {
    first = 1;
    for (int k = 1; k <= 30; ++k)
      if (d->juz[k] > 0) {
        tf_buf_printf(&b, "%s\"%d\":%lld", first ? ",\"juz_ayahs\":{" : ",", k, d->juz[k]);
        first = 0;
      }
    if (!first) tf_buf_puts(&b, "}");
  }
  if (ON("hours")) {
    first = 1;
    for (int h = 0; h < 24; ++h)
      if (d->hour_ms[h] > 0) {
        tf_buf_printf(&b, "%s\"%d\":", first ? (disk ? ",\"hour_speech_ms\":{" : ",\"hour_speech_s\":{") : ",", h);
        if (disk) tf_buf_printf(&b, "%lld", d->hour_ms[h]);
        else tf_buf_num(&b, d->hour_ms[h] > 86400000LL ? 86400.0 : (double)d->hour_ms[h] / 1000.0);
        first = 0;
      }
    if (!first) tf_buf_puts(&b, "}");
  }
  if (ON("lengths")) {
    long long any = 0;
    for (int k = 0; k < 8; ++k) any += d->take_n[k];
    if (any) write_arr(&b, "take_s", kTakeS, 8, d->take_n);
    any = 0;
    for (int k = 0; k < 7; ++k) any += d->session_n[k];
    if (any) write_arr(&b, "session_s", kSessionS, 7, d->session_n);
  }
  if (ON("repeats")) write_count(&b, "repeat_ayahs", d->repeat_ayahs);
  if (ON("health")) {
    first = 1;
    for (int i = 0; i < 6; ++i)
      if (d->rt[i] > 0) {
        tf_buf_printf(&b, "%s\"%s\":%lld", first ? ",\"recited_takes\":{" : ",", kRt[i], d->rt[i]);
        first = 0;
      }
    if (!first) tf_buf_puts(&b, "}");
    long long failed = 0;
    for (int i = 0; i < 6; ++i) failed += d->lease_failed[i];
    if (d->lease_ok || failed) {
      tf_buf_puts(&b, ",\"lease\":{");
      if (d->lease_ok) tf_buf_printf(&b, "\"ok\":%lld", d->lease_ok);
      first = 1;
      for (int i = 0; i < 6; ++i)
        if (d->lease_failed[i] > 0) {
          tf_buf_printf(&b, "%s\"%s\":%lld", first ? (d->lease_ok ? ",\"failed\":{" : "\"failed\":{") : ",", kLeaseCause[i], d->lease_failed[i]);
          first = 0;
        }
      if (!first) tf_buf_puts(&b, "}");
      tf_buf_puts(&b, "}");
    }
  }
  const int judged = (d->j_ayahs > 0 || d->j_unpaired > 0) && ON("judged");
  int any_tenure = 0;
  if (judged) {
    first = 1;
    for (int k = 0; k < 10; ++k)
      if (*judged_slot((Day *)d, k) > 0) {
        tf_buf_printf(&b, "%s\"%s\":%lld", first ? ",\"judged\":{" : ",", kJudged[k], *judged_slot((Day *)d, k));
        first = 0;
      }
    if (!first) tf_buf_puts(&b, "}");
    for (int pass = 0; pass < 2; ++pass) {
      const long long *v = pass ? d->c_surah : d->j_surah;
      first = 1;
      for (int s = 1; s <= 114; ++s)
        if (v[s] > 0) {
          tf_buf_printf(&b, "%s\"%d\":%lld", first ? (pass ? ",\"correct_surah_ayahs\":{" : ",\"judged_surah_ayahs\":{") : ",", s, v[s]);
          first = 0;
        }
      if (!first) tf_buf_puts(&b, "}");
    }
    first = 1;
    for (int k = 0; k < 4; ++k)
      if (d->tenure[k][0] > 0) {
        tf_buf_printf(&b, "%s\"%s\":{\"judged\":%lld,\"correct\":%lld}", first ? ",\"judged_tenure\":{" : ",", kTenure[k], d->tenure[k][0],
                      d->tenure[k][1] < d->tenure[k][0] ? d->tenure[k][1] : d->tenure[k][0]);
        first = 0;
        any_tenure = 1;
      }
    if (!first) tf_buf_puts(&b, "}");
  }
  const Facts *fx = disk ? NULL : t_facts;
  if (fx && fx->learner && ON("learner")) {
    tf_buf_printf(&b, ",\"learner\":{\"active\":%d,\"new\":%d,\"d1\":%d,\"d7\":%d,\"d30\":%d},\"learner_gap\":{\"%s\":1},\"streak\":{\"%s\":1}", fx->l[0],
                  fx->l[1], fx->l[2], fx->l[3], fx->l[4], fx->gap, fx->streak);
  }
  const int trend = fx && fx->trend && ON("judged");
  if (trend) tf_buf_printf(&b, ",\"trend\":{\"%s\":1}", fx->trend_v);
#undef ON
  if (disk && d->n_ids) {
    tf_buf_puts(&b, ",\"take_ids\":[");
    for (int i = 0; i < d->n_ids; ++i) tf_buf_printf(&b, "%s\"%016llx\"", i ? "," : "", d->ids[i]);
    tf_buf_puts(&b, "]");
  }
  /* every flags key is a checks key with a count no larger (the report call refuses flagged > checked) */
  int any_checks = 0, any_flags = 0;
  for (int f = 0; f < 2; ++f) {
    first = 1;
    for (int k = 0; k < d->n_rules; ++k) {
      const long long v = f ? d->rules[k].flags : d->rules[k].checks;
      if (v <= 0) continue;
      tf_buf_printf(&b, "%s\"%s\":%lld", first ? (f ? ",\"rule_flags\":{" : ",\"rule_checks\":{") : ",", d->rules[k].name, v);
      first = 0;
      if (f) any_flags = 1;
      else any_checks = 1;
    }
    if (!first) tf_buf_puts(&b, "}");
  }
  write_count(&b, "share_asked", d->share_asked);
  write_count(&b, "share_yes", d->share_yes);
  write_count(&b, "share_no", d->share_no);
  /* which of the fields above the app reported (only those present) */
  {
    const struct {
      const char *name;
      int present;
    } rep[] = {{"app_ayahs", d->app_ayahs > 0}, {"app_surah_ayahs", any_app_surah}, {"words", d->words > 0},
               {"rule_checks", any_checks},      {"rule_flags", any_flags},           {"judged", judged && d->j_ayahs + d->j_unpaired > 0},
               {"judged_surah_ayahs", judged && d->j_ayahs > 0}, {"correct_surah_ayahs", judged && d->j_correct > 0},
               {"judged_tenure", any_tenure},    {"trend", trend}};
    first = 1;
    for (int i = 0; i < NB(rep); ++i)
      if (rep[i].present) {
        tf_buf_printf(&b, "%s\"%s\"", first ? ",\"reported\":[" : ",", rep[i].name);
        first = 0;
      }
    if (!first) tf_buf_puts(&b, "]");
  }
  tf_buf_puts(&b, "}");
  return tf_buf_take(&b);
}

static void day_save(const TfClient *c, const char *day, const Day *d) {
  char name[64];
  snprintf(name, sizeof name, "telemetry-%s.json", day);
  char *p = spath(c, name), *body = day_body(c, day, d, NULL);
  if (p && body) tf_write_file(p, body, strlen(body));
  free(p);
  free(body);
}

/* SDK 4.0 (owner, 11-16): always on for every app; the signed config's flags.analytics false is the only off */
int tf__client_telemetry_on(const TfClient *c) { return tf__client_flag(c, "analytics", 1); }

/* +n to one of today's top-level counters: launches, share_asked, share_yes, share_no */
void tf__telemetry_add(TfClient *c, const char *field, long long n) {
  if (!c || !tf__client_telemetry_on(c)) return;
  tf_mutex_lock(&c->tlock);
  char day[11];
  day_utc(day, 0);
  Day d;
  day_load(c, day, &d);
  if (!strcmp(field, "launches")) d.launches += n;
  else if (!strcmp(field, "share_asked")) d.share_asked += n;
  else if (!strcmp(field, "share_yes")) d.share_yes += n;
  else if (!strcmp(field, "share_no")) d.share_no += n;
  day_save(c, day, &d);
  tf_mutex_unlock(&c->tlock);
}

void tf__client_count_download(TfClient *c, const char *version, const char *build, long long bytes, int ok, int resumed) {
  if (!c || !version || !build || !tf__client_telemetry_on(c)) return;
  tf_mutex_lock(&c->tlock);
  char day[11];
  day_utc(day, 0);
  Day d;
  day_load(c, day, &d);
  int k = 0;
  while (k < d.n_dl && (strcmp(d.dl[k].revision, version) || strcmp(d.dl[k].build, build))) ++k;
  if (k == d.n_dl && d.n_dl < MAX_DOWNLOADS) {
    memset(&d.dl[k], 0, sizeof d.dl[k]);
    snprintf(d.dl[k].revision, sizeof d.dl[k].revision, "%s", version);
    snprintf(d.dl[k].build, sizeof d.dl[k].build, "%s", build);
    d.n_dl++;
  }
  if (k < d.n_dl) {
    d.dl[k].bytes += bytes > 0 ? bytes : 0;
    d.dl[k].success += ok != 0;
    d.dl[k].failed += ok == 0;
    d.dl[k].resumed += resumed != 0;
    if (!ok) {
      /* 24.8: the failure's reason (the cause the failed call ended on; none known: other) */
      const char *fr = tf__rep_failure_reason(tf__cause_reason());
      for (int i = 0; i < 6; ++i) d.dl[k].fr[i] += !strcmp(kFailure[i], fr);
    }
    day_save(c, day, &d);
  }
  tf_mutex_unlock(&c->tlock);
}

/* App-reported (SPEC 6.3), counted into today's sums as the app's fallback (4.0: app_ayahs). Validated by the
 * caller. */
void tf__client_count_recited(TfClient *c, int surah, int ayah_from, int ayah_to) {
  TfRecitedCount k;
  memset(&k, 0, sizeof k);
  k.source = 2;
  k.surah = surah;
  k.first = ayah_from;
  k.last = ayah_to;
  tf__client_count_take_range(c, &k);
}
/* One take's recited range into the day (its own day: a take finished after a restart counts on the day it began):
 * measured (ayahs, surah_ayahs), the app's (app_ayahs, app_surah_ayahs), or nothing, and the app's judgement (17.9;
 * each rule of an incorrect ayah adds 1 to rule_checks and rule_flags). A take id is counted once: the day keeps the
 * ids it counted in the same write as the counts. */
/* the session's sets of ayahs (17.8 repeat_ayahs; 17.9 first and repeat): key surah * 300 + ayah, in memory */
static int sess_bit(uint8_t *set, int surah, int ayah) {
  const int k = surah * 300 + ayah;
  const int was = set[k >> 3] >> (k & 7) & 1;
  set[k >> 3] |= (uint8_t)(1u << (k & 7));
  return was;
}

/* The app's outcomes of a take (17.9) on the ayahs counted for it (`measured`: the detector's range; else the app's
 * own): the whole-range outcome on every ayah per_ayah does not name; a per_ayah entry outside the measured range is
 * dropped (unpaired). Each judged ayah counts toward first or repeat in its session, the install's tenure and its UTC
 * week (judged_tenure, trend; the week's counts go to the Core's journal in one call per take). A take counted
 * through a refrain tie has no sure ayah number: its outcomes count toward the surah and the totals, not toward first
 * or repeat. A range outcome's mistakes count once for the range (decided 11-14, as Apple); each rule of an incorrect
 * ayah adds 1 to that rule's checks and flags. The Apple SDK's judge(), ported. */
static void judge(TfClient *c, Day *d, TfLearner *j, const char *day, const TfRecitedCount *r, int measured) {
  const TfRecited *a = r->app;
  if (!a || !a->surah || (!a->whole.set && !a->n_per)) return;
  for (int i = 0; measured && i < a->n_per; ++i)
    if (a->surah != r->surah || a->per[i].ayah < r->first || a->per[i].ayah > r->last) d->j_unpaired++;
  if (a->whole.set && !a->whole.correct) d->j_mistakes += a->whole.mistakes;
  const int ten = tf__learner_tenure(j, day);
  int wk_judged = 0, wk_correct = 0;
  for (int ay = r->first; ay <= r->last; ++ay) {
    const TfPerAyah *pe = NULL;
    for (int i = 0; a->surah == r->surah && i < a->n_per; ++i)
      if (a->per[i].ayah == ay) pe = &a->per[i];
    const TfOutcomeX *o = pe ? &pe->o : a->whole.set ? &a->whole : NULL;
    if (!o) continue;
    const int tie = r->ties > 0;
    const int first = !tie && !sess_bit(c->sess_judged, r->surah, ay);
    d->j_ayahs++;
    d->j_surah[r->surah]++;
    d->tenure[ten][0]++;
    wk_judged++;
    wk_correct += o->correct ? 1 : 0;
    if (measured) d->j_measured++;
    if (first) d->j_first++;
    else if (!tie) d->j_repeat++;
    if (o->correct) {
      d->j_correct++;
      if (first) d->j_first_correct++;
      else if (!tie) d->j_repeat_correct++;
      d->c_surah[r->surah]++;
      d->tenure[ten][1]++;
    } else {
      d->j_incorrect++;
      if (pe) d->j_mistakes += pe->o.mistakes;
      for (int i = 0; i < o->n_rules; ++i) {
        int dup = 0;
        for (int k = 0; k < i; ++k) dup |= !strcmp(o->rules[k], o->rules[i]);
        Rule *ru = dup ? NULL : day_rule(d, o->rules[i], 1);
        if (ru) {
          ru->checks++;
          ru->flags++;
        }
      }
    }
  }
  tf__learner_judged(c, j, day, wk_judged, wk_correct);
}

/* A take counted through a refrain tie (R5: equally good ranges besides the one counted) counts toward a juz only when
 * every copy lies in that juz (17.8). Without the session context every copy is in the counted surah (a tie across
 * surahs counts nothing), so a surah inside one juz settles it; a take the context decided may have copies elsewhere. */
static int tie_in_one_juz(const TfRecitedCount *r) {
  return !r->context && tf__juz_of(r->surah, 1) == tf__juz_of(r->surah, tf__ayah_count(r->surah));
}

/* One take's recited range into the day (its own day: a take finished after a restart counts on the day it began):
 * measured (ayahs, surah_ayahs, juz_ayahs, repeat_ayahs), the app's (app_ayahs, app_surah_ayahs), or nothing, and
 * the app's judgement (17.9). `late`: the app's report of a measured take that came after the take ended: its outcomes
 * only. A take id is counted once: the day keeps the ids it counted in the same write as the counts. Each count is
 * kept whatever the switches; a group switched off is not sent (day_body). */
void tf__client_count_take_range(TfClient *c, const TfRecitedCount *r) {
  if (!c || !r || !tf__client_telemetry_on(c)) return;
  const int n = r->source && r->surah >= 1 && r->surah <= 114 && r->first >= 1 && r->last >= r->first && r->last <= 286 ? r->last - r->first + 1 : 0;
  tf_mutex_lock(&c->tlock);
  char day[11];
  if (r->day && strlen(r->day) == 10) snprintf(day, sizeof day, "%s", r->day);
  else day_utc(day, 0);
  Day d;
  day_load(c, day, &d);
  int seen = 0;
  for (int i = 0; r->take_id && i < d.n_ids; ++i) seen |= d.ids[i] == r->take_id;
  if (!seen) {
    TfLearner j;
    tf__learner_load(c, &j);
    if (n && r->source == 1 && !r->late) {
      d.ayahs += n;
      d.surah_ayahs[r->surah] += n;
      d.rt[0]++;
      if (r->context) d.rt[1]++;
      /* a take counted through a refrain tie: in a juz only when every copy is (tie_in_one_juz), never a repeat */
      const int tie = r->ties > 0, in_juz = !tie || tie_in_one_juz(r);
      for (int ay = r->first; ay <= r->last; ++ay) {
        if (in_juz) d.juz[tf__juz_of(r->surah, ay)]++;
        if (!tie && sess_bit(c->sess_counted, r->surah, ay)) d.repeat_ayahs++;
      }
    } else if (n && r->source == 2) {
      d.app_ayahs += n;
      d.app_surah_ayahs[r->surah] += n;
      d.rt[3]++;
    } else if (r->take_id) {
      d.rt[2]++;
    }
    if (r->recovered) d.rt[4]++;
    if (n) judge(c, &d, &j, day, r, r->source == 1);
    if (r->take_id) {
      if (d.n_ids == MAX_TAKE_IDS) {
        memmove(d.ids, d.ids + 1, sizeof d.ids[0] * (MAX_TAKE_IDS - 1));
        d.n_ids--;
      }
      d.ids[d.n_ids++] = r->take_id;
    }
    day_save(c, day, &d);
  }
  tf_mutex_unlock(&c->tlock);
}

/* 17.8 `lease`: one lease request's outcome (0 ok; 1 offline, 2 refused, 3 expired, 4 clock, 5 attestation, 6 other) */
void tf__client_count_lease(TfClient *c, int outcome) {
  if (!c || !tf__client_telemetry_on(c) || outcome < 0 || outcome > 6) return;
  tf_mutex_lock(&c->tlock);
  char day[11];
  day_utc(day, 0);
  Day d;
  day_load(c, day, &d);
  if (outcome == 0) d.lease_ok++;
  else d.lease_failed[outcome - 1]++;
  day_save(c, day, &d);
  tf_mutex_unlock(&c->tlock);
}

int tf__learner_test_kill; /* tests: the Client ends as a killed process would, its open session left on disk */
/* The analytics session ends (the Model closed, the Client ended, or a process that ended left one open, closed at the
 * next start): its seconds of audio into session_s, and the session's sets of counted and judged ayahs start again.
 * The journal is written first: a kill between the two writes loses the session rather than counting it twice. */
void tf__client_session_close(TfClient *c) {
  if (!c) return;
  tf_mutex_lock(&c->tlock);
  TfLearner j;
  tf__learner_load(c, &j);
  if (j.session_open) {
    const long long ms = j.session_ms;
    j.session_open = 0;
    j.session_ms = 0;
    tf__learner_save_session(c, &j);
    if (ms > 0 && tf__client_telemetry_on(c)) {
      char day[11];
      day_utc(day, 0);
      Day d;
      day_load(c, day, &d);
      d.session_n[bucket((double)ms / 1000.0, kSessionSV, 6)]++;
      day_save(c, day, &d);
    }
  }
  memset(c->sess_counted, 0, sizeof c->sess_counted);
  memset(c->sess_judged, 0, sizeof c->sess_judged);
  tf_mutex_unlock(&c->tlock);
}
/* 1 when a take id is among the ids the last 8 days counted */
int tf__client_take_counted(TfClient *c, uint64_t take_id) {
  if (!c || !take_id) return 0;
  int seen = 0;
  tf_mutex_lock(&c->tlock);
  for (int back = 0; back < 8 && !seen; ++back) {
    char day[11];
    day_utc(day, -86400LL * back);
    Day d;
    day_load(c, day, &d);
    for (int i = 0; i < d.n_ids; ++i) seen |= d.ids[i] == take_id;
  }
  tf_mutex_unlock(&c->tlock);
  return seen;
}
/* flags.metrics.<group> of the verified config (17.8): on unless it is false. The whole of analytics is off apart
 * from this (tf__client_telemetry_on). */
int tf__client_metric_on(const TfClient *c, const char *group) {
  tf_mutex_lock((TfMutex *)&c->lock);
  if (!c->config || c->config_exp <= now_unix()) {
    tf_mutex_unlock((TfMutex *)&c->lock);
    return 1;
  }
  TfJson *j = tf_json_parse(c->config, strlen(c->config), NULL);
  tf_mutex_unlock((TfMutex *)&c->lock);
  const TfJson *f = tf_json_get(tf_json_get(tf_json_get(j, "flags"), "metrics"), group);
  const int v = !(f && f->type == TJ_BOOL && !f->boolean);
  tf_json_free(j);
  return v;
}
/* App-reported (SPEC 6.3): the app's own judge checked `checked` items of a rule and flagged `flagged` of them.
 * Validated by the caller. A call with checked 0 counts nothing and takes no place among the day's rules; a 33rd
 * rule of the day is dropped. Returns 1 when the report was dropped, else 0. */
int tf__client_count_checks(TfClient *c, const char *rule, int checked, int flagged) {
  if (!c || !tf__client_telemetry_on(c) || checked <= 0) return 0;
  tf_mutex_lock(&c->tlock);
  char day[11];
  day_utc(day, 0);
  Day d;
  day_load(c, day, &d);
  Rule *r = day_rule(&d, rule, 1);
  if (r) {
    r->checks += checked;
    r->flags += flagged;
    if (!strcmp(rule, "word")) d.words += checked;
    day_save(c, day, &d);
  }
  tf_mutex_unlock(&c->tlock);
  return r ? 0 : 1;
}

/* today's counts as the delta that would be sent now (a fixed report id of zeros), for the tests; caller frees */
char *tf__client_day_body(TfClient *c) {
  tf_mutex_lock(&c->tlock);
  char day[11];
  day_utc(day, 0);
  Day d;
  day_load(c, day, &d);
  char *body = day_body(c, day, &d, "00000000000000000000000000000000");
  tf_mutex_unlock(&c->tlock);
  return body;
}

#define TF_DELTA_S (15 * 60) /* a delta every 15 minutes while a client is open (Quran Lab, 11-09) */
#define TF_CORE_TICK_S 10 /* the Core's tick: the recited journal is written at least this often (17.6) */
static void seal(TfClient *c, int include_today);

void tf__client_session_finished(TfClient *c, const TfSessionStats *st) {
  if (!c || !st) return;
  tf__sharing_session(c, st->end, st->speech_frames); /* 4.0: how the take ended and its speech (20.2) */
  if (!tf__client_telemetry_on(c)) return;
  tf_mutex_lock(&c->tlock);
  char day[11];
  day_utc(day, 0);
  Day d;
  day_load(c, day, &d);
  /* the cohort: model version, build, context, backend (always the CPU engine here) */
  Cohort key;
  memset(&key, 0, sizeof key);
  snprintf(key.backend, sizeof key.backend, "cpu");
  if (st->model) {
    snprintf(key.revision, sizeof key.revision, "%s", tf_model_version(st->model));
    snprintf(key.build, sizeof key.build, "%s", tf_model_build(st->model));
  } else {
    snprintf(key.revision, sizeof key.revision, "%s", st->version ? st->version : "");
    snprintf(key.build, sizeof key.build, "%s", st->build ? st->build : "");
  }
  snprintf(key.context, sizeof key.context, "%s", st->context ? st->context : "");
  Cohort *x = NULL;
  for (int i = 0; i < d.n; ++i)
    if (!strcmp(d.co[i].revision, key.revision) && !strcmp(d.co[i].build, key.build) && !strcmp(d.co[i].context, key.context) &&
        !strcmp(d.co[i].backend, key.backend))
      x = &d.co[i];
  if (!x) {
    if (d.n < 16) x = &d.co[d.n++];
    else { /* merged into the smallest */
      x = &d.co[0];
      for (int i = 1; i < d.n; ++i)
        if (d.co[i].sessions < x->sessions) x = &d.co[i];
    }
    if (!x->sessions) *x = key;
  }
  x->sessions++;
  x->minutes += st->audio_s / 60.0;
  x->chunks += st->chunks;
  x->aborted += st->aborted != 0;
  if (!st->aborted) {
    /* 6.2: a take counts when it ended with its Heads; a cancelled or failed one adds nothing here */
    x->takes++;
    x->audio_ms += st->audio_ms;
    x->speech_ms += 40LL * st->speech_frames;
    x->silence_ms += 40LL * st->silence_frames;
    x->compute_us += st->compute_us;
    for (int h = 0; h < TF_HEAD_COUNT; ++h) x->tokens[h] += st->tokens[h];
  }
  if (st->fallback > 0) {
    static const int map[] = {0, 0, 1, 4, 6, 5, 7};
    x->fb[map[st->fallback < 7 ? st->fallback : 6]]++;
  }
  if (st->chunks > 0 && st->compute_s > 0) {
    x->lat[bucket(1000.0 * st->compute_s / st->chunks, kLatV, 7)] += st->chunks;
    x->rtf[bucket(st->audio_s > 0 ? st->compute_s / st->audio_s : 9, kRtfV, 4)] += st->chunks;
  }
  if (st->cold_start && st->model_load_ms > 0) x->cold[bucket(st->model_load_ms, kMsV, 5)]++;
  if (st->model_load_ms > 0) x->load[bucket(st->model_load_ms, kMsV, 5)]++;
  if (st->decoded) {
    /* margin and confidence per emitted token, from the decoded layer */
    for (int i = 0; i < st->decoded->n_tokens; ++i) {
      const TfToken *t = &st->decoded->tokens[i];
      x->margin[bucket(t->margin, kMarginV, 5)]++;
      if (t->confidence == t->confidence) x->conf[bucket(t->confidence, kConfV, 5)]++;
    }
  }
  /* 4.0 (17.8): a take with audio makes the day active and adds to the open session; one that ended with its Heads
   * counts its length (take_s) and its speech by local hour (hour_speech_s) */
  if (st->audio_s > 0) {
    if (!st->aborted && st->audio_ms > 0) d.take_n[bucket((double)st->audio_ms / 1000.0, kTakeSV, 7)]++;
    for (int h = 0; !st->aborted && h < 24; ++h) d.hour_ms[h] += st->hour_ms[h] > 0 ? st->hour_ms[h] : 0;
    TfLearner j;
    tf__learner_load(c, &j);
    char local[11];
    tf__local_now(local, NULL);
    tf__learner_mark_active(c, &j, day, local); /* the Core's journal (R10) */
    j.session_open = 1;
    j.session_ms += (long long)(st->audio_s * 1000.0 + 0.5);
    tf__learner_save_session(c, &j);
  }
  day_save(c, day, &d);
  tf_mutex_unlock(&c->tlock);
  /* the next SDK call 15 minutes after the last delta is a send point: seal now; the timer (when running) sends */
  if (now_unix() - c->last_seal >= TF_DELTA_S) {
    seal(c, 1);
    tf_mutex_lock(&c->tm);
    c->t_send = 1;
    tf_cond_signal(&c->tcv);
    tf_mutex_unlock(&c->tm);
  }
}

static int list_days(const TfClient *c, char days[][11], int max) {
  int n = 0;
#ifdef _WIN32
  char *pat = spath(c, "telemetry-*.json");
  WIN32_FIND_DATAA fd;
  HANDLE h = pat ? FindFirstFileA(pat, &fd) : INVALID_HANDLE_VALUE;
  free(pat);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (n < max && strlen(fd.cFileName) == 25) snprintf(days[n++], 11, "%.10s", fd.cFileName + 10);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
#else
  DIR *dd = opendir(c->dir);
  struct dirent *e;
  while (dd && (e = readdir(dd))) {
    if (n < max && !strncmp(e->d_name, "telemetry-", 10) && strlen(e->d_name) == 25) snprintf(days[n++], 11, "%.10s", e->d_name + 10);
  }
  if (dd) closedir(dd);
#endif
  return n;
}

static void drop_day(const TfClient *c, const char *day) {
  char name[64];
  snprintf(name, sizeof name, "telemetry-%s.json", day);
  char *p = spath(c, name);
  if (p) remove(p);
  free(p);
}

/* Sealing (2.0.1): the counts of a day since its last delta (telemetry-<day>.json) become a delta with a fresh random
 * report_id, frozen as the exact body to send (report-<day>-<sealed at>-<report_id>.json), so a retry sends the same
 * bytes. The pending file is first renamed to sealing-<day>-<sealed at>-<report_id>.json (atomic): a crash between
 * the two steps finishes the same seal under the same id later, never counting twice. Each day is sealed under its
 * own day, so no delta spans midnight; a day with nothing counted seals nothing. Tried and rejected: one file per
 * day rewritten until midnight (2.0.0: a day's counts reached the server only the next day). */
static int list_prefixed(const TfClient *c, const char *prefix, char names[][96], int max) {
  int n = 0;
  const size_t pl = strlen(prefix);
#ifdef _WIN32
  char pat[64];
  snprintf(pat, sizeof pat, "%s*.json", prefix);
  char *pp = spath(c, pat);
  WIN32_FIND_DATAA fd;
  HANDLE h = pp ? FindFirstFileA(pp, &fd) : INVALID_HANDLE_VALUE;
  free(pp);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (n < max && strlen(fd.cFileName) < 96 && !strncmp(fd.cFileName, prefix, pl)) snprintf(names[n++], 96, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
#else
  DIR *dd = opendir(c->dir);
  struct dirent *e;
  while (dd && (e = readdir(dd)))
    if (n < max && strlen(e->d_name) < 96 && !strncmp(e->d_name, prefix, pl) && strstr(e->d_name, ".json")) snprintf(names[n++], 96, "%s", e->d_name);
  if (dd) closedir(dd);
#endif
  /* by name: by day, then by the time sealed */
  for (int i = 1; i < n; ++i)
    for (int k = i; k > 0 && strcmp(names[k], names[k - 1]) < 0; --k) {
      char t[96];
      memcpy(t, names[k], 96);
      memcpy(names[k], names[k - 1], 96);
      memcpy(names[k - 1], t, 96);
    }
  return n;
}
static void remove_named(const TfClient *c, const char *name) {
  char *p = spath(c, name);
  if (p) remove(p);
  free(p);
}
/* sealing-<day>-<t10>-<id32>.json: write the report from it, then remove it */
/* the learner facts and the trend a delta of `day` carries now (17.8, 17.9): from the journal, once, while their
 * group is on */
static void facts_for(TfClient *c, const TfLearner *j, const char *day, Facts *f) {
  memset(f, 0, sizeof *f);
  if (tf__client_metric_on(c, "learner")) f->learner = tf__learner_facts(j, day, f->l, f->gap, f->streak);
  char week[11];
  if (tf__client_metric_on(c, "judged")) f->trend = tf__learner_trend(j, day, f->trend_v, week);
}
/* what a written delta carried: its learner facts and its trend are marked sent in the journal (also after a kill
 * between the delta and the journal: the next seal repairs it from the delta before anything else) */
static void mark_from_report(TfClient *c, const char *rp) {
  char *text = NULL;
  size_t n = 0;
  TfJson *r = rp && !tf_read_file(rp, &text, &n) ? tf_json_parse(text, n, NULL) : NULL;
  free(text);
  const char *day = tf_json_str(tf_json_get(r, "day"));
  if (day && (tf_json_get(r, "learner") || tf_json_get(r, "trend"))) {
    TfLearner j;
    tf__learner_load(c, &j);
    if (tf_json_get(r, "learner")) tf__learner_mark_sent(c, &j, day);
    if (tf_json_get(r, "trend")) {
      char mon[11], last[11];
      tf__day_monday(day, mon);
      tf__day_str(tf__day_num(mon) - 7, last);
      if (!j.trend_week[0] || strcmp(j.trend_week, last) < 0) tf__learner_trend_sent(c, &j, last);
    }
  }
  tf_json_free(r);
}
static void finish_seal(TfClient *c, const char *sealing) {
  if (strlen(sealing) != 8 + 10 + 1 + 10 + 1 + 32 + 5) {
    remove_named(c, sealing);
    return;
  }
  char day[11], rid[33], rname[96];
  snprintf(day, sizeof day, "%.10s", sealing + 8);
  snprintf(rid, sizeof rid, "%.32s", sealing + 8 + 10 + 1 + 10 + 1);
  snprintf(rname, sizeof rname, "report-%s", sealing + 8);
  char *rp = spath(c, rname);
  int ok = rp && tf_file_exists(rp);
  if (!ok) {
    Day d;
    day_load_file(c, sealing, &d);
    TfLearner j;
    tf__learner_load(c, &j);
    Facts f;
    facts_for(c, &j, day, &f);
    t_facts = &f;
    char *body = day_empty(&d) ? NULL : day_body(c, day, &d, rid);
    t_facts = NULL;
    ok = !body || (rp && !tf_write_file(rp, body, strlen(body)));
    free(body);
  }
  if (ok && rp && tf_file_exists(rp)) mark_from_report(c, rp);
  if (ok) remove_named(c, sealing);
  free(rp);
}
static void seal(TfClient *c, int include_today) {
  tf_mutex_lock(&c->tlock);
  char names[64][96], today[11];
  day_utc(today, 0);
  int n = list_prefixed(c, "sealing-", names, 64);
  for (int i = 0; i < n; ++i) finish_seal(c, names[i]);
  char days[64][11];
  n = list_days(c, days, 64);
  for (int i = 0; i < n; ++i) {
    if (!include_today && !strcmp(days[i], today)) continue;
    uint8_t r[16];
    char rid[33], from[64], to[96];
    tf_random_bytes(r, sizeof r);
    tf_hex_encode(r, 16, rid);
    snprintf(from, sizeof from, "telemetry-%s.json", days[i]);
    snprintf(to, sizeof to, "sealing-%s-%010lld-%s.json", days[i], now_unix() % 10000000000LL, rid);
    char *pf = spath(c, from), *pt = spath(c, to);
    int moved = pf && pt && !rename(pf, pt);
    free(pf);
    free(pt);
    if (moved) finish_seal(c, to);
  }
  c->last_seal = now_unix();
  tf_mutex_unlock(&c->tlock);
}
/* Sends the sealed deltas, oldest first. 202 (a duplicate too) deletes; 400 and 413 drop (never retry a refused
 * body; 400 also queues the bad_request issue); 401 renews the token once (in call) and keeps; 429, 5xx, 503 and
 * the network keep it, same bytes, and end the pass. Deltas older than 7 days are deleted unsent; the kill switch
 * (flags.analytics false) drops the whole queue. Returns the deltas accepted. */
static int send_reports(TfClient *c) {
  tf_mutex_lock(&c->send_lock);
  char names[256][96], oldest[11];
  const int n = list_prefixed(c, "report-", names, 256);
  day_utc(oldest, -7 * 86400);
  const int on = tf__client_telemetry_on(c);
  int sent = 0, kept = 0;
  for (int i = 0; i < n; ++i) {
    if (!on || strncmp(names[i] + 7, oldest, 10) < 0) {
      /* 24.1: a delta kept 7 days without being accepted is a dropped report (the switch off is not) */
      if (on) tf__rep_silent("events", "expired", 0, NULL, NULL);
      remove_named(c, names[i]);
      continue;
    }
    char *p = spath(c, names[i]), *body = NULL;
    size_t bn;
    const int rd = p ? tf_read_file(p, &body, &bn) : -1;
    free(p);
    if (rd) continue;
    Call k = {"POST", "/v1/events", body, 1, NULL};
    TfHttpResponse r;
    const int rc = call(c, &k, &r);
    free(body);
    if (rc) {
      kept = 1;
      break;
    }
    const int st = r.status;
    char ecode[33];
    tf__rep_answer_reason(st, r.body, r.body_n, ecode);
    tf_http_response_free(&r);
    if (st == 202) {
      remove_named(c, names[i]);
      sent++;
    } else if (st == 400 || st == 413) {
      remove_named(c, names[i]);
      tf__rep_silent("events", "http", st, NULL, ecode); /* 24.1: a telemetry report refused for good */
      if (st == 400) tf__client_report_issue(c, "other", "bad_request:events", NULL);
    } else {
      kept = 1;
      break;
    }
  }
  if (!on) {
    char days[64][11];
    const int nd = list_days(c, days, 64);
    for (int i = 0; i < nd; ++i) drop_day(c, days[i]);
  }
  /* the timer's backoff: 1 min, then 10 min (explicit send points ignore it) */
  c->fails = kept ? c->fails + 1 : 0;
  c->next_try = kept ? now_unix() + (c->fails <= 1 ? 60 : 600) : 0;
  tf_mutex_unlock(&c->send_lock);
  return sent;
}

int tf__client_flush(TfClient *c, int include_today) {
  tf__clear_error();
  if (!c) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  if (tf__client_telemetry_on(c)) seal(c, include_today);
  const int sent = send_reports(c);
  /* queued issues */
  char *ip = spath(c, "issues.json"), *text = NULL;
  size_t tn;
  if (ip && !tf_read_file(ip, &text, &tn)) {
    TfJson *q = tf_json_parse(text, tn, NULL);
    const TfJson *items = tf_json_get(q, "queue");
    TfBuf keep;
    tf_buf_init(&keep);
    int kept = 0;
    const int issues_on = tf__client_flag(c, "issues", 1);
    for (int i = 0; items && i < items->n; ++i) {
      TfBuf one;
      tf_buf_init(&one);
      tf_json_write(&one, &items->kids[i]);
      int ok = !issues_on;
      if (issues_on) {
        Call k = {"POST", "/v1/issues", one.p, 1, NULL};
        TfHttpResponse r;
        if (!call(c, &k, &r)) {
          ok = r.status == 202 || r.status == 400;
          tf_http_response_free(&r);
        }
      }
      if (!ok) {
        tf_buf_printf(&keep, "%s%s", kept ? "," : "", one.p);
        kept++;
      }
      tf_buf_free(&one);
    }
    const TfJson *sent_sigs = tf_json_get(q, "sent");
    TfBuf out;
    tf_buf_init(&out);
    tf_buf_printf(&out, "{\"queue\":[%s],\"sent\":", keep.p ? keep.p : "");
    tf_json_write(&out, sent_sigs);
    tf_buf_puts(&out, "}");
    tf_write_file(ip, out.p, out.n);
    tf_buf_free(&out);
    tf_buf_free(&keep);
    tf_json_free(q);
    free(text);
  }
  free(ip);
  tf__rep_send(c); /* 24.6: after the telemetry, the error reports */
  return sent;
}

/* ------------------------------------------------------------------ issues (no free text; deduped per day) */
int tf__client_report_issue(TfClient *c, const char *kind, const char *code, const char *message) {
  (void)message; /* the SDK never sends free text */
  static const char *const kinds[] = {"engine_failed", "model_load", "checksum_mismatch", "signature_invalid", "update_failed",
                                      "fallback", "self_test_failed", "audio_input", "other"};
  if (!c || !kind || !code) return tf__fail(TF_ERR_INVALID_ARGUMENT, "kind and code are required");
  int ok = 0;
  for (int i = 0; i < NB(kinds); ++i) ok |= !strcmp(kind, kinds[i]);
  for (const char *p = code; *p; ++p)
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("_.:-", *p))) ok = 0;
  if (!ok || !*code || strlen(code) > 64) return tf__fail(TF_ERR_INVALID_ARGUMENT, "kind or code is not allowed");
  char day[11];
  day_utc(day, 0);
  char sig[160];
  snprintf(sig, sizeof sig, "%s|%s|%s", day, kind, code);
  char *ip = spath(c, "issues.json"), *text = NULL;
  size_t tn;
  TfJson *q = NULL;
  if (ip && !tf_read_file(ip, &text, &tn)) q = tf_json_parse(text, tn, NULL);
  free(text);
  const TfJson *sent = tf_json_get(q, "sent");
  for (int i = 0; sent && sent->type == TJ_ARR && i < sent->n; ++i)
    if (tf_json_str(&sent->kids[i]) && !strcmp(sent->kids[i].str, sig)) {
      tf_json_free(q);
      free(ip);
      return 0; /* one report per signature per day */
    }
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"queue\":[");
  const TfJson *queue = tf_json_get(q, "queue");
  for (int i = 0; queue && i < queue->n && i < 32; ++i) {
    tf_json_write(&b, &queue->kids[i]);
    tf_buf_puts(&b, ",");
  }
  tf_buf_printf(&b, "{\"day\":\"%s\",\"platform\":", day);
  tf_buf_json_str(&b, c->platform);
  tf_buf_printf(&b, ",\"sdk_version\":\"%s\",\"app_version\":", TF_SDK_VERSION);
  tf_buf_json_str(&b, c->app_version);
  tf_buf_printf(&b, ",\"os_version\":\"%d\",\"device_class\":\"x86_64\",\"backend\":\"exact\",\"kind\":", os_major());
  tf_buf_json_str(&b, kind);
  tf_buf_puts(&b, ",\"code\":");
  tf_buf_json_str(&b, code);
  tf_buf_puts(&b, "}],\"sent\":[");
  int k = 0;
  for (int i = 0; sent && sent->type == TJ_ARR && i < sent->n; ++i) {
    const char *s = tf_json_str(&sent->kids[i]);
    if (s && !strncmp(s, day, 10)) {
      tf_buf_printf(&b, "%s", k++ ? "," : "");
      tf_buf_json_str(&b, s);
    }
  }
  tf_buf_printf(&b, "%s", k ? "," : "");
  tf_buf_json_str(&b, sig);
  tf_buf_puts(&b, "]}");
  if (ip) tf_write_file(ip, b.p, b.n);
  tf_buf_free(&b);
  tf_json_free(q);
  free(ip);
  return 0;
}

/* ------------------------------------------------------------------ send points and the timer */
/* seal everything counted, send every sealed delta */
static int telemetry_send_point(TfClient *c) {
  seal(c, 1);
  const int sent = send_reports(c);
  tf__rep_send(c); /* 24.6: every telemetry send point sends the error reports after the telemetry */
  return sent;
}

int tf__client_background(TfClient *c) {
  tf__clear_error();
  if (!c) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  /* 2.0.3: the shared recording does not wait for another take while the app is in the background */
  tf__share_background(c);
  tf__rep_foreground(0); /* no 30 s send while in the background (24.6) */
  if (!tf__client_telemetry_on(c)) {
    send_reports(c); /* drops the queue */
    tf__rep_send(c);
    return 0;
  }
  return telemetry_send_point(c);
}

/* The client's timer (started by tf__client_launch): every 15 minutes while the client is open it seals and sends a
 * delta (and registers once the UTC day changed); a seal by tf__client_session_finished wakes it to send. Failed
 * sends wait 1, then 10 minutes. */
static void timer_thread(void *arg) {
  TfClient *c = (TfClient *)arg;
  tf_mutex_lock(&c->tm);
  while (!c->t_quit) {
    const long long now = now_unix(), due = c->last_seal + TF_DELTA_S;
    if (c->t_send || now >= due) {
      c->t_send = 0;
      tf_mutex_unlock(&c->tm);
      if (tf__client_telemetry_on(c)) {
        if (now >= due) seal(c, 1);
        if (!c->next_try || now_unix() >= c->next_try) send_reports(c);
      } else {
        c->last_seal = now;
      }
      tf__rep_send(c); /* 24.6: the error reports after the telemetry */
      char today[11];
      day_utc(today, 0);
      if (strcmp(c->register_day, today) && c->key) tf__client_register(c);
      tf_mutex_lock(&c->tm);
      continue;
    }
    /* SDK 4.0: the Core's tick (lease clock, the day's audio, the recited journal) and the lease's refresh when due */
    if (now >= c->next_tick) {
      tf_mutex_unlock(&c->tm);
      tf__store_tick(c);
      c->next_tick = now_unix() + TF_CORE_TICK_S;
      tf_mutex_lock(&c->tm);
      continue;
    }
    /* 24.6: 30 s after an occurrence that made a new report (tf_report.c wakes this thread when it sets the time) */
    const long long rdue = tf__rep_due();
    if (rdue && now >= rdue) {
      tf_mutex_unlock(&c->tm);
      tf__rep_send(c);
      tf_mutex_lock(&c->tm);
      continue;
    }
    long long ms = (due - now) * 1000;
    if (rdue && (rdue - now) * 1000 < ms) ms = (rdue - now) * 1000;
    if (ms < 1000) ms = 1000;
    if (ms > TF_CORE_TICK_S * 1000LL) ms = TF_CORE_TICK_S * 1000LL;
    tf_cond_wait_ms(&c->tcv, &c->tm, (long)ms);
  }
  tf_mutex_unlock(&c->tm);
}
int tf__client_no_timer; /* tests: no timer thread (the replayed exchanges run on one thread) */
void tf__client_timer_start(TfClient *c) {
  if (tf__client_no_timer) return;
  tf_mutex_lock(&c->tm);
  if (!c->t_on && !tf_thread_start(&c->tth, timer_thread, c)) c->t_on = 1;
  tf_mutex_unlock(&c->tm);
}
static void timer_stop(TfClient *c) {
  tf_mutex_lock(&c->tm);
  const int on = c->t_on;
  c->t_quit = 1;
  tf_cond_signal(&c->tcv);
  tf_mutex_unlock(&c->tm);
  if (on) tf_thread_join(c->tth);
  c->t_on = 0;
}

/* ------------------------------------------------------------------ launch */
int tf__client_launch(TfClient *c) {
  if (!c) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  int rc = 0;
  if (tf__client_register(c)) rc = tf__error_code();
  if (tf__client_refresh_config(c) && !rc) rc = tf__error_code();
  tf__client_check_latest(c); /* the config in force (a stored one when the refresh failed) */
  /* launch is a send point: this launch and what earlier runs left */
  tf__client_flush(c, 1);
  tf__client_timer_start(c);
  /* shared sessions a previous run left (crash, restart, offline) go up in the background */
  if (tf__client_sharing_state(c)) tf__sharing_outbox_resume(c);
  return rc;
}
