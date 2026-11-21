/* SDK 4.0.1, included by tf40_tests.c:
 *
 *   tf40_tests app27 <fixture> <work>       APP_VERSION_40.md 27.3: the cleaning (check 7), app_version and app_build
 *                                           on every config request (checks 1, 3, 6), register's app_version (check 2),
 *                                           tf_client_create and tf_client_create2 with an ext of each size (check 5)
 *   tf40_tests endpoints <vectors> <fixture> <work>   ENDPOINTS_40.md 28.8: endpoints_40.json on this SDK's code: hosts
 *                                           (the allow-list), config_lists (28.2), bases and first_manifest (28.4),
 *                                           answers (28.3's table), requests (sequences on a fake clock through
 *                                           tf__ep_http), attempts (lease attempts through the Client and the Core),
 *                                           downloads (one file through 16.2's resumes across hosts, with
 *                                           downloads_if_range); standby_40.json's answers as the SDK reads them
 *   tf40_tests loopback <fixture> <work>    28.8's checks on this platform's real HTTP code (WinHTTP, libcurl) against
 *                                           stand-in servers on 127.0.0.1 (tf_loopback.c): a new install whose first
 *                                           hosts refuse every connection, a 503, a refusal on either host, a 429, the
 *                                           probe back, and a file resumed on the next host */
#include "tf_loopback.h"

/* ------------------------------------------------------------------ section 27 */
static int app27_client(const char *work, const char *label, const char *version, const TfClientOptionsExt *ext, int use_create2, TfClient **out) {
  char w[1100];
  snprintf(w, sizeof w, "%s/%s", work, label);
  rmrf(w);
  TfClientOptions o;
  memset(&o, 0, sizeof o);
  o.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "com.example.recite";
  o.app_version = version;
  o.storage = w;
  fake_reset(LEASE_OK);
  *out = use_create2 ? tf__client_create2_with_key(&o, ext, S(g_f.fx, "signer_hex")) : tf__client_create_with_key(&o, S(g_f.fx, "signer_hex"));
  if (!*out) return tf_last_error_code();
  /* the first config request, then a refresh (If-None-Match while the config is fresh), then register */
  tf__client_refresh_config(*out);
  char first[600];
  snprintf(first, sizeof first, "%s", g_f.last_config);
  tf__client_refresh_config(*out);
  CHECK(g_f.configs == 2 && !strcmp(first, g_f.last_config), "app27 %s: both config requests carry the same query (%d: %s, %s)", label, g_f.configs, first,
        g_f.last_config);
  tf__client_register(*out);
  return 0;
}
/* the config query after sdk=: what section 27 adds ("" for nothing) */
static const char *app27_tail(void) {
  const char *p = strstr(g_f.last_config, "&sdk=" TF_SDK_VERSION);
  return p ? p + strlen("&sdk=" TF_SDK_VERSION) : "(no sdk parameter)";
}
static int app27_register_is(const char *want) {
  char needle[80];
  snprintf(needle, sizeof needle, "\"app_version\":\"%s\"", want);
  return strstr(g_f.last_register, needle) != NULL;
}
static int t_app27(const char *fixture, const char *work) {
  TfJson *fx = flow_setup(fixture);
  if (!fx) return 2;
  /* check 7: the one cleaning, in its order */
  static const char *const clean[][2] = {
      {" 7.1 (beta)", "7.1beta"}, {"-7.1", "7.1"}, {"v2.0+42", "v2.0+42"}, {"7.1-\xc3\xa9", "7.1-"}, {"...", ""}, {"0", ""}, {"", ""},
      {"+.-0.1", "0.1"}, {".+-", ""}, {"\xc3\xa9" "7", "7"}, {"00", "00"}, {"1234567890123456789012345678901234567890", "12345678901234567890123456789012"},
      {"-----------------------------------1", "1"}, {"412", "412"}};
  for (size_t i = 0; i < sizeof clean / sizeof clean[0]; ++i) {
    char out[33];
    tf__clean27(clean[i][0], out);
    CHECK(!strcmp(out, clean[i][1]), "app27: \"%s\" is cleaned to \"%s\" (got \"%s\")", clean[i][0], clean[i][1], out);
  }
  {
    char out[33];
    tf__clean27(NULL, out);
    CHECK(!out[0], "app27: no value is nothing");
  }
  /* 27.4: TfClientOptions is 4.0.0's, its last field `user` */
  CHECK(offsetof(TfClientOptions, user) + sizeof(void *) == sizeof(TfClientOptions), "app27: TfClientOptions ends at 4.0.0's last field");
  TfClient *c = NULL;
  /* check 5 and 6: a 4.0.0 call (tf_client_create, no version): the request is 4.0.0's, register sends "0" (check 2) */
  CHECK(!app27_client(work, "v400", NULL, NULL, 0, &c) && !*app27_tail() && app27_register_is("0"),
        "app27: tf_client_create with no version: no parameter added (%s), register \"0\"", g_f.last_config);
  if (c) tf_client_destroy(c);
  CHECK(!app27_client(work, "null_ext", "1.4", NULL, 1, &c) && !strcmp(app27_tail(), "&app_version=1.4") && app27_register_is("1.4"),
        "app27: tf_client_create2(options, NULL): app_version only (%s)", app27_tail());
  if (c) tf_client_destroy(c);
  TfClientOptionsExt ext;
  memset(&ext, 0, sizeof ext);
  ext.app_build = "412";
  ext.size = 4; /* the size field alone: app_build lies past it, so it is not read */
  CHECK(!app27_client(work, "ext4", "1.4", &ext, 1, &c) && !strcmp(app27_tail(), "&app_version=1.4"), "app27: an ext of size 4 adds nothing (%s)",
        app27_tail());
  if (c) tf_client_destroy(c);
  ext.size = (uint32_t)(offsetof(TfClientOptionsExt, app_build) + sizeof(const char *)) - 1; /* app_build not wholly within */
  CHECK(!app27_client(work, "ext_short", "1.4", &ext, 1, &c) && !strcmp(app27_tail(), "&app_version=1.4"), "app27: a field past size is not read (%s)",
        app27_tail());
  if (c) tf_client_destroy(c);
  ext.size = sizeof ext;
  CHECK(!app27_client(work, "ext_full", "1.4", &ext, 1, &c) && !strcmp(app27_tail(), "&app_version=1.4&app_build=412"),
        "app27: an ext of its full size with app_build 412 (%s)", app27_tail());
  if (c) tf_client_destroy(c);
  ext.size = 3;
  c = NULL;
  CHECK(app27_client(work, "ext3", "1.4", &ext, 1, &c) == TF_ERR_INVALID_ARGUMENT && !c, "app27: an ext whose size is below 4 is invalid_argument, no Client");
  if (c) tf_client_destroy(c);
  /* checks 1, 3 and 7 on the wire: the cleaned values, '+' as %2B, register the same string */
  static const struct {
    const char *version, *build, *tail, *reg;
  } wire[] = {
      {" 7.1 (beta)", "412", "&app_version=7.1beta&app_build=412", "7.1beta"},
      {"7.1+beta", NULL, "&app_version=7.1%2Bbeta", "7.1+beta"},
      {"v2.0+42", " (build 42) ", "&app_version=v2.0%2B42&app_build=build42", "v2.0+42"},
      {"-7.1", "0", "&app_version=7.1", "7.1"},
      {"...", "...", "", "0"},
      {"0", "1234567890123456789012345678901234567890", "&app_build=12345678901234567890123456789012", "0"},
  };
  for (size_t i = 0; i < sizeof wire / sizeof wire[0]; ++i) {
    char label[32];
    snprintf(label, sizeof label, "wire%d", (int)i);
    ext.size = sizeof ext;
    ext.app_build = wire[i].build;
    CHECK(!app27_client(work, label, wire[i].version, &ext, 1, &c) && !strcmp(app27_tail(), wire[i].tail) && app27_register_is(wire[i].reg),
          "app27: version \"%s\", build \"%s\": the query ends \"%s\" (got \"%s\"), register \"%s\" (%s)", wire[i].version,
          wire[i].build ? wire[i].build : "(none)", wire[i].tail, app27_tail(), wire[i].reg, g_f.last_register);
    if (c) tf_client_destroy(c);
  }
  tf_http_set_transport(NULL, NULL);
  tf_json_free(fx);
  return done("app27");
}

/* ------------------------------------------------------------------ section 28: the replays */
static int ep_role(const char *name) { return !strcmp(name, "models") ? TF_EP_MODELS : TF_EP_API; }
static int ep_class_of(const char *name) {
  return !strcmp(name, "taken") ? TF_EP_TAKEN : !strcmp(name, "refused") ? TF_EP_REFUSED : !strcmp(name, "slow_down") ? TF_EP_SLOW : TF_EP_UNREACHABLE;
}
/* the hosts of a JSON array against a list */
static int ep_same(const TfJson *arr, char hosts[][TF_EP_HOST], int n) {
  if (!arr || arr->type != TJ_ARR || arr->n != n) return 0;
  for (int i = 0; i < n; ++i)
    if (!tf_json_str(&arr->kids[i]) || strcmp(tf_json_str(&arr->kids[i]), hosts[i])) return 0;
  return 1;
}
/* one host's answer of the vectors (answers_are): none or stalled is no answer; else a status with a body of its kind.
 * A 2xx that is the route's own answer carries "route_answer":true; one that fails the route's check ("invalid", a
 * lease the Core refuses) carries false; ep_taken reads it, as each route's own check does in the library */
static int ep_answer(const TfJson *a, TfHttpResponse *r) {
  const char *none = S(a, "answer");
  if (*none) {
    if (!strcmp(none, "stalled")) {
      r->status = 200;
      tf__http_note(TF__FAIL_TIMEOUT, "stalled", 0);
    }
    return tf__fail(TF__ERR_NETWORK, "no answer (%s)", none);
  }
  const int st = (int)N(a, "status");
  const char *body = S(a, "body"), *err = S(a, "error"), *why = S(a, "reason"), *core = S(a, "core");
  TfBuf o;
  tf_buf_init(&o);
  if (!strcmp(body, "html")) {
    tf_buf_puts(&o, "<html><body>a page that is not the API's</body></html>");
  } else if (!strcmp(body, "bytes")) {
    tf_buf_puts(&o, "BYTES");
  } else if (!strcmp(body, "invalid")) {
    tf_buf_puts(&o, "{\"route_answer\":false}");
  } else if (!strcmp(body, "json")) {
    if (*err) {
      tf_buf_puts(&o, "{\"error\":");
      tf_buf_json_str(&o, err);
      if (*why) {
        tf_buf_puts(&o, ",\"reason\":");
        tf_buf_json_str(&o, why);
      }
      tf_buf_puts(&o, "}");
    } else if (st >= 200 && st < 300) {
      tf_buf_puts(&o, strcmp(core, "refused") ? "{\"route_answer\":true}" : "{\"route_answer\":false}");
    } else {
      tf_buf_puts(&o, "{\"message\":\"no error string\"}");
    }
  }
  r->status = st;
  r->body_n = o.p ? o.n : 0;
  r->body = o.p ? o.p : dupz("", 0);
  if (tf_json_get(a, "retry_after") && tf_json_get(a, "retry_after")->type == TJ_NUM) r->retry_after = (int)N(a, "retry_after");
  return 0;
}
static int ep_taken(void *user, const TfHttpResponse *r) {
  (void)user;
  return r->body && (strstr(r->body, "\"route_answer\":true") || !strcmp(r->body, "BYTES"));
}
/* the transport of a request step: each host answers what the step says; the hosts asked are recorded */
static const TfJson *g_ep_answers;
static char g_ep_asked[24][TF_EP_HOST], g_ep_path[700];
static int g_ep_n;
static long long ep_fsize(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  const long long n = ftell(f);
  fclose(f);
  return n;
}
static void url_host(const char *url, char out[TF_EP_HOST]) {
  const char *h = strstr(url, "://");
  h = h ? h + 3 : url;
  const char *e = strchr(h, '/');
  const size_t n = e ? (size_t)(e - h) : strlen(h);
  snprintf(out, TF_EP_HOST, "%.*s", (int)(n < TF_EP_HOST ? n : TF_EP_HOST - 1), h);
}
static int ep_transport(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  char host[TF_EP_HOST];
  url_host(q->url, host);
  if (g_ep_n < 24) snprintf(g_ep_asked[g_ep_n++], TF_EP_HOST, "%s", host);
  const char *path = strstr(q->url, "://");
  path = path ? strchr(path + 3, '/') : NULL;
  snprintf(g_ep_path, sizeof g_ep_path, "%s", path ? path : "");
  const TfJson *a = tf_json_get(g_ep_answers, host);
  if (!a) return tf__fail(TF__ERR_NETWORK, "%s is not in the step", host);
  return ep_answer(a, r);
}
static int ep_asked_is(const TfJson *arr) { return ep_same(arr, g_ep_asked, g_ep_n); }

/* downloads: one file through fetch_raw (16.2, 28.4) on a stand-in that delivers what each attempt of the case gets */
static struct {
  const TfJson *c;
  int i;
  unsigned char *data;
  long long size, pause_ms;
  char host[24][TF_EP_HOST], if_range[24][96];
  long long range[24], pause[24];
} g_dl;
static void dl_on_pause(long long ms) { g_dl.pause_ms = ms; }
static int dl_transport(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  const int i = g_dl.i++;
  if (i < 24) {
    url_host(q->url, g_dl.host[i]);
    g_dl.range[i] = q->range_from;
    snprintf(g_dl.if_range[i], sizeof g_dl.if_range[i], "%s", q->if_range ? q->if_range : "");
    g_dl.pause[i] = g_dl.pause_ms;
  }
  g_dl.pause_ms = 0;
  const TfJson *ans = tf_json_get(g_dl.c, "answers");
  if (!ans || i >= ans->n) return tf__fail(TF__ERR_NETWORK, "past the case's answers");
  const TfJson *a = &ans->kids[i];
  if (*S(a, "answer")) return tf__fail(TF__ERR_NETWORK, "no answer (%s)", S(a, "answer"));
  const int st = (int)N(a, "status");
  const char *body = S(a, "body");
  const long long from = q->range_from > 0 ? q->range_from : 0;
  if ((st == 200 || st == 206) && (!strcmp(body, "bytes") || !strcmp(body, "invalid"))) {
    char host[TF_EP_HOST];
    url_host(q->url, host);
    r->status = st;
    if (st == 200) snprintf(r->etag, sizeof r->etag, "%s", S(tf_json_get(g_dl.c, "etags"), host));
    r->content_length = st == 200 ? g_dl.size : g_dl.size - from;
    if (st == 206) {
      r->range_start = from;
      r->range_total = !strcmp(body, "invalid") ? g_dl.size + 1 : g_dl.size; /* a 206 of another total */
    }
    if (q->accept && !q->accept(q->bytes_user, r)) {
      tf__http_note(TF__FAIL_ANSWER, NULL, 0);
      return tf__fail(TF__ERR_NETWORK, "the answer (HTTP %d) is not the one that was asked for", st);
    }
    const char *file = st == 200 && from > 0 && q->download_whole ? q->download_whole : q->download_file;
    FILE *f = fopen(file, st == 206 ? "ab" : "wb");
    if (!f) return tf__fail(TF__ERR_IO, "cannot write %s", file);
    if (q->on_head) q->on_head(q->bytes_user, r);
    const long long start = st == 206 ? from : 0, n = (long long)N(a, "deliver");
    fwrite(g_dl.data + start, 1, (size_t)n, f);
    fclose(f);
    if (q->on_bytes) q->on_bytes(q->bytes_user, n);
    const char *end = S(a, "end");
    if (!strcmp(end, "stall")) {
      tf__http_note(TF__FAIL_TIMEOUT, "stalled", 0);
      return tf__fail(TF__ERR_NETWORK, "the answer stalled");
    }
    if (!strcmp(end, "reset")) return tf__fail(TF__ERR_NETWORK, "the connection was reset");
    return 0;
  }
  return ep_answer(a, r);
}

static int t_endpoints(const char *vec, const char *fixture, const char *work) {
  TfJson *v = load_json(vec, "endpoints_40.json"), *sb = load_json(vec, "standby_40.json");
  if (!v || !sb) return 2;
  int n;
  /* roles: the built-in lists are 28.1's */
  {
    TfEndpoints *e = tf__ep_new(NULL, NULL);
    for (int role = 0; role < 2; ++role) {
      TfEpOrder o;
      tf__ep_order(e, role, NULL, &o);
      CHECK(ep_same(tf_json_get(tf_json_get(v, "roles"), role ? "models" : "api"), o.host, o.n), "roles: the built-in %s hosts are 28.1's, in order",
            role ? "models" : "api");
    }
    tf__ep_free(e);
  }
  /* hosts: the allow-list */
  const TfJson *hs = tf_json_get(v, "hosts");
  n = 0;
  for (int i = 0; hs && i < hs->n; ++i, ++n) {
    const char *h = S(&hs->kids[i], "host");
    const int want = tf_json_get(&hs->kids[i], "allowed")->boolean;
    CHECK(tf__ep_host_allowed(h, strlen(h)) == want, "hosts: %s %s", h, want ? "allowed" : "refused");
  }
  printf("  hosts: %d\n", n);
  /* config_lists (28.2) */
  const TfJson *cl = tf_json_get(v, "config_lists");
  for (int i = 0; cl && i < cl->n; ++i) {
    const TfJson *k = &cl->kids[i];
    for (int role = 0; role < 2; ++role) {
      char out[TF_EP_MAX][TF_EP_HOST];
      const int m = tf__ep_role_list(role, tf_json_get(k, "endpoints"), tf_json_get(k, "in_force")->boolean, out);
      CHECK(ep_same(tf_json_get(tf_json_get(k, "lists"), role ? "models" : "api"), out, m), "config_lists: %s, the %s list", S(k, "name"),
            role ? "models" : "api");
    }
    /* the same through the set a Client keeps: a config's text, in force or not (tf__client_endpoints gives it the
     * config in memory and whether it is past expires_at) */
    TfBuf ct;
    tf_buf_init(&ct);
    tf_buf_puts(&ct, "{\"v\":1,\"endpoints\":");
    if (tf_json_get(k, "endpoints")) tf_json_write(&ct, tf_json_get(k, "endpoints"));
    else tf_buf_puts(&ct, "null");
    tf_buf_puts(&ct, "}");
    TfEndpoints *e = tf__ep_new(NULL, NULL);
    tf__ep_set_config(e, ct.p, tf_json_get(k, "in_force")->boolean);
    for (int role = 0; role < 2; ++role) {
      TfEpOrder o;
      tf__ep_order(e, role, NULL, &o);
      CHECK(ep_same(tf_json_get(tf_json_get(k, "lists"), role ? "models" : "api"), o.host, o.n), "config_lists: %s, the %s order of a Client's set",
            S(k, "name"), role ? "models" : "api");
    }
    tf__ep_free(e);
    tf_buf_free(&ct);
  }
  printf("  config_lists: %d\n", cl ? cl->n : 0);
  /* bases (28.4), and the hosts a build's first model request asks, with P first and with P on the standby */
  tf__ep_now_override = 1000;
  const TfJson *bs = tf_json_get(v, "bases");
  for (int i = 0; bs && i < bs->n; ++i) {
    const TfJson *k = &bs->kids[i];
    char host[TF_EP_HOST] = "", path[600] = "";
    const int ok = tf__ep_base_parts(S(k, "base"), host, path, sizeof path);
    const int want = tf_json_get(k, "accepted")->boolean;
    CHECK(ok == want, "bases: %s %s", S(k, "name"), want ? "accepted" : "refused");
    if (!ok || !want) continue;
    CHECK(!strcmp(host, S(k, "host")) && !strcmp(path, S(k, "path")), "bases: %s: host %s, path %s", S(k, "name"), host, path);
    TfEndpoints *e = tf__ep_new(NULL, NULL);
    TfEpOrder o;
    tf__ep_order(e, TF_EP_MODELS, host, &o);
    CHECK(ep_same(tf_json_get(k, "asks_with_p_first"), o.host, o.n), "bases: %s: the hosts asked with P first", S(k, "name"));
    tf__ep_settle(e, TF_EP_MODELS, "cdn-b.tajweedformer.com", 0);
    tf__ep_order(e, TF_EP_MODELS, host, &o);
    CHECK(ep_same(tf_json_get(k, "asks_with_p_standby"), o.host, o.n), "bases: %s: the hosts asked with P on the standby", S(k, "name"));
    tf__ep_free(e);
  }
  printf("  bases: %d\n", bs ? bs->n : 0);
  /* first_manifest (28.4): through the downloader itself (no host answers, so each is asked once, in order), and the
   * order with P on the standby */
  const TfJson *fm = tf_json_get(v, "first_manifest");
  for (int i = 0; fm && i < fm->n; ++i) {
    const TfJson *k = &fm->kids[i], *rec = tf_json_get(k, "recommended");
    g_ep_answers = NULL;
    g_ep_n = 0;
    tf_http_set_transport(ep_transport, NULL);
    TfDownload d = tf__download_defaults();
    char dest[1100];
    snprintf(dest, sizeof dest, "%s/first_manifest%d", work, i);
    d.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
    d.repo = S(rec, "model");
    d.revision = S(rec, "revision");
    d.folder = S(rec, "folder");
    d.dest = dest;
    d.base_hint = tf_json_str(tf_json_get(k, "lease_base"));
    const int rc = tf__download_model(&d);
    tf_http_set_transport(NULL, NULL);
    CHECK(rc && ep_asked_is(tf_json_get(k, "asks_with_p_first")), "first_manifest: %s: every host asked once, in order (%d asked)", S(k, "name"), g_ep_n);
    CHECK(!strcmp(g_ep_path, S(k, "path")), "first_manifest: %s: the path asked %s", S(k, "name"), g_ep_path);
    char folder[600] = "", host[TF_EP_HOST] = "";
    const char *lb = tf_json_str(tf_json_get(k, "lease_base"));
    if (lb) tf__ep_base_parts(lb, host, folder, sizeof folder);
    TfEndpoints *e = tf__ep_new(NULL, NULL);
    TfEpOrder o;
    tf__ep_settle(e, TF_EP_MODELS, "cdn-b.tajweedformer.com", 0);
    tf__ep_order(e, TF_EP_MODELS, host[0] ? host : NULL, &o);
    CHECK(ep_same(tf_json_get(k, "asks_with_p_standby"), o.host, o.n), "first_manifest: %s: the hosts with P on the standby", S(k, "name"));
    tf__ep_free(e);
  }
  printf("  first_manifest: %d\n", fm ? fm->n : 0);
  /* answers: 28.3's table, the same for every route */
  const TfJson *as = tf_json_get(v, "answers");
  for (int i = 0; as && i < as->n; ++i) {
    const TfJson *k = &as->kids[i];
    TfHttpResponse r;
    memset(&r, 0, sizeof r);
    r.content_length = r.range_start = r.range_total = -1;
    tf__http_note(TF__FAIL_NETWORK, NULL, 0);
    const int rc = ep_answer(tf_json_get(k, "answer"), &r);
    TfEpReq er;
    memset(&er, 0, sizeof er);
    er.taken = ep_taken;
    er.not_modified_ok = !strcmp(S(k, "route"), "config");
    const int cls = tf__ep_class(&er, rc, &r);
    CHECK(cls == ep_class_of(S(k, "class")), "answers %d (%s, %s): %s, got %d", i, S(k, "route"), r.body ? r.body : "no answer", S(k, "class"), cls);
    tf_http_response_free(&r);
    tf__clear_error();
  }
  printf("  answers: %d\n", as ? as->n : 0);
  /* requests: sequences on one role over time, through tf__ep_http */
  const TfJson *rs = tf_json_get(v, "requests");
  int seqs = 0, steps = 0;
  tf_http_set_transport(ep_transport, NULL);
  for (int i = 0; rs && i < rs->n; ++i) {
    const TfJson *k = &rs->kids[i];
    if (!strcmp(S(k, "role"), "web_frame")) continue; /* web only: C has no web_frame role */
    const int role = ep_role(S(k, "role"));
    TfEndpoints *e = tf__ep_new(NULL, NULL);
    const TfJson *st = tf_json_get(k, "steps");
    seqs++;
    for (int j = 0; st && j < st->n; ++j) {
      const TfJson *s = &st->kids[j];
      char p[TF_EP_HOST];
      const TfJson *list = tf_json_get(s, "list");
      if (list) {
        char l[TF_EP_MAX][TF_EP_HOST];
        int m = 0;
        for (int x = 0; x < list->n && m < TF_EP_MAX; ++x) snprintf(l[m++], TF_EP_HOST, "%s", tf_json_str(&list->kids[x]));
        tf__ep_set_lists(e, role, l, m);
        tf__ep_preferred(e, role, p);
        CHECK(!strcmp(p, S(s, "p_after")), "requests: %s, step %d: a new list, P %s (%s)", S(k, "name"), j, S(s, "p_after"), p);
        continue;
      }
      steps++;
      tf__ep_now_override = 1000 + N(s, "at");
      TfEpOrder o;
      tf__ep_order(e, role, NULL, &o);
      g_ep_answers = tf_json_get(s, "answers");
      g_ep_n = 0;
      TfHttpRequest q;
      memset(&q, 0, sizeof q);
      q.method = "GET";
      q.timeout_s = 30;
      TfEpReq er;
      memset(&er, 0, sizeof er);
      er.role = role;
      er.path = "/v1/x";
      er.taken = ep_taken;
      er.not_modified_ok = !strcmp(S(s, "route"), "config");
      TfHttpResponse r;
      memset(&r, 0, sizeof r);
      TfEpOut out;
      tf__ep_http(e, &er, &q, &r, &out);
      tf_http_response_free(&r);
      tf__clear_error();
      tf__ep_preferred(e, role, p);
      const char *by = tf_json_str(tf_json_get(s, "by"));
      CHECK(ep_asked_is(tf_json_get(s, "asked")) && out.cls == ep_class_of(S(s, "class")) && !strcmp(out.by, by ? by : "") &&
                out.fallback == tf_json_get(s, "fallback")->boolean && o.probe == tf_json_get(s, "probe")->boolean && !strcmp(p, S(s, "p_after")),
            "requests: %s, step %d at %lld: asked %d (first %s), class %d, by %s, fallback %d, probe %d, P %s", S(k, "name"), j, (long long)N(s, "at"), g_ep_n,
            g_ep_n ? g_ep_asked[0] : "-", out.cls, out.by, out.fallback, o.probe, p);
    }
    tf__ep_free(e);
  }
  tf_http_set_transport(NULL, NULL);
  tf__ep_now_override = 0;
  printf("  requests: %d sequences, %d steps (the web_frame sequence is web's)\n", seqs, steps);
  /* downloads (16.2, 28.4) and downloads_if_range */
  const TfJson *dls = tf_json_get(v, "downloads");
  tf__download_on_pause = dl_on_pause;
  tf_http_set_transport(dl_transport, NULL);
  for (int i = 0; dls && i < dls->n; ++i) {
    const TfJson *k = &dls->kids[i];
    memset(&g_dl, 0, sizeof g_dl);
    g_dl.c = k;
    g_dl.size = (long long)N(k, "size");
    g_dl.data = (unsigned char *)malloc((size_t)g_dl.size);
    for (long long b = 0; b < g_dl.size; ++b) g_dl.data[b] = (unsigned char)(b * 7 + 3);
    uint8_t h[32];
    char sha[65], dir[1100], to[1200];
    tf_sha256(g_dl.data, (size_t)g_dl.size, h);
    tf_hex_encode(h, 32, sha);
    snprintf(dir, sizeof dir, "%s/download%02d", work, i);
    rmrf(dir);
    tf_mkdirs(dir);
    snprintf(to, sizeof to, "%s/file.bin", dir);
    tf__ep_now_override = 1000;
    TfEndpoints *e = tf__ep_new(NULL, NULL);
    tf__ep_settle(e, TF_EP_MODELS, S(k, "p_before"), 0);
    const int rc = tf__download_file_test(e, "/models/Quran-Lab/tajweedformer-v4/4b4b/sdk/v4.0/", "file.bin", g_dl.size, sha, to);
    const int code = tf_last_error_code();
    const TfJson *rq = tf_json_get(k, "requests");
    int same = rq && rq->n == g_dl.i;
    for (int x = 0; same && x < rq->n; ++x) {
      const TfJson *w = &rq->kids[x];
      const char *range = tf_json_str(tf_json_get(w, "range")), *ifr = tf_json_str(tf_json_get(w, "if_range"));
      char want_range[40] = "";
      if (range) snprintf(want_range, sizeof want_range, "%s", range);
      char got_range[40] = "";
      if (g_dl.range[x] > 0) snprintf(got_range, sizeof got_range, "bytes=%lld-", g_dl.range[x]);
      /* downloads_if_range: no If-Range where the case gives null; the case's value, or none, where it gives one */
      const int if_ok = ifr ? (!g_dl.if_range[x][0] || !strcmp(g_dl.if_range[x], ifr)) : !g_dl.if_range[x][0];
      same = !strcmp(g_dl.host[x], S(w, "host")) && !strcmp(got_range, want_range) && if_ok && g_dl.pause[x] == (long long)N(w, "pause_before") * 1000;
      if (!same)
        fprintf(stderr, "  download %d request %d: host %s range %s if-range %s pause %lld\n", i, x, g_dl.host[x], got_range, g_dl.if_range[x], g_dl.pause[x]);
    }
    const char *end = S(k, "end");
    const int end_ok = !strcmp(end, "installed") ? rc == 0
                       : !strcmp(end, "refused")  ? rc != 0 && code == TF_ERR_SERVER_REJECTED
                                                  : rc != 0 && code == TF_ERR_DOWNLOAD_FAILED;
    char part[1300], p[TF_EP_HOST];
    snprintf(part, sizeof part, "%s.part", to);
    long long stored = ep_fsize(rc == 0 ? to : part);
    if (stored < 0) stored = 0;
    tf__ep_preferred(e, TF_EP_MODELS, p);
    CHECK(same && end_ok && stored == (long long)N(k, "stored") && !strcmp(p, S(k, "p_after")),
          "downloads: %s: %d requests (want %d), end %s (%s), stored %lld, P %s", S(k, "name"), g_dl.i, rq ? rq->n : 0, end, rc ? tf_error_name(code) : "ok",
          stored, p);
    tf__ep_free(e);
    free(g_dl.data);
    tf__clear_error();
  }
  tf_http_set_transport(NULL, NULL);
  tf__download_on_pause = NULL;
  tf__ep_now_override = 0;
  printf("  downloads: %d\n", dls ? dls->n : 0);
  /* standby_40.json, as the SDK reads a standby's answers (28.9 is the server's; nothing in it is for an SDK to
   * replay, so this is the SDK's side): every answer a standby sends instead of the API's (503 service_unavailable) is
   * unreachable, the next host; every refusal it passes on is a refusal, final; a 429 is a slow down */
  {
    const TfJson *ps = tf_json_get(sb, "passes");
    int np = 0, nr = 0;
    for (int i = 0; ps && i < ps->n; ++i, ++np) {
      const TfJson *k = &ps->kids[i], *up = tf_json_get(k, "upstream");
      const int pass = !strcmp(S(k, "standby"), "pass");
      TfHttpResponse r;
      memset(&r, 0, sizeof r);
      TfBuf o;
      tf_buf_init(&o);
      if (pass) {
        r.status = (int)N(up, "status");
        if (tf_json_get(up, "json")) tf_json_write(&o, tf_json_get(up, "json"));
      } else {
        r.status = 503;
        tf_buf_puts(&o, "{\"error\":\"service_unavailable\",\"message\":\"this host does not serve that now\"}");
        r.retry_after = 30;
      }
      r.body = o.p ? o.p : dupz("", 0);
      r.body_n = o.p ? o.n : 0;
      TfEpReq er;
      memset(&er, 0, sizeof er);
      const int cls = tf__ep_class(&er, 0, &r);
      const int st = r.status;
      const int want = !pass                        ? TF_EP_UNREACHABLE
                       : st == 429                  ? TF_EP_SLOW
                       : st >= 200 && st < 300      ? TF_EP_TAKEN
                       : st >= 400 && st < 500      ? TF_EP_REFUSED
                                                    : TF_EP_UNREACHABLE;
      CHECK(cls == want, "standby passes: %s (%s %d): class %d, want %d", S(k, "name"), S(k, "standby"), st, cls, want);
      tf_http_response_free(&r);
    }
    const TfJson *rq = tf_json_get(sb, "requests");
    for (int i = 0; rq && i < rq->n; ++i) {
      const TfJson *k = &rq->kids[i], *ex = tf_json_get(k, "expect");
      const int st = (int)N(ex, "status");
      if (!strcmp(S(k, "role"), "web_frame") || (st != 503 && st != 429)) continue; /* health and preflights: no SDK request */
      nr++;
      TfHttpResponse r;
      memset(&r, 0, sizeof r);
      TfBuf o;
      tf_buf_init(&o);
      tf_buf_puts(&o, "{\"error\":");
      tf_buf_json_str(&o, S(ex, "error"));
      tf_buf_puts(&o, "}");
      r.status = st;
      r.body = o.p;
      r.body_n = o.n;
      const int cls = tf__ep_class(NULL, 0, &r);
      CHECK(cls == (st == 429 ? TF_EP_SLOW : TF_EP_UNREACHABLE), "standby requests: %s (%s %s %d): class %d", S(k, "name"), S(k, "role"), S(k, "path"), st, cls);
      tf_http_response_free(&r);
    }
    printf("  standby_40: %d passes and %d requests read as 28.3 says\n", np, nr);
  }
  /* attempts (28.5): lease attempts through the Client and the Core, each request through the api order; the answers
   * of each request's hosts given in the order the vector asks them, on the hosts it names */
  {
    TfJson *fx = flow_setup(fixture);
    const TfJson *at = tf_json_get(v, "attempts");
    if (!fx || !at) {
      CHECK(0, "attempts: the fixture and the vector");
    } else {
      const TfJson *l1 = tf_json_get(fx, "lease");
      const long long iat = N(l1, "iat"), exp = N(l1, "exp"), b0 = 3600000, t0 = exp + 7200;
      const char *mt = S(fx, "manifest"), *build = S(fx, "build");
      TfJson *mj = tf_json_parse(mt, strlen(mt), NULL);
      tf__lease_wait_s = 1;
      for (int i = 0; i < at->n; ++i) {
        const TfJson *k = &at->kids[i], *reqs = tf_json_get(k, "requests");
        /* the script: each request's answers in the order its hosts are asked */
        TfBuf sc;
        tf_buf_init(&sc);
        tf_buf_puts(&sc, "[");
        int want_hosts[32], nw = 0;
        for (int x = 0; reqs && x < reqs->n; ++x) {
          const TfJson *rqx = &reqs->kids[x], *asked = tf_json_get(rqx, "asked");
          for (int y = 0; asked && y < asked->n; ++y) {
            const char *h = tf_json_str(&asked->kids[y]);
            const TfJson *a = tf_json_get(tf_json_get(rqx, "answers"), h);
            TfBuf one;
            tf_buf_init(&one);
            tf_json_write(&one, a);
            /* the answer with its route: {"route": ..., <the answer's members>} */
            tf_buf_printf(&sc, "%s{\"route\":\"%s\"%s%s", nw ? "," : "", S(rqx, "route"), one.n > 2 ? "," : "", one.n > 2 ? one.p + 1 : "}");
            tf_buf_free(&one);
            int hi = -1;
            for (int z = 0; z < N_API_HOSTS; ++z)
              if (!strcmp(h, k_api_hosts[z])) hi = z;
            if (nw < 32) want_hosts[nw++] = hi;
          }
        }
        tf_buf_puts(&sc, "]");
        TfJson *script = tf_json_parse(sc.p, sc.n, NULL);
        tf_buf_free(&sc);
        char w[1100];
        snprintf(w, sizeof w, "%s/attempt%02d", work, i);
        grace_install(w, iat, b0);
        tf__plat_test_boot_ms = b0 + (t0 - iat) * 1000;
        TfClient *c = flow_client_in(w);
        fake_reset(LEASE_OK);
        g_f.script = script;
        const long long before = (long long)time(NULL);
        const int rc = c ? tf__pm_lease(c, tf__pm(c), mt, strlen(mt), mj, build) : -1;
        qlc_lease_status ls;
        tf__pm_status(c ? tf__pm(c) : NULL, &ls);
        const int grace = c ? tf__pm_grace(tf__pm(c), NULL) : 0;
        const char *want = S(k, "outcome");
        int good;
        if (!strcmp(want, "lease")) good = rc == 0 && ls.valid && !grace;
        else if (!strcmp(want, "stop")) good = rc == TF_ERR_INVALID_KEY && !ls.has_lease && !grace;
        else if (!strcmp(want, "refused")) good = rc != 0 && !grace;
        else good = rc == 0 && grace && !ls.valid; /* unreachable */
        int hosts_ok = script && g_f.at == script->n && g_f.at == nw;
        for (int x = 0; hosts_ok && x < nw; ++x) hosts_ok = g_f.script_host[x] == want_hosts[x];
        const TfJson *ra = tf_json_get(k, "retry_after");
        int ra_ok = 1;
        if (ra && ra->type == TJ_NUM && c) {
          const long long next = tf__pm_next_try(tf__pm(c));
          ra_ok = next >= before + (long long)ra->num && next <= (long long)time(NULL) + (long long)ra->num;
        }
        CHECK(good && hosts_ok && ra_ok, "attempts: %s: want %s, got %s (valid %d, grace %d); answers given %d of %d on the hosts named %d; Retry-After %d",
              S(k, "name"), want, rc ? tf_error_name(rc) : "ok", ls.valid, grace, g_f.at, script ? script->n : 0, hosts_ok, ra_ok);
        g_f.script = NULL;
        tf_json_free(script);
        if (c) tf_client_destroy(c);
      }
      tf__lease_wait_s = 10;
      printf("  attempts: %d\n", at->n);
      tf_json_free(mj);
    }
    tf_http_set_transport(NULL, NULL);
    tf_json_free(fx);
  }
  tf_json_free(v);
  tf_json_free(sb);
  return done("endpoints");
}

/* ------------------------------------------------------------------ section 28 on the real HTTP code */
enum { LB_REFUSE = 0, LB_FAKE, LB_FIXED, LB_FILE, LB_SILENT };
typedef struct {
  const char *host; /* the name it stands in for */
  int mode;
  TfLbServer *srv;
  int port, n;
  int status, retry_after; /* LB_FIXED */
  const char *body;
  const unsigned char *data; /* LB_FILE */
  long long size, cut;
  const char *etag;
  char range[64], if_range[96];
  char origin[64];
} LbHost;
static LbHost g_lb[8];
static int g_nlb;
static const char *lb_origin(const char *host) {
  for (int i = 0; i < g_nlb; ++i)
    if (!strcmp(g_lb[i].host, host)) return g_lb[i].origin;
  return "http://127.0.0.1:1"; /* never reached: every host of the lists is set */
}
static TfMutex g_lb_fake_lock;
static int g_lb_fake_lock_made;
static void lb_handle(void *user, const TfLbRequest *q, TfLbAnswer *a) {
  LbHost *h = (LbHost *)user;
  h->n++;
  tf_lb_header(q, "range", h->range, sizeof h->range);
  tf_lb_header(q, "if-range", h->if_range, sizeof h->if_range);
  if (h->mode == LB_FIXED) {
    a->status = h->status;
    a->body = dupz(h->body, strlen(h->body));
    a->body_n = strlen(h->body);
    snprintf(a->headers, sizeof a->headers, "content-type: application/json\r\n");
    if (h->retry_after) snprintf(a->headers + strlen(a->headers), sizeof a->headers - strlen(a->headers), "retry-after: %d\r\n", h->retry_after);
    return;
  }
  if (h->mode == LB_FILE) {
    const long long from = !strncmp(h->range, "bytes=", 6) ? atoll(h->range + 6) : 0;
    a->status = from > 0 ? 206 : 200;
    a->body_n = (size_t)(h->size - from);
    a->body = dupz((const char *)h->data + from, a->body_n);
    if (from > 0) snprintf(a->headers, sizeof a->headers, "content-range: bytes %lld-%lld/%lld\r\netag: %s\r\n", from, h->size - 1, h->size, h->etag);
    else snprintf(a->headers, sizeof a->headers, "etag: %s\r\n", h->etag);
    a->cut = h->cut;
    h->cut = -1; /* once */
    return;
  }
  /* LB_FAKE: the flow's stand-in, as if this request had come to `host`; one request at a time across the servers
   * (each runs its own thread, and the stand-in's state is one) */
  tf_mutex_lock(&g_lb_fake_lock);
  char url[2300], lines[24][1024];
  const char *hdr[25];
  int nh = 0;
  snprintf(url, sizeof url, "https://%s%s", h->host, q->path);
  for (const char *p = q->headers; *p && nh < 24;) {
    const char *e = strstr(p, "\r\n");
    if (!e) break;
    snprintf(lines[nh], sizeof lines[0], "%.*s", (int)(e - p), p);
    hdr[nh] = lines[nh];
    nh++;
    p = e + 2;
  }
  hdr[nh] = NULL;
  TfHttpRequest rq;
  memset(&rq, 0, sizeof rq);
  rq.method = q->method;
  rq.url = url;
  rq.headers = hdr;
  rq.body = q->body;
  rq.body_n = q->body_n;
  TfHttpResponse r;
  memset(&r, 0, sizeof r);
  r.content_length = r.range_start = r.range_total = -1;
  const int frc = fake(&rq, &r, NULL);
  tf_mutex_unlock(&g_lb_fake_lock);
  if (frc) {
    a->close_now = 1;
    tf_http_response_free(&r);
    return;
  }
  a->status = r.status;
  char *hp = a->headers;
  size_t left = sizeof a->headers;
#define LB_ADD(...)                                  \
  do {                                               \
    const int k_ = snprintf(hp, left, __VA_ARGS__); \
    if (k_ > 0 && (size_t)k_ < left) {               \
      hp += k_;                                      \
      left -= (size_t)k_;                            \
    }                                                \
  } while (0)
  if (r.signature[0]) LB_ADD("x-qlab-signature: %s\r\n", r.signature);
  if (r.key_id[0]) LB_ADD("x-qlab-key-id: %s\r\n", r.key_id);
  if (r.etag[0]) LB_ADD("etag: %s\r\n", r.etag);
  if (r.retry_after) LB_ADD("retry-after: %d\r\n", r.retry_after);
  const long long from = !strncmp(h->range, "bytes=", 6) ? atoll(h->range + 6) : 0;
  if (from > 0 && r.status == 200 && r.body && (size_t)from < r.body_n) {
    LB_ADD("content-range: bytes %lld-%lld/%lld\r\n", from, (long long)r.body_n - 1, (long long)r.body_n);
    a->status = 206;
    a->body_n = r.body_n - (size_t)from;
    a->body = dupz(r.body + from, a->body_n);
  } else {
    a->body = r.body;
    a->body_n = r.body_n;
    r.body = NULL;
  }
#undef LB_ADD
  tf_http_response_free(&r);
}
static void lb_setup(const char *const *hosts, const int *modes, int n) {
  for (int i = 0; i < g_nlb; ++i) tf_lb_stop(g_lb[i].srv);
  if (!g_lb_fake_lock_made) { /* made on this thread before any server runs */
    tf_mutex_init(&g_lb_fake_lock);
    g_lb_fake_lock_made = 1;
  }
  memset(g_lb, 0, sizeof g_lb);
  g_nlb = n;
  for (int i = 0; i < n; ++i) {
    LbHost *h = &g_lb[i];
    h->host = hosts[i];
    h->mode = modes[i];
    h->cut = -1;
    if (h->mode == LB_REFUSE) {
      h->port = tf_lb_closed_port();
    } else if (h->mode == LB_SILENT) {
      h->srv = tf_lb_start_silent();
      h->port = tf_lb_port(h->srv);
    } else {
      h->srv = tf_lb_start(lb_handle, h);
      h->port = tf_lb_port(h->srv);
    }
    snprintf(h->origin, sizeof h->origin, "http://127.0.0.1:%d", h->port);
  }
  tf__ep_test_origin = lb_origin;
}
static LbHost *lb_of(const char *host) {
  for (int i = 0; i < g_nlb; ++i)
    if (!strcmp(g_lb[i].host, host)) return &g_lb[i];
  return NULL;
}
static void lb_fixed(const char *host, int status, const char *body, int retry_after) {
  LbHost *h = lb_of(host);
  h->status = status;
  h->body = body;
  h->retry_after = retry_after;
}
/* one API request through the api order on the real HTTP code */
static int lb_request(TfEndpoints *e, TfEpOut *out, TfHttpResponse *r) {
  TfHttpRequest q;
  memset(&q, 0, sizeof q);
  q.method = "GET";
  q.timeout_s = 30;
  TfEpReq er;
  memset(&er, 0, sizeof er);
  er.role = TF_EP_API;
  er.path = "/v1/health";
  memset(r, 0, sizeof *r);
  const int rc = tf__ep_http(e, &er, &q, r, out);
  return rc;
}
static int t_loopback(const char *fixture, const char *work) {
#ifndef _WIN32
  /* the stand-ins are on 127.0.0.1: no proxy of the environment may stand between */
  unsetenv("http_proxy");
  unsetenv("HTTP_PROXY");
  unsetenv("all_proxy");
  unsetenv("ALL_PROXY");
#endif
  static const char *const api[] = {"api.quranlab.ai", "api-b.tajweedformer.com", "api-g.tajweedformer.com", "api-a.tajweedformer.com",
                                    "cdn.quranlab.ai", "cdn-b.tajweedformer.com", "cdn-g.tajweedformer.com", "cdn-a.tajweedformer.com"};
  /* 1. a new install whose first hosts refuse every connection: config, the token, attest with the install keys, the
   * challenge, the lease, the manifest and every file from the standby hosts (28.8 checks 1, 4 and 6) */
  {
    TfJson *fx = flow_setup(fixture);
    if (!fx) return 2;
    tf_http_set_transport(NULL, NULL); /* the real HTTP code */
    static const int modes[] = {LB_REFUSE, LB_FAKE, LB_REFUSE, LB_REFUSE, LB_REFUSE, LB_FAKE, LB_REFUSE, LB_REFUSE};
    lb_setup(api, modes, 8);
    fake_reset(LEASE_OK);
    char w[1100];
    snprintf(w, sizeof w, "%s/new_install", work);
    const double t0 = tf__now_s();
    TfClient *c = flow_client(w);
    TfModel *m = c ? tf_client_model(c, NULL) : NULL;
    const int code = tf_last_error_code();
    const char *dj = c ? tf_client_diagnose_json(c) : NULL;
    const char *ep = dj ? strstr(dj, "\"endpoints\"") : NULL;
    /* the vector build's vocabulary is refused after its install (as in the flow test): the whole way was walked */
    CHECK(!m && code == TF_ERR_MODEL_INCOMPATIBLE && g_f.leases == 1 && g_f.attest_keys == 1 && lb_of("cdn-b.tajweedformer.com")->n > 2,
          "loopback 1: a new install on the standby hosts (%s: %s; leases %d, attests %d, standby api %d, standby cdn %d requests, %.1f s)",
          tf_error_name(code), tf_last_error() ? tf_last_error() : "", g_f.leases, g_f.attest_keys, lb_of("api-b.tajweedformer.com")->n,
          lb_of("cdn-b.tajweedformer.com")->n, tf__now_s() - t0);
    CHECK(ep && strstr(ep, "\"api\":\"api-b.tajweedformer.com\"") && strstr(ep, "\"models\":\"cdn-b.tajweedformer.com\"") &&
              strstr(ep, "\"from\":\"cdn.quranlab.ai\"") && strstr(ep, "\"reason\":\"network\""),
          "loopback 1: diagnose's endpoints: %s", ep ? ep : "(none)");
    printf("  loopback 1: a new install with the first hosts refusing every connection: standby api %d and cdn %d requests, %.1f s\n",
           lb_of("api-b.tajweedformer.com")->n, lb_of("cdn-b.tajweedformer.com")->n, tf__now_s() - t0);
    if (m) tf_model_close(m);
    if (c) tf_client_destroy(c); /* a send point: the reports go to the standby api host */
    /* 28.7, 24.1 item 2: the silent failover reports, one for the first api request and one for the manifest: code
     * none, the first host's reason network, the role, fallback true, no host name */
    int api_fo = 0, models_fo = 0, named = 0;
    for (int i = 0; i < g_f.n_err; ++i) {
      const char *bd = g_f.err_bodies[i];
      named |= strstr(bd, "quranlab.ai") || strstr(bd, "tajweedformer.com");
      if (!strstr(bd, "\"code\":\"none\"") || !strstr(bd, "\"reason\":\"failover\"") || !strstr(bd, "\"error_code\":\"network\"") ||
          !strstr(bd, "\"fallback\":true"))
        continue;
      api_fo += strstr(bd, "\"host_role\":\"api\"") != NULL;
      models_fo += strstr(bd, "\"stage\":\"manifest\"") && strstr(bd, "\"host_role\":\"models\"");
    }
    CHECK(api_fo >= 1 && models_fo == 1 && !named, "loopback 1: the silent failover reports (api %d, manifest %d, a host named %d; %d reports)", api_fo,
          models_fo, named, g_f.n_err);
    tf_json_free(fx);
    tf_http_set_transport(NULL, NULL);
  }
  /* 2. request by request: a 503, a refusal on either host, a 429, the probe back (28.8 checks 1, 2 and 5) */
  {
    static const int modes[] = {LB_FIXED, LB_FIXED, LB_FIXED, LB_FIXED};
    lb_setup(api, modes, 4);
    static const char *const ok = "{\"ok\":true}", *const down = "{\"error\":\"service_unavailable\"}";
    TfEpOut out;
    TfHttpResponse r;
    tf__ep_now_override = 1000;
    TfEndpoints *e = tf__ep_new(NULL, NULL);
    char p[TF_EP_HOST];
    lb_fixed(api[0], 503, down, 0);
    lb_fixed(api[1], 200, ok, 0);
    lb_fixed(api[2], 200, ok, 0);
    lb_fixed(api[3], 200, ok, 0);
    int rc = lb_request(e, &out, &r);
    tf__ep_preferred(e, TF_EP_API, p);
    CHECK(!rc && r.status == 200 && out.cls == TF_EP_TAKEN && out.fallback && !strcmp(out.by, api[1]) && !strcmp(p, api[1]) && g_lb[0].n == 1 && g_lb[1].n == 1,
          "loopback 2: a 503 on the first host: the standby answers and is P (%d, by %s, P %s)", r.status, out.by, p);
    tf_http_response_free(&r);
    rc = lb_request(e, &out, &r);
    CHECK(!rc && g_lb[0].n == 1 && g_lb[1].n == 2 && !out.fallback, "loopback 2: the next request goes to the standby first (first host %d, standby %d)",
          g_lb[0].n, g_lb[1].n);
    tf_http_response_free(&r);
    /* the probe back at 30 minutes, the first host up again */
    lb_fixed(api[0], 200, ok, 0);
    tf__ep_now_override = 1000 + 1800;
    rc = lb_request(e, &out, &r);
    tf__ep_preferred(e, TF_EP_API, p);
    CHECK(!rc && g_lb[0].n == 2 && g_lb[1].n == 2 && !strcmp(p, api[0]), "loopback 2: 30 minutes later the first host is asked first and is P again (%s)", p);
    tf_http_response_free(&r);
    tf__ep_free(e);
    /* a refusal on the first host is the answer: no other host is asked */
    e = tf__ep_new(NULL, NULL);
    for (int i = 0; i < 4; ++i) g_lb[i].n = 0;
    lb_fixed(api[0], 401, "{\"error\":\"invalid_key\",\"reason\":\"key_revoked\"}", 0);
    rc = lb_request(e, &out, &r);
    CHECK(!rc && r.status == 401 && out.cls == TF_EP_REFUSED && g_lb[0].n == 1 && !g_lb[1].n && !g_lb[2].n && !g_lb[3].n,
          "loopback 2: a refusal on the first host is final (standby asked %d times)", g_lb[1].n);
    tf_http_response_free(&r);
    tf__ep_free(e);
    /* the first host refuses the connection, the standby refuses the request: the third host is never asked */
    static const int modes2[] = {LB_REFUSE, LB_FIXED, LB_FIXED, LB_FIXED};
    lb_setup(api, modes2, 4);
    lb_fixed(api[1], 403, "{\"error\":\"app_blocked\",\"reason\":\"app_blocked\"}", 0);
    lb_fixed(api[2], 200, ok, 0);
    lb_fixed(api[3], 200, ok, 0);
    e = tf__ep_new(NULL, NULL);
    rc = lb_request(e, &out, &r);
    tf__ep_preferred(e, TF_EP_API, p);
    CHECK(!rc && r.status == 403 && out.cls == TF_EP_REFUSED && out.fallback && !strcmp(out.by, api[1]) && g_lb[1].n == 1 && !g_lb[2].n && !g_lb[3].n &&
              !strcmp(p, api[1]) && !strcmp(out.first_reason, "network"),
          "loopback 2: a refused connection, then a refusal on the standby: final, the third never asked (%d, %s, third %d)", r.status, out.first_reason,
          g_lb[2].n);
    tf_http_response_free(&r);
    tf__ep_free(e);
    /* a 429 on the first host: no other host is asked, P stays, its Retry-After read */
    static const int modes3[] = {LB_FIXED, LB_FIXED, LB_FIXED, LB_FIXED};
    lb_setup(api, modes3, 4);
    lb_fixed(api[0], 429, "{\"error\":\"rate_limited\"}", 7);
    lb_fixed(api[1], 200, ok, 0);
    lb_fixed(api[2], 200, ok, 0);
    lb_fixed(api[3], 200, ok, 0);
    e = tf__ep_new(NULL, NULL);
    rc = lb_request(e, &out, &r);
    tf__ep_preferred(e, TF_EP_API, p);
    CHECK(!rc && r.status == 429 && r.retry_after == 7 && out.cls == TF_EP_SLOW && !out.fallback && g_lb[0].n == 1 && !g_lb[1].n && !strcmp(p, api[0]),
          "loopback 2: a 429 never moves to another host (standby %d, Retry-After %d)", g_lb[1].n, r.retry_after);
    tf_http_response_free(&r);
    tf__ep_free(e);
    tf__ep_now_override = 0;
  }
  /* 3. a file whose first host resets the connection mid-file: resumed on the standby with Range and no If-Range,
   * installed at its size and SHA-256 (28.8 check 4) */
  {
    static const int modes[] = {LB_FILE, LB_FILE, LB_FILE, LB_FILE};
    lb_setup(api + 4, modes, 4);
    static unsigned char data[200000];
    for (size_t i = 0; i < sizeof data; ++i) data[i] = (unsigned char)(i * 13 + 5);
    static const char *const etags[] = {"\"c-1\"", "\"b-1\"", "\"g-1\"", "\"a-1\""};
    for (int i = 0; i < 4; ++i) {
      g_lb[i].data = data;
      g_lb[i].size = sizeof data;
      g_lb[i].etag = etags[i];
    }
    g_lb[0].cut = 70000;
    uint8_t h[32];
    char sha[65], dir[1100], to[1200], p[TF_EP_HOST];
    tf_sha256(data, sizeof data, h);
    tf_hex_encode(h, 32, sha);
    snprintf(dir, sizeof dir, "%s/file", work);
    rmrf(dir);
    tf_mkdirs(dir);
    snprintf(to, sizeof to, "%s/file.bin", dir);
    tf__download_on_pause = dl_on_pause;
    TfEndpoints *e = tf__ep_new(NULL, NULL);
    const int rc = tf__download_file_test(e, "/models/Quran-Lab/tajweedformer-v4/4b4b/sdk/v4.0/", "file.bin", sizeof data, sha, to);
    tf__ep_preferred(e, TF_EP_MODELS, p);
    char *dg = tf__ep_diag_json(e);
    CHECK(!rc && ep_fsize(to) == (long long)sizeof data && g_lb[0].n == 1 && g_lb[1].n == 1 && !strncmp(g_lb[1].range, "bytes=", 6) &&
              atoll(g_lb[1].range + 6) > 0 && !g_lb[1].if_range[0] && !strcmp(p, api[5]),
          "loopback 3: reset at 70000 bytes, resumed on the standby with %s and no If-Range (%s), installed (%s, P %s)", g_lb[1].range,
          g_lb[1].if_range[0] ? g_lb[1].if_range : "none", rc ? tf_error_name(tf_last_error_code()) : "ok", p);
    CHECK(dg && strstr(dg, "\"lastFailover\":{\"role\":\"models\",\"from\":\"cdn.quranlab.ai\",\"to\":\"cdn-b.tajweedformer.com\""),
          "loopback 3: lastFailover names the file's failover: %s", dg ? dg : "(none)");
    printf("  loopback 3: the first host reset after 70000 of %d bytes; the standby asked with %s, If-Range %s\n", (int)sizeof data, g_lb[1].range,
           g_lb[1].if_range[0] ? g_lb[1].if_range : "none");
    free(dg);
    tf__ep_free(e);
    tf__download_on_pause = NULL;
  }
  /* 4. a role of one host that is down, and every host down: lastFailover records it with no host that answered (28.7) */
  {
    static const int modes[] = {LB_REFUSE, LB_REFUSE, LB_REFUSE, LB_REFUSE};
    lb_setup(api, modes, 4);
    TfEndpoints *e = tf__ep_new(NULL, NULL);
    char one[1][TF_EP_HOST] = {"api.quranlab.ai"};
    tf__ep_set_lists(e, TF_EP_API, one, 1);
    TfEpOut out;
    TfHttpResponse r;
    const int rc = lb_request(e, &out, &r);
    char *dg = tf__ep_diag_json(e);
    CHECK(rc && out.cls == TF_EP_UNREACHABLE && out.asked == 1 && dg &&
              strstr(dg, "\"lastFailover\":{\"role\":\"api\",\"from\":\"api.quranlab.ai\",\"to\":null,\"reason\":\"network\",\"status\":null"),
          "loopback 4: a one-host list whose host is down is recorded, to null: %s", dg ? dg : "(none)");
    free(dg);
    tf_http_response_free(&r);
    tf__clear_error();
    tf__ep_free(e);
  }
  /* 5. a first host that accepts and never answers, in plain HTTP and in TLS (its handshake never completes): 10 s to
   * the answer's headers (TF_EP_HEAD_MS; 28.3 item 6, where the connect cannot be bounded alone), then the next host
   * (4.0.0 waited the request's whole 30 s) */
  for (int tls = 0; tls < 2; ++tls) {
    static const int modes[] = {LB_SILENT, LB_FIXED, LB_FIXED, LB_FIXED};
    lb_setup(api, modes, 4);
    if (tls) snprintf(g_lb[0].origin, sizeof g_lb[0].origin, "https://127.0.0.1:%d", g_lb[0].port);
    lb_fixed(api[1], 200, "{\"ok\":true}", 0);
    lb_fixed(api[2], 200, "{\"ok\":true}", 0);
    lb_fixed(api[3], 200, "{\"ok\":true}", 0);
    TfEndpoints *e = tf__ep_new(NULL, NULL);
    TfEpOut out;
    TfHttpResponse r;
    const double t0 = tf__now_s();
    const int rc = lb_request(e, &out, &r);
    const double dt = tf__now_s() - t0;
    char p[TF_EP_HOST];
    tf__ep_preferred(e, TF_EP_API, p);
    /* libcurl bounds TLS inside its 5 s connect, WinHTTP only through the 10 s to the headers */
    const double lo = tls ? 4.5 : 9.5;
    CHECK(!rc && r.status == 200 && out.fallback && !strcmp(out.by, api[1]) && !strcmp(p, api[1]) && g_lb[1].n == 1 && dt >= lo && dt < 13.0,
          "loopback 5: a first host that accepts and never answers (%s): given up after %.1f s, the next host answers (%d, by %s)",
          tls ? "TLS" : "plain HTTP", dt, r.status, out.by);
    printf("  loopback 5: a silent first host (%s): the next host asked after %.1f s\n", tls ? "TLS handshake" : "plain HTTP", dt);
    tf_http_response_free(&r);
    tf__ep_free(e);
  }
  for (int i = 0; i < g_nlb; ++i) tf_lb_stop(g_lb[i].srv);
  g_nlb = 0;
  tf__ep_test_origin = NULL;
  return done("loopback");
}
