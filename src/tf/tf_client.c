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
char *tf__settings_json_alloc(TfModel *m);
void tf__sharing_session(TfClient *c, int feedback, double audio_s);
void tf__sharing_init(TfClient *c);
void tf__sharing_free(TfClient *c);
void tf__share_init(TfClient *c);
void tf__share_free(TfClient *c);
void tf__share_background(TfClient *c);
void tf__share_window_check(TfClient *c);
int tf_sharing_outbox_resume(TfClient *c);
void tf__telemetry_add(TfClient *c, const char *field, long long n);
int tf__client_telemetry_on(const TfClient *c);

static int g_contrib_default;
void zf_platform_set_contributions(int on) { g_contrib_default = on ? 1 : 0; }

struct TfClient {
  TfClientOptions o;
  char *key, *app_id, *platform, *app_name, *app_version, *api, *dir, *language;
  uint8_t pk[32];
  char kid[17];
  int bucket;
  int new_install;
  char token[1024];
  long long token_exp;
  int stopped;         /* a stop code this launch: no more network calls */
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
  /* the timer: a thread started by tf_client_launch */
  TfMutex tm;
  TfCond tcv;
  TfThread tth;
  int t_on, t_quit, t_send;
  int latest_logged;   /* the sdk_latest line was logged (once per client) */
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
const TfClientOptions *tf__client_options(const TfClient *c) { return &c->o; }
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

static int classify_stop(const TfHttpResponse *r) {
  if (!r->body) return 0;
  static const char *const stops[] = {"invalid_key", "developer_not_approved", "not_a_member", "key_type_mismatch",
                                      "origin_not_allowed", "app_not_allowed", "app_blocked", "environment_mismatch",
                                      "sdk_too_old", "unauthorized"};
  for (size_t i = 0; i < sizeof stops / sizeof stops[0]; ++i) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"error\":\"%s\"", stops[i]);
    if (strstr(r->body, pat)) return 1;
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
  if (c->stopped) return tf__fail(TF_ERR_NOT_AVAILABLE, "network calls are stopped for this launch");
  if (!c->key) return tf__fail(TF_ERR_NOT_AVAILABLE, "no developer key");
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (k->auth && ensure_token(c)) return tf_last_error_code();
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
    if (tf_http(&q, r)) return tf_last_error_code() ? tf_last_error_code() : TF_ERR_NETWORK;
    if (r->status == 401 && k->auth && attempt == 0 && r->body && strstr(r->body, "invalid_token")) {
      c->token[0] = 0;
      c->token_exp = 0;
      tf_http_response_free(r);
      continue;
    }
    break;
  }
  if (r->status >= 400 && classify_stop(r)) c->stopped = 1;
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

TfClientOptions tf_client_default_options(void) {
  TfClientOptions o;
  memset(&o, 0, sizeof o);
  o.struct_size = sizeof o;
  o.telemetry = 1;
  o.contributions = g_contrib_default;
  return o;
}

TfClient *tf_client_create(const TfClientOptions *opt) {
  tf__clear_error();
  TfClientOptions o = tf_client_default_options();
  if (opt) memcpy(&o, opt, opt->struct_size && opt->struct_size < sizeof o ? opt->struct_size : sizeof o);
  if (!o.storage_dir || !o.app_id) {
    tf__fail(TF_ERR_ARGUMENT, "storage_dir and app_id are required");
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
  c->dir = dupstr(o.storage_dir);
  c->language = dupstr(o.language ? o.language : "en");
  c->o.key = c->key;
  c->o.app_id = c->app_id;
  c->o.platform = c->platform;
  c->o.app_name = c->app_name;
  c->o.app_version = c->app_version;
  c->o.api_url = c->api;
  c->o.storage_dir = c->dir;
  c->o.language = c->language;
  c->o.trusted_key_hex = NULL;
  {
    uint8_t h[32];
    if (tf_hex_decode(opt && opt->trusted_key_hex ? opt->trusted_key_hex : TF_QURANLAB_KEY_HEX, c->pk, 32)) {
      tf__fail(TF_ERR_ARGUMENT, "trusted_key_hex is not 64 hex digits");
      tf_client_destroy(c);
      return NULL;
    }
    tf_sha256(c->pk, 32, h);
    tf_hex_encode(h, 8, c->kid);
  }
  size_t n = strlen(c->api);
  while (n && c->api[n - 1] == '/') c->api[--n] = 0;
  if (tf_mkdirs(c->dir)) {
    tf__fail(TF_ERR_IO, "cannot create the storage directory");
    tf_client_destroy(c);
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

void tf_client_destroy(TfClient *c) {
  if (!c) return;
  timer_stop(c);
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
    rc = tf__fail(TF_ERR_HTTP, "token: HTTP %d %s", r.status, r.body ? r.body : "");
    tf_http_response_free(&r);
    return rc;
  }
  TfJson *j = tf_json_parse(r.body, r.body_n, NULL);
  const char *t = tf_json_str(tf_json_get(j, "token"));
  const TfJson *e = tf_json_get(j, "expires_in");
  if (!t || strlen(t) >= sizeof c->token) rc = tf__fail(TF_ERR_HTTP, "token: no token in the answer");
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
int tf_client_register(TfClient *c) {
  tf__clear_error();
  if (!c) return tf__fail(TF_ERR_ARGUMENT, "no client");
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
    rc = tf__fail(TF_ERR_HTTP, "register: HTTP %d %s", r.status, r.body ? r.body : "");
  }
  tf_http_response_free(&r);
  return rc;
}

/* ------------------------------------------------------------------ config */
/* Verify a config body as received: kid pinned, Ed25519 over the exact bytes, v 1, expires_at in the future.
 * Returns 0 and the expiry, or TF_ERR_CONFIG. Exposed for the tests (the recorded fixtures). */
int tf__verify_config(const uint8_t pk[32], const char *kid_pinned, const char *body, size_t n, const char *sig_header,
                      const char *kid, long long now, long long *expires) {
  if (!kid || _tf_strnicmp(kid, kid_pinned, 17)) return tf__fail(TF_ERR_CONFIG, "config: unknown key id %s", kid ? kid : "");
  if (!sig_header || strncmp(sig_header, "ed25519=", 8)) return tf__fail(TF_ERR_CONFIG, "config: no ed25519 signature");
  uint8_t sig[70];
  if (tf_base64_decode(sig_header + 8, sig, sizeof sig) != 64) return tf__fail(TF_ERR_CONFIG, "config: malformed signature");
  if (!tf_ed25519_verify(sig, (const uint8_t *)body, n, pk)) return tf__fail(TF_ERR_CONFIG, "config: the signature does not verify");
  TfJson *j = tf_json_parse(body, n, NULL);
  const TfJson *v = tf_json_get(j, "v"), *e = tf_json_get(j, "expires_at");
  int rc = 0;
  if (!v || v->type != TJ_NUM || v->num != 1) rc = tf__fail(TF_ERR_CONFIG, "config: v is not 1");
  else if (!e || e->type != TJ_NUM || (long long)e->num <= now) rc = tf__fail(TF_ERR_CONFIG, "config: expired");
  else if (expires) *expires = (long long)e->num;
  tf_json_free(j);
  return rc;
}

static void check_sdk_latest(TfClient *c);
int tf_client_refresh_config(TfClient *c) {
  tf__clear_error();
  if (!c) return tf__fail(TF_ERR_ARGUMENT, "no client");
  char path[256], inm[120];
  snprintf(path, sizeof path, "/v1/config?bucket=%d&sdk=%s", c->bucket, TF_SDK_VERSION);
  /* within 1 h of expiry: ask without If-None-Match for a freshly signed body */
  const int conditional = c->config && c->etag[0] && c->config_exp - now_unix() > 3600;
  snprintf(inm, sizeof inm, "if-none-match: %s", c->etag);
  Call k = {"GET", path, NULL, 1, conditional ? inm : NULL};
  TfHttpResponse r;
  int rc = call(c, &k, &r);
  if (rc) return rc;
  if (r.status == 304 && c->config) {
    tf_http_response_free(&r);
    return 0;
  }
  if (r.status != 200) {
    rc = tf__fail(TF_ERR_HTTP, "config: HTTP %d", r.status);
    tf_http_response_free(&r);
    return rc;
  }
  long long exp = 0;
  rc = tf__verify_config(c->pk, c->kid, r.body, r.body_n, r.signature, r.key_id, now_unix(), &exp);
  if (rc) {
    tf_client_report_issue(c, "signature_invalid", "config", NULL);
    tf_http_response_free(&r);
    return rc; /* the last good config stays */
  }
  tf_mutex_lock(&c->lock);
  free(c->config);
  c->config = r.body;
  r.body = NULL;
  c->config_exp = exp;
  snprintf(c->etag, sizeof c->etag, "%s", r.etag);
  char *p = spath(c, "config.body");
  if (p) tf_write_file(p, c->config, strlen(c->config));
  free(p);
  save_state(c);
  tf_mutex_unlock(&c->lock);
  tf_http_response_free(&r);
  check_sdk_latest(c);
  /* 2.0.3: a config that turns sharing off, or sets another join window, reaches a recording that waits for its
   * next take now, not at the end of the window it started with */
  tf__share_window_check(c);
  return 0;
}

/* ------------------------------------------------------------------ the developer's log and sdk_latest */
/* debug builds only (the app's TfClientOptions.debug_build: the library itself is always a release build, so its
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
static void check_sdk_latest(TfClient *c) {
  if (c->latest_logged || !c->o.debug_build) return;
  tf_mutex_lock(&c->lock);
  TfJson *j = c->config && c->config_exp > now_unix() ? tf_json_parse(c->config, strlen(c->config), NULL) : NULL;
  tf_mutex_unlock(&c->lock);
  const TfJson *l = tf_json_get(j, "sdk_latest");
  const char *v = tf_json_str(tf_json_get(l, "version")), *sev = tf_json_str(tf_json_get(l, "severity"));
  const char *url = tf_json_str(tf_json_get(l, "url"));
  int ok = l && l->type == TJ_OBJ && v && sev && url && *sev && strlen(sev) <= 32 && !strncmp(url, "https://", 8) && strlen(url) < 512;
  for (const char *p = sev; ok && *p; ++p) ok = (*p >= 'a' && *p <= 'z') || *p == '_' || *p == '-';
  for (const char *p = url; ok && *p; ++p) ok = (unsigned char)*p > 0x20 && (unsigned char)*p < 0x7f;
  if (ok && version_newer(v, TF_SDK_VERSION)) {
    char line[700];
    snprintf(line, sizeof line, "Quran Lab SDK %s is available (%s), you have %s: %s", v, sev, TF_SDK_VERSION, url);
    tf__client_log(c, TF_LOG_WARNING, line);
    c->latest_logged = 1;
  }
  tf_json_free(j);
}

const char *tf_client_config_json(TfClient *c) {
  if (!c || !c->config) return NULL;
  if (c->config_exp <= now_unix()) return NULL;
  return c->config;
}

int tf_client_apply_config(TfClient *c, TfModel *m) {
  const char *cfg = tf_client_config_json(c);
  if (!m) return tf__fail(TF_ERR_ARGUMENT, "no model");
  const int d = tf_settings_set_remote_json(m, cfg, cfg ? strlen(cfg) : 0);
  return d < 0 ? d : 0;
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
#define NB(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const char *const kFb[] = {"non_finite", "deadline", "ane_unavailable", "npu_unavailable", "model_load", "thermal", "memory", "other"};
static const char *const kLat[] = {"le_5", "le_10", "le_20", "le_40", "le_80", "le_160", "le_320", "gt_320"};
static const double kLatV[] = {5, 10, 20, 40, 80, 160, 320};
static const char *const kRtf[] = {"le_0_1", "le_0_25", "le_0_5", "le_1", "gt_1"};
static const double kRtfV[] = {0.1, 0.25, 0.5, 1};
static const char *const kMs[] = {"le_250", "le_500", "le_1000", "le_2000", "le_4000", "gt_4000"};
static const double kMsV[] = {250, 500, 1000, 2000, 4000};
static const char *const kChecks[] = {"letters", "vowels", "length", "madd", "gemination", "qalqalah", "lazim"};
static const char *const kClasses[] = {"vowel_sub", "vowel_del", "vowel_ins", "short_to_long", "long_to_short", "cons_sub", "cons_del", "insertion"};
static const char *const kPct[] = {"le_1", "le_2", "le_5", "le_10", "le_20", "gt_20"};
static const double kPctV[] = {1, 2, 5, 10, 20};
static const char *const kMargin[] = {"le_0_5", "le_1", "le_2", "le_4", "le_8", "gt_8"};
static const double kMarginV[] = {0.5, 1, 2, 4, 8};
static const char *const kConf[] = {"le_0_5", "le_0_7", "le_0_8", "le_0_9", "le_0_95", "gt_0_95"};
static const double kConfV[] = {0.5, 0.7, 0.8, 0.9, 0.95};
static const char *const kMadd[] = {"c2", "c4", "c6", "flagged"};

typedef struct {
  char revision[64], context[16], backend[16], preset[16], word_level[12], word_bucket[8];
  long long site_overrides, sessions, chunks, aborted;
  double minutes;
  long long fb[8], st[2], lat[8], rtf[5], cold[6], load[6], thermal[2], checks[7], cls[8][2], sfp[6], margin[6], conf[6],
      madd[4], words[2];
} Cohort;
/* the 2.0.1 rule keys: each site class, then madd, qalqalah, gemination and word */
static const char *const kRule[] = {"vowel_sub", "vowel_del", "vowel_ins", "short_to_long", "long_to_short", "cons_sub", "cons_del",
                                    "insertion", "madd", "qalqalah", "gemination", "word"};
enum { R_MADD = 8, R_QALQALAH = 9, R_GEMINATION = 10, R_WORD = 11, N_RULE = 12 };
typedef struct {
  Cohort co[16];
  int n;
  long long coverage[115];
  /* 2.0.1 */
  long long launches, ayahs, words, surah_ayahs[115], rule[N_RULE][2], share_asked, share_yes, share_no;
} Day;
static int day_empty(const Day *d) {
  long long any = d->n + d->launches + d->ayahs + d->words + d->share_asked + d->share_yes + d->share_no;
  for (int s = 1; s <= 114; ++s) any += d->coverage[s] + d->surah_ayahs[s];
  for (int k = 0; k < N_RULE; ++k) any += d->rule[k][0];
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
    copy_str(x->context, sizeof x->context, o, "context");
    copy_str(x->backend, sizeof x->backend, o, "backend");
    copy_str(x->preset, sizeof x->preset, o, "preset");
    copy_str(x->word_level, sizeof x->word_level, o, "word_level");
    copy_str(x->word_bucket, sizeof x->word_bucket, o, "word_bucket");
    x->site_overrides = num(o, "site_overrides");
    x->sessions = num(o, "sessions");
    x->chunks = num(o, "chunks");
    x->aborted = num(o, "aborted");
    const TfJson *mi = tf_json_get(o, "minutes");
    x->minutes = mi && mi->type == TJ_NUM ? mi->num : 0;
    read_arr(tf_json_get(o, "fallbacks"), kFb, 8, x->fb);
    static const char *const stn[] = {"pass", "fail"};
    read_arr(tf_json_get(o, "self_test"), stn, 2, x->st);
    read_arr(tf_json_get(o, "latency_ms"), kLat, 8, x->lat);
    read_arr(tf_json_get(o, "rtf"), kRtf, 5, x->rtf);
    read_arr(tf_json_get(o, "cold_start_ms"), kMs, 6, x->cold);
    read_arr(tf_json_get(o, "model_load_ms"), kMs, 6, x->load);
    static const char *const thn[] = {"transitions", "throttled_sessions"};
    read_arr(tf_json_get(o, "thermal"), thn, 2, x->thermal);
    read_arr(tf_json_get(o, "checks_on"), kChecks, 7, x->checks);
    for (int k = 0; k < 8; ++k) {
      const TfJson *cl = tf_json_get(tf_json_get(o, "classes"), kClasses[k]);
      x->cls[k][0] = num(cl, "sites");
      x->cls[k][1] = num(cl, "flagged");
    }
    read_arr(tf_json_get(o, "session_flag_pct"), kPct, 6, x->sfp);
    read_arr(tf_json_get(o, "margin"), kMargin, 6, x->margin);
    read_arr(tf_json_get(o, "confidence"), kConf, 6, x->conf);
    read_arr(tf_json_get(o, "madd"), kMadd, 4, x->madd);
    static const char *const wn[] = {"scored", "flagged"};
    read_arr(tf_json_get(o, "words"), wn, 2, x->words);
  }
  const TfJson *cov = tf_json_get(j, "coverage");
  for (int i = 0; cov && cov->type == TJ_OBJ && i < cov->n; ++i) {
    const int s = atoi(cov->kids[i].key);
    if (s >= 1 && s <= 114 && cov->kids[i].type == TJ_NUM) d->coverage[s] = (long long)cov->kids[i].num;
  }
  d->launches = num(j, "launches");
  d->ayahs = num(j, "ayahs");
  d->words = num(j, "words");
  d->share_asked = num(j, "share_asked");
  d->share_yes = num(j, "share_yes");
  d->share_no = num(j, "share_no");
  const TfJson *sa = tf_json_get(j, "surah_ayahs");
  for (int i = 0; sa && sa->type == TJ_OBJ && i < sa->n; ++i) {
    const int s = atoi(sa->kids[i].key);
    if (s >= 1 && s <= 114 && sa->kids[i].type == TJ_NUM) d->surah_ayahs[s] = (long long)sa->kids[i].num;
  }
  for (int k = 0; k < N_RULE; ++k) {
    d->rule[k][0] = num(tf_json_get(j, "rule_checks"), kRule[k]);
    d->rule[k][1] = num(tf_json_get(j, "rule_flags"), kRule[k]);
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
/* the v2 body for a day (closed schema: exactly these fields, the 2.0.1 ones each left out when zero);
 * report_id NULL: the pending counts on disk, not a delta to send */
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
    tf_buf_puts(&b, "{\"revision\":");
    tf_buf_json_str(&b, x->revision);
    tf_buf_puts(&b, ",\"context\":");
    tf_buf_json_str(&b, x->context);
    tf_buf_puts(&b, ",\"backend\":");
    tf_buf_json_str(&b, x->backend);
    tf_buf_puts(&b, ",\"preset\":");
    tf_buf_json_str(&b, x->preset);
    tf_buf_puts(&b, ",\"word_level\":");
    tf_buf_json_str(&b, x->word_level);
    tf_buf_puts(&b, ",\"word_bucket\":");
    tf_buf_json_str(&b, x->word_bucket);
    tf_buf_printf(&b, ",\"site_overrides\":%lld,\"sessions\":%lld,\"minutes\":", x->site_overrides, x->sessions);
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
    write_arr(&b, "checks_on", kChecks, 7, x->checks);
    tf_buf_puts(&b, ",\"classes\":{");
    for (int k = 0; k < 8; ++k)
      tf_buf_printf(&b, "%s\"%s\":{\"sites\":%lld,\"flagged\":%lld}", k ? "," : "", kClasses[k], x->cls[k][0], x->cls[k][1]);
    tf_buf_puts(&b, "}");
    write_arr(&b, "session_flag_pct", kPct, 6, x->sfp);
    write_arr(&b, "margin", kMargin, 6, x->margin);
    write_arr(&b, "confidence", kConf, 6, x->conf);
    write_arr(&b, "madd", kMadd, 4, x->madd);
    static const char *const wn[] = {"scored", "flagged"};
    write_arr(&b, "words", wn, 2, x->words);
    tf_buf_puts(&b, "}");
  }
  tf_buf_puts(&b, "],\"coverage\":{");
  int first = 1;
  for (int s = 1; s <= 114; ++s)
    if (d->coverage[s]) {
      tf_buf_printf(&b, "%s\"%d\":%lld", first ? "" : ",", s, d->coverage[s]);
      first = 0;
    }
  tf_buf_puts(&b, "},\"downloads\":[]");
  if (report_id) tf_buf_printf(&b, ",\"report_id\":\"%s\"", report_id);
  write_count(&b, "launches", d->launches);
  write_count(&b, "ayahs", d->ayahs);
  write_count(&b, "words", d->words);
  first = 1;
  for (int s = 1; s <= 114; ++s)
    if (d->surah_ayahs[s]) {
      tf_buf_printf(&b, "%s\"%d\":%lld", first ? ",\"surah_ayahs\":{" : ",", s, d->surah_ayahs[s]);
      first = 0;
    }
  if (!first) tf_buf_puts(&b, "}");
  /* every flags key is a checks key with a count no larger (flags are counted only with their check) */
  for (int f = 0; f < 2; ++f) {
    first = 1;
    for (int k = 0; k < N_RULE; ++k)
      if (d->rule[k][f]) {
        tf_buf_printf(&b, "%s\"%s\":%lld", first ? (f ? ",\"rule_flags\":{" : ",\"rule_checks\":{") : ",", kRule[k], d->rule[k][f]);
        first = 0;
      }
    if (!first) tf_buf_puts(&b, "}");
  }
  write_count(&b, "share_asked", d->share_asked);
  write_count(&b, "share_yes", d->share_yes);
  write_count(&b, "share_no", d->share_no);
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

#define TF_DELTA_S (15 * 60) /* a delta every 15 minutes while a client is open (Quran Lab, 11-09) */
static void seal(TfClient *c, int include_today);

void tf_client_session_finished(TfClient *c, const TfSessionStats *st) {
  if (!c || !st) return;
  tf__sharing_session(c, st->feedback_shown, st->audio_s);
  if (!tf__client_telemetry_on(c)) return;
  tf_mutex_lock(&c->tlock);
  char day[11];
  day_utc(day, 0);
  Day d;
  day_load(c, day, &d);
  /* the cohort: revision, context, backend, preset, word level */
  Cohort key;
  memset(&key, 0, sizeof key);
  snprintf(key.backend, sizeof key.backend, "cpu");
  snprintf(key.word_level, sizeof key.word_level, "off");
  long long checks[7] = {0};
  if (st->model) {
    TfModel *m = (TfModel *)st->model;
    TfJson *info = tf_json_parse(tf_model_info_json(m), strlen(tf_model_info_json(m)), NULL);
    snprintf(key.revision, sizeof key.revision, "%s+%s", tf_json_str(tf_json_get(info, "model_id")) ? tf_json_str(tf_json_get(info, "model_id")) : "",
             tf_json_str(tf_json_get(info, "revision")) ? tf_json_str(tf_json_get(info, "revision")) : "");
    snprintf(key.context, sizeof key.context, "%s", tf_model_context(m));
    tf_json_free(info);
    char *sj = tf__settings_json_alloc(m);
    TfJson *s = sj ? tf_json_parse(sj, strlen(sj), NULL) : NULL;
    free(sj);
    snprintf(key.preset, sizeof key.preset, "%s", tf_json_str(tf_json_get(s, "preset")) ? tf_json_str(tf_json_get(s, "preset")) : "");
    const TfJson *sites = tf_json_get(s, "sites");
    for (int i = 0; sites && i < sites->n; ++i) {
      const char *src = tf_json_str(tf_json_get(&sites->kids[i], "source"));
      const TfJson *th = tf_json_get(&sites->kids[i], "threshold");
      const char *pr = tf_json_str(tf_json_get(&sites->kids[i], "preset"));
      if (src && strcmp(src, "calibration") && pr && strcmp(pr, key.preset)) key.site_overrides++;
      if (th && th->type == TJ_NUM && !pr) snprintf(key.preset, sizeof key.preset, "custom");
    }
    const TfJson *w = tf_json_get(s, "word");
    if (w && tf_json_get(w, "enabled") && tf_json_get(w, "enabled")->boolean) {
      const char *wp = tf_json_str(tf_json_get(w, "preset"));
      snprintf(key.word_level, sizeof key.word_level, "%s", wp ? wp : "custom");
      if (wp && !strcmp(wp, "custom")) {
        static const char *const wb[] = {"le_3", "le_4", "le_5", "le_6", "le_7", "gt_7"};
        static const double wv[] = {3, 4, 5, 6, 7};
        const TfJson *t = tf_json_get(w, "threshold");
        snprintf(key.word_bucket, sizeof key.word_bucket, "%s", wb[bucket(t && t->type == TJ_NUM ? t->num : 0, wv, 5)]);
      }
    }
    for (int k = 0; k < 7; ++k) {
      const char *ck = kChecks[k];
      const TfJson *e = NULL;
      if (!strcmp(ck, "madd") || !strcmp(ck, "gemination") || !strcmp(ck, "qalqalah") || !strcmp(ck, "lazim")) e = tf_json_get(tf_json_get(s, ck), "enabled");
      else {
        /* a site check is on when any of its classes is enabled */
        for (int i = 0; sites && i < sites->n; ++i) {
          const int cls = zf_tajweed_class_from_name(sites->kids[i].key);
          const TfJson *en = tf_json_get(&sites->kids[i], "enabled");
          if (cls >= 0 && zf_tajweed_class_check(cls) == k && en && en->boolean) e = en;
        }
      }
      checks[k] = e && e->type == TJ_BOOL && e->boolean;
    }
    tf_json_free(s);
  }
  Cohort *x = NULL;
  for (int i = 0; i < d.n; ++i)
    if (!strcmp(d.co[i].revision, key.revision) && !strcmp(d.co[i].context, key.context) && !strcmp(d.co[i].backend, key.backend) &&
        !strcmp(d.co[i].preset, key.preset) && !strcmp(d.co[i].word_level, key.word_level))
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
  for (int k = 0; k < 7; ++k) x->checks[k] += checks[k];
  if (st->decoded) {
    const TfToken *t;
    const int nt = tf_decoded_tokens(st->decoded, &t);
    for (int i = 0; i < nt; ++i) {
      x->margin[bucket(t[i].margin, kMarginV, 5)]++;
      if (t[i].confidence == t[i].confidence) x->conf[bucket(t[i].confidence, kConfV, 5)]++;
    }
  }
  if (st->result) {
    const ZfTajweedResult *r = tf_result_engine(st->result);
    const ZfTajweedSite *s;
    const int ns = zf_tajweed_result_sites(r, &s);
    int flagged = 0, windows = 0;
    for (int i = 0; i < ns; ++i) {
      x->cls[s[i].cls][0]++;
      x->cls[s[i].cls][1] += s[i].flagged;
      flagged += s[i].flagged;
    }
    ZfTajweedSummary sum;
    if (zf_tajweed_result_summary(r, &sum) == 0) windows = sum.n_sites;
    if (windows > 0) x->sfp[bucket(100.0 * flagged / windows, kPctV, 5)]++;
    const ZfTajweedMadd *md;
    const int nm = zf_tajweed_result_madd(r, &md);
    for (int i = 0; i < nm; ++i) {
      if (md[i].count == 2) x->madd[0]++;
      else if (md[i].count == 4) x->madd[1]++;
      else if (md[i].count == 6) x->madd[2]++;
      x->madd[3] += md[i].flagged;
    }
    const TfWordScore *w;
    const int nw = tf_result_words(st->result, &w);
    for (int i = 0; i < nw; ++i) {
      x->words[0] += w[i].has_score;
      x->words[1] += w[i].flagged == 1;
    }
    TfJson *rj = tf_json_parse(tf_result_json((TfResult *)st->result), strlen(tf_result_json((TfResult *)st->result)), NULL);
    const TfJson *su = tf_json_get(rj, "surah");
    if (su && su->type == TJ_NUM && su->num >= 1 && su->num <= 114) {
      d.coverage[(int)su->num]++;
      d.surah_ayahs[(int)su->num]++;
    }
    /* 2.0.1: one scored ayah, its words, and per rule the verdicts whose check ran (enabled in the settings the
     * result was scored with) and how many of those were flagged */
    d.ayahs++;
    const TfJson *set = tf_json_get(rj, "settings");
    const TfJson *sites = tf_json_get(set, "sites");
    for (int i = 0; i < ns; ++i) {
      if (s[i].cls < 0 || s[i].cls >= 8) continue;
      const TfJson *en = tf_json_get(tf_json_get(sites, kClasses[s[i].cls]), "enabled");
      if (!en || en->type != TJ_BOOL || !en->boolean) continue;
      d.rule[s[i].cls][0]++;
      d.rule[s[i].cls][1] += s[i].flagged != 0;
    }
    const TfJson *me = tf_json_get(tf_json_get(set, "madd"), "enabled"), *le = tf_json_get(tf_json_get(set, "lazim"), "enabled");
    for (int i = 0; i < nm; ++i) {
      const TfJson *en = md[i].madd_class == ZF_TJ_MADD_LAZIM ? le : me;
      if (!en || en->type != TJ_BOOL || !en->boolean) continue;
      d.rule[R_MADD][0]++;
      d.rule[R_MADD][1] += md[i].flagged != 0;
    }
    static const struct { const char *check; int rule; } letters[] = {{"qalqalah", R_QALQALAH}, {"gemination", R_GEMINATION}};
    for (int k = 0; k < 2; ++k) {
      const TfJson *en = tf_json_get(tf_json_get(set, letters[k].check), "enabled");
      if (!en || en->type != TJ_BOOL || !en->boolean) continue;
      const ZfTajweedLetter *lt;
      const int nl = k == 0 ? zf_tajweed_result_qalqalah(r, &lt) : zf_tajweed_result_gemination(r, &lt);
      for (int i = 0; i < nl; ++i) {
        d.rule[letters[k].rule][0]++;
        d.rule[letters[k].rule][1] += lt[i].flagged != 0;
      }
    }
    for (int i = 0; i < nw; ++i) {
      d.words += w[i].has_score;
      if (w[i].has_score && w[i].flagged >= 0) { /* -1: the word check off or no word calibration */
        d.rule[R_WORD][0]++;
        d.rule[R_WORD][1] += w[i].flagged == 1;
      }
    }
    tf_json_free(rj);
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
      if (st == 400) tf_client_report_issue(c, "other", "bad_request:events", NULL);
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

int tf_client_flush(TfClient *c, int include_today) {
  tf__clear_error();
  if (!c) return tf__fail(TF_ERR_ARGUMENT, "no client");
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
int tf_client_report_issue(TfClient *c, const char *kind, const char *code, const char *message) {
  (void)message; /* the SDK never sends free text */
  static const char *const kinds[] = {"engine_failed", "model_load", "checksum_mismatch", "signature_invalid", "update_failed",
                                      "fallback", "self_test_failed", "audio_input", "other"};
  if (!c || !kind || !code) return tf__fail(TF_ERR_ARGUMENT, "kind and code are required");
  int ok = 0;
  for (int i = 0; i < NB(kinds); ++i) ok |= !strcmp(kind, kinds[i]);
  for (const char *p = code; *p; ++p)
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("_.:-", *p))) ok = 0;
  if (!ok || !*code || strlen(code) > 64) return tf__fail(TF_ERR_ARGUMENT, "kind or code is not allowed");
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

int tf_client_background(TfClient *c) {
  tf__clear_error();
  if (!c) return tf__fail(TF_ERR_ARGUMENT, "no client");
  /* 2.0.3: the shared recording does not wait for another take while the app is in the background */
  tf__share_background(c);
  if (!tf__client_telemetry_on(c)) {
    send_reports(c); /* drops the queue */
    return 0;
  }
  return telemetry_send_point(c);
}

/* The client's timer (started by tf_client_launch): every 15 minutes while the client is open it seals and sends a
 * delta (and registers once the UTC day changed); a seal by tf_client_session_finished wakes it to send. Failed
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
      if (strcmp(c->register_day, today) && c->key) tf_client_register(c);
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
static void timer_start(TfClient *c) {
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
int tf_client_launch(TfClient *c) {
  if (!c) return tf__fail(TF_ERR_ARGUMENT, "no client");
  int rc = 0;
  if (tf_client_register(c)) rc = tf_last_error_code();
  if (tf_client_refresh_config(c) && !rc) rc = tf_last_error_code();
  check_sdk_latest(c); /* the config in force (a stored one when the refresh failed) */
  /* launch is a send point: this launch and what earlier runs left */
  tf_client_flush(c, 1);
  timer_start(c);
  /* shared sessions a previous run left (crash, restart, offline) go up in the background */
  if (tf_client_sharing_state(c)) tf_sharing_outbox_resume(c);
  return rc;
}
