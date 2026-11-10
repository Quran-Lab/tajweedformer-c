/* Included by tf_tests.c after the 2.0.1 tests: the signed model manifest (SDK 2.0.2).
 *
 *   tf_tests manifest <data dir> <work dir>
 *
 * The two real signed manifests of model V4 (test/data, verbatim from the cross-SDK manifest vectors): build
 * 519235d0 (min SDK 2.0.1, with golden_tolerance) and build d4644956 (min SDK 2.0.0, without it), both signed with
 * Quran Lab's pinned key (key id 660a07197c4808da). Data-free: no model files, no network.
 *
 * Each must verify with the pinned key, through the verifier, tf_model_open and tf_download_model (a stubbed
 * transport), and be refused with one byte changed in a header value, in a tolerance value (519235d0) and in a
 * file line. The 519235d0 signed text must carry the golden_tolerance lines in make_manifest3.py signed_text()'s
 * order: after the header lines, before the first file line, sorted by the build key's UTF-8 bytes. A tolerance
 * the signature does not cover (added to d4644956) verifies through the fallback and counts as absent.
 * SDK 2.0.1 built the text without those lines, so 519235d0 failed with signature_invalid; the downloader also
 * refused every min_sdk_version above 2.0.0. */

static const char *const kMfBuilds[2] = {"manifest_v4.0_build519235d0_signed.json", "manifest_v4.0_build_d4644956_signed.json"};

/* the golden_tolerance lines of build 519235d0, as make_manifest3.py signed_text() writes them */
static const char kMfTolerance519[] =
    "golden_tolerance coreml 0 1 1\n"
    "golden_tolerance coreml-compact 0 1 1\n"
    "golden_tolerance npu:exynos2500:c16_l64 0 1 1\n"
    "golden_tolerance npu:exynos2600:c16_l64 0 1 1\n"
    "golden_tolerance npu:tensor_g3:c16_l64 0 1 1\n"
    "golden_tolerance npu:tensor_g4:c16_l64 0 1 1\n"
    "golden_tolerance npu:tensor_g5:c16_l64 0 1 1\n";

/* Changes one byte of the value of the first "key" at or after `after` (the first byte past the quote for a string,
 * the first digit for a number). Returns 0, or -1 when the key is not there. */
static int mf_mutate(char *text, const char *after, const char *key) {
  char *s = after ? strstr(text, after) : text;
  char q[96];
  snprintf(q, sizeof q, "\"%s\"", key);
  s = s ? strstr(s, q) : NULL;
  if (!s) return -1;
  s = strchr(s + strlen(q), ':');
  if (!s) return -1;
  ++s;
  while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') ++s;
  if (*s == '"') ++s;
  /* stays a digit, a hex digit or a letter, so the JSON parses and only the signature can refuse it */
  if (*s >= '0' && *s <= '9') *s = *s == '0' ? '1' : (char)(*s - 1);
  else *s = *s == 'a' ? 'b' : *s == 'v' ? 'w' : 'a';
  return 0;
}

/* verify with the pinned key; tolerance_signed gets the fallback flag */
static int mf_verify(const char *text, int *tolerance_signed) {
  TfJson *m = tf_json_parse(text, strlen(text), NULL);
  if (!m) return -1;
  const int ok = tf_manifest_verify_signature_ex(m, TF_QURANLAB_KEY_HEX, tolerance_signed);
  tf_json_free(m);
  return ok;
}

/* tf_model_open on a folder holding only this manifest.json: the error code (the signature is checked before any
 * listed file, so a manifest that verifies fails later, on its first missing file: TF_ERR_IO) */
static int mf_open(const char *work, const char *name, const char *text) {
  char *dir = tf_path_join(work, name), *mp = dir ? tf_path_join(dir, "manifest.json") : NULL;
  if (!mp || tf_mkdirs(dir) || tf_write_file(mp, text, strlen(text))) {
    free(dir);
    free(mp);
    return 1;
  }
  TfModelOptions o = tf_model_default_options();
  o.load_engine = 0;
  TfModel *m = tf_model_open(dir, &o);
  const int rc = m ? 0 : tf_last_error_code();
  if (m) tf_model_close(m);
  free(dir);
  free(mp);
  return rc;
}

/* the downloader against a stub serving this manifest and 404 for every file */
static struct {
  const char *manifest;
  int manifest_calls, file_calls;
} g_mf;
static int mf_stub(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  const size_t n = strlen(q->url);
  const char *body = "";
  if (n >= 14 && !strcmp(q->url + n - 14, "/manifest.json")) {
    g_mf.manifest_calls++;
    body = g_mf.manifest;
    r->status = 200;
  } else {
    g_mf.file_calls++;
    r->status = 404;
  }
  r->body_n = strlen(body);
  r->body = (char *)malloc(r->body_n + 1);
  memcpy(r->body, body, r->body_n + 1);
  return 0;
}
static int mf_download(const char *work, const char *name, const char *text) {
  char *dir = tf_path_join(work, name);
  if (!dir) return 1;
  g_mf.manifest = text;
  g_mf.manifest_calls = g_mf.file_calls = 0;
  tf_http_set_transport(mf_stub, NULL);
  TfDownloadOptions o = tf_download_default_options();
  o.key = "qlk_v1_test";
  o.revision = "test";
  o.dest = dir;
  const int rc = tf_download_model(&o);
  tf_http_set_transport(NULL, NULL);
  free(dir);
  return rc;
}

static int manifest_tests(const char *data, const char *work) {
  tf_mkdirs(work);
  /* the pinned key is the one the vectors are signed with */
  {
    uint8_t pk[32], h[32];
    char id[17];
    CHECK(!tf_hex_decode(TF_QURANLAB_KEY_HEX, pk, 32), "the pinned key is not hex");
    tf_sha256(pk, 32, h);
    tf_hex_encode(h, 8, id);
    CHECK(!strcmp(id, "660a07197c4808da"), "pinned key id %s, want 660a07197c4808da", id);
  }
  CHECK(!strcmp(tf_version(), "2.0.3") && !strcmp(TF_SDK_VERSION, "2.0.3"), "version %s", tf_version());
  for (int b = 0; b < 2; ++b) {
    const int has_tol = b == 0;
    char *path = tf_path_join(data, kMfBuilds[b]);
    size_t n = 0;
    char *text = path ? read_text(path, &n) : NULL;
    free(path);
    if (!text) {
      fprintf(stderr, "cannot read %s in %s\n", kMfBuilds[b], data);
      return 2;
    }
    TfJson *m = tf_json_parse(text, n, NULL);
    CHECK(m != NULL, "%s is not JSON", kMfBuilds[b]);
    if (!m) {
      free(text);
      continue;
    }
    CHECK(tf_manifest_has_tolerance(m) == has_tol, "%s: golden_tolerance %s", kMfBuilds[b], has_tol ? "missing" : "present");

    /* 1. it verifies with the pinned key: through the verifier, tf_model_open and the downloader */
    int tsigned = -1;
    CHECK(tf_manifest_verify_signature_ex(m, TF_QURANLAB_KEY_HEX, &tsigned) == 1, "%s does not verify with the pinned key",
          kMfBuilds[b]);
    CHECK(tsigned == has_tol, "%s: tolerance_signed %d, want %d", kMfBuilds[b], tsigned, has_tol);
    CHECK(tf__version_at_least(TF_SDK_VERSION, tf_json_str(tf_json_get(m, "min_sdk_version"))), "%s needs SDK %s",
          kMfBuilds[b], tf_json_str(tf_json_get(m, "min_sdk_version")));
    char name[64];
    snprintf(name, sizeof name, "open-%d", b);
    int rc = mf_open(work, name, text);
    CHECK(rc == TF_ERR_IO, "%s: tf_model_open %s (%s), want io past the signature", kMfBuilds[b], tf_error_name(rc),
          tf_last_error() ? tf_last_error() : "");
    snprintf(name, sizeof name, "download-%d", b);
    rc = mf_download(work, name, text);
    CHECK(rc == TF_ERR_HTTP && g_mf.manifest_calls == 1 && g_mf.file_calls >= 1,
          "%s: tf_download_model %s (%s), %d file requests: want http on the first file past the signature", kMfBuilds[b],
          tf_error_name(rc), tf_last_error() ? tf_last_error() : "", g_mf.file_calls);

    /* 2. the signed text: golden_tolerance lines in the reference order (519235d0), none for d4644956 */
    char *st = tf_manifest_signed_text(m);
    char *st0 = tf_manifest_signed_text_ex(m, 0);
    CHECK(st && st0, "no signed text");
    if (st && st0) {
      const char *first_file = strstr(st, "\nfile ");
      const char *ctx = strstr(st, "\ncontexts c16_l64,c24_l72,c24_l256,c8_l64\n");
      static const char hdr[] = "quranlab-model-manifest 3\nmodel_id v4.0\nfamily tajweedformer\nrevision ";
      CHECK(!strncmp(st, hdr, sizeof hdr - 1), "%s: header %.80s", kMfBuilds[b], st);
      CHECK(ctx && first_file, "%s: no contexts or file line", kMfBuilds[b]);
      if (has_tol) {
        const char *tl = strstr(st, kMfTolerance519);
        CHECK(tl && ctx && tl == ctx + strlen("\ncontexts c16_l64,c24_l72,c24_l256,c8_l64\n") &&
                  first_file == tl + strlen(kMfTolerance519) - 1,
              "%s: the golden_tolerance lines are not right after the header lines in the reference order", kMfBuilds[b]);
        CHECK(!strstr(st0, "golden_tolerance") && strlen(st) == strlen(st0) + strlen(kMfTolerance519),
              "%s: the text without the lines", kMfBuilds[b]);
        /* the 2.0.1 bug: the text without the lines does not verify */
        uint8_t sig[66], pk[32];
        tf_hex_decode(TF_QURANLAB_KEY_HEX, pk, 32);
        CHECK(tf_base64_decode(tf_json_str(tf_json_get(m, "signature")), sig, sizeof sig) == 64 &&
                  !tf_ed25519_verify(sig, (const uint8_t *)st0, strlen(st0), pk),
              "%s: verifies without its golden_tolerance lines", kMfBuilds[b]);
      } else {
        CHECK(!strstr(st, "golden_tolerance") && !strcmp(st, st0), "%s: golden_tolerance lines without the field", kMfBuilds[b]);
      }
    }
    free(st);
    free(st0);

    /* 3. one byte changed: in a header value, a tolerance value (519235d0) and a file line: refused everywhere */
    static const struct {
      const char *what, *after, *key;
      int tol;
    } kMut[] = {
        {"a header value (revision)", NULL, "revision", 0},
        {"a header value (tokens_sha256)", NULL, "tokens_sha256", 0},
        {"a tolerance value (coreml token_diffs)", "\"golden_tolerance\"", "token_diffs", 1},
        {"a tolerance value (npu:tensor_g5:c16_l64 word_flag_diffs)", "\"npu:tensor_g5:c16_l64\"", "word_flag_diffs", 1},
        {"a file line (sha256)", "\"files\"", "sha256", 0},
        {"a file line (size)", "\"files\"", "size", 0},
    };
    for (size_t k = 0; k < sizeof kMut / sizeof kMut[0]; ++k) {
      if (kMut[k].tol && !has_tol) continue;
      char *t = (char *)malloc(n + 1);
      memcpy(t, text, n + 1);
      CHECK(!mf_mutate(t, kMut[k].after, kMut[k].key), "%s: no %s to change", kMfBuilds[b], kMut[k].what);
      size_t diff = 0;
      for (size_t i = 0; i < n; ++i) diff += t[i] != text[i];
      CHECK(diff == 1, "%s: %d bytes changed", kMfBuilds[b], (int)diff);
      CHECK(mf_verify(t, NULL) == 0, "%s: verifies with one byte changed in %s", kMfBuilds[b], kMut[k].what);
      snprintf(name, sizeof name, "open-%d-mut-%d", b, (int)k);
      rc = mf_open(work, name, t);
      CHECK(rc == TF_ERR_SIGNATURE, "%s, one byte changed in %s: tf_model_open %s", kMfBuilds[b], kMut[k].what, tf_error_name(rc));
      snprintf(name, sizeof name, "download-%d-mut-%d", b, (int)k);
      rc = mf_download(work, name, t);
      CHECK(rc == TF_ERR_SIGNATURE && g_mf.file_calls == 0, "%s, one byte changed in %s: tf_download_model %s, %d file requests",
            kMfBuilds[b], kMut[k].what, tf_error_name(rc), g_mf.file_calls);
      printf("%s: one byte changed in %s: refused\n", kMfBuilds[b], kMut[k].what);
      free(t);
    }

    if (has_tol) {
      /* the signed tolerance taken out: refused (the signature covers its lines) */
      for (int i = 0; i < m->n; ++i)
        if (m->kids[i].key && !strcmp(m->kids[i].key, "golden_tolerance")) m->kids[i].key[0] = 'x';
      CHECK(!tf_manifest_has_tolerance(m) && tf_manifest_verify_signature(m, TF_QURANLAB_KEY_HEX) == 0,
            "%s verifies with golden_tolerance removed", kMfBuilds[b]);
    } else {
      /* a tolerance the signature does not cover: verifies through the fallback, and counts as absent */
      static const char add[] = "{\n \"golden_tolerance\": {\"coreml\": {\"site_flag_diffs\": 9, \"token_diffs\": 9, \"word_flag_diffs\": 9}},";
      char *t = (char *)malloc(n + sizeof add);
      CHECK(text[0] == '{', "%s does not start with {", kMfBuilds[b]);
      memcpy(t, add, sizeof add - 1);
      memcpy(t + sizeof add - 1, text + 1, n);
      int ts = -1;
      CHECK(mf_verify(t, &ts) == 1 && ts == 0, "%s with an unsigned golden_tolerance: verify %d, tolerance_signed %d", kMfBuilds[b],
            mf_verify(t, NULL), ts);
      rc = mf_open(work, "open-unsigned-tolerance", t);
      CHECK(rc == TF_ERR_IO, "%s with an unsigned golden_tolerance: tf_model_open %s", kMfBuilds[b], tf_error_name(rc));
      printf("%s: an unsigned golden_tolerance verifies through the fallback and is ignored\n", kMfBuilds[b]);
      free(t);
    }
    printf("%s: verifies with the pinned key (tolerance %s), tf_model_open and tf_download_model get past the signature\n",
           kMfBuilds[b], has_tol ? "signed" : "absent");
    tf_json_free(m);
    free(text);
  }
  printf("manifest: %s\n", fails ? "FAIL" : "all ok");
  return fails ? 1 : 0;
}
