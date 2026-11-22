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

/* ------------------------------------------------------------------ 4.0.3: the folder's one open model
 *
 *   tf40_tests lifetime_open <testbuild fixture> <work>
 *
 * On the encrypted test build (make_flow40.py --testbuild): real model opens through the Core. The Core holds one
 * open model (a second qlc_model_open is QLC_E_BUSY), and from 4.0.2 a storage folder has one Core in the process,
 * so that open model is the folder's, whichever Client's Model holds it:
 *
 *   6. Client A's Model opens and a take runs; A is destroyed while the app keeps that Model; Client B on the same
 *      folder opens a Model: it opens and a take on it works (4.0.2: session_open, the Core's open model held by A's
 *      idle Model). A's Model, still held, runs a take again, then B's; each Model is freed once at its close, and the
 *      folder's Core goes with its last holder.
 *   7. A take of A's Model open while B opens: B's open waits for it, bounded (tf__open_wait_s, set here so a slow
 *      machine does not decide the outcome). A take that ends within the bound lets B open; one that does not gives B
 *      session_open after the bound, and the take goes on unharmed; once it ends, B opens.
 *   8. Takes of A's Model and of B's on two threads at once, the Core's open model going back and forth between them:
 *      every take runs (TF_OPEN_ROUNDS rounds of 6 takes each, default 2).
 *   9. The reference data of B's Model fetched and read while A's idle Model has the Core's open model, with a take of
 *      A's between two reads.
 *  10. An app's UI thread never waits (Windows: IsGUIThread; here the test's hook says so): B's open there while A's
 *      take runs is session_open at once; once the take ended, B opens there without a wait.
 *  11. Windows only, the real check without the hook: the console thread is no UI thread; a thread made a GUI thread
 *      (IsGUIThread(TRUE)) is, and B's open on it while A's take runs is session_open at once. */
#define OPEN_NO_MODEL (-1000) /* the step had no Model (its open failed) */
static void open_audio(float *x, int n) {
  unsigned s = 20261122u;
  for (int i = 0; i < n; ++i) x[i] = 0.05f * (float)((int)(life_rand(&s) % 2001) - 1000) / 1000.0f;
}
static int open_push(TfSession *s, const float *x, int n) {
  for (int at = 0; at < n; at += 320) {
    const int rc = tf_session_push(s, x + at, n - at < 320 ? n - at : 320);
    if (rc < 0) return rc;
  }
  return 0;
}
/* the take's end: its finish, then its Heads with frames. 0, or the error */
static int open_finish(TfSession *s) {
  if (tf_session_finish(s) < 0) return tf_last_error_code() ? tf_last_error_code() : TF_ERR_ENGINE_FAILED;
  const TfHeads *h = tf_session_heads(s);
  const TfDecoded *d = h ? tf_heads_decoded(h) : NULL;
  return d && d->n_frames > 0 ? 0 : TF_ERR_ENGINE_FAILED;
}
/* one take of 1 s on m: 0, or the error of the step that failed (*step names it) */
static int open_take(TfModel *m, const float *x, const char **step) {
  *step = "create";
  TfSession *s = m ? tf_session_create(m, NULL) : NULL;
  if (!s) return m ? tf_last_error_code() : OPEN_NO_MODEL;
  *step = "push";
  int rc = open_push(s, x, 16000);
  if (!rc) *step = "finish", rc = open_finish(s);
  tf_session_destroy(s);
  return rc;
}
static int open_ui_thread(void) { return 1; } /* tf__ui_thread_hook: every thread is a UI thread */
#ifdef _WIN32
/* case 11: B's open on a thread made a GUI thread (user32 loaded for it and left loaded: a GUI thread uses it) */
typedef struct {
  TfClient *b;
  TfModel *mb;
  int rc, ui;
  double dt;
} OpenGui;
static void open_gui(void *arg) {
  OpenGui *g = (OpenGui *)arg;
  typedef BOOL(WINAPI * IsGuiFn)(BOOL);
  HMODULE u = LoadLibraryW(L"user32.dll");
  IsGuiFn f = u ? (IsGuiFn)(void (*)(void))GetProcAddress(u, "IsGUIThread") : NULL;
  if (f) f(TRUE); /* converts this thread to a GUI thread */
  g->ui = tf__ui_thread();
  const double t0 = tf__now_s();
  g->mb = g->b ? tf_client_model(g->b, NULL) : NULL;
  g->dt = tf__now_s() - t0;
  g->rc = g->mb ? 0 : tf_last_error_code();
}
#endif
static const char *open_rc(int rc) { return rc == OPEN_NO_MODEL ? "no Model" : rc ? tf_error_name(rc) : "ok"; }
/* Client A's Model open with a take run, A destroyed (the app keeps the Model), Client B made on the same folder */
static TfModel *open_a(const char *w, const float *x, TfClient **b) {
  fake_reset(LEASE_OK);
  TfClient *a = flow_client(w);
  TfModel *ma = a ? tf_client_model(a, NULL) : NULL;
  const char *step = "";
  const int rc = ma ? open_take(ma, x, &step) : tf_last_error_code();
  CHECK(ma && !rc, "open: Client A's Model opens and a take runs (%s at %s: %s)", open_rc(rc), ma ? step : "open", rc ? tf_last_error() : "");
  if (a) tf_client_destroy(a);
  fake_reset(LEASE_OK);
  *b = flow_client_in(w);
  return ma;
}
typedef struct {
  TfSession *s;
  const float *x;
  int delay_ms, rc;
} OpenEnd;
static void open_end(void *arg) {
  OpenEnd *e = (OpenEnd *)arg;
  tf__sleep_ms(e->delay_ms);
  int rc = open_push(e->s, e->x + 8000, 8000);
  if (!rc) rc = open_finish(e->s);
  e->rc = rc;
}
typedef struct {
  TfModel *m;
  const float *x;
  int takes, ran, refused, other, other_rc;
} OpenTurns;
static void open_turns(void *arg) {
  OpenTurns *t = (OpenTurns *)arg;
  for (int i = 0; i < t->takes; ++i) {
    const char *step = "";
    const int rc = open_take(t->m, t->x, &step);
    if (!rc) t->ran++;
    else if (rc == TF_ERR_SESSION_OPEN && !strcmp(step, "create")) t->refused++;
    else t->other++, t->other_rc = rc;
    tf__sleep_ms(20); /* a gap between takes: the other thread's waiting open finds the Core's model free */
  }
}

static int t_lifetime_open(const char *fixture, const char *work) {
  TfJson *fx = flow_setup(fixture);
  if (!fx) return 2;
  float *x = (float *)malloc(16000 * sizeof(float));
  if (!x) return 2;
  open_audio(x, 16000);
  char w[1100];
  const char *step = "";
  /* 6: the reported sequence */
  {
    snprintf(w, sizeof w, "%s/open6", work);
    TfClient *b = NULL;
    TfModel *ma = open_a(w, x, &b);
    const double t0 = tf__now_s();
    TfModel *mb = b ? tf_client_model(b, NULL) : NULL;
    const int orc = mb ? 0 : tf_last_error_code();
    const int f0 = tf__model_frees; /* after B's open (a failed open frees its own) */
    CHECK(mb != NULL, "open 6: Client B on the same folder opens a Model while A's Model is held after A's destroy (%s in %.2f s%s%s)",
          open_rc(orc), tf__now_s() - t0, mb ? "" : ": ", mb ? "" : tf_last_error());
    int rc = mb ? open_take(mb, x, &step) : OPEN_NO_MODEL;
    CHECK(mb && !rc, "open 6: a take on B's Model works (%s at %s)", open_rc(rc), step);
    rc = ma ? open_take(ma, x, &step) : OPEN_NO_MODEL;
    CHECK(ma && !rc, "open 6: A's Model, held after its Client's destroy, runs a take again (%s at %s)", open_rc(rc), step);
    rc = mb ? open_take(mb, x, &step) : OPEN_NO_MODEL;
    CHECK(mb && !rc, "open 6: then B's Model again (%s at %s)", open_rc(rc), step);
    if (ma) tf_model_close(ma);
    if (mb) tf_model_close(mb);
    if (b) tf_client_destroy(b);
    CHECK(tf__model_frees - f0 == (ma != NULL) + (mb != NULL), "open 6: each Model freed once at its close (%d free(s))", tf__model_frees - f0);
    const int made = tf__pm_made;
    fake_reset(LEASE_OK);
    TfClient *c = flow_client_in(w);
    TfPm *pc = c ? tf__pm(c) : NULL;
    CHECK(pc && tf__pm_made == made + 1, "open 6: the folder's Core went with its last holder (a new Client makes one: %d made)", tf__pm_made - made);
    if (c) tf_client_destroy(c);
  }
  /* 7a: a take of A's Model open while B opens; it ends 0.4 s later on another thread: B's open waits and opens */
  tf__open_wait_s = 60;
  {
    snprintf(w, sizeof w, "%s/open7a", work);
    TfClient *b = NULL;
    TfModel *ma = open_a(w, x, &b);
    TfSession *sa = ma ? tf_session_create(ma, NULL) : NULL;
    const int pushed = sa ? open_push(sa, x, 8000) : OPEN_NO_MODEL;
    CHECK(sa && !pushed, "open 7a: a take of A's Model is open (%s)", sa ? open_rc(pushed) : tf_last_error());
    OpenEnd e = {sa, x, 400, -1};
    TfThread th;
    const int started = sa && !tf_thread_start(&th, open_end, &e);
    const double t0 = tf__now_s();
    TfModel *mb = b ? tf_client_model(b, NULL) : NULL;
    const double dt = tf__now_s() - t0;
    const int orc = mb ? 0 : tf_last_error_code();
    if (started) tf_thread_join(th);
    CHECK(mb != NULL && dt > 0.3, "open 7a: B's open waits for A's take, which ends after 0.4 s, and opens (%s after %.2f s)", open_rc(orc), dt);
    CHECK(started && e.rc == 0, "open 7a: A's take ends normally on its thread (%s)", open_rc(e.rc));
    const int rc = mb ? open_take(mb, x, &step) : OPEN_NO_MODEL;
    CHECK(mb && !rc, "open 7a: a take on B's Model works (%s at %s)", open_rc(rc), step);
    if (sa) tf_session_destroy(sa);
    if (ma) tf_model_close(ma);
    if (mb) tf_model_close(mb);
    if (b) tf_client_destroy(b);
  }
  /* 7b: a take of A's Model open past the bound (1 s here): B gets session_open after the wait; the take goes on; once
   * it ends, B opens */
  tf__open_wait_s = 1;
  {
    snprintf(w, sizeof w, "%s/open7b", work);
    TfClient *b = NULL;
    TfModel *ma = open_a(w, x, &b);
    TfSession *sa = ma ? tf_session_create(ma, NULL) : NULL;
    const int pushed = sa ? open_push(sa, x, 8000) : OPEN_NO_MODEL;
    const double t0 = tf__now_s();
    TfModel *mb = b ? tf_client_model(b, NULL) : NULL;
    const double dt = tf__now_s() - t0;
    const int orc = mb ? 0 : tf_last_error_code();
    CHECK(sa && !pushed && !mb && orc == TF_ERR_SESSION_OPEN && dt > 0.9 && dt < 10,
          "open 7b: B's open while A's take stays open: session_open after the 1 s wait (%s after %.2f s)", open_rc(orc), dt);
    int rc = sa ? open_push(sa, x + 8000, 8000) : OPEN_NO_MODEL;
    if (!rc) rc = open_finish(sa);
    CHECK(sa && !rc, "open 7b: A's take goes on and ends normally (%s)", open_rc(rc));
    if (sa) tf_session_destroy(sa);
    if (mb) tf_model_close(mb);
    mb = b ? tf_client_model(b, NULL) : NULL;
    CHECK(mb != NULL, "open 7b: once A's take ended, B opens (%s)", mb ? "ok" : open_rc(tf_last_error_code()));
    rc = mb ? open_take(mb, x, &step) : OPEN_NO_MODEL;
    CHECK(mb && !rc, "open 7b: a take on B's Model works (%s at %s)", open_rc(rc), step);
    if (ma) tf_model_close(ma);
    if (mb) tf_model_close(mb);
    if (b) tf_client_destroy(b);
  }
  /* 8: takes of A's Model and B's on two threads at once */
  tf__open_wait_s = 60;
  const int rounds = getenv("TF_OPEN_ROUNDS") ? atoi(getenv("TF_OPEN_ROUNDS")) : 2;
  for (int round = 0; round < rounds; ++round) {
    snprintf(w, sizeof w, "%s/open8", work);
    TfClient *b = NULL;
    TfModel *ma = open_a(w, x, &b);
    TfModel *mb = b ? tf_client_model(b, NULL) : NULL;
    CHECK(mb != NULL, "open 8: B opens (%s)", mb ? "ok" : open_rc(tf_last_error_code()));
    OpenTurns ta = {ma, x, 6, 0, 0, 0, 0}, tb = {mb, x, 6, 0, 0, 0, 0};
    TfThread th;
    const int started = ma && mb && !tf_thread_start(&th, open_turns, &ta);
    const double t0 = tf__now_s();
    if (started) {
      open_turns(&tb);
      tf_thread_join(th);
    }
    printf("  open 8 round %d: A %d run, %d refused; B %d run, %d refused; %.1f s\n", round + 1, ta.ran, ta.refused, tb.ran, tb.refused,
           tf__now_s() - t0);
    CHECK(started && ta.ran == 6 && tb.ran == 6,
          "open 8 round %d: takes of both Models on two threads all run (A %d run, %d refused, %d other %s; B %d run, %d refused, %d other %s)",
          round + 1, ta.ran, ta.refused, ta.other, open_rc(ta.other_rc), tb.ran, tb.refused, tb.other, open_rc(tb.other_rc));
    if (ma) tf_model_close(ma);
    if (mb) tf_model_close(mb);
    if (b) tf_client_destroy(b);
  }
  /* 9: the reference data beside another Model's idle engine. A's kept Model has the Core's open model (a take of it
   * ran last); B fetches the reference data (the Core installs it only while no model of the build is open, so A's idle
   * engine is closed for it) and reads an ayah; A runs a take (the open model back to A); B reads another ayah */
  {
    snprintf(w, sizeof w, "%s/open9", work);
    TfClient *b = NULL;
    TfModel *ma = open_a(w, x, &b);
    TfModel *mb = b ? tf_client_model(b, NULL) : NULL;
    int rc = ma && mb ? open_take(ma, x, &step) : OPEN_NO_MODEL;
    CHECK(mb && !rc, "open 9: B opens, then A's Model runs a take (%s at %s)", open_rc(rc), mb ? step : "open");
    TfReference *ref = mb ? tf_model_reference(mb) : NULL;
    rc = ref ? tf_reference_fetch(ref) : OPEN_NO_MODEL;
    CHECK(ref && !rc && tf_reference_is_downloaded(ref), "open 9: B fetches the reference data while A's idle Model has the Core's open model (%s%s%s)",
          open_rc(rc), rc && rc != OPEN_NO_MODEL ? ": " : "", rc && rc != OPEN_NO_MODEL ? tf_last_error() : "");
    TfUnits u;
    memset(&u, 0, sizeof u);
    rc = ref && tf_reference_is_downloaded(ref) ? tf_reference_units(ref, 1, 1, &u) : OPEN_NO_MODEL;
    CHECK(!rc && u.n > 0 && u.n_words > 0, "open 9: B reads 1:1 (%s, %d units)", open_rc(rc), u.n);
    rc = ma ? open_take(ma, x, &step) : OPEN_NO_MODEL;
    CHECK(ma && !rc, "open 9: A's Model runs a take after B's read (%s at %s)", open_rc(rc), step);
    memset(&u, 0, sizeof u);
    rc = ref && tf_reference_is_downloaded(ref) ? tf_reference_units(ref, 112, 1, &u) : OPEN_NO_MODEL;
    CHECK(!rc && u.n > 0, "open 9: B reads 112:1 after A's take (%s, %d units)", open_rc(rc), u.n);
    rc = mb ? open_take(mb, x, &step) : OPEN_NO_MODEL;
    CHECK(mb && !rc, "open 9: and a take on B's Model works (%s at %s)", open_rc(rc), step);
    if (ma) tf_model_close(ma);
    if (mb) tf_model_close(mb);
    if (b) tf_client_destroy(b);
  }
  /* 10: on a UI thread, no wait */
  tf__open_wait_s = 3;
  {
    snprintf(w, sizeof w, "%s/open10", work);
    TfClient *b = NULL;
    TfModel *ma = open_a(w, x, &b);
    TfSession *sa = ma ? tf_session_create(ma, NULL) : NULL;
    const int pushed = sa ? open_push(sa, x, 8000) : OPEN_NO_MODEL;
    tf__ui_thread_hook = open_ui_thread;
    double t0 = tf__now_s();
    TfModel *mb = b ? tf_client_model(b, NULL) : NULL;
    double dt = tf__now_s() - t0;
    const int orc = mb ? 0 : tf_last_error_code();
    CHECK(sa && !pushed && !mb && orc == TF_ERR_SESSION_OPEN && dt < 1.0,
          "open 10: on a UI thread, B's open while A's take runs is session_open at once (%s after %.2f s; off it the wait is up to 3 s)", open_rc(orc), dt);
    int rc = sa ? open_push(sa, x + 8000, 8000) : OPEN_NO_MODEL;
    if (!rc) rc = open_finish(sa);
    CHECK(sa && !rc, "open 10: A's take goes on and ends normally (%s)", open_rc(rc));
    if (sa) tf_session_destroy(sa);
    if (mb) tf_model_close(mb);
    t0 = tf__now_s();
    mb = b ? tf_client_model(b, NULL) : NULL;
    dt = tf__now_s() - t0;
    tf__ui_thread_hook = NULL;
    CHECK(mb != NULL, "open 10: on a UI thread, once A's take ended, B opens (%s in %.2f s)", mb ? "ok" : open_rc(tf_last_error_code()), dt);
    if (ma) tf_model_close(ma);
    if (mb) tf_model_close(mb);
    if (b) tf_client_destroy(b);
  }
#ifdef _WIN32
  /* 11 (Windows): IsGUIThread itself, no hook */
  {
    const int console_ui = tf__ui_thread();
    snprintf(w, sizeof w, "%s/open11", work);
    TfClient *b = NULL;
    TfModel *ma = open_a(w, x, &b);
    TfSession *sa = ma ? tf_session_create(ma, NULL) : NULL;
    const int pushed = sa ? open_push(sa, x, 8000) : OPEN_NO_MODEL;
    OpenGui g = {b, NULL, -1, 0, 0};
    TfThread th;
    const int started = sa && !pushed && !tf_thread_start(&th, open_gui, &g);
    if (started) tf_thread_join(th);
    CHECK(!console_ui && started && g.ui == 1 && !g.mb && g.rc == TF_ERR_SESSION_OPEN && g.dt < 1.0,
          "open 11: the console thread is no UI thread (%d); on a thread made a GUI thread (IsGUIThread %d), B's open while A's take "
          "runs is session_open at once (%s after %.2f s)",
          console_ui, g.ui, open_rc(g.rc), g.dt);
    int rc = sa ? open_push(sa, x + 8000, 8000) : OPEN_NO_MODEL;
    if (!rc) rc = open_finish(sa);
    CHECK(sa && !rc, "open 11: A's take goes on and ends normally (%s)", open_rc(rc));
    if (sa) tf_session_destroy(sa);
    if (g.mb) tf_model_close(g.mb);
    if (ma) tf_model_close(ma);
    if (b) tf_client_destroy(b);
  }
#endif
  tf__open_wait_s = 3;
  free(x);
  tf_json_free(fx);
  return done("lifetime_open");
}
