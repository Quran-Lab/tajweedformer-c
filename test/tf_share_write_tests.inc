/* Contract section 29 (sdk_4.0.0/SHARE_WRITE_40.md, SDK 4.0.2): a write that fails during a shared recording ends it as
 * a crash would, and the crash recovery keeps the closed segments and the open segment's whole pages when at least one
 * segment closed and sharing is on; nothing is kept otherwise. The shared vector's six cases (share_write_40.json, a
 * copy of the contract's vectors/share_write_40.json), each on a new storage folder, through the real capture and the
 * real uploader against the stub server. A full disk is tf__share_test_disk_full: every write of the recording fails
 * (its pages, markers, capture.json, take list, and the recovery's rewrite). When the write fails, this test also
 * leaves a torn page at the end of the open segment, as a write cut short would, so the recovery's cut is checked.
 *
 *   tf_tests share_write <share_write_40.json> <work dir> */
extern int tf__share_test_disk_full;

/* the only recording in the outbox: its folder, or "" */
static void sw_session(const char *outbox, char *out, size_t n) {
  char names[16][256];
  const int k = tf__list_dir(outbox, names, 16);
  out[0] = 0;
  for (int i = 0; i < k; ++i)
    if (strlen(names[i]) == 32) snprintf(out, n, "%s/%s", outbox, names[i]);
}
static int sw_count(const char *outbox) {
  char names[16][256];
  const int k = tf__list_dir(outbox, names, 16);
  int c = 0;
  for (int i = 0; i < k; ++i) c += strlen(names[i]) == 32;
  return c;
}
/* a torn page at the end of the open segment: a page header cut short */
static void sw_tear(const char *outbox) {
  char sess[600];
  sw_session(outbox, sess, sizeof sess);
  if (!sess[0]) return;
  TfJson *cap = load_json(sess, "capture.json");
  const TfJson *cl = tf_json_get(cap, "closed");
  const int open_n = (cl ? (int)cl->num : 0) + 1;
  tf_json_free(cap);
  char name[32];
  snprintf(name, sizeof name, "seg-%05d.opus", open_n);
  char *p = tf_path_join(sess, name);
  if (p && tf_file_exists(p)) {
    FILE *f = fopen(p, "ab");
    static const uint8_t torn[26] = {'O', 'g', 'g', 'S', 0, 0, 0x40, 0x1f};
    if (f) {
      fwrite(torn, 1, sizeof torn, f);
      fclose(f);
    }
  }
  free(p);
}
static TfClient *sw_client(const char *dir, const TfJson *flags) {
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"contributions\":true");
  for (int i = 0; flags && i < flags->n; ++i) {
    const TfJson *f = &flags->kids[i];
    if (f->type == TJ_BOOL) tf_buf_printf(&b, ",\"%s\":%s", f->key, f->boolean ? "true" : "false");
    else if (f->type == TJ_NUM) tf_buf_printf(&b, ",\"%s\":%lld", f->key, (long long)f->num);
  }
  tf_buf_puts(&b, "}");
  seed_config(dir, b.p);
  tf_buf_free(&b);
  TfClientConfig o = tf__client_default_config();
  o.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "org.example.ShareTest";
  o.platform = "linux";
  o.api_url = "https://api.test";
  o.storage_dir = dir;
  return tf__client_open(&o);
}
/* the uploaded stream of the stub's only session: segments, audio ms, EOS seen */
static int sw_stream(const StubSession *st, const char *form, int rate, int *segments, long long *audio_ms) {
  size_t n = 0;
  for (int k = 1; k <= st->max_n; ++k) n += st->seg_n[k];
  uint8_t *d = (uint8_t *)malloc(n + 1);
  size_t at = 0;
  int missing = 0;
  for (int k = 1; k <= st->max_n; ++k) {
    if (!st->seg[k]) missing++;
    else memcpy(d + at, st->seg[k], st->seg_n[k]);
    at += st->seg_n[k];
  }
  long long g = 0;
  int pages = 0;
  const int bad = missing ? -1 : check_ogg(d, n, 0, &g, &pages); /* valid pages, no EOS */
  long long pre_skip = 0;
  if (!strcmp(form, "opus") && n > 40 && !memcmp(d + 28, "OpusHead", 8)) pre_skip = d[38] | d[39] << 8;
  *audio_ms = !strcmp(form, "opus") ? (g - pre_skip) * 1000 / 48000 : g * 1000 / rate;
  *segments = st->max_n;
  free(d);
  return bad == 0 && pages > 0;
}

static int share_write_tests(const char *vpath, const char *dir) {
  TfJson *v = read_json_file(vpath);
  if (!v) return 2;
  tf_mutex_init(&g_st.m);
  tf_http_set_transport(stub, NULL);
  const TfJson *plan = tf_json_get(v, "plan_s"), *cases = tf_json_get(v, "cases");
  for (int i = 0; plan && i < 4 && i < plan->n; ++i) tf__share_plan[i] = (int)plan->kids[i].num;
  int passed = 0;
  for (int ci = 0; cases && ci < cases->n; ++ci) {
    const TfJson *cs = &cases->kids[ci], *ev = tf_json_get(cs, "events"), *ex = tf_json_get(cs, "expect");
    const char *name = tf_json_str(tf_json_get(cs, "name")), *form = tf_json_str(tf_json_get(cs, "form"));
    const int rate = (int)tf_json_get(cs, "rate")->num, frame = rate / 50;
    const int fails0 = fails;
    stub_reset();
    char sub[16];
    snprintf(sub, sizeof sub, "c%d", ci + 1);
    char *d = tf_path_join(dir, sub), *outbox = tf_path_join(d, "outbox");
    TfClient *c = sw_client(d, tf_json_get(cs, "flags"));
    CHECK(c && tf__client_sharing_answer(c, 1) == 0, "%s: the learner's yes", name);
    float *x = (float *)malloc(sizeof(float) * (size_t)rate * 40);
    lcg_noise(x, rate * 40, 0.2);
    TfShareSession *ss = NULL;
    long long fed = 0; /* frames fed in this case: the signal goes on across takes */
    int take_ms[8], n_take = 0, relaunched = 0;
    for (int k = 0; c && ev && k < ev->n; ++k) {
      const char *e = tf_json_str(tf_json_get(&ev->kids[k], "event"));
      if (!strcmp(e, "take_start")) {
        ss = tf__sharing_session_begin(c, rate, NULL);
        CHECK(ss != NULL, "%s: a take begins (%s)", name, ss ? "ok" : tf_last_error());
      } else if (!strcmp(e, "audio")) {
        const int frames = (int)(tf_json_get(&ev->kids[k], "s")->num * 50);
        for (int f = 0; ss && f < frames; ++f, ++fed) tf__sharing_session_push(ss, x + (size_t)(fed % (40 * 50)) * frame, frame);
      } else if (!strcmp(e, "write_fails")) {
        tf__share_test_disk_full = 1;
        sw_tear(outbox);
      } else if (!strcmp(e, "disk_ok")) {
        tf__share_test_disk_full = 0;
      } else if (!strcmp(e, "take_end")) {
        const TfJson *cp = tf_json_get(&ev->kids[k], "completed");
        const int ms = ss ? tf__sharing_session_finish(ss, cp && cp->type == TJ_BOOL ? cp->boolean : 1) : -1;
        ss = NULL;
        if (n_take < 8) take_ms[n_take++] = ms;
      } else if (!strcmp(e, "sharing_off")) {
        tf__client_sharing_set(c, 0);
      } else if (!strcmp(e, "relaunch")) {
        /* the process ends with the files as they are; a new client on the same folder (nothing captures in it) */
        tf__client_close(c);
        c = sw_client(d, tf_json_get(cs, "flags"));
        relaunched = 1;
      }
    }
    tf__share_test_disk_full = 0;
    const TfJson *tm = tf_json_get(ex, "take_ms");
    int tm_ok = tm && tm->n == n_take;
    for (int k = 0; tm_ok && k < n_take; ++k) tm_ok = (int)tm->kids[k].num == take_ms[k];
    CHECK(tm_ok, "%s: what each take returned (%d takes, the last %d)", name, n_take, n_take ? take_ms[n_take - 1] : -1);
    const TfJson *kj = tf_json_get(ex, "kept");
    const int kept = kj && kj->type == TJ_BOOL && kj->boolean;
    const int smin = kept ? (int)tf_json_get(ex, "segments_min")->num : 0, smax = kept ? (int)tf_json_get(ex, "segments_max")->num : 0;
    if (c) tf__sharing_recording_close(c);
    /* on disk, before any upload (the uploader has not run in this process; after a relaunch the next one recovers) */
    if (!relaunched) {
      char sess[600];
      sw_session(outbox, sess, sizeof sess);
      if (kept) {
        TfJson *cap = sess[0] ? load_json(sess, "capture.json") : NULL;
        const TfJson *en = tf_json_get(cap, "ended"), *co = tf_json_get(cap, "completed"), *re = tf_json_get(cap, "recovered"),
                     *cl = tf_json_get(cap, "closed");
        CHECK(sw_count(outbox) == 1 && cap && en && en->boolean && co && !co->boolean && re && re->boolean && cl && cl->num >= smin && cl->num <= smax,
              "%s: one recording kept on disk, ended, recovered, not completed (%d in the outbox, closed %d)", name, sw_count(outbox),
              cl ? (int)cl->num : -1);
        tf_json_free(cap);
      } else {
        CHECK(sw_count(outbox) == 0, "%s: nothing kept on disk (%d in the outbox)", name, sw_count(outbox));
      }
    } else {
      char sess[600];
      sw_session(outbox, sess, sizeof sess);
      TfJson *cap = sess[0] ? load_json(sess, "capture.json") : NULL;
      const TfJson *en = tf_json_get(cap, "ended");
      CHECK(cap && en && !en->boolean, "%s: the full disk left the recording as the capture last wrote it (not ended) for the next launch", name);
      tf_json_free(cap);
    }
    /* the uploader: the kept recording goes up as one valid stream without EOS; nothing else goes. Waited for until a
     * pass has run and what this case expects is there (20 s at most): tf__sharing_outbox_wait asks for two passes,
     * and with nothing left to wake the uploader the second never comes */
    const TfJson *tk0 = tf_json_get(ex, "takes");
    const int want_list = kept && tk0 && tk0->type == TJ_NUM;
    const int before = c ? tf__share_passes(c) : 0;
    if (c) tf__sharing_outbox_resume(c);
    for (int w = 0; c && w < 400; ++w) {
      const int done = tf__share_passes(c) > before && tf__sharing_outbox_pending(c) == 0 &&
                       (!kept || (strstr(tf__client_sharing_status_json(c), "\"sharedSessions\":1") && (!want_list || g_st.takes_calls >= 1)));
      if (done) break;
      SLEEP_MS(50);
    }
    const int left = c ? tf__sharing_outbox_pending(c) : -1;
    if (kept) {
      int segs = 0;
      long long ams = -1;
      const int ok = g_st.n_s == 1 && sw_stream(&g_st.s[0], form, rate, &segs, &ams);
      CHECK(left == 0 && ok && segs >= smin && segs <= smax, "%s: uploaded as one stream without EOS (%d left, %d sessions, %d segments)", name, left,
            g_st.n_s, segs);
      const long long amin = (long long)tf_json_get(ex, "audio_ms_min")->num, amax = (long long)tf_json_get(ex, "audio_ms_max")->num;
      CHECK(ams >= amin && ams <= amax, "%s: its audio %lld ms, want %lld to %lld", name, ams, amin, amax);
      const TfJson *tk = tf_json_get(ex, "takes");
      if (tk && tk->type == TJ_NUM) {
        int n_list = 0;
        for (const char *q = g_st.takes_calls ? g_st.takes_body[0] : NULL; q && (q = strstr(q, "\"start_ms\"")); ++q) n_list++;
        CHECK(g_st.takes_calls >= 1 && n_list == (int)tk->num, "%s: the take list holds %d takes, want %d", name, n_list, (int)tk->num);
      } else {
        CHECK(g_st.takes_calls == 0, "%s: a single take sends no take list (%d calls)", name, g_st.takes_calls);
      }
      CHECK(c && strstr(tf__client_sharing_status_json(c), "\"sharedSessions\":1"), "%s: counted as shared: %s", name,
            c ? tf__client_sharing_status_json(c) : "no client");
      if (fails == fails0) printf("%s: kept, %d segments, %lld ms, uploaded\n", name, segs, ams);
      else printf("%s: FAIL\n", name);
    } else {
      CHECK(g_st.n_s == 0 && sw_count(outbox) == 0, "%s: nothing uploaded and nothing left (%d sessions, %d in the outbox)", name, g_st.n_s,
            sw_count(outbox));
      printf("%s: %s\n", name, fails == fails0 ? "nothing kept" : "FAIL");
    }
    passed += fails == fails0;
    if (c) tf__client_close(c);
    free(x);
    free(outbox);
    free(d);
  }
  printf("share_write: %d of %d cases\n", passed, cases ? cases->n : 0);
  tf_http_set_transport(NULL, NULL);
  stub_reset();
  tf_json_free(v);
  printf("share_write: %s\n", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}
