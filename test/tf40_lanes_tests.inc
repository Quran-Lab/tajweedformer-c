/* 4.0.1: the downloader's lanes (tf_download.c), on the flow fixture's signed manifest and files, against a stand-in
 * model host that several lanes call at once (its counts are kept under a lock):
 *   - at most tf__download_lanes files in flight, every file asked once, the progress on the caller's thread and never
 *     past the total;
 *   - each file hashed as it is written: no file is read again after its last byte (tf__download_rereads), a partial
 *     file left by an earlier call is hashed first and goes on with Range, a partial file whose bytes are not the
 *     file's is caught at the end and fetched whole once more;
 *   - a connection that ends half way is resumed; a file that is never the signed bytes is checksum_mismatch after
 *     its second whole fetch; a file whose answer stalls every time is download_failed after its fifth resume;
 *   - with a lease hook: the files come from the build's folder on the model host while the hook runs, nothing is
 *     installed before it returns; a hook that fails leaves no file of this call; a lease answer naming another folder
 *     drops what came from the host and fetches the files from that folder.
 *   tf40_tests lanes <flow fixture> <work> */
#include "tf_thread.h"

static TfMutex g_l_lock; /* the stand-in's counts: several lanes call it at once */
static struct {
  const TfJson *fx;
  int in_flight, max_in_flight, requests, ranged, host, lease_base;
  int per_layers, per_frontend, layers_ranged_first;
  long long layers_first_from;
  int delay_ms;
  int cut_layers;          /* the next n answers for layers.zfw.qle end half way */
  int bad_frontend;        /* frontend.zfw.qle's bytes are not the signed ones */
  int stall_layers;        /* every answer for layers.zfw.qle stalls */
  volatile int hook_over;  /* the lease hook has returned */
  int during_hook;         /* file requests made while the hook ran */
  int installs, installs_before_hook;
  const char *down_host;   /* 4.0.1: every file request to this models host gets no answer (its manifest answers) */
  int down_asked, other_hosts;
} g_l;

#ifdef _WIN32
static unsigned long lanes_tid(void) { return (unsigned long)GetCurrentThreadId(); }
#else
static unsigned long lanes_tid(void) { return (unsigned long)pthread_self(); }
#endif

static int lanes_net(const TfHttpRequest *q, TfHttpResponse *r, void *user) {
  (void)user;
  memset(r, 0, sizeof *r);
  const char *rel = strstr(q->url, "/sdk/v4.0/");
  if (!rel || !strstr(q->url, S(g_l.fx, "build"))) {
    r->status = 404;
    return 0;
  }
  rel += 10;
  if (!strcmp(rel, "manifest.json")) {
    const char *mt = S(g_l.fx, "manifest");
    r->status = 200;
    r->body = dupz(mt, strlen(mt));
    r->body_n = strlen(mt);
    return 0;
  }
  if (g_l.down_host) {
    char want[300];
    snprintf(want, sizeof want, "https://%s/", g_l.down_host);
    const int down = !strncmp(q->url, want, strlen(want));
    tf_mutex_lock(&g_l_lock);
    if (down) g_l.down_asked++;
    else g_l.other_hosts++;
    tf_mutex_unlock(&g_l_lock);
    if (down) return tf__fail(TF__ERR_NETWORK, "the first models host gives no answer (simulated)");
  }
  uint8_t *data = NULL;
  const size_t n = tf_json_get(tf_json_get(g_l.fx, "files"), rel) ? unhex(S(tf_json_get(g_l.fx, "files"), rel), &data) : 0;
  if (!data || !q->download_file) {
    free(data);
    r->status = 404;
    return 0;
  }
  const int layers = !strcmp(rel, "layers.zfw.qle"), frontend = !strcmp(rel, "frontend.zfw.qle");
  const long long from = q->range_from > 0 ? q->range_from : 0;
  int cut = 0, stall = 0;
  tf_mutex_lock(&g_l_lock);
  g_l.requests++;
  g_l.in_flight++;
  if (g_l.in_flight > g_l.max_in_flight) g_l.max_in_flight = g_l.in_flight;
  g_l.ranged += from > 0;
  if (strstr(q->url, "/models/lease/")) g_l.lease_base++;
  else g_l.host++;
  if (!g_l.hook_over) g_l.during_hook++;
  if (layers) {
    if (!g_l.per_layers++) g_l.layers_first_from = from;
    if (g_l.cut_layers > 0) {
      g_l.cut_layers--;
      cut = 1;
    }
    stall = g_l.stall_layers;
  }
  if (frontend) g_l.per_frontend++;
  tf_mutex_unlock(&g_l_lock);
  if (g_l.delay_ms) tf__sleep_ms(g_l.delay_ms);
  int rc = 0;
  if (stall) {
    r->status = 200;
    tf__http_note(TF__FAIL_TIMEOUT, "ERROR_WINHTTP_TIMEOUT", 12002);
    rc = tf__fail(TF__ERR_NETWORK, "the stand-in's answer stalled");
  } else if (from > (long long)n) {
    r->status = 416;
  } else {
    if (frontend && g_l.bad_frontend && n) data[n - 1] ^= 1;
    FILE *f = fopen(q->download_file, from > 0 ? "ab" : "wb");
    size_t len = n - (size_t)from;
    if (cut) len /= 2;
    if (f) {
      fwrite(data + from, 1, len, f);
      fclose(f);
    }
    /* as the real transports: the bytes as they reach the file, in order */
    if (len && q->on_data) q->on_data(q->bytes_user, data + from, len);
    if (len && q->on_bytes) q->on_bytes(q->bytes_user, (long long)len);
    r->status = from > 0 ? 206 : 200;
    if (cut) rc = tf__fail(TF__ERR_NETWORK, "connection reset (simulated)");
  }
  free(data);
  tf_mutex_lock(&g_l_lock);
  g_l.in_flight--;
  tf_mutex_unlock(&g_l_lock);
  return rc;
}

static unsigned long g_lanes_caller;
static long long g_lanes_last, g_lanes_total;
static int g_lanes_back, g_lanes_past, g_lanes_other_thread, g_lanes_calls;
static void lanes_progress(void *user, long long done, long long total) {
  (void)user;
  g_lanes_calls++;
  if (done < g_lanes_last) g_lanes_back++;
  if (done > total) g_lanes_past++;
  if (lanes_tid() != g_lanes_caller) g_lanes_other_thread++;
  g_lanes_last = done;
  g_lanes_total = total;
}
static const char *g_lanes_hook_base; /* the folder the stand-in lease answer names; NULL: the hook fails */
static int lanes_hook(void *user, const char *manifest, size_t n, const TfJson *m, char *base, size_t cap) {
  (void)user;
  (void)manifest;
  (void)n;
  (void)m;
  tf__sleep_ms(300); /* the install key, the attest and the lease: Windows' TPM key took 1.5 s in Stage B */
  g_l.hook_over = 1;
  if (!g_lanes_hook_base) return tf__fail(TF_ERR_SERVER_REJECTED, "the stand-in lease route refused");
  snprintf(base, cap, "%s", g_lanes_hook_base);
  return 0;
}
static int lanes_install(void *user, const char *decoded_path, const char *manifest, size_t n) {
  (void)user;
  (void)decoded_path;
  (void)manifest;
  (void)n;
  g_l.installs++;
  if (!g_l.hook_over) g_l.installs_before_hook++;
  return 0;
}

static void lanes_reset(int lanes) {
  const TfJson *fx = g_l.fx;
  memset(&g_l, 0, sizeof g_l);
  g_l.fx = fx;
  g_l.delay_ms = 20;
  tf__download_lanes = lanes;
  tf__download_rereads = 0;
  g_lanes_caller = lanes_tid();
  g_lanes_last = -1;
  g_lanes_total = 0;
  g_lanes_back = g_lanes_past = g_lanes_other_thread = g_lanes_calls = 0;
}
static TfDownload lanes_opts(const char *dest) {
  TfDownload d = tf__download_defaults();
  d.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  d.revision = S(g_l.fx, "build");
  d.dest = dest;
  d.contexts = "c16_l64";
  d.trusted_key_hex = S(g_l.fx, "signer_hex");
  d.check_build = 1;
  d.progress = lanes_progress;
  return d;
}
/* the stored file of one entry is the signed bytes */
static int lanes_has(const char *dest, const char *rel, const char *path_in_fixture) {
  char *p = tf_path_join(dest, rel);
  uint8_t *want = NULL;
  const size_t n = unhex(S(tf_json_get(g_l.fx, "files"), path_in_fixture), &want);
  char *got = NULL;
  size_t gn = 0;
  const int ok = p && !tf_read_file(p, &got, &gn) && gn == n && !memcmp(got, want, n);
  free(got);
  free(want);
  free(p);
  return ok;
}
/* any file of this call's kind left in dest: CDN forms, partial files, the plain files */
static int lanes_left(const char *dest) {
  static const char *const names[] = {"cdn/tokens.txt.qle", "cdn/layers.zfw.qle", "cdn/layers.zfw.qle.part", "cdn/frontend.zfw.qle",
                                      "cdn/c16_l64/model_engine2.onnx.zfg.qle", "cdn/c16_l64/frames.zfw.qle", "calibration.json",
                                      "calibration.json.gz", "selftest/clip01.wav", "manifest.json"};
  int left = 0;
  for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
    char *p = tf_path_join(dest, names[i]);
    left += p && tf_file_exists(p);
    free(p);
  }
  return left;
}

static int t_lanes(const char *fixture, const char *work) {
  TfJson *fx = load_json(fixture, "flow.json");
  if (!fx) return 2;
  tf_mutex_init(&g_l_lock);
  g_l.fx = fx;
  tf_http_set_transport(lanes_net, NULL);
  tf__download_pause_ms = 1;
  char dest[1100];
  const char *host = "https://cdn.quranlab.ai/models/Quran-Lab/tajweedformer-v4/";
  char host_folder[300], lease_folder[300];
  snprintf(host_folder, sizeof host_folder, "%s%s/sdk/v4.0/", host, S(fx, "build"));
  snprintf(lease_folder, sizeof lease_folder, "https://cdn.quranlab.ai/models/lease/Quran-Lab/tajweedformer-v4/%s/sdk/v4.0/", S(fx, "build"));

  /* 1. four lanes: at most four in flight, each file once, no file read again, progress on this thread */
  for (int lanes = 1; lanes <= 4; lanes += 3) {
    lanes_reset(lanes);
    snprintf(dest, sizeof dest, "%s/plain%d", work, lanes);
    rmrf(dest);
    TfDownload d = lanes_opts(dest);
    const int rc = tf__download_model(&d);
    CHECK(rc == 0, "lanes %d: the download (%s)", lanes, rc ? tf_last_error() : "ok");
    CHECK(g_l.max_in_flight >= (lanes > 1 ? 2 : 1) && g_l.max_in_flight <= lanes, "lanes %d: %d files in flight at most", lanes, g_l.max_in_flight);
    CHECK(g_l.requests == 8 && !g_l.ranged, "lanes %d: the eight files of the native profile asked once each (%d requests, %d ranged)", lanes, g_l.requests,
          g_l.ranged);
    CHECK(tf__download_rereads == 0, "lanes %d: no file read again after its last byte (%d)", lanes, tf__download_rereads);
    CHECK(lanes_has(dest, "cdn/layers.zfw.qle", "layers.zfw.qle") && lanes_has(dest, "cdn/c16_l64/frames.zfw.qle", "c16_l64/frames.zfw.qle"),
          "lanes %d: the CDN forms are the signed bytes", lanes);
    CHECK(!g_lanes_other_thread && !g_lanes_past && g_lanes_calls > 0 && g_lanes_last == g_lanes_total && !g_lanes_back,
          "lanes %d: the progress on the caller's thread, never back, never past the total, ending at it (%d calls, other thread %d, back %d)", lanes,
          g_lanes_calls, g_lanes_other_thread, g_lanes_back);
  }
  /* 2. a partial file an earlier call left: hashed first, then the rest with Range; nothing read again at the end */
  {
    lanes_reset(4);
    snprintf(dest, sizeof dest, "%s/resume", work);
    rmrf(dest);
    uint8_t *lb = NULL;
    const size_t ln = unhex(S(tf_json_get(fx, "files"), "layers.zfw.qle"), &lb);
    char *part = tf_path_join(dest, "cdn/layers.zfw.qle.part"), *cdn = tf_path_join(dest, "cdn");
    tf_mkdirs(cdn);
    tf_write_file(part, (const char *)lb, ln / 2);
    TfDownload d = lanes_opts(dest);
    const int rc = tf__download_model(&d);
    CHECK(rc == 0 && g_l.per_layers == 1 && g_l.layers_first_from == (long long)(ln / 2) && tf__download_rereads == 0 &&
            lanes_has(dest, "cdn/layers.zfw.qle", "layers.zfw.qle"),
          "resume: the partial file goes on from byte %lld with Range and is the signed bytes, read again %d times (%s)", g_l.layers_first_from,
          tf__download_rereads, rc ? tf_last_error() : "ok");
    /* the partial file's bytes are not the file's: caught by the hash at the end, the file fetched whole once more */
    lanes_reset(4);
    rmrf(dest);
    tf_mkdirs(cdn);
    lb[1] ^= 1;
    tf_write_file(part, (const char *)lb, ln / 2);
    d = lanes_opts(dest);
    const int rc2 = tf__download_model(&d);
    CHECK(rc2 == 0 && g_l.per_layers == 2 && tf__download_rereads == 0 && lanes_has(dest, "cdn/layers.zfw.qle", "layers.zfw.qle"),
          "resume: a partial file of other bytes is caught at the end and the file fetched whole again (%d asks, %s)", g_l.per_layers,
          rc2 ? tf_last_error() : "ok");
    free(lb);
    free(part);
    free(cdn);
  }
  /* 3. a connection that ends half way: resumed with Range inside the call, beside the other lanes */
  {
    lanes_reset(4);
    g_l.cut_layers = 1;
    snprintf(dest, sizeof dest, "%s/cut", work);
    rmrf(dest);
    TfDownload d = lanes_opts(dest);
    const int rc = tf__download_model(&d);
    CHECK(rc == 0 && g_l.per_layers == 2 && g_l.ranged == 1 && lanes_has(dest, "cdn/layers.zfw.qle", "layers.zfw.qle"),
          "cut: the file resumed with Range (%d asks, %d ranged, %s)", g_l.per_layers, g_l.ranged, rc ? tf_last_error() : "ok");
  }
  /* 4. a file that is never the signed bytes: checksum_mismatch after its second whole fetch, the other lanes stop */
  {
    lanes_reset(4);
    g_l.bad_frontend = 1;
    snprintf(dest, sizeof dest, "%s/mismatch", work);
    rmrf(dest);
    TfDownload d = lanes_opts(dest);
    const int rc = tf__download_model(&d);
    CHECK(rc == TF_ERR_CHECKSUM_MISMATCH && g_l.per_frontend == 2 && tf_last_error() && strstr(tf_last_error(), "frontend.zfw.qle"),
          "mismatch: checksum_mismatch naming the file after two whole fetches (%s: %s, %d fetches)", tf_error_name(rc), tf_last_error(),
          g_l.per_frontend);
  }
  /* 5. a file whose answer stalls every time: download_failed after its fifth resume, as one lane had it */
  {
    lanes_reset(4);
    g_l.stall_layers = 1;
    snprintf(dest, sizeof dest, "%s/stall", work);
    rmrf(dest);
    TfDownload d = lanes_opts(dest);
    const int rc = tf__download_model(&d);
    CHECK(rc == TF_ERR_DOWNLOAD_FAILED && g_l.per_layers == 6, "stall: download_failed after the fifth resume (%s, %d asks)", tf_error_name(rc),
          g_l.per_layers);
  }
  /* 6. with a lease hook: the files come from the build's folder while the hook runs; installs only after it */
  {
    lanes_reset(4);
    g_l.delay_ms = 5;
    g_lanes_hook_base = host_folder;
    snprintf(dest, sizeof dest, "%s/hook", work);
    rmrf(dest);
    TfDownload d = lanes_opts(dest);
    d.after_manifest = lanes_hook;
    d.install = lanes_install;
    const int rc = tf__download_model(&d);
    CHECK(rc == 0 && g_l.requests == 8 && g_l.during_hook == 8 && g_l.host == 8 && !g_l.lease_base,
          "hook: the eight files from the build's folder while the lease hook ran (%d of %d during it, %s)", g_l.during_hook, g_l.requests,
          rc ? tf_last_error() : "ok");
    CHECK(g_l.installs == 6 && !g_l.installs_before_hook, "hook: six containers installed, none before the hook returned (%d, %d before)", g_l.installs,
          g_l.installs_before_hook);
  }
  /* 7. a hook that fails (no lease): no file of this call is left */
  {
    lanes_reset(4);
    g_l.delay_ms = 5;
    g_lanes_hook_base = NULL;
    snprintf(dest, sizeof dest, "%s/refused", work);
    rmrf(dest);
    TfDownload d = lanes_opts(dest);
    d.after_manifest = lanes_hook;
    d.install = lanes_install;
    const int rc = tf__download_model(&d);
    CHECK(rc == TF_ERR_SERVER_REJECTED && g_l.during_hook > 0 && !g_l.installs && lanes_left(dest) == 0,
          "no lease: the hook's error, nothing installed, and none of the %d files fetched meanwhile left (%s, %d left)", g_l.during_hook,
          tf_error_name(rc), lanes_left(dest));
  }
  /* 8. a lease answer that names another folder: what came from the host goes, the files come from that folder */
  {
    lanes_reset(4);
    g_l.delay_ms = 5;
    g_lanes_hook_base = lease_folder;
    snprintf(dest, sizeof dest, "%s/moved", work);
    rmrf(dest);
    TfDownload d = lanes_opts(dest);
    d.after_manifest = lanes_hook;
    const int rc = tf__download_model(&d);
    CHECK(rc == 0 && g_l.host > 0 && g_l.lease_base == 8 && lanes_has(dest, "cdn/layers.zfw.qle", "layers.zfw.qle"),
          "moved: the files fetched again from the lease answer's folder (%d from the host first, %d from the folder, %s)", g_l.host, g_l.lease_base,
          rc ? tf_last_error() : "ok");
  }
  /* 4.0.1 (ENDPOINTS_40.md 28.4): the first models host gives the manifest, then no answer to any file: each of the
   * four lanes that asks it resumes on the next host, which becomes P, so the files after go there first; every file
   * comes once from the second host, the signed bytes, and the call succeeds */
  {
    lanes_reset(4);
    g_l.delay_ms = 50;
    g_l.down_host = "cdn.quranlab.ai";
    snprintf(dest, sizeof dest, "%s/failover", work);
    rmrf(dest);
    TfDownload d = lanes_opts(dest);
    const int rc = tf__download_model(&d);
    CHECK(rc == 0 && g_l.down_asked >= 1 && g_l.down_asked <= 4 && g_l.other_hosts == 8 && g_l.requests == 8 &&
              lanes_has(dest, "cdn/layers.zfw.qle", "layers.zfw.qle") && lanes_has(dest, "cdn/c16_l64/frames.zfw.qle", "c16_l64/frames.zfw.qle"),
          "failover: the first models host down for files: %d asks there (at most one a lane), %d on the next host, %d files served (%s)", g_l.down_asked,
          g_l.other_hosts, g_l.requests, rc ? tf_last_error() : "ok");
    printf("  failover across lanes: %d file requests to the host that gave no answer, %d to the next host\n", g_l.down_asked, g_l.other_hosts);
  }
  tf__download_lanes = 4;
  tf__download_pause_ms = 1000;
  tf_http_set_transport(NULL, NULL);
  tf_json_free(fx);
  tf_mutex_destroy(&g_l_lock);
  return done("lanes");
}
