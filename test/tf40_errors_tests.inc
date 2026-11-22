/* Error reports, included by tf40_tests.c.
 *
 *   tf40_tests errors      <test/data/sdk_4.0.0> <work>   vectors/errors_40.json (a copy of the contract's at adca6f6):
 *                                           every `causes` row (24.13, 4.0.2) through a raised error and
 *                                           tf_last_error_cause; every `codes` case through tf__rep_code_for and tf__rep_reported, every
 *                                           `queue` sequence through the queue machine (each occurrence's made, each
 *                                           send point's requests and their fields), every `telemetry` reason, every
 *                                           `signatures` string and hex; the queue's file read back; a body's closed
 *                                           schema
 *   tf40_tests errors_flow <fixture> <work>  each stage this SDK reports, from the failure itself, against the stand-in
 *                                           server (make_flow40.py): the code the app gets and the report it sends */

static int jbool(const TfJson *o, const char *k) {
  const TfJson *v = tf_json_get(o, k);
  return v && v->type == TJ_BOOL && v->boolean;
}
static const char *jstr0(const TfJson *o, const char *k) { return tf_json_str(tf_json_get(o, k)); }
static int jnum0(const TfJson *o, const char *k) {
  const TfJson *v = tf_json_get(o, k);
  return v && v->type == TJ_NUM ? (int)v->num : 0;
}

/* the answers of one send point, in order (null: no answer); -1 when they ran out */
typedef struct {
  const TfJson *a;
  int at;
} Answers;
static int answers_post(void *user, const TfRepItem *it) {
  (void)it;
  Answers *x = (Answers *)user;
  if (!x->a || x->at >= x->a->n) return -1;
  const TfJson *v = &x->a->kids[x->at++];
  return v->type == TJ_NUM ? (int)v->num : 0;
}

/* 24.13 (4.0.2) */
static int code_of(const char *name) {
  for (int c = -1; c >= TF_ERR_SHARING_UNAVAILABLE; --c)
    if (tf_error_name(c) && !strcmp(tf_error_name(c), name)) return c;
  return 0;
}
static int same_or_null(const char *got, const char *want) { return want ? got && !strcmp(got, want) : got == NULL; }
/* the cause the app's error carries against a row's `cause` object (NULL or JSON null: none) */
static int cause_is(int has, const TfErrorCause *got, const TfJson *want) {
  if (!want || want->type == TJ_NULL) return !has;
  return has && same_or_null(got->stage, jstr0(want, "stage")) && same_or_null(got->reason, jstr0(want, "reason")) &&
         got->http_status == jnum0(want, "http_status") && same_or_null(got->error_name, jstr0(want, "error_name")) &&
         same_or_null(got->error_code, jstr0(want, "error_code")) && same_or_null(got->needs, jstr0(want, "needs"));
}
static void other_thread_fails(void *u) {
  (void)u;
  tf__clear_error();
  tf__cause("storage", "quota", 0, NULL, NULL);
  tf__raise(tf__fail(TF_ERR_STORAGE_FULL, "full"));
}

static int t_errors(const char *dir, const char *work) {
  (void)work;
  TfJson *v = load_json(dir, "errors_40.json");
  if (!v) return 2;
  const TfJson *lim = tf_json_get(v, "limits");
  CHECK(jnum0(lim, "daily_signatures") == TF_REP_DAILY && jnum0(lim, "queue_max") == TF_REP_QUEUE && jnum0(lim, "reported_max") == TF_REP_REPORTED &&
            jnum0(lim, "first_again_days") == TF_REP_FIRST_AGAIN && jnum0(lim, "keep_days") == TF_REP_KEEP && jnum0(lim, "count_max") == TF_REP_COUNT_MAX,
        "errors: the limits are 24.5's");
  /* 24.4: the code the app gets, and whether a failure makes a report */
  const TfJson *codes = tf_json_get(v, "codes");
  int ok_codes = 0;
  for (int i = 0; codes && i < codes->n; ++i) {
    const TfJson *k = &codes->kids[i];
    const char *od = jstr0(k, "on_device");
    const int on = !strcmp(od, "none") ? TF_REP_NONE : !strcmp(od, "partial") ? TF_REP_PARTIAL : TF_REP_COMPLETE;
    const char *got = tf__rep_code_for(jstr0(k, "stage"), jstr0(k, "reason"), jnum0(k, "http_status"), on, jstr0(k, "error_code"));
    const int rep = tf__rep_reported(jstr0(k, "reason"), got);
    const int good = !strcmp(got, jstr0(k, "code")) && rep == jbool(k, "reported");
    CHECK(good, "codes[%d] %s/%s %s: %s (reported %d), want %s", i, jstr0(k, "stage"), jstr0(k, "reason"), od, got, rep, jstr0(k, "code"));
    ok_codes += good;
  }
  /* 24.3: a time limit reached before any answer is `network` (and so needs_connection by 24.4 rule 1); an answer that
   * began and stalled is `timeout` (download_failed by rule 2). Stage B (staging, 11-18): a lease or config request with
   * no answer read `timeout`, its code did not fit the call's needs_connection and the report read download, network */
  {
    tf__clear_error();
    tf__http_note(TF__FAIL_TIMEOUT, "ERROR_WINHTTP_TIMEOUT", 12002);
    tf__cause_transport("lease", 0);
    const int before = !strcmp(tf__cause_reason(), "network");
    tf__http_note(TF__FAIL_TIMEOUT, "ERROR_WINHTTP_TIMEOUT", 12002);
    tf__cause_transport("lease", 200);
    const int began = !strcmp(tf__cause_reason(), "timeout");
    tf__clear_error();
    CHECK(before && began && !strcmp(tf__rep_code_for("lease", "network", 0, TF_REP_NONE, ""), "needs_connection"),
          "errors: a timeout before any answer is network (needs_connection), after an answer began timeout");
  }
  /* 24.13 (4.0.2): `causes`, each occurrence the way a real failure goes: the cause recorded where it is seen (with
   * whether every file the call needs is on the device), the code 24.4 gives it set on the thread, the error raised
   * to the app; then tf_last_error_cause gives the row's cause, or none. No Client is attached here, so no report is
   * made or queued: the cause does not depend on the reports. */
  const TfJson *causes = tf_json_get(v, "causes");
  int ok_causes = 0;
  for (int i = 0; causes && i < causes->n; ++i) {
    const TfJson *k = &causes->kids[i];
    const char *od = jstr0(k, "on_device");
    const int on = !strcmp(od, "none") ? TF_REP_NONE : !strcmp(od, "partial") ? TF_REP_PARTIAL : TF_REP_COMPLETE;
    tf__clear_error();
    const int was = tf__cause_files_here(on == TF_REP_COMPLETE);
    tf__cause(jstr0(k, "stage"), jstr0(k, "reason"), jnum0(k, "http_status"), jstr0(k, "error_name"), jstr0(k, "error_code"));
    tf__cause_files_here(was);
    const char *name = tf__rep_code_for(jstr0(k, "stage"), jstr0(k, "reason"), jnum0(k, "http_status"), on, jstr0(k, "error_code"));
    if (strcmp(name, "none")) tf__raise(tf__fail(code_of(name), "x"));
    TfErrorCause got;
    const int has = tf_last_error_cause(&got);
    const int good = !strcmp(name, jstr0(k, "code")) && (strcmp(name, "none") || !tf_last_error_code()) && cause_is(has, &got, tf_json_get(k, "cause"));
    CHECK(good, "causes[%d] '%s': %s, cause %s %s/%s %d %s %s needs %s", i, jstr0(k, "label"), name, has ? "yes" : "none", got.stage ? got.stage : "-",
          got.reason ? got.reason : "-", got.http_status, got.error_name ? got.error_name : "-", got.error_code ? got.error_code : "-",
          got.needs ? got.needs : "-");
    ok_causes += good;
  }
  CHECK(causes && causes->n == 15, "causes: the contract's 15 rows (%d)", causes ? causes->n : 0);
  /* the codes of a call used wrongly carry none; NULL is 0; a failure never handed to the app shows no older cause;
   * the strings stay as they were after a call that succeeds, until the thread's next failure; another thread's
   * failure is its own */
  {
    TfErrorCause a, b;
    tf__clear_error();
    tf__raise(tf__fail(TF_ERR_INVALID_ARGUMENT, "x"));
    CHECK(!tf_last_error_cause(&a) && !a.stage && !tf_last_error_cause(NULL), "causes: invalid_argument has none, NULL is 0");
    tf__clear_error();
    tf__cause_answer("download", 503, "{}", 2);
    tf__raise(tf__fail(TF_ERR_DOWNLOAD_FAILED, "x"));
    const int had = tf_last_error_cause(&a);
    tf__clear_error(); /* the next call, which succeeds */
    const int after = tf_last_error_cause(&b);
    CHECK(had && !after && !strcmp(a.stage, "download") && !strcmp(a.reason, "http") && a.http_status == 503 && !a.needs,
          "causes: a 503 of a file, then a call that succeeds: no cause, and the strings read as before (%s/%s)", a.stage, a.reason);
    tf__fail(TF_ERR_DOWNLOAD_FAILED, "x"); /* set, never raised */
    CHECK(!tf_last_error_cause(&b), "causes: an error that never reached the app shows no older cause");
    tf__clear_error();
    tf__cause("config", "network", 0, NULL, NULL);
    tf__raise(tf__fail(TF_ERR_NEEDS_CONNECTION, NULL));
    TfThread th;
    const int started = !tf_thread_start(&th, other_thread_fails, NULL);
    if (started) tf_thread_join(th);
    CHECK(started && tf_last_error_cause(&b) && !strcmp(b.stage, "config") && !strcmp(b.reason, "network") && b.needs && !strcmp(b.needs, "model") &&
              tf_last_error_code() == TF_ERR_NEEDS_CONNECTION,
          "causes: this thread's needs_connection (config, network, needs model) whatever another thread's failure");
    tf__clear_error();
  }
  /* 24.5: the signature string and its 32 hex */
  const TfJson *sigs = tf_json_get(v, "signatures");
  int ok_sigs = 0;
  for (int i = 0; sigs && i < sigs->n; ++i) {
    const TfJson *k = &sigs->kids[i];
    char s[200], hex[33];
    tf__rep_signature(jstr0(k, "stage"), jstr0(k, "reason"), jstr0(k, "code"), jnum0(k, "http_status"), s, sizeof s);
    tf__rep_signature_hex(jstr0(k, "stage"), jstr0(k, "reason"), jstr0(k, "code"), jnum0(k, "http_status"), hex);
    const int good = !strcmp(s, jstr0(k, "string")) && !strcmp(hex, jstr0(k, "hex"));
    CHECK(good, "signatures[%d]: %s", i, hex);
    ok_sigs += good;
  }
  /* 24.8: a failed download's failure_reasons name */
  const TfJson *tel = tf_json_get(v, "telemetry");
  int ok_tel = 0;
  for (int i = 0; tel && i < tel->n; ++i) {
    const int good = !strcmp(tf__rep_failure_reason(jstr0(&tel->kids[i], "reason")), jstr0(&tel->kids[i], "failure_reason"));
    CHECK(good, "telemetry[%d] %s", i, jstr0(&tel->kids[i], "reason"));
    ok_tel += good;
  }
  /* the buckets of storage_free (24.7; a native SDK buckets its volume's free space the same way) */
  const TfJson *sf = tf_json_get(tf_json_get(v, "web"), "storage_free");
  for (int i = 0; sf && i < sf->n; ++i) {
    const TfJson *b = tf_json_get(&sf->kids[i], "bytes");
    CHECK(b && !strcmp(tf__rep_storage_free((long long)b->num), jstr0(&sf->kids[i], "storage_free")), "storage_free[%d]", i);
  }
  /* 24.5, 24.6: every sequence of occurrences and send points on its fake clock; after each sequence the state read
   * back from its file is the same state */
  const TfJson *qs = tf_json_get(v, "queue");
  int ok_q = 0;
  TfRepState *r = (TfRepState *)malloc(sizeof *r), *back = (TfRepState *)malloc(sizeof *back);
  TfRepItem *made = (TfRepItem *)malloc(sizeof *made * 64);
  for (int i = 0; r && back && made && qs && i < qs->n; ++i) {
    const TfJson *steps = tf_json_get(&qs->kids[i], "steps");
    const char *name = jstr0(&qs->kids[i], "name");
    memset(r, 0, sizeof *r);
    const int before = fails;
    for (int k = 0; steps && k < steps->n; ++k) {
      const TfJson *s = &steps->kids[k], *ex = tf_json_get(s, "expect");
      const char *op = jstr0(s, "op");
      if (!strcmp(op, "occur")) {
        TfRepOcc o;
        memset(&o, 0, sizeof o);
        o.stage = jstr0(s, "stage"), o.reason = jstr0(s, "reason"), o.code = jstr0(s, "code");
        o.status = jnum0(s, "http_status"), o.first_launch = jbool(s, "first_launch"), o.retries = -1;
        const int m = tf__rep_occur(r, jstr0(s, "day"), &o);
        CHECK(m == jbool(ex, "made"), "queue '%s' step %d: made %d", name, k, m);
      } else if (!strcmp(op, "issues_off")) {
        tf__rep_off(r, 1);
      } else {
        Answers a = {tf_json_get(s, "answers"), 0};
        const int n = tf__rep_send_queue(r, jstr0(s, "day"), answers_post, &a, made, 64);
        const TfJson *want = tf_json_get(ex, "requests");
        CHECK(want && n == want->n, "queue '%s' step %d: %d requests, want %d", name, k, n, want ? want->n : -1);
        for (int j = 0; want && j < n && j < want->n; ++j) {
          const TfJson *w = &want->kids[j];
          const TfRepItem *it = &made[j];
          CHECK(!strcmp(it->day, jstr0(w, "day")) && !strcmp(it->stage, jstr0(w, "stage")) && !strcmp(it->reason, jstr0(w, "reason")) &&
                    !strcmp(it->code, jstr0(w, "code")) && it->status == jnum0(w, "http_status") && it->count == jnum0(w, "count") &&
                    it->first_report == jbool(w, "first_report") && it->first_launch == jbool(w, "first_launch") && it->answer == jnum0(w, "answer"),
                "queue '%s' step %d request %d: %s %s/%s count %d first_report %d first_launch %d answer %d", name, k, j, it->day, it->stage,
                it->reason, it->count, it->first_report, it->first_launch, it->answer);
        }
      }
    }
    char *t1 = tf__rep_to_json(r);
    memset(back, 0, sizeof *back);
    tf__rep_from_json(back, t1, t1 ? strlen(t1) : 0);
    back->off = r->off;
    char *t2 = tf__rep_to_json(back);
    CHECK(t1 && t2 && !strcmp(t1, t2), "queue '%s': the state read back from errors.json differs", name);
    free(t1);
    free(t2);
    ok_q += fails == before;
  }
  /* 24.2: what is not a token, or not of its closed set, is left out, never cut into one */
  if (r) {
    memset(r, 0, sizeof *r);
    TfRepOcc o = {"download", "http", "download_failed", 503, 1, 5, "WinHTTP", "12002", "8_16", "lt_5g", "4.1.0", "04c57bbbc6b225dfe6db7aa4a9afa1afd7ac51b3", NULL, 0};
    tf__rep_occur(r, "2026-11-18", &o);
    TfRepOcc bad = {"runtime", "memory", "engine_failed", 0, 0, -1, "could not allocate", "/data/x", "3g", "2g", "x", "abc", NULL, 0};
    tf__rep_occur(r, "2026-11-18", &bad);
    CHECK(r->nq == 2 && !r->q[1].name[0] && !r->q[1].ecode[0] && !r->q[1].ram[0] && !r->q[1].free_[0] && !r->q[1].revision[0] && !r->q[1].build[0],
          "a name, code, tier, bucket, version or build that is not a token or of its set is left out");
  }
  /* 24.3: a 403 lease_integrity_failed's error_code is why:check, why alone past 32 characters or
   * when check is not a token, nothing without a why (the caller uses lease_integrity_failed); only tokens are read */
  {
    char ec[33], d[400], long_check[40];
    memset(long_check, 'a', 25);
    long_check[25] = 0;
    CHECK(tf__integrity_code("verdict", "boot", ec) && !strcmp(ec, "verdict:boot"), "integrity: verdict:boot (%s)", ec);
    CHECK(tf__integrity_code("core", NULL, ec) && !strcmp(ec, "core"), "integrity: core (%s)", ec);
    CHECK(tf__integrity_code("downgrade", "", ec) && !strcmp(ec, "downgrade"), "integrity: downgrade (%s)", ec);
    CHECK(tf__integrity_code("verdict", long_check, ec) && !strcmp(ec, "verdict"), "integrity: past 32 characters, why alone (%s)", ec);
    long_check[24] = 0;
    CHECK(tf__integrity_code("verdict", long_check, ec) && strlen(ec) == 32, "integrity: exactly 32, both (%s)", ec);
    CHECK(tf__integrity_code("verdict", "Boot check!", ec) && !strcmp(ec, "verdict"), "integrity: a check that is not a token is left out (%s)", ec);
    CHECK(!tf__integrity_code("not a token", "boot", ec) && !tf__integrity_code(NULL, "boot", ec) && !ec[0], "integrity: no why, no code");
    tf__integrity_detail("verdict", "boot", d, sizeof d);
    CHECK(!strcmp(d, "this phone's bootloader is unlocked or its boot is not verified, and Quran Lab gives such a phone no lease (lease_integrity_failed, why verdict, check boot)"),
          "integrity: the words of verdict:boot: %s", d);
    tf__integrity_detail("core", NULL, d, sizeof d);
    CHECK(!strcmp(d, "the Quran Lab Core in this app could not be verified (lease_integrity_failed, why core)"), "integrity: the words of core: %s", d);
    tf__integrity_detail("<script>", "x y", d, sizeof d);
    CHECK(!strcmp(d, "the install could not be verified (lease_integrity_failed)"), "integrity: free text is never echoed: %s", d);
    const char *a1 = "{\"error\":\"lease_integrity_failed\",\"why\":\"verdict\",\"check\":\"digest\"}";
    const char *why = tf__rep_answer_reason(403, a1, strlen(a1), ec);
    CHECK(!strcmp(why, "integrity_failed") && !strcmp(ec, "verdict:digest"), "integrity: the answer's reason and code (%s, %s)", why, ec);
    const char *a2 = "{\"error\":\"lease_integrity_failed\"}";
    why = tf__rep_answer_reason(403, a2, strlen(a2), ec);
    CHECK(!strcmp(why, "integrity_failed") && !strcmp(ec, "lease_integrity_failed"), "integrity: no why: the error (%s, %s)", why, ec);
    const char *a3 = "{\"error\":\"lease_attestation_required\"}";
    why = tf__rep_answer_reason(403, a3, strlen(a3), ec);
    CHECK(!strcmp(why, "refused") && !strcmp(ec, "lease_attestation_required"), "integrity: another refusal as before (%s, %s)", why, ec);
    CHECK(!strcmp(tf__rep_code_for("attest", "integrity_failed", 403, TF_REP_NONE, "verdict:boot"), "server_rejected") &&
              !strcmp(tf__rep_code_for("lease", "integrity_failed", 403, TF_REP_NONE, "core"), "server_rejected") &&
              !strcmp(tf__rep_code_for("attest", "integrity_failed", 403, TF_REP_COMPLETE, "verdict:boot"), "none"),
          "integrity: server_rejected at the attest and the lease; with the build on the device the attest goes on (rule 3a)");
  }
  printf("  codes %d of %d, signatures %d of %d, telemetry %d of %d, queue sequences %d of %d, causes %d of %d\n", ok_codes, codes ? codes->n : 0,
         ok_sigs, sigs ? sigs->n : 0, ok_tel, tel ? tel->n : 0, ok_q, qs ? qs->n : 0, ok_causes, causes ? causes->n : 0);
  free(r);
  free(back);
  free(made);
  tf_json_free(v);
  return done("errors");
}

/* ------------------------------------------------------------------ each stage, from the failure itself */
/* The report the stand-in took for stage, reason and code (24.2's closed body checked), or NULL */
static TfJson *took(const char *stage, const char *reason, const char *code) {
  char want[160];
  snprintf(want, sizeof want, "\"code\":\"%s\",\"error\":{\"stage\":\"%s\",\"reason\":\"%s\"", code, stage, reason);
  for (int i = 0; i < g_f.n_err; ++i) {
    if (!strstr(g_f.err_bodies[i], want)) continue;
    if (getenv("TF_ERRORS_PRINT")) fprintf(stderr, "report: %s\n", g_f.err_bodies[i]); /* to read what is sent */
    TfJson *b = tf_json_parse(g_f.err_bodies[i], strlen(g_f.err_bodies[i]), NULL);
    static const char *const top[] = {"day", "platform", "sdk_version", "app_version", "os_version", "device_class", "revision", "kind", "code", "error", "backend"};
    /* 4.0.1 (ENDPOINTS_40.md 28.7): host_role and fallback on a report whose cause is a request to Quran Lab's hosts */
    static const char *const err[] = {"stage", "reason", "http_status", "error_name", "error_code", "count", "retries", "first_launch", "first_report",
                                      "build", "device_model", "ram_tier", "storage_free", "abi", "host_role", "fallback"};
    int closed = b && b->type == TJ_OBJ;
    for (int k = 0; closed && k < b->n; ++k) {
      int in = 0;
      for (size_t t = 0; t < sizeof top / sizeof top[0]; ++t) in |= !strcmp(b->kids[k].key, top[t]);
      closed = in;
    }
    const TfJson *e = tf_json_get(b, "error");
    for (int k = 0; closed && e && k < e->n; ++k) {
      int in = 0;
      for (size_t t = 0; t < sizeof err / sizeof err[0]; ++t) in |= !strcmp(e->kids[k].key, err[t]);
      closed = in;
    }
    for (size_t t = 0; closed && t < 9; ++t) closed = top[t][0] == 'r' || tf_json_get(b, top[t]) != NULL; /* the required ones */
    CHECK(closed && e && !strcmp(jstr0(b, "kind"), "error") && !strcmp(jstr0(b, "sdk_version"), TF_SDK_VERSION) && jstr0(e, "abi"),
          "the report is 24.2's closed body: %s", g_f.err_bodies[i]);
    return b;
  }
  for (int i = 0; i < g_f.n_err; ++i) fprintf(stderr, "  report taken: %s\n", g_f.err_bodies[i]);
  return NULL;
}

/* 28.7: a report's host_role and fallback (role NULL: neither is there) */
static int role_is(const TfJson *b, const char *role, int fallback) {
  const TfJson *e = tf_json_get(b, "error"), *fb = tf_json_get(e, "fallback");
  const char *r = jstr0(e, "host_role");
  if (!role) return !r && !fb;
  return r && !strcmp(r, role) && fb && fb->type == TJ_BOOL && fb->boolean == fallback;
}
/* the report's role and fallback checked, then freed */
static void role_free(TfJson *b, const char *label, const char *role, int fallback) {
  const TfJson *e = tf_json_get(b, "error");
  CHECK(b && role_is(b, role, fallback), "%s: host_role %s, fallback %s (sent %s, %s)", label, role ? role : "none", fallback ? "true" : "false",
        jstr0(e, "host_role") ? jstr0(e, "host_role") : "none", tf_json_get(e, "fallback") ? (jbool(e, "fallback") ? "true" : "false") : "none");
  tf_json_free(b);
}
/* One failure of a model call against the stand-in: the code the app gets and the report sent at the Client's end */
static TfJson *model_fails(const char *work, const char *label, int want, const char *stage, const char *reason, int status) {
  char w[1100];
  snprintf(w, sizeof w, "%s/%s", work, label);
  TfClient *c = flow_client(w);
  TfModel *m = c ? tf_client_model(c, NULL) : NULL;
  const int got = tf_last_error_code();
  CHECK(!m && got == want, "%s: %s (%s), want %s", label, tf_error_name(got), tf_last_error() ? tf_last_error() : "", tf_error_name(want));
  CHECK(tf__rep_due() > 0, "%s: a new report is sent 30 s later (24.6)", label);
  /* 24.13 (4.0.2): the cause on the app's error, kept to hold against the report the stand-in takes */
  TfErrorCause ec;
  const int has = tf_last_error_cause(&ec);
  char cs[5][64];
  const char *const cf[5] = {ec.stage, ec.reason, ec.error_name, ec.error_code, ec.needs};
  for (int i = 0; i < 5; ++i) snprintf(cs[i], sizeof cs[i], "%s", cf[i] ? cf[i] : "");
  if (m) tf_model_close(m);
  g_f.fail_url = NULL; /* the server answers again (a refused token would keep the report for the next launch) */
  if (c) tf_client_destroy(c); /* a send point */
  TfJson *b = took(stage, reason, tf_error_name(want));
  const TfJson *e = tf_json_get(b, "error");
  const char *rn = jstr0(e, "error_name"), *rc = jstr0(e, "error_code");
  CHECK(has && b && !strcmp(cs[0], stage) && !strcmp(cs[1], reason) && ec.http_status == jnum0(e, "http_status") && !strcmp(cs[2], rn ? rn : "") &&
            !strcmp(cs[3], rc ? rc : "") && !strcmp(cs[4], want == TF_ERR_NEEDS_CONNECTION ? "model" : ""),
        "%s: the app's error carries the report's cause (24.13): %s/%s %d %s %s needs %s", label, cs[0], cs[1], ec.http_status, cs[2], cs[3], cs[4]);
  CHECK(b && jnum0(e, "http_status") == status && jnum0(e, "count") == 1 && jbool(e, "first_report") && jbool(e, "first_launch") &&
            !strcmp(jstr0(b, "revision") ? jstr0(b, "revision") : "", !strcmp(stage, "config") || !strcmp(stage, "attest") ? "" : "4.1.0"),
        "%s: the report of %s/%s %d", label, stage, reason, status);
  return b;
}
/* 24.3: a 403 lease_integrity_failed at the lease or the install's attest: server_rejected, the
 * message naming the check in words and tokens (never the answer's own message), the report's reason integrity_failed
 * and error_code why:check; the signature (stage, reason, code, status) as before */
static void integrity_case(const char *work, const char *label, const char *url, const char *with, const char *body, const char *stage,
                           const char *want_ec, const char *want_words) {
  fake_reset(LEASE_OK);
  g_f.fail_url = url, g_f.fail_status = 403, g_f.fail_body = body, g_f.fail_with = with;
  char w[1100], msg[700];
  snprintf(w, sizeof w, "%s/%s", work, label);
  TfClient *c = flow_client(w);
  TfModel *m = c ? tf_client_model(c, NULL) : NULL;
  const int got = tf_last_error_code();
  snprintf(msg, sizeof msg, "%s", tf_last_error() ? tf_last_error() : "");
  CHECK(!m && got == TF_ERR_SERVER_REJECTED, "%s: %s (%s), want server_rejected", label, tf_error_name(got), msg);
  CHECK(strstr(msg, "Quran Lab refused this app") && strstr(msg, want_words) && !strstr(msg, "free text"), "%s: the message names the check: %s", label, msg);
  if (m) tf_model_close(m);
  g_f.fail_url = NULL, g_f.fail_with = NULL;
  if (c) tf_client_destroy(c); /* a send point */
  TfJson *b = took(stage, "integrity_failed", "server_rejected");
  const TfJson *e = tf_json_get(b, "error");
  int clean = 1;
  for (int i = 0; i < g_f.n_err; ++i) clean &= !strstr(g_f.err_bodies[i], "free text");
  CHECK(b && jnum0(e, "http_status") == 403 && jstr0(e, "error_code") && !strcmp(jstr0(e, "error_code"), want_ec) && clean,
        "%s: the report %s/integrity_failed/server_rejected/403 with error_code %s (%s)", label, stage, want_ec, jstr0(e, "error_code") ? jstr0(e, "error_code") : "none");
  tf_json_free(b);
}
static int t_errors_flow(const char *fixture, const char *work, const char *errvec) {
  TfJson *fx = flow_setup(fixture);
  if (!fx) return 2;
  tf__download_pause_ms = 1;
  TfJson *b;
  /* config: a 5xx with no config on the device is download_failed (24.4), its cause the config's answer */
  fake_reset(LEASE_OK);
  g_f.fail_url = "/v1/config", g_f.fail_status = 503;
  role_free(model_fails(work, "config503", TF_ERR_DOWNLOAD_FAILED, "config", "http", 503), "config503", "api", 1);
  CHECK(g_f.api_asked[0] >= 1 && g_f.api_asked[1] >= 1 && g_f.api_asked[2] >= 1 && g_f.api_asked[3] >= 1,
        "config503: the 503 is unreachable, so every api host was asked (%d %d %d %d)", g_f.api_asked[0], g_f.api_asked[1], g_f.api_asked[2], g_f.api_asked[3]);
  /* no answer at all and nothing on the device: needs_connection, reported (it reached the app) */
  fake_reset(LEASE_OK);
  g_f.fail_url = "/v1/config", g_f.fail_status = 0;
  tf_json_free(model_fails(work, "confignet", TF_ERR_NEEDS_CONNECTION, "config", "network", 0));
  /* a config that does not verify (no signature): download_failed, not needs_connection */
  fake_reset(LEASE_OK);
  g_f.fail_url = "/v1/config", g_f.fail_status = 200, g_f.fail_body = "{\"v\":1}";
  tf_json_free(model_fails(work, "configsig", TF_ERR_DOWNLOAD_FAILED, "config", "signature", 200));
  /* lease: a 503 is download_failed (it read as server_rejected before 24.4) */
  fake_reset(LEASE_OK);
  g_f.fail_url = "/v1/model-key", g_f.fail_status = 503;
  tf_json_free(model_fails(work, "lease503", TF_ERR_DOWNLOAD_FAILED, "lease", "http", 503));
  /* 24.4 rule 3: a stop the Core verified is the stop (reason stopped, key_revoked: invalid_key); an unsigned one is
   * a refusal (server_rejected, reason refused with the server's error) */
  {
    fake_reset(LEASE_SIGNED_STOP_NOW);
    char w[1100];
    snprintf(w, sizeof w, "%s/leasestop", work);
    TfClient *c = flow_client(w);
    TfModel *m = c ? tf_client_model(c, NULL) : NULL;
    CHECK(!m && tf_last_error_code() == TF_ERR_INVALID_KEY, "leasestop: %s", tf_error_name(tf_last_error_code()));
    char *st = tf__rep_state_json();
    CHECK(st && strstr(st, "\"stage\":\"lease\",\"reason\":\"stopped\",\"code\":\"invalid_key\",\"status\":401"), "leasestop: the report queued: %s",
          st ? st : "none");
    free(st);
    if (c) tf_client_destroy(c);
    CHECK(g_f.n_err == 0, "leasestop: the stop ends this launch's calls, so the report waits for the next launch (24.6)");
  }
  fake_reset(LEASE_UNSIGNED_STOP);
  role_free(model_fails(work, "leaseunsigned", TF_ERR_SERVER_REJECTED, "lease", "refused", 401), "leaseunsigned", "api", 0);
  /* 24.3 (69326fb): an integrity refusal names its check, at the lease and at the install's attest */
  integrity_case(work, "integrity_core", "/v1/model-key", NULL,
                 "{\"error\":\"lease_integrity_failed\",\"message\":\"free text the SDK never echoes\",\"why\":\"core\"}", "lease", "core",
                 "the Quran Lab Core in this app could not be verified (lease_integrity_failed, why core)");
  integrity_case(work, "integrity_boot", "/v1/model-key", NULL, "{\"error\":\"lease_integrity_failed\",\"why\":\"verdict\",\"check\":\"boot\"}", "lease",
                 "verdict:boot", "bootloader is unlocked or its boot is not verified, and Quran Lab gives such a phone no lease (lease_integrity_failed, why verdict, check boot)");
  integrity_case(work, "integrity_downgrade", "/v1/model-key", NULL, "{\"error\":\"lease_integrity_failed\",\"why\":\"downgrade\"}", "lease", "downgrade",
                 "this install attested with a stronger check before and did not this time (lease_integrity_failed, why downgrade)");
  integrity_case(work, "integrity_nowhy", "/v1/model-key", NULL, "{\"error\":\"lease_integrity_failed\"}", "lease", "lease_integrity_failed",
                 "the install could not be verified (lease_integrity_failed)");
  integrity_case(work, "integrity_attest", "/v1/attest", "install_key", "{\"error\":\"lease_integrity_failed\",\"why\":\"verdict\",\"check\":\"boot\"}",
                 "attest", "verdict:boot", "(lease_integrity_failed, why verdict, check boot)");
  /* the route off: model_unavailable, the lease's 404 its cause */
  fake_reset(LEASE_ROUTE_OFF);
  tf_json_free(model_fails(work, "leaseoff", TF_ERR_MODEL_UNAVAILABLE, "lease", "http", 404));
  /* manifest: a 503 and a redirect, each download_failed, never followed */
  fake_reset(LEASE_OK);
  g_f.fail_url = "manifest.json", g_f.fail_status = 503;
  role_free(model_fails(work, "manifest503", TF_ERR_DOWNLOAD_FAILED, "manifest", "http", 503), "manifest503", "models", 1);
  fake_reset(LEASE_OK);
  g_f.fail_url = "manifest.json", g_f.fail_status = 302;
  tf_json_free(model_fails(work, "manifest302", TF_ERR_DOWNLOAD_FAILED, "manifest", "redirect", 302));
  /* download: a file answered 503 every time is resumed five times (16.2), then download_failed with retries 5; the
   * day's telemetry counts the failed download under failure_reasons.http (24.8) */
  {
    fake_reset(LEASE_OK);
    g_f.fail_url = ".qle", g_f.fail_status = 503;
    char w[1100];
    snprintf(w, sizeof w, "%s/file503", work);
    TfClient *c = flow_client(w);
    TfModel *m = c ? tf_client_model(c, NULL) : NULL;
    CHECK(!m && tf_last_error_code() == TF_ERR_DOWNLOAD_FAILED, "file503: %s", tf_error_name(tf_last_error_code()));
    char *tb = c ? tf__client_day_body(c) : NULL;
    const char *fr = tb ? strstr(tb, "\"failure_reasons\":{") : NULL;
    CHECK(fr && strstr(fr, "\"http\":1") && strstr(tb, "\"failed\":1"), "file503: the failed download in telemetry: %s", tb ? tb : "(none)");
    free(tb);
    if (c) tf_client_destroy(c);
    g_f.fail_url = NULL;
    b = took("download", "http", "download_failed");
    CHECK(b && jnum0(tf_json_get(b, "error"), "http_status") == 503 && jnum0(tf_json_get(b, "error"), "retries") == 5, "file503: the report, retries 5");
    tf_json_free(b);
  }
  /* a file that is not the one the manifest names, twice: checksum_mismatch, the download's hash */
  fake_reset(LEASE_OK);
  g_f.corrupt = 1;
  role_free(model_fails(work, "hash", TF_ERR_CHECKSUM_MISMATCH, "download", "hash", 0), "hash", "models", 0);
  /* the whole way, then the vector build's vocabulary refused at open: model_incompatible, the open's own */
  fake_reset(LEASE_OK);
  role_free(model_fails(work, "open", TF_ERR_MODEL_INCOMPATIBLE, "model_open", "incompatible", 0), "open", NULL, 0);
  /* attest: the app token refused with a 503 holds up the config: download_failed, the token's answer its cause */
  fake_reset(LEASE_OK);
  g_f.fail_url = "/v1/attest", g_f.fail_status = 503;
  tf_json_free(model_fails(work, "attest503", TF_ERR_DOWNLOAD_FAILED, "attest", "http", 503));
  /* 24.4 rule 1: offline with part of the build here is download_failed; with nothing of it, needs_connection */
  {
    fake_reset(LEASE_OK);
    g_f.fail_url = "manifest.json", g_f.fail_status = 503;
    char w[1100], p[1300];
    snprintf(w, sizeof w, "%s/partial", work);
    TfClient *c = flow_client(w);
    TfModel *m = c ? tf_client_model(c, NULL) : NULL; /* the config is kept, the build's folder and record are made */
    snprintf(p, sizeof p, "%s/models/%s/cdn", w, S(fx, "build"));
    tf_mkdirs(p);
    snprintf(p, sizeof p, "%s/models/%s/cdn/layers.zfw.qle.part", w, S(fx, "build"));
    tf_write_file(p, "part", 4);
    g_f.fail_url = "/v1/config", g_f.fail_status = 0;
    m = c ? tf_client_model(c, NULL) : NULL;
    CHECK(!m && tf_last_error_code() == TF_ERR_DOWNLOAD_FAILED, "partial: offline with part of the build is %s", tf_error_name(tf_last_error_code()));
    remove(p);
    m = c ? tf_client_model(c, NULL) : NULL;
    CHECK(!m && tf_last_error_code() == TF_ERR_NEEDS_CONNECTION, "partial: offline with none of it is %s", tf_error_name(tf_last_error_code()));
    TfErrorCause pc;
    CHECK(tf_last_error_cause(&pc) && !strcmp(pc.stage, "config") && !strcmp(pc.reason, "network") && pc.needs && !strcmp(pc.needs, "model"),
          "partial: offline with none of it, the cause config/network needs model (24.13)");
    if (c) tf_client_destroy(c);
    g_f.fail_url = NULL;
    b = took("config", "network", "download_failed");
    CHECK(b != NULL, "partial: the report of the offline call with part of the build");
    tf_json_free(b);
    b = took("config", "network", "needs_connection");
    CHECK(b != NULL, "partial: the report of the offline call with nothing of it");
    tf_json_free(b);
  }
  /* sharing: a call the config does not allow is sharing_unavailable, reported (it reached the app) */
  {
    fake_reset(LEASE_OK);
    char w[1100];
    snprintf(w, sizeof w, "%s/sharing", work);
    TfClient *c = flow_client(w);
    if (c) tf_client_model(c, NULL); /* the fixture's config: sharing false */
    CHECK(c && tf_sharing_set_on(c, 1) == TF_ERR_SHARING_UNAVAILABLE, "sharing: %s", tf_error_name(tf_last_error_code()));
    if (c) tf_client_destroy(c);
    b = took("sharing", "other", "sharing_unavailable");
    CHECK(b != NULL, "sharing: the report");
    tf_json_free(b);
  }
  /* a burst: the same failure three times in a day is one report with its count (24.5); the queue answered 503 is
   * kept, and the next send point sends it */
  {
    fake_reset(LEASE_OK);
    g_f.fail_url = "/v1/config", g_f.fail_status = 503;
    char w[1100];
    snprintf(w, sizeof w, "%s/burst", work);
    TfClient *c = flow_client(w);
    for (int i = 0; i < 3 && c; ++i) tf_client_model(c, NULL);
    g_f.issues_status = 503;
    if (c) tf__client_flush(c, 1);
    /* 4.0.1 (28.3): the send point's one request asks each api host once, in order (a 503 is unreachable), and the
     * report is kept when every host was */
    int in_order = g_f.n_err == N_API_HOSTS;
    for (int i = 0; in_order && i < N_API_HOSTS; ++i) in_order = g_f.err_host[i] == i;
    CHECK(in_order && tf__rep_due() == 0, "burst: one request at the send point, each api host asked once, kept on a 503 (%d)", g_f.n_err);
    g_f.n_err = 0;
    g_f.issues_status = 0;
    if (c) tf_client_destroy(c);
    g_f.fail_url = NULL;
    b = took("config", "http", "download_failed");
    CHECK(b && jnum0(tf_json_get(b, "error"), "count") == 3 && g_f.n_err == 1, "burst: one report, count 3 (%d sent)", g_f.n_err);
    tf_json_free(b);
  }
  /* silent failures (24.1 item 2), code none: a telemetry delta refused for good, a register answered 500 */
  {
    fake_reset(LEASE_OK);
    char w[1100];
    snprintf(w, sizeof w, "%s/silent", work);
    TfClient *c = flow_client(w);
    g_f.fail_url = "/v1/events", g_f.fail_status = 400, g_f.fail_body = "{\"error\":\"invalid_body\"}";
    if (c) tf__client_flush(c, 1);
    b = took("events", "http", "none");
    CHECK(role_is(b, "api", 0), "events: a refusal on the first host, host_role api, fallback false");
    CHECK(b && jnum0(tf_json_get(b, "error"), "http_status") == 400 && !strcmp(jstr0(tf_json_get(b, "error"), "error_code") ? jstr0(tf_json_get(b, "error"), "error_code") : "", "invalid_body"),
          "events: a delta refused with 400 is reported, with the server's error");
    tf_json_free(b);
    g_f.fail_url = "/v1/register", g_f.fail_status = 500, g_f.fail_body = NULL;
    if (c) tf__client_register(c);
    if (c) tf__client_flush(c, 1);
    b = took("register", "http", "none");
    CHECK(b && jnum0(tf_json_get(b, "error"), "http_status") == 500 && role_is(b, "api", 1), "register: a 500 at the launch is reported (every api host asked)");
    tf_json_free(b);
    g_f.fail_url = "/v1/register", g_f.fail_status = 0;
    const int before = g_f.n_err;
    if (c) tf__client_register(c);
    if (c) tf__client_flush(c, 1);
    CHECK(g_f.n_err == before, "register: a plain network failure in the background is never reported");
    g_f.fail_url = NULL;
    if (c) tf_client_destroy(c);
  }
  /* storage: a folder the SDK cannot create (a file in its place) is storage_failed, the store's write */
  {
    fake_reset(LEASE_OK);
    char w[1100], p[1200];
    snprintf(w, sizeof w, "%s/storage", work);
    TfClient *c = flow_client(w);
    snprintf(p, sizeof p, "%s/models", w);
    tf_write_file(p, "x", 1);
    TfModel *m = c ? tf_client_model(c, NULL) : NULL;
    CHECK(!m && tf_last_error_code() == TF_ERR_STORAGE_FAILED, "storage: %s", tf_error_name(tf_last_error_code()));
    if (c) tf_client_destroy(c);
    b = took("storage", "write", "storage_failed");
    CHECK(b != NULL, "storage: the report");
    tf_json_free(b);
  }
  /* 24.3 (4.0.1, Gate C, contract 7f097b9): a model open the Core fails, at the Core's boundary (tf__pm_open, the
   * open every model open makes, made to fail with each status of errors_40.json core_open) and reported as an app
   * call's failure is (tf__raise at the call's end). The flow's vector build is refused for its vocabulary before any
   * engine open, so the open is asked directly, on that install with its build and lease 1. The statuses C answers
   * engine_failed for carry the status's name as error_code (nomem: reason memory; engine: reason engine) and as
   * error_name; C answers QLC_E_BUSY with session_open (never reported) and QLC_E_ARG with model_invalid, which carry
   * no such error_code (core_open_are). */
  {
    TfJson *ev = errvec ? load_json(errvec, "errors_40.json") : NULL;
    const TfJson *co = tf_json_get(ev, "core_open");
    const char *mt = S(fx, "manifest");
    int seen = 0;
    for (int i = 0; co && i < co->n; ++i) {
      const TfJson *k = &co->kids[i];
      const int st = (int)N(k, "status");
      char label[64], w[1100], bd[1300];
      snprintf(label, sizeof label, "core_open_%s", S(k, "error_code"));
      snprintf(w, sizeof w, "%s/%s", work, label);
      snprintf(bd, sizeof bd, "%s/models/%s", w, S(fx, "build"));
      fake_reset(LEASE_OK);
      TfClient *c = flow_client(w);
      if (c) tf_client_model(c, NULL); /* the build installed and lease 1 held (its vocabulary refused after) */
      const int before = g_f.n_err;
      tf__clear_error();
      tf__pm_open_fail_status = st;
      tf__app_call++;
      qlc_model *zm = c ? tf__pm_open(tf__pm(c), bd, mt, strlen(mt), NULL, 1, 16000, 0) : NULL;
      const int got = tf_last_error_code();
      if (!zm && c) tf__client_note_error(c, tf__raise(got)); /* as an app call's end does */
      tf__app_call--;
      tf__pm_open_fail_status = 0;
      const int engine = st == QLC_E_NOMEM || st == QLC_E_ENGINE;
      const int want = engine ? TF_ERR_ENGINE_FAILED : st == QLC_E_BUSY ? TF_ERR_SESSION_OPEN : TF_ERR_MODEL_INVALID;
      CHECK(!zm && got == want && (!engine || !strcmp(S(k, "code"), "engine_failed")), "%s: C answers %s with %s (%s)", label, S(k, "name"),
            tf_error_name(want), tf_error_name(got));
      if (c) tf_client_destroy(c); /* a send point */
      if (engine) {
        b = took("model_open", S(k, "reason"), "engine_failed");
        const TfJson *e = tf_json_get(b, "error");
        CHECK(b && jstr0(e, "error_code") && !strcmp(jstr0(e, "error_code"), S(k, "error_code")) && jstr0(e, "error_name") &&
                  !strcmp(jstr0(e, "error_name"), S(k, "name")) && jstr0(b, "revision"),
              "%s: the report model_open/%s with error_code %s and error_name %s (sent %s, %s)", label, S(k, "reason"), S(k, "error_code"), S(k, "name"),
              jstr0(e, "error_code") ? jstr0(e, "error_code") : "none", jstr0(e, "error_name") ? jstr0(e, "error_name") : "none");
        role_free(b, label, NULL, 0);
      } else {
        int carries = 0, busy_sent = 0;
        for (int j = before; j < g_f.n_err; ++j) {
          carries |= strstr(g_f.err_bodies[j], "\"error_code\":\"busy\"") || strstr(g_f.err_bodies[j], "\"error_code\":\"arg\"");
          busy_sent |= strstr(g_f.err_bodies[j], "\"code\":\"session_open\"") != NULL;
        }
        CHECK(!carries && !busy_sent, "%s: no report carries busy or arg, and session_open is never reported", label);
      }
      seen++;
    }
    CHECK(!errvec || seen == 4, "core_open: the four statuses of errors_40.json replayed (%d)", seen);
    tf_json_free(ev);
  }
  /* the app's own misuse is never reported */
  {
    fake_reset(LEASE_OK);
    char w[1100];
    snprintf(w, sizeof w, "%s/misuse", work);
    TfClient *c = flow_client(w);
    TfPrefetchOptions po;
    memset(&po, 0, sizeof po);
    po.version = "not a version";
    CHECK(c && tf_client_prefetch(c, &po) == TF_ERR_INVALID_ARGUMENT && tf__rep_due() == 0, "misuse: invalid_argument makes no report");
    if (c) tf_client_destroy(c);
    CHECK(g_f.n_err == 0, "misuse: nothing sent (%d)", g_f.n_err);
  }
  tf__download_pause_ms = 1000;
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(fx);
  return done("errors_flow");
}
