/* Included by tf_tests.c: SDK 3.1.3, the model download through the library's REAL HTTP code.
 *
 *   net <url> <work dir>    <url> is the stand-in model host of test/net_stand_in.py, which starts it, runs this
 *                           and stops it (ctest: tf_net)
 *
 * Every other test replaces the transport (tf_http_set_transport), so none of them runs WinHTTP or libcurl. The
 * stall limit, the resume inside the call, the kept connection and the protocol record live in that code, and
 * these tests run it against a server on 127.0.0.1 that can stall, end a connection, refuse, answer 503 and
 * ignore a Range, and that counts the connections and requests it got.
 *
 * What this cannot show: the server speaks plain HTTP/1.1. HTTP/2 and HTTP/3 need TLS, so the protocol options
 * (WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, CURLOPT_HTTP_VERSION) are set here and never negotiated, and the record
 * in the diagnosis is checked for "http/1.1" only. A real mobile link is not shown either: the stall and the
 * broken connection are made by the server.
 *
 * The model is a stand-in (five files that are not a model, a manifest signed with a throwaway key): it is
 * downloaded and verified, never opened. */

static struct {
  char url[128];   /* http://127.0.0.1:<port> */
  char base[160];  /* <url>/models: the model base of a download without a Client */
  TfJson *setup;   /* GET /_ctl/setup */
  const char *key_hex, *build, *big;
  char big_etag[40];
  long long big_size, total; /* bytes of the large file, and of the five files */
  int api_requests;
} g_net;

/* one request to the server's control side, through the library's HTTP code; the parsed answer or NULL */
static TfJson *net_ctl(const char *method, const char *path, const char *body) {
  char url[256];
  snprintf(url, sizeof url, "%s%s", g_net.url, path);
  TfHttpRequest q;
  memset(&q, 0, sizeof q);
  q.method = method;
  q.url = url;
  q.body = body;
  q.body_n = body ? strlen(body) : 0;
  q.timeout_s = 20;
  TfHttpResponse r;
  const int rc = tf__http_native(&q, &r); /* whatever transport a part of the test has set */
  TfJson *j = !rc && r.status == 200 && r.body ? tf_json_parse(r.body, r.body_n, NULL) : NULL;
  tf_http_response_free(&r);
  CHECK(j != NULL, "the stand-in server did not answer %s %s", method, path);
  return j;
}
static void net_reset(void) { tf_json_free(net_ctl("POST", "/_ctl/reset", "{}")); }
/* the next requests for `file` are answered by these steps (test/net_stand_in.py names them) */
static void net_plan(const char *file, const char *steps) {
  char body[1024];
  snprintf(body, sizeof body, "{\"file\":\"%s\",\"steps\":%s}", file, steps);
  tf_json_free(net_ctl("POST", "/_ctl/plan", body));
}

/* the model requests the server got for one file (NULL: for every file), in order of arrival */
#define NET_MAX 24
typedef struct {
  int n;                    /* requests for the file */
  int all, connections;     /* model requests for every file, and the connections that carried them */
  int no_key, no_token;     /* requests without x-qlab-key, and without a bearer token */
  /* requests that FOLLOWED a redirect: they arrived at a path under /moved/. Not counted in n */
  int moved, moved_key, moved_token, moved_localhost; /* how many; with x-qlab-key; with a bearer token; made to the name localhost */
  long long moved_body;     /* the bytes of request bodies that arrived there */
  char moved_names[400];    /* the method and the NAMES of the headers of the last of them (the server keeps no value) */
  int status[NET_MAX];
  long long from[NET_MAX];  /* the N of "Range: bytes=N-", -1 without a Range */
  int cond[NET_MAX];        /* If-Range: 0 none, 1 the large file's ETag, 2 another value */
  int conn[NET_MAX];
  long long sent[NET_MAX], at_ms[NET_MAX];
} NetReqs;
static void net_requests(const char *file, NetReqs *o) {
  memset(o, 0, sizeof *o);
  TfJson *st = net_ctl("GET", "/_ctl/stats", NULL);
  const TfJson *rs = tf_json_get(st, "requests");
  o->connections = st && tf_json_get(st, "connections") ? (int)tf_json_get(st, "connections")->num : -1;
  for (int i = 0; rs && i < rs->n; ++i) {
    const TfJson *q = &rs->kids[i];
    o->all++;
    const TfJson *key = tf_json_get(q, "key");
    const TfJson *token = tf_json_get(q, "token");
    o->no_key += !(key && key->type == TJ_BOOL && key->boolean);
    o->no_token += !(token && token->type == TJ_BOOL && token->boolean);
    const char *path = tf_json_str(tf_json_get(q, "path"));
    if (path && !strncmp(path, "/moved/", 7)) {
      const TfJson *lh = tf_json_get(q, "localhost"), *bd = tf_json_get(q, "body"), *hs = tf_json_get(q, "headers");
      o->moved++;
      o->moved_key += key && key->type == TJ_BOOL && key->boolean;
      o->moved_token += token && token->type == TJ_BOOL && token->boolean;
      o->moved_localhost += lh && lh->type == TJ_BOOL && lh->boolean;
      o->moved_body += bd && bd->type == TJ_NUM ? (long long)bd->num : 0;
      snprintf(o->moved_names, sizeof o->moved_names, "%s:", tf_json_str(tf_json_get(q, "method")) ? tf_json_str(tf_json_get(q, "method")) : "?");
      for (int h = 0; hs && h < hs->n; ++h) {
        const size_t at = strlen(o->moved_names);
        snprintf(o->moved_names + at, sizeof o->moved_names - at, " %s", tf_json_str(&hs->kids[h]) ? hs->kids[h].str : "?");
      }
      continue; /* not a request the library was asked to make */
    }
    if (file && strcmp(tf_json_str(tf_json_get(q, "file")), file)) continue;
    if (o->n == NET_MAX) continue;
    const char *range = tf_json_str(tf_json_get(q, "range")), *cond = tf_json_str(tf_json_get(q, "if_range"));
    o->status[o->n] = (int)tf_json_get(q, "status")->num;
    o->from[o->n] = range && !strncmp(range, "bytes=", 6) ? atoll(range + 6) : -1;
    o->cond[o->n] = !cond ? 0 : !strcmp(cond, g_net.big_etag) ? 1 : 2;
    o->conn[o->n] = (int)tf_json_get(q, "conn")->num;
    o->sent[o->n] = (long long)tf_json_get(q, "sent")->num;
    o->at_ms[o->n] = (long long)tf_json_get(q, "at_ms")->num;
    o->n++;
  }
  tf_json_free(st);
}

static long long net_size(const char *dir, const char *name) {
  char *p = tf_path_join(dir, name);
  FILE *f = p ? fopen(p, "rb") : NULL;
  long long n = -1;
  if (f) {
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
  }
  free(p);
  return n;
}
/* 1 when the five files in `dir` are the server's (size and SHA-256), the manifest is there and no partial
 * file or validator is left */
static int net_all_ok(const char *dir) {
  const TfJson *fs = tf_json_get(g_net.setup, "files");
  int ok = fs && fs->n == 5;
  for (int i = 0; ok && i < fs->n; ++i) {
    const char *name = tf_json_str(tf_json_get(&fs->kids[i], "path"));
    char *p = tf_path_join(dir, name), hex[65] = "", part[80], tag[80];
    uint8_t h[32];
    long long n = 0;
    if (!p || tf_sha256_file(p, h, &n)) ok = 0;
    else tf_hex_encode(h, 32, hex);
    ok = ok && n == (long long)tf_json_get(&fs->kids[i], "size")->num && !strcmp(hex, tf_json_str(tf_json_get(&fs->kids[i], "sha256")));
    snprintf(part, sizeof part, "%s.part", name);
    snprintf(tag, sizeof tag, "%s.part.etag", name);
    ok = ok && net_size(dir, part) < 0 && net_size(dir, tag) < 0;
    free(p);
  }
  return ok && net_size(dir, "manifest.json") > 0;
}

/* one download of the stand-in model into `dir`, without a Client (the key alone) */
static int net_download(const char *dir, long long *fetched, int *resumed, volatile int *cancel) {
  TfDownload d = tf__download_defaults();
  d.cdn_url = g_net.base;
  d.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  d.revision = g_net.build;
  d.dest = dir;
  d.trusted_key_hex = g_net.key_hex;
  d.want_version = "4.0.0";
  d.check_build = 1;
  d.fetched = fetched;
  d.resumed = resumed;
  d.cancel = cancel;
  return tf__download_model(&d);
}
typedef struct {
  const char *dir;
  volatile int cancel, done;
  int rc;
} NetRun;
static void net_thread(void *arg) {
  NetRun *r = (NetRun *)arg;
  r->rc = net_download(r->dir, NULL, NULL, &r->cancel);
  r->done = 1;
}
/* waits (at most 5 s) until the server has sent `sent` bytes of the first answer for the large file */
static void net_wait_first(long long sent) {
  for (int i = 0; i < 100; ++i) {
    NetReqs q;
    net_requests(g_net.big, &q);
    if (q.n >= 1 && q.sent[0] >= sent) return;
    tf__sleep_ms(50);
  }
}

/* the Client part: the API is answered here (a token, the signed config that names the server's build), and a
 * model request goes to the stand-in server through the real HTTP code. The signed config can only name an
 * https base, so the model host's URL is rewritten here. */
static void net_reply(TfHttpResponse *r, int status, const char *body) {
  r->status = status;
  r->body_n = strlen(body);
  r->body = (char *)malloc(r->body_n + 1);
  memcpy(r->body, body, r->body_n + 1);
}
static int net_stub(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  static const char cdn[] = "https://cdn.quranlab.ai/models/";
  const char *path = strstr(q->url, "/v1/");
  if (!strncmp(q->url, cdn, sizeof cdn - 1)) {
    char url[4096];
    snprintf(url, sizeof url, "%s/%s", g_net.base, q->url + sizeof cdn - 1);
    TfHttpRequest real = *q;
    real.url = url;
    return tf__http_native(&real, r);
  }
  if (!path) return tf__fail(TF__ERR_NETWORK, "no network (test)");
  g_net.api_requests++;
  if (!strncmp(path, "/v1/attest", 10)) {
    net_reply(r, 200, "{\"token\":\"qlt_v1.test1.sig\",\"expires_in\":86400,\"attested\":false}");
  } else if (!strncmp(path, "/v1/config", 10)) {
    net_reply(r, 200, tf_json_str(tf_json_get(g_net.setup, "config_body")));
    snprintf(r->signature, sizeof r->signature, "%s", tf_json_str(tf_json_get(g_net.setup, "config_signature")));
    snprintf(r->key_id, sizeof r->key_id, "%s", tf_json_str(tf_json_get(g_net.setup, "key_id")));
  } else if (!strncmp(path, "/v1/register", 12)) {
    net_reply(r, 200, "{\"ok\":true,\"app_id\":\"a\",\"store_status\":\"unknown\"}");
  } else {
    net_reply(r, 202, "{\"ok\":true,\"points\":1}");
  }
  return 0;
}
typedef struct {
  TfClient *c;
  int rc;
  volatile int done;
} NetPrefetch;
static void net_prefetch_thread(void *arg) {
  NetPrefetch *p = (NetPrefetch *)arg;
  p->rc = tf_client_prefetch(p->c, NULL);
  p->done = 1;
}
static TfClient *net_client(const char *dir) {
  tf_mkdirs(dir);
  TfClient *c = tf__client_create_with_key(&(TfClientOptions){.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa", .app_id = "org.example.Net", .app_version = "1.0", .storage = dir},
                                           g_net.key_hex);
  CHECK(c != NULL, "the client: %s", tf_last_error() ? tf_last_error() : "");
  return c;
}
/* the diagnosis' downloadProtocol: the string, "(null)" for a JSON null, "(absent)" when the report has no such key */
static void net_protocol(TfClient *c, char out[24]) {
  const char *text = tf_client_diagnose_json(c);
  TfJson *j = text ? tf_json_parse(text, strlen(text), NULL) : NULL;
  const TfJson *p = tf_json_get(j, "downloadProtocol");
  snprintf(out, 24, "%s", !p ? "(absent)" : p->type == TJ_NULL ? "(null)" : tf_json_str(p) ? tf_json_str(p) : "(not a string)");
  tf_json_free(j);
}

/* The library's own uploader, with the upload test's stand-in API (stub, tf_share_tests.inc) for everything but
 * ONE kind of request, which goes to the stand-in server through the real HTTP code: 1 the sharing start (POST
 * /v1/uploads, with the token and the recording's id), 2 the PUT of a segment to its signed URL. */
static int g_net_share;
static int net_share_transport(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  const char *path = strstr(q->url, "://");
  path = path ? strchr(path + 3, '/') : NULL;
  const char *m = q->method ? q->method : "GET";
  const int start = g_net_share == 1 && path && !strcmp(m, "POST") && !strcmp(path, "/v1/uploads");
  const int put = g_net_share == 2 && !strcmp(m, "PUT");
  if (start || put) {
    char url[256];
    snprintf(url, sizeof url, "%s%s", g_net.url, start ? "/v1/uploads" : "/put/segment");
    TfHttpRequest real = *q;
    real.url = url;
    return tf__http_native(&real, r);
  }
  return stub(q, r, u);
}
/* what arrived at the path a redirect named, for a failing check's line */
static const char *net_moved(const NetReqs *q, char out[640]) {
  if (!q->moved) snprintf(out, 640, "nothing arrived at the redirect's path");
  else
    snprintf(out, 640, "%d request(s) ARRIVED at the redirect's path (%d to the name localhost): %d with x-qlab-key, %d with a bearer token, %lld body bytes; the last was [%s]",
             q->moved, q->moved_localhost, q->moved_key, q->moved_token, q->moved_body, q->moved_names);
  return out;
}

/* Puts the first `n` bytes of the large file, as the download in `from_dir` left it, into `dir` as the partial
 * file of a download, with its validator beside it: what a call that broke after n bytes leaves. The SHA-256 of
 * those bytes goes to `sha` (hex). 0 when written. */
static int net_seed_part(const char *from_dir, const char *dir, long long n, char sha[65]) {
  char *src = tf_path_join(from_dir, "big.bin"), *pp = tf_path_join(dir, "big.bin.part"), *pe = tf_path_join(dir, "big.bin.part.etag");
  char *data = NULL;
  size_t len = 0;
  uint8_t h[32];
  tf_mkdirs(dir);
  int bad = !src || !pp || !pe || tf_read_file(src, &data, &len) || (long long)len < n;
  if (!bad) {
    tf_sha256(data, (size_t)n, h);
    tf_hex_encode(h, 32, sha);
    bad = tf_write_file(pp, data, (size_t)n) || tf_write_file(pe, g_net.big_etag, strlen(g_net.big_etag));
  }
  free(data);
  free(src);
  free(pp);
  free(pe);
  return bad;
}
/* the SHA-256 (hex) of a file in `dir`; "" when it is not there */
static void net_sha(const char *dir, const char *name, char sha[65]) {
  char *p = tf_path_join(dir, name);
  uint8_t h[32];
  long long n = 0;
  sha[0] = 0;
  if (p && !tf_sha256_file(p, h, &n)) tf_hex_encode(h, 32, sha);
  free(p);
}


#define NET_DONE(what)                                                         \
  do {                                                                         \
    printf("net: %s: %s\n", what, fails == before ? "ok" : "FAIL");            \
    fflush(stdout);                                                            \
    before = fails;                                                            \
  } while (0)

static int net_tests(const char *url, const char *work) {
  tf_mkdirs(work);
  snprintf(g_net.url, sizeof g_net.url, "%s", url);
  snprintf(g_net.base, sizeof g_net.base, "%s/models", url);
  g_net.setup = net_ctl("GET", "/_ctl/setup", NULL);
  if (!g_net.setup) return 2;
  g_net.key_hex = tf_json_str(tf_json_get(g_net.setup, "public_key_hex"));
  g_net.build = tf_json_str(tf_json_get(g_net.setup, "build"));
  g_net.big = tf_json_str(tf_json_get(g_net.setup, "big"));
  const TfJson *fs = tf_json_get(g_net.setup, "files");
  for (int i = 0; fs && i < fs->n; ++i) {
    const long long size = (long long)tf_json_get(&fs->kids[i], "size")->num;
    g_net.total += size;
    if (!strcmp(tf_json_str(tf_json_get(&fs->kids[i], "path")), g_net.big)) {
      g_net.big_size = size;
      snprintf(g_net.big_etag, sizeof g_net.big_etag, "%s", tf_json_str(tf_json_get(&fs->kids[i], "etag")));
    }
  }
  if (!g_net.key_hex || !g_net.build || !g_net.big || !g_net.big_size) return 2;
  /* A look by hand, not a test: TF_NET_PROBE_URL=https://host/path asks that URL twice over one kept connection,
   * as a model request is asked but without a key or a token, and prints the status and the protocol of each
   * answer: what a host with TLS negotiates, which the stand-in server cannot show. */
  const char *purl = getenv("TF_NET_PROBE_URL");
  if (purl && *purl) {
    TfHttpConn *k = tf__http_conn_open();
    const int opened0 = tf__http_opened;
    for (int i = 0; i < 2; ++i) {
      TfHttpRequest pq;
      memset(&pq, 0, sizeof pq);
      pq.method = "GET";
      pq.url = purl;
      pq.stall_ms = 30000;
      pq.conn = k;
      TfHttpResponse pr;
      const int prc = tf__http_native(&pq, &pr);
      printf("net probe: request %d: %s, status %d, protocol %s\n", i + 1, prc ? "no answer" : "answered", pr.status, pr.protocol[0] ? pr.protocol : "(none)");
      tf_http_response_free(&pr);
    }
    printf("net probe: %d session(s) or handle(s) opened for the two requests\n", tf__http_opened - opened0);
    tf__http_conn_close(k);
    return 0;
  }
  /* A measuring mode, not a test: TF_NET_PROBE_STALL_MS=500,1500,5000 runs one stalled download per limit and
   * prints how long the call took, which is how long this system's transport waited without a byte before it
   * gave the request up (the first part and the resume take a few milliseconds on 127.0.0.1). */
  const char *probe = getenv("TF_NET_PROBE_STALL_MS");
  if (probe && *probe) {
    tf__download_pause_ms = 1;
    for (int k = 0; *probe; ++k) {
      const int limit = atoi(probe);
      probe += strcspn(probe, ",");
      probe += *probe == ',';
      if (limit <= 0) continue;
      char plan[96], pd[1100];
      snprintf(plan, sizeof plan, "[{\"stall\":100000,\"hold_ms\":%d}]", limit * 4 + 8000);
      snprintf(pd, sizeof pd, "%s/probe%d", work, k);
      net_reset();
      net_plan(g_net.big, plan);
      tf__download_stall_ms = limit;
      const double p0 = tf__now_s();
      const int prc = net_download(pd, NULL, NULL, NULL);
      printf("net probe: stall limit %d ms: the call took %.2f s (%s)\n", limit, tf__now_s() - p0, tf_error_name(prc));
      fflush(stdout);
    }
    tf__download_stall_ms = 30000;
    tf__download_pause_ms = 1000;
    return 0;
  }
  /* the limits of a test run: no byte for 1.5 s is a stall (30 s in an app), and the pauses before the five
   * resumes are 20, 40, 80, 160 and 320 ms (1, 2, 4, 8 and 16 s in an app) */
  tf__download_stall_ms = 1500;
  tf__download_pause_ms = 20;
  int before = fails, rc, resumed;
  long long fetched;
  char d[1100];
  NetReqs q;

  /* 1. one download of several files (the manifest and five files) uses ONE connection: one WinHTTP session,
   * or one curl handle, opened by the library for all six requests, and one connection seen by the server.
   * (The server's count alone does not tell on Windows: WinHTTP shares the connections of one process between
   * its sessions, so six sessions also arrived on one connection there.) */
  net_reset();
  snprintf(d, sizeof d, "%s/one", work);
  int opened = tf__http_opened;
  rc = net_download(d, &fetched, &resumed, NULL);
  opened = tf__http_opened - opened;
  net_requests(NULL, &q);
  CHECK(opened == 1, "one download: the library opened %d sessions or handles for it, want 1", opened);
  CHECK(rc == 0 && net_all_ok(d), "a plain download: %s (%s)", tf_error_name(rc), tf_last_error() ? tf_last_error() : "");
  CHECK(q.all == 6 && q.connections == 1, "one download: %d requests on %d connections, want 6 requests on 1 connection", q.all, q.connections);
  CHECK(q.no_key == 0 && fetched == g_net.total && resumed == 0, "one download: %d requests without the key, %lld bytes fetched of %lld, resumed %d", q.no_key,
        fetched, g_net.total, resumed);
  NET_DONE("one download of the manifest and five files uses one connection");

  /* 2. the connection ends at 40 percent of the large file: the call resumes with Range and If-Range, and the
   * file is right; no byte is fetched twice */
  net_reset();
  net_plan(g_net.big, "[{\"cut\":160000}]");
  snprintf(d, sizeof d, "%s/cut", work);
  fetched = resumed = 0;
  opened = tf__http_opened;
  rc = net_download(d, &fetched, &resumed, NULL);
  opened = tf__http_opened - opened;
  net_requests(g_net.big, &q);
  CHECK(opened == 1, "the resume: the library opened %d sessions or handles for the download, want 1", opened);
  CHECK(rc == 0 && net_all_ok(d), "a connection that ends at 40 percent: %s (%s)", tf_error_name(rc), tf_last_error() ? tf_last_error() : "");
  CHECK(q.n == 2 && q.from[0] < 0 && q.from[1] > 0 && q.from[1] <= 160000 && q.cond[1] == 1 && q.status[1] == 206,
        "the resume: %d requests; the second from byte %lld with If-Range %d, status %d (want 2 requests, the second with Range and the ETag, 206)", q.n,
        q.from[1], q.cond[1], q.status[1]);
  /* the download's resumed mark is for a partial file an earlier call left: a resume inside the call is not one */
  CHECK(fetched == g_net.total && resumed == 0, "the resume: %lld bytes fetched of %lld, the resumed mark is %d (want 0 inside one call)", fetched,
        g_net.total, resumed);
  NET_DONE("a connection that ends at 40 percent is resumed inside the call with Range and If-Range");

  /* 3. the server sends 100000 bytes and then nothing (it would hold the connection for 12 s): the stall limit
   * ends the request, and the call resumes and finishes. Never before the limit; after it within a second on
   * libcurl and within about four on WinHTTP, which looks at its waits about every four seconds (tf_http.c) */
  net_reset();
  net_plan(g_net.big, "[{\"stall\":100000,\"hold_ms\":12000}]");
  snprintf(d, sizeof d, "%s/stall", work);
  double t0 = tf__now_s();
  rc = net_download(d, NULL, NULL, NULL);
  double dt = tf__now_s() - t0;
  net_requests(g_net.big, &q);
  CHECK(rc == 0 && net_all_ok(d), "a stall: %s (%s)", tf_error_name(rc), tf_last_error() ? tf_last_error() : "");
  CHECK(q.n == 2 && q.from[1] > 0 && q.from[1] <= 100000 && q.cond[1] == 1, "a stall: %d requests, the second from byte %lld with If-Range %d", q.n, q.from[1],
        q.cond[1]);
  CHECK(dt >= 1.4 && dt < 9.0, "a stall: the call took %.1f s (the limit is 1.5 s, the server holds for 12 s)", dt);
  printf("net:   the call with a stall took %.2f s (stall limit 1.5 s); resumed from byte %lld\n", dt, q.n > 1 ? q.from[1] : -1);
  NET_DONE("a stall ends the request after the stall limit and the call resumes");

  /* 4. six failures in a row (a broken connection, two 503, three connections that end before a byte): the call
   * ends with download_failed after the fifth resume, the pauses grow, and the partial file with its validator
   * is kept; the next call resumes from it */
  net_reset();
  net_plan(g_net.big, "[{\"cut\":50000},{\"status\":503},{\"status\":503},{\"cut\":0},{\"cut\":0},{\"cut\":0}]");
  snprintf(d, sizeof d, "%s/bound", work);
  rc = net_download(d, NULL, NULL, NULL);
  net_requests(g_net.big, &q);
  const long long kept = net_size(d, "big.bin.part");
  CHECK(rc == TF_ERR_DOWNLOAD_FAILED, "six failures in a row: %s, want download_failed", tf_error_name(rc));
  CHECK(q.n == 6, "six failures in a row: %d requests for the file, want 6 (the first and five resumes)", q.n);
  CHECK(q.n == 6 && q.at_ms[5] - q.at_ms[4] >= 250 && q.at_ms[5] - q.at_ms[0] >= 550,
        "the pauses: %lld ms before the fifth resume (320 set), %lld ms over all five (620 set)", q.n == 6 ? q.at_ms[5] - q.at_ms[4] : -1,
        q.n == 6 ? q.at_ms[5] - q.at_ms[0] : -1);
  CHECK(kept > 0 && kept <= 50000 && net_size(d, "big.bin.part.etag") > 0 && net_size(d, "big.bin") < 0 && net_size(d, "manifest.json") < 0,
        "after the bound: the partial file has %lld bytes, its validator %lld", kept, net_size(d, "big.bin.part.etag"));
  resumed = 0;
  rc = net_download(d, NULL, &resumed, NULL);
  net_requests(g_net.big, &q);
  CHECK(resumed == 1, "the next call: the resumed mark is %d, want 1 (it went on from the partial file the call before left)", resumed);
  CHECK(rc == 0 && net_all_ok(d) && q.n == 7 && q.from[6] == kept && q.cond[6] == 1 && q.status[6] == 206,
        "the next call: %s, %d requests, the last from byte %lld (the partial file had %lld) with If-Range %d", tf_error_name(rc), q.n, q.n == 7 ? q.from[6] : -1,
        kept, q.n == 7 ? q.cond[6] : -1);
  NET_DONE("five resumes that fail end the call with the error, and the partial file is kept");

  /* 5. a 200 to a range request is the whole file: the partial file is replaced by it, never appended to, and
   * nothing is asked a third time. (a) the server ignores the Range; (b) the partial file is not this file's
   * (another validator beside it), which If-Range tells the server */
  net_reset();
  net_plan(g_net.big, "[{\"cut\":160000},{\"whole\":true}]");
  snprintf(d, sizeof d, "%s/whole", work);
  fetched = 0;
  rc = net_download(d, &fetched, NULL, NULL);
  net_requests(g_net.big, &q);
  CHECK(rc == 0 && net_all_ok(d), "a 200 to a range request: %s (%s)", tf_error_name(rc), tf_last_error() ? tf_last_error() : "");
  CHECK(q.n == 2 && q.from[1] > 0 && q.status[1] == 200 && fetched == g_net.total + q.from[1],
        "a 200 to a range request: %d requests, the second from byte %lld got %d; %lld bytes fetched, want the files and the first part once more", q.n,
        q.from[1], q.status[1], fetched);
  net_reset();
  snprintf(d, sizeof d, "%s/stale", work);
  tf_mkdirs(d);
  {
    char junk[1000], *pp = tf_path_join(d, "big.bin.part"), *pe = tf_path_join(d, "big.bin.part.etag");
    memset(junk, 'x', sizeof junk);
    CHECK(pp && pe && !tf_write_file(pp, junk, sizeof junk) && !tf_write_file(pe, "\"another-file\"", 14), "the partial file of another file was not written");
    free(pp);
    free(pe);
  }
  rc = net_download(d, NULL, NULL, NULL);
  net_requests(g_net.big, &q);
  CHECK(rc == 0 && net_all_ok(d), "a partial file with another validator: %s (%s)", tf_error_name(rc), tf_last_error() ? tf_last_error() : "");
  CHECK(q.n == 1 && q.from[0] == 1000 && q.cond[0] == 2 && q.status[0] == 200,
        "a partial file with another validator: %d requests, the first from byte %lld with If-Range %d got %d (want one request, 200)", q.n, q.from[0], q.cond[0],
        q.status[0]);
  NET_DONE("a 200 to a range request replaces the partial file");

  /* 6. an answer that refuses is asked ONCE. With a stop reason (SPEC 15.3): invalid_key, server_rejected. A
   * daily limit and a network that is not served are not stops, and are not asked again either */
  {
    static const struct {
      const char *name, *step;
      int want;
    } cases[] = {
      {"401 key_revoked", "[{\"status\":401,\"body\":{\"error\":\"invalid_key\",\"message\":\"missing or invalid x-qlab-key\",\"reason\":\"key_revoked\"}}]", TF_ERR_INVALID_KEY},
      {"403 app_blocked", "[{\"status\":403,\"body\":{\"error\":\"app_blocked\",\"message\":\"blocked\",\"reason\":\"app_blocked\"}}]", TF_ERR_SERVER_REJECTED},
      {"429 model_limit", "[{\"status\":429,\"body\":{\"error\":\"model_limit\",\"scope\":\"install\"}}]", TF_ERR_DOWNLOAD_FAILED},
      {"503 network_blocked", "[{\"status\":503,\"body\":{\"error\":\"network_blocked\",\"retry_after\":3600}}]", TF_ERR_DOWNLOAD_FAILED},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
      net_reset();
      net_plan(g_net.big, cases[i].step);
      snprintf(d, sizeof d, "%s/refused%d", work, (int)i);
      rc = net_download(d, NULL, NULL, NULL);
      net_requests(g_net.big, &q);
      CHECK(rc == cases[i].want && q.n == 1, "%s: %s after %d requests for the file, want %s after 1", cases[i].name, tf_error_name(rc), q.n,
            tf_error_name(cases[i].want));
    }
  }
  NET_DONE("a refusal is asked once and never retried");

  /* 7. a slow link: 64 bytes every 300 ms for 4 s, which is longer than the stall limit of 1.5 s. It keeps
   * delivering, so it is not cut: one request */
  net_reset();
  net_plan(g_net.big, "[{\"slow\":{\"bytes\":64,\"every_ms\":300,\"for_ms\":4000}}]");
  snprintf(d, sizeof d, "%s/slow", work);
  t0 = tf__now_s();
  rc = net_download(d, NULL, NULL, NULL);
  dt = tf__now_s() - t0;
  net_requests(g_net.big, &q);
  CHECK(rc == 0 && net_all_ok(d) && q.n == 1 && dt >= 3.5, "a slow link: %s, %d requests for the file in %.1f s, want 1 request over 4 s", tf_error_name(rc), q.n, dt);
  printf("net:   the slow link's download took %.2f s in %d request(s)\n", dt, q.n);
  NET_DONE("a slow link that keeps delivering for longer than the stall limit is not cut");

  /* 13. NO request of the library follows a redirect (SPEC 15.4 and 15.5, contract 1.17.0), on the real HTTP
   * code. The server answers with a 302, and with a 307, to another path, where it would serve the request
   * too and where it records what arrives (the method, the header NAMES, the size of the body). Another path
   * of this server plays another host; case (b) names another host for real, localhost for 127.0.0.1. Wanted
   * each time: nothing arrives at the other path, and the request fails as its kind fails. */
  {
    static const int codes[] = {302, 307};
    char step[1200], one[200], port[16], why[640];
    snprintf(port, sizeof port, "%s", strrchr(g_net.url, ':') ? strrchr(g_net.url, ':') + 1 : "");
    tf_mutex_init(&g_st.m); /* the upload test's stand-in API (cases g and h) */
    for (int k = 0; k < 2; ++k) {
      const int code = codes[k];
      /* (a) a file of a download without a Client (the key alone): download_failed after one request; (b) the
       * same with a Location that names another host */
      for (int host = 0; host < 2; ++host) {
        net_reset();
        snprintf(step, sizeof step, "[{\"status\":%d,\"location\":\"%s%s/moved/sdk/v4.0/big.bin\"}]", code, host ? "http://localhost:" : "", host ? port : "");
        net_plan(g_net.big, step);
        snprintf(d, sizeof d, "%s/redirect_file%d_%d", work, code, host);
        rc = net_download(d, NULL, NULL, NULL);
        net_requests(g_net.big, &q);
        CHECK(rc == TF_ERR_DOWNLOAD_FAILED && q.n == 1 && q.moved == 0, "%d to a model file%s: the download returned %s after %d request(s) for the file; %s", code,
              host ? ", Location on the host name localhost" : "", tf_error_name(rc), q.n, net_moved(&q, why));
      }
      /* (c) the manifest of a Client's download (the key and the app token): download_failed, and never a
       * stop: no refusal is remembered */
      tf__now_override = 1794355200 + 9 * 3600;
      net_reset();
      snprintf(step, sizeof step, "[{\"status\":%d,\"location\":\"/moved/sdk/v4.0/manifest.json\"}]", code);
      net_plan("manifest.json", step);
      tf_http_set_transport(net_stub, NULL);
      snprintf(d, sizeof d, "%s/redirect_client%d", work, code);
      TfClient *rcl = net_client(d);
      if (!rcl) return 2;
      rc = tf_client_prefetch(rcl, NULL);
      net_requests("manifest.json", &q);
      CHECK(rc == TF_ERR_DOWNLOAD_FAILED && q.n == 1 && q.moved == 0 && !*tf__client_gate(rcl),
            "%d to a Client's manifest request: prefetch returned %s after %d request(s) for the manifest, remembered refusal \"%s\"; %s", code, tf_error_name(rc), q.n,
            tf__client_gate(rcl), net_moved(&q, why));
      tf_client_destroy(rcl);
      tf_http_set_transport(NULL, NULL);
      tf__now_override = 0;
      /* (d) the API call that carries the key in a header (the config), (e) a call that carries the app token,
       * (f) a call as the sharing start is made (the token and the recording's id, a JSON body), of a Client
       * whose API is the stand-in server. The call's answer is the 3xx itself: its ordinary failure */
      snprintf(d, sizeof d, "%s/redirect_api%d", work, code);
      tf_mkdirs(d);
      TfClientConfig ao = tf__client_default_config();
      ao.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
      ao.app_id = "org.example.Net";
      ao.storage_dir = d;
      ao.api_url = g_net.url;
      ao.telemetry = 0;
      TfClient *ac = tf__client_open(&ao);
      if (!ac) return 2;
      static const struct {
        const char *name, *method, *path, *plan, *body, *extra;
        int auth;
      } calls[] = {
        {"the API call with the key (GET /v1/config)", "GET", "/v1/config?bucket=1", "/v1/config", NULL, NULL, 0},
        {"an API call with the app token (POST /v1/telemetry)", "POST", "/v1/telemetry", "/v1/telemetry", "{\"v\":1}", NULL, 1},
        {"a call made as the sharing start is (POST /v1/uploads)", "POST", "/v1/uploads", "/v1/uploads", "{\"v\":1}",
         "x-qlab-id: 0123456789abcdef0123456789abcdef", 1},
      };
      for (int i = 0; i < 3; ++i) {
        net_reset();
        snprintf(step, sizeof step, "[{\"status\":%d,\"location\":\"/moved%s\"}]", code, calls[i].plan);
        net_plan(calls[i].plan, step);
        TfHttpResponse ar;
        memset(&ar, 0, sizeof ar);
        const int arc = tf__client_call(ac, calls[i].method, calls[i].path, calls[i].body, calls[i].auth, calls[i].extra, &ar);
        net_requests("api", &q);
        /* the first call with a token asks /v1/attest before it: one request more */
        const int want = 1 + (i == 1);
        CHECK(arc == 0 && ar.status == code && q.n == want && q.moved == 0, "%d to %s: the call returned %d with status %d after %d API request(s) (want status %d after %d); %s",
              code, calls[i].name, arc, ar.status, q.n, code, want, net_moved(&q, why));
        if (!arc) tf_http_response_free(&ar);
      }
      tf__client_close(ac);
      /* (g) a sharing start and (h) a segment upload, made by the library's own uploader: a Client with the
       * learner's yes records a session of 2.5 s and its outbox sends it. Every start (g), or every PUT of a
       * segment (h), is answered with the redirect; the uploader asks again by its ordinary rule, so more than
       * one request may be made, and none may arrive at the other path */
      for (int kind = 1; kind <= 2; ++kind) {
        const char *plan = kind == 1 ? "/v1/uploads" : "/put/segment";
        net_reset();
        stub_reset();
        snprintf(one, sizeof one, "{\"status\":%d,\"location\":\"/moved%s\"}", code, plan);
        snprintf(step, sizeof step, "[%s,%s,%s,%s,%s]", one, one, one, one, one);
        net_plan(plan, step);
        tf__share_plan[0] = tf__share_plan[1] = tf__share_plan[2] = tf__share_plan[3] = 1; /* 1 s segments */
        g_net_share = kind;
        tf_http_set_transport(net_share_transport, NULL);
        snprintf(d, sizeof d, "%s/redirect_share%d_%d", work, code, kind);
        TfClient *sc = share_client(d);
        if (!sc) return 2;
        static float noise[48000 * 3];
        lcg_noise(noise, 48000 * 3, 0.2);
        const int yes = tf__client_sharing_answer(sc, 1) == 0 && tf__client_sharing_state(sc);
        tf__sharing_outbox_resume(sc);
        TfShareSession *ss = tf__sharing_session_begin(sc, 48000, NULL);
        for (int i = 0; ss && i < 125; ++i) tf__sharing_session_push(ss, noise + i * 960, 960);
        if (ss) share_finish(sc, ss, 1);
        /* two passes of the uploader (at most 4 s), then a moment for a request that followed a redirect */
        tf__sharing_outbox_wait(sc, 4000);
        tf__sleep_ms(300);
        net_requests(kind == 1 ? "api" : "put", &q);
        CHECK(yes && ss && q.n >= 1 && q.moved == 0, "%d to %s of the uploader: consent %d, %d request(s) reached the server; %s", code,
              kind == 1 ? "the sharing start" : "a segment's PUT", yes, q.n, net_moved(&q, why));
        tf__client_close(sc);
        tf_http_set_transport(NULL, NULL);
        g_net_share = 0;
      }
      tf__share_plan[0] = 10, tf__share_plan[1] = 20, tf__share_plan[2] = 40, tf__share_plan[3] = 60;
    }
  }
  NET_DONE("no request follows a redirect (302, 307): model requests, API calls, a sharing start, a segment upload");

  /* 14. Only the answer that was asked for is taken (3.1.3). A file request accepts a 200 whose length is the
   * file's, and a 206 whose Content-Range starts at the request's first byte and names the file's size. Any
   * other 2xx is a failed attempt: it counts against the five resumes, the pauses apply, nothing loops, and the
   * partial file that was on disk is there afterwards, byte for byte. Here the server gives the same wrong
   * answer at every attempt, to a download that has the first 100000 bytes of the file on disk: the call ends
   * with download_failed after at most seven requests for the file (the first, five resumes, and one more
   * after a full-size file with a wrong hash), and the next call finishes from the same 100000 bytes. */
  {
    static const struct {
      const char *name, *raw;
      int seeded;
    } bad[] = {
      {"a 204", "{\"status\":204}", 1},
      {"a 200 shorter than the file", "{\"status\":200,\"length\":5000}", 1},
      {"a 200 shorter than the file, without a Content-Length", "{\"status\":200,\"length\":5000,\"chunked\":true}", 1},
      {"a 200 longer than the file", "{\"status\":200,\"length\":405000}", 1},
      {"a 200 longer than the file, without a Content-Length", "{\"status\":200,\"from\":1000,\"length\":405000,\"chunked\":true}", 1},
      {"an empty 206", "{\"status\":206,\"length\":0,\"content_range\":\"bytes 100000-399999/400000\"}", 1},
      {"a 206 with another total", "{\"status\":206,\"from\":100000,\"length\":300000,\"content_range\":\"bytes 100000-399999/999999\"}", 1},
      {"a 206 with another start", "{\"status\":206,\"from\":50000,\"length\":300000,\"content_range\":\"bytes 50000-349999/400000\"}", 1},
      {"a 206 without a Content-Range", "{\"status\":206,\"from\":100000,\"length\":300000}", 1},
      {"a 200 with a JSON error body", "{\"status\":200,\"json\":true}", 1},
      /* with nothing on disk there is no partial file to keep: the body is cut at the file's size and the
       * call still ends with an error (the SHA-256 of what came is not the file's) */
      {"a 200 longer than the file, without a Content-Length, nothing on disk",
       "{\"status\":200,\"from\":1000,\"length\":405000,\"chunked\":true}", 0},
    };
    const long long had = 100000;
    char src[1100], plan[1400], sha0[65], sha1[65];
    snprintf(src, sizeof src, "%s/one", work);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
      net_reset();
      snprintf(plan, sizeof plan, "[{\"raw\":%s},{\"raw\":%s},{\"raw\":%s},{\"raw\":%s},{\"raw\":%s},{\"raw\":%s},{\"raw\":%s},{\"raw\":%s}]",
               bad[i].raw, bad[i].raw, bad[i].raw, bad[i].raw, bad[i].raw, bad[i].raw, bad[i].raw, bad[i].raw);
      net_plan(g_net.big, plan);
      snprintf(d, sizeof d, "%s/answer%d", work, (int)i);
      sha0[0] = 0;
      CHECK(!bad[i].seeded || !net_seed_part(src, d, had, sha0), "%s: the partial file was not put in place", bad[i].name);
      rc = net_download(d, NULL, NULL, NULL);
      net_requests(g_net.big, &q);
      const long long kept = net_size(d, "big.bin.part");
      net_sha(d, "big.bin.part", sha1);
      const int same = bad[i].seeded ? kept == had && !strcmp(sha0, sha1) : kept <= g_net.big_size;
      printf("net:   %s at every attempt: %s after %d request(s) for the file; the partial file had %lld bytes and has %lld (%s)\n", bad[i].name, tf_error_name(rc), q.n,
             bad[i].seeded ? had : 0, kept, bad[i].seeded ? (same ? "the same bytes" : "NOT the bytes it had") : "nothing was there before");
      CHECK(rc != 0 && (!bad[i].seeded || rc == TF_ERR_DOWNLOAD_FAILED), "%s: the call returned %s, want an error (download_failed with a partial file on disk)", bad[i].name,
            tf_error_name(rc));
      CHECK(q.n >= 1 && q.n <= 7, "%s: %d requests for the file, want at most 7", bad[i].name, q.n);
      CHECK(same, "%s: the partial file had %lld bytes and has %lld%s", bad[i].name, bad[i].seeded ? had : 0, kept,
            bad[i].seeded && kept == had ? ", and they are not the bytes it had" : "");
      CHECK(net_size(d, "big.bin.part.new") < 0 && net_size(d, "big.bin") < 0, "%s: a file was left beside the partial file, or the file was put in place", bad[i].name);
      if (!bad[i].seeded) continue;
      /* the next call, to a server that answers as asked, finishes from the bytes that were kept */
      net_reset();
      fetched = resumed = 0;
      rc = net_download(d, &fetched, &resumed, NULL);
      net_requests(g_net.big, &q);
      CHECK(rc == 0 && net_all_ok(d) && q.n == 1 && q.from[0] == had && fetched == g_net.big_size - had && resumed == 1,
            "%s, the next call: %s after %d request(s), the first from byte %lld, %lld bytes fetched (want ok, 1 request from %lld, %lld bytes)", bad[i].name,
            tf_error_name(rc), q.n, q.n ? q.from[0] : -1, fetched, had, g_net.big_size - had);
    }
  }
  NET_DONE("an answer that is not the one asked for is a failed attempt, and the partial file is kept");

  /* 8. a cancel ends the pause before a resume at once (the pause is 4 s here), and nothing is asked after it */
  {
    net_reset();
    net_plan(g_net.big, "[{\"cut\":1000},{\"cut\":0}]");
    tf__download_pause_ms = 4000;
    snprintf(d, sizeof d, "%s/cancel_pause", work);
    NetRun run = {d, 0, 0, 0};
    TfThread th;
    if (tf_thread_start(&th, net_thread, &run)) return 2;
    net_wait_first(1000);
    tf__sleep_ms(400);
    const int waiting = !run.done;
    t0 = tf__now_s();
    tf__http_cancel(&run.cancel);
    tf_thread_join(th);
    dt = tf__now_s() - t0;
    net_requests(g_net.big, &q);
    CHECK(waiting && run.rc == TF_ERR_DOWNLOAD_FAILED && dt < 1.0 && q.n == 1,
          "a cancel in the pause: the call was %s, ended %.2f s after the cancel with %s, %d requests for the file (want: waiting, under 1 s, download_failed, 1)",
          waiting ? "waiting" : "over already", dt, tf_error_name(run.rc), q.n);
    tf__download_pause_ms = 20;
    printf("net:   the call ended %.2f s after the cancel in its pause\n", dt);
    NET_DONE("a cancel ends the pause before a resume at once");
  }

  /* 9. a cancel ends a request in flight (the server sent 100000 bytes and holds for 6 s; the stall limit is
   * 30 s here, so only the cancel can end it) */
  {
    net_reset();
    net_plan(g_net.big, "[{\"stall\":100000,\"hold_ms\":6000}]");
    tf__download_stall_ms = 30000;
    snprintf(d, sizeof d, "%s/cancel_flight", work);
    NetRun run = {d, 0, 0, 0};
    TfThread th;
    if (tf_thread_start(&th, net_thread, &run)) return 2;
    net_wait_first(100000);
    tf__sleep_ms(300);
    const int waiting = !run.done;
    t0 = tf__now_s();
    tf__http_cancel(&run.cancel);
    tf_thread_join(th);
    dt = tf__now_s() - t0;
    net_requests(g_net.big, &q);
    CHECK(waiting && run.rc == TF_ERR_DOWNLOAD_FAILED && dt < 2.5 && q.n == 1,
          "a cancel in flight: the call was %s, ended %.2f s after the cancel with %s, %d requests for the file (want: waiting, under 2.5 s, download_failed, 1)",
          waiting ? "waiting" : "over already", dt, tf_error_name(run.rc), q.n);
    tf__download_stall_ms = 1500;
    printf("net:   the call ended %.2f s after the cancel of its request in flight\n", dt);
    NET_DONE("a cancel ends a model request in flight");
  }

  /* 10. through a Client: the diagnosis names the protocol of the last model request ("http/1.1" from this
   * server; "h2" and "h3" need TLS and are not shown), nothing before the first; the whole download on one
   * connection, every request with the key and the app token */
  {
    tf__now_override = 1794355200 + 9 * 3600;
    net_reset();
    tf_http_set_transport(net_stub, NULL);
    snprintf(d, sizeof d, "%s/client", work);
    TfClient *c = net_client(d);
    if (!c) return 2;
    char proto[24];
    net_protocol(c, proto);
    CHECK(!strcmp(proto, "(null)"), "downloadProtocol before a model request: %s, want null", proto);
    opened = tf__http_opened;
    rc = tf_client_prefetch(c, NULL);
    opened = tf__http_opened - opened;
    CHECK(opened == 1, "a Client's download: the library opened %d sessions or handles for it, want 1", opened);
    CHECK(rc == 0, "prefetch from the stand-in server: %s (%s)", tf_error_name(rc), tf_last_error() ? tf_last_error() : "");
    net_protocol(c, proto);
    TfDiagnosis dg;
    CHECK(!strcmp(proto, "http/1.1"), "downloadProtocol after a download over plain HTTP: %s, want http/1.1", proto);
    /* the struct has no field for it (TfDiagnosis does not grow in a patch release): its json has the key */
    CHECK(tf_client_diagnose(c, &dg) == 0 && dg.json && strstr(dg.json, "\"downloadProtocol\":\"http/1.1\""), "TfDiagnosis.json: %s",
          dg.json ? dg.json : "(null)");
    net_requests(NULL, &q);
    CHECK(q.all == 6 && q.connections == 1 && q.no_key == 0 && q.no_token == 0, "a Client's download: %d requests on %d connections, %d without the key, %d without the token",
          q.all, q.connections, q.no_key, q.no_token);
    tf_client_destroy(c);
    NET_DONE("the diagnosis names the protocol of the last model download (http/1.1 here)");

    /* 11. a refusal with a stop reason in the middle of a Client's download: asked once, the error is the
     * refusal's, the refusal is remembered and no model file stays (as in 3.1.0) */
    net_reset();
    net_plan(g_net.big, "[{\"status\":401,\"body\":{\"error\":\"invalid_key\",\"message\":\"missing or invalid x-qlab-key\",\"reason\":\"key_revoked\"}}]");
    snprintf(d, sizeof d, "%s/client_stop", work);
    c = net_client(d);
    if (!c) return 2;
    rc = tf_client_prefetch(c, NULL);
    net_requests(g_net.big, &q);
    char folder[1300];
    snprintf(folder, sizeof folder, "%s/models/%s", d, g_net.build);
    CHECK(rc == TF_ERR_INVALID_KEY && q.n == 1 && q.all == 6, "a stop in a Client's download: %s after %d requests for the file (%d in all), want invalid_key after 1",
          tf_error_name(rc), q.n, q.all);
    CHECK(!strcmp(tf__client_gate(c), "key_revoked") && net_size(folder, "tokens.txt") < 0 && net_size(folder, "a.bin") < 0 && tf__dir_bytes(folder) <= 0,
          "a stop in a Client's download: the remembered refusal is \"%s\", the build folder holds %lld bytes", tf__client_gate(c), tf__dir_bytes(folder));
    tf_client_destroy(c);
    NET_DONE("a refusal with a stop reason is asked once and stops the model");

    /* 12. destroying the Client ends its download in flight: tf_client_prefetch on another thread is in a
     * request that the server holds for 12 s (the stall limit is 30 s here), tf_client_destroy does not wait
     * for it, and the prefetch returns download_failed with nothing asked after it */
    net_reset();
    net_plan(g_net.big, "[{\"stall\":100000,\"hold_ms\":12000}]");
    tf__download_stall_ms = 30000;
    snprintf(d, sizeof d, "%s/client_destroy", work);
    c = net_client(d);
    if (!c) return 2;
    NetPrefetch pf = {c, 0, 0};
    TfThread pth;
    if (tf_thread_start(&pth, net_prefetch_thread, &pf)) return 2;
    net_wait_first(100000);
    tf__sleep_ms(300);
    const int in_flight = !pf.done;
    const double d0 = tf__now_s();
    tf_client_destroy(c);
    const double d_destroy = tf__now_s() - d0;
    tf_thread_join(pth);
    net_requests(g_net.big, &q);
    CHECK(in_flight && pf.rc == TF_ERR_DOWNLOAD_FAILED && d_destroy < 2.5 && q.n == 1,
          "destroying the Client in a download: the prefetch was %s, the destroy took %.2f s, the prefetch returned %s, %d requests for the file "
          "(want: in flight, under 2.5 s, download_failed, 1)",
          in_flight ? "in flight" : "over already", d_destroy, tf_error_name(pf.rc), q.n);
    printf("net:   tf_client_destroy took %.2f s with a download in flight\n", d_destroy);
    tf__download_stall_ms = 1500;
    tf_http_set_transport(NULL, NULL);
    tf__now_override = 0;
    NET_DONE("destroying the Client ends its download in flight");
  }

  tf__download_stall_ms = 30000;
  tf__download_pause_ms = 1000;
  tf_json_free(g_net.setup);
  printf("net (the real HTTP code against the stand-in server): %s\n", fails ? "FAIL" : "all ok");
  return fails ? 1 : 0;
}
