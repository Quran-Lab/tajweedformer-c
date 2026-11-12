/* Included by tf_tests.c: the Model, the Session, listen and the reference data on the local 3.0 model folders
 * (they are not in the repository: 78 MB each; test/run_tests.sh runs this when TF_LOCAL30 names them).
 *
 *   model30 <local30 dir> <key_hex> <work dir> [<configs.json> [<variant>]]
 *
 * <key_hex> and the build ids are read from <local30 dir>/local30.json by the caller (test/local_proof.py,
 * test/run_tests.sh): nothing of the local folders is recorded in the repository. <configs.json> is made by
 * test/make_model_configs.py for the variant's build; <variant> is full (the default) or prod (the folder shaped
 * like the production build: tokens-only self-test files, no tolerances).
 *
 * <local30 dir> holds full-decoded/sdk/v4.0 and bare-decoded/sdk/v4.0 (the layout of the SDK's model store after a
 * download) and full/sdk/v4.0 (the layout of an upload: gzip files, what the CDN serves), all signed with the
 * throwaway key <key_hex>. Nothing is written into those folders: a test that changes a file copies the folder
 * first.
 *
 * What it proves: the Model's data (version, build, contexts, tokens, calibration byte for byte, sample clips);
 * a sample clip through tf_session_listen gives the tokens of the self-test file, in every context, and the same
 * output as the same audio pushed by hand; the events and end_on_silence_ms on real audio; the option refusals;
 * one open Session; the reference lookup byte for byte against the table for all 6236 ayahs, and a build or a
 * folder without it; damaged folders; and the Client's whole path against a stub CDN: prefetch with progress, a
 * cut download resumed, the model opened and warm, storage, diagnose, a context and the reference data fetched on
 * first use, starting without a network, removing the files. */
extern const char *tf__model_key_override;

typedef struct {
  int chunks, finals, last_final, speech_chunks, last_end_ms, bad;
  int events[8], n_events;
  int ended_reason, ended_error, started_ms, ended_ms, silence_ms;
  uint64_t logp;
  char last_text[2048];
} M30Run;
static void m30_chunk(void *user, const TfChunk *c) {
  M30Run *r = (M30Run *)user;
  TfHead lp;
  if (tf_heads_get(c->heads, "log_probs", &lp) == 0) r->logp = fnv1a(r->logp, lp.values, (size_t)lp.frames * lp.classes * sizeof(float));
  if (c->index != r->chunks || c->start_ms != r->last_end_ms || !c->decoded || c->decoded->n_frames * 40 != c->end_ms || strcmp(c->backend.name, "cpu")) r->bad++;
  r->last_end_ms = c->end_ms;
  r->speech_chunks += c->speech;
  r->finals += c->decoded && c->decoded->is_final;
  r->last_final = c->decoded && c->decoded->is_final;
  if (c->decoded) snprintf(r->last_text, sizeof r->last_text, "%s", c->decoded->text);
  r->chunks++;
}
static void m30_event(void *user, const TfEvent *e) {
  M30Run *r = (M30Run *)user;
  if (r->n_events < 8) r->events[r->n_events] = e->type;
  r->n_events++;
  if (!e->name || strcmp(e->name, tf__event_name(e->type))) r->bad++;
  if (e->type == TF_EVENT_SPEECH_STARTED && r->started_ms < 0) r->started_ms = e->at_ms;
  if (e->type == TF_EVENT_SPEECH_ENDED) {
    r->ended_ms = e->at_ms;
    r->silence_ms = e->silence_ms;
  }
  if (e->type == TF_EVENT_ENDED) {
    r->ended_reason = e->reason;
    r->ended_error = e->error;
    if (!e->reason_name || strcmp(e->reason_name, tf__ended_reason_name(e->reason))) r->bad++;
  }
}
static void m30_reset(M30Run *r) {
  memset(r, 0, sizeof *r);
  r->logp = FNV0;
  r->started_ms = -1;
}
/* the tokens of a clip in the folder's self-test file for a context: 1 when `tk` is exactly that list */
static int m30_expected(const char *folder, const char *context, const char *clip, const TfToken *tk, int n) {
  char rel[160];
  snprintf(rel, sizeof rel, "selftest/expected_%s.json", context);
  TfJson *e = t30_json(folder, rel);
  const TfJson *want = tf_json_get(tf_json_get(tf_json_get(e, "clips"), clip), "tokens");
  int same = want && want->type == TJ_ARR && want->n == n && n > 0;
  for (int i = 0; same && i < n; ++i) same = (int)want->kids[i].num == tk[i].id;
  tf_json_free(e);
  return same;
}
/* a clip's 16-bit samples as floats, read by the test itself */
static float *m30_clip(const char *folder, const char *name, int *n) {
  char rel[200];
  snprintf(rel, sizeof rel, "%s/selftest/%s.s16", folder, name);
  size_t bytes = 0;
  char *raw = read_text(rel, &bytes);
  if (!raw) return NULL;
  *n = (int)(bytes / 2);
  float *x = (float *)malloc(((size_t)*n + 1) * sizeof(float));
  for (int i = 0; i < *n; ++i) x[i] = (float)(int16_t)((unsigned char)raw[2 * i] | (unsigned char)raw[2 * i + 1] << 8) / 32768.0f;
  free(raw);
  return x;
}
/* copies a model folder (two levels: the files, and the files of each folder in it), leaving `skip` out */
static void m30_copy(const char *from, const char *to, const char *skip) {
  char (*names)[256] = (char (*)[256])malloc(128 * 256), (*sub)[256] = (char (*)[256])malloc(128 * 256);
  tf_mkdirs(to);
  const int n = tf__list_dir(from, names, 128);
  for (int i = 0; i < n; ++i) {
    char a[1300], b[1300];
    snprintf(a, sizeof a, "%s/%s", from, names[i]);
    snprintf(b, sizeof b, "%s/%s", to, names[i]);
    if (skip && !strcmp(names[i], skip)) continue;
    size_t bytes = 0;
    char *data = read_text(a, &bytes);
    if (data) {
      tf_write_file(b, data, bytes);
      free(data);
      continue;
    }
    tf_mkdirs(b);
    const int k = tf__list_dir(a, sub, 128);
    for (int j = 0; j < k; ++j) {
      char c[1600], d[1600];
      snprintf(c, sizeof c, "%s/%s", a, sub[j]);
      snprintf(d, sizeof d, "%s/%s", b, sub[j]);
      data = read_text(c, &bytes);
      if (data) tf_write_file(d, data, bytes);
      free(data);
    }
  }
  free(names);
  free(sub);
}
static void m30_flip(const char *folder, const char *rel) {
  char p[1300];
  snprintf(p, sizeof p, "%s/%s", folder, rel);
  size_t n = 0;
  char *data = read_text(p, &n);
  if (data && n) {
    data[n / 2] ^= 1;
    tf_write_file(p, data, n);
  }
  free(data);
}

/* ---- the stub: the API (ap_api, with the signed configs) and a CDN that serves the stored folder ---- */
static struct {
  char cdn[1024];
  int file_requests, manifest_requests, ranged, cut_at_half, bad_url;
  char build[41];
  long long last_done, last_total;
  int progress_calls, progress_bad;
  int log_lines;
  char log_last[256];
  /* SDK 3.1.0 */
  int no_headers;      /* model requests that did not carry both the key and the app token */
  char record[1400];   /* the install record's path: looked at when the first model request arrives */
  int record_first;    /* 1: the record was there, state downloading, before the first model request; 0: not; -1: not looked */
} g_m30;
static int m30_stub(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  int rc = 0;
  if (g_ap.mode == 0) return tf__fail(TF__ERR_NETWORK, "no network (test)");
  if (ap_api(q, r, &rc)) return rc;
  /* {base}/{repo}/{build}/{folder}/{path}: the build URL, never a version path */
  char want[256];
  snprintf(want, sizeof want, "https://cdn.quranlab.ai/models/Quran-Lab/tajweedformer-v4/%s/sdk/v4.0/", g_m30.build);
  if (strncmp(q->url, want, strlen(want))) g_m30.bad_url++;
  const char *rel = strstr(q->url, "/sdk/v4.0/");
  if (!rel) return tf__fail(TF__ERR_NETWORK, "unexpected URL");
  rel += 10;
  if (!strcmp(rel, "manifest.json")) g_m30.manifest_requests++;
  else g_m30.file_requests++;
  {
    /* SPEC 15.4: the key and the app token on every model request */
    int has_key = 0, has_token = 0;
    for (const char *const *h = q->headers; h && *h; ++h) {
      has_key |= !strcmp(*h, "x-qlab-key: qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa");
      has_token |= !strncmp(*h, "authorization: Bearer qlt_v1.test", 33);
    }
    g_m30.no_headers += !(has_key && has_token);
    /* SPEC 15.2: the record is written before the first byte of a build is asked for */
    if (g_m30.record[0] && g_m30.record_first < 0) {
      size_t rn = 0;
      char *rec = read_text(g_m30.record, &rn);
      g_m30.record_first = rec && rn > 12 && !strncmp(rec, "downloading ", 12);
      free(rec);
    }
  }
  char *p = tf_path_join(g_m30.cdn, rel), *data = NULL;
  size_t n = 0;
  const int have = p && !tf_read_file(p, &data, &n);
  free(p);
  if (!have) {
    r->status = 404;
    return 0;
  }
  if (!q->download_file) {
    r->status = 200;
    r->body = data;
    r->body_n = n;
    return 0;
  }
  const long long from = q->range_from > 0 ? q->range_from : 0;
  g_m30.ranged += from > 0;
  FILE *f = fopen(q->download_file, from > 0 ? "ab" : "wb");
  size_t len = n - (size_t)from;
  int cut = 0;
  if (g_m30.cut_at_half && strstr(rel, "layers.zfw.gz") && from == 0) {
    len /= 2; /* the connection drops half way through the weights */
    cut = 1;
    g_m30.cut_at_half = 0;
  }
  /* in four pieces, as bytes arrive */
  for (int k = 0; k < 4; ++k) {
    const size_t a = len * (size_t)k / 4, b = len * (size_t)(k + 1) / 4;
    fwrite(data + from + a, 1, b - a, f);
    if (q->on_bytes) q->on_bytes(q->bytes_user, (long long)(b - a));
  }
  fclose(f);
  free(data);
  if (cut) return tf__fail(TF__ERR_NETWORK, "connection reset (simulated)");
  r->status = from > 0 ? 206 : 200;
  return 0;
}
static void m30_progress(void *user, long long done, long long total) {
  (void)user;
  if (done < g_m30.last_done || done > total || (g_m30.last_total && total != g_m30.last_total)) g_m30.progress_bad++;
  g_m30.last_done = done;
  g_m30.last_total = total;
  g_m30.progress_calls++;
}
static void m30_log(void *user, int level, const char *message) {
  (void)user;
  (void)level;
  g_m30.log_lines++;
  snprintf(g_m30.log_last, sizeof g_m30.log_last, "%s", message);
}
/* SDK 3.1.0: fills storage `b` in advance with the build that storage `a` downloaded, exactly as a real download
 * leaves it (the build folder and current.json). how 0: no install record; 1: a's record, byte for byte, without
 * a's key file (as on another machine: b makes its own key); 2: a record that is not this build's, under this
 * build's name, with b's own key present. */
static void m30_fill(const char *a, const char *b, const char *build, int how) {
  char from[1400], to[1400];
  size_t n = 0;
  snprintf(from, sizeof from, "%s/models/%s", a, build);
  snprintf(to, sizeof to, "%s/models/%s", b, build);
  m30_copy(from, to, NULL);
  snprintf(from, sizeof from, "%s/models/current.json", a);
  snprintf(to, sizeof to, "%s/models/current.json", b);
  char *t = read_text(from, &n);
  if (t) tf_write_file(to, t, n);
  free(t);
  snprintf(to, sizeof to, "%s/models/%s.install", b, build);
  if (how == 1) {
    snprintf(from, sizeof from, "%s/models/%s.install", a, build);
    t = read_text(from, &n);
    if (t) tf_write_file(to, t, n);
    free(t);
  } else if (how == 2) {
    uint8_t dk[32];
    tf__install_key(b, 1, dk);
    tf_write_file(to, "installed 0000000000000000000000000000000000000000000000000000000000000000\n", 75);
  }
}
static TfClient *m30_client(const char *dir, int debug) {
  tf_mkdirs(dir);
  TfClient *c = tf__client_create_with_key(&(TfClientOptions){.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa", .app_id = "org.example.Api", .app_version = "1.0", .storage = dir, .debug = debug, .on_log = m30_log}, tf_json_str(tf_json_get(g_ap.fx, "public_key_hex")));
  if (!c) fprintf(stderr, "client: %s\n", tf_last_error());
  return c;
}

static int model30_tests(const char *local, const char *key, const char *work, const char *configs, const char *variant) {
  char full[1100], bare[1100], d[1300];
  if (!variant) variant = "full";
  snprintf(full, sizeof full, "%s/%s-decoded/sdk/v4.0", local, variant);
  snprintf(bare, sizeof bare, "%s/bare-decoded/sdk/v4.0", local);
  tf_mkdirs(work);
  
  static const char *const kContexts[] = {"c16_l64", "c8_l64", "c24_l72", "c24_l256"};
  static const int kChunkMs[] = {320, 160, 480, 480};

  /* ---- 1. the Model's data ---- */
  CHECK(tf__model_open_folder(full, NULL, NULL) == NULL && tf_last_error_code() == TF_ERR_SIGNATURE_INVALID, "a folder signed with a test key opened without that key");
  TfModel *m = tf__model_open_folder(full, NULL, key);
  if (!m) {
    fprintf(stderr, "open %s: %s\n", full, tf_last_error());
    return 2;
  }
  {
    char mp[1300], hex[65];
    snprintf(mp, sizeof mp, "%s/manifest.json", full);
    size_t n = 0;
    char *text = read_text(mp, &n);
    uint8_t h[32];
    tf_sha256(text, n, h);
    tf_hex_encode(h, 32, hex);
    free(text);
    CHECK(!strcmp(tf_model_id(m), "v4.0") && !strcmp(tf_model_version(m), "4.0.0") && strlen(tf_model_build(m)) == 40 && !strncmp(tf_model_build(m), hex, 40),
          "id %s, version %s, build %s", tf_model_id(m), tf_model_version(m), tf_model_build(m));
    TfJson *info = tf_json_parse(tf_model_info_json(m), strlen(tf_model_info_json(m)), NULL);
    const TfJson *cx = tf_json_get(info, "contexts");
    CHECK(info && cx && cx->n == 4 && !strcmp(ap_str(info, "default_context", NULL), "c16_l64") && !strcmp(ap_str(info, "version", NULL), "4.0.0") &&
              !strcmp(ap_str(info, "build", NULL), tf_model_build(m)) && tf_json_get(info, "heads")->n == 12 && tf_json_get(info, "default_heads")->n == 6 &&
              tf_json_get(info, "sample_clips")->n == 5 && ap_true(info, "reference", NULL) && !strcmp(ap_str(info, "calibration", "format"), "quranlab.tajweed-calibration/1"),
          "the info JSON: %s", tf_model_info_json(m));
    tf_json_free(info);
    for (int i = 0; i < 4; ++i) CHECK(tf_model_chunk_ms(m, kContexts[i]) == kChunkMs[i], "chunk_ms(%s) %d", kContexts[i], tf_model_chunk_ms(m, kContexts[i]));
    CHECK(tf_model_chunk_ms(m, NULL) == 320 && tf_model_chunk_ms(m, "c99_l1") == 0, "chunk_ms of the default and of an unknown context");
    const TfTokens *tk = tf_model_tokens(m);
    CHECK(tk && tk->count == 38 && tk->blank == 0 && tk->word_bar == 1 && !strcmp(tk->names[1], "|") && tf_model_token_count(m) == 38 &&
              !strcmp(tf_model_token_name(m, 1), "|") && tf_model_token_name(m, 38) == NULL && tf_model_token_name(m, -1) == NULL &&
              tf_model_token_id(m, tk->names[17]) == 17 && tf_model_token_id(m, "nope") == -1,
          "the vocabulary");
    TfBackend be;
    CHECK(tf_model_backend(m, &be) == 0 && !strcmp(be.kind, "cpu") && !strcmp(be.name, "cpu") && !strcmp(be.stage, "cpu") && be.reason == NULL, "the Model's backend");
    /* the calibration: the shipped file byte for byte, read only */
    TfCalibration cal;
    snprintf(mp, sizeof mp, "%s/calibration.json", full);
    text = read_text(mp, &n);
    const int have = tf_model_calibration(m, &cal);
    tf_sha256(cal.json, cal.size, h);
    tf_hex_encode(h, 32, hex);
    CHECK(have == 1 && text && cal.size == n && !memcmp(cal.json, text, n) && tf_model_calibration_json(m) == cal.json &&
              !strcmp(hex, "747b7b4cff040efbd46b2ef709f9344c35771b109a4ca1bee1e52b65a5bd991c") && !strcmp(cal.sha256, hex) &&
              !strcmp(cal.format, "quranlab.tajweed-calibration/1"),
          "the calibration is not the shipped file (sha256 %s)", hex);
    TfJson *cj = text ? tf_json_parse(text, n, NULL) : NULL;
    const TfJson *th = tf_json_get(tf_json_get(tf_json_get(tf_json_get(tf_json_get(tf_json_get(cj, "contexts"), "c16_l64"), "sites"), "vowel_sub"), "thresholds"), "balanced");
    double got = 0;
    CHECK(th && th->type == TJ_NUM && tf_model_calibration_number(m, "contexts.c16_l64.sites.vowel_sub.thresholds.balanced", &got) == 1 && got == th->num &&
              tf_model_calibration_number(m, "contexts.c16_l64.sites.nope", &got) == 0 && tf_model_calibration_number(m, "format", &got) == 0,
          "a number of the calibration by its path");
    tf_json_free(cj);
    free(text);
    /* the sample clips */
    TfClip clip;
    int clean = 0, planted = 0;
    CHECK(tf_model_sample_clip_count(m) == 5 && tf_model_sample_clip(m, 5, &clip) == TF_ERR_INVALID_ARGUMENT, "five sample clips");
    for (int i = 0; i < tf_model_sample_clip_count(m); ++i) {
      tf_model_sample_clip(m, i, &clip);
      clean += !strcmp(clip.kind, "clean");
      planted += !strcmp(clip.kind, "planted");
      CHECK(clip.name && clip.duration_ms > 2000 && clip.duration_ms < 8000, "clip %d: %s, %d ms", i, clip.name ? clip.name : "", clip.duration_ms);
    }
    CHECK(clean == 4 && planted == 1, "clip kinds: %d clean, %d planted", clean, planted);
    printf("model: v4.0 4.0.0 build %.8s, 4 contexts, 38 tokens, calibration byte for byte, 5 sample clips\n", tf_model_build(m));
  }

  /* ---- 2. a sample clip through listen gives the tokens of the self-test file, in every context ---- */
  {
    int ok = 0, runs = 0;
    for (int ci = 0; ci < 4; ++ci)
      for (int i = 0; i < tf_model_sample_clip_count(m); ++i, ++runs) {
        TfClip clip;
        tf_model_sample_clip(m, i, &clip);
        M30Run r;
        m30_reset(&r);
        TfSession *s = tf_session_listen(m, &clip, &(TfSessionOptions){.context = kContexts[ci], .pace = TF_PACE_FAST, .on_chunk = m30_chunk, .on_event = m30_event, .user = &r});
        const TfHeads *h = s ? tf_session_heads(s) : NULL;
        const TfToken *tk = NULL;
        const int n = h ? tf_heads_greedy(h, NULL, &tk) : -1;
        const TfDecoded *dec = h ? tf_heads_decoded(h) : NULL;
        const int good = s && h && dec && m30_expected(full, kContexts[ci], clip.name, tk, n) && dec->n_tokens == n && dec->is_final && dec->settled == n &&
                         !strcmp(dec->text, r.last_text) && r.finals == 1 && r.last_final && r.bad == 0 && r.speech_chunks > 0 &&
                         r.last_end_ms == dec->n_frames * 40 && r.n_events >= 2 && r.events[0] == TF_EVENT_SPEECH_STARTED && r.ended_reason == TF_ENDED_SOURCE_END &&
                         (r.n_events > 8 || r.events[r.n_events - 1] == TF_EVENT_ENDED) && tf__session_kept_peak(s) >= 1 && tf__session_kept_peak(s) * kChunkMs[ci] <= 2000 && tf_session_finish(s) == 0 &&
                         tf_session_push(s, (const float *)full, 0) == TF_ERR_SESSION_CLOSED;
        CHECK(good, "listen %s in %s: %d tokens, %d chunks, %d final, %d events, ended %d, %d chunks held back: %s", clip.name, kContexts[ci], n, r.chunks, r.finals, r.n_events,
              r.ended_reason, tf__session_kept_peak(s), tf_last_error() ? tf_last_error() : "");
        ok += good;
        tf__clear_error();
        tf_session_destroy(s);
      }
    printf("listen: %d/%d takes (5 sample clips in 4 contexts) give the self-test tokens; the last chunk is final; ended(sourceEnd)\n", ok, runs);
  }

  /* ---- 3. the same path as pushed audio; real-time pace; end_on_silence_ms; the speech events ---- */
  {
    int n = 0;
    float *x = m30_clip(full, "Husary_001001", &n);
    TfClip clip = {.name = "Husary_001001"};
    M30Run a, b;
    m30_reset(&a);
    m30_reset(&b);
    TfSession *s = tf_session_listen(m, &clip, &(TfSessionOptions){.pace = TF_PACE_FAST, .on_chunk = m30_chunk, .user = &a});
    const TfDecoded *da = s ? tf_heads_decoded(tf_session_heads(s)) : NULL;
    char text_a[2048];
    snprintf(text_a, sizeof text_a, "%s", da ? da->text : "");
    const int tokens_a = da ? da->n_tokens : -1;
    tf_session_destroy(s);
    s = tf_session_create(m, &(TfSessionOptions){.on_chunk = m30_chunk, .user = &b});
    for (int at = 0, step = 777; s && x && at < n; at += step) tf_session_push(s, x + at, n - at < step ? n - at : step);
    tf_session_finish(s);
    const TfDecoded *db = s ? tf_heads_decoded(tf_session_heads(s)) : NULL;
    CHECK(x && da != NULL && db && a.chunks == b.chunks && a.logp == b.logp && tokens_a == db->n_tokens && !strcmp(text_a, db->text),
          "listen and push of the same clip differ (%d and %d chunks)", a.chunks, b.chunks);
    tf_session_destroy(s);
    /* the clip's own pace: it takes about as long as the clip is */
    TfClip shortest = {.name = "Saood_001001"};
    const double t0 = tf__now_s();
    s = tf_session_listen(m, &shortest, NULL);
    const double took = tf__now_s() - t0;
    tf_model_sample_clip(m, 0, &clip);
    CHECK(s && took > 2.4 && took < 6.0, "real-time pace: %.2f s for a 2.7 s clip", took);
    tf_session_destroy(s);
    /* the clip, three seconds of silence, the clip again: without the option two speech spans in one take; with
     * end_on_silence_ms the take ends in the silence, with the first clip's tokens */
    const int gap = 3 * 16000, total = 2 * n + gap;
    float *twice = (float *)calloc((size_t)total + 1, sizeof(float));
    memcpy(twice, x, (size_t)n * sizeof(float));
    memcpy(twice + n + gap, x, (size_t)n * sizeof(float));
    M30Run r;
    m30_reset(&r);
    s = tf_session_create(m, &(TfSessionOptions){.on_chunk = m30_chunk, .on_event = m30_event, .user = &r});
    for (int at = 0; s && at < total; at += 1600) tf_session_push(s, twice + at, total - at < 1600 ? total - at : 1600);
    tf_session_finish(s);
    const TfToken *tk = NULL;
    int nt = s ? tf_heads_greedy(tf_session_heads(s), NULL, &tk) : -1;
    CHECK(nt == 2 * tokens_a && r.n_events == 3 && r.events[0] == TF_EVENT_SPEECH_STARTED && r.events[1] == TF_EVENT_SPEECH_ENDED &&
              r.events[2] == TF_EVENT_SPEECH_STARTED && r.silence_ms == 1520 && r.ended_ms > r.started_ms && r.finals == 1,
          "two speech spans: %d tokens, %d events, silence %d ms", nt, r.n_events, r.silence_ms);
    tf_session_destroy(s);
    m30_reset(&r);
    s = tf_session_create(m, &(TfSessionOptions){.end_on_silence_ms = 600, .on_chunk = m30_chunk, .on_event = m30_event, .user = &r});
    int pushed = 0, closed_at = -1;
    for (int at = 0; s && at < total; at += 1600) {
      const int rc = tf_session_push(s, twice + at, total - at < 1600 ? total - at : 1600);
      if (rc == TF_ERR_SESSION_CLOSED && closed_at < 0) closed_at = at;
      pushed += rc >= 0;
    }
    nt = s ? tf_heads_greedy(tf_session_heads(s), NULL, &tk) : -1;
    const TfDecoded *ds = s ? tf_heads_decoded(tf_session_heads(s)) : NULL;
    CHECK(s && nt == tokens_a && m30_expected(full, "c16_l64", "Husary_001001", tk, nt) && r.n_events == 3 && r.events[0] == TF_EVENT_SPEECH_STARTED &&
              r.events[1] == TF_EVENT_SPEECH_ENDED && r.events[2] == TF_EVENT_ENDED && r.ended_reason == TF_ENDED_SILENCE && r.silence_ms == 600 &&
              r.finals == 1 && r.last_final && closed_at > n && closed_at < n + gap && pushed == closed_at / 1600 && ds && ds->is_final && ds->n_frames * 40 == r.last_end_ms &&
              tf_session_finish(s) == 0,
          "end_on_silence_ms 600: %d tokens, %d events, reason %d, silence %d ms, closed at sample %d of %d", nt, r.n_events, r.ended_reason, r.silence_ms, closed_at, total);
    tf__clear_error();
    tf_session_destroy(s);
    /* the decoded layer to depth 5; a head selection; "all" */
    s = tf_session_create(m, &(TfSessionOptions){.top_k = 5, .heads = "gem,attr_face"});
    for (int at = 0; s && at < n; at += 1600) tf_session_push(s, x + at, n - at < 1600 ? n - at : 1600);
    tf_session_finish(s);
    const TfHeads *h = s ? tf_session_heads(s) : NULL;
    const TfDecoded *dk = h ? tf_heads_decoded(h) : NULL;
    int rank_ok = dk && dk->top_k == 5 && dk->ranked_ids && dk->ranked_log_probs && dk->ranked_margins;
    for (int t = 0; rank_ok && t < dk->n_frames; ++t)
      rank_ok = dk->ranked_ids[5 * t] == dk->frames[t].top1 && dk->ranked_ids[5 * t + 1] == dk->frames[t].runner_up && dk->ranked_margins[5 * t] == dk->frames[t].margin;
    TfHead hd;
    CHECK(rank_ok && tf_heads_count(h) == 3 && tf_heads_get(h, "gem", &hd) == 0 && hd.classes == 2 && hd.frame_ms == 20 && hd.frames == 2 * dk->n_frames &&
              hd.class_names && !strcmp(hd.class_names[1], "held") && tf_heads_get(h, "conf", &hd) == TF_ERR_INVALID_ARGUMENT &&
              tf_heads_greedy(h, "gem", NULL) == TF_ERR_INVALID_ARGUMENT,
          "top_k 5 with the heads gem and attr_face (%d heads)", h ? tf_heads_count(h) : -1);
    tf__clear_error();
    /* confidence comes from conf: NAN without it */
    CHECK(dk && dk->n_tokens > 0 && dk->tokens[0].confidence != dk->tokens[0].confidence && dk->tokens[0].name && dk->tokens[0].peak_ms >= dk->tokens[0].first_ms &&
              dk->tokens[0].peak_ms <= dk->tokens[0].last_ms,
          "a token without the conf head");
    tf_session_destroy(s);
    s = tf_session_create(m, &(TfSessionOptions){.heads = "all"});
    for (int at = 0; s && at < n; at += 1600) tf_session_push(s, x + at, n - at < 1600 ? n - at : 1600);
    tf_session_finish(s);
    h = s ? tf_session_heads(s) : NULL;
    const TfToken *t20 = NULL;
    CHECK(h && tf_heads_count(h) == 12 && tf_heads_greedy(h, "timing_log_probs", &t20) > 0 && t20[0].last_ms - t20[0].first_ms == 20 * (t20[0].last_frame - t20[0].first_frame) &&
              tf_heads_decoded(h)->tokens[0].confidence == tf_heads_decoded(h)->tokens[0].confidence,
          "all twelve heads, greedy tokens of a 20 ms head, confidence with conf");
    tf_session_destroy(s);
    free(twice);
    free(x);
    printf("session: listen == push (same chunks, same bits of log_probs); real-time pace %.1f s; two speech spans (silence 1520 ms); end_on_silence_ms 600 ends the "
           "take in the silence with the clip's tokens; top_k, head selections\n",
           took);
  }

  /* ---- 4. refusals, one open Session, cancel ---- */
  {
    static const TfSessionOptions bad[] = {{.context = "c99_l1"}, {.sample_rate = 5}, {.heads = "logits"}, {.backend = "npu"}, {.top_k = -1}, {.top_k = 39},
                                           {.end_on_silence_ms = 50}, {.end_on_silence_ms = 60001}, {.end_on_silence_ms = -1}, {.pace = 7}};
    int refused = 0;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
      TfSession *s = tf_session_create(m, &bad[i]);
      refused += !s && tf_last_error_code() == TF_ERR_INVALID_ARGUMENT;
      tf_session_destroy(s);
    }
    CHECK(refused == (int)(sizeof bad / sizeof bad[0]), "%d of %d invalid session options refused", refused, (int)(sizeof bad / sizeof bad[0]));
    TfSession *s = tf_session_create(m, &(TfSessionOptions){.backend = "auto", .sample_rate = 48000, .context = "c8_l64"});
    CHECK(s != NULL, "backend auto at 48 kHz in c8_l64: %s", tf_last_error() ? tf_last_error() : "");
    CHECK(tf_session_create(m, NULL) == NULL && tf_last_error_code() == TF_ERR_SESSION_OPEN && !strcmp(tf_last_error(), "This model already has an open session."),
          "a second open session");
    /* another Model of the process cannot start a take while this one runs (the engine runs one at a time) */
    TfModel *m2 = tf__model_open(bare, &(TfModelOpen){.trusted_key_hex = key, .no_engine = 1});
    CHECK(m2 && tf_session_create(m2, NULL) == NULL && tf_last_error_code() == TF_ERR_SESSION_OPEN, "a take on a second model while the first runs: %s",
          tf_error_name(tf_last_error_code()));
    static const float quiet[1600];
    CHECK(tf_session_push(s, quiet, 1600) >= 0 && tf_session_report_recited(s, 1, 1, 7) == 0 && tf_session_report_recited(s, 1, 1, 8) == TF_ERR_INVALID_ARGUMENT &&
              tf_session_report_recited(s, 115, 1, 1) == TF_ERR_INVALID_ARGUMENT,
          "reportRecited on an open take");
    TfBackend be;
    CHECK(tf_session_backend(s, &be) == 0 && !strcmp(be.name, "cpu"), "the Session's backend");
    CHECK(tf_session_cancel(s) == 0 && tf_session_heads(s) == NULL && tf_last_error_code() == TF_ERR_CANCELLED && tf_session_push(s, quiet, 1600) == TF_ERR_CANCELLED &&
              tf_session_finish(s) == TF_ERR_CANCELLED && tf_session_report_recited(s, 1, 1, 7) == 0 && tf_session_cancel(s) == 0,
          "a cancelled take: %s", tf_error_name(tf_last_error_code()));
    tf_session_destroy(s);
    /* now the second model's take starts (the first gave its engine stream up), and the first works again after */
    TfSession *s2 = m2 ? tf_session_create(m2, NULL) : NULL;
    CHECK(s2 != NULL, "a take on the second model once the first is idle: %s", tf_last_error() ? tf_last_error() : "");
    tf_session_destroy(s2);
    s = tf_session_create(m, NULL);
    CHECK(s != NULL, "the first model again: %s", tf_last_error() ? tf_last_error() : "");
    tf_session_destroy(s);
    tf_model_close(m2);
    tf__clear_error();
    printf("refusals: %d invalid options, a second open session (session_open), cancel (cancelled), two models taking turns\n", refused);
  }

  /* ---- 4b. a long take keeps no audio: five minutes at 48 kHz pushed 100 ms at a time from one small buffer.
   * What the take holds grows with its length by the heads only (the take's Heads are its output); the audio is
   * gone once pushed. Four minutes of 48 kHz float audio are 46 MB: the growth from minute 1 to minute 5 must
   * stay far under that. The run is five minutes of audio and no more, and the process caps its own memory. ---- */
  {
    static float buf[4800];
    uint32_t seed = 12345;
    for (int i = 0; i < 4800; ++i) {
      seed = seed * 1664525u + 1013904223u;
      buf[i] = ((float)(seed >> 8) / 8388608.0f - 1.0f) * 0.01f;
    }
    M30Run r;
    m30_reset(&r);
    TfSession *s = tf_session_create(m, &(TfSessionOptions){.sample_rate = 48000, .on_chunk = m30_chunk, .user = &r});
    long long at1 = 0, at5 = 0;
    int pushes = 0;
    for (int i = 0; s && i < 3000; ++i) { /* 3000 x 100 ms */
      if (tf_session_push(s, buf, 4800) < 0) break;
      pushes++;
      if (i == 599) at1 = own_memory();
    }
    at5 = own_memory();
    const int rc = s ? tf_session_finish(s) : -1;
    const TfDecoded *dec = s && rc >= 0 ? tf_heads_decoded(tf_session_heads(s)) : NULL;
    const long long grew = at5 - at1;
    CHECK(pushes == 3000 && dec && dec->n_frames >= 7500 && dec->n_frames <= 7550 && dec->n_frames * 40 == r.last_end_ms && r.bad == 0 &&
              tf__session_kept_peak(s) >= 1 && tf__session_kept_peak(s) * 320 <= 2000,
          "a five-minute take: %d pushes, %d frames, %d chunks held back at most", pushes, dec ? dec->n_frames : -1, tf__session_kept_peak(s));
    CHECK(at1 == 0 || grew < (20ll << 20), "a five-minute take at 48 kHz grew the process by %lld bytes from minute 1 to minute 5 (the audio alone would be 46 MB)", grew);
    const int peak = tf__session_kept_peak(s);
    tf_session_destroy(s);
    printf("bounds: a five-minute take at 48 kHz in 3000 pushes: %d frames, %d chunks, memory %lld MB at minute 1 and %lld MB at minute 5 (grew %lld KB; the pushed "
           "audio is 46 MB); a finish holds back %d chunks at most\n",
           r.last_end_ms / 40, r.chunks, at1 >> 20, at5 >> 20, grew >> 10, peak);
  }

  /* ---- 4c. a take holds at most TF_TAKE_MAX_S (20 minutes) of audio: then it ends by itself, ended(limit),
   * the last chunk final, the Heads kept, a later push session_closed. Run with the limit set to 2 s: fifty
   * pushes of 100 ms, of which twenty are taken; and one push of 5 s, of which 2 s are taken. ---- */
  {
    static float quiet[80000];
    CHECK(TF_TAKE_MAX_S == 1200 && tf__take_max_s == TF_TAKE_MAX_S, "the take limit is %d s", tf__take_max_s);
    tf__take_max_s = 2;
    int frames[2] = {0, 0};
    for (int big = 0; big < 2; ++big) {
      M30Run r;
      m30_reset(&r);
      TfSession *s = tf_session_create(m, &(TfSessionOptions){.on_chunk = m30_chunk, .on_event = m30_event, .user = &r});
      int taken = 0, refused = 0;
      if (big) {
        taken = s && tf_session_push(s, quiet, 80000) >= 0;
        refused = s && tf_session_push(s, quiet, 1600) == TF_ERR_SESSION_CLOSED;
      } else {
        for (int i = 0; s && i < 50; ++i) {
          const int rc = tf_session_push(s, quiet, 1600);
          taken += rc >= 0;
          refused += rc == TF_ERR_SESSION_CLOSED;
        }
      }
      tf__clear_error();
      const TfHeads *h = s ? tf_session_heads(s) : NULL;
      const TfDecoded *dec = h ? tf_heads_decoded(h) : NULL;
      frames[big] = dec ? dec->n_frames : -1;
      CHECK(taken == (big ? 1 : 20) && refused == (big ? 1 : 30) && dec && dec->is_final && dec->n_frames >= 50 && dec->n_frames <= 80 && r.finals == 1 && r.last_final &&
                r.n_events == 1 && r.events[0] == TF_EVENT_ENDED && r.ended_reason == TF_ENDED_LIMIT && !strcmp(tf__ended_reason_name(TF_ENDED_LIMIT), "limit") &&
                tf_session_finish(s) == 0,
            "a take at its limit (%s): %d pushes taken, %d refused, %d frames, %d events, reason %d", big ? "one push of 5 s" : "100 ms pushes", taken, refused,
            dec ? dec->n_frames : -1, r.n_events, r.ended_reason);
      tf_session_destroy(s);
    }
    CHECK(frames[0] == frames[1], "the take at its limit holds %d frames pushed in pieces and %d pushed at once", frames[0], frames[1]);
    tf__take_max_s = TF_TAKE_MAX_S;
    printf("limit: a take ends by itself at its limit (20 minutes; 2 s here): ended(limit), %d frames either way, the last chunk final, later pushes session_closed\n", frames[0]);
  }

  /* ---- 5. the reference data: the lookup byte for byte against the table ---- */
  {
    TfReference *ref = tf_model_reference(m);
    char tp[1300];
    snprintf(tp, sizeof tp, "%s/expected_tj3.zfx", full);
    const char *err = NULL;
    ZfTajweedReference *table = zf_tajweed_reference_open(tp, &err);
    CHECK(ref && tf_reference_is_downloaded(ref) && table, "the reference data of the full folder: %s", err ? err : "");
    int ayahs = 0, same = 0, units = 0;
    for (int su = 1; ref && table && su <= 114; ++su) {
      const int count = zf_tajweed_reference_ayah_count(table, su);
      CHECK(count == tf__ayah_count(su), "surah %d: %d ayahs in the table, %d in the SDK", su, count, tf__ayah_count(su));
      for (int ay = 1; ay <= count; ++ay, ++ayahs) {
        const int *ids = NULL, *word = NULL;
        int n_words = 0;
        const int n = zf_tajweed_reference_units(table, su, ay, &ids, &word, &n_words);
        TfUnits u;
        const int rc = tf_reference_units(ref, su, ay, &u);
        const int good = rc == 0 && u.n == n && u.n_words == n_words && n > 0 && !memcmp(u.ids, ids, (size_t)n * sizeof(int)) && !memcmp(u.word, word, (size_t)n * sizeof(int));
        same += good;
        units += n;
      }
    }
    TfUnits u;
    CHECK(ayahs == 6236 && same == ayahs, "the reference lookup: %d of %d ayahs equal the table", same, ayahs);
    CHECK(ref && tf_reference_units(ref, 0, 1, &u) == TF_ERR_INVALID_ARGUMENT && tf_reference_units(ref, 115, 1, &u) == TF_ERR_INVALID_ARGUMENT &&
              tf_reference_units(ref, 1, 8, &u) == TF_ERR_INVALID_ARGUMENT && tf_reference_units(ref, 1, 0, &u) == TF_ERR_INVALID_ARGUMENT &&
              tf_reference_units(ref, 114, 6, &u) == 0 && u.n > 0 && u.word[u.n - 1] == u.n_words - 1 && tf_reference_fetch(ref) == 0,
          "the reference lookup's refusals");
    /* its ids are the model's tokens: the table says which vocabulary it was built for */
    {
      char kp[1300], hex[65];
      snprintf(kp, sizeof kp, "%s/tokens.txt", full);
      size_t n = 0;
      char *text = read_text(kp, &n);
      uint8_t h[32];
      tf_sha256(text, n, h);
      tf_hex_encode(h, 32, hex);
      free(text);
      CHECK(table && !memcmp(zf_tajweed_reference_tokens_sha256(table), h, 32), "the reference table is for another vocabulary than tokens.txt (%s)", hex);
    }
    if (table) zf_tajweed_reference_close(table);
    tf__clear_error();
    printf("reference: %d/%d ayahs (%d units) byte for byte the table's; refusals\n", same, ayahs, units);
  }
  tf_model_close(m);

  /* ---- 6. a build without the data, a folder where it was not fetched, damaged folders ---- */
  {
    TfModel *b = tf__model_open_folder(bare, NULL, key);
    TfCalibration cal;
    CHECK(b && tf_model_calibration_json(b) == NULL && tf_model_calibration(b, &cal) == 0 && cal.json == NULL && tf_model_reference(b) == NULL &&
              tf_last_error_code() == 0 && !strcmp(tf_model_version(b), "4.0.0") && strcmp(tf_model_build(b), g_m30.build),
          "the folder without calibration.json and reference data: %s", tf_last_error() ? tf_last_error() : "");
    TfClip clip = {.name = "Abdul_001001"};
    TfSession *s = b ? tf_session_listen(b, &clip, &(TfSessionOptions){.pace = TF_PACE_FAST}) : NULL;
    const TfToken *tk = NULL;
    const int n = s ? tf_heads_greedy(tf_session_heads(s), NULL, &tk) : -1;
    CHECK(s && m30_expected(bare, "c16_l64", "Abdul_001001", tk, n), "the folder without calibration.json and reference data decodes (%d tokens)", n);
    tf_session_destroy(s);
    tf_model_close(b);
    /* listed, not on disk: the reference is there to fetch, the lookup says it is missing; a folder opened by hand
     * has nothing to fetch from */
    snprintf(d, sizeof d, "%s/noref", work);
    m30_copy(full, d, "expected_tj3.zfx");
    b = tf__model_open_folder(d, NULL, key);
    TfReference *ref = b ? tf_model_reference(b) : NULL;
    TfUnits u;
    CHECK(b && ref && !tf_reference_is_downloaded(ref) && tf_reference_units(ref, 1, 1, &u) == TF_ERR_REFERENCE_MISSING &&
              !strcmp(tf_last_error(), "The reference data is not on this device.") && tf_reference_fetch(ref) == TF_ERR_REFERENCE_MISSING,
          "reference data that was not fetched: %s", tf_error_name(tf_last_error_code()));
    s = b ? tf_session_listen(b, &clip, &(TfSessionOptions){.pace = TF_PACE_FAST}) : NULL;
    CHECK(s != NULL, "a folder without the reference file decodes: %s", tf_last_error() ? tf_last_error() : "");
    tf_session_destroy(s);
    tf_model_close(b);
    /* damage: one bit of an engine file, of the tokens, of the manifest; a file gone; the context's files gone */
    static const struct {
      const char *rel;
      int code;
    } damage[] = {{"c16_l64/frames.zfw", TF_ERR_CHECKSUM_MISMATCH}, {"tokens.txt", TF_ERR_CHECKSUM_MISMATCH}, {"calibration.json", TF_ERR_CHECKSUM_MISMATCH},
                  {"manifest.json", 0 /* signature_invalid or model_invalid, below */}};
    int dok = 0;
    for (size_t i = 0; i < sizeof damage / sizeof damage[0]; ++i) {
      snprintf(d, sizeof d, "%s/damage%d", work, (int)i);
      m30_copy(full, d, "expected_tj3.zfx");
      m30_flip(d, damage[i].rel);
      b = tf__model_open_folder(d, NULL, key);
      const int code = tf_last_error_code();
      const int good = !b && (damage[i].code ? code == damage[i].code : code == TF_ERR_SIGNATURE_INVALID || code == TF_ERR_MODEL_INVALID) &&
                       (code != TF_ERR_CHECKSUM_MISMATCH || strstr(tf_last_error(), damage[i].rel));
      CHECK(good, "one bit changed in %s: %s (%s)", damage[i].rel, b ? "opened" : tf_error_name(code), tf_last_error() ? tf_last_error() : "");
      dok += good;
      tf_model_close(b);
    }
    snprintf(d, sizeof d, "%s/damage0/layers.zfw", work);
    remove(d);
    snprintf(d, sizeof d, "%s/damage0", work);
    m30_flip(d, "c16_l64/frames.zfw"); /* back as it was */
    b = tf__model_open_folder(d, NULL, key);
    CHECK(!b && tf_last_error_code() == TF_ERR_MODEL_MISSING && strstr(tf_last_error(), "layers.zfw"), "a listed file that is gone: %s", tf_error_name(tf_last_error_code()));
    tf_model_close(b);
    tf__clear_error();
    printf("folders: without calibration and reference data it opens and decodes; reference not fetched is reference_missing; %d/4 damaged folders refused; a missing "
           "file is model_missing\n",
           dok);
  }

  /* ---- 7. the Client: download by build, verify, open, warm; storage; diagnose; first use of a context and of
   * the reference data; no network; removal ---- */
  char data_dir[1200];
  snprintf(data_dir, sizeof data_dir, "%s", __FILE__);
  {
    char *slash = strrchr(data_dir, '/'), *bs = strrchr(data_dir, '\\');
    if (bs > slash) slash = bs;
    snprintf(slash ? slash + 1 : data_dir, sizeof data_dir - (size_t)(slash ? slash + 1 - data_dir : 0), "data");
  }
  g_ap.fx = configs ? read_json_file(configs) : t30_json(data_dir, "model_configs.json");
  snprintf(g_m30.cdn, sizeof g_m30.cdn, "%s/%s/sdk/v4.0", local, variant);
  {
    TfModel *probe = tf__model_open(full, &(TfModelOpen){.trusted_key_hex = key, .no_engine = 1});
    snprintf(g_m30.build, sizeof g_m30.build, "%s", probe ? tf_model_build(probe) : "");
    tf_model_close(probe);
  }
  if (!g_ap.fx || strcmp(g_m30.build, tf_json_str(tf_json_get(g_ap.fx, "build")))) {
    printf("client: test/data/model_configs.json names build %s and the local folder is %s: make the configs again (test/make_model_configs.py with local30.json); "
           "the Client's download tests did not run\n",
           g_ap.fx ? tf_json_str(tf_json_get(g_ap.fx, "build")) : "(none)", g_m30.build);
    fails++;
  } else {
    tf_http_set_transport(m30_stub, NULL);
    tf__now_override = 1794355200 + 9 * 3600;
    tf__model_key_override = key; /* the folders are signed with the vectors' key, the configs with another */
    g_ap.mode = 4;
    g_ap.serve = "model";
    snprintf(d, sizeof d, "%s/store", work);
    TfClient *c = m30_client(d, 0);
    /* An unmetered-only prefetch, with the kind of connection from a fake: on a metered connection and without
     * one nothing is fetched and the call says it waits. (On unmetered and on unknown, this library's own
     * answer, it downloads: the resumed download below runs with no fake set.) */
    for (int k = TF__CONNECTION_METERED; k <= TF__CONNECTION_NONE; ++k) {
      tf__connection_override = k;
      CHECK(c && tf_client_prefetch(c, &(TfPrefetchOptions){.unmetered_only = 1}) == TF_ERR_DOWNLOAD_FAILED && g_m30.file_requests == 0 &&
                strstr(tf_last_error(), "waits for an unmetered connection"),
            "an unmetered-only prefetch on a %s connection: %s, %d file requests", tf__connection_name(k), tf_error_name(tf_last_error_code()), g_m30.file_requests);
    }
    tf__connection_override = -1;
    CHECK(tf__connection() == TF__CONNECTION_UNKNOWN, "the library's own answer about the connection is not unknown");
    /* the download drops half way through the weights; the next call resumes it with a Range request */
    g_m30.cut_at_half = 1;
    CHECK(c && tf_client_prefetch(c, &(TfPrefetchOptions){.on_progress = m30_progress}) == TF_ERR_DOWNLOAD_FAILED && strstr(tf_last_error(), "The model download stopped"),
          "a download that drops: %s", tf_error_name(tf_last_error_code()));
    const long long at_cut = g_m30.last_done;
    char *st = c ? (char *)tf_client_storage_json(c) : NULL;
    CHECK(st && strstr(st, "\"version\":\"4.0.0\"") && strstr(st, g_m30.build), "storage while the download is incomplete: %s", st ? st : "");
    /* half a download is not a model: without a network the answer is needs_connection, not half a folder opened */
    g_ap.mode = 0;
    CHECK(c && tf_client_model(c, NULL) == NULL && tf_last_error_code() == TF_ERR_NEEDS_CONNECTION, "model() with half a download and no network: %s",
          tf_error_name(tf_last_error_code()));
    g_ap.mode = 4;
    g_m30.last_done = g_m30.last_total = 0;
    g_m30.progress_calls = 0;
    const int rc = c ? tf_client_prefetch(c, &(TfPrefetchOptions){.unmetered_only = 1, .on_progress = m30_progress}) : -1;
    CHECK(rc == 0 && g_m30.ranged >= 1 && g_m30.progress_bad == 0 && g_m30.progress_calls > 5 && g_m30.last_done == g_m30.last_total && g_m30.last_total > 50000000 &&
              at_cut > 1000000 && g_m30.bad_url == 0,
          "the resumed download: rc %s, %d Range requests, progress %lld of %lld in %d calls (%d out of order), %d URLs not under the build", tf_error_name(rc),
          g_m30.ranged, g_m30.last_done, g_m30.last_total, g_m30.progress_calls, g_m30.progress_bad, g_m30.bad_url);
    /* the day's telemetry has one downloads entry for the build: the attempt that dropped and the one that
     * finished (resumed), with the bytes that came over the network; the attempts that waited are not counted */
    {
      TfJson *day = c ? t30_day(c) : NULL;
      const TfJson *dl = tf_json_get(day, "downloads"), *e0 = dl && dl->type == TJ_ARR && dl->n == 1 ? &dl->kids[0] : NULL;
      const char *prof = e0 ? ap_str(e0, "profile", NULL) : NULL;
      CHECK(e0 && prof && !strcmp(ap_str(e0, "revision", NULL), "4.0.0") && !strcmp(ap_str(e0, "build", NULL), g_m30.build) && strlen(prof) > 4 &&
                !strcmp(prof + strlen(prof) - 4, "-cpu") && t30_num(e0, "bytes") == (double)g_m30.last_total && t30_num(e0, "success") == 1 &&
                t30_num(e0, "failed") == 1 && t30_num(e0, "resumed") == 1,
            "the day's downloads entry: profile %s, %.0f bytes (want %lld), success %.0f, failed %.0f, resumed %.0f", prof ? prof : "(none)",
            e0 ? t30_num(e0, "bytes") : -1, g_m30.last_total, e0 ? t30_num(e0, "success") : -1, e0 ? t30_num(e0, "failed") : -1, e0 ? t30_num(e0, "resumed") : -1);
      tf_json_free(day);
    }
    /* done is done: a prefetch again asks for the manifest and no file */
    int before = g_m30.file_requests;
    CHECK(c && tf_client_prefetch(c, NULL) == 0 && g_m30.file_requests == before, "a prefetch of a model that is here fetched %d files", g_m30.file_requests - before);
    /* the model: opened and warm; the same Model while it is open */
    TfModel *cm = c ? tf_client_model(c, NULL) : NULL;
    TfModel *again = c ? tf_client_model(c, NULL) : NULL;
    CHECK(cm && again == cm && !strcmp(tf_model_version(cm), "4.0.0") && !strcmp(tf_model_build(cm), g_m30.build) && g_m30.file_requests == before,
          "client.model(): %s", tf_last_error() ? tf_last_error() : "");
    tf_model_close(again);
    /* only the CPU set of the context in force and the sample clips came down: no other context, no reference
     * table, no file of another platform */
    {
      char p[1500];
      snprintf(p, sizeof p, "%s/models/%s/c24_l72/frames.zfw", d, g_m30.build);
      const int other_ctx = tf_file_exists(p);
      snprintf(p, sizeof p, "%s/models/%s/expected_tj3.zfx", d, g_m30.build);
      const int ref_file = tf_file_exists(p);
      snprintf(p, sizeof p, "%s/models/%s/coreml", d, g_m30.build);
      const long long coreml = tf__dir_bytes(p);
      snprintf(p, sizeof p, "%s/models/%s/selftest/selftest.json", d, g_m30.build);
      CHECK(!other_ctx && !ref_file && coreml == 0 && tf_file_exists(p), "the download profile: other context %d, reference %d, coreml %lld bytes", other_ctx, ref_file, coreml);
    }
    TfJson *r = c ? ap_diagnose(c, "a model on the device") : NULL;
    const TfJson *models = tf_json_get(tf_json_get(r, "storage"), "models");
    CHECK(r && !strcmp(ap_str(r, "model", "version"), "4.0.0") && !strcmp(ap_str(r, "model", "build"), g_m30.build) && ap_true(r, "model", "verified") &&
              !strcmp(ap_str(r, "context", NULL), "c16_l64") && !strcmp(ap_str(r, "backend", "kind"), "cpu") && !strcmp(ap_str(r, "backend", "stage"), "cpu") &&
              !strcmp(ap_str(r, "config", NULL), "fresh") && ap_true(r, "key", "accepted") && models && models->n == 1 && t30_num(&models->kids[0], "bytes") > 60000000,
          "diagnose with a model: %s", c ? tf_client_diagnose_json(c) : "");
    tf_json_free(r);
    /* a take through the Client's Model is counted: version and build in the cohort, the 6.2 counters, the report */
    if (cm) {
      TfClip clip = {.name = "Husary_001001"};
      TfSession *s = tf_session_listen(cm, &clip, &(TfSessionOptions){.pace = TF_PACE_FAST});
      CHECK(s && tf_session_report_recited(s, 1, 1, 1) == 0 && tf_session_report_recited(s, 1, 1, 7) == 0, "reportRecited after the take ended");
      tf_session_destroy(s);
      TfJson *day = t30_day(c);
      const TfJson *co = tf_json_get(day, "cohorts");
      const TfJson *x = co && co->n == 1 ? &co->kids[0] : NULL;
      CHECK(x && !strcmp(ap_str(x, "revision", NULL), "4.0.0") && !strcmp(ap_str(x, "build", NULL), g_m30.build) && !strcmp(ap_str(x, "context", NULL), "c16_l64") &&
                !strcmp(ap_str(x, "backend", NULL), "cpu") && t30_num(x, "sessions") == 1 && t30_num(x, "takes") == 1 && fabs(t30_num(x, "audio_s") - 5.12) < 1e-9 &&
                t30_num(x, "speech_s") > 1 && fabs(t30_num(x, "speech_s") + t30_num(x, "silence_s") - 0.04 * (int)((t30_num(x, "speech_s") + t30_num(x, "silence_s")) / 0.04 + 0.5)) < 1e-9 &&
                t30_num(tf_json_get(x, "tokens"), "log_probs") == 21 && t30_num(tf_json_get(x, "tokens"), "inter1_log_probs") > 0 && t30_num(x, "compute_s") > 0 &&
                t30_num(day, "ayahs") == 1 && t30_in_list(tf_json_get(day, "reported"), "ayahs"),
            "the take's telemetry: %s", day ? "see the day's body" : "no body");
      keys_within(day, tf_json_get(t30_json(data_dir, "sdk_3.0.0/telemetry_30.json"), "example_payload"), "the take's delta", 0);
      tf_json_free(day);
    }
    /* another context on first use: its files are fetched, then the take runs in it */
    before = g_m30.file_requests;
    TfSession *s = cm ? tf_session_create(cm, &(TfSessionOptions){.context = "c8_l64"}) : NULL;
    CHECK(s && g_m30.file_requests > before, "a context on first use: %s (%d files fetched)", tf_last_error() ? tf_last_error() : "", g_m30.file_requests - before);
    tf_session_destroy(s);
    /* the reference data on first ask */
    TfReference *ref = cm ? tf_model_reference(cm) : NULL;
    TfUnits u;
    CHECK(ref && !tf_reference_is_downloaded(ref) && tf_reference_units(ref, 1, 1, &u) == TF_ERR_REFERENCE_MISSING && tf_reference_fetch(ref) == 0 &&
              tf_reference_is_downloaded(ref) && tf_reference_units(ref, 112, 1, &u) == 0 && u.n > 5 && tf_reference_fetch(ref) == 0,
          "the reference data fetched on the app's ask: %s", tf_error_name(tf_last_error_code()));
    /* the files cannot go while the Model is open; after it is closed they can, and the consent state stays */
    CHECK(c && tf_client_remove_models(c) == TF_ERR_STORAGE_FAILED, "removeModels while the Model is open: %s", tf_error_name(tf_last_error_code()));
    tf_model_close(cm);
    tf_client_destroy(c);

    /* a start without a network: the model on disk opens (a new process would do the same) */
    g_ap.mode = 0;
    c = m30_client(d, 0);
    cm = c ? tf_client_model(c, NULL) : NULL;
    CHECK(cm && !strcmp(tf_model_build(cm), g_m30.build), "a start without a network with the model on disk: %s", tf_last_error() ? tf_last_error() : "");
    r = c ? ap_diagnose(c, "offline with a model") : NULL;
    CHECK(r && ap_true(r, "model", "verified") && ap_null(r, "key", "accepted"), "diagnose offline: %s", c ? tf_client_diagnose_json(c) : "");
    tf_json_free(r);
    tf_model_close(cm);
    /* a damaged file in the store: offline it is refused, online the next model() fetches it again */
    {
      char p[1500];
      snprintf(p, sizeof p, "%s/models/%s", d, g_m30.build);
      m30_flip(p, "c16_l64/frames.zfw");
    }
    cm = c ? tf_client_model(c, NULL) : NULL;
    CHECK(!cm && tf_last_error_code() == TF_ERR_CHECKSUM_MISMATCH, "a damaged file with no network: %s", tf_error_name(tf_last_error_code()));
    r = c ? ap_diagnose(c, "a damaged model") : NULL;
    CHECK(r && ap_false(r, "model", "verified") && !strcmp(ap_str(r, "lastError", NULL), "checksum_mismatch"), "diagnose with a damaged file: %s", c ? tf_client_diagnose_json(c) : "");
    tf_json_free(r);
    g_ap.mode = 4;
    cm = c ? tf_client_model(c, NULL) : NULL;
    CHECK(cm != NULL, "the damaged file was not fetched again: %s", tf_last_error() ? tf_last_error() : "");
    tf_model_close(cm);
    CHECK(c && tf_client_remove_models(c) == 0 && !strcmp(tf_client_storage_json(c), "{\"models\":[],\"sharingQueueBytes\":0}"), "removeModels: %s",
          c ? tf_client_storage_json(c) : "");
    tf_client_destroy(c);

    /* the portal's Response time setting: the config's inference.context is the context in force; one the build
     * does not have is ignored; the app's choice wins */
    static const struct {
      const char *serve, *want;
      int ms;
    } ctx[] = {{"model c24", "c24_l72", 480}, {"model unknown context", "c16_l64", 320}};
    for (int i = 0; i < 2; ++i) {
      g_ap.serve = ctx[i].serve;
      snprintf(d, sizeof d, "%s/ctx%d", work, i);
      c = m30_client(d, 0);
      cm = c ? tf_client_model(c, NULL) : NULL;
      M30Run run;
      m30_reset(&run);
      TfClip clip = {.name = "Saood_001001"};
      s = cm ? tf_session_listen(cm, &clip, &(TfSessionOptions){.pace = TF_PACE_FAST, .on_chunk = m30_chunk, .user = &run}) : NULL;
      r = c ? ap_diagnose(c, ctx[i].serve) : NULL;
      CHECK(s && run.chunks > 0 && run.last_end_ms % ctx[i].ms == 0 && r && !strcmp(ap_str(r, "context", NULL), ctx[i].want), "%s: chunks end at %d ms, context %s",
            ctx[i].serve, run.last_end_ms, r ? ap_str(r, "context", NULL) : "");
      tf_json_free(r);
      tf_session_destroy(s);
      if (i == 0) {
        s = cm ? tf_session_create(cm, &(TfSessionOptions){.context = "c16_l64"}) : NULL;
        CHECK(s != NULL, "the app's context over the config's: %s", tf_last_error() ? tf_last_error() : "");
        tf_session_destroy(s);
      }
      tf_model_close(cm);
      tf_client_destroy(c);
    }
    /* the config names another build, or another version, than the manifest that is served */
    static const struct {
      const char *serve;
      int code;
    } wrong[] = {{"model other build", TF_ERR_CHECKSUM_MISMATCH},{"model other version", TF_ERR_MODEL_INVALID}};
    for (int i = 0; i < 2; ++i) {
      g_ap.serve = wrong[i].serve;
      snprintf(d, sizeof d, "%s/wrong%d", work, i);
      c = m30_client(d, 0);
      cm = c ? tf_client_model(c, NULL) : NULL;
      CHECK(!cm && tf_last_error_code() == wrong[i].code, "%s: %s (%s)", wrong[i].serve, cm ? "opened" : tf_error_name(tf_last_error_code()), tf_last_error() ? tf_last_error() : "");
      tf_model_close(cm);
      tf_client_destroy(c);
    }
    /* a pin the server could not honour: the normal choice is used and a debug build logs it once */
    g_ap.serve = "model pin unavailable";
    snprintf(d, sizeof d, "%s/pin", work);
    g_m30.log_lines = 0;
    c = m30_client(d, 1);
    CHECK(c && tf_client_prefetch(c, &(TfPrefetchOptions){.version = "9.9.9"}) == 0 && strstr(g_ap.config_path, "model_version=9.9.9") && g_m30.log_lines >= 1 &&
              strstr(g_m30.log_last, "9.9.9"),
          "a pin that is unavailable: %s, %d log lines: %s", tf_error_name(tf_last_error_code()), g_m30.log_lines, g_m30.log_last);
    tf_client_destroy(c);
    /* ---- 7b. SDK 3.1.0 (SPEC section 15): the Client opens only a build it downloaded itself, a refused Client
     * does not run the model, and every model request carries the key and the app token ---- */
    {
      char a[1100], b[1100], to[1300], cur[1300];
      g_ap.mode = 4;
      g_ap.serve = "model";
      /* a real download into storage A: the record is there before the first request, and says installed after */
      snprintf(a, sizeof a, "%s/rec_a", work);
      snprintf(g_m30.record, sizeof g_m30.record, "%s/models/%s.install", a, g_m30.build);
      g_m30.record_first = -1;
      g_m30.no_headers = 0;
      c = m30_client(a, 0);
      int before = g_m30.file_requests;
      CHECK(c && tf_client_prefetch(c, NULL) == 0, "a download into a new storage: %s", tf_last_error() ? tf_last_error() : "");
      const int full = g_m30.file_requests - before;
      size_t rn = 0;
      char *rtext = read_text(g_m30.record, &rn);
      CHECK(g_m30.record_first == 1 && rtext && !strncmp(rtext, "installed ", 10) && rn == 10 + 64 + 1 && full > 5,
            "the install record: there before the first model request %d, afterwards \"%.20s\", %d files fetched", g_m30.record_first, rtext ? rtext : "(none)", full);
      free(rtext);
      g_m30.record[0] = 0;
      tf_client_destroy(c);
      /* the next start: the build opens with no request for a file, and without a network too */
      c = m30_client(a, 0);
      before = g_m30.file_requests;
      cm = c ? tf_client_model(c, NULL) : NULL;
      CHECK(cm && g_m30.file_requests == before, "the next start after a real download: %s, %d files fetched", cm ? "opened" : tf_error_name(tf_last_error_code()),
            g_m30.file_requests - before);
      tf_model_close(cm);
      tf_client_destroy(c);
      g_ap.mode = 0;
      c = m30_client(a, 0);
      cm = c ? tf_client_model(c, NULL) : NULL;
      CHECK(cm != NULL, "the next start without a network after a real download: %s", tf_error_name(tf_last_error_code()));
      tf_model_close(cm);
      tf_client_destroy(c);
      g_ap.mode = 4;

      /* storage filled in advance, exactly as a real download leaves it (the build folder and current.json), in three
       * ways: no record; the record of the other storage without its key (as on another machine); a record of
       * another build under this build's name. None is opened: the folder goes and the build is downloaded. */
      static const char *const kFill[] = {"no record", "a record made with another key", "the record of another build"};
      for (int k = 0; k < 3; ++k) {
#ifndef _WIN32
        if (k == 1) continue; /* on Linux the key is the machine's and the user's: a record of the same machine is this machine's own */
#endif
        snprintf(b, sizeof b, "%s/rec_b%d", work, k);
        snprintf(to, sizeof to, "%s/models/%s", b, g_m30.build);
        snprintf(cur, sizeof cur, "%s/models/current.json", b);
        m30_fill(a, b, g_m30.build, k);
        /* without a network nothing is opened, and the folder is gone */
        g_ap.mode = 0;
        c = m30_client(b, 0);
        char mf[1500];
        snprintf(mf, sizeof mf, "%s/manifest.json", to);
        cm = c ? tf_client_model(c, NULL) : NULL;
        CHECK(!cm && tf_last_error_code() == TF_ERR_NEEDS_CONNECTION && !tf_file_exists(mf) && !tf_file_exists(cur),
              "a storage filled in advance (%s), no network: %s, the folder %s", kFill[k], cm ? "OPENED" : tf_error_name(tf_last_error_code()),
              tf_file_exists(mf) ? "is still there" : "is gone");
        tf_model_close(cm);
        tf_client_destroy(c);
        /* with one: the whole build is downloaded, nothing of the copy reused */
        m30_fill(a, b, g_m30.build, k);
        g_ap.mode = 4;
        c = m30_client(b, 0);
        before = g_m30.file_requests;
        cm = c ? tf_client_model(c, NULL) : NULL;
        CHECK(cm && g_m30.file_requests - before == full, "a storage filled in advance (%s): %s, %d files fetched, a whole download is %d", kFill[k],
              cm ? "opened after a download" : tf_error_name(tf_last_error_code()), g_m30.file_requests - before, full);
        tf_model_close(cm);
        tf_client_destroy(c);
      }

      /* the key store: "there is no key" makes every record invalid (the build is downloaded again); a store that
       * fails is not "no key": nothing is opened, nothing deleted, nothing downloaded, and the next call works */
      c = m30_client(a, 0);
      before = g_m30.file_requests;
      int mbefore = g_m30.manifest_requests;
      tf__install_key_test = -1;
      cm = c ? tf_client_model(c, NULL) : NULL;
      snprintf(to, sizeof to, "%s/models/%s/manifest.json", a, g_m30.build);
      CHECK(!cm && tf_last_error_code() == TF_ERR_STORAGE_FAILED && tf_file_exists(to) && g_m30.file_requests == before && g_m30.manifest_requests == mbefore,
            "the key store fails: %s, the model %s, %d model requests", cm ? "OPENED" : tf_error_name(tf_last_error_code()), tf_file_exists(to) ? "is still there" : "WAS DELETED",
            g_m30.file_requests - before + g_m30.manifest_requests - mbefore);
      tf_model_close(cm);
      tf__install_key_test = 0;
      cm = c ? tf_client_model(c, NULL) : NULL;
      CHECK(cm && g_m30.file_requests == before, "after the key store works again: %s, %d files fetched", cm ? "opened" : tf_error_name(tf_last_error_code()),
            g_m30.file_requests - before);
      tf_model_close(cm);
      tf__install_key_test = 1;
      cm = c ? tf_client_model(c, NULL) : NULL;
      CHECK(cm && g_m30.file_requests - before == full, "the key store has no key: %s, %d files fetched, a whole download is %d",
            cm ? "opened after a download" : tf_error_name(tf_last_error_code()), g_m30.file_requests - before, full);
      tf_model_close(cm);
      tf__install_key_test = 0;
      tf_client_destroy(c);

      /* a refused Client with a model from a real download on this device (SPEC 15.3, contract 1.10.0): a reason
       * of the list stops it AND removes it. When the call returns the storage holds no model file, no install
       * record and no current.json; the remembered refusal and the device key are still there. */
      char amf[1500], arec[1500], acur[1500], amodels[1300], astate[1300], akey[1300], b0[1100];
      snprintf(amodels, sizeof amodels, "%s/models", a);
      snprintf(amf, sizeof amf, "%s/models/%s/manifest.json", a, g_m30.build);
      snprintf(arec, sizeof arec, "%s/models/%s.install", a, g_m30.build);
      snprintf(acur, sizeof acur, "%s/models/current.json", a);
      snprintf(astate, sizeof astate, "%s/client.json", a);
      snprintf(akey, sizeof akey, "%s/install.key", a);
      snprintf(b0, sizeof b0, "%s/rec_b0", work);
      char (*left)[256] = (char (*)[256])malloc(8 * 256);
      const int had_key = tf_file_exists(akey); /* Windows, and a Linux machine without a machine id */
      CHECK(tf_file_exists(amf) && tf_file_exists(arec) && tf_file_exists(acur), "before the refusal the downloaded model is on the device");
      g_ap.mode = 5;
      g_ap.raw_status = 403;
      g_ap.raw_body = "{\"error\":\"app_blocked\",\"message\":\"This app is not served.\",\"reason\":\"app_blocked\"}";
      c = m30_client(a, 0);
      cm = c ? tf_client_model(c, NULL) : NULL;
      CHECK(!cm && tf_last_error_code() == TF_ERR_SERVER_REJECTED && strstr(tf_last_error(), "(app_blocked)"), "app_blocked with a model on disk: %s (%s)",
            cm ? "THE MODEL RAN" : tf_error_name(tf_last_error_code()), tf_last_error() ? tf_last_error() : "");
      int nleft = left ? tf__list_dir(amodels, left, 8) : -1;
      size_t sn = 0;
      char *stext = read_text(astate, &sn);
      CHECK(nleft <= 0 && !tf_file_exists(amf) && !tf_file_exists(arec) && !tf_file_exists(acur), "app_blocked with a model on disk: %d entries are left under models/ (%s)",
            nleft, nleft > 0 ? left[0] : "");
      CHECK(stext && strstr(stext, "\"refused\":\"app_blocked\"") && strstr(stext, "\"remove_models\":false") && tf_file_exists(akey) == had_key,
            "after the removal the remembered refusal and the device key are there: %s, key file %d (before: %d)", stext ? stext : "(no state)", tf_file_exists(akey), had_key);
      free(stext);
      tf_model_close(cm);
      tf_client_destroy(c);
      /* the next start, without a network, with the model put back by hand (the folder and current.json of
       * another storage's real download): removed at the start, before any call, and the call is the refusal */
      m30_fill(b0, a, g_m30.build, 0);
      CHECK(tf_file_exists(amf) && tf_file_exists(acur), "the model put back by hand");
      g_ap.mode = 0;
      c = m30_client(a, 0);
      nleft = left ? tf__list_dir(amodels, left, 8) : -1;
      CHECK(c && nleft <= 0 && !tf_file_exists(amf), "a start with a remembered refusal and a model put back by hand: %d entries are left under models/", nleft);
      cm = c ? tf_client_model(c, NULL) : NULL;
      CHECK(!cm && tf_last_error_code() == TF_ERR_SERVER_REJECTED, "the next start without a network after app_blocked: %s",
            cm ? "THE MODEL RAN" : tf_error_name(tf_last_error_code()));
      tf_model_close(cm);
      tf_client_destroy(c);
      g_ap.mode = 5;
      g_ap.raw_status = 401;
      g_ap.raw_body = "{\"error\":\"invalid_key\",\"message\":\"This key is not valid.\",\"reason\":\"key_revoked\"}";
      c = m30_client(a, 0);
      cm = c ? tf_client_model(c, NULL) : NULL;
      CHECK(!cm && tf_last_error_code() == TF_ERR_INVALID_KEY, "a revoked key with a model on disk: %s", cm ? "THE MODEL RAN" : tf_error_name(tf_last_error_code()));
      tf_model_close(cm);
      tf_client_destroy(c);
      /* an answer that accepts the key: the device holds no model, so the build is downloaded as on a first
       * install, every file of it (1.8.0 let it run again without a download: no longer) */
      g_ap.mode = 4;
      c = m30_client(a, 0);
      before = g_m30.file_requests;
      cm = c ? tf_client_model(c, NULL) : NULL;
      CHECK(cm && g_m30.file_requests - before == full && tf_file_exists(amf) && tf_file_exists(arec),
            "after an answer that accepts the key: %s, %d files fetched, a whole download is %d", cm ? "the model runs" : tf_error_name(tf_last_error_code()),
            g_m30.file_requests - before, full);
      /* a Model that is open when the refusal arrives keeps running, with its files, until the app closes it;
       * then the files go (owed until then: remove_models is true in the state) */
      g_ap.mode = 5;
      g_ap.raw_status = 401;
      g_ap.raw_body = "{\"error\":\"invalid_key\",\"message\":\"This key is not valid.\",\"reason\":\"key_revoked\"}";
      CHECK(cm && tf_client_prefetch(c, NULL) == TF_ERR_INVALID_KEY && !strcmp(tf__client_gate(c), "key_revoked"), "a refusal while a Model is open: %s, remembered \"%s\"",
            tf_error_name(tf_last_error_code()), c ? tf__client_gate(c) : "");
      stext = read_text(astate, &sn);
      CHECK(tf_file_exists(amf) && tf_file_exists(arec) && stext && strstr(stext, "\"remove_models\":true"), "while the Model is open its files stay and the removal is owed: %s",
            stext ? stext : "(no state)");
      free(stext);
      {
        static const float still[1600];
        TfSession *os = cm ? tf_session_create(cm, NULL) : NULL;
        int pushed = 0;
        for (int i = 0; os && i < 20; ++i) pushed += tf_session_push(os, still, 1600) >= 0;
        CHECK(os && pushed == 20 && tf_session_finish(os) >= 0, "the open Model after the refusal: a take of 2 s, %d of 20 pushes taken (%s)", pushed,
              tf_last_error() ? tf_last_error() : "");
        tf_session_destroy(os);
      }
      tf_model_close(cm);
      nleft = left ? tf__list_dir(amodels, left, 8) : -1;
      stext = read_text(astate, &sn);
      CHECK(nleft <= 0 && !tf_file_exists(amf) && !tf_file_exists(arec) && stext && strstr(stext, "\"remove_models\":false") &&
                strstr(stext, "\"refused\":\"key_revoked\""),
            "once the Model is closed its files are gone: %d entries are left under models/, state %s", nleft, stext ? stext : "(none)");
      free(stext);
      tf_client_destroy(c);
      /* accepted again: a whole download once more, for the answers below */
      g_ap.mode = 4;
      c = m30_client(a, 0);
      before = g_m30.file_requests;
      cm = c ? tf_client_model(c, NULL) : NULL;
      CHECK(cm && g_m30.file_requests - before == full, "accepted again after the second refusal: %s, %d files fetched", cm ? "the model runs" : tf_error_name(tf_last_error_code()),
            g_m30.file_requests - before);
      tf_model_close(cm);
      tf_client_destroy(c);
      free(left);
      /* an answer without a reason never stops a model that is here: a bare 401, the same 403 without its reason,
       * the floor's answer, a network Quran Lab does not serve */
      static const struct {
        int status;
        const char *body, *what;
      } soft[] = {{401, "{\"error\":\"invalid_key\",\"message\":\"unknown key\"}", "401 invalid_key without a reason"},
                  {403, "{\"error\":\"app_blocked\",\"message\":\"refused\"}", "403 app_blocked without a reason"},
                  {403, "{\"error\":\"sdk_too_old\",\"message\":\"newer\",\"min_sdk\":\"3.2.0\"}", "403 sdk_too_old"},
                  {503, "{\"error\":\"network_blocked\",\"message\":\"not served\",\"retry_after\":3600}", "503 network_blocked"}};
      for (int k = 0; k < 4; ++k) {
        g_ap.mode = 5;
        g_ap.raw_status = soft[k].status;
        g_ap.raw_body = soft[k].body;
        c = m30_client(a, 0);
        before = g_m30.file_requests;
        cm = c ? tf_client_model(c, NULL) : NULL;
        CHECK(cm && !*tf__client_gate(c) && g_m30.file_requests == before && tf_file_exists(amf) && tf_file_exists(arec),
              "%s with a model on disk: %s, remembered \"%s\", %d files fetched (an answer that is no stop removes nothing)", soft[k].what,
              cm ? "the model runs" : tf_error_name(tf_last_error_code()), c ? tf__client_gate(c) : "", g_m30.file_requests - before);
        tf_model_close(cm);
        tf_client_destroy(c);
      }
      g_ap.raw_body = NULL;
      g_ap.mode = 4;
      CHECK(g_m30.no_headers == 0, "%d model requests did not carry both the key and the app token", g_m30.no_headers);
      printf("client 3.1.0: the install record (written before the first request, %d files in a whole download); a storage filled in advance is not opened and is "
             "downloaded, in 3 ways; the key store's two failures; a refusal with a model on disk stops it and removes it (no file, no record; the refusal "
             "and the device key stay), a model put back by hand goes at the next start without a network, an accepting answer downloads every file "
             "again, a Model open at the refusal ran a take and its files went at its close; 4 answers without a reason stop nothing and remove "
             "nothing; every model request carried the key and the token\n",
             full);
    }
    tf__model_key_override = NULL;
    tf_http_set_transport(NULL, NULL);
    tf__now_override = 0;
    printf("client: prefetch (a cut download resumed, progress in bytes to %lld), model() opened and warm and the same while open, storage, diagnose, a take counted, "
           "a context and the reference data on first use, a start without a network, a damaged file refused offline and fetched again online, removal, the config's "
           "context, a wrong build and version refused, an unavailable pin logged\n",
           g_m30.last_total);
  }
  tf_json_free(g_ap.fx);
  g_ap.fx = NULL;
  printf("model30 (%s): peak memory %lld MB\n", variant, own_peak_memory() >> 20);
  printf("model30: %s\n", fails ? "FAIL" : "all ok");
  return fails ? 1 : 0;
}
