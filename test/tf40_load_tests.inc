/* SDK 4.0.2 under load (the Ramadan capacity model and the shedding rules, 2026-11-21), included by tf40_tests.c after
 * tf40_nowait_tests.inc.
 *
 *   tf40_tests load <fixture> <work>
 *
 * Contract (sdk-4.0.2 dd76474) PROTECTED_MODEL.md 18.4.7.3 item 6 (the lease's retries: 600 to 1800 s drawn afresh, a
 * Retry-After as the floor, kept across restarts) and ENDPOINTS_40.md 28.11 item 6 (every route honours Retry-After,
 * kept across restarts; events after a 429), on fixed clocks: tf__now_override (the SDK's own schedules),
 * tf__ep_now_override (the holds') and the platform's boot clock (the Core's). The uploads case of 28.11 item 6 is
 * tf_tests upload's case N. */

static void load_clock(long long t, long long iat, long long b0) {
  tf__now_override = t;
  tf__ep_now_override = (double)t;
  tf__plat_test_boot_ms = b0 + (t - iat) * 1000;
}
static void load_at(long long t) {
  tf__now_override = t;
  tf__ep_now_override = (double)t;
}
static void load_reset(void) {
  tf__now_override = 0;
  tf__ep_now_override = 0;
}
static const char kLeaseUnavailable[] = "{\"error\":\"lease_unavailable\",\"message\":\"the lease service is unavailable\"}";
static const char kRateLimited[] = "{\"error\":\"rate_limited\",\"message\":\"too many requests, retry later\"}";
/* the requests whose URL ends with `path` (before any query), on the first host of its role and on any other */
static void load_hosts(const char *path, int *first, int *others) {
  *first = *others = 0;
  const size_t pn = strlen(path);
  tf_mutex_lock(&g_f_lock);
  for (int i = 0; i < g_f.n_req; ++i) {
    const char *u = g_f.req[i];
    const size_t un = strcspn(u, "?");
    if (un < pn || strncmp(u + un - pn, path, pn)) continue;
    if (!strncmp(u, "https://api.quranlab.ai/", 24) || !strncmp(u, "https://cdn.quranlab.ai/", 24)) (*first)++;
    else (*others)++;
  }
  tf_mutex_unlock(&g_f_lock);
}
static int load_asked(const char *path) {
  int f = 0, o = 0;
  load_hosts(path, &f, &o);
  return f + o;
}

/* 28.11 item 6: one route's 429 with Retry-After 90; 30 s on, nothing of that route; 90 s on, one; no other host ever.
 * how: 0 a model call, 1 a send point, 2 register, 3 an error report's send (one queued first). */
static void load_route_hold(const char *work, const char *label, const char *fail, const char *count, int how, int restart) {
  char w[1100];
  snprintf(w, sizeof w, "%s/hold_%s%s", work, label, restart ? "_restart" : "");
  const long long t = (long long)time(NULL);
  load_at(t);
  fake_reset(LEASE_OK);
  TfClient *c = flow_client(w);
  if (how == 3 && c) {
    g_f.fail_url = "/v1/config", g_f.fail_status = 503;
    tf_client_model(c, NULL);
  }
  int asked[3] = {0, 0, 0}, first = 0, others = 0;
  const long long at[3] = {0, restart ? 80 : 30, 90}; /* the restart's new process starts 10 s on (below) */
  for (int k = 0; k < 3 && c; ++k) {
    int before;
    if (k == 1 && restart) {
      /* the process ends 10 s after the 429 and a new one starts on the same folder: its end (a send point) and the new
       * launch count too */
      load_at(t + 10);
      before = load_asked(count);
      tf_client_destroy(c);
      c = flow_client_in(w);
      if (!c) break;
      load_at(t + at[k]);
    } else {
      load_at(t + at[k]);
      before = load_asked(count);
    }
    if (k == 0) g_f.fail_url = fail, g_f.fail_status = 429, g_f.fail_body = kRateLimited, g_f.retry_after = 90;
    if (k == 2) g_f.fail_url = NULL; /* the server answers again */
    if (how == 0) {
      TfModel *m = tf_client_model(c, NULL);
      if (m) tf_model_close(m);
    } else if (how == 2) {
      tf__client_register(c);
    } else {
      tf__client_flush(c, 1);
    }
    asked[k] = load_asked(count) - before;
  }
  load_hosts(count, &first, &others);
  /* the token's route: once it is asked again, the download that follows asks a token for each file */
  const int at90 = !strcmp(label, "attest") ? asked[2] >= 1 : asked[2] == 1;
  CHECK(asked[0] == 1 && asked[1] == 0 && at90 && others == 0,
        "429 Retry-After 90 on %s%s: asked %d at 0 s, %d at %lld s, %d at 90 s (want 1, 0, 1); %d of another host", label,
        restart ? " and a restart 10 s on" : "", asked[0], asked[1], at[1], asked[2], others);
  g_f.fail_url = NULL;
  if (c) tf_client_destroy(c);
  load_reset();
}

/* 24.1 item 2 and 28.7 with 28.11's holds: a silent `failover` report says the first host was asked and was
 * unreachable. A first host held by a Retry-After an earlier process kept (holds.json read back at the start) is not
 * asked, so the next host's answer makes no such report; lastFailover and the resume count stay as they were. */
/* a hold kept for the build's file `rel` on the model host `host` (slow: a 429's, else a 503's), and with_config one for
 * the config on the first api host */
static void held_write_on(const char *w, const char *build, const char *rel, const char *host, int slow, int with_config) {
  char p[1200], route[400], body[1400];
  snprintf(route, sizeof route, "/models/Quran-Lab/tajweedformer-v4/%s/sdk/v4.0/%s", build, rel);
  const long long until = (long long)time(NULL) + 3600;
  int n = snprintf(body, sizeof body, "{\"v\":1,\"holds\":[{\"role\":%d,\"slow\":%d,\"route\":\"%s\",\"host\":\"%s\",\"error\":\"%s\",\"until\":%lld}",
                   TF_EP_MODELS, slow, route, host, slow ? "rate_limited" : "", until);
  if (with_config)
    n += snprintf(body + n, sizeof body - (size_t)n, ",{\"role\":%d,\"slow\":0,\"route\":\"/v1/config\",\"host\":\"api.quranlab.ai\",\"error\":\"\",\"until\":%lld}",
                  TF_EP_API, until);
  n += snprintf(body + n, sizeof body - (size_t)n, "],\"rows\":[{\"row\":0,\"until\":0},{\"row\":0,\"until\":0},{\"row\":0,\"until\":0}]}\n");
  rmrf(w);
  tf_mkdirs(w);
  snprintf(p, sizeof p, "%s/holds.json", w);
  tf_write_file(p, body, (size_t)n);
}
static void held_write(const char *w, const char *build, int with_config) {
  held_write_on(w, build, "layers.zfw.qle", "cdn.quranlab.ai", 0, with_config);
}
/* the queued report of `stage` and `reason` (tf__rep_state_json): its fallback as kept (1 false, 2 true) when its
 * host_role is models, 0 when it has none, -1 when there is no such report */
static int held_fallback(const char *st, const char *stage, const char *reason) {
  static const char kRole[] = "\"host_role\":\"models\",\"fallback\":";
  char k[96];
  snprintf(k, sizeof k, "\"stage\":\"%s\",\"reason\":\"%s\",", stage, reason);
  const char *p = st ? strstr(st, k) : NULL;
  if (!p) return -1;
  const char *e = strchr(p, '}'), *f = strstr(p, kRole);
  return f && e && f < e ? atoi(f + sizeof kRole - 1) : 0;
}
static int held_count(const char *needle_host, const char *path) {
  int n = 0;
  tf_mutex_lock(&g_f_lock);
  for (int i = 0; i < g_f.n_req; ++i) n += !strncmp(g_f.req[i], needle_host, strlen(needle_host)) && strstr(g_f.req[i], path) != NULL;
  tf_mutex_unlock(&g_f_lock);
  return n;
}
static void load_held_failover(const TfJson *fx, const char *work) {
  char w[1100];
  /* the weights' path held on the first model host and the config on the first api host; the next hosts answer */
  {
    snprintf(w, sizeof w, "%s/held_failover", work);
    held_write(w, S(fx, "build"), 1);
    fake_reset(LEASE_HOST_BASE);
    TfClient *c = flow_client_in(w);
    const int rc = c ? tf_client_prefetch(c, NULL) : -1;
    char *st = tf__rep_state_json();
    const char *dj = c ? tf_client_diagnose_json(c) : NULL;
    const int reports = st && strstr(st, "\"reason\":\"failover\"") != NULL;
    const int last = dj && strstr(dj, "\"lastFailover\":{") != NULL;
    CHECK(rc == 0 && held_count("https://cdn.quranlab.ai/", "/layers.zfw.qle") == 0 && held_count("https://cdn-b.tajweedformer.com/", "/layers.zfw.qle") == 1 &&
              held_count("https://api.quranlab.ai/", "/v1/config") == 0 && held_count("https://api-b.tajweedformer.com/", "/v1/config") >= 1 && !reports && last,
          "held first hosts: the weights asked %d of the first model host and %d of the next, the config %d of the first api host and %d of the next; "
          "failover reports %s; lastFailover %s (%s)",
          held_count("https://cdn.quranlab.ai/", "/layers.zfw.qle"), held_count("https://cdn-b.tajweedformer.com/", "/layers.zfw.qle"),
          held_count("https://api.quranlab.ai/", "/v1/config"), held_count("https://api-b.tajweedformer.com/", "/v1/config"), reports ? "queued" : "none",
          last ? "kept" : "none", tf_error_name(rc));
    free(st);
    if (c) tf_client_destroy(c);
  }
  /* the resume count: the held host still counts as one of 16.2's five, as before; every other host answers 503: the
   * download fails after five resumes (retries 5), the held host never asked */
  {
    snprintf(w, sizeof w, "%s/held_retries", work);
    held_write(w, S(fx, "build"), 0);
    fake_reset(LEASE_HOST_BASE);
    g_f.fail_url = "/layers.zfw.qle", g_f.fail_status = 503;
    TfClient *c = flow_client_in(w);
    const int rc = c ? tf_client_prefetch(c, NULL) : -1;
    char *st = tf__rep_state_json();
    const char *d = st ? strstr(st, "\"stage\":\"download\",\"reason\":\"http\"") : NULL;
    const char *rt = d ? strstr(d, "\"retries\":") : NULL;
    const int retries = rt ? atoi(rt + 10) : -1;
    /* the other three hosts were asked: the report's fallback is true (ERRORS_40.md 24.2) */
    const int fb = held_fallback(st, "download", "http");
    CHECK(rc == TF_ERR_DOWNLOAD_FAILED && retries == 5 && held_count("https://cdn.quranlab.ai/", "/layers.zfw.qle") == 0 &&
              (!st || !strstr(st, "\"reason\":\"failover\"")) && fb == 2,
          "held first model host, the others 503: %s with retries %d (want 5), the held host asked %d times, fallback %s (want true)",
          tf_error_name(rc), retries, held_count("https://cdn.quranlab.ai/", "/layers.zfw.qle"), fb == 2 ? "true" : fb == 1 ? "false" : "none");
    free(st);
    g_f.fail_url = NULL;
    if (c) tf_client_destroy(c);
  }
  /* 4.0.2 (ERRORS_40.md 24.2 with 28.11's holds): an error report's fallback is true only when the request asked more
   * than one host; a host a Retry-After held was not asked. The first model host held for a file (the weights, then the
   * manifest) and the next host's 429 ending the request: one host asked, fallback false. (4.0.2 before this counted
   * the held host: fallback true.) */
  static const char *const rels[] = {"layers.zfw.qle", "manifest.json"};
  for (int i = 0; i < 2; ++i) {
    const char *rel = rels[i], *stage = i ? "manifest" : "download";
    char path[64];
    snprintf(path, sizeof path, "/%s", rel);
    snprintf(w, sizeof w, "%s/held_fallback_%s", work, stage);
    held_write_on(w, S(fx, "build"), rel, "cdn.quranlab.ai", 0, 0);
    fake_reset(LEASE_HOST_BASE);
    g_f.fail_url = path, g_f.fail_status = 429, g_f.fail_body = kRateLimited;
    TfClient *c = flow_client_in(w);
    const int rc = c ? tf_client_prefetch(c, NULL) : -1;
    char *st = tf__rep_state_json();
    const int fb = held_fallback(st, stage, "rate_limited");
    const int held = held_count("https://cdn.quranlab.ai/", path), next = held_count("https://cdn-b.tajweedformer.com/", path),
              rest = held_count("https://cdn-g.tajweedformer.com/", path) + held_count("https://cdn-a.tajweedformer.com/", path);
    CHECK(rc != 0 && held == 0 && next == 1 && rest == 0 && fb == 1 && (!st || !strstr(st, "\"reason\":\"failover\"")),
          "%s held on the first model host, a 429 on the next: %s; asked %d, %d, %d of the hosts (want 0, 1, 0); fallback %s (want false)", rel,
          tf_error_name(rc), held, next, rest, fb == 2 ? "true" : fb == 1 ? "false" : fb == 0 ? "no host_role" : "no report");
    free(st);
    g_f.fail_url = NULL, g_f.fail_body = NULL;
    if (c) tf_client_destroy(c);
  }
  /* the first host asked and unreachable, the next one held by a 429 (no other host is asked after a 429): one host
   * asked, so fallback false, and no failover report (the request was answered on no other host) */
  {
    char url[400];
    snprintf(url, sizeof url, "https://cdn.quranlab.ai/models/Quran-Lab/tajweedformer-v4/%s/sdk/v4.0/layers.zfw.qle", S(fx, "build"));
    snprintf(w, sizeof w, "%s/held_fallback_next", work);
    held_write_on(w, S(fx, "build"), "layers.zfw.qle", "cdn-b.tajweedformer.com", 1, 0);
    fake_reset(LEASE_HOST_BASE);
    g_f.fail_url = url, g_f.fail_status = 503;
    TfClient *c = flow_client_in(w);
    const int rc = c ? tf_client_prefetch(c, NULL) : -1;
    char *st = tf__rep_state_json();
    const int fb = held_fallback(st, "download", "rate_limited"), reports = st && strstr(st, "\"reason\":\"failover\"") != NULL;
    const int first = held_count("https://cdn.quranlab.ai/", "/layers.zfw.qle"), next = held_count("https://cdn-b.tajweedformer.com/", "/layers.zfw.qle");
    CHECK(rc != 0 && first == 1 && next == 0 && fb == 1 && !reports,
          "the first model host 503, the next held by a 429: %s; asked %d, %d (want 1, 0); fallback %s (want false); failover report %s (want none)",
          tf_error_name(rc), first, next, fb == 2 ? "true" : fb == 1 ? "false" : fb == 0 ? "no host_role" : "no report", reports ? "queued" : "none");
    free(st);
    g_f.fail_url = NULL;
    if (c) tf_client_destroy(c);
  }
}

/* 4.0.2 (PROTECTED_MODEL.md 18.4.7.3, the Client's close): a lease attempt in flight when the Client is destroyed gets up
 * to 3 s to end, so the lease it brings is accepted and kept, and an attempt the close cancels is no failed attempt (the
 * Core is told nothing, no failure row, no wait). The refresh runs on the Client's own timer, as in use; its answer is
 * held, the Client destroyed, and a new Client made offline on the same folder. */
static int close_wait_for(void) {
  for (int i = 0; i < 1000 && !__atomic_load_n(&g_f.holding, __ATOMIC_SEQ_CST); ++i) tf__sleep_ms(10);
  return __atomic_load_n(&g_f.holding, __ATOMIC_SEQ_CST);
}
/* the Client's timer started (the tests run without one), its refresh's model key answer held hold_ms; then destroyed:
 * how long the destroy took */
static double close_during_refresh(TfClient *c, int hold_ms, int *held) {
  g_f.hold_url = "/v1/model-key", g_f.hold_ms = hold_ms;
  tf__client_no_timer = 0;
  tf__client_timer_start(c);
  tf__client_no_timer = 1;
  *held = close_wait_for();
  const double s0 = tf__now_s();
  tf_client_destroy(c);
  const double took = tf__now_s() - s0;
  g_f.hold_url = NULL;
  return took;
}
static TfClient *close_offline(const char *w, TfPm **p) {
  fake_reset(LEASE_OK);
  g_f.fail_url = "https://", g_f.fail_status = 0; /* no answer from anywhere */
  TfClient *c = flow_client_in(w);
  *p = c ? tf__pm(c) : NULL;
  return c;
}
static const char *close_held(int h) { return h == 3 ? "answered" : h == 2 ? "cancelled" : h == 1 ? "waiting" : "never asked"; }
static void load_close(const TfJson *fx, const char *work) {
  const TfJson *l1 = tf_json_get(fx, "lease"), *l7 = tf_json_get(fx, "lease_7d"), *later = tf_json_get(fx, "lease_later");
  const long long iat = N(l1, "iat"), exp = N(l1, "exp"), refresh = N(l1, "refresh_after"), b0 = 3600000;
  const char *mt = S(fx, "manifest"), *build = S(fx, "build");
  TfJson *mj = tf_json_parse(mt, strlen(mt), NULL);
  char w[1100];
  qlc_lease_status ls;
  /* the lease valid, its refresh due: the refresh's answer (a 7-day lease) comes 1 s after the destroy began */
  {
    snprintf(w, sizeof w, "%s/close_due", work);
    load_reset();
    grace_install(w, iat, b0);
    load_clock(refresh + 60, iat, b0);
    TfClient *c = flow_client_in(w);
    TfPm *p = c ? tf__pm(c) : NULL;
    fake_reset(LEASE_OK);
    TfBuf o;
    tf_buf_init(&o);
    tf_json_write(&o, l7);
    g_f.fail_url = "/v1/model-key", g_f.fail_status = 200, g_f.fail_body = o.p;
    int held = 0;
    const double took = c && p ? close_during_refresh(c, 1000, &held) : -1;
    const int done = __atomic_load_n(&g_f.holding, __ATOMIC_SEQ_CST);
    c = close_offline(w, &p);
    const int rc = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
    tf__pm_status(p, &ls);
    CHECK(held == 1 && done == 3 && took >= 0.8 && took < 3.0 && rc == 0 && ls.valid && ls.exp == N(l7, "exp") && p && tf__pm_fails(p) == 0,
          "close: the refresh due, its answer 1 s into the destroy: the destroy took %.2f s (%s), and offline after it the new lease opens "
          "(%s, valid %d, exp %s)",
          took, close_held(done), rc ? tf_error_name(rc) : "ok", ls.valid,
          ls.exp == N(l7, "exp") ? "the new lease's" : ls.exp == exp ? "the old lease's" : "other");
    if (c) tf_client_destroy(c);
    tf_buf_free(&o);
  }
  /* the lease past exp, in grace: the refresh at the row's next attempt brings the next lease 1 s into the destroy */
  {
    snprintf(w, sizeof w, "%s/close_grace", work);
    load_reset();
    grace_install(w, iat, b0);
    load_clock(exp + 7200, iat, b0);
    TfClient *c = flow_client_in(w);
    TfPm *p = c ? tf__pm(c) : NULL;
    fake_reset(LEASE_OK);
    g_f.fail_url = "/v1/model-key", g_f.fail_status = 503, g_f.fail_body = kLeaseUnavailable;
    const int rc0 = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
    const int grace0 = p && tf__pm_grace(p, NULL);
    if (p) load_clock(tf__pm_next_try(p), iat, b0);
    fake_reset(LEASE_LATER);
    int held = 0;
    const double took = c && p ? close_during_refresh(c, 1000, &held) : -1;
    const int done = __atomic_load_n(&g_f.holding, __ATOMIC_SEQ_CST);
    c = close_offline(w, &p);
    const int rc = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
    tf__pm_status(p, &ls);
    const int grace = p && tf__pm_grace(p, NULL);
    CHECK(rc0 == 0 && grace0 && held == 1 && done == 3 && took >= 0.8 && took < 3.0 && rc == 0 && ls.valid && ls.exp == N(later, "exp") && !grace &&
              p && tf__pm_fails(p) == 0,
          "close: in grace, the refresh's answer 1 s into the destroy: the destroy took %.2f s (%s), and offline after it the new lease opens "
          "(%s, valid %d, exp %s, grace %d)",
          took, close_held(done), rc ? tf_error_name(rc) : "ok", ls.valid,
          ls.exp == N(later, "exp") ? "the new lease's" : ls.exp == exp ? "the old lease's" : "other", grace);
    if (c) tf_client_destroy(c);
  }
  /* the lease past exp, no attempt since: the refresh's answer does not come within the 3 s, the close cancels it, and
   * that is no failed attempt: no failure row, no wait, no grace begun by it */
  {
    snprintf(w, sizeof w, "%s/close_cut", work);
    load_reset();
    grace_install(w, iat, b0);
    load_clock(exp + 7200, iat, b0);
    TfClient *c = flow_client_in(w);
    TfPm *p = c ? tf__pm(c) : NULL;
    fake_reset(LEASE_LATER);
    int held = 0;
    const double took = c && p ? close_during_refresh(c, 20000, &held) : -1;
    const int done = __atomic_load_n(&g_f.holding, __ATOMIC_SEQ_CST);
    c = close_offline(w, &p);
    tf__pm_status(p, &ls);
    const int grace = p && tf__pm_grace(p, NULL);
    CHECK(held == 1 && done == 2 && took >= 2.8 && took < 6.0 && p && tf__pm_fails(p) == 0 && tf__pm_next_try(p) == 0 && !grace && ls.exp == exp,
          "close: the refresh's answer not within 3 s: the destroy took %.2f s (%s); then %d failure(s) in a row, next try %lld, grace %d "
          "(want 0, 0, 0)",
          took, close_held(done), p ? tf__pm_fails(p) : -1, p ? tf__pm_next_try(p) : -1LL, grace);
    if (c) tf_client_destroy(c);
  }
  load_reset();
  tf_json_free(mj);
}

static int t_load(const char *fixture, const char *work) {
  TfJson *fx = flow_setup(fixture);
  if (!fx) return 2;
  tf__download_pause_ms = 1;
  const TfJson *l1 = tf_json_get(fx, "lease");
  const long long iat = N(l1, "iat"), exp = N(l1, "exp"), refresh = N(l1, "refresh_after"), b0 = 3600000;
  const char *mt = S(fx, "manifest"), *build = S(fx, "build");
  TfJson *mj = tf_json_parse(mt, strlen(mt), NULL);
  char w[1100];

  /* 18.4.7.3: the background refresh past refresh_after, the lease valid, a run of 503 lease_unavailable with
   * Retry-After 30: every wait between 600 and 1800 s, never 30, not all the same; a valid lease opens with no request
   * while a retry waits; a restart while a retry waits sends nothing before the kept time */
  {
    snprintf(w, sizeof w, "%s/row", work);
    load_reset();
    grace_install(w, iat, b0);
    long long t = refresh + 60;
    load_clock(t, iat, b0);
    TfClient *c = flow_client_in(w);
    TfPm *p = c ? tf__pm(c) : NULL;
    fake_reset(LEASE_OK);
    g_f.fail_url = "/v1/model-key", g_f.fail_status = 503, g_f.fail_body = kLeaseUnavailable, g_f.retry_after = 30;
    long long waits[5] = {0};
    int asked[5] = {0}, good = 1, distinct = 0;
    for (int k = 0; k < 5 && c; ++k) {
      const int before = load_asked("/v1/model-key");
      tf__store_tick(c);
      asked[k] = load_asked("/v1/model-key") - before;
      waits[k] = tf__pm_next_try(p) - t;
      good &= asked[k] >= 1 && waits[k] >= 600 && waits[k] <= 1800 && tf__pm_fails(p) == k + 1;
      distinct |= k > 0 && waits[k] != waits[0];
      if (k == 1) {
        /* a valid lease opens with no request while the retry waits */
        load_clock(t + 10, iat, b0);
        const int b2 = load_asked("/v1/model-key"), ch = load_asked("/v1/attest/challenge");
        const int rc = tf__pm_lease(c, p, mt, strlen(mt), mj, build);
        CHECK(rc == 0 && load_asked("/v1/model-key") == b2 && load_asked("/v1/attest/challenge") == ch,
              "lease: a valid lease opens with no request while a retry waits (%s)", rc ? tf_error_name(rc) : "ok");
      }
      t += waits[k];
      load_clock(t, iat, b0);
    }
    CHECK(good && distinct, "lease: 503 lease_unavailable with Retry-After 30: waits %lld, %lld, %lld, %lld, %lld s (each 600 to 1800, not all one), "
                            "asked %d %d %d %d %d", waits[0], waits[1], waits[2], waits[3], waits[4], asked[0], asked[1], asked[2], asked[3], asked[4]);
    printf("  lease: waits %lld, %lld, %lld, %lld, %lld s under 503 lease_unavailable with Retry-After 30\n", waits[0], waits[1], waits[2], waits[3],
           waits[4]);
    /* a restart while a retry waits: one more failure, the process ends 10 s later, the new one asks nothing before the
     * kept time and asks at it */
    const int before = load_asked("/v1/model-key");
    if (c) tf__store_tick(c);
    const int after_tick = load_asked("/v1/model-key");
    const long long kept = p ? tf__pm_next_try(p) : 0;
    if (c) tf_client_destroy(c);
    load_clock(t + 10, iat, b0);
    c = flow_client_in(w);
    p = c ? tf__pm(c) : NULL;
    const int after_start = load_asked("/v1/model-key");
    load_clock(kept - 1, iat, b0);
    if (c) tf__store_tick(c);
    const int before_kept = load_asked("/v1/model-key");
    load_clock(kept, iat, b0);
    if (c) tf__store_tick(c);
    const int at_kept = load_asked("/v1/model-key");
    CHECK(kept - t >= 600 && kept - t <= 1800 && after_tick > before && after_start == after_tick && before_kept == after_start && at_kept > before_kept &&
              p && tf__pm_fails(p) == 7,
          "lease: a restart while a retry waits asks nothing before the kept time (%lld s on: %d at the start, %d just before) and asks at it "
          "(%d; %d in a row)",
          kept - t, after_start - after_tick, before_kept - after_start, at_kept - before_kept, p ? tf__pm_fails(p) : -1);
    /* the row ends at a lease the Core accepts */
    g_f.fail_url = NULL;
    load_clock(tf__pm_next_try(p), iat, b0);
    if (c) tf__store_tick(c);
    CHECK(c && tf__pm_fails(p) == 0 && tf__pm_next_try(p) == 0, "lease: a lease accepted ends the row (%d in a row)", tf__pm_fails(p));
    if (c) tf_client_destroy(c);
  }
  /* each wait is drawn afresh */
  {
    long long mn = 1 << 30, mx = 0;
    for (int i = 0; i < 200; ++i) {
      const long long d = tf__pm_lease_step();
      mn = d < mn ? d : mn;
      mx = d > mx ? d : mx;
    }
    CHECK(mn >= 600 && mx <= 1800 && mx - mn > 600, "lease: the wait is drawn afresh in [600, 1800] (%lld to %lld over 200)", mn, mx);
  }
  /* Retry-After 3000 gives 3000 (the floor over the drawn time) */
  {
    snprintf(w, sizeof w, "%s/ra3000", work);
    load_reset();
    grace_install(w, iat, b0);
    const long long t = refresh + 60;
    load_clock(t, iat, b0);
    TfClient *c = flow_client_in(w);
    TfPm *p = c ? tf__pm(c) : NULL;
    fake_reset(LEASE_OK);
    g_f.fail_url = "/v1/model-key", g_f.fail_status = 503, g_f.fail_body = kLeaseUnavailable, g_f.retry_after = 3000;
    if (c) tf__store_tick(c);
    CHECK(p && tf__pm_next_try(p) - t == 3000, "lease: Retry-After 3000 gives %lld s", p ? tf__pm_next_try(p) - t : -1);
    if (c) tf_client_destroy(c);
  }
  /* a call with no usable lease (past its end and its grace) asks at once when the route is not held, and fails with
   * the code it failed with before; while the route is held by the answer's Retry-After it fails the same, without a
   * request */
  {
    snprintf(w, sizeof w, "%s/nolease", work);
    load_reset();
    grace_install(w, iat, b0);
    const long long t = exp + 86400 + 600;
    load_clock(t, iat, b0);
    TfClient *c = flow_client_in(w);
    TfPm *p = c ? tf__pm(c) : NULL;
    fake_reset(LEASE_OK);
    g_f.fail_url = "/v1/model-key", g_f.fail_status = 503, g_f.fail_body = kLeaseUnavailable, g_f.retry_after = 30;
    tf__lease_reuse_s = 0; /* 18.4.7.1's reuse of an outcome less than a minute old runs on the real clock: off here */
    int rc[3] = {0, 0, 0}, asked[3] = {0, 0, 0};
    const long long at[3] = {0, 10, 31}; /* the second inside the Retry-After's hold, the third past it, inside the retry's wait */
    for (int k = 0; k < 3 && c; ++k) {
      load_clock(t + at[k], iat, b0);
      const int before = load_asked("/v1/model-key") + load_asked("/v1/attest/challenge");
      rc[k] = tf__pm_lease(c, p, mt, strlen(mt), mj, build);
      asked[k] = load_asked("/v1/model-key") + load_asked("/v1/attest/challenge") - before;
    }
    tf__lease_reuse_s = 60;
    CHECK(rc[0] == TF_ERR_DOWNLOAD_FAILED && rc[1] == rc[0] && rc[2] == rc[0] && asked[0] >= 2 && asked[1] == 0 && asked[2] >= 2 && p &&
              tf__pm_next_try(p) > t + 31,
          "lease: a call with no lease asks at once (%d), held by Retry-After asks nothing (%d), after it at once (%d), and fails as before "
          "(%s %s %s)",
          asked[0], asked[1], asked[2], tf_error_name(rc[0]), tf_error_name(rc[1]), tf_error_name(rc[2]));
    if (c) tf_client_destroy(c);
  }
  /* an open in the grace while a retry waits asks nothing (the SDK's own attempt, 18.4.7.3 item 3) */
  {
    snprintf(w, sizeof w, "%s/grace_row", work);
    load_reset();
    grace_install(w, iat, b0);
    const long long t = exp + 3600;
    load_clock(t, iat, b0);
    TfClient *c = flow_client_in(w);
    TfPm *p = c ? tf__pm(c) : NULL;
    fake_reset(LEASE_OK);
    g_f.fail_url = "/v1/model-key", g_f.fail_status = 503, g_f.fail_body = kLeaseUnavailable, g_f.retry_after = 30;
    tf__lease_reuse_s = 0;
    const int rc1 = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1; /* unreachable: opens in the grace */
    load_clock(t + 45, iat, b0);
    const int before = load_asked("/v1/model-key");
    const int rc2 = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
    const int asked = load_asked("/v1/model-key") - before;
    tf__lease_reuse_s = 60;
    CHECK(rc1 == 0 && rc2 == 0 && asked == 0 && p && tf__pm_grace(p, NULL), "lease: an open in the grace while a retry waits asks nothing (%d asked)",
          asked);
    if (c) tf_client_destroy(c);
  }
  load_reset();
  tf__plat_test_boot_ms = 0;
  tf__plat_test_wall_ms = 0;
  tf__plat_test_boot_id = NULL;

  /* 28.11 item 6: a 429 with Retry-After 90 on each route, then the same with a restart 10 s on */
  for (int restart = 0; restart < 2; ++restart) {
    load_route_hold(work, "config", "/v1/config", "/v1/config", 0, restart);
    load_route_hold(work, "register", "/v1/register", "/v1/register", 2, restart);
    load_route_hold(work, "attest", "/v1/attest", "/v1/attest", 0, restart);
    load_route_hold(work, "lease", "/v1/model-key", "/v1/model-key", 0, restart);
    load_route_hold(work, "events", "/v1/events", "/v1/events", 1, restart);
    load_route_hold(work, "issues", "/v1/issues", "/v1/issues", 3, restart);
  }

  /* 28.11 item 6: events, a 429 with Retry-After 120; the next send point 30 s later sends nothing, the one after 120 s
   * sends the same report; no other host is asked */
  {
    snprintf(w, sizeof w, "%s/events", work);
    fake_reset(LEASE_OK);
    const long long t = (long long)time(NULL);
    load_at(t);
    TfClient *c = flow_client(w);
    g_f.fail_url = "/v1/events", g_f.fail_status = 429, g_f.fail_body = kRateLimited, g_f.retry_after = 120;
    nw_forget();
    if (c) tf__client_flush(c, 1);
    int first = 0, others = 0;
    load_hosts("/v1/events", &first, &others);
    const int n1 = load_asked("/v1/events");
    load_at(t + 30);
    if (c) tf__client_flush(c, 1);
    const int n2 = load_asked("/v1/events");
    g_f.fail_url = NULL;
    load_at(t + 120);
    if (c) tf__client_flush(c, 1);
    const int n3 = load_asked("/v1/events");
    CHECK(n1 == 1 && first == 1 && others == 0 && n2 == 1 && n3 >= 2, "events: a 429 with Retry-After 120 (asked %d, other hosts %d), 30 s on %d more, "
                                                                     "120 s on %d more", n1, others, n2 - n1, n3 - n2);
    load_reset();
    if (c) tf_client_destroy(c);
  }
  /* 28.3: a 429 on any route of the stand-in asks no other host */
  {
    static const struct {
      const char *label, *fail, *count;
      int how; /* 0 a model call, 1 a send point, 2 register, 3 an error report's send */
    } k[] = {{"config", "/v1/config", "/v1/config", 0},
             {"attest", "/v1/attest", "/v1/attest", 0},
             {"challenge", "/v1/attest/challenge", "/v1/attest/challenge", 0},
             {"model-key", "/v1/model-key", "/v1/model-key", 0},
             {"manifest", "manifest.json", "manifest.json", 0},
             {"file", ".qle", ".qle", 0},
             {"events", "/v1/events", "/v1/events", 1},
             {"register", "/v1/register", "/v1/register", 2},
             {"issues", "/v1/issues", "/v1/issues", 3}};
    int ok = 0;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; ++i) {
      snprintf(w, sizeof w, "%s/r429_%s", work, k[i].label);
      fake_reset(LEASE_OK);
      TfClient *c = flow_client(w);
      if (k[i].how == 3 && c) { /* an error report queued first: a config that answers 503 */
        g_f.fail_url = "/v1/config", g_f.fail_status = 503;
        tf_client_model(c, NULL);
      }
      nw_forget();
      g_f.fail_url = k[i].fail, g_f.fail_status = 429, g_f.fail_body = kRateLimited, g_f.retry_after = 0;
      if (c && k[i].how == 0) {
        TfModel *m = tf_client_model(c, NULL);
        if (m) tf_model_close(m);
      } else if (c && k[i].how == 2) {
        tf__client_register(c);
      } else if (c) {
        tf__client_flush(c, 1);
      }
      int first = 0, others = 0;
      tf_mutex_lock(&g_f_lock);
      for (int r = 0; r < g_f.n_req; ++r) {
        if (!strstr(g_f.req[r], k[i].count)) continue;
        if (!strncmp(g_f.req[r], "https://api.quranlab.ai/", 24) || !strncmp(g_f.req[r], "https://cdn.quranlab.ai/", 24)) first++;
        else others++;
      }
      tf_mutex_unlock(&g_f_lock);
      const int good = first >= 1 && others == 0;
      CHECK(good, "429 on %s: asked %d time(s) of the first host, %d of another", k[i].label, first, others);
      ok += good;
      g_f.fail_url = NULL;
      if (c) tf_client_destroy(c);
    }
    printf("  429: %d of %d routes asked no other host\n", ok, (int)(sizeof k / sizeof k[0]));
  }
  load_held_failover(fx, work);
  load_close(fx, work);
  tf__download_pause_ms = 1000;
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(mj);
  tf_json_free(fx);
  return done("load");
}
