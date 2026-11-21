/* Endpoint sets: more than one host per role, and failover.
 *
 * Each kind of Quran Lab host is an ordered list built into the SDK: `api` (every API request) and `models` (every model
 * request: a build's manifest.json, its files, their ranges). A signed config's `endpoints` may reorder, leave out and
 * add hosts of the allow-list, at most 8 a role, and only while that config is in force (28.2). One request asks the
 * preferred host P first, then the role's other hosts in list order; 30 minutes after P became preferred the first
 * host is asked first again (the probe back). Each answer is read by 18.4.7.1's table: taken, a refusal (a JSON 4xx
 * other than 429: the request's answer, never taken to another host), slow down (a 429: no other host is asked) or
 * unreachable (no answer, a stall, a 5xx, a redirect, a 4xx that is not the API's, a 2xx that is not what was asked),
 * on which the next host is asked at once. A request that every host left unreachable is the first host's failure.
 *
 * Nothing here decides what is trusted: configs, manifests, leases, refusals and files are checked by their own
 * signatures and hashes whatever host served them. Every host is Quran Lab's: the allow-list of 28.1. A test build
 * may name its own one host per role (TfClientConfig.api_url, cdn_url) or reach the built-in names at stand-in servers
 * (tf__ep_test_origin); a released build does neither. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tf_internal.h"
#include "tf_thread.h"

/* 28.1: each role's hosts in their order; the standby hosts' names are reserved (a host that does not serve yet answers
 * 503 or does not resolve, which is unreachable at once). The fourth of each role was added last (564c144), so every
 * earlier name keeps its place. */
#define TF_EP_BUILT 4
static const char *const BUILT[2][TF_EP_BUILT] = {
    {"api.quranlab.ai", "api-b.tajweedformer.com", "api-g.tajweedformer.com", "api-a.tajweedformer.com"},
    {"cdn.quranlab.ai", "cdn-b.tajweedformer.com", "cdn-g.tajweedformer.com", "cdn-a.tajweedformer.com"},
};
static const char *const SUFFIXES[] = {"quranlab.ai", "tajweedformer.com", "quran-lab.com"};

double tf__ep_now_override;
const char *(*tf__ep_test_origin)(const char *host);
static double now_s(void) { return tf__ep_now_override > 0 ? tf__ep_now_override : tf__now_s(); }
static long long wall_s(void) { return tf__now_override ? tf__now_override : (long long)time(NULL); }
const char *tf__ep_role_name(int role) { return role == TF_EP_MODELS ? "models" : "api"; }

/* ------------------------------------------------------------------ 28.1 the allow-list */
static int label_ok(const char *s, size_t n) {
  if (n < 1 || n > 63 || s[0] == '-' || s[n - 1] == '-') return 0;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return 0;
  }
  return 1;
}
int tf__ep_host_allowed(const char *h, size_t n) {
  if (!h || n < 1 || n > 253) return 0;
  for (size_t at = 0; at <= n;) {
    const char *dot = (const char *)memchr(h + at, '.', n - at);
    const size_t end = dot ? (size_t)(dot - h) : n;
    if (!label_ok(h + at, end - at)) return 0;
    at = end + 1;
    if (!dot) break;
  }
  for (size_t i = 0; i < sizeof SUFFIXES / sizeof SUFFIXES[0]; ++i) {
    const size_t k = strlen(SUFFIXES[i]);
    if (n == k && !_tf_strnicmp(h, SUFFIXES[i], k)) return 1;
    if (n > k + 1 && h[n - k - 1] == '.' && !_tf_strnicmp(h + n - k, SUFFIXES[i], k)) return 1;
  }
  return 0;
}
static void lower_copy(char *out, const char *s, size_t n) {
  for (size_t i = 0; i < n; ++i) out[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
  out[n] = 0;
}

/* ------------------------------------------------------------------ 28.4 a lease's base */
int tf__ep_base_parts(const char *b, char host[TF_EP_HOST], char *path, size_t cap) {
  const size_t n = b ? strlen(b) : 0;
  if (n < 9 || n > 511 || strncmp(b, "https://", 8) || b[n - 1] != '/' || strstr(b, "..")) return 0;
  for (size_t i = 0; i < n; ++i)
    if ((unsigned char)b[i] < 0x21 || (unsigned char)b[i] > 0x7e || strchr("?#@\\", b[i])) return 0;
  const char *h = b + 8, *e = strchr(h, '/');
  if (!e) return 0;
  const size_t hn = (size_t)(e - h);
  if (memchr(h, ':', hn) || !tf__ep_host_allowed(h, hn)) return 0;
  if (host) lower_copy(host, h, hn);
  if (path) {
    if (strlen(e) >= cap) return 0;
    snprintf(path, cap, "%s", e);
  }
  return 1;
}

/* ------------------------------------------------------------------ 28.2 the config's lists */
static int built_list(int role, char out[TF_EP_MAX][TF_EP_HOST]) {
  for (int i = 0; i < TF_EP_BUILT; ++i) snprintf(out[i], TF_EP_HOST, "%s", BUILT[role][i]);
  return TF_EP_BUILT;
}
int tf__ep_role_list(int role, const TfJson *ep, int in_force, char out[TF_EP_MAX][TF_EP_HOST]) {
  const char *name = role == TF_EP_MODELS ? "models" : "api";
  const TfJson *l = in_force && ep && ep->type == TJ_OBJ ? tf_json_get(ep, name) : NULL;
  if (!l || l->type != TJ_ARR || l->n > TF_EP_MAX) return built_list(role, out);
  int n = 0;
  for (int i = 0; i < l->n; ++i) {
    const TfJson *v = &l->kids[i];
    if (v->type != TJ_STR || !tf__ep_host_allowed(v->str, strlen(v->str))) continue;
    char h[TF_EP_HOST];
    lower_copy(h, v->str, strlen(v->str));
    int dup = 0;
    for (int k = 0; k < n && !dup; ++k) dup = !strcmp(out[k], h);
    if (!dup) snprintf(out[n++], TF_EP_HOST, "%s", h);
  }
  return n ? n : built_list(role, out);
}

/* ------------------------------------------------------------------ the set */
typedef struct {
  char host[TF_EP_MAX][TF_EP_HOST];
  int n;
  int p;          /* the preferred host P (28.3), an index into host */
  double since;   /* when P became preferred (now_s), while P is not the first host */
  char fixed[300]; /* a test build's override: the role's one host is reached at this origin */
} Role;
struct TfEndpoints {
  TfMutex lock;
  Role r[2];
  char prefix[256]; /* the models folder's prefix: "/models" (MODEL_BASE's path) */
  struct {
    int set, role, status;
    char from[TF_EP_HOST], to[TF_EP_HOST], reason[20];
    long long at;
  } last; /* 28.7 lastFailover */
};

/* a test's override URL: scheme://authority[/path]; its authority is the role's one host name */
static void set_fixed(TfEndpoints *e, int role, const char *url) {
  Role *r = &e->r[role];
  const char *a = strstr(url, "://");
  a = a ? a + 3 : url;
  const char *slash = strchr(a, '/');
  const size_t an = slash ? (size_t)(slash - a) : strlen(a);
  const size_t on = (size_t)(a - url) + an;
  snprintf(r->fixed, sizeof r->fixed, "%.*s", (int)on, url);
  snprintf(r->host[0], TF_EP_HOST, "%.*s", (int)(an < TF_EP_HOST ? an : TF_EP_HOST - 1), a);
  r->n = 1;
  if (role == TF_EP_MODELS) {
    snprintf(e->prefix, sizeof e->prefix, "%s", slash ? slash : "");
    size_t k = strlen(e->prefix);
    while (k && e->prefix[k - 1] == '/') e->prefix[--k] = 0;
  }
}
TfEndpoints *tf__ep_new(const char *api_url, const char *cdn_url) {
  TfEndpoints *e = (TfEndpoints *)calloc(1, sizeof *e);
  if (!e) return NULL;
  tf_mutex_init(&e->lock);
  snprintf(e->prefix, sizeof e->prefix, "/models");
  for (int role = 0; role < 2; ++role) e->r[role].n = built_list(role, e->r[role].host);
  if (api_url && *api_url) set_fixed(e, TF_EP_API, api_url);
  if (cdn_url && *cdn_url) set_fixed(e, TF_EP_MODELS, cdn_url);
  return e;
}
void tf__ep_free(TfEndpoints *e) {
  if (!e) return;
  tf_mutex_destroy(&e->lock);
  free(e);
}
const char *tf__ep_prefix(const TfEndpoints *e) { return e ? e->prefix : "/models"; }

/* 28.3 item 5: a list that changes starts again at its first host */
static void set_hosts(Role *r, char hosts[][TF_EP_HOST], int n) {
  int same = n == r->n;
  for (int i = 0; same && i < n; ++i) same = !strcmp(hosts[i], r->host[i]);
  if (same || n < 1) return;
  for (int i = 0; i < n && i < TF_EP_MAX; ++i) snprintf(r->host[i], TF_EP_HOST, "%s", hosts[i]);
  r->n = n < TF_EP_MAX ? n : TF_EP_MAX;
  r->p = 0;
  r->since = 0;
}
void tf__ep_set_lists(TfEndpoints *e, int role, char hosts[][TF_EP_HOST], int n) {
  if (!e) return;
  tf_mutex_lock(&e->lock);
  if (!e->r[role].fixed[0]) set_hosts(&e->r[role], hosts, n);
  tf_mutex_unlock(&e->lock);
}
void tf__ep_set_config(TfEndpoints *e, const char *config_json, int in_force) {
  if (!e) return;
  TfJson *j = config_json && in_force ? tf_json_parse(config_json, strlen(config_json), NULL) : NULL;
  const TfJson *ep = tf_json_get(j, "endpoints");
  for (int role = 0; role < 2; ++role) {
    char l[TF_EP_MAX][TF_EP_HOST];
    const int n = tf__ep_role_list(role, ep, in_force && j, l);
    tf__ep_set_lists(e, role, l, n);
  }
  tf_json_free(j);
}

static int index_of(const Role *r, const char *h) {
  for (int i = 0; h && i < r->n; ++i)
    if (!strcmp(r->host[i], h)) return i;
  return -1;
}
void tf__ep_origin(const TfEndpoints *e, int role, const char *host, char *out, size_t n) {
  const Role *r = &e->r[role];
  const char *o = r->fixed[0] && !strcmp(host, r->host[0]) ? r->fixed : tf__ep_test_origin ? tf__ep_test_origin(host) : NULL;
  if (o) snprintf(out, n, "%s", o);
  else snprintf(out, n, "https://%s", host);
}
void tf__ep_order(TfEndpoints *e, int role, const char *first, TfEpOrder *o) {
  tf_mutex_lock(&e->lock);
  const Role *r = &e->r[role];
  memset(o, 0, sizeof *o);
  if (first && *first && index_of(r, first) < 0) snprintf(o->host[o->n++], TF_EP_HOST, "%s", first);
  o->probe = r->p != 0 && now_s() - r->since >= TF_EP_PROBE_S;
  const int p = o->probe ? 0 : r->p;
  snprintf(o->host[o->n++], TF_EP_HOST, "%s", r->host[p]);
  for (int i = 0; i < r->n; ++i)
    if (i != p) snprintf(o->host[o->n++], TF_EP_HOST, "%s", r->host[i]);
  tf_mutex_unlock(&e->lock);
}
void tf__ep_settle(TfEndpoints *e, int role, const char *host, int probe) {
  if (!e) return;
  tf_mutex_lock(&e->lock);
  Role *r = &e->r[role];
  const int i = index_of(r, host);
  if (i >= 0 && (i != r->p || probe)) {
    r->p = i;
    r->since = i ? now_s() : 0;
  }
  tf_mutex_unlock(&e->lock);
}
/* the preferred host P of a role (28.3), whatever the probe would ask first; tests and the diagnosis's reader */
void tf__ep_preferred(TfEndpoints *e, int role, char out[TF_EP_HOST]) {
  out[0] = 0;
  if (!e) return;
  tf_mutex_lock(&e->lock);
  snprintf(out, TF_EP_HOST, "%s", e->r[role].host[e->r[role].p]);
  tf_mutex_unlock(&e->lock);
}
void tf__ep_failed_over(TfEndpoints *e, int role, const char *from, const char *to, const char *reason, int status) {
  if (!e) return;
  tf_mutex_lock(&e->lock);
  e->last.set = 1;
  e->last.role = role;
  e->last.status = status;
  snprintf(e->last.from, sizeof e->last.from, "%s", from ? from : "");
  snprintf(e->last.to, sizeof e->last.to, "%s", to ? to : "");
  snprintf(e->last.reason, sizeof e->last.reason, "%s", reason ? reason : "network");
  e->last.at = wall_s();
  tf_mutex_unlock(&e->lock);
}
/* 28.7: {"api","models","lastFailover"}: the host each role's next request asks first, and the last failover */
char *tf__ep_diag_json(TfEndpoints *e) {
  if (!e) return NULL;
  TfEpOrder oa, om;
  tf__ep_order(e, TF_EP_API, NULL, &oa);
  tf__ep_order(e, TF_EP_MODELS, NULL, &om);
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"api\":");
  tf_buf_json_str(&b, oa.host[0]);
  tf_buf_puts(&b, ",\"models\":");
  tf_buf_json_str(&b, om.host[0]);
  tf_buf_puts(&b, ",\"lastFailover\":");
  tf_mutex_lock(&e->lock);
  if (!e->last.set) {
    tf_buf_puts(&b, "null");
  } else {
    tf_buf_printf(&b, "{\"role\":\"%s\",\"from\":", tf__ep_role_name(e->last.role));
    tf_buf_json_str(&b, e->last.from);
    tf_buf_puts(&b, ",\"to\":");
    if (e->last.to[0]) tf_buf_json_str(&b, e->last.to);
    else tf_buf_puts(&b, "null");
    tf_buf_printf(&b, ",\"reason\":\"%s\",\"status\":", e->last.reason);
    if (e->last.status) tf_buf_printf(&b, "%d", e->last.status);
    else tf_buf_puts(&b, "null");
    tf_buf_printf(&b, ",\"at\":%lld}", e->last.at);
  }
  tf_mutex_unlock(&e->lock);
  tf_buf_puts(&b, "}");
  return tf_buf_take(&b);
}

/* ------------------------------------------------------------------ 28.3 one answer, one request */
static _Thread_local int t_note_role = -1, t_note_fallback;
void tf__ep_note(int role, int fallback) {
  t_note_role = role;
  t_note_fallback = fallback != 0;
}
int tf__ep_noted(int *role, int *fallback) {
  if (t_note_role < 0) return 0;
  if (role) *role = t_note_role;
  if (fallback) *fallback = t_note_fallback;
  return 1;
}
void tf__ep_note_clear(void) { t_note_role = -1; }

/* a body that is empty or JSON: the generic answer of an API route (an HTML page is not) */
static int json_or_empty(const TfHttpResponse *r) {
  if (!r->body || !r->body_n) return 1;
  TfJson *j = tf_json_parse(r->body, r->body_n, NULL);
  const int ok = j != NULL;
  tf_json_free(j);
  return ok;
}
/* a 4xx is a refusal only with the API's own body: JSON whose `error` is a string */
static int api_error(const TfHttpResponse *r) {
  if (!r->body || !r->body_n || r->body_n >= 65536) return 0;
  TfJson *j = tf_json_parse(r->body, r->body_n, NULL);
  const int ok = j && j->type == TJ_OBJ && tf_json_str(tf_json_get(j, "error")) != NULL;
  tf_json_free(j);
  return ok;
}
int tf__ep_class(const TfEpReq *er, int rc, const TfHttpResponse *r) {
  if (rc) return TF_EP_UNREACHABLE;
  const int st = r->status;
  if (st == 429) return TF_EP_SLOW;
  if (st == 304) return er && er->not_modified_ok ? TF_EP_TAKEN : TF_EP_UNREACHABLE;
  if (st >= 200 && st < 300) {
    const int ok = er && er->taken ? er->taken(er->user, r) : json_or_empty(r);
    return ok ? TF_EP_TAKEN : TF_EP_UNREACHABLE;
  }
  if (st >= 400 && st < 500) return api_error(r) ? TF_EP_REFUSED : TF_EP_UNREACHABLE;
  return TF_EP_UNREACHABLE; /* a redirect (never followed), a 5xx, anything else */
}
/* 24.3: the reason of a host's answer that was unreachable (28.7 lastFailover, the failover report) */
const char *tf__ep_reason(int rc, const TfHttpResponse *r) {
  if (rc) {
    const int kind = tf__http_failure(NULL, NULL);
    if (kind == TF__FAIL_ANSWER) return "invalid_answer";
    return kind == TF__FAIL_TIMEOUT && r->status > 0 ? "timeout" : "network";
  }
  if (r->status >= 300 && r->status < 400) return "redirect";
  if (r->status >= 200 && r->status < 300) return "invalid_answer";
  return "http";
}

int tf__ep_http(TfEndpoints *e, const TfEpReq *er, TfHttpRequest *q, TfHttpResponse *r, TfEpOut *out) {
  TfEpOut o;
  memset(&o, 0, sizeof o);
  o.cls = TF_EP_UNREACHABLE;
  TfEpOrder ord;
  tf__ep_order(e, er->role, er->first, &ord);
  const int timeout0 = q->timeout_s;
  if (q->head_ms <= 0) q->head_ms = TF_EP_HEAD_MS; /* 28.3 item 6: 10 s to the answer's headers on each host */
  TfErrSave *before = tf__error_save(), *first_err = NULL;
  TfHttpState first_state;
  memset(&first_state, 0, sizeof first_state);
  TfHttpResponse first;
  memset(&first, 0, sizeof first);
  int rc_first = -1, max_ra = 0, rc = -1;
  char url[4096], origin[320];
  for (int i = 0; i < ord.n; ++i) {
    if (i > 0 && q->cancel && *q->cancel) break; /* the call is being abandoned: no other host */
    if (er->deadline > 0) {
      const double left = er->deadline - tf__now_s();
      if (i > 0 && left <= 0) break; /* 18.4.7.1: past the open's wait no host is asked */
      if (left > 0 && (q->timeout_s <= 0 || left < q->timeout_s)) q->timeout_s = left < 1 ? 1 : (int)(left + 0.999);
    }
    tf__ep_origin(e, er->role, ord.host[i], origin, sizeof origin);
    snprintf(url, sizeof url, "%s%s", origin, er->path);
    q->url = url;
    TfHttpResponse ri;
    memset(&ri, 0, sizeof ri);
    rc = tf_http(q, &ri);
    q->timeout_s = timeout0;
    const int cls = tf__ep_class(er, rc, &ri);
    o.asked = i + 1;
    if (cls != TF_EP_UNREACHABLE) {
      o.cls = cls;
      snprintf(o.by, sizeof o.by, "%s", ord.host[i]);
      tf__ep_settle(e, er->role, ord.host[i], ord.probe);
      *r = ri;
      break;
    }
    if (ri.retry_after > max_ra) max_ra = ri.retry_after;
    if (i == 0) {
      snprintf(o.first_reason, sizeof o.first_reason, "%s", tf__ep_reason(rc, &ri));
      o.first_status = rc ? (tf__http_failure(NULL, NULL) == TF__FAIL_ANSWER ? ri.status : 0) : ri.status;
      first = ri;
      rc_first = rc;
      tf__http_state_save(&first_state);
      first_err = rc ? tf__error_save() : NULL;
    } else {
      tf_http_response_free(&ri);
    }
  }
  o.fallback = o.asked > 1;
  if (o.cls == TF_EP_UNREACHABLE) {
    /* every host asked was unreachable: the first host's answer is the request's (28.3 item 4) */
    *r = first;
    memset(&first, 0, sizeof first);
    r->retry_after = max_ra;
    rc = rc_first;
    tf__http_state_restore(&first_state);
    if (rc && first_err) tf__error_restore(first_err, 0);
    else tf__error_restore(before, 0);
    /* 28.7: lastFailover records every request whose first host was unreachable, a role of one
     * host and an offline device too, with no host that answered */
    if (o.asked > 0) tf__ep_failed_over(e, er->role, ord.host[0], NULL, o.first_reason, o.first_status);
  } else {
    tf__error_restore(before, 0);
    if (o.asked > 1) {
      tf__ep_failed_over(e, er->role, ord.host[0], o.by, o.first_reason, o.first_status);
      /* 28.7, 24.1 item 2: answered on another host after the first was unreachable, silently reported */
      tf__ep_note(er->role, 1);
      if (er->stage) tf__rep_silent(er->stage, "failover", o.first_status, NULL, o.first_reason);
    }
  }
  tf_http_response_free(&first);
  free(before);
  free(first_err);
  tf__ep_note(er->role, o.fallback);
  if (out) *out = o;
  return rc;
}
