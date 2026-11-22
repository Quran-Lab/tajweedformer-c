/* SPEC 16.7: one request per model file. Included by tf40_tests.c.
 *
 *   tf40_tests requests <fixture> <work>
 *
 * Against the stand-in server (make_flow40.py's build, the lease answer naming the build's own folder on the model host
 * as production's does): a full install counts the model-file requests and their Range headers (one GET per file, no
 * Range); the same with the transfer of the largest file cut once: one resume, from the cut, open-ended, with If-Range
 * where it goes to the host that gave the validator (ENDPOINTS_40.md 28.4: a cut is unreachable, so the resume goes to
 * the next model host, with the Range alone, when the role has more than one); and a partial file an earlier call left
 * (the same files as 4.0.1 leaves) completed from its last byte.
 *
 *   tf40_tests hosts <nowait40> <test/data/sdk_4.0.0> <work>
 *
 * STEERING_40.md 34.6 (4.0.2): the per-host counts of the day, from a stand-in with known timings on the calling thread's
 * clock (whole; failed, resumed and failover), read as a delta carries them (the switch on, as 4.0.2 ships); with it
 * off a delta has none and the day's state keeps them; steering_40.json telemetry's 11 bodies through the server's
 * check. */

static void rq_forget(void) {
  tf_mutex_lock(&g_f_lock);
  g_f.n_req = 0;
  tf_mutex_unlock(&g_f_lock);
}
static int rq_file(const char *u) {
  const char *r = strstr(u, "/sdk/v4.0/");
  return r && strcmp(r + 10, "manifest.json") != 0;
}
/* the files of the vector build a C install fetches: required, every platform and SoC, the shared and the default
 * context's */
static int rq_wanted(const TfJson *fx) {
  const char *mt = S(fx, "manifest");
  TfJson *m = tf_json_parse(mt, strlen(mt), NULL);
  const TfJson *files = tf_json_get(m, "files");
  int n = 0;
  for (int i = 0; files && i < files->n; ++i) {
    const TfJson *f = &files->kids[i], *pl = tf_json_get(f, "platforms");
    const char *need = tf_json_str(tf_json_get(f, "need")), *cx = tf_json_str(tf_json_get(f, "context")), *soc = tf_json_str(tf_json_get(f, "soc"));
    n += need && !strcmp(need, "required") && pl && pl->type == TJ_STR && !strcmp(pl->str, "*") && soc && !strcmp(soc, "*") && cx &&
         (!strcmp(cx, "*") || !strcmp(cx, "c16_l64"));
  }
  tf_json_free(m);
  return n;
}
typedef struct {
  int files, distinct, ranged, if_range, manifests, cut_file, resume_ifr, resume_same_host;
  long long resume_from;
} RqCount;
static void rq_count(const char *cut, RqCount *k) {
  memset(k, 0, sizeof *k);
  tf_mutex_lock(&g_f_lock);
  char first_host[200] = "";
  for (int i = 0; i < g_f.n_req; ++i) {
    const char *u = g_f.req[i];
    if (strstr(u, "/sdk/v4.0/manifest.json")) k->manifests++;
    if (!rq_file(u)) continue;
    k->files++;
    k->ranged += g_f.req_from[i] > 0;
    k->if_range += g_f.req_ifr[i];
    int seen = 0;
    for (int j = 0; j < i && !seen; ++j) seen = rq_file(g_f.req[j]) && !strcmp(strstr(g_f.req[j], "/sdk/v4.0/"), strstr(u, "/sdk/v4.0/"));
    k->distinct += !seen;
    if (cut && strstr(u, cut)) {
      const char *h = strstr(u, "://") + 3;
      const size_t hn = strcspn(h, "/");
      if (!k->cut_file++) snprintf(first_host, sizeof first_host, "%.*s", (int)hn, h);
      else if (g_f.req_from[i] > 0) {
        k->resume_from = g_f.req_from[i];
        k->resume_ifr = g_f.req_ifr[i];
        k->resume_same_host = strlen(first_host) == hn && !strncmp(first_host, h, hn);
      }
    }
  }
  tf_mutex_unlock(&g_f_lock);
}
static long long rq_size(const TfJson *fx, const char *rel) {
  const char *hex = S(tf_json_get(fx, "files"), rel);
  return hex ? (long long)strlen(hex) / 2 : 0;
}

static int t_requests(const char *fixture, const char *work) {
  TfJson *fx = flow_setup(fixture);
  if (!fx) return 2;
  tf__download_pause_ms = 1;
  int want = rq_wanted(fx);
  const long long big = rq_size(fx, "layers.zfw.qle");
  char w[1100];
  RqCount k;
  /* a full install: one GET per file, no Range, plus the manifest */
  {
    snprintf(w, sizeof w, "%s/full", work);
    fake_reset(LEASE_HOST_BASE);
    TfClient *c = flow_client(w);
    const int rc = c ? tf_client_prefetch(c, NULL) : -1;
    rq_count(NULL, &k);
    if (getenv("TF_REQUESTS_PRINT"))
      for (int i = 0; i < g_f.n_req; ++i) fprintf(stderr, "  %s%s\n", g_f.req[i], g_f.req_from[i] ? " (ranged)" : "");
    want = k.distinct; /* every file the install fetched; each must be asked once */
    CHECK(rc == 0 && k.files == want && k.distinct == want && want >= rq_wanted(fx) && k.ranged == 0 && k.manifests == 1,
          "full install: %d model-file requests for %d files (want %d), %d with Range, %d manifest (%s)", k.files, k.distinct, want, k.ranged,
          k.manifests, tf_error_name(rc));
    printf("  full install: %d model-file requests for %d files, %d with a Range, plus %d manifest\n", k.files, k.distinct, k.ranged, k.manifests);
    if (c) tf_client_destroy(c);
  }
  /* the largest file's transfer cut once, half way: one resume, from the cut, open-ended */
  {
    snprintf(w, sizeof w, "%s/cut", work);
    fake_reset(LEASE_HOST_BASE);
    g_f.cut_url = "/layers.zfw.qle", g_f.cut_n = 1;
    TfClient *c = flow_client(w);
    const int rc = c ? tf_client_prefetch(c, NULL) : -1;
    rq_count("/layers.zfw.qle", &k);
    CHECK(rc == 0 && k.files == want + 1 && k.distinct == want && k.cut_file == 2 && k.ranged == 1 && k.resume_from == big / 2 &&
              k.resume_ifr == k.resume_same_host,
          "one cut: %d model-file requests (want %d), the largest file asked %d times, %d with Range, resumed from %lld of %lld, If-Range %d, "
          "same host %d (%s)",
          k.files, want + 1, k.cut_file, k.ranged, k.resume_from, big, k.resume_ifr, k.resume_same_host, tf_error_name(rc));
    printf("  one cut in the largest file: %d model-file requests, one resume from byte %lld with %s on the %s host\n", k.files, k.resume_from,
           k.resume_ifr ? "If-Range" : "the Range alone", k.resume_same_host ? "same" : "next");
    if (c) tf_client_destroy(c);
  }
  /* the same with one model host: the resume goes to the host that gave the validator, with If-Range */
  {
    snprintf(w, sizeof w, "%s/cut_one_host", work);
    fake_reset(LEASE_HOST_BASE);
    g_f.cfg = "config_one_models"; /* the signed config's endpoints: one model host (28.2) */
    g_f.cut_url = "/layers.zfw.qle", g_f.cut_n = 1;
    TfClient *c = flow_client(w);
    rq_forget();
    const int rc = c ? tf_client_prefetch(c, NULL) : -1;
    rq_count("/layers.zfw.qle", &k);
    CHECK(rc == 0 && k.files == want + 1 && k.cut_file == 2 && k.ranged == 1 && k.resume_from == big / 2 && k.resume_ifr && k.resume_same_host,
          "one cut, one model host: %d model-file requests (want %d), resumed from %lld with If-Range %d on the same host %d (%s)", k.files, want + 1,
          k.resume_from, k.resume_ifr, k.resume_same_host, tf_error_name(rc));
    printf("  one cut, one model host: %d model-file requests, one resume from byte %lld with %s\n", k.files, k.resume_from,
           k.resume_ifr ? "If-Range" : "no If-Range");
    if (c) tf_client_destroy(c);
  }
  /* a partial file an earlier call left (the largest, cut at every attempt of that call) is completed by the next call
   * from its last byte: no byte is asked twice */
  {
    snprintf(w, sizeof w, "%s/partial", work);
    fake_reset(LEASE_HOST_BASE);
    g_f.cut_url = "/layers.zfw.qle", g_f.cut_n = 100;
    TfClient *c = flow_client(w);
    const int rc1 = c ? tf_client_prefetch(c, NULL) : -1;
    char part[1400];
    snprintf(part, sizeof part, "%s/models/%s/cdn/layers.zfw.qle.part", w, S(fx, "build"));
    FILE *pf = fopen(part, "rb");
    long long have = 0;
    if (pf) {
      fseek(pf, 0, SEEK_END);
      have = ftell(pf);
      fclose(pf);
    }
    g_f.cut_url = NULL, g_f.cut_n = 0;
    rq_forget();
    const int rc2 = c ? tf_client_prefetch(c, NULL) : -1;
    rq_count("/layers.zfw.qle", &k);
    CHECK(rc1 == TF_ERR_DOWNLOAD_FAILED && have > 0 && have < big && rc2 == 0 && k.cut_file == 1 && k.ranged >= 1, "partial: a call left %lld of %lld "
                                                                                                              "bytes (%s); the next asks the rest once "
                                                                                                              "(%d asks, %d ranged) and is done (%s)",
          have, big, tf_error_name(rc1), k.cut_file, k.ranged, tf_error_name(rc2));
    int from_have = 0;
    tf_mutex_lock(&g_f_lock);
    for (int i = 0; i < g_f.n_req; ++i)
      if (strstr(g_f.req[i], "/layers.zfw.qle")) from_have = g_f.req_from[i] == have;
    tf_mutex_unlock(&g_f_lock);
    CHECK(from_have, "partial: the rest asked from byte %lld, the partial file's end", have);
    printf("  partial file of %lld bytes: completed with %d request(s) from its end\n", have, k.cut_file);
    if (c) tf_client_destroy(c);
  }
  /* ENDPOINTS_40.md 28.11 item 2: on the models role a Retry-After hold is per file path and host.
   * One file answered 503 with Retry-After 3600 on the first model host (a standby's answer for a file it does not
   * hold) is asked of the next host; the next download asks every other file of the first host, and that one file of
   * the next host only */
  {
    snprintf(w, sizeof w, "%s/hold_path", work);
    fake_reset(LEASE_HOST_BASE);
    /* the held file is the install's smallest, asked last (the lanes take the largest first), so the next host it is
     * taken on becomes P (28.3 item 5) only after every other file has been asked */
    char held[600], small[200] = "";
    {
      const char *mt = S(fx, "manifest");
      TfJson *m = tf_json_parse(mt, strlen(mt), NULL);
      const TfJson *files = tf_json_get(m, "files");
      double best = 1e18;
      for (int i = 0; files && i < files->n; ++i) {
        const TfJson *f = &files->kids[i], *pl = tf_json_get(f, "platforms");
        const char *need = tf_json_str(tf_json_get(f, "need")), *cx = tf_json_str(tf_json_get(f, "context")), *path = tf_json_str(tf_json_get(f, "path"));
        const int wanted = need && !strcmp(need, "required") && pl && pl->type == TJ_STR && !strcmp(pl->str, "*") && cx &&
                           (!strcmp(cx, "*") || !strcmp(cx, "c16_l64"));
        if (wanted && path && tf_json_get(f, "size")->num < best) {
          best = tf_json_get(f, "size")->num;
          snprintf(small, sizeof small, "/%s", path);
        }
      }
      tf_json_free(m);
    }
    snprintf(held, sizeof held, "https://cdn.quranlab.ai/models/Quran-Lab/tajweedformer-v4/%s/sdk/v4.0%s", S(fx, "build"), small);
    g_f.fail_url = held, g_f.fail_status = 503, g_f.fail_body = "{\"error\":\"not_held\"}", g_f.retry_after = 3600;
    const long long t0 = (long long)time(NULL);
    tf__ep_now_override = (double)t0; /* the holds' and 28.3's clock */
    TfClient *c = flow_client(w);
    const int rc1 = c ? tf_client_prefetch(c, NULL) : -1;
    /* the models go, the holds stay (this Client's, and <storage>/holds.json); the same build again */
    const int rm = c ? tf_client_remove_models(c) : -1;
    rq_forget();
    /* 31 minutes on: the first host is asked first again (28.3 item 5's probe back; the held file's success on the
     * next host made that one P), and the hold of 3600 s stands */
    tf__ep_now_override = (double)(t0 + 1900);
    const int rc2 = c ? tf_client_prefetch(c, NULL) : -1;
    tf__ep_now_override = 0;
    int first_other = 0, other_held = 0, first_held = 0, next_held = 0;
    tf_mutex_lock(&g_f_lock);
    for (int i = 0; i < g_f.n_req; ++i) {
      const char *u = g_f.req[i];
      if (!rq_file(u)) continue;
      const int on_first = !strncmp(u, "https://cdn.quranlab.ai/", 24), is_held = strstr(u, small) != NULL;
      if (is_held) on_first ? first_held++ : next_held++;
      else on_first ? first_other++ : other_held++;
    }
    tf_mutex_unlock(&g_f_lock);
    CHECK(rc1 == 0 && rm == 0 && rc2 == 0 && first_held == 0 && next_held == 1 && first_other >= 7 && other_held == 0,
          "hold per path: the held file asked %d of the first host and %d of the next; the other files %d of the first host, %d elsewhere (%s, %s)",
          first_held, next_held, first_other, other_held, tf_error_name(rc1), tf_error_name(rc2));
    printf("  a 503 Retry-After 3600 on one file of the first model host: the next download asks %d other files there and that one "
           "of the next host\n", first_other);
    g_f.fail_url = NULL;
    if (c) tf_client_destroy(c);
  }
  tf__download_pause_ms = 1000;
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(fx);
  return done("requests");
}

/* ------------------------------------------------------------------ STEERING_40.md 34.6 (4.0.2): per-host counts */
/* the day's `hosts` as a delta would carry them with the switch on: the entry of a host, or NULL */
static const TfJson *hc_entry(const TfJson *hosts, const char *host) {
  for (int i = 0; hosts && i < hosts->n; ++i)
    if (!strcmp(S(&hosts->kids[i], "host") ? S(&hosts->kids[i], "host") : "", host)) return &hosts->kids[i];
  return NULL;
}
static long long hc_sum(const TfJson *e, const char *list) {
  const TfJson *l = tf_json_get(e, list);
  long long s = 0;
  for (int i = 0; l && i < l->n; ++i) s += (long long)l->kids[i].num;
  return s;
}
static long long hc_at(const TfJson *e, const char *list, int i) {
  const TfJson *l = tf_json_get(e, list);
  return l && i < l->n ? (long long)l->kids[i].num : -1;
}
static TfJson *hc_hosts(TfClient *c, TfJson **body) {
  char *t = c ? tf__client_day_body(c) : NULL;
  *body = t ? tf_json_parse(t, strlen(t), NULL) : NULL;
  free(t);
  return (TfJson *)tf_json_get(*body, "hosts");
}
static int t_hosts(const char *nowait, const char *vec, const char *work) {
  TfJson *v = vec ? load_json(vec, "steering_40.json") : NULL;
  const TfJson *tel = tf_json_get(v, "telemetry"), *cases = tf_json_get(tel, "cases");
  /* the vectors: the built body passes the server's check exactly where an accepted case does */
  int ok = 0;
  for (int i = 0; cases && i < cases->n; ++i) {
    const TfJson *k = &cases->kids[i];
    const int accepted = tf_json_get(k, "accepted")->boolean, got = tf__hosts_problem(tf_json_get(k, "hosts")) == NULL;
    CHECK(got == accepted, "hosts vectors: '%s' %s, want %s (%s)", S(k, "name"), got ? "passes" : "refused", accepted ? "passes" : "refused",
          tf__hosts_problem(tf_json_get(k, "hosts")) ? tf__hosts_problem(tf_json_get(k, "hosts")) : "-");
    ok += got == accepted;
  }
  CHECK(cases && cases->n == 11 && ok == 11, "hosts vectors: %d of %d", ok, cases ? cases->n : 0);
  const TfJson *tb = tf_json_get(tel, "ttfb_buckets_ms"), *kb = tf_json_get(tel, "kbps_buckets");
  CHECK(tb && tb->n == 8 && kb && kb->n == 9 && tb->kids[0].num == 100 && tb->kids[7].num == 10000 && kb->kids[0].num == 32 && kb->kids[8].num == 8192,
        "hosts vectors: the buckets' edges are 34.6's");
  tf_json_free(v);
  TfJson *fx = flow_setup(nowait);
  if (!fx) return 2;
  tf__download_pause_ms = 1;
  tf__dl_clock = fake_clock;
  tf__telemetry_hosts = 1; /* on, as 4.0.2 ships: `hosts` read as a delta carries it */
  char w[1100];
  const long long layers = rq_size(fx, "layers.zfw.qle");
  /* whole: every file and the manifest from the first host, the status 300 ms after each request, the bodies at
   * 400 kB/s: one bucket of ttfb_ms ([250, 500)) for all of them, one rate ([256, 512) kB/s) for the one transfer of
   * at least 256 KiB */
  {
    snprintf(w, sizeof w, "%s/hosts_whole", work);
    fake_reset(LEASE_HOST_BASE);
    g_f.ttfb_ms = 300, g_f.rate_kBps = 400;
    TfClient *c = flow_client(w);
    rq_forget();
    const int rc = c ? tf_client_prefetch(c, NULL) : -1;
    RqCount k;
    rq_count(NULL, &k);
    long long bytes = 0;
    tf_mutex_lock(&g_f_lock);
    for (int i = 0; i < g_f.n_req; ++i) {
      const char *r = strstr(g_f.req[i], "/sdk/v4.0/");
      bytes += !r ? 0 : !strcmp(r + 10, "manifest.json") ? (long long)strlen(S(fx, "manifest")) : rq_size(fx, r + 10);
    }
    tf_mutex_unlock(&g_f_lock);
    TfJson *body = NULL;
    const TfJson *hs = hc_hosts(c, &body), *e = hc_entry(hs, "cdn.quranlab.ai");
    /* contract 0d9f5d7: the manifest is one of `files` on the host it came from */
    CHECK(rc == 0 && hs && hs->n == 1 && e && N(e, "files") == k.files + 1 && N(e, "requests") == k.files + 1 && N(e, "unreachable") == 0 &&
              N(e, "bytes") == bytes && hc_at(e, "ttfb_ms", 2) == k.files + 1 && hc_sum(e, "ttfb_ms") == k.files + 1 && hc_at(e, "kbps", 4) == 1 &&
              hc_sum(e, "kbps") == 1 && N(e, "hedged") == 0 && N(e, "slow_switches") == 0 && layers >= 256 * 1024 && tf__hosts_problem(hs) == NULL,
          "hosts, whole: %d host(s); files %lld (want %d), requests %lld (want %d), bytes %lld (want %lld), ttfb[2] %lld, kbps[4] %lld (%s)",
          hs ? hs->n : -1, e ? N(e, "files") : -1, k.files + 1, e ? N(e, "requests") : -1, k.files + 1, e ? N(e, "bytes") : -1, bytes,
          e ? hc_at(e, "ttfb_ms", 2) : -1, e ? hc_at(e, "kbps", 4) : -1, tf_error_name(rc));
    tf_json_free(body);
    /* the switch off (4.0.2 ships it on): a delta carries no `hosts`; the day's state on disk keeps them */
    tf__telemetry_hosts = 0;
    hs = hc_hosts(c, &body);
    char day[1300], today[11];
    tf__day_str((tf__now_override ? tf__now_override : (long long)time(NULL)) / 86400, today);
    snprintf(day, sizeof day, "%s/telemetry-%s.json", w, today);
    TfJson *kept = load_json(w, day + strlen(w) + 1);
    CHECK(!hs && tf_json_get(kept, "hosts") && tf_json_get(kept, "hosts")->n == 1, "hosts, switch off: a delta has none (%s), the day kept them (%s)",
          hs ? "has" : "none", tf_json_get(kept, "hosts") ? "kept" : "not kept");
    tf_json_free(kept);
    tf_json_free(body);
    tf__telemetry_hosts = 1;
    if (c) tf_client_destroy(c);
  }
  /* failed, resumed and failover: the first host gives no answer for the frontend (no status), cuts the weights half
   * way and answers 503 for the tokens; each goes on at the next host */
  {
    snprintf(w, sizeof w, "%s/hosts_failover", work);
    fake_reset(LEASE_HOST_BASE);
    g_f.ttfb_ms = 300, g_f.rate_kBps = 400;
    char nf[600], fail503[600];
    snprintf(nf, sizeof nf, "https://cdn.quranlab.ai/models/Quran-Lab/tajweedformer-v4/%s/sdk/v4.0/frontend.zfw.qle", S(fx, "build"));
    snprintf(fail503, sizeof fail503, "https://cdn.quranlab.ai/models/Quran-Lab/tajweedformer-v4/%s/sdk/v4.0/tokens.txt.qle", S(fx, "build"));
    g_f.fail2_url = nf, g_f.fail2_status = 0;
    g_f.fail_url = fail503, g_f.fail_status = 503;
    TfClient *c = flow_client(w);
    char cut[600];
    snprintf(cut, sizeof cut, "https://cdn.quranlab.ai/models/Quran-Lab/tajweedformer-v4/%s/sdk/v4.0/layers.zfw.qle", S(fx, "build"));
    g_f.cut_url = cut, g_f.cut_n = 1;
    rq_forget();
    const int rc = c ? tf_client_prefetch(c, NULL) : -1;
    /* what the stand-in took: model-file requests per host (28.3 item 5: once a file is taken on the next host, the
     * files after it start there), and the distinct files */
    /* a file started after P moved never meets its failure on the first host: what failed there is read from the log
     * (each failing file once there: the no answer and the 503 every time, the cut once) */
    int f0 = 0, f1 = 0, other = 0, no_answer0 = 0, fail0 = 0, cut0 = 0;
    RqCount k;
    rq_count(NULL, &k);
    tf_mutex_lock(&g_f_lock);
    for (int i = 0; i < g_f.n_req; ++i) {
      if (!rq_file(g_f.req[i])) continue;
      if (!strncmp(g_f.req[i], "https://cdn.quranlab.ai/", 24)) {
        f0++;
        no_answer0 += !strcmp(g_f.req[i], nf);
        fail0 += !strcmp(g_f.req[i], fail503);
        cut0 += !strcmp(g_f.req[i], cut);
      } else if (!strncmp(g_f.req[i], "https://cdn-b.tajweedformer.com/", 32)) {
        f1++;
      } else {
        other++;
      }
    }
    tf_mutex_unlock(&g_f_lock);
    const int unr0 = no_answer0 + fail0 + (cut0 > 0);
    TfJson *body = NULL;
    const TfJson *hs = hc_hosts(c, &body), *e0 = hc_entry(hs, "cdn.quranlab.ai"), *e1 = hc_entry(hs, "cdn-b.tajweedformer.com");
    CHECK(rc == 0 && other == 0 && unr0 >= 1 && hs && hs->n == 2 && e0 && e1 && N(e0, "requests") == f0 + 1 && N(e0, "unreachable") == unr0 &&
              N(e0, "files") == f0 - unr0 + 1 && hc_sum(e0, "ttfb_ms") == f0 + 1 - no_answer0 && N(e1, "requests") == f1 && N(e1, "unreachable") == 0 &&
              N(e1, "files") == f1 && hc_sum(e1, "ttfb_ms") == f1 && N(e0, "files") + N(e1, "files") == k.distinct + 1 && f1 >= unr0 &&
              hc_sum(e0, "kbps") == 0 && hc_sum(e1, "kbps") == 0 && tf__hosts_problem(hs) == NULL,
          "hosts, failed, resumed and failover: first host requests %lld (want %d), unreachable %lld (want %d), files %lld (want %d), ttfb %lld "
          "(want %d); next host requests %lld, files %lld, ttfb %lld (want %d each); %d files (%s)",
          e0 ? N(e0, "requests") : -1, f0 + 1, e0 ? N(e0, "unreachable") : -1, unr0, e0 ? N(e0, "files") : -1, f0 - unr0 + 1,
          e0 ? hc_sum(e0, "ttfb_ms") : -1, f0 + 1 - no_answer0, e1 ? N(e1, "requests") : -1, e1 ? N(e1, "files") : -1, e1 ? hc_sum(e1, "ttfb_ms") : -1, f1,
          k.distinct, tf_error_name(rc));
    printf("  hosts: first host %lld requests, %lld unreachable, %lld files; next host %lld requests, %lld files\n", e0 ? N(e0, "requests") : -1,
           e0 ? N(e0, "unreachable") : -1, e0 ? N(e0, "files") : -1, e1 ? N(e1, "requests") : -1, e1 ? N(e1, "files") : -1);
    tf_json_free(body);
    g_f.fail_url = NULL, g_f.fail2_url = NULL, g_f.cut_url = NULL;
    if (c) tf_client_destroy(c);
  }
  /* contract ec46782: a cut transfer of 256 KiB or more counts in kbps at its rate over what arrived (90% of the
   * 320 KiB weights at 400 kB/s: [256, 512)); its 10% resume does not */
  {
    snprintf(w, sizeof w, "%s/hosts_cut_rate", work);
    fake_reset(LEASE_HOST_BASE);
    g_f.ttfb_ms = 300, g_f.rate_kBps = 400;
    char cut[600];
    snprintf(cut, sizeof cut, "https://cdn.quranlab.ai/models/Quran-Lab/tajweedformer-v4/%s/sdk/v4.0/layers.zfw.qle", S(fx, "build"));
    g_f.cut_url = cut, g_f.cut_n = 1, g_f.cut_pct = 90;
    TfClient *c = flow_client(w);
    const int rc = c ? tf_client_prefetch(c, NULL) : -1;
    TfJson *body = NULL;
    const TfJson *hs = hc_hosts(c, &body), *e0 = hc_entry(hs, "cdn.quranlab.ai"), *e1 = hc_entry(hs, "cdn-b.tajweedformer.com");
    CHECK(rc == 0 && e0 && e1 && hc_at(e0, "kbps", 4) == 1 && hc_sum(e0, "kbps") == 1 && hc_sum(e1, "kbps") == 0 && N(e0, "unreachable") == 1,
          "hosts, a cut of 288 KiB: its rate counted on the first host (%lld in [256, 512)), the resume's not (%lld) (%s)", e0 ? hc_at(e0, "kbps", 4) : -1,
          e1 ? hc_sum(e1, "kbps") : -1, tf_error_name(rc));
    tf_json_free(body);
    g_f.cut_url = NULL, g_f.cut_pct = 0;
    if (c) tf_client_destroy(c);
  }
  tf__dl_clock = NULL;
  tf__telemetry_hosts = 1; /* as it ships */
  tf__download_pause_ms = 1000;
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(fx);
  return done("hosts");
}
