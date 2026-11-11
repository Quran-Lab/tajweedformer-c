/* Included by tf_tests.c after tf_30_tests.inc: the public Client of SDK 3.0.0.
 *
 *   api <work dir> <data dir>   the options, one Client per process, storage, the app-reported counter, sharing
 *                               while it is unavailable, and diagnose() for each failure it names: no network, a
 *                               key refused in three ways, no build for this SDK, a stale config; pins
 *
 * The signed configs are test/data/model_configs.json (test/make_model_configs.py: a throwaway key). */

/* The diagnose() report against the rules of diagnosis.schema.json, written out by hand (the library has no JSON
 * Schema reader): the nine properties in order and nothing else, each with its type, enum or pattern. The same
 * function is run over diagnosis_examples.json first, so it is known to accept the valid ones and to refuse each
 * invalid one. */
static int t30_is_build(const char *s) { return s && strlen(s) == 40 && strspn(s, "0123456789abcdef") == 40; }
static int t30_keys_are(const TfJson *o, const char *const *keys, int n) {
  if (!o || o->type != TJ_OBJ || o->n != n) return 0;
  for (int i = 0; i < n; ++i)
    if (strcmp(o->kids[i].key, keys[i])) return 0;
  return 1;
}
static int t30_whole(const TfJson *v) { return v && v->type == TJ_NUM && v->num >= 0 && v->num == floor(v->num); }
static int t30_diag_ok(const TfJson *r) {
  static const char *const top[] = {"sdk", "key", "config", "model", "context", "backend", "microphone", "storage", "lastError", "droppedChunks", "connection"};
  static const char *const k_sdk[] = {"version", "latest"}, *const k_key[] = {"accepted", "reason"}, *const k_model[] = {"version", "build", "verified"};
  static const char *const k_backend[] = {"kind", "name", "stage", "reason"}, *const k_storage[] = {"models", "sharingQueueBytes"};
  static const char *const k_entry[] = {"version", "build", "bytes"};
  if (!t30_keys_are(r, top, 11)) return 0;
  const char *conn = tf_json_str(tf_json_get(r, "connection"));
  if (!t30_whole(tf_json_get(r, "droppedChunks")) || !conn ||
      (strcmp(conn, "unmetered") && strcmp(conn, "metered") && strcmp(conn, "none") && strcmp(conn, "unknown")))
    return 0;
  const TfJson *sdk = tf_json_get(r, "sdk"), *key = tf_json_get(r, "key"), *model = tf_json_get(r, "model"), *be = tf_json_get(r, "backend");
  const TfJson *st = tf_json_get(r, "storage"), *ctx = tf_json_get(r, "context"), *le = tf_json_get(r, "lastError");
  if (!t30_keys_are(sdk, k_sdk, 2) || !tf__is_model_version(tf_json_str(tf_json_get(sdk, "version")))) return 0;
  const TfJson *latest = tf_json_get(sdk, "latest");
  if (latest->type != TJ_NULL && !tf__is_model_version(tf_json_str(latest))) return 0;
  if (!t30_keys_are(key, k_key, 2)) return 0;
  const TfJson *acc = tf_json_get(key, "accepted"), *reason = tf_json_get(key, "reason");
  if (acc->type != TJ_BOOL && acc->type != TJ_NULL) return 0;
  const char *rs = tf_json_str(reason);
  if (acc->type == TJ_BOOL && !acc->boolean) {
    if (!rs || (strcmp(rs, "invalid_key") && strcmp(rs, "sdk_too_old") && strcmp(rs, "server_rejected"))) return 0;
  } else if (reason->type != TJ_NULL) {
    return 0;
  }
  const char *cfg = tf_json_str(tf_json_get(r, "config"));
  if (!cfg || (strcmp(cfg, "fresh") && strcmp(cfg, "stale") && strcmp(cfg, "none"))) return 0;
  if (model->type != TJ_NULL) {
    if (!t30_keys_are(model, k_model, 3) || !tf__is_model_version(tf_json_str(tf_json_get(model, "version"))) ||
        !t30_is_build(tf_json_str(tf_json_get(model, "build"))) || tf_json_get(model, "verified")->type != TJ_BOOL)
      return 0;
  }
  if (ctx->type != TJ_NULL) {
    /* ^c[0-9]+_l[0-9]+$ */
    const char *c = tf_json_str(ctx);
    if (!c || c[0] != 'c') return 0;
    const size_t a = strspn(c + 1, "0123456789");
    if (!a || c[1 + a] != '_' || c[2 + a] != 'l') return 0;
    const size_t b = strspn(c + 3 + a, "0123456789");
    if (!b || c[3 + a + b]) return 0;
  }
  if (be->type != TJ_NULL) {
    if (!t30_keys_are(be, k_backend, 4)) return 0;
    const char *kind = tf_json_str(tf_json_get(be, "kind")), *name = tf_json_str(tf_json_get(be, "name")), *stage = tf_json_str(tf_json_get(be, "stage"));
    static const char *const stages[] = {"cpu", "downloading", "compiling", "testing", "promoted", "demoted"};
    int sok = 0;
    for (int i = 0; stage && i < 6; ++i) sok |= !strcmp(stage, stages[i]);
    if (!kind || !name || !sok) return 0;
    const int cpu_name = !strcmp(name, "cpu") || !strcmp(name, "wasm");
    const int acc_name = !strcmp(name, "coreml") || !strcmp(name, "coreml-compact") ||
                         (!strncmp(name, "npu:", 4) && name[4] && strspn(name + 4, "abcdefghijklmnopqrstuvwxyz0123456789_") == strlen(name + 4));
    if (!strcmp(kind, "cpu") ? !cpu_name : strcmp(kind, "accelerator") || !acc_name) return 0;
    const TfJson *br = tf_json_get(be, "reason");
    if (br->type != TJ_NULL && (!tf_json_str(br) || !tf__rule_name_ok(br->str))) return 0;
  }
  const char *mic = tf_json_str(tf_json_get(r, "microphone"));
  if (!mic || (strcmp(mic, "granted") && strcmp(mic, "denied") && strcmp(mic, "notAsked") && strcmp(mic, "none"))) return 0;
  if (!t30_keys_are(st, k_storage, 2) || tf_json_get(st, "models")->type != TJ_ARR || !t30_whole(tf_json_get(st, "sharingQueueBytes"))) return 0;
  const TfJson *ms = tf_json_get(st, "models");
  for (int i = 0; i < ms->n; ++i)
    if (!t30_keys_are(&ms->kids[i], k_entry, 3) || !tf__is_model_version(tf_json_str(tf_json_get(&ms->kids[i], "version"))) ||
        !t30_is_build(tf_json_str(tf_json_get(&ms->kids[i], "build"))) || !t30_whole(tf_json_get(&ms->kids[i], "bytes")))
      return 0;
  if (le->type != TJ_NULL) {
    int known = 0;
    for (int e = -1; e >= -25 && tf_json_str(le); --e) known |= !strcmp(tf_error_name(e), le->str);
    if (!known) return 0;
  }
  return 1;
}

static struct {
  TfJson *fx;          /* model_configs.json */
  const char *serve;   /* the config to answer with (mode 4) */
  int mode;            /* 0 no network, 1 the key is unknown, 2 the key needs a newer SDK, 3 the app is refused, 4 a config */
  const char *refusal; /* mode 3: the server's 403 code (NULL: app_blocked) */
  int raw_status;      /* mode 5: every API request gets this status and body (refusals.json) */
  const char *raw_body;
  int config_calls, requests;
  char config_path[256];
} g_ap;
static void ap_reply(TfHttpResponse *r, int status, const char *body) {
  r->status = status;
  r->body_n = strlen(body);
  r->body = (char *)malloc(r->body_n + 1);
  memcpy(r->body, body, r->body_n + 1);
}
static const TfJson *ap_config(const char *name) {
  const TfJson *cs = tf_json_get(g_ap.fx, "configs");
  for (int i = 0; cs && i < cs->n; ++i)
    if (!strcmp(tf_json_str(tf_json_get(&cs->kids[i], "name")), name)) return &cs->kids[i];
  return NULL;
}
/* the API side of the stub: 1 when the request was one of the API's and was answered */
static int ap_api(const TfHttpRequest *q, TfHttpResponse *r, int *rc) {
  const char *path = strstr(q->url, "/v1/");
  if (!path) return 0;
  *rc = 0;
  if (g_ap.mode == 1) {
    ap_reply(r, 401, "{\"error\":\"invalid_key\",\"message\":\"unknown key\"}");
    return 1;
  }
  if (g_ap.mode == 5) {
    if (!strncmp(path, "/v1/config", 10)) g_ap.config_calls++;
    ap_reply(r, g_ap.raw_status, g_ap.raw_body);
    return 1;
  }
  if (!strncmp(path, "/v1/attest", 10)) {
    ap_reply(r, 200, "{\"token\":\"qlt_v1.test.sig\",\"expires_in\":86400,\"attested\":false}");
  } else if (!strncmp(path, "/v1/config", 10)) {
    g_ap.config_calls++;
    snprintf(g_ap.config_path, sizeof g_ap.config_path, "%s", path);
    if (g_ap.mode == 2) ap_reply(r, 403, "{\"error\":\"sdk_too_old\",\"message\":\"this key needs a newer SDK\",\"min_sdk\":\"3.1.0\"}");
    else if (g_ap.mode == 3) {
      char body[160];
      snprintf(body, sizeof body, "{\"error\":\"%s\",\"message\":\"refused\"}", g_ap.refusal ? g_ap.refusal : "app_blocked");
      ap_reply(r, 403, body);
    }
    else {
      const TfJson *cfg = ap_config(g_ap.serve);
      if (!cfg) {
        *rc = tf__fail(TF__ERR_NETWORK, "no such test config");
        return 1;
      }
      ap_reply(r, 200, tf_json_str(tf_json_get(cfg, "body")));
      snprintf(r->signature, sizeof r->signature, "%s", tf_json_str(tf_json_get(cfg, "signature")));
      snprintf(r->key_id, sizeof r->key_id, "%s", tf_json_str(tf_json_get(g_ap.fx, "key_id")));
    }
  } else if (!strncmp(path, "/v1/register", 12)) {
    if (g_ap.mode == 2) ap_reply(r, 403, "{\"error\":\"sdk_too_old\",\"message\":\"this key needs a newer SDK\",\"min_sdk\":\"3.1.0\"}");
    else ap_reply(r, 200, "{\"ok\":true,\"app_id\":\"a\",\"store_status\":\"unknown\"}");
  } else {
    ap_reply(r, 202, "{\"ok\":true,\"points\":1}");
  }
  return 1;
}
static int ap_stub(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  g_ap.requests++;
  int rc = 0;
  if (g_ap.mode == 0 || !ap_api(q, r, &rc)) return tf__fail(TF__ERR_NETWORK, "no network (test)");
  return rc;
}
static TfClient *ap_client(const char *dir) {
  tf_mkdirs(dir);
  TfClient *c = tf_client_create(&(TfClientOptions){.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa", .app_id = "org.example.Api", .app_version = "1.0", .storage = dir,
                                                    .trusted_key_hex = tf_json_str(tf_json_get(g_ap.fx, "public_key_hex"))});
  if (!c) fprintf(stderr, "client: %s\n", tf_last_error());
  return c;
}
/* the report of tf_client_diagnose_json, parsed and held to the schema's rules */
static TfJson *ap_diagnose(TfClient *c, const char *what) {
  const char *text = tf_client_diagnose_json(c);
  TfJson *j = text ? tf_json_parse(text, strlen(text), NULL) : NULL;
  CHECK(j && t30_diag_ok(j), "%s: the diagnose report does not follow the schema: %s", what, text ? text : "(none)");
  /* in this SDK always: no chunk waits (callbacks), and the kind of connection is not known */
  CHECK(j && tf_json_get(j, "droppedChunks") && tf_json_get(j, "droppedChunks")->num == 0 && tf_json_str(tf_json_get(j, "connection")) &&
            !strcmp(tf_json_str(tf_json_get(j, "connection")), "unknown"),
        "%s: droppedChunks is not 0 or connection is not unknown: %s", what, text ? text : "(none)");
  TfDiagnosis d;
  CHECK(tf_client_diagnose(c, &d) == 0 && d.json && d.sdk_version && !strcmp(d.sdk_version, TF_SDK_VERSION) && !strcmp(d.microphone, "none"), "%s: TfDiagnosis", what);
  return j;
}
static const char *ap_str(const TfJson *j, const char *a, const char *b) { return tf_json_str(b ? tf_json_get(tf_json_get(j, a), b) : tf_json_get(j, a)); }
static int ap_null(const TfJson *j, const char *a, const char *b) {
  const TfJson *v = b ? tf_json_get(tf_json_get(j, a), b) : tf_json_get(j, a);
  return v && v->type == TJ_NULL;
}
static int ap_true(const TfJson *j, const char *a, const char *b) {
  const TfJson *v = b ? tf_json_get(tf_json_get(j, a), b) : tf_json_get(j, a);
  return v && v->type == TJ_BOOL && v->boolean;
}
static int ap_false(const TfJson *j, const char *a, const char *b) {
  const TfJson *v = b ? tf_json_get(tf_json_get(j, a), b) : tf_json_get(j, a);
  return v && v->type == TJ_BOOL && !v->boolean;
}

static int api_tests(const char *work, const char *data) {
  tf_mkdirs(work);
  char d[1100];
  /* the schema checker itself: every valid example passes, every invalid one fails */
  {
    TfJson *ex = t30_json(data, "diagnosis_examples.json");
    const TfJson *valid = tf_json_get(ex, "valid"), *invalid = tf_json_get(ex, "invalid");
    int vok = 0, iok = 0;
    for (int i = 0; valid && i < valid->n; ++i) vok += t30_diag_ok(tf_json_get(&valid->kids[i], "report"));
    for (int i = 0; invalid && i < invalid->n; ++i) {
      const int refused = !t30_diag_ok(tf_json_get(&invalid->kids[i], "report"));
      CHECK(refused, "the checker accepts the invalid example \"%s\"", tf_json_str(tf_json_get(&invalid->kids[i], "name")));
      iok += refused;
    }
    CHECK(valid && invalid && vok == valid->n && iok == invalid->n && valid->n >= 5 && invalid->n >= 12, "diagnosis examples: %d valid pass, %d invalid refused", vok, iok);
    printf("diagnose: the report checker accepts %d/%d valid examples and refuses %d/%d invalid ones\n", vok, valid ? valid->n : 0, iok, invalid ? invalid->n : 0);
    tf_json_free(ex);
  }
  g_ap.fx = t30_json(data, "../model_configs.json");
  if (!g_ap.fx) return 2;
  tf_http_set_transport(ap_stub, NULL);
  tf__now_override = 1794355200 + 9 * 3600;

  /* 1. the options, and one Client per process */
  g_ap.mode = 0;
  snprintf(d, sizeof d, "%s/one", work);
  CHECK(tf_client_create(NULL) == NULL && tf_last_error_code() == TF_ERR_INVALID_ARGUMENT, "a client without options");
  CHECK(tf_client_create(&(TfClientOptions){.app_id = "a", .storage = d}) == NULL && tf_last_error_code() == TF_ERR_INVALID_ARGUMENT, "a client without a key");
  CHECK(tf_client_create(&(TfClientOptions){.key = "qlk_v1_x", .storage = d}) == NULL && tf_last_error_code() == TF_ERR_INVALID_ARGUMENT, "a client without an app id");
  CHECK(tf_client_create(&(TfClientOptions){.key = "qlk_v1_x", .app_id = "a", .storage = d, .audience = "kids"}) == NULL &&
            tf_last_error_code() == TF_ERR_INVALID_ARGUMENT,
        "an audience that is not general or children");
  CHECK(tf_client_create(&(TfClientOptions){.key = "qlk_v1_x", .app_id = "a", .storage = d, .trusted_key_hex = "zz"}) == NULL, "a trusted key that is not hex");
  TfClient *c = ap_client(d);
  CHECK(c != NULL, "the first client: %s", tf_last_error() ? tf_last_error() : "");
  TfClient *second = tf_client_create(&(TfClientOptions){.key = "qlk_v1_other", .app_id = "b", .storage = d});
  CHECK(second == NULL && tf_last_error_code() == TF_ERR_CLIENT_EXISTS && !strcmp(tf_last_error(), "This process already has a TajweedClient.") &&
            !strcmp(tf_error_docs(tf_last_error_code()), "https://sdk.quranlab.ai/docs/errors#client_exists"),
        "a second client while the first is open: %s", tf_error_name(tf_last_error_code()));
  second = tf_client_create(&(TfClientOptions){.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa", .app_id = "org.example.Api", .storage = d});
  CHECK(second == NULL && tf_last_error_code() == TF_ERR_CLIENT_EXISTS, "a second client with the same key");
  tf_client_destroy(c);
  c = ap_client(d);
  CHECK(c != NULL, "a client after the first was destroyed: %s", tf_last_error() ? tf_last_error() : "");
  /* children: the consent is addressed to a guardian (the option reaches the client) */
  tf_client_destroy(c);
  c = tf_client_create(&(TfClientOptions){.key = "qlk_v1_x", .app_id = "org.example.Api", .storage = d, .audience = "children", .analytics = TF_ANALYTICS_OFF});
  CHECK(c && tf__client_options(c)->children == 1 && tf__client_options(c)->telemetry == 0, "audience children and analytics off did not reach the client");
  tf_client_destroy(c);
  c = ap_client(d);

  /* 2. no network, nothing on disk: the report says so, and the model is a connection away (not a timeout) */
  tf__fail(TF_ERR_CLIENT_EXISTS, NULL);
  tf__raise(-1); /* the last error this process raised, as after the refused second client */
  tf__clear_error();
  TfJson *r = ap_diagnose(c, "no network");
  CHECK(r && ap_null(r, "key", "accepted") && ap_null(r, "key", "reason") && !strcmp(ap_str(r, "config", NULL), "none") && ap_null(r, "model", NULL) &&
            ap_null(r, "context", NULL) && ap_null(r, "backend", NULL) && ap_null(r, "sdk", "latest") && !strcmp(ap_str(r, "lastError", NULL), "client_exists"),
        "no network: %s", tf_client_diagnose_json(c));
  tf_json_free(r);
  CHECK(tf_client_model(c, NULL) == NULL && tf_last_error_code() == TF_ERR_NEEDS_CONNECTION && !strcmp(tf_last_error(), "The model is not on this device yet."),
        "a first run without a network: %s", tf_error_name(tf_last_error_code()));
  CHECK(tf_client_prefetch(c, NULL) == TF_ERR_NEEDS_CONNECTION, "prefetch without a network: %s", tf_error_name(tf_last_error_code()));
  r = ap_diagnose(c, "after needs_connection");
  CHECK(r && !strcmp(ap_str(r, "lastError", NULL), "needs_connection"), "lastError after needs_connection");
  tf_json_free(r);
  /* storage: nothing yet; then one build folder with a manifest; removed again (never the client's own state) */
  CHECK(!strcmp(tf_client_storage_json(c), "{\"models\":[],\"sharingQueueBytes\":0}"), "storage of an empty folder: %s", tf_client_storage_json(c));
  {
    char bd[1300], f[1400];
    snprintf(bd, sizeof bd, "%s/models/0123456789abcdef0123456789abcdef01234567", d);
    tf_mkdirs(bd);
    snprintf(f, sizeof f, "%s/manifest.json", bd);
    tf_write_file(f, "{\"version\":\"4.0.0\"}", 19);
    snprintf(f, sizeof f, "%s/layers.zfw", bd);
    char blob[1000];
    memset(blob, 7, sizeof blob);
    tf_write_file(f, blob, sizeof blob);
    CHECK(!strcmp(tf_client_storage_json(c), "{\"models\":[{\"version\":\"4.0.0\",\"build\":\"0123456789abcdef0123456789abcdef01234567\",\"bytes\":1019}],\"sharingQueueBytes\":0}"),
          "storage with one build: %s", tf_client_storage_json(c));
    r = ap_diagnose(c, "a build folder on disk");
    tf_json_free(r);
    CHECK(tf_client_remove_models(c) == 0 && !strcmp(tf_client_storage_json(c), "{\"models\":[],\"sharingQueueBytes\":0}"), "removeModels: %s", tf_client_storage_json(c));
    snprintf(f, sizeof f, "%s/client.json", d);
    CHECK(tf_file_exists(f), "removeModels took the client's state with it");
    snprintf(f, sizeof f, "%s/sharing.json", d);
    tf_sharing_set_on(c, 0); /* writes the consent state */
    CHECK(tf_file_exists(f) && tf_client_remove_models(c) == 0 && tf_file_exists(f), "removeModels took the consent state with it");
  }
  /* the app-reported counter: its refusals are programming errors */
  CHECK(tf_client_report_checks(c, "Madd", 1, 0) == TF_ERR_INVALID_ARGUMENT && tf_client_report_checks(c, "madd", 1, 2) == TF_ERR_INVALID_ARGUMENT &&
            tf_client_report_checks(c, "madd", 100001, 0) == TF_ERR_INVALID_ARGUMENT && tf_client_report_checks(c, NULL, 1, 0) == TF_ERR_INVALID_ARGUMENT &&
            tf_client_report_checks(c, "madd", 12, 3) == 0,
        "reportChecks refusals");
  CHECK(tf_client_background(c) == 0 && tf_client_foreground(c) == 0, "background and foreground");
  /* sharing, while Quran Lab's config does not offer it: nothing is on, and the calls that would turn it on say so */
  {
    TfSharing sh;
    char code[33] = "x";
    static const float silence[160];
    CHECK(tf_sharing_status(c, &sh) == 0 && !sh.is_available && !sh.is_on && sh.asks == 0 && sh.consent_version && !sh.consent_text, "sharing status without a config");
    CHECK(!tf_sharing_is_available(c) && !tf_sharing_is_on(c) && tf_sharing_set_on(c, 1) == TF_ERR_SHARING_UNAVAILABLE && tf_sharing_ask(c) == TF_ERR_SHARING_UNAVAILABLE &&
              !strcmp(tf_last_error(), "Sharing is not available for this app.") && tf_sharing_set_on(c, 0) == 0 && !tf_sharing_is_on(c),
          "sharing calls while it is unavailable");
    CHECK(tf_sharing_copy_json(c) && strstr(tf_sharing_copy_json(c), "consent_version") && tf_sharing_recovery_code(c, code) == 0 && code[0] == 0 &&
              tf_sharing_summary_json(c) == NULL && tf_last_error_code() == 0 && tf_sharing_items_json(c) && !strcmp(tf_sharing_items_json(c), "[]") &&
              tf_sharing_delete(c) == 0,
          "sharing data calls before anything was shared: %s", tf_last_error() ? tf_last_error() : "");
    CHECK(tf_sharing_feed(c, silence, 160, 16000) == 0 && tf_sharing_feed(c, silence, 160, 100) == TF_ERR_INVALID_ARGUMENT && tf_sharing_feed(c, NULL, 5, 16000) == TF_ERR_INVALID_ARGUMENT,
          "the feed without a yes drops its audio; a rate out of range is refused");
  }
  tf_client_destroy(c);

  /* 3. the server refuses the key, in the three ways the report names; the model call fails with the same code */
  static const struct {
    int mode, code;
    const char *reason, *dir;
  } refused[] = {{1, TF_ERR_INVALID_KEY, "invalid_key", "key"}, {2, TF_ERR_SDK_TOO_OLD, "sdk_too_old", "old"}, {3, TF_ERR_SERVER_REJECTED, "server_rejected", "blocked"}};
  for (int i = 0; i < 3; ++i) {
    g_ap.mode = refused[i].mode;
    snprintf(d, sizeof d, "%s/%s", work, refused[i].dir);
    c = ap_client(d);
    CHECK(c && tf_client_model(c, NULL) == NULL && tf_last_error_code() == refused[i].code, "%s: model() gave %s", refused[i].reason, tf_error_name(tf_last_error_code()));
    if (refused[i].mode == 2)
      CHECK(!strcmp(tf_last_error(), "This key needs SDK 3.1.0 or later; this is 3.0.0.") && strstr(tf_last_error_fix(), "3.1.0"),
            "sdk_too_old names the version of the answer: %s / %s", tf_last_error(), tf_last_error_fix());
    r = c ? ap_diagnose(c, refused[i].reason) : NULL;
    CHECK(r && ap_false(r, "key", "accepted") && !strcmp(ap_str(r, "key", "reason"), refused[i].reason) &&
              !strcmp(ap_str(r, "lastError", NULL), tf_error_name(refused[i].code)),
          "%s: %s", refused[i].reason, c ? tf_client_diagnose_json(c) : "");
    tf_json_free(r);
    tf_client_destroy(c);
  }

  /* 3b. a refusal says what to do: for each refusal the catalogue names, the error is server_rejected with that
   * refusal's own message and fix (the app's identifier where the sentence has {origin}); a code the catalogue
   * does not know gives the entry's own sentence with the code in it */
  {
    size_t en = 0;
    const unsigned char *emb = tf__errors_json(&en);
    TfJson *cat = tf_json_parse((const char *)emb, en, NULL);
    const TfJson *errs = tf_json_get(cat, "errors"), *reasons = NULL;
    for (int i = 0; errs && i < errs->n; ++i)
      if (!strcmp(tf_json_str(tf_json_get(&errs->kids[i], "code")), "server_rejected")) reasons = tf_json_get(&errs->kids[i], "reasons");
    /* seven are the server's 403 codes; the eighth (local_needs_development_key) is the web origin check's and is
     * looked up the same way */
    CHECK(reasons && reasons->n == 8, "the catalogue has %d entries under server_rejected's reasons, want 8", reasons ? reasons->n : 0);
    int told = 0;
    for (int i = 0; reasons && i < reasons->n; ++i) {
      const TfJson *rs = &reasons->kids[i];
      const char *msg = tf_json_str(tf_json_get(rs, "message")), *fix = tf_json_str(tf_json_get(rs, "fix"));
      g_ap.mode = 3;
      g_ap.refusal = rs->key;
      snprintf(d, sizeof d, "%s/refused_%d", work, i);
      c = ap_client(d);
      char want[640];
      const char *o = strstr(msg, "{origin}");
      if (o) snprintf(want, sizeof want, "%.*s%s%s", (int)(o - msg), msg, c ? tf__client_app(c) : "", o + 8);
      else snprintf(want, sizeof want, "%s", msg);
      const int ok = c && tf_client_model(c, NULL) == NULL && tf_last_error_code() == TF_ERR_SERVER_REJECTED && !strcmp(tf_last_error(), want) &&
                     tf_last_error_fix() && !strcmp(tf_last_error_fix(), fix) && strstr(tf_last_error(), rs->key);
      CHECK(ok, "refusal %s: %s / %s", rs->key, tf_last_error(), tf_last_error_fix() ? tf_last_error_fix() : "(no fix)");
      told += ok;
      tf_client_destroy(c);
    }
    g_ap.refusal = "app_not_allowed";
    snprintf(d, sizeof d, "%s/refused_app", work);
    c = ap_client(d);
    CHECK(c && tf_client_model(c, NULL) == NULL && !strcmp(tf_last_error(), "This key belongs to another app (app_not_allowed).") &&
              !strcmp(tf_last_error_fix(), "A key works for the first app that used it. Make a key for this app at https://sdk.quranlab.ai/keys, or allow "
                                           "this app's identifier on the key's page."),
          "app_not_allowed: %s / %s", tf_last_error(), tf_last_error_fix() ? tf_last_error_fix() : "(no fix)");
    tf_client_destroy(c);
    g_ap.refusal = "a_code_from_later";
    snprintf(d, sizeof d, "%s/refused_new", work);
    c = ap_client(d);
    CHECK(c && tf_client_model(c, NULL) == NULL && tf_last_error_code() == TF_ERR_SERVER_REJECTED &&
              !strcmp(tf_last_error(), "Quran Lab refused this app: a_code_from_later."),
          "a refusal the catalogue does not know: %s", tf_last_error());
    tf_client_destroy(c);
    g_ap.refusal = NULL;
    printf("api: a refusal says what to do: %d of %d refusals of the catalogue, app_not_allowed word for word, an unknown code\n", told, reasons ? reasons->n : 0);
    tf_json_free(cat);
  }

  /* 3c. the shared refusal vectors (refusals.json): the first answer of the API, status and body, and the error
   * the app gets: the body's "reason" decides when the catalogue has it, else "error"; code, message and fix
   * exactly; diagnose() has the key not accepted with that code */
  {
    TfJson *v = t30_json(data, "refusals.json");
    const TfJson *cases = tf_json_get(v, "cases");
    const char *app = tf_json_str(tf_json_get(v, "app"));
    int ok = 0;
    for (int i = 0; cases && i < cases->n; ++i) {
      const TfJson *cs = &cases->kids[i], *e = tf_json_get(cs, "expect");
      TfBuf body;
      tf_buf_init(&body);
      tf_json_write(&body, tf_json_get(cs, "body"));
      char *text = tf_buf_take(&body);
      g_ap.mode = 5;
      g_ap.raw_status = (int)t30_num(cs, "status");
      g_ap.raw_body = text;
      snprintf(d, sizeof d, "%s/vec_refusal_%d", work, i);
      tf_mkdirs(d);
      c = tf_client_create(&(TfClientOptions){.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa", .app_id = app, .app_version = "1.0", .storage = d,
                                              .trusted_key_hex = tf_json_str(tf_json_get(g_ap.fx, "public_key_hex"))});
      const char *code = tf_json_str(tf_json_get(e, "code")), *msg = tf_json_str(tf_json_get(e, "message")), *fix = tf_json_str(tf_json_get(e, "fix"));
      const int good = c && text && tf_client_model(c, NULL) == NULL && !strcmp(tf_error_name(tf_last_error_code()), code) &&
                       !strcmp(tf_last_error(), msg) && tf_last_error_fix() && !strcmp(tf_last_error_fix(), fix);
      CHECK(good, "refusals.json %s: %s / %s / %s", tf_json_str(tf_json_get(cs, "name")), tf_error_name(tf_last_error_code()), tf_last_error(),
            tf_last_error_fix() ? tf_last_error_fix() : "(no fix)");
      r = c ? ap_diagnose(c, "refusal vector") : NULL;
      const int dg = r && ap_false(r, "key", "accepted") && !strcmp(ap_str(r, "key", "reason"), code) && !strcmp(ap_str(r, "lastError", NULL), code);
      CHECK(dg, "refusals.json %s: diagnose %s", tf_json_str(tf_json_get(cs, "name")), c ? tf_client_diagnose_json(c) : "");
      ok += good && dg;
      tf_json_free(r);
      tf_client_destroy(c);
      free(text);
    }
    g_ap.raw_body = NULL;
    CHECK(cases && cases->n == 11 && ok == cases->n, "refusals.json: %d of %d cases", ok, cases ? cases->n : 0);
    printf("api: refusals.json: %d of %d answers give the catalogue's code, message and fix (the reason is read first)\n", ok, cases ? cases->n : 0);
    tf_json_free(v);
  }

  /* 4. Quran Lab has no build for this SDK yet: model_unavailable from model() and prefetch(), one config request
   * per call, nothing retried in a loop; the report shows a key that works and a fresh config */
  g_ap.mode = 4;
  g_ap.serve = "no model";
  snprintf(d, sizeof d, "%s/nomodel", work);
  c = ap_client(d);
  g_ap.config_calls = 0;
  CHECK(c && tf_client_model(c, NULL) == NULL && tf_last_error_code() == TF_ERR_MODEL_UNAVAILABLE && !strcmp(tf_last_error(), "Quran Lab has no model build for SDK 3.0.0 yet."),
        "no build for this SDK: %s", tf_error_name(tf_last_error_code()));
  CHECK(c && tf_client_prefetch(c, NULL) == TF_ERR_MODEL_UNAVAILABLE && g_ap.config_calls == 2, "prefetch: %s after %d config requests", tf_error_name(tf_last_error_code()),
        g_ap.config_calls);
  r = c ? ap_diagnose(c, "no model") : NULL;
  CHECK(r && ap_true(r, "key", "accepted") && ap_null(r, "key", "reason") && !strcmp(ap_str(r, "config", NULL), "fresh") && ap_null(r, "model", NULL) &&
            ap_null(r, "backend", NULL) && !strcmp(ap_str(r, "sdk", "latest"), "3.0.0") && !strcmp(ap_str(r, "lastError", NULL), "model_unavailable") &&
            g_ap.config_calls == 2,
        "no model: %s (%d config requests)", c ? tf_client_diagnose_json(c) : "", g_ap.config_calls);
  tf_json_free(r);
  /* a pin: a version goes with the config request; a string that is not a version is refused and never sent */
  g_ap.config_calls = 0;
  CHECK(c && tf_client_model(c, &(TfModelOptions){.version = "4.0"}) == NULL && tf_last_error_code() == TF_ERR_INVALID_ARGUMENT && g_ap.config_calls == 0,
        "a pin that is not a version: %s, %d config requests", tf_error_name(tf_last_error_code()), g_ap.config_calls);
  CHECK(c && tf_client_prefetch(c, &(TfPrefetchOptions){.version = "4.1.0"}) == TF_ERR_MODEL_UNAVAILABLE && strstr(g_ap.config_path, "model_version=4.1.0") &&
            strstr(g_ap.config_path, "sdk=3.0.0"),
        "a pinned version in the config request: %s", g_ap.config_path);
  CHECK(c && tf_client_prefetch(c, NULL) == TF_ERR_MODEL_UNAVAILABLE && !strstr(g_ap.config_path, "model_version"), "no pin, no model_version: %s", g_ap.config_path);
  tf_client_destroy(c);
  /* the stored config past its expires_at, and no network: stale */
  g_ap.mode = 0;
  tf__now_override = 4102444800LL + 60;
  c = ap_client(d);
  r = c ? ap_diagnose(c, "stale config") : NULL;
  CHECK(r && !strcmp(ap_str(r, "config", NULL), "stale") && ap_null(r, "key", "accepted"), "a stored config past its expiry: %s", c ? tf_client_diagnose_json(c) : "");
  tf_json_free(r);
  tf_client_destroy(c);
  tf__now_override = 1794355200 + 9 * 3600;

  /* 5. a folder on disk: what tf_model_open refuses before it reads a model */
  CHECK(tf_model_open(NULL, NULL) == NULL && tf_last_error_code() == TF_ERR_INVALID_ARGUMENT, "tf_model_open without a folder");
  snprintf(d, sizeof d, "%s/empty", work);
  tf_mkdirs(d);
  CHECK(tf_model_open(d, NULL) == NULL && tf_last_error_code() == TF_ERR_MODEL_MISSING && strstr(tf_last_error(), "No model here"), "an empty folder: %s",
        tf_error_name(tf_last_error_code()));
  CHECK(tf_model_open(d, &(TfModelOptions){.version = "4.0.0"}) == NULL && tf_last_error_code() == TF_ERR_INVALID_ARGUMENT, "a version pin on a folder");
  tf_http_set_transport(NULL, NULL);
  tf__now_override = 0;
  tf_json_free(g_ap.fx);
  g_ap.fx = NULL;
  printf("api: %s\n", fails ? "FAIL" : "all ok (options, one client per process, storage, the app-reported counter, sharing while unavailable, diagnose for no "
                                      "network, a refused key in three ways, no build for this SDK, a stale config; pins)");
  return fails ? 1 : 0;
}
