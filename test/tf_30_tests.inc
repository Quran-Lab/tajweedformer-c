/* Included by tf_tests.c: what SDK 3.0.0 adds.
 *
 *   errors <data dir>                 the embedded error catalogue against the contract's file (byte for byte, when
 *                                     it is on this machine), against the repository's copy and against a recorded
 *                                     SHA-256; the 26 codes in order; messages, fixes and docs links
 *   vectors30 <data dir> <work dir>   the shared 3.0 vectors (test/data/sdk_3.0.0, copies of the contract's):
 *                                     manifest_30.json, speech.json, settled.json, telemetry_30.json,
 *                                     ayah_counts.json, inference_context.json, names.json
 *   api <work dir>                    the public Client: its options, one per process, the app-reported counters,
 *                                     storage, and diagnose() for each failure it names
 *   model30 <local30 dir> <key> <work dir>   the local 3.0 model folders (not in the repository) */

/* the SHA-256 of sdk_3.0.0/errors.json as recorded when the catalogue was embedded (the contract's file at
 * version 1.3.9: 26 codes, and the eight entries under server_rejected's reasons). A changed catalogue changes this line, the generated header and test/data together. */
#define TF_TEST_ERRORS_SHA256 "9d692efed682a65f5b1d2fb0afbe0df37aad790884a0cb9cf5479ca77889e631"
#define TF_TEST_CONTRACT_ERRORS ""

static int t30_down(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)q;
  (void)r;
  (void)u;
  return tf__fail(TF__ERR_NETWORK, "no network (test)");
}

static TfJson *t30_json(const char *dir, const char *name) {
  char *p = tf_path_join(dir, name);
  TfJson *j = p ? read_json_file(p) : NULL;
  if (!j) fprintf(stderr, "cannot read %s in %s\n", name, dir);
  free(p);
  return j;
}
static double t30_num(const TfJson *o, const char *k) {
  const TfJson *v = tf_json_get(o, k);
  return v && v->type == TJ_NUM ? v->num : 0;
}
static int t30_bool(const TfJson *o, const char *k) {
  const TfJson *v = tf_json_get(o, k);
  return v && v->type == TJ_BOOL && v->boolean;
}

/* ------------------------------------------------------------------ errors */
static int errors_tests(const char *data) {
  size_t n = 0;
  const unsigned char *emb = tf__errors_json(&n);
  /* 1. the embedded bytes are the catalogue: the contract's file when this machine has it, the repository's copy,
   * and the recorded SHA-256 */
  {
    char *text = NULL;
    size_t tn = 0;
    if (!tf_read_file(TF_TEST_CONTRACT_ERRORS, &text, &tn)) {
      CHECK(tn == n && !memcmp(text, emb, n), "the embedded catalogue differs from %s (run tools/gen_errors.py)", TF_TEST_CONTRACT_ERRORS);
      printf("errors: the embedded catalogue is the contract's file byte for byte (%d bytes)\n", (int)n);
      free(text);
    } else {
      printf("errors: %s is not on this machine: the comparison with the contract's file is skipped (the recorded SHA-256 below stands in)\n",
             TF_TEST_CONTRACT_ERRORS);
    }
    char *p = tf_path_join(data, "errors.json");
    text = NULL;
    CHECK(p && !tf_read_file(p, &text, &tn) && tn == n && !memcmp(text, emb, n), "the embedded catalogue differs from test/data/sdk_3.0.0/errors.json");
    free(text);
    free(p);
    uint8_t h[32];
    char hex[65];
    tf_sha256(emb, n, h);
    tf_hex_encode(h, 32, hex);
    CHECK(!strcmp(hex, TF_TEST_ERRORS_SHA256), "the embedded catalogue's SHA-256 is %s, recorded %s", hex, TF_TEST_ERRORS_SHA256);
    CHECK(!strcmp(tf__errors_sha256(), TF_TEST_ERRORS_SHA256), "the generated header records %s", tf__errors_sha256());
  }
  /* 2. the table the library uses is the catalogue's entries, in its order, and the TfError values are that order */
  TfJson *cat = tf_json_parse((const char *)emb, n, NULL);
  const TfJson *errs = tf_json_get(cat, "errors");
  const char *base = tf_json_str(tf_json_get(cat, "docs_base"));
  static const struct {
    int code;
    const char *name;
  } kCodes[] = {{TF_ERR_INVALID_ARGUMENT, "invalid_argument"},
                {TF_ERR_INVALID_KEY, "invalid_key"},
                {TF_ERR_SDK_TOO_OLD, "sdk_too_old"},
                {TF_ERR_SERVER_REJECTED, "server_rejected"},
                {TF_ERR_CLIENT_EXISTS, "client_exists"},
                {TF_ERR_MODEL_UNAVAILABLE, "model_unavailable"},
                {TF_ERR_NEEDS_CONNECTION, "needs_connection"},
                {TF_ERR_NETWORK_NOT_PERMITTED, "network_not_permitted"},
                {TF_ERR_DOWNLOAD_FAILED, "download_failed"},
                {TF_ERR_STORAGE_FULL, "storage_full"},
                {TF_ERR_STORAGE_FAILED, "storage_failed"},
                {TF_ERR_MODEL_MISSING, "model_missing"},
                {TF_ERR_MODEL_INVALID, "model_invalid"},
                {TF_ERR_SIGNATURE_INVALID, "signature_invalid"},
                {TF_ERR_CHECKSUM_MISMATCH, "checksum_mismatch"},
                {TF_ERR_MODEL_INCOMPATIBLE, "model_incompatible"},
                {TF_ERR_RUNTIME_UNSUPPORTED, "runtime_unsupported"},
                {TF_ERR_REFERENCE_MISSING, "reference_missing"},
                {TF_ERR_SESSION_OPEN, "session_open"},
                {TF_ERR_SESSION_CLOSED, "session_closed"},
                {TF_ERR_CANCELLED, "cancelled"},
                {TF_ERR_ENGINE_FAILED, "engine_failed"},
                {TF_ERR_MICROPHONE_DENIED, "microphone_denied"},
                {TF_ERR_MICROPHONE_UNAVAILABLE, "microphone_unavailable"},
                {TF_ERR_INTERRUPTED, "interrupted"},
                {TF_ERR_SHARING_UNAVAILABLE, "sharing_unavailable"}};
  const int count = (int)(sizeof kCodes / sizeof kCodes[0]);
  CHECK(errs && errs->type == TJ_ARR && errs->n == count && count == 26, "the catalogue has %d errors, the header %d", errs ? errs->n : -1, count);
  CHECK(base && !strcmp(base, "https://sdk.quranlab.ai/docs/errors#"), "docs_base %s", base ? base : "(none)");
  int ok = 0;
  for (int i = 0; errs && i < errs->n && i < count; ++i) {
    const TfJson *e = &errs->kids[i];
    const char *code = tf_json_str(tf_json_get(e, "code")), *msg = tf_json_str(tf_json_get(e, "message")), *fix = tf_json_str(tf_json_get(e, "fix"));
    const int value = -(i + 1);
    char docs[160];
    snprintf(docs, sizeof docs, "%s%s", base ? base : "", code);
    int good = kCodes[i].code == value && !strcmp(kCodes[i].name, code) && tf_error_name(value) && !strcmp(tf_error_name(value), code) &&
               tf_error_fix(value) && !strcmp(tf_error_fix(value), fix) && tf_error_docs(value) && !strcmp(tf_error_docs(value), docs);
    /* raised with a detail: the message is the catalogue's with its placeholder filled, and nothing is left open */
    tf__clear_error();
    if (value == TF_ERR_SDK_TOO_OLD) tf__fail_sdk_too_old("3.1.0");
    else if (value == TF_ERR_STORAGE_FULL) tf__fail_storage_full(60000000, 1024);
    else tf__fail(value, "the detail");
    const char *got = tf_last_error(), *gfix = tf_last_error_fix();
    good = good && tf_last_error_code() == value && got && gfix && !strchr(got, '{') && !strchr(gfix, '{');
    if (strstr(msg, "{detail}") || strstr(msg, "{file}")) good = good && strstr(got, "the detail") != NULL;
    else if (!strchr(msg, '{')) good = good && !strcmp(got, msg);
    if (!strchr(fix, '{')) good = good && !strcmp(gfix, fix);
    CHECK(good, "error %d %s: name %s, message %s, fix %s", value, code, tf_error_name(value), got ? got : "(none)", gfix ? gfix : "(none)");
    ok += good;
  }
  tf__fail_sdk_too_old("3.1.0");
  CHECK(!strcmp(tf_last_error(), "This key needs SDK 3.1.0 or later; this is 3.0.0.") && strstr(tf_last_error_fix(), "to 3.1.0 or later"),
        "sdk_too_old: %s / %s", tf_last_error(), tf_last_error_fix());
  tf__fail_storage_full(60000000, 1024);
  CHECK(!strcmp(tf_last_error(), "Not enough free space for the model: 60000000 bytes needed, 1024 free."), "storage_full: %s", tf_last_error());
  /* runtime_unsupported: {minimum} comes from one constant, the README's "Before you start"; this build runs here */
  tf__fail(TF_ERR_RUNTIME_UNSUPPORTED, "a test");
  CHECK(!strcmp(tf_last_error(), "The SDK cannot run here: a test.") &&
            !strcmp(tf_last_error_fix(), "Run the app where the SDK is supported: " TF_RUNTIME_MINIMUM ".") &&
            !strcmp(TF_RUNTIME_MINIMUM, "Windows and Linux on x86-64") && !strcmp(tf_error_name(TF_ERR_RUNTIME_UNSUPPORTED), "runtime_unsupported"),
        "runtime_unsupported: %s / %s", tf_last_error(), tf_last_error_fix());
  tf__clear_error();
  CHECK(tf__runtime_check() == 0 && tf_last_error_code() == 0, "this build is refused on the machine that runs its tests");
  tf__fail(TF_ERR_MODEL_UNAVAILABLE, NULL);
  CHECK(!strcmp(tf_last_error(), "Quran Lab has no model build for SDK 3.0.0 yet."), "model_unavailable: %s", tf_last_error());
  tf__fail(TF_ERR_CHECKSUM_MISMATCH, "c16_l64/frames.zfw");
  CHECK(!strcmp(tf_last_error(), "A model file differs from the manifest: c16_l64/frames.zfw."), "checksum_mismatch: %s", tf_last_error());
  /* 3. no code outside the catalogue reaches the app: an internal failure reads as a catalogue code, 0 is "ok",
   * anything else has no name */
  static const int internal[] = {TF__ERR_NETWORK, TF__ERR_HTTP, TF__ERR_CONFIG, TF__ERR_NOT_AVAILABLE, TF__ERR_IO, TF__ERR_NO_MEMORY};
  for (size_t i = 0; i < sizeof internal / sizeof internal[0]; ++i) {
    tf__fail(internal[i], "inside");
    const int pub = tf_last_error_code();
    CHECK(pub <= -1 && pub >= -26 && tf_error_name(pub) && tf_last_error() && tf_last_error_fix(), "internal code %d reads as %d", internal[i], pub);
  }
  CHECK(!strcmp(tf_error_name(0), "ok") && !tf_error_name(-27) && !tf_error_name(1) && !tf_error_fix(0) && !tf_error_docs(-99), "names outside the catalogue");
  tf__clear_error();
  CHECK(tf_last_error_code() == 0 && tf_last_error() == NULL && tf_last_error_fix() == NULL, "an error after tf__clear_error");
  /* 4. a call on no object keeps the first failure of a chain (the quick start checks only its last call) */
  CHECK(tf_session_finish(NULL) == TF_ERR_INVALID_ARGUMENT, "finish on no session with no earlier error: %s", tf_error_name(tf_last_error_code()));
  tf__clear_error();
  tf__fail(TF_ERR_MODEL_UNAVAILABLE, NULL);
  TfSession *s = tf_session_create(NULL, NULL);
  CHECK(!s && tf_session_push(NULL, NULL, 0) == TF_ERR_MODEL_UNAVAILABLE && tf_session_finish(NULL) == TF_ERR_MODEL_UNAVAILABLE &&
            tf_last_error_code() == TF_ERR_MODEL_UNAVAILABLE && tf_heads_decoded(tf_session_heads(NULL)) == NULL &&
            tf_last_error_code() == TF_ERR_MODEL_UNAVAILABLE,
        "the first failure of a chain was replaced: %s", tf_error_name(tf_last_error_code()));
  tf__clear_error();
  tf_json_free(cat);
  printf("errors: %d/%d catalogue entries (code, value, message, fix, docs link); SHA-256 %s\n", ok, count, TF_TEST_ERRORS_SHA256);
  return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ vectors: manifest_30.json */
static int t30_in_list(const TfJson *list, const char *s) {
  for (int i = 0; list && list->type == TJ_ARR && i < list->n; ++i)
    if (tf_json_str(&list->kids[i]) && !strcmp(list->kids[i].str, s)) return 1;
  return 0;
}
static void t30_manifest(const char *data) {
  TfJson *v = t30_json(data, "manifest_30.json");
  if (!v) {
    fails++;
    return;
  }
  const char *key = tf_json_str(tf_json_get(v, "public_key_hex"));
  const TfJson *cases = tf_json_get(v, "cases");
  CHECK(!strcmp(tf_json_str(tf_json_get(v, "sdk_version")), TF_SDK_VERSION), "the vector is for SDK %s", tf_json_str(tf_json_get(v, "sdk_version")));
  int ok = 0, accepted = 0;
  for (int i = 0; cases && i < cases->n; ++i) {
    const TfJson *c = &cases->kids[i], *e = tf_json_get(c, "expect"), *cfg = tf_json_get(c, "config");
    const char *name = tf_json_str(tf_json_get(c, "name")), *text = tf_json_str(tf_json_get(c, "manifest"));
    const char *rev = cfg && cfg->type == TJ_OBJ ? tf_json_str(tf_json_get(cfg, "revision")) : NULL;
    const char *ver = cfg && cfg->type == TJ_OBJ ? tf_json_str(tf_json_get(cfg, "version")) : NULL;
    TfJson *m = NULL;
    char sha[65];
    int tol = -1, raw = -1;
    tf__clear_error();
    const int rc = tf__manifest_accept(text, strlen(text), rev, ver, key, 0, &m, sha, &tol, &raw);
    int good = !strcmp(sha, tf_json_str(tf_json_get(c, "sha256")));
    if (!strcmp(tf_json_str(tf_json_get(e, "outcome")), "refused")) {
      good = good && rc < 0 && !m && !strcmp(tf_error_name(rc), tf_json_str(tf_json_get(e, "code")));
      CHECK(good, "manifest case %s: %s (%s), want refused with %s", name, rc ? tf_error_name(rc) : "accepted", tf_last_error() ? tf_last_error() : "",
            tf_json_str(tf_json_get(e, "code")));
    } else {
      accepted++;
      good = good && rc == 0 && m && !strcmp(tf_json_str(tf_json_get(m, "version")), tf_json_str(tf_json_get(e, "version"))) &&
             !strncmp(sha, tf_json_str(tf_json_get(e, "build")), 40) && strlen(tf_json_str(tf_json_get(e, "build"))) == 40 &&
             tol == t30_bool(e, "golden_tolerance_signed") && raw == t30_bool(e, "golden_raw_signed");
      /* the text the signature verified on */
      char *st = m ? tf__manifest_text(m, tol, raw) : NULL;
      const char *want = tf_json_str(tf_json_get(c, "signed_text"));
      /* a tolerance field that is present and not signed leaves the same text as its absence: compare what was verified */
      good = good && st && want && !strcmp(st, want);
      free(st);
      /* the tolerance in force: the well-formed builds of a signed field; the malformed ones are judged on tokens */
      const TfJson *gr = m ? tf_json_get(m, "golden_raw_tolerance") : NULL, *wt = tf_json_get(e, "golden_raw_tolerance"), *only = tf_json_get(e, "tokens_only_builds");
      int well = 0, bad = 0, listed_ok = 1;
      for (int k = 0; raw == 1 && gr && gr->type == TJ_OBJ && k < gr->n; ++k) {
        if (tf__golden_raw_build_ok(&gr->kids[k])) {
          well++;
          listed_ok = listed_ok && wt && tf_json_get(wt, gr->kids[k].key) != NULL;
        } else {
          bad++;
          listed_ok = listed_ok && t30_in_list(only, gr->kids[k].key);
        }
      }
      good = good && listed_ok && bad == (only ? only->n : 0) && (raw == 1 ? wt && wt->type == TJ_OBJ && wt->n == well : !wt || wt->type == TJ_NULL);
      CHECK(good, "manifest case %s: rc %s, tolerance signed %d, raw signed %d, %d well-formed and %d malformed builds", name, rc ? tf_error_name(rc) : "ok", tol,
            raw, well, bad);
    }
    ok += good;
    tf_json_free(m);
  }
  /* versions */
  const TfJson *vs = tf_json_get(v, "versions");
  int vok = 0, vn = 0;
  for (int pass = 0; pass < 2; ++pass) {
    const TfJson *l = tf_json_get(vs, pass ? "invalid" : "valid");
    for (int i = 0; l && i < l->n; ++i, ++vn) {
      const int good = tf__is_model_version(tf_json_str(&l->kids[i])) == !pass;
      CHECK(good, "version %s: %s", tf_json_str(&l->kids[i]), pass ? "accepted" : "refused");
      vok += good;
    }
  }
  const TfJson *ord = tf_json_get(vs, "order");
  for (int i = 0; ord && i < ord->n; ++i, ++vn) {
    const TfJson *o = &ord->kids[i];
    const int good = o->n == 3 && tf__version_cmp(tf_json_str(&o->kids[0]), tf_json_str(&o->kids[1])) == (int)o->kids[2].num;
    CHECK(good, "version order %s vs %s", tf_json_str(&o->kids[0]), tf_json_str(&o->kids[1]));
    vok += good;
  }
  CHECK(tf__version_cmp("4.0", "4.0.0") == -2 && tf__version_cmp("4.0.0", "v4") == -2, "a string that is not a version was ordered");
  /* the two production manifests (test/data): they verify with the production key and are refused as 2.x builds */
  const TfJson *real = tf_json_get(v, "real_manifests");
  int rok = 0;
  for (int i = 0; real && i < real->n; ++i) {
    const char *file = tf_json_str(tf_json_get(&real->kids[i], "file"));
    const char *base = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
    char up[1100];
    snprintf(up, sizeof up, "%s/../%s", data, base);
    size_t tn = 0;
    char *text = read_text(up, &tn);
    TfJson *m = text ? tf_json_parse(text, tn, NULL) : NULL;
    const char *tk = tf_json_str(tf_json_get(&real->kids[i], "trusted_key"));
    const int rc = text ? tf__manifest_accept(text, tn, NULL, NULL, tk, 0, NULL, NULL, NULL, NULL) : 0;
    const int good = m && tf__manifest_verify(m, tk, NULL, NULL) == 1 && rc == TF_ERR_MODEL_INCOMPATIBLE &&
                     !strcmp(tf_json_str(tf_json_get(tf_json_get(&real->kids[i], "expect"), "code")), "model_incompatible");
    CHECK(good, "%s: verify with the production key and model_incompatible expected, got %s", base, rc ? tf_error_name(rc) : "accepted");
    rok += good;
    tf_json_free(m);
    free(text);
  }
  CHECK(cases && cases->n == 35 && ok == cases->n, "manifest cases %d/%d", ok, cases ? cases->n : 0);
  printf("manifest_30.json: %d/%d cases (%d accepted with their signed text and tolerance, %d refused with their code); %d/%d version checks; "
         "%d/%d production manifests verify and are refused as builds for SDK 2\n",
         ok, cases ? cases->n : 0, accepted, cases ? cases->n - accepted : 0, vok, vn, rok, real ? real->n : 0);
  tf_json_free(v);
}

/* ------------------------------------------------------------------ vectors: speech.json */
static void t30_speech(const char *data) {
  TfJson *v = t30_json(data, "speech.json");
  if (!v) {
    fails++;
    return;
  }
  const TfJson *cases = tf_json_get(v, "cases");
  int ok = 0;
  for (int i = 0; cases && i < cases->n; ++i) {
    const TfJson *c = &cases->kids[i], *e = tf_json_get(c, "expect"), *fr = tf_json_get(c, "frames"), *tmj = tf_json_get(c, "timing");
    const char *name = tf_json_str(tf_json_get(c, "name"));
    const int T = fr->n, per = (int)t30_num(c, "chunk_frames");
    const TfJson *eos = tf_json_get(c, "endOnSilenceMs");
    int *ids = (int *)malloc(((size_t)T + 1) * sizeof(int)), *tm = (int *)calloc(2 * (size_t)T + 2, sizeof(int));
    for (int t = 0; t < T; ++t) ids[t] = (int)fr->kids[t].num;
    for (int t = 0; tmj && t < 2 * T && t < tmj->n; ++t) tm[t] = (int)tmj->kids[t].num;
    int n_started = 0, n_ended = 0, n_take_ended = 0, started_ms = -1, ended_ms = -1, ended_silence = -1, ended_chunk = -1;
    TfSpeech sp;
    tf__speech_init(&sp, eos && eos->type == TJ_NUM ? (int)eos->num : 0);
    /* the take is delivered in chunks; an event is raised with the chunk that carries its frame; with
     * endOnSilenceMs the chunk that carries the frame of speechEnded is whole and is the last */
    const TfJson *wev = tf_json_get(e, "events"), *wcs = tf_json_get(e, "chunk_speech");
    int used = 0, chunk = 0, nev = 0, good = tmj && tmj->n == 2 * T, ended = 0;
    for (int at = 0; at < T && !ended; at += per, ++chunk) {
      int speech = 0;
      for (int t = at; t < at + per && t < T; ++t) {
        TfSpeechEvent ev;
        if (tf__speech_frame(&sp, tf__speech_active(ids[t], tm[2 * t], tm[2 * t + 1]), &ev)) {
          const TfJson *w = wev && nev < wev->n ? &wev->kids[nev] : NULL;
          const char *wn = tf_json_str(tf_json_get(w, "event"));
          good = good && w && wn && !strcmp(wn, ev.type == TF_EVENT_SPEECH_STARTED ? "speechStarted" : "speechEnded") &&
                 (int)t30_num(w, "atMs") == ev.at_ms && (int)t30_num(w, "chunk") == chunk &&
                 (ev.type == TF_EVENT_SPEECH_STARTED || (int)t30_num(w, "silenceMs") == ev.silence_ms);
          nev++;
          if (ev.type == TF_EVENT_SPEECH_STARTED) {
            if (!n_started++) started_ms = ev.at_ms;
          } else {
            n_ended++;
            ended_ms = ev.at_ms, ended_silence = ev.silence_ms, ended_chunk = chunk;
          }
        }
        speech |= sp.active;
        used++;
      }
      good = good && wcs && chunk < wcs->n && (wcs->kids[chunk].type == TJ_BOOL ? wcs->kids[chunk].boolean : (int)wcs->kids[chunk].num) == speech;
      if (sp.ended) {
        const TfJson *w = wev && nev < wev->n ? &wev->kids[nev] : NULL;
        good = good && w && !strcmp(tf_json_str(tf_json_get(w, "event")), "ended") && !strcmp(tf_json_str(tf_json_get(w, "reason")), "silence") &&
               (int)t30_num(w, "chunk") == chunk;
        nev++;
        n_take_ended++;
        ended = 1;
      }
    }
    good = good && nev == (wev ? wev->n : 0) && chunk == (wcs ? wcs->n : 0) && used == (int)t30_num(e, "frames_used");
    /* the totals of the take's frames, and the tokens per head */
    const int speech = tf__speech_frames(ids, tm, used);
    good = good && speech == (int)t30_num(e, "speech_frames") && used - speech == (int)t30_num(e, "silence_frames") &&
           fabs(speech * 0.04 - t30_num(e, "speech_s")) < 1e-9 && fabs((used - speech) * 0.04 - t30_num(e, "silence_s")) < 1e-9;
    const TfJson *ws = tf_json_get(e, "speech");
    for (int t = 0; good && ws && t < used; ++t) {
      /* frame by frame: speech when log_probs or one of the two timing frames is non-blank; no padding */
      good = (int)ws->kids[t].num == (ids[t] != 0 || tm[2 * t] != 0 || tm[2 * t + 1] != 0);
    }
    const TfJson *wtok = tf_json_get(e, "tokens"), *oh = tf_json_get(c, "other_heads");
    good = good && (int)t30_num(wtok, "log_probs") == tf__token_runs(ids, used);
    for (int k = 0; good && oh && k < oh->n; ++k) {
      const TfJson *h = &oh->kids[k];
      int *hid = (int *)malloc(((size_t)h->n + 1) * sizeof(int));
      for (int t = 0; t < h->n; ++t) hid[t] = (int)h->kids[t].num;
      good = (int)t30_num(wtok, h->key) == tf__token_runs(hid, h->n);
      free(hid);
    }
    CHECK(good, "speech case %s: %d events, %d chunks, %d frames used, %d speech frames", name, nev, chunk, used, speech);
    ok += good;
    /* three results on the fixture's take 20, by name and with their numbers */
    if (!strncmp(name, "fixture take 20: the longest held vowel", 39)) {
      int run = 0, longest = 0, from = -1, first = -1, lastt = -1;
      for (int t = 0; t < T; ++t)
        if (ids[t] != 0) first = first < 0 ? t : first, lastt = t;
      for (int t = first; t >= 0 && t <= lastt; ++t) {
        run = ids[t] == 0 ? run + 1 : 0;
        if (run > longest) longest = run, from = t - run + 1;
      }
      CHECK(good && T == 128 && used == 128 && longest == 39 && from == 57 && n_started == 1 && started_ms == 280 && n_ended == 0 && n_take_ended == 0,
            "the longest held vowel: %d frames, log_probs blank for %d frames from frame %d, %d speechStarted at %d ms, %d speechEnded", T, longest, from, n_started,
            started_ms, n_ended);
      printf("speech, the longest held vowel of the fixture does not end the take: %d frames, log_probs blank for %d frames (%.2f s) from frame %d, "
             "one speechStarted at %d ms, %d speechEnded at the default 1500 ms\n", T, longest, longest * 0.04, from, started_ms, n_ended);
    } else if (!strncmp(name, "fixture take 20 followed by silence", 35)) {
      CHECK(good && n_ended == 1 && ended_ms == 4000 && ended_silence == 1520 && ended_chunk == 17 && per == 8 && used == 144,
            "real silence: speechEnded at %d ms, silence %d ms, chunk %d of %d-frame chunks, %d frames used", ended_ms, ended_silence, ended_chunk, per, used);
      printf("speech, real silence ends the take 1500 ms after the last active frame: speechEnded atMs %d, silenceMs %d, raised with chunk %d of %d-frame "
             "chunks, %d frames used\n", ended_ms, ended_silence, ended_chunk, per, used);
    } else if (!strncmp(name, "fixture take 20 cut at the end of its held vowel", 48)) {
      CHECK(good && n_started == 1 && n_ended == 1 && ended_ms == 3560 && n_take_ended == 1, "a held vowel then silence: %d speechStarted, %d speechEnded at %d ms, %d ended",
            n_started, n_ended, ended_ms, n_take_ended);
      printf("speech, a clip that ends on a held vowel followed by silence ends once: %d speechStarted, %d speechEnded atMs %d, %d ended\n", n_started, n_ended,
             ended_ms, n_take_ended);
    }
    free(ids);
    free(tm);
  }
  /* the option's values */
  const TfJson *opt = tf_json_get(v, "endOnSilenceMs");
  int vok = 1;
  const TfJson *valid = tf_json_get(opt, "valid"), *invalid = tf_json_get(opt, "invalid_argument");
  for (int i = 0; valid && i < valid->n; ++i) vok = vok && tf__end_on_silence_ok((int)valid->kids[i].num);
  for (int i = 0; invalid && i < invalid->n; ++i)
    if (tf_json_is_int(&invalid->kids[i])) vok = vok && !tf__end_on_silence_ok((int)invalid->kids[i].num); /* the others cannot reach a C int */
  CHECK(vok && tf__end_on_silence_ok(0) && !tf__end_on_silence_ok(499) && tf__end_on_silence_ok(500), "endOnSilenceMs: off is 0, valid is 500 to 60000");
  CHECK(cases && cases->n == 32 && ok == cases->n, "speech cases %d/%d", ok, cases ? cases->n : 0);
  printf("speech.json: %d/%d cases (speech frames, totals, tokens per head, chunk.speech, the events and the chunk that raises each)\n", ok,
         cases ? cases->n : 0);
  tf_json_free(v);
}

/* ------------------------------------------------------------------ vectors: settled.json */
/* log-probs whose greedy id per frame is ids[t]: the decoded layer then sees exactly these top-1 ids */
static float *t30_logp(const int *ids, int T) {
  float *x = (float *)malloc(((size_t)T * ZF_TAJWEED_VOCAB + 1) * sizeof(float));
  for (int t = 0; t < T; ++t)
    for (int k = 0; k < ZF_TAJWEED_VOCAB; ++k) x[(size_t)t * ZF_TAJWEED_VOCAB + k] = k == ids[t] ? -0.125f : -12.0f - 0.25f * (float)k;
  return x;
}
static void t30_settled(const char *data) {
  TfJson *v = t30_json(data, "settled.json");
  if (!v) {
    fails++;
    return;
  }
  const TfJson *cases = tf_json_get(v, "cases");
  int ok = 0, steps_seen = 0;
  for (int i = 0; cases && i < cases->n; ++i) {
    const TfJson *c = &cases->kids[i], *steps = tf_json_get(c, "steps");
    int ids[2048], T = 0, starts[256], nchunks = 0, good = 1, last_settled = 0;
    for (int k = 0; steps && k < steps->n; ++k, ++steps_seen) {
      const TfJson *st = &steps->kids[k], *e = tf_json_get(st, "expect");
      const TfJson *chunk = tf_json_get(st, "chunk"), *bc = tf_json_get(st, "backendChanged");
      int replaced = 0;
      if (chunk) {
        starts[nchunks++] = T;
        for (int t = 0; t < chunk->n; ++t) ids[T++] = (int)chunk->kids[t].num;
      } else if (bc) {
        /* a failover replaces every chunk delivered from fromChunk on; the decoded layer is rebuilt from the frames
         * now in force (this SDK has no accelerator: the rule is the decoded layer's own) */
        const int from = (int)t30_num(bc, "fromChunk");
        const TfJson *nc = tf_json_get(bc, "chunks");
        T = starts[from];
        nchunks = from;
        for (int q = 0; nc && q < nc->n; ++q) {
          starts[nchunks++] = T;
          for (int t = 0; t < nc->kids[q].n; ++t) ids[T++] = (int)nc->kids[q].kids[t].num;
        }
        replaced = 1;
      }
      float *lp = t30_logp(ids, T);
      ZfTajweedHeads h;
      memset(&h, 0, sizeof h);
      h.n40 = T;
      h.n20 = 2 * T;
      h.vocab = ZF_TAJWEED_VOCAB;
      h.log_probs = lp;
      TfDecoded *d = tf__decode(NULL, &h, NULL, 0);
      tf__decoded_mark(d, -1, t30_bool(st, "final"));
      const TfJson *wt = tf_json_get(e, "tokens");
      int sg = d && d->n_frames == (int)t30_num(e, "frames") && d->n_tokens == (wt ? wt->n : -1) && d->settled == (int)t30_num(e, "settled") &&
               d->is_final == t30_bool(e, "isFinal");
      for (int q = 0; sg && q < d->n_tokens; ++q) {
        const TfJson *w = &wt->kids[q];
        const TfToken *tk = &d->tokens[q];
        sg = tk->id == (int)t30_num(w, "id") && tk->first_frame == (int)t30_num(w, "firstFrame") && tk->last_frame == (int)t30_num(w, "lastFrame") &&
             tk->first_ms == (int)t30_num(w, "firstMs") && tk->last_ms == (int)t30_num(w, "lastMs") && tk->end_ms == tk->last_ms + 40;
      }
      /* settled never decreases within a take, except once after a replacement */
      if (d && !replaced && d->settled < last_settled) sg = 0;
      if (d) last_settled = d->settled;
      CHECK(sg, "settled case %s, step %d: %d tokens, settled %d, final %d", tf_json_str(tf_json_get(c, "name")), k, d ? d->n_tokens : -1, d ? d->settled : -1,
            d ? d->is_final : -1);
      good = good && sg;
      tf__decoded_free(d);
      free(lp);
    }
    ok += good;
  }
  CHECK(cases && cases->n == 11 && ok == cases->n, "settled cases %d/%d", ok, cases ? cases->n : 0);
  printf("settled.json: %d/%d cases, %d steps (tokens with first and last frame and ms, settled, isFinal, replacements)\n", ok, cases ? cases->n : 0,
         steps_seen);
  tf_json_free(v);
}

/* ------------------------------------------------------------------ vectors: telemetry_30.json */
static TfClient *t30_client(const char *dir, TfAnalytics analytics) {
  tf_mkdirs(dir);
  TfClient *c = tf_client_create(&(TfClientOptions){.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa", .app_id = "org.example.Vectors", .app_version = "3.2",
                                                    .storage = dir, .analytics = analytics});
  if (!c) fprintf(stderr, "client: %s\n", tf_last_error());
  return c;
}
/* today's counts as the delta the client would send */
static TfJson *t30_day(TfClient *c) {
  char *b = tf__client_day_body(c);
  TfJson *j = b ? tf_json_parse(b, strlen(b), NULL) : NULL;
  free(b);
  return j;
}
/* the six app-reported fields of a body are exactly `want` (an object of the vector), and `reported` lists
 * exactly those that are there, in the vector's order */
static int t30_reported_is(const TfJson *body, const TfJson *want) {
  static const char *const six[] = {"ayahs", "surah_ayahs", "coverage", "words", "rule_checks", "rule_flags"};
  int good = 1, listed = 0;
  const TfJson *rep = tf_json_get(body, "reported");
  for (int i = 0; i < 6; ++i) {
    const TfJson *g = tf_json_get(body, six[i]), *w = tf_json_get(want, six[i]);
    good = good && !g == !w;
    if (g && w) {
      TfBuf a, b;
      tf_buf_init(&a);
      tf_buf_init(&b);
      tf_json_write(&a, g);
      tf_json_write(&b, w);
      if (g->type == TJ_OBJ) {
        /* the same keys with the same counts, whatever their order */
        good = good && g->n == w->n;
        for (int k = 0; good && k < w->n; ++k) good = tf_json_get(g, w->kids[k].key) && tf_json_get(g, w->kids[k].key)->num == w->kids[k].num;
      } else {
        good = good && a.p && b.p && !strcmp(a.p, b.p);
      }
      tf_buf_free(&a);
      tf_buf_free(&b);
    }
    /* the list is built from the payload: a field that is there is listed, at its place; one that is not is not */
    if (g) good = good && rep && listed < rep->n && !strcmp(tf_json_str(&rep->kids[listed]), six[i]) && ++listed;
  }
  return good && (listed ? rep->n == listed : rep == NULL);
}

static void t30_telemetry(const char *data, const char *work) {
  TfJson *v = t30_json(data, "telemetry_30.json");
  if (!v) {
    fails++;
    return;
  }
  char d[1100];
  tf_http_set_transport(t30_down, NULL);
  tf__now_override = 1794355200 + 9 * 3600; /* 2026-11-11, mid-morning UTC: every call of a test falls on one day */
  /* 1. the 6.2 counters: the six takes into the day's cohorts */
  snprintf(d, sizeof d, "%s/takes", work);
  TfClient *c = t30_client(d, TF_ANALYTICS_ON);
  const TfJson *takes = tf_json_get(v, "takes");
  for (int i = 0; c && takes && i < takes->n; ++i) {
    const TfJson *t = &takes->kids[i], *heads = tf_json_get(t, "heads"), *sel = tf_json_get(t, "selected"), *cu = tf_json_get(t, "chunk_compute_us");
    const char *end = tf_json_str(tf_json_get(t, "end"));
    TfSessionStats st;
    memset(&st, 0, sizeof st);
    st.version = tf_json_str(tf_json_get(v, "version"));
    st.build = tf_json_str(tf_json_get(v, "build"));
    st.context = tf_json_str(tf_json_get(t, "context"));
    st.aborted = !strcmp(end, "cancel") || !strcmp(end, "ended:error"); /* a take counts when it ended with its Heads */
    st.chunks = cu ? cu->n : 0;
    const double samples = t30_num(t, "samples"), rate = t30_num(t, "sample_rate");
    st.audio_s = samples / rate;
    st.audio_ms = (long long)(1000.0 * samples) / (long long)rate;
    for (int k = 0; cu && k < cu->n; ++k) st.compute_us += (long long)cu->kids[k].num;
    st.compute_s = (double)st.compute_us / 1e6;
    const TfJson *lp = tf_json_get(heads, "log_probs");
    int *ids = (int *)malloc(((size_t)lp->n + 1) * sizeof(int));
    for (int k = 0; k < lp->n; ++k) ids[k] = (int)lp->kids[k].num;
    /* the timing head is given for every take: the SDK always computes it for speech and silence */
    const TfJson *tmh = tf_json_get(heads, "timing_log_probs");
    int *tmi = (int *)calloc(2 * (size_t)lp->n + 2, sizeof(int));
    for (int k = 0; tmh && k < tmh->n && k < 2 * lp->n; ++k) tmi[k] = (int)tmh->kids[k].num;
    CHECK(tmh && tmh->n == 2 * lp->n, "telemetry take %d: the vector gives %d timing ids for %d frames", i, tmh ? tmh->n : -1, lp->n);
    st.speech_frames = tf__speech_frames(ids, tmi, lp->n);
    st.silence_frames = lp->n - st.speech_frames;
    free(ids);
    free(tmi);
    for (int k = 0; sel && k < sel->n; ++k) {
      const int h = tf_head_from_name(tf_json_str(&sel->kids[k]));
      const TfJson *hv = tf_json_get(heads, tf_json_str(&sel->kids[k]));
      if (h < 0 || !hv || strcmp(tf_head_info(h)->kind, "log_softmax")) continue;
      int *hid = (int *)malloc(((size_t)hv->n + 1) * sizeof(int));
      for (int q = 0; q < hv->n; ++q) hid[q] = (int)hv->kids[q].num;
      st.tokens[h] = tf__token_runs(hid, hv->n);
      free(hid);
    }
    tf__client_session_finished(c, &st);
  }
  TfJson *day = c ? t30_day(c) : NULL;
  const TfJson *got = tf_json_get(day, "cohorts"), *want = tf_json_get(v, "cohorts");
  int cok = 0;
  CHECK(got && want && got->n == want->n, "cohorts: %d, want %d", got ? got->n : -1, want ? want->n : -1);
  for (int i = 0; got && want && i < want->n && i < got->n; ++i) {
    const TfJson *g = &got->kids[i], *w = &want->kids[i];
    int good = 1;
    for (int k = 0; k < w->n; ++k) {
      const TfJson *wk = &w->kids[k], *gk = tf_json_get(g, wk->key);
      if (wk->type == TJ_STR) good = good && gk && tf_json_str(gk) && !strcmp(gk->str, wk->str);
      else if (wk->type == TJ_NUM) good = good && gk && gk->type == TJ_NUM && fabs(gk->num - wk->num) < 1e-9;
      else if (wk->type == TJ_OBJ) {
        good = good && gk && gk->type == TJ_OBJ && gk->n == wk->n;
        for (int q = 0; good && q < wk->n; ++q) good = tf_json_get(gk, wk->kids[q].key) && tf_json_get(gk, wk->kids[q].key)->num == wk->kids[q].num;
      }
    }
    /* nothing the scorer or the settings fed is left in a cohort */
    const TfJson *gone = tf_json_get(v, "cohort_fields_removed");
    for (int k = 0; gone && k < gone->n; ++k) good = good && !tf_json_get(g, tf_json_str(&gone->kids[k]));
    CHECK(good, "cohort %d (%s) differs from the vector", i, tf_json_str(tf_json_get(w, "context")));
    cok += good;
  }
  CHECK(day && t30_reported_is(day, tf_json_get(tf_json_get(v, "nothing_reported"), "fields")), "a day without a report carries app-reported fields");
  /* every field of the delta is one the example payload has: the server's schema is closed */
  const TfJson *example = tf_json_get(v, "example_payload");
  keys_within(day, example, "delta", 0);
  if (got && got->n) keys_within(&got->kids[0], &tf_json_get(example, "cohorts")->kids[0], "delta.cohort", 0);
  tf_json_free(day);
  tf_client_destroy(c);

  /* 2. reportChecks: single calls, the day's sequence, 33 rules a day */
  const TfJson *rc = tf_json_get(v, "report_checks");
  snprintf(d, sizeof d, "%s/checks", work);
  c = t30_client(d, TF_ANALYTICS_ON);
  int vok = 0, vn = 0;
  for (int pass = 0; c && pass < 2; ++pass) {
    const TfJson *l = tf_json_get(rc, pass ? "refused" : "valid");
    for (int i = 0; l && i < l->n; ++i, ++vn) {
      const TfJson *k = &l->kids[i];
      const int r = tf_client_report_checks(c, tf_json_str(&k->kids[0]), (int)k->kids[1].num, (int)k->kids[2].num);
      const int good = pass ? r == TF_ERR_INVALID_ARGUMENT && tf_last_error_code() == TF_ERR_INVALID_ARGUMENT : r == 0;
      CHECK(good, "reportChecks(%s, %d, %d): %d", tf_json_str(&k->kids[0]), (int)k->kids[1].num, (int)k->kids[2].num, r);
      vok += good;
    }
  }
  tf_client_destroy(c);
  static const char *const seqs[] = {"sequence", "rules_a_day"};
  int sok = 0;
  for (int q = 0; q < 2; ++q) {
    const TfJson *sq = tf_json_get(rc, seqs[q]), *calls = tf_json_get(sq, "calls"), *results = tf_json_get(sq, "results");
    snprintf(d, sizeof d, "%s/checks-%s", work, seqs[q]);
    c = t30_client(d, TF_ANALYTICS_ON);
    int good = c != NULL;
    for (int i = 0; c && calls && i < calls->n; ++i) {
      const TfJson *k = tf_json_get(&calls->kids[i], "call") ? tf_json_get(&calls->kids[i], "call") : &calls->kids[i];
      const char *wr = tf_json_get(&calls->kids[i], "result") ? tf_json_str(tf_json_get(&calls->kids[i], "result")) : tf_json_str(&results->kids[i]);
      const int r = tf_client_report_checks(c, tf_json_str(&k->kids[0]), (int)k->kids[1].num, (int)k->kids[2].num);
      good = good && !strcmp(r == 0 ? "ok" : tf_error_name(r), wr);
    }
    day = c ? t30_day(c) : NULL;
    good = good && day && t30_reported_is(day, tf_json_get(sq, "day_after"));
    CHECK(good, "reportChecks %s: the day's fields differ from the vector", seqs[q]);
    sok += good;
    tf_json_free(day);
    tf_client_destroy(c);
  }

  /* 3. reportRecited: single calls, and the takes of one day (the report belongs to its take) */
  const TfJson *rr = tf_json_get(v, "report_recited");
  int rok = 0, rn = 0;
  for (int pass = 0; pass < 2; ++pass) {
    const TfJson *l = tf_json_get(rr, pass ? "refused" : "valid");
    for (int i = 0; l && i < l->n; ++i, ++rn) {
      const TfJson *k = &l->kids[i];
      const int good = tf__recited_ok((int)k->kids[0].num, (int)k->kids[1].num, (int)k->kids[2].num) == !pass;
      CHECK(good, "reportRecited(%d, %d, %d) %s", (int)k->kids[0].num, (int)k->kids[1].num, (int)k->kids[2].num, pass ? "accepted" : "refused");
      rok += good;
    }
  }
  snprintf(d, sizeof d, "%s/recited", work);
  c = t30_client(d, TF_ANALYTICS_ON);
  const TfJson *rt = tf_json_get(rr, "takes");
  int tok = 0;
  for (int i = 0; c && rt && i < rt->n; ++i) {
    const TfJson *t = &rt->kids[i], *steps = tf_json_get(t, "steps"), *results = tf_json_get(t, "results");
    TfRecited r;
    memset(&r, 0, sizeof r);
    int state = 0, nres = 0, good = 1; /* 0: the take is open; 1: it ended with its Heads; 2: cancelled or failed */
    for (int k = 0; steps && k < steps->n; ++k) {
      const TfJson *st = &steps->kids[k];
      if (!strcmp(tf_json_str(&st->kids[0]), "end")) {
        const char *how = tf_json_str(&st->kids[1]);
        state = !strcmp(how, "cancel") || !strcmp(how, "ended:error") ? 2 : 1;
        tf__recited_end(&r, c, state == 1);
      } else {
        const int rcode = tf__recited_report(&r, c, state, (int)st->kids[1].num, (int)st->kids[2].num, (int)st->kids[3].num);
        good = good && results && nres < results->n && !strcmp(rcode == 0 ? "ok" : tf_error_name(rcode), tf_json_str(&results->kids[nres]));
        nres++;
      }
    }
    day = t30_day(c);
    good = good && nres == (results ? results->n : 0) && day && t30_reported_is(day, tf_json_get(t, "day_after"));
    CHECK(good, "reportRecited take \"%s\": the day's fields differ from the vector", tf_json_str(tf_json_get(t, "name")));
    tok += good;
    tf_json_free(day);
  }
  tf_client_destroy(c);

  /* 4. the example payload's app-reported fields from its reports, and `reported` for every combination of
   * reports the two calls can make (the list is built from the payload itself) */
  snprintf(d, sizeof d, "%s/example", work);
  c = t30_client(d, TF_ANALYTICS_ON);
  const TfJson *ex = tf_json_get(v, "example_reports");
  const TfJson *er = tf_json_get(ex, "recited"), *ec = tf_json_get(ex, "checks");
  for (int i = 0; c && er && i < er->n; ++i) {
    TfRecited r;
    memset(&r, 0, sizeof r);
    tf__recited_report(&r, c, 0, (int)er->kids[i].kids[0].num, (int)er->kids[i].kids[1].num, (int)er->kids[i].kids[2].num);
    tf__recited_end(&r, c, 1);
  }
  for (int i = 0; c && ec && i < ec->n; ++i)
    tf_client_report_checks(c, tf_json_str(&ec->kids[i].kids[0]), (int)ec->kids[i].kids[1].num, (int)ec->kids[i].kids[2].num);
  day = c ? t30_day(c) : NULL;
  CHECK(day && t30_reported_is(day, example), "the example payload's app-reported fields differ");
  keys_within(day, example, "delta with reports", 0);
  tf_json_free(day);
  tf_client_destroy(c);
  int combos = 0, combos_ok = 0;
  for (int mask = 0; mask < 16; ++mask, ++combos) {
    snprintf(d, sizeof d, "%s/combo%d", work, mask);
    c = t30_client(d, TF_ANALYTICS_ON);
    if (!c) continue;
    if (mask & 1) {
      TfRecited r;
      memset(&r, 0, sizeof r);
      tf__recited_report(&r, c, 1, 36, 1, 14);
    }
    if (mask & 2) tf_client_report_checks(c, "madd", 40, mask & 8 ? 3 : 0);
    if (mask & 4) tf_client_report_checks(c, "word", 140, mask & 8 ? 5 : 0);
    day = t30_day(c);
    /* what the body carries decides the list, field by field */
    static const char *const six[] = {"ayahs", "surah_ayahs", "coverage", "words", "rule_checks", "rule_flags"};
    const TfJson *rep = tf_json_get(day, "reported");
    int good = day != NULL, present = 0;
    for (int i = 0; i < 6; ++i) {
      const int has = tf_json_get(day, six[i]) != NULL;
      good = good && has == t30_in_list(rep, six[i]);
      present += has;
    }
    good = good && (present ? rep && rep->n == present : rep == NULL);
    good = good && !!tf_json_get(day, "ayahs") == !!(mask & 1) && !!tf_json_get(day, "coverage") == !!(mask & 1) &&
           !!tf_json_get(day, "words") == !!(mask & 4) && !!tf_json_get(day, "rule_checks") == !!(mask & 6) &&
           !!tf_json_get(day, "rule_flags") == !!((mask & 6) && (mask & 8));
    CHECK(good, "reported for the combination %d of reports", mask);
    combos_ok += good;
    tf_json_free(day);
    tf_client_destroy(c);
  }
  /* 5. analytics off: the calls are still validated and nothing is counted */
  snprintf(d, sizeof d, "%s/off", work);
  c = t30_client(d, TF_ANALYTICS_OFF);
  CHECK(c && tf_client_report_checks(c, "madd", 5, 1) == 0 && tf_client_report_checks(c, "Madd", 5, 1) == TF_ERR_INVALID_ARGUMENT, "analytics off: validation");
  day = c ? t30_day(c) : NULL;
  CHECK(day && !tf_json_get(day, "rule_checks") && !tf_json_get(day, "reported") && !tf_json_get(day, "launches"), "analytics off counted something");
  tf_json_free(day);
  tf_client_destroy(c);
  tf_http_set_transport(NULL, NULL);
  tf__now_override = 0;
  printf("telemetry_30.json: %d/%d cohorts (takes, audio_s, speech_s, silence_s, tokens, compute_s; removed fields gone); reportChecks %d/%d calls and "
         "%d/2 sequences; reportRecited %d/%d calls and %d/%d takes; reported lists exactly the fields present in %d/%d combinations\n",
         cok, want ? want->n : 0, vok, vn, sok, rok, rn, tok, rt ? rt->n : 0, combos_ok, combos);
  /* ayah_counts.json */
  TfJson *ac = t30_json(data, "ayah_counts.json");
  const TfJson *counts = tf_json_get(ac, "counts") ? tf_json_get(ac, "counts") : tf_json_get(ac, "ayahs");
  int total = 0, same = counts && counts->type == TJ_ARR && counts->n == 114;
  for (int s = 1; same && s <= 114; ++s) {
    same = tf__ayah_count(s) == (int)counts->kids[s - 1].num;
    total += tf__ayah_count(s);
  }
  CHECK(same && total == 6236 && tf__ayah_count(0) == 0 && tf__ayah_count(115) == 0, "the 114 ayah counts (%d in all)", total);
  printf("ayah_counts.json: 114 counts, %d ayahs\n", total);
  tf_json_free(ac);
  tf_json_free(v);
}

/* ------------------------------------------------------------------ vectors: inference_context.json, names.json */
static void t30_context(const char *data) {
  TfJson *v = t30_json(data, "inference_context.json");
  if (!v) {
    fails++;
    return;
  }
  const TfJson *cases = tf_json_get(v, "cases");
  int ok = 0;
  for (int i = 0; cases && i < cases->n; ++i) {
    const TfJson *c = &cases->kids[i], *e = tf_json_get(c, "expect"), *man = tf_json_get(c, "manifest"), *cx = tf_json_get(man, "contexts");
    const char *list[16];
    int n = 0;
    for (int k = 0; cx && k < cx->n && n < 16; ++k) list[n++] = tf_json_str(&cx->kids[k]);
    const TfJson *cfg = tf_json_get(c, "config");
    TfBuf b;
    tf_buf_init(&b);
    if (cfg && cfg->type != TJ_NULL) tf_json_write(&b, cfg);
    char out[64] = "";
    const char *src = NULL;
    tf__clear_error();
    const int rc = tf__resolve_context(tf_json_str(tf_json_get(c, "app")), cfg && cfg->type != TJ_NULL ? b.p : NULL, list, n,
                                       tf_json_str(tf_json_get(man, "default_context")), out, sizeof out, &src);
    const char *werr = tf_json_str(tf_json_get(e, "error"));
    const int good = werr ? rc < 0 && !strcmp(tf_error_name(rc), werr)
                          : rc == 0 && !strcmp(out, tf_json_str(tf_json_get(e, "context"))) && src && !strcmp(src, tf_json_str(tf_json_get(e, "source")));
    CHECK(good, "context case %s: %s from %s (rc %d)", tf_json_str(tf_json_get(c, "name")), out, src ? src : "?", rc);
    ok += good;
    tf_buf_free(&b);
  }
  CHECK(cases && cases->n == 19 && ok == cases->n, "inference context cases %d/%d", ok, cases ? cases->n : 0);
  printf("inference_context.json: %d/%d cases (the app, then the config's inference.context, then the manifest's default)\n", ok, cases ? cases->n : 0);
  tf_json_free(v);
}

static void t30_names(const char *data) {
  TfJson *v = t30_json(data, "names.json");
  if (!v) {
    fails++;
    return;
  }
  /* the twelve heads through the public TfHead of a Heads object that holds them all */
  const TfJson *heads = tf_json_get(v, "heads");
  static float zeros[64];
  ZfTajweedHeads z;
  memset(&z, 0, sizeof z);
  z.n40 = 1;
  z.n20 = 1;
  z.vocab = ZF_TAJWEED_VOCAB;
  z.log_probs = z.inter1_log_probs = z.inter4_log_probs = z.id_log_probs = z.timing_log_probs = z.gem = z.attr_tafkheem = z.attr_ghunna = z.attr_qalqalah =
      z.attr_face = z.boundary = z.conf = zeros;
  TfHeads all;
  tf__heads_view(&all, &z, TF_HEADS_ALL, NULL);
  int hok = heads && heads->n == 12 && tf_heads_count(&all) == 12;
  for (int i = 0; hok && i < heads->n; ++i) {
    const TfJson *h = &heads->kids[i];
    TfHead hd;
    hok = tf_heads_get(&all, tf_json_str(tf_json_get(h, "name")), &hd) == 0 && !strcmp(tf_heads_name(&all, i), hd.name) &&
          !strcmp(hd.kind, tf_json_str(tf_json_get(h, "kind"))) && hd.frame_ms == (int)t30_num(h, "frameMs") && hd.classes == (int)t30_num(h, "classes");
  }
  CHECK(hok, "the twelve heads (name, kind, frame period, classes)");
  TfHead none;
  CHECK(tf_heads_get(&all, "logits", &none) == TF_ERR_INVALID_ARGUMENT, "an unknown head name was accepted");
  tf__clear_error();
  /* the default selection and the keyword for all of them */
  const TfJson *dh = tf_json_get(v, "default_heads");
  unsigned mask = 0;
  for (int i = 0; dh && i < dh->n; ++i) mask |= TF_HEAD_BIT(tf_head_from_name(tf_json_str(&dh->kids[i])));
  CHECK(mask == TF_HEADS_DEFAULT && tf__heads_mask(NULL) == TF_HEADS_DEFAULT && tf__heads_mask(tf_json_str(tf_json_get(v, "all_heads_keyword"))) == TF_HEADS_ALL &&
            tf__heads_mask("gem") == (TF_HEAD_BIT(TF_HEAD_GEM) | TF_HEAD_BIT(TF_HEAD_LOG_PROBS)) && tf__heads_mask("gem,nope") == 0,
        "the default head selection");
  tf__clear_error();
  const TfJson *tok = tf_json_get(v, "tokens");
  CHECK((int)t30_num(tok, "blank") == ZF_TAJWEED_BLANK && (int)t30_num(tok, "count") == ZF_TAJWEED_VOCAB && (int)t30_num(tok, "word_bar") == 1, "tokens");
  /* events and reasons */
  static const int ev_types[] = {TF_EVENT_SPEECH_STARTED, TF_EVENT_SPEECH_ENDED, TF_EVENT_INTERRUPTED, TF_EVENT_RESUMED, TF_EVENT_BACKEND_CHANGED, TF_EVENT_ENDED};
  static const int reasons[] = {TF_ENDED_SILENCE, TF_ENDED_BACKGROUND, TF_ENDED_HIDDEN, TF_ENDED_INTERRUPTED, TF_ENDED_SOURCE_END, TF_ENDED_LIMIT, TF_ENDED_ERROR};
  const TfJson *evs = tf_json_get(v, "events"), *ers = tf_json_get(v, "ended_reasons");
  int eok = evs && evs->n == 6 && ers && ers->n == 7;
  for (int i = 0; eok && i < 6; ++i) eok = !strcmp(tf__event_name(ev_types[i]), tf_json_str(tf_json_get(&evs->kids[i], "name")));
  for (int i = 0; eok && i < 7; ++i) eok = !strcmp(tf__ended_reason_name(reasons[i]), tf_json_str(&ers->kids[i]));
  CHECK(eok, "event names and ended reasons");
  /* the Backend's values here, the option values, the error codes */
  const TfJson *be = tf_json_get(v, "backend");
  CHECK(t30_in_list(tf_json_get(be, "kind"), "cpu") && t30_in_list(tf_json_get(be, "name"), "cpu") && t30_in_list(tf_json_get(be, "stage"), "cpu"), "backend values");
  const TfJson *so = tf_json_get(v, "session_options"), *eo = tf_json_get(so, "endOnSilenceMs");
  CHECK((int)t30_num(so, "sampleRate_default") == 16000 && (int)t30_num(so, "topK_default") == 0 && tf__end_on_silence_ok((int)t30_num(eo, "min")) &&
            tf__end_on_silence_ok((int)t30_num(eo, "max")) && !tf__end_on_silence_ok((int)t30_num(eo, "min") - 1) && !tf__end_on_silence_ok((int)t30_num(eo, "max") + 1) &&
            TF_PACE_REAL_TIME == 0 && TF_PACE_FAST == 1 && TF_ANALYTICS_ON == 0 && TF_ANALYTICS_OFF == 1,
        "option values");
  const TfJson *sp = tf_json_get(v, "speech");
  TfSpeech dflt;
  tf__speech_init(&dflt, 0);
  /* a frame is active on log_probs or timing_log_probs; no padding (the 12 frames of the first rule are gone) */
  const TfJson *act = tf_json_get(sp, "active");
  CHECK((int)t30_num(sp, "frame_ms") == 40 && (int)t30_num(sp, "timing_frame_ms") == 20 && !tf_json_get(sp, "near_frames") && act && act->n == 2 &&
            !strcmp(tf_json_str(&act->kids[0]), "log_probs") && !strcmp(tf_json_str(&act->kids[1]), "timing_log_probs") &&
            dflt.window == ((int)t30_num(sp, "default_window_ms") + 39) / 40 && tf__speech_active(0, 0, 0) == 0 && tf__speech_active(5, 0, 0) == 1 &&
            tf__speech_active(0, 7, 0) == 1 && tf__speech_active(0, 0, 7) == 1,
        "speech constants");
  const TfJson *codes = tf_json_get(v, "error_codes") ? tf_json_get(v, "error_codes") : tf_json_get(tf_json_get(v, "errors"), "codes");
  int cok = codes && codes->type == TJ_ARR;
  for (int i = 0; cok && i < codes->n; ++i) cok = tf_error_name(-(i + 1)) && !strcmp(tf_error_name(-(i + 1)), tf_json_str(&codes->kids[i]));
  CHECK(cok && codes->n == 26, "the error codes in catalogue order (%d in names.json)", codes ? codes->n : -1);
  const char *base = tf_json_str(tf_json_get(v, "error_docs_base")) ? tf_json_str(tf_json_get(v, "error_docs_base")) : tf_json_str(tf_json_get(tf_json_get(v, "errors"), "docs_base"));
  CHECK(base && !strncmp(tf_error_docs(TF_ERR_INVALID_KEY), base, strlen(base)), "the error docs base %s", base ? base : "(none)");
  tf__heads_release(&all);
  printf("names.json: 12 heads, the default selection, events and reasons, option values, %d error codes\n", codes ? codes->n : 0);
  tf_json_free(v);
}

/* topk.json: one frame of log_probs per case (38 float32 bit patterns) and its full ranking by the reference
 * (the contract's api/layers_ref.py topk). The decoded layer ranked k deep must hold the first k of each list:
 * ids and log-probs exactly, margins exactly (one that is off in its last place was not computed as a double
 * difference of the two floats). */
static void t30_topk(const char *data) {
  TfJson *v = t30_json(data, "topk.json");
  if (!v) {
    fails++;
    return;
  }
  const TfJson *cases = tf_json_get(v, "cases"), *ks = tf_json_get(v, "k_to_test");
  int ok = 0, values = 0;
  for (int i = 0; cases && i < cases->n; ++i) {
    const TfJson *c = &cases->kids[i], *bits = tf_json_get(c, "bits"), *r = tf_json_get(c, "ranked");
    const TfJson *ids = tf_json_get(r, "ids"), *lps = tf_json_get(r, "log_probs"), *ms = tf_json_get(r, "margins");
    float row[ZF_TAJWEED_VOCAB];
    int good = bits && ids && lps && ms && bits->n == ZF_TAJWEED_VOCAB && ids->n == ZF_TAJWEED_VOCAB &&
               lps->n == ZF_TAJWEED_VOCAB && ms->n == ZF_TAJWEED_VOCAB && ks && ks->n > 0;
    for (int t = 0; good && t < ZF_TAJWEED_VOCAB; ++t) {
      uint8_t b[4];
      const char *hex = tf_json_str(&bits->kids[t]);
      good = hex && strlen(hex) == 8 && tf_hex_decode(hex, b, 4) == 0;
      if (!good) break;
      const uint32_t u = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3];
      memcpy(&row[t], &u, 4);
    }
    ZfTajweedHeads h;
    memset(&h, 0, sizeof h);
    h.n40 = 1;
    h.n20 = 2;
    h.vocab = ZF_TAJWEED_VOCAB;
    h.log_probs = row;
    for (int q = 0; good && q < ks->n; ++q) {
      const int k = (int)ks->kids[q].num;
      TfDecoded *d = tf__decode(NULL, &h, NULL, k);
      good = d && d->top_k == k && d->n_frames == 1 && d->ranked_ids && d->frames[0].top1 == (int)ids->kids[0].num &&
             d->frames[0].runner_up == (int)ids->kids[1].num && d->frames[0].margin == ms->kids[0].num;
      for (int j = 0; good && j < k; ++j, values += 3)
        good = d->ranked_ids[j] == (int)ids->kids[j].num && (double)d->ranked_log_probs[j] == lps->kids[j].num &&
               d->ranked_margins[j] == ms->kids[j].num;
      CHECK(good, "topk.json %s: the ranking %d deep differs from the reference", tf_json_str(tf_json_get(c, "name")), k);
      if (d) tf__decoded_free(d);
    }
    ok += good;
  }
  CHECK(cases && cases->n >= 26 && ok == cases->n, "topk.json: %d of %d rows", ok, cases ? cases->n : 0);
  printf("  topk.json: %d of %d rows ranked as the reference at every k tested (%d values, exact)\n", ok, cases ? cases->n : 0, values);
  tf_json_free(v);
}

static int vectors30_tests(const char *data, const char *work) {
  tf_mkdirs(work);
  t30_manifest(data);
  t30_speech(data);
  t30_settled(data);
  t30_telemetry(data, work);
  t30_context(data);
  t30_names(data);
  t30_topk(data);
  printf("vectors30: %s\n", fails ? "FAIL" : "all ok");
  return fails ? 1 : 0;
}
