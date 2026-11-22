/* SDK 4.0.2: the objects' lifetimes (a call after an object's end never touches freed memory), included by
 * tf40_tests.c after tf40_requests_tests.inc.
 *
 *   tf40_tests lifetime <nowait40 fixture> <work>
 *
 * A call after an object's end gets a clean error and never touches freed memory. The stand-in Core opens no engine for
 * the fixture's build, so the Model is opened without one (as tf_client_model opens it, the engine left for a first
 * Session) and its Session is an ended one with no stream (tf__session_test_ended): the paths below are the objects'
 * links, not audio. A Model's free is counted (tf__model_frees), so a Model freed too early fails a check at once and
 * the test does not touch it after. Path 4 can only show under AddressSanitizer on the code before (Linux CI).
 *
 *   1. a Session used and destroyed after its Model was closed;
 *   2. (no test: it needs a real stream) a Session whose stream another Model's take destroyed reads it only while it
 *      is still its own (tf__model_stream_of, its generation), under the streams' lock;
 *   3. a Model the Client gave out twice, the Client destroyed, then the Model closed twice;
 *   4. an ended Session's late recited report after its Client was destroyed;
 *   5. a Client destroyed while its Model and Session are in use on another thread: the stress, 1000 rounds.
 * And one Core per storage folder (a Client made while an earlier Client's Model is open takes its Core; a lease.json
 * never goes back to an older lease; a clear always goes). */

static TfModel *life_model(TfClient *c) {
  TfFetch f;
  memset(&f, 0, sizeof f);
  char dir[1024];
  TfModelOpen open;
  memset(&open, 0, sizeof open);
  if (!c || tf__store_fetch(c, &f, dir, sizeof dir, &open)) return NULL;
  open.no_engine = 1;
  return tf__model_open(dir, &open);
}
static TfClient *life_client(const char *work, const char *name) {
  char w[1100];
  snprintf(w, sizeof w, "%s/%s", work, name);
  fake_reset(LEASE_OK);
  return flow_client(w);
}

/* the stress: a Model and its ended Session used on their own thread while the Client is destroyed on this one, its
 * timer's renewal in flight (its answer held 0 to 19 ms); then the Session destroyed and the Model closed (once, or
 * twice when given out twice), in either order. No audio: the stream reads are the guarded ones (tf__session_topk). */
typedef struct {
  TfModel *m;
  TfSession *s;
  int holders, model_first, iters;
} LifeRun;
static void life_user(void *arg) {
  LifeRun *r = (LifeRun *)arg;
  char ctx[32];
  qlc_lease_status ls;
  for (int i = 0; i < r->iters; ++i) {
    tf_session_report_recited(r->s, 1, 1, 7);  /* reaches the Client, while it has one */
    (void)tf_session_heads(r->s);
    tf__topk_free(tf__session_topk(r->s, 0, 1)); /* the stream, guarded */
    tf__model_context_in_force(r->m, ctx, sizeof ctx); /* the Client's config, while it has one */
    tf__pm_status(tf__model_pm(r->m), &ls);    /* the Core: its tick saves its state */
    tf__clear_error();
  }
  if (r->model_first) {
    for (int k = 0; k < r->holders; ++k) tf_model_close(r->m);
    tf_session_destroy(r->s);
  } else {
    tf_session_destroy(r->s);
    for (int k = 0; k < r->holders; ++k) tf_model_close(r->m);
  }
}
static unsigned life_rand(unsigned *x) {
  *x = *x * 1103515245u + 12345u;
  return (*x >> 16) & 0x7fff;
}
static void life_stress(const TfJson *fx, const char *work, int rounds) {
  const TfJson *l1 = tf_json_get(fx, "lease");
  const long long iat = N(l1, "iat"), refresh = N(l1, "refresh_after"), b0 = 3600000;
  char w[1100];
  snprintf(w, sizeof w, "%s/stress", work);
  load_clock(refresh + 60, iat, b0); /* the lease valid and its refresh due: every round's timer asks for one */
  fake_reset(LEASE_OK);
  TfClient *c0 = flow_client(w);
  const int pre = c0 ? tf_client_prefetch(c0, NULL) : -1;
  if (c0) tf_client_destroy(c0);
  unsigned seed = 20261122u;
  int done_rounds = 0, opened = 0;
  const int f0 = tf__model_frees;
  const double t0 = tf__now_s();
  for (int i = 0; i < rounds && pre == 0; ++i) {
    fake_reset(LEASE_OK);
    g_f.hold_url = "/v1/model-key", g_f.hold_ms = (int)(life_rand(&seed) % 20);
    TfClient *c = flow_client_in(w);
    TfModel *m = life_model(c);
    if (!m) {
      if (c) tf_client_destroy(c);
      continue;
    }
    opened++;
    LifeRun r;
    r.m = m;
    r.s = tf__session_test_ended(m);
    r.holders = 1 + (int)(life_rand(&seed) % 2);
    r.model_first = (int)(life_rand(&seed) % 2);
    r.iters = (int)(life_rand(&seed) % 40);
    tf__client_test_model(c, m, r.holders);
    tf__client_no_timer = 0;
    tf__client_timer_start(c);
    tf__client_no_timer = 1;
    TfThread th;
    const int started = r.s && !tf_thread_start(&th, life_user, &r);
    tf__sleep_ms((int)(life_rand(&seed) % 5));
    tf_client_destroy(c);
    if (started) tf_thread_join(th);
    done_rounds += started;
  }
  g_f.hold_url = NULL;
  load_reset();
  printf("  stress: %d rounds in %.1f s\n", done_rounds, tf__now_s() - t0);
  CHECK(pre == 0 && done_rounds == rounds && opened == rounds && tf__model_frees - f0 == rounds,
        "stress: %d of %d rounds of a Client destroyed while its Model and Session are in use on another thread and its renewal is in "
        "flight; each Model freed once (%d frees)",
        done_rounds, rounds, tf__model_frees - f0);
}

static int t_lifetime(const char *nowait, const char *work) {
  TfJson *fx = flow_setup(nowait);
  if (!fx) return 2;
  tf__download_pause_ms = 1;
  /* the Model as tf_client_model gives it, without its engine */
  {
    TfClient *c = life_client(work, "probe");
    TfModel *m = life_model(c);
    CHECK(m != NULL, "lifetime: the fixture's Model opens without its engine (%s)", m ? "ok" : tf_last_error());
    if (!m) {
      if (c) tf_client_destroy(c);
      tf_json_free(fx);
      return done("lifetime");
    }
    tf_model_close(m);
    tf_client_destroy(c);
  }
  /* 1: the Model closed while its ended Session lives: the Model's memory stays until the Session goes; the Session's
   * calls answer cleanly; its destroy frees the Model */
  {
    TfClient *c = life_client(work, "p1");
    TfModel *m = life_model(c);
    TfSession *s = m ? tf__session_test_ended(m) : NULL;
    if (m) tf__client_test_model(c, m, 1);
    const int f0 = tf__model_frees;
    if (m) tf_model_close(m);
    const int kept = s && tf__model_frees == f0;
    CHECK(kept, "lifetime 1: a Model closed while its Session lives is kept for it (%d free(s) at the close, want 0)", tf__model_frees - f0);
    if (kept) {
      tf__clear_error();
      const TfHeads *h = tf_session_heads(s);
      const int heads_err = !h && tf_last_error_code() == TF_ERR_INVALID_ARGUMENT;
      const int push = tf_session_push(s, NULL, 0);
      tf_session_destroy(s);
      CHECK(heads_err && push == TF_ERR_SESSION_CLOSED && tf__model_frees == f0 + 1,
            "lifetime 1: the Session after its Model's close: heads %s, push %s, its destroy frees the Model (%d free(s))",
            heads_err ? "a clean error" : "?", tf_error_name(push), tf__model_frees - f0);
    }
    if (c) tf_client_destroy(c);
  }
  /* 3: a Model the Client gave out twice; the Client destroyed: each holder's close counts, the second frees it */
  {
    TfClient *c = life_client(work, "p3");
    TfModel *m = life_model(c);
    if (m) tf__client_test_model(c, m, 2);
    if (c) tf_client_destroy(c);
    const int f0 = tf__model_frees;
    if (m) tf_model_close(m);
    const int kept = m && tf__model_frees == f0;
    CHECK(kept, "lifetime 3: a Model held twice, its Client destroyed: the first close keeps it (%d free(s), want 0)", tf__model_frees - f0);
    if (kept) {
      tf_model_close(m);
      CHECK(tf__model_frees == f0 + 1, "lifetime 3: the second holder's close frees it (%d free(s))", tf__model_frees - f0);
    }
  }
  /* one Core per storage folder: Client A destroyed while its Model is open, Client B made on the same folder. B takes
   * A's live Core (one Core, refcounted), so B's newer lease is the one saved, whatever A's Core does after; before,
   * B made a second Core and a save of A's Core (here its tick) wrote A's older state over B's */
  {
    const TfJson *l1 = tf_json_get(fx, "lease"), *later = tf_json_get(fx, "lease_later");
    const long long iat = N(l1, "iat"), exp = N(l1, "exp"), b0 = 3600000;
    const char *mt = S(fx, "manifest"), *build = S(fx, "build");
    TfJson *mj = tf_json_parse(mt, strlen(mt), NULL);
    char w[1100];
    snprintf(w, sizeof w, "%s/folder", work);
    load_clock(iat + 60, iat, b0);
    fake_reset(LEASE_OK);
    TfClient *a = flow_client(w);
    TfModel *m = life_model(a);
    if (m) tf__client_test_model(a, m, 1);
    if (a) tf_client_destroy(a);
    const int made0 = tf__pm_made;
    load_clock(exp + 7200, iat, b0); /* lease 1 past its end: B asks for the next one */
    fake_reset(LEASE_LATER);
    TfClient *b = flow_client_in(w);
    TfPm *pb = b ? tf__pm(b) : NULL;
    const int shared = m && pb && pb == tf__model_pm(m) && tf__pm_made == made0;
    CHECK(shared, "folder: Client B made while Client A's Model is open takes A's Core (%s, %d Core(s) made)", shared ? "one Core" : "a second Core",
          tf__pm_made - made0);
    const int rc = pb ? tf__pm_lease(b, pb, mt, strlen(mt), mj, build) : -1;
    qlc_lease_status ls;
    if (m) tf__pm_status(tf__model_pm(m), &ls); /* A's Core ticks: it saves its state */
    if (m) tf_model_close(m);
    if (b) tf_client_destroy(b);
    fake_reset(LEASE_OK);
    g_f.fail_url = "https://", g_f.fail_status = 0;
    const int made1 = tf__pm_made;
    TfClient *c = flow_client_in(w);
    TfPm *pc = c ? tf__pm(c) : NULL;
    tf__pm_status(pc, &ls);
    CHECK(rc == 0 && ls.exp == N(later, "exp"), "folder: B's newer lease survives A's Core (%s; after a restart offline exp is %s)",
          rc ? tf_error_name(rc) : "ok", ls.exp == N(later, "exp") ? "B's lease's" : ls.exp == exp ? "A's older lease's" : "other");
    /* the folder's Core went with its last holder: the next Client makes a new one */
    CHECK(pc && tf__pm_made == made1 + 1, "folder: after the last holder a new Client makes a new Core (%d made)", tf__pm_made - made1);
    /* lease.json never goes back to an older lease: one issued earlier than the one written is refused */
    {
      char p[1200], *text = NULL;
      size_t n = 0;
      snprintf(p, sizeof p, "%s/install/lease.json", w);
      const char *newer = "{\"build\":\"1111111111111111111111111111111111111111\",\"base\":\"\",\"iat\":2000000000}\n";
      tf_write_file(p, newer, strlen(newer));
      tf__pm_test_rec_save(pc, "2222222222222222222222222222222222222222", 1900000000);
      const int kept = !tf_read_file(p, &text, &n) && text && strstr(text, "1111111111111111111111111111111111111111") != NULL;
      CHECK(kept, "folder: a lease.json from an older lease is refused (%s)", kept ? "the newer one stays" : "overwritten");
      free(text);
    }
    if (c) tf_client_destroy(c);
    load_reset();
    tf_json_free(mj);
  }
  /* a clear always goes: a stored lease, then a stop the Core verifies, then a restart with no network: no lease, no
   * lease.json, and the open fails (a monotonic save never holds a clear back) */
  {
    const TfJson *l1 = tf_json_get(fx, "lease");
    const long long iat = N(l1, "iat"), exp = N(l1, "exp"), b0 = 3600000, t0 = exp + 7200;
    const char *mt = S(fx, "manifest"), *build = S(fx, "build");
    TfJson *mj = tf_json_parse(mt, strlen(mt), NULL);
    static const char kStop[] = "[{\"route\":\"challenge\",\"status\":200,\"body\":\"json\"},"
                                "{\"route\":\"model-key\",\"status\":401,\"body\":\"json\",\"error\":\"invalid_key\",\"reason\":\"key_revoked\",\"core\":\"stop\"}]";
    TfJson *script = tf_json_parse(kStop, strlen(kStop), NULL);
    char w[1100], rec[1200];
    snprintf(w, sizeof w, "%s/stop", work);
    snprintf(rec, sizeof rec, "%s/install/lease.json", w);
    grace_install(w, iat, b0);
    const int had = tf_file_exists(rec);
    tf__plat_test_boot_ms = b0 + (t0 - iat) * 1000;
    TfClient *c = flow_client_in(w);
    TfPm *p = c ? tf__pm(c) : NULL;
    fake_reset(LEASE_OK);
    g_f.script = script;
    const int rc = p ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
    g_f.script = NULL;
    if (c) tf_client_destroy(c);
    fake_reset(LEASE_OK);
    g_f.fail_url = "https://", g_f.fail_status = 0;
    c = flow_client_in(w);
    p = c ? tf__pm(c) : NULL;
    qlc_lease_status ls;
    tf__pm_status(p, &ls);
    const int rc2 = p ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
    CHECK(had && rc == TF_ERR_INVALID_KEY && !ls.has_lease && !ls.valid && rc2 != 0 && !tf_file_exists(rec),
          "clear: a stored lease (lease.json %s), a verified stop (%s), a restart with no network: lease %s, lease.json %s, the open %s",
          had ? "there" : "missing", tf_error_name(rc), ls.has_lease ? "kept" : "gone", tf_file_exists(rec) ? "kept" : "gone",
          rc2 ? tf_error_name(rc2) : "opened");
    if (c) tf_client_destroy(c);
    tf_json_free(script);
    tf_json_free(mj);
  }
  /* 5: the stress (1000 rounds; TF_LIFETIME_ROUNDS to change it) */
  life_stress(fx, work, getenv("TF_LIFETIME_ROUNDS") ? atoi(getenv("TF_LIFETIME_ROUNDS")) : 1000);
  /* 4 (last: on the code before, only AddressSanitizer sees it, and it ends the process): an ended Session's late
   * recited report after its Client was destroyed reaches no Client */
  {
    TfClient *c = life_client(work, "p4");
    TfModel *m = life_model(c);
    TfSession *s = m ? tf__session_test_ended(m) : NULL;
    if (m) tf__client_test_model(c, m, 1);
    if (c) tf_client_destroy(c);
    const int rc = s ? tf_session_report_recited(s, 1, 1, 7) : -1;
    CHECK(s && rc == 0, "lifetime 4: a late recited report after the Client's destroy answers cleanly (%s)", rc ? tf_error_name(rc) : "ok");
    if (s) tf_session_destroy(s);
    if (m) tf_model_close(m);
  }
  tf_json_free(fx);
  return done("lifetime");
}
