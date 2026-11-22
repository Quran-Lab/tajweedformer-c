/* Section 33: nothing waits for Wi-Fi. Included by tf40_tests.c.
 *
 *   tf40_tests no_wait <flow40> <nowait40> <test/data/sdk_4.0.0> <work>
 *
 * Every case of vectors/no_wait_40.json that names c (its byte copy in test/data/sdk_4.0.0), against the stand-in
 * server, with the platform's connection report set to the case's (tf__connection_override: this library cannot tell
 * the network and reports unknown); with no network no request is answered. <nowait40> is the vector build with two
 * contexts and the tj3 vocabulary (make_flow40.py --contexts c16_l64,c24_l256): a Model opens on it, so a session on
 * c24_l256 fetches that context's files (the Core then refuses its engine: a vector build has no model data, so the
 * open goes as far as the engine and no further). <flow40> is the one-context build, the build on the device before
 * an update. Each case checks the requests the stand-in took, not only how the call ended. The upload case is
 * tf_tests upload's case M (the sharing queue's own stand-in). */

static int nw_asked(const char *needle) {
  int n = 0;
  tf_mutex_lock(&g_f_lock);
  for (int i = 0; i < g_f.n_req; ++i) n += strstr(g_f.req[i], needle) != NULL;
  tf_mutex_unlock(&g_f_lock);
  return n;
}
/* TF_NOWAIT_PRINT=1: the requests the stand-in took */
static void nw_print(const char *label) {
  if (!getenv("TF_NOWAIT_PRINT")) return;
  tf_mutex_lock(&g_f_lock);
  for (int i = 0; i < g_f.n_req; ++i) fprintf(stderr, "  %s: %s\n", label, g_f.req[i]);
  tf_mutex_unlock(&g_f_lock);
  fprintf(stderr, "  %s: last error %s: %s\n", label, tf_error_name(tf_last_error_code()), tf_last_error() ? tf_last_error() : "");
}
static void nw_forget(void) {
  tf_mutex_lock(&g_f_lock);
  g_f.n_req = 0;
  tf_mutex_unlock(&g_f_lock);
}
/* the case's network: what the platform reports, and with none no request answered */
static void nw_net(const char *conn) {
  tf__connection_override = !strcmp(conn, "unmetered") ? TF__CONNECTION_UNMETERED
                            : !strcmp(conn, "metered") ? TF__CONNECTION_METERED
                            : !strcmp(conn, "none")    ? TF__CONNECTION_NONE
                                                       : TF__CONNECTION_UNKNOWN;
  g_f.fail_url = !strcmp(conn, "none") ? "https://" : NULL;
  g_f.fail_status = 0;
}
/* how many of a context's 3 files (graph, graph data, frames) the stand-in was asked for, each at least once (a file
 * is asked of the model host's folder and of the lease's, as the stand-in serves both) */
static int nw_files(const char *context) {
  static const char *const names[] = {"model_engine2.onnx.zfg.qle", "model_engine2.onnx.zfg.bin.qle", "frames.zfw.qle"};
  int n = 0;
  for (int i = 0; i < 3; ++i) {
    char want[96];
    snprintf(want, sizeof want, "/%s/%s", context, names[i]);
    n += nw_asked(want) > 0;
  }
  return n;
}
/* a code that says the call stopped before the engine: the files, the network, the model's availability */
static int nw_before_engine(int code) {
  return code == TF_ERR_NEEDS_CONNECTION || code == TF_ERR_DOWNLOAD_FAILED || code == TF_ERR_MODEL_MISSING || code == TF_ERR_MODEL_UNAVAILABLE ||
         code == TF_ERR_CHECKSUM_MISMATCH || code == TF_ERR_STORAGE_FAILED;
}
/* the Model the app holds, as tf_client_model gives it, its engine opened at the first Session (a vector build's
 * engine is refused, so tf_client_model itself ends there) */
static TfModel *nw_model(TfClient *c) {
  TfFetch f;
  memset(&f, 0, sizeof f);
  char dir[1024];
  TfModelOpen o;
  memset(&o, 0, sizeof o);
  if (tf__store_fetch(c, &f, dir, sizeof dir, &o)) return NULL;
  o.no_engine = 1;
  return tf__model_open(dir, &o);
}
/* the device forms of a context's files under the build's folder */
static int nw_installed(const char *work, const char *build, const char *context) {
  static const char *const names[] = {"model_engine2.onnx.zfg", "model_engine2.onnx.zfg.bin", "frames.zfw"};
  int n = 0;
  for (int i = 0; i < 3; ++i) {
    char p[1400];
    snprintf(p, sizeof p, "%s/models/%s/dev/%s/%s.qld", work, build, context, names[i]);
    n += tf_file_exists(p);
  }
  return n;
}
static int nw_current(const char *work, const char *build) {
  char p[1200];
  snprintf(p, sizeof p, "%s/models/current.json", work);
  char *t = NULL;
  size_t n = 0;
  const int ok = !tf_read_file(p, &t, &n) && t && strstr(t, build);
  free(t);
  return ok;
}

/* 24.13 (4.0.2): the cause on the app's error of a case with no network, and the case's needs. With no network at all
 * the first request of this SDK that gets no answer is the install's token (stage attest, which gates the config,
 * the manifest's model token and the lease: 24.4 rule 3a), so that is the cause its report and its error name. */
static void nw_cause(const char *name, const TfJson *k, const char *stage) {
  TfErrorCause ec;
  const int has = tf_last_error_cause(&ec);
  CHECK(has && !strcmp(ec.stage, stage) && !strcmp(ec.reason, "network") && ec.http_status == 0 && ec.needs && !strcmp(ec.needs, S(k, "needs")),
        "%s: the cause %s/%s needs %s, want %s/network needs %s", name, has ? ec.stage : "none", has ? ec.reason : "", has && ec.needs ? ec.needs : "none",
        stage, S(k, "needs"));
}

static int t_no_wait(const char *flow40, const char *nowait40, const char *vecdir, const char *work) {
  TfJson *one = load_json(flow40, "flow.json"), *v = load_json(vecdir, "no_wait_40.json");
  TfJson *two = flow_setup(nowait40);
  if (!one || !two || !v) return 2;
  tf__download_pause_ms = 1;
  const char *b1 = S(one, "build"), *b2 = S(two, "build");
  const TfJson *cases = tf_json_get(v, "cases");
  int named = 0, replayed = 0;
  for (int i = 0; cases && i < cases->n; ++i) {
    const TfJson *k = &cases->kids[i], *pl = tf_json_get(k, "platforms");
    int forc = 0;
    for (int j = 0; pl && j < pl->n; ++j) forc |= tf_json_str(&pl->kids[j]) && !strcmp(pl->kids[j].str, "c");
    if (!forc) continue;
    named++;
    const char *name = S(k, "name"), *call = S(k, "call"), *conn = S(k, "connection"), *out = S(k, "outcome");
    const int none = !strcmp(conn, "none");
    CHECK(!strcmp(S(k, "network_condition"), "none"), "%s: the vector's requests carry no network condition", name);
    if (!strcmp(call, "upload")) {
      printf("  %s: replayed by tf_tests upload, case M\n", name);
      replayed++;
      continue;
    }
    char w[1100];
    snprintf(w, sizeof w, "%s/%s", work, name);
    fake_reset(LEASE_OK);
    g_f.fx = !strcmp(call, "update") ? one : two;
    TfClient *c = flow_client(w);
    CHECK(c != NULL, "%s: a client", name);
    if (!c) continue;
    int code = 0;
    if (!strcmp(call, "model") || !strcmp(call, "prefetch")) {
      /* a first install: the files of the default context, with the platform reporting the case's network */
      nw_net(conn);
      TfModel *m = NULL;
      if (!strcmp(call, "model")) {
        m = tf_client_model(c, NULL);
        code = m ? 0 : tf_last_error_code();
      } else {
        code = tf_client_prefetch(c, NULL);
      }
      nw_print(name);
      const int files = nw_files("c16_l64"), man = nw_asked("manifest.json");
      if (none) {
        CHECK(!strcmp(out, "error") && code == TF_ERR_NEEDS_CONNECTION && files == 0 && man == 0, "%s: no network: %s, %d files asked, want %s",
              name, tf_error_name(code), files, S(k, "code"));
        nw_cause(name, k, "attest");
      } else if (!strcmp(call, "model")) {
        CHECK(!strcmp(out, "opens") && !nw_before_engine(code) && man >= 1 && files == 3 && nw_current(w, b2),
              "%s: config, manifest and the context's 3 files asked (%d, %d), installed, then the engine (%s)", name, man, files,
              m ? "opened" : tf_error_name(code));
      } else {
        CHECK(!strcmp(out, "downloaded") && code == 0 && man >= 1 && files == 3 && nw_current(w, b2), "%s: downloaded on the %s network (%s, %d files)",
              name, conn, tf_error_name(code), files);
      }
      if (m) tf_model_close(m);
    } else if (!strcmp(call, "session")) {
      /* the 4.0.1 report: prefetch() installed the default context, then a session on c24_l256 */
      nw_net("unmetered");
      CHECK(tf_client_prefetch(c, NULL) == 0 && nw_files("c16_l64") == 3 && nw_asked("c24_l256/") == 0 && nw_installed(w, b2, "c16_l64") == 3 &&
                nw_installed(w, b2, "c24_l256") == 0,
            "%s: prefetch installed the default context only (%s)", name, tf_error_name(tf_last_error_code()));
      TfModel *m = nw_model(c);
      CHECK(m != NULL, "%s: the Model open on the default context (%s)", name, tf_error_name(tf_last_error_code()));
      nw_forget();
      nw_net(conn);
      TfSessionOptions so;
      memset(&so, 0, sizeof so);
      so.context = "c24_l256";
      TfSession *s = m ? tf_session_create(m, &so) : NULL;
      code = s ? 0 : tf_last_error_code();
      nw_print(name);
      const int files = nw_files("c24_l256");
      if (none) {
        CHECK(!s && code == TF_ERR_NEEDS_CONNECTION && nw_asked("c24_l256/") == 0 && nw_installed(w, b2, "c24_l256") == 0,
              "%s: another context's files with no network: %s (want %s), %d asked", name, tf_error_name(code), S(k, "code"), files);
        nw_cause(name, k, "attest");
      } else {
        CHECK(files == 3 && nw_installed(w, b2, "c24_l256") == 3 && !nw_before_engine(code),
              "%s: c24_l256's 3 files asked (%d) and installed (%d) on the %s network, then the engine (%s)", name, files,
              nw_installed(w, b2, "c24_l256"), conn, s ? "opened" : tf_error_name(code));
      }
      if (s) tf_session_destroy(s);
      if (m) tf_model_close(m);
    } else if (!strcmp(call, "update")) {
      /* the build on the device, then a config naming a newer one */
      nw_net("unmetered");
      CHECK(tf_client_prefetch(c, NULL) == 0 && nw_current(w, b1), "%s: the first build installed (%s)", name, tf_error_name(tf_last_error_code()));
      g_f.fx = two;
      nw_forget();
      nw_net(conn);
      code = tf_client_prefetch(c, NULL);
      CHECK(code == 0 && nw_asked(b2) >= 4 && nw_files("c16_l64") == 3 && nw_current(w, b2), "%s: the newer build downloaded on the %s network (%s, %d requests)",
            name, conn, tf_error_name(code), nw_asked(b2));
    } else {
      CHECK(0, "%s: a call this test does not know (%s)", name, call);
    }
    tf__connection_override = -1;
    g_f.fail_url = NULL;
    tf_client_destroy(c);
    replayed++;
  }
  /* 24.13 item 3 on the open's own path (4.0.2): the model on the device, its lease past its end and its grace, no
   * network: needs_connection whose cause is the lease's, needs lease (the files are here; Quran Lab's answer is not) */
  {
    const TfJson *l = tf_json_get(two, "lease");
    const long long iat = N(l, "iat"), exp = N(l, "exp"), b0 = 3600000;
    char w[1100];
    snprintf(w, sizeof w, "%s/needs_lease", work);
    fake_reset(LEASE_OK);
    g_f.fx = two;
    set_clock(iat + 60, b0, "boot-nowait");
    TfClient *c = flow_client(w);
    const int pre = c ? tf_client_prefetch(c, NULL) : -1;
    if (c) tf_client_destroy(c);
    tf__plat_test_boot_ms = b0 + (exp + 86400 + 600 - iat) * 1000;
    nw_forget();
    nw_net("none");
    c = flow_client_in(w);
    TfModel *m = c ? tf_client_model(c, NULL) : NULL;
    const int code = m ? 0 : tf_last_error_code();
    nw_print("needs_lease");
    TfErrorCause ec;
    const int has = tf_last_error_cause(&ec);
    CHECK(pre == 0 && !m && code == TF_ERR_NEEDS_CONNECTION && has && ec.needs && !strcmp(ec.needs, "lease") && !strcmp(ec.reason, "network") &&
              !strcmp(ec.stage, "attest") && nw_asked("/v1/model-key") == 0 && nw_asked("/v1/config") == 0,
          "needs_lease: the model here, no lease, no network: %s, cause %s/%s needs %s", tf_error_name(code), has ? ec.stage : "none", has ? ec.reason : "",
          has && ec.needs ? ec.needs : "none");
    if (m) tf_model_close(m);
    if (c) tf_client_destroy(c);
    set_clock(0, 0, NULL);
    tf__connection_override = -1;
    g_f.fail_url = NULL;
  }
  /* a prefetch with no network: needs_connection, its cause on the error (tf_client_prefetch's own way out) */
  {
    char w[1100];
    snprintf(w, sizeof w, "%s/prefetch_none", work);
    fake_reset(LEASE_OK);
    g_f.fx = two;
    TfClient *c = flow_client(w);
    nw_net("none");
    const int rc = c ? tf_client_prefetch(c, NULL) : -1;
    TfErrorCause ec;
    const int has = tf_last_error_cause(&ec);
    CHECK(rc == TF_ERR_NEEDS_CONNECTION && has && !strcmp(ec.stage, "attest") && !strcmp(ec.reason, "network") && ec.needs && !strcmp(ec.needs, "model"),
          "prefetch_none: %s, cause %s/%s needs %s", tf_error_name(rc), has ? ec.stage : "none", has ? ec.reason : "", has && ec.needs ? ec.needs : "none");
    tf__connection_override = -1;
    g_f.fail_url = NULL;
    if (c) tf_client_destroy(c);
  }
  /* 24.4 rule 1 for another context: part of its files here (a download that stopped) and no network is
   * download_failed, not needs_connection */
  {
    char w[1100], p[1400];
    snprintf(w, sizeof w, "%s/other_context_partial", work);
    fake_reset(LEASE_OK);
    g_f.fx = two;
    TfClient *c = flow_client(w);
    nw_net("unmetered");
    const int pre = c ? tf_client_prefetch(c, NULL) : -1;
    TfModel *m = c ? nw_model(c) : NULL;
    snprintf(p, sizeof p, "%s/models/%s/cdn/c24_l256", w, b2);
    tf_mkdirs(p);
    snprintf(p, sizeof p, "%s/models/%s/cdn/c24_l256/frames.zfw.qle.part", w, b2);
    tf_write_file(p, "part", 4);
    nw_net("none");
    TfSessionOptions so;
    memset(&so, 0, sizeof so);
    so.context = "c24_l256";
    TfSession *s = m ? tf_session_create(m, &so) : NULL;
    const int code = s ? 0 : tf_last_error_code();
    CHECK(pre == 0 && m && !s && code == TF_ERR_DOWNLOAD_FAILED, "other_context_partial: part of c24_l256 here, no network: %s (want download_failed)",
          tf_error_name(code));
    if (s) tf_session_destroy(s);
    if (m) tf_model_close(m);
    tf__connection_override = -1;
    g_f.fail_url = NULL;
    if (c) tf_client_destroy(c);
  }
  /* the report is never read: with the platform reporting no network and the stand-in answering, a prefetch goes ahead */
  {
    char w[1100];
    snprintf(w, sizeof w, "%s/report_none_answering", work);
    fake_reset(LEASE_OK);
    g_f.fx = two;
    TfClient *c = flow_client(w);
    tf__connection_override = TF__CONNECTION_NONE;
    const int rc = c ? tf_client_prefetch(c, NULL) : -1;
    CHECK(rc == 0 && nw_files("c16_l64") == 3, "the report says none, the stand-in answers: the prefetch downloads (%s)", tf_error_name(rc));
    tf__connection_override = -1;
    if (c) tf_client_destroy(c);
  }
  printf("  no_wait: %d of %d cases that name c replayed\n", replayed, named);
  CHECK(named == 8 && replayed == named, "no_wait: the 8 cases that name c (%d named, %d replayed)", named, replayed);
  tf__download_pause_ms = 1000;
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(one);
  tf_json_free(two);
  tf_json_free(v);
  return done("no_wait");
}
