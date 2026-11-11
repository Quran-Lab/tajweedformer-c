/* Included by tf_tests.c: shared recordings in SDK 3.0.0 (the feed, the cap, take_seq and gap_ms_prev).
 *
 *   share30 <data dir> <work dir>     <data dir> = test/data/sdk_3.0.0 (sharing_30.json)
 *
 * 1. Every case of the shared vector sharing_30.json is replayed on a fake wall clock through the calls an app's
 *    audio reaches: tf_sharing_feed for audio outside takes, and the take calls a Session makes (begin, push,
 *    finish). Checked per feed call: where its samples went (tail, ring, dropped) and the ring's size. Per take:
 *    its pre-roll, whether it was recorded, that audio fed during it was dropped, that the ring is empty after.
 *    Per recording, when it closes: rate, samples, audio_ms, take_seq, gap_ms_prev (absent or its value), the
 *    take list, completed, and that it holds exactly 20 minutes when the vector says it closed at the cap.
 * 2. The ring is bounded: ten minutes fed in 100 ms pieces, and one buffer of five seconds, leave exactly one
 *    second in a buffer of exactly one second; another rate replaces it; background and sharing off free it.
 * 3. During the tail wait only fed audio enters a recording: a take ends, nothing is fed, the 1500 ms run out,
 *    and the recording holds exactly the take's samples (also with a join window of 0, and at the window's end).
 * The audio is silence: the rules do not hear it, and twenty minutes of it encode in a few seconds. Nothing is
 * uploaded (the transport has no network). Every loop has a fixed number of steps. */
#define S30_T0 1794300000000LL

static float g_s30_zero[48000];

static TfClient *s30_client(const char *dir, const char *flags, int yes) {
  seed_config(dir, flags);
  TfClientConfig o = tf__client_default_config();
  o.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "org.example.Share30";
  o.platform = "linux";
  o.api_url = "https://api.test";
  o.storage_dir = dir;
  o.telemetry = 0;
  TfClient *c = tf__client_open(&o);
  if (c && yes && !tf__client_sharing_state(c)) tf__client_sharing_answer(c, 1);
  return c;
}

/* a recording on disk once it has closed: 0 while it is open or when its folder is gone */
typedef struct {
  int ended, completed, rate, has_gap;
  long long samples, audio_ms, take_seq, gap_prev;
  char *takes;
} S30Rec;
static int s30_rec(const char *dir, const char *id, S30Rec *r) {
  memset(r, 0, sizeof *r);
  char *ob = tf_path_join(dir, "outbox"), *p = join3(ob, id, "capture.json");
  TfJson *cap = read_json_file(p);
  free(p);
  const TfJson *e = tf_json_get(cap, "ended"), *co = tf_json_get(cap, "completed");
  r->ended = e && e->type == TJ_BOOL && e->boolean;
  r->completed = co && co->type == TJ_BOOL && co->boolean;
  r->samples = cap ? jnum(cap, "samples") : -1;
  r->audio_ms = cap ? jnum(cap, "duration_ms") : -1;
  tf_json_free(cap);
  p = join3(ob, id, "session.json");
  TfJson *sj = read_json_file(p);
  free(p);
  r->rate = (int)jnum(tf_json_get(sj, "audio"), "input_sample_rate");
  r->take_seq = sj ? jnum(sj, "take_seq") : -1;
  r->has_gap = tf_json_get(sj, "gap_ms_prev") != NULL;
  r->gap_prev = r->has_gap ? jnum(sj, "gap_ms_prev") : -1;
  tf_json_free(sj);
  p = join3(ob, id, "takes.json");
  r->takes = read_text(p, NULL);
  free(p);
  free(ob);
  return r->ended;
}

/* the recordings that closed since the last look are compared with the vector's, in the order they were made */
static void s30_closed(const char *dir, const char *name, char ids[][33], int n_ids, int *done, const TfJson *want_all, int *checked) {
  for (int k = 0; k < n_ids; ++k) {
    S30Rec r;
    if (done[k] || !s30_rec(dir, ids[k], &r)) {
      if (!done[k]) free(r.takes);
      continue;
    }
    done[k] = 1;
    const TfJson *w = want_all && k < want_all->n ? &want_all->kids[k] : NULL;
    if (!w) {
      CHECK(0, "%s: recording %d is not in the vector", name, k);
      free(r.takes);
      continue;
    }
    const TfJson *wg = tf_json_get(w, "gap_ms_prev"), *wt = tf_json_get(w, "takes"), *wd = tf_json_get(w, "deleted"), *wc = tf_json_get(w, "closed_at_cap");
    const int at_cap = wc && wc->type == TJ_BOOL && wc->boolean;
    CHECK(!(wd && wd->type == TJ_BOOL && wd->boolean), "%s: recording %d closed and was kept, the vector says it was deleted", name, k);
    CHECK(r.completed && r.rate == (int)jnum(w, "rate") && r.samples == jnum(w, "samples") && r.audio_ms == jnum(w, "audio_ms") && r.take_seq == jnum(w, "take_seq"),
          "%s: recording %d: completed %d, rate %d, %lld samples (want %lld), %lld ms, take_seq %lld (want %lld)", name, k, r.completed, r.rate, r.samples,
          jnum(w, "samples"), r.audio_ms, r.take_seq, jnum(w, "take_seq"));
    CHECK(wg && wg->type == TJ_NUM ? r.has_gap && r.gap_prev == (long long)wg->num : !r.has_gap, "%s: recording %d: gap_ms_prev %s %lld", name, k,
          r.has_gap ? "is" : "is absent, got", r.gap_prev);
    CHECK((r.samples == (long long)TF_SHARING_RECORDING_MAX_S * r.rate) == at_cap, "%s: recording %d holds %lld samples, closed at the cap: %d in the vector", name, k,
          r.samples, at_cap);
    if (!wt || wt->type != TJ_ARR) {
      CHECK(r.takes == NULL, "%s: recording %d has a take list (%s), want none", name, k, r.takes ? r.takes : "");
    } else {
      TfBuf wb;
      tf_buf_init(&wb);
      tf_buf_puts(&wb, "{\"takes\":[");
      for (int i = 0; i < wt->n; ++i)
        tf_buf_printf(&wb, "%s{\"start_ms\":%lld,\"gap_ms\":%lld}", i ? "," : "", jnum(&wt->kids[i], "start_ms"), jnum(&wt->kids[i], "gap_ms"));
      tf_buf_puts(&wb, "]}");
      CHECK(r.takes && wb.p && !strcmp(r.takes, wb.p), "%s: recording %d: take list %s, want %s", name, k, r.takes ? r.takes : "(none)", wb.p ? wb.p : "");
      tf_buf_free(&wb);
    }
    /* the vector's parts add up to the recording */
    const TfJson *parts = tf_json_get(w, "parts");
    long long sum = 0;
    for (int i = 0; parts && i < parts->n; ++i) sum += jnum(&parts->kids[i], "pre_roll") + jnum(&parts->kids[i], "take") + jnum(&parts->kids[i], "tail");
    CHECK(sum == r.samples, "%s: recording %d: the vector's parts are %lld samples, the recording %lld", name, k, sum, r.samples);
    free(r.takes);
    (*checked)++;
  }
}

static int s30_vector(const char *data, const char *dir) {
  TfJson *v = t30_json(data, "sharing_30.json");
  if (!v) return 2;
  CHECK(jnum(v, "default_window_ms") == TF_SHARING_JOIN_MS_DEFAULT && jnum(v, "cap_ms") == TF_SHARING_RECORDING_MAX_S * 1000LL && jnum(v, "ring_ms") == TF_SHARING_RING_MS &&
            jnum(v, "tail_ms") == TF_SHARING_TAIL_MS && jnum(v, "tail_wait_ms") == TF_SHARING_TAIL_WAIT_MS && jnum(v, "stale_ms") == TF_SHARING_FEED_STALE_MS &&
            jnum(v, "gap_ms_prev_max") == TF_SHARING_GAP_PREV_MAX_MS && jnum(v, "hold_ms") == TF_SHARING_HOLD_MS,
        "the vector's constants differ from the SDK's");
  const TfJson *cases = tf_json_get(v, "cases");
  int ok_cases = 0, n_feeds = 0, n_takes = 0, n_recs = 0;
  const double t0 = tf__now_s();
  for (int i = 0; cases && i < cases->n; ++i) {
    const int f0 = fails;
    const TfJson *cs = &cases->kids[i], *fl = tf_json_get(cs, "flags"), *ev = tf_json_get(cs, "events"), *want = tf_json_get(cs, "recordings");
    const TfJson *on0 = tf_json_get(cs, "sharing_on_at_start");
    const char *name = tf_json_str(tf_json_get(cs, "name"));
    char d[1100];
    snprintf(d, sizeof d, "%s/case%d", dir, i);
    TfBuf fb;
    tf_buf_init(&fb);
    tf_buf_puts(&fb, "{\"contributions\":true");
    for (int k = 0; fl && k < fl->n; ++k) {
      tf_buf_puts(&fb, ",");
      tf_buf_json_str(&fb, fl->kids[k].key);
      tf_buf_puts(&fb, ":");
      tf_json_write(&fb, &fl->kids[k]);
    }
    tf_buf_puts(&fb, "}");
    tf__share_now_ms = S30_T0;
    TfClient *c = s30_client(d, fb.p, on0 && on0->type == TJ_BOOL && on0->boolean);
    tf_buf_free(&fb);
    CHECK(c != NULL, "%s: no client", name);
    if (!c) continue;
    long long max_feed = 1;
    for (int k = 0; ev && k < ev->n; ++k)
      if (jnum(&ev->kids[k], "samples") > max_feed && !strcmp(tf_json_str(tf_json_get(&ev->kids[k], "event")), "feed")) max_feed = jnum(&ev->kids[k], "samples");
    for (int k = 0; ev && k < ev->n; ++k)
      if (jnum(&ev->kids[k], "fed_during") > max_feed) max_feed = jnum(&ev->kids[k], "fed_during");
    float *fed = (float *)calloc((size_t)max_feed, sizeof(float));
    char ids[16][33];
    int n_ids = 0, done[16] = {0}, checked = 0;
    long long last = 0, tail = 0, ring = 0, dropped = 0, ring_n = 0, pre = 0;
    for (int k = 0; ev && fed && k < ev->n; ++k) {
      const TfJson *e = &ev->kids[k], *x = tf_json_get(e, "expect");
      const char *kind = tf_json_str(tf_json_get(e, "event"));
      if (!strcmp(kind, "feed")) {
        last = jnum(e, "wall_ms");
        tf__share_now_ms = S30_T0 + last;
        const int rc = tf_sharing_feed(c, fed, (int)jnum(e, "samples"), (int)jnum(e, "rate"));
        tf__share_feed_stats(c, &tail, &ring, &dropped, &ring_n, NULL, NULL);
        CHECK(rc == 0 && tail == jnum(x, "to_tail") && ring == jnum(x, "to_ring") && dropped == jnum(x, "dropped") && ring_n == jnum(x, "ring_samples"),
              "%s: event %d, feed at %lld ms: tail %lld (want %lld), ring %lld (%lld), dropped %lld (%lld), ring holds %lld (%lld)", name, k, last, tail,
              jnum(x, "to_tail"), ring, jnum(x, "to_ring"), dropped, jnum(x, "dropped"), ring_n, jnum(x, "ring_samples"));
        n_feeds++;
      } else if (!strcmp(kind, "take")) {
        const int rate = (int)jnum(e, "rate");
        const long long n = jnum(e, "samples"), during = jnum(e, "fed_during") > 0 ? jnum(e, "fed_during") : 0;
        const TfJson *xr = tf_json_get(x, "recorded");
        tf__share_now_ms = S30_T0 + jnum(e, "start_wall_ms");
        TfShareSession *ss = tf__sharing_session_begin(c, rate, "built_in");
        tf__clear_error();
        CHECK((ss != NULL) == (xr && xr->type == TJ_BOOL && xr->boolean), "%s: event %d: the take was %srecorded", name, k, ss ? "" : "not ");
        if (ss) {
          tf__share_feed_stats(c, NULL, NULL, NULL, NULL, NULL, &pre);
          CHECK(pre == jnum(x, "pre_roll"), "%s: event %d: pre-roll %lld samples, want %lld", name, k, pre, jnum(x, "pre_roll"));
        }
        for (long long at = 0; ss && at < n; at += rate) {
          tf__sharing_session_push(ss, g_s30_zero, (int)(n - at < rate ? n - at : rate));
          if (at == 0) tk_new_ids(c, ids, &n_ids, 16);
        }
        if (during) {
          tf__share_now_ms = S30_T0 + jnum(e, "start_wall_ms") + 1;
          tf_sharing_feed(c, fed, (int)during, rate);
          tf__share_feed_stats(c, &tail, &ring, &dropped, &ring_n, NULL, NULL);
          CHECK(tail == 0 && ring == 0 && dropped == jnum(x, "fed_during_dropped") && ring_n == 0, "%s: event %d: fed during the take: tail %lld, ring %lld, dropped %lld", name,
                k, tail, ring, dropped);
        }
        tk_new_ids(c, ids, &n_ids, 16);
        last = jnum(e, "end_wall_ms");
        tf__share_now_ms = S30_T0 + last;
        if (ss) tf__sharing_session_finish(ss, 1);
        tk_new_ids(c, ids, &n_ids, 16);
        tf__share_feed_stats(c, NULL, NULL, NULL, &ring_n, NULL, NULL);
        CHECK(ring_n == jnum(x, "ring_samples"), "%s: event %d: the ring holds %lld samples after the take", name, k, ring_n);
        n_takes++;
      } else {
        if (!strcmp(kind, "background")) {
          last = jnum(e, "wall_ms");
          tf__share_now_ms = S30_T0 + last;
          tf__client_background(c);
        } else if (!strcmp(kind, "sharing")) {
          const TfJson *on = tf_json_get(e, "on");
          tf__client_sharing_set(c, on && on->type == TJ_BOOL && on->boolean);
        } else if (!strcmp(kind, "delete")) {
          tf__sharing_deleted(c);
        } else {
          CHECK(0, "%s: unknown event %s", name, kind);
        }
        long long bytes = -1;
        tf__share_feed_stats(c, NULL, NULL, NULL, &ring_n, &bytes, NULL);
        CHECK(ring_n == jnum(x, "ring_samples") && (ring_n > 0 || bytes == 0), "%s: event %d (%s): the ring holds %lld samples in %lld bytes", name, k, kind, ring_n, bytes);
      }
      s30_closed(d, name, ids, n_ids, done, want, &checked);
    }
    /* no take follows: the window and the tail wait pass */
    tf__share_now_ms = S30_T0 + last + TF_SHARING_JOIN_MS_MAX + TF_SHARING_TAIL_WAIT_MS + 1;
    tf__share_window_check(c);
    CHECK(!tf__share_open(c, NULL, NULL, NULL), "%s: a recording is still open after its window and its tail wait", name);
    s30_closed(d, name, ids, n_ids, done, want, &checked);
    /* what never closed was deleted with the queue, as the vector says, and its folder is gone */
    int want_kept = 0;
    for (int k = 0; want && k < want->n; ++k) {
      const TfJson *wd = tf_json_get(&want->kids[k], "deleted");
      const int deleted = wd && wd->type == TJ_BOOL && wd->boolean;
      want_kept += !deleted;
      if (k < n_ids && !done[k]) {
        char *ob = tf_path_join(d, "outbox"), *p = join3(ob, ids[k], "capture.json");
        CHECK(deleted && !tf_file_exists(p), "%s: recording %d never closed (deleted in the vector: %d)", name, k, deleted);
        free(p);
        free(ob);
      }
    }
    CHECK(want && n_ids == want->n && checked == want_kept, "%s: %d recordings made (want %d), %d closed and kept (want %d)", name, n_ids, want ? want->n : -1, checked,
          want_kept);
    n_recs += checked;
    free(fed);
    tf__client_close(c);
    ok_cases += fails == f0;
  }
  const int n_cases = cases ? cases->n : 0;
  CHECK(n_cases == 41, "sharing_30.json has %d cases, 41 expected", n_cases);
  printf("sharing_30.json: %d/%d cases (%d feed calls, %d takes, %d recordings checked when they closed) in %.1f s\n", ok_cases, n_cases, n_feeds, n_takes, n_recs,
         tf__now_s() - t0);
  tf_json_free(v);
  return 0;
}

/* the ring holds one second and no more, whatever is fed and for however long */
static void s30_ring(const char *dir) {
  char d[1100];
  snprintf(d, sizeof d, "%s/ring", dir);
  tf__share_now_ms = S30_T0;
  TfClient *c = s30_client(d, "{\"contributions\":true}", 1);
  if (!c) {
    CHECK(0, "ring: no client");
    return;
  }
  static float five[240000]; /* five seconds at 48 kHz */
  long long n = 0, bytes = 0, worst_n = 0, worst_bytes = 0;
  const long long mem0 = own_memory();
  for (int i = 0; i < 6000; ++i) { /* ten minutes in 100 ms pieces */
    tf__share_now_ms += 100;
    tf_sharing_feed(c, five, 4800, 48000);
    tf__share_feed_stats(c, NULL, NULL, NULL, &n, &bytes, NULL);
    if (n > worst_n) worst_n = n;
    if (bytes > worst_bytes) worst_bytes = bytes;
  }
  const long long grew = own_memory() - mem0;
  CHECK(n == 48000 && worst_n == 48000 && worst_bytes == 48000 * (long long)sizeof(float), "ten minutes fed: the ring holds %lld samples (most %lld) in %lld bytes", n, worst_n,
        worst_bytes);
  CHECK(mem0 == 0 || grew < (4ll << 20), "ten minutes fed grew the process by %lld bytes (the audio fed is 115 MB)", grew);
  tf__share_now_ms += 5000;
  tf_sharing_feed(c, five, 240000, 48000);
  tf__share_feed_stats(c, NULL, NULL, NULL, &n, &bytes, NULL);
  CHECK(n == 48000 && bytes == 48000 * (long long)sizeof(float), "one buffer of five seconds: the ring holds %lld samples in %lld bytes", n, bytes);
  tf__share_now_ms += 100;
  tf_sharing_feed(c, five, 1600, 16000);
  tf__share_feed_stats(c, NULL, NULL, NULL, &n, &bytes, NULL);
  CHECK(n == 1600 && bytes == 16000 * (long long)sizeof(float), "another rate: the ring holds %lld samples in %lld bytes", n, bytes);
  tf__client_background(c);
  tf__share_feed_stats(c, NULL, NULL, NULL, &n, &bytes, NULL);
  CHECK(n == 0 && bytes == 0, "background: the ring holds %lld samples in %lld bytes", n, bytes);
  tf__share_now_ms += 100;
  tf_sharing_feed(c, five, 4800, 48000);
  tf__client_sharing_set(c, 0);
  tf__share_feed_stats(c, NULL, NULL, NULL, &n, &bytes, NULL);
  CHECK(n == 0 && bytes == 0, "sharing off: the ring holds %lld samples in %lld bytes", n, bytes);
  tf__share_now_ms += 100;
  long long dropped = 0;
  tf_sharing_feed(c, five, 4800, 48000);
  tf__share_feed_stats(c, NULL, NULL, &dropped, &n, &bytes, NULL);
  CHECK(dropped == 4800 && n == 0 && bytes == 0, "fed with sharing off: %lld dropped, the ring holds %lld samples in %lld bytes", dropped, n, bytes);
  CHECK(tf_sharing_feed(c, five, -1, 48000) == TF_ERR_INVALID_ARGUMENT && tf_sharing_feed(c, NULL, 10, 48000) == TF_ERR_INVALID_ARGUMENT &&
            tf_sharing_feed(c, five, 10, 100) == TF_ERR_INVALID_ARGUMENT && tf_sharing_feed(NULL, five, 10, 48000) == TF_ERR_INVALID_ARGUMENT,
        "the feed's refusals");
  tf__clear_error();
  tf__client_close(c);
  printf("ring: ten minutes fed in 6000 calls and one buffer of five seconds leave exactly one second (192000 bytes; the process grew %lld KB); another rate replaces it; "
         "background and sharing off free it\n",
         grew >> 10);
}

/* after a take ends, nothing but fed audio goes into its recording */
static void s30_only_fed(const char *dir) {
  static const struct {
    const char *flags;
    int window;
  } kWin[] = {{"{\"contributions\":true,\"share_join_ms\":0}", 0}, {"{\"contributions\":true}", 3000}};
  for (int w = 0; w < 2; ++w) {
    char d[1100], id[33] = "";
    snprintf(d, sizeof d, "%s/onlyfed%d", dir, w);
    tf__share_now_ms = S30_T0;
    TfClient *c = s30_client(d, kWin[w].flags, 1);
    if (!c) {
      CHECK(0, "only fed audio: no client");
      continue;
    }
    TfShareSession *ss = tf__sharing_session_begin(c, 16000, "built_in");
    for (int i = 0; ss && i < 3; ++i) tf__sharing_session_push(ss, g_s30_zero, 16000);
    tf__share_now_ms = S30_T0 + 3000;
    if (ss) tf__sharing_session_finish(ss, 1);
    long long samples = -1;
    /* the recording waits for a tail (1500 ms) and for a take (the window): nothing is added while it waits */
    CHECK(tf__share_open(c, id, NULL, &samples) && samples == 48000, "only fed audio (window %d): the recording is not open with the take's 48000 samples (%lld)", kWin[w].window,
          samples);
    tf__share_now_ms = S30_T0 + 3000 + TF_SHARING_TAIL_WAIT_MS;
    tf__share_window_check(c);
    CHECK(tf__share_open(c, NULL, NULL, &samples) && samples == 48000, "only fed audio (window %d): at 1500 ms the recording closed or grew (%lld samples)", kWin[w].window, samples);
    const int wait = kWin[w].window > TF_SHARING_TAIL_WAIT_MS ? kWin[w].window : TF_SHARING_TAIL_WAIT_MS;
    tf__share_now_ms = S30_T0 + 3000 + wait + 1;
    tf__share_window_check(c);
    S30Rec r;
    const int closed = s30_rec(d, id, &r);
    CHECK(!tf__share_open(c, NULL, NULL, NULL) && closed && r.completed && r.samples == 48000 && r.audio_ms == 3000 && !r.takes,
          "only fed audio (window %d): after the wait the recording holds %lld samples (%lld ms), closed %d", kWin[w].window, r.samples, r.audio_ms, closed);
    free(r.takes);
    /* a feed call after the wait goes to the ring, never to the closed recording */
    long long tail = -1, ring = -1;
    tf__share_now_ms += 100;
    tf_sharing_feed(c, g_s30_zero, 1600, 16000);
    tf__share_feed_stats(c, &tail, &ring, NULL, NULL, NULL, NULL);
    s30_rec(d, id, &r);
    CHECK(tail == 0 && ring == 1600 && r.samples == 48000, "only fed audio (window %d): a late feed: tail %lld, ring %lld, the recording %lld samples", kWin[w].window, tail, ring,
          r.samples);
    free(r.takes);
    tf__client_close(c);
  }
  printf("tail wait: a take of 48000 samples ends, nothing is fed, the wait runs out: the recording holds exactly 48000 samples (join window 0 and 3000 ms); a late "
         "feed goes to the ring\n");
}

static int share30_tests(const char *data, const char *dir) {
  tf_mkdirs(dir);
  tf_http_set_transport(t30_down, NULL);
  const int rc = s30_vector(data, dir);
  fflush(stdout);
  s30_ring(dir);
  s30_only_fed(dir);
  tf__share_now_ms = 0;
  tf_http_set_transport(NULL, NULL);
  if (rc) return rc;
  printf("shared recordings (SDK 3.0.0): %s\n", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}
