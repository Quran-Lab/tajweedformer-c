/* The platform client: token, register, the signed
 * config (Ed25519 over the exact body, pinned key id, expiry, ETag), telemetry v2 (anonymous daily
 * aggregates, on by default, flags.analytics kill switch), issues (no free text). State lives in the
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
void tf__sharing_session(TfClient *c, int feedback, double audio_s);
void tf__sharing_init(TfClient *c);
void tf__sharing_free(TfClient *c);
void tf__share_init(TfClient *c);
void tf__share_free(TfClient *c);
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
  int latest_logged;   /* the sdk_latest line was logged (once per client) */
  char latest[24];     /* the SDK release the config in force names (sdk_latest.version), "" when none */
  int answered;        /* the server answered an authenticated request of this client with a success */
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
const char *tf__client_key(const TfClient *c) { return c->key; }
const char *tf__client_trusted_hex(const TfClient *c) { return c->trusted_hex; }
const char *tf__model_key_override;
const char *tf__client_model_key(const TfClient *c) { return tf__model_key_override ? tf__model_key_override : c->trusted_hex; }
void **tf__client_api_slot(TfClient *c) { return &c->api_state; }
void tf__client_note_error(TfClient *c, int code) {
  if (c) c->last_error = code;
}
int tf__client_last_error(const TfClient *c) { return c->last_error; }
/* why the network calls of this launch stopped: the server's code ("invalid_key", "sdk_too_old", ...), or "" */
const char *tf__client_stop_code(const TfClient *c) { return c->stop_code; }
const char *tf__client_latest(const TfClient *c) { return c->latest[0] ? c->latest : NULL; }
int tf__client_stopped(const TfClient *c) { return c->stopped; }
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

static int call_locked(TfClient *c, const Call *k, TfHttpResponse *r);
static int call(TfClient *c, const Call *k, TfHttpResponse *r) {
  tf_mutex_lock(&c->lock);
  const int rc = call_locked(c, k, r);
  tf_mutex_unlock(&c->lock);
  return rc;
}
static int call_locked(TfClient *c, const Call *k, TfHttpResponse *r) {
  if (c->stopped) return tf__fail(TF__ERR_NOT_AVAILABLE, "network calls are stopped for this launch");
  if (!c->key) return tf__fail(TF__ERR_NOT_AVAILABLE, "no developer key");
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
    q.cancel = &c->cancel;
    if (tf_http(&q, r)) return tf__error_code() ? tf__error_code() : TF__ERR_NETWORK;
    if (r->status == 401 && k->auth && attempt == 0 && r->body && strstr(r->body, "invalid_token")) {
      c->token[0] = 0;
      c->token_exp = 0;
      tf_http_response_free(r);
      continue;
    }
    break;
  }
  if (r->status >= 400 && classify_stop(c, r)) c->stopped = 1;
  if (r->status >= 200 && r->status < 400) c->answered = 1;
  return 0;
}

int tf__client_call(TfClient *c, const char *method, const char *path, const char *body, int auth, const char *extra,
                    TfHttpResponse *r) {
  Call k = {method, path, body, auth, extra};
  return call(c, &k, r);
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
  tf_buf_puts(&b, "}");
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
  o.telemetry = 1;
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
  load_state(c);
  save_state(c);
  tf__sharing_init(c);
  tf__share_init(c);
  /* telemetry: opening a client is a launch; the 15 minutes run from here */
  c->last_seal = now_unix();
  tf__telemetry_add(c, "launches", 1);
  return c;
}

static void timer_stop(TfClient *c);
static int telemetry_send_point(TfClient *c);

void tf__client_cancel_calls(TfClient *c, int on) {
  if (on) tf__http_cancel(&c->cancel);
  else c->cancel = 0;
}

void tf__client_close(TfClient *c) {
  if (!c) return;
  timer_stop(c);
  c->cancel = 0; /* the last send point below is a request of its own */
  /* closing the client is a send point (what is not sent stays on disk for the next launch) */
  if (c->key && tf__client_telemetry_on(c)) telemetry_send_point(c);
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
  if (!t || strlen(t) >= sizeof c->token) rc = tf__fail(TF__ERR_HTTP, "token: no token in the answer");
  else {
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
int tf__verify_config(const uint8_t pk[32], const char *kid_pinned, const char *body, size_t n, const char *sig_header,
                      const char *kid, long long now, long long *expires) {
  if (!kid || _tf_strnicmp(kid, kid_pinned, 17)) return tf__fail(TF__ERR_CONFIG, "config: unknown key id %s", kid ? kid : "");
  if (!sig_header || strncmp(sig_header, "ed25519=", 8)) return tf__fail(TF__ERR_CONFIG, "config: no ed25519 signature");
  uint8_t sig[70];
  if (tf_base64_decode(sig_header + 8, sig, sizeof sig) != 64) return tf__fail(TF__ERR_CONFIG, "config: malformed signature");
  if (!tf_ed25519_verify(sig, (const uint8_t *)body, n, pk)) return tf__fail(TF__ERR_CONFIG, "config: the signature does not verify");
  TfJson *j = tf_json_parse(body, n, NULL);
  const TfJson *v = tf_json_get(j, "v"), *e = tf_json_get(j, "expires_at");
  int rc = 0;
  if (!v || v->type != TJ_NUM || v->num != 1) rc = tf__fail(TF__ERR_CONFIG, "config: v is not 1");
  else if (!e || e->type != TJ_NUM || (long long)e->num <= now) rc = tf__fail(TF__ERR_CONFIG, "config: expired");
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
    rc = tf__fail(TF__ERR_HTTP, "config: HTTP %d", r.status);
    tf_http_response_free(&r);
    return rc;
  }
  long long exp = 0;
  rc = tf__verify_config(c->pk, c->kid, r.body, r.body_n, r.signature, r.key_id, now_unix(), &exp);
  if (rc) {
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
typedef struct {
  Cohort co[16];
  int n;
  long long launches, share_asked, share_yes, share_no;
  /* app-reported */
  long long ayahs, words, surah_ayahs[115];
  Rule rules[MAX_RULES];
  int n_rules;
  /* model downloads: per build, the attempts that ended and the bytes fetched */
  struct {
    char revision[32], build[41];
    long long bytes, success, failed, resumed;
  } dl[MAX_DOWNLOADS];
  int n_dl;
} Day;
static int day_empty(const Day *d) {
  long long any = d->n + d->launches + d->ayahs + d->words + d->share_asked + d->share_yes + d->share_no + d->n_rules + d->n_dl;
  for (int s = 1; s <= 114; ++s) any += d->surah_ayahs[s];
  return any == 0;
}

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
    d->n_dl++;
  }
  d->share_yes = num(j, "share_yes");
  d->share_no = num(j, "share_no");
  const TfJson *sa = tf_json_get(j, "surah_ayahs");
  for (int i = 0; sa && sa->type == TJ_OBJ && i < sa->n; ++i) {
    const int s = atoi(sa->kids[i].key);
    if (s >= 1 && s <= 114 && sa->kids[i].type == TJ_NUM) d->surah_ayahs[s] = (long long)sa->kids[i].num;
  }
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
  for (int i = 0; i < d->n_dl; ++i)
    tf_buf_printf(&b, "%s{\"revision\":\"%s\",\"build\":\"%s\",\"profile\":\"%s-cpu\",\"bytes\":%lld,\"success\":%lld,\"failed\":%lld,\"resumed\":%lld}",
                  i ? "," : ",\"downloads\":[", d->dl[i].revision, d->dl[i].build, tf__client_platform(c), d->dl[i].bytes, d->dl[i].success, d->dl[i].failed,
                  d->dl[i].resumed);
  if (d->n_dl) tf_buf_puts(&b, "]");
  if (report_id) tf_buf_printf(&b, ",\"report_id\":\"%s\"", report_id);
  write_count(&b, "launches", d->launches);
  write_count(&b, "ayahs", d->ayahs > 0 ? d->ayahs : 0);
  write_count(&b, "words", d->words > 0 ? d->words : 0);
  /* surah_ayahs, and coverage with the same values (kept for readers of 2.x) */
  for (int pass = 0; pass < 2; ++pass) {
    first = 1;
    for (int s = 1; s <= 114; ++s)
      if (d->surah_ayahs[s] > 0) {
        tf_buf_printf(&b, "%s\"%d\":%lld", first ? (pass ? ",\"coverage\":{" : ",\"surah_ayahs\":{") : ",", s, d->surah_ayahs[s]);
        first = 0;
        any_surah = 1;
      }
    if (!first) tf_buf_puts(&b, "}");
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
    } rep[] = {{"ayahs", d->ayahs > 0},   {"surah_ayahs", any_surah}, {"coverage", any_surah},
               {"words", d->words > 0},   {"rule_checks", any_checks}, {"rule_flags", any_flags}};
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

int tf__client_telemetry_on(const TfClient *c) { return c->o.telemetry && tf__client_flag(c, "analytics", 1); }

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
    day_save(c, day, &d);
  }
  tf_mutex_unlock(&c->tlock);
}

/* App-reported (SPEC 6.3), counted into today's sums: where the learner recited. Validated by the caller. */
void tf__client_count_recited(TfClient *c, int surah, int ayah_from, int ayah_to) {
  if (!c || !tf__client_telemetry_on(c) || surah < 1 || surah > 114 || ayah_to < ayah_from) return;
  tf_mutex_lock(&c->tlock);
  char day[11];
  day_utc(day, 0);
  Day d;
  day_load(c, day, &d);
  d.ayahs += ayah_to - ayah_from + 1;
  d.surah_ayahs[surah] += ayah_to - ayah_from + 1;
  day_save(c, day, &d);
  tf_mutex_unlock(&c->tlock);
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
static void seal(TfClient *c, int include_today);

void tf__client_session_finished(TfClient *c, const TfSessionStats *st) {
  if (!c || !st) return;
  tf__sharing_session(c, st->feedback_shown, st->audio_s);
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
    char *body = day_empty(&d) ? NULL : day_body(c, day, &d, rid);
    ok = !body || (rp && !tf_write_file(rp, body, strlen(body)));
    free(body);
  }
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
 * (flags.analytics false, or the developer's opt-out) drops the whole queue. Returns the deltas accepted. */
static int send_reports(TfClient *c) {
  tf_mutex_lock(&c->send_lock);
  char names[256][96], oldest[11];
  const int n = list_prefixed(c, "report-", names, 256);
  day_utc(oldest, -7 * 86400);
  const int on = tf__client_telemetry_on(c);
  int sent = 0, kept = 0;
  for (int i = 0; i < n; ++i) {
    if (!on || strncmp(names[i] + 7, oldest, 10) < 0) {
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
    tf_http_response_free(&r);
    if (st == 202) {
      remove_named(c, names[i]);
      sent++;
    } else if (st == 400 || st == 413) {
      remove_named(c, names[i]);
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
  return send_reports(c);
}

int tf__client_background(TfClient *c) {
  tf__clear_error();
  if (!c) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  /* 2.0.3: the shared recording does not wait for another take while the app is in the background */
  tf__share_background(c);
  if (!tf__client_telemetry_on(c)) {
    send_reports(c); /* drops the queue */
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
      char today[11];
      day_utc(today, 0);
      if (strcmp(c->register_day, today) && c->key) tf__client_register(c);
      tf_mutex_lock(&c->tm);
      continue;
    }
    long long ms = (due - now) * 1000;
    if (ms < 1000) ms = 1000;
    if (ms > TF_DELTA_S * 1000LL) ms = TF_DELTA_S * 1000LL;
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
