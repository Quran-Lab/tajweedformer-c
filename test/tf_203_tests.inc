/* Included by tf_tests.c after the 2.0.1 tests: takes in a row share one recording (SDK 2.0.3).
 *
 *   takes <vectors dir> <work dir>
 *
 * 1. The shared vector takes_join.json: every case replayed through the real recorder (the public session calls,
 *    real libopus, the outbox folder on disk) with the wall clock set by the test; the recordings that form are
 *    read back from disk: how many, each one's input rate, audio ms and take list (none for a single take).
 * 2. A real stream: the self-test clip in three takes with wall gaps of 1.2 s and 0.8 s is ONE recording whose
 *    segments, joined, pass every check of the 2.0.1 stream test (pages, CRCs, sequence, granules, headers once, EOS
 *    last), decode to exactly the takes' samples at 48 kHz, and are byte for byte (and sample for sample) what one
 *    continuous take of the same audio gives with the same segment plan and bitrates. At 16 kHz through the
 *    resampler with the ladder moving between segments, and at 48 kHz with a take that ends exactly on a segment
 *    boundary. ffprobe and ffmpeg's decoders when they are on PATH.
 * 3. A gap over the window: two recordings, each a valid stream of its own.
 * 4. A crash between two takes and inside the second take: one recording recovered with its take list and
 *    uploaded with it; a take whose audio did not survive is left out of the list.
 * 5. The take list call: once, after the last segment, with the exact body and the recording's contributor; a
 *    single take sends none; 404 drops it (the session stays uploaded, one log line per client); a failed
 *    connection keeps it across a restart and sends the same body later; 400 (the issue), 401 (a new token), 410,
 *    503 and 429 (the backoff; the bitrate ladder does not move); the 7-day limit; withdrawal.
 * 6. Withdrawal in the join window and during a joined take; background in the window and during a take;
 *    share_join_ms 0 and 8000 and what counts as a window; the timer's own thread; 256 takes; a take without
 *    audio; two takes within one millisecond; another model revision; the 2 s minimum on the recording's total;
 *    the consent ask between takes; the 20-minute cap with its hold.
 * No test sleeps for a join window: the clock is tf__share_now_ms and the window's end is tf__share_window_check
 * (what the timer calls). */
extern long long tf__share_now_ms;
extern uint32_t tf__share_serial;
void tf__share_window_check(TfClient *c);
void tf__share_timer_poke(TfClient *c);
void tf__share_crash_open(TfClient *c);
int tf__share_join_ms(TfClient *c);
int tf__share_open(TfClient *c, char id[33], int *takes, long long *samples);
int tf__share_lists_waiting(TfClient *c);
int tf__share_passes(TfClient *c);

#define TK_T0 1794300000000LL /* where the tests' wall clock starts (ms) */

static struct {
  int n;
  char line[8][200];
} g_tk_log;
static void tk_log(void *user, int level, const char *message) {
  (void)user;
  (void)level;
  if (g_tk_log.n < 8) snprintf(g_tk_log.line[g_tk_log.n], sizeof g_tk_log.line[0], "%s", message);
  g_tk_log.n++;
}

/* a client whose signed config carries `flags`, with the learner's yes */
static TfClient *takes_client(const char *dir, const char *flags) {
  seed_config(dir, flags);
  TfClientConfig o = tf__client_default_config();
  o.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "org.example.TakesTest";
  o.platform = "linux";
  o.api_url = "https://api.test";
  o.storage_dir = dir;
  o.telemetry = 0;
  o.debug_build = 1;
  o.on_log = tk_log;
  TfClient *c = tf__client_open(&o);
  if (c && !tf__client_sharing_state(c)) tf__client_sharing_answer(c, 1);
  return c;
}
#define TK_ON "{\"contributions\":true}"

static float g_tk_zero[48000];
/* One take through the public calls: it begins at wall ms `start` (the timer had its chance first: the clock
 * decides), pushes n samples of x (silence when x is NULL) in pieces of `chunk`, and ends at wall ms `end`.
 * Returns what finish returned, -1 when the take did not begin. */
static int tk_take(TfClient *c, int rate, const float *x, long long n, int chunk, long long start, long long end, int completed) {
  tf__share_now_ms = TK_T0 + start;
  tf__share_window_check(c);
  TfShareSession *ss = tf__sharing_session_begin(c, rate, "built_in");
  if (!ss) return -1;
  for (long long at = 0; at < n; at += chunk) {
    const int k = (int)(n - at < chunk ? n - at : chunk);
    tf__sharing_session_push(ss, x ? x + at : g_tk_zero, k);
  }
  tf__share_now_ms = TK_T0 + end;
  return tf__sharing_session_finish(ss, completed);
}
/* the window's end at wall ms `at` */
static void tk_check_at(TfClient *c, long long at) {
  tf__share_now_ms = TK_T0 + at;
  tf__share_window_check(c);
}

/* a recording as it is on disk */
typedef struct {
  int found, ended, completed, closed, rate;
  long long duration_ms;
  char route[16];
  char *takes; /* takes.json, NULL when there is none */
} TkRec;
static void tk_rec(const char *dir, const char *id, TkRec *r) {
  memset(r, 0, sizeof *r);
  char *ob = tf_path_join(dir, "outbox");
  char *p = join3(ob, id, "capture.json");
  TfJson *cap = read_json_file(p);
  free(p);
  if (cap) {
    const TfJson *e = tf_json_get(cap, "ended"), *co = tf_json_get(cap, "completed");
    r->found = 1;
    r->ended = e && e->type == TJ_BOOL && e->boolean;
    r->completed = co && co->type == TJ_BOOL && co->boolean;
    r->closed = (int)jnum(cap, "closed");
    r->duration_ms = jnum(cap, "duration_ms");
  }
  tf_json_free(cap);
  p = join3(ob, id, "session.json");
  TfJson *sj = read_json_file(p);
  free(p);
  r->rate = (int)jnum(tf_json_get(sj, "audio"), "input_sample_rate");
  const char *rt = tf_json_str(tf_json_get(tf_json_get(sj, "audio"), "input_route"));
  snprintf(r->route, sizeof r->route, "%s", rt ? rt : "");
  tf_json_free(sj);
  p = join3(ob, id, "takes.json");
  r->takes = read_text(p, NULL);
  free(p);
  free(ob);
}
static int tk_file(const char *dir, const char *id, const char *name) {
  char *ob = tf_path_join(dir, "outbox"), *p = join3(ob, id, name);
  const int yes = tf_file_exists(p);
  free(ob);
  free(p);
  return yes;
}
/* the ids of the outbox's listing that ids[] does not hold yet are appended: a recording appears there when its
 * first take begins */
static void tk_new_ids(TfClient *c, char ids[][33], int *n, int max) {
  char *js = tf__sharing_outbox_json(c);
  TfJson *j = js ? tf_json_parse(js, strlen(js), NULL) : NULL;
  free(js);
  for (int i = 0; j && j->type == TJ_ARR && i < j->n; ++i) {
    const char *id = tf_json_str(tf_json_get(&j->kids[i], "id"));
    int seen = 0;
    for (int k = 0; id && k < *n; ++k) seen |= !strcmp(ids[k], id);
    if (id && !seen && *n < max) snprintf(ids[(*n)++], 33, "%s", id);
  }
  tf_json_free(j);
}
/* a number of the outbox listing's item `id` (-1 when it is not listed) */
static long long tk_item(TfClient *c, const char *id, const char *key) {
  char *js = tf__sharing_outbox_json(c);
  TfJson *j = js ? tf_json_parse(js, strlen(js), NULL) : NULL;
  free(js);
  long long v = -1;
  for (int i = 0; j && j->type == TJ_ARR && i < j->n; ++i) {
    const char *x = tf_json_str(tf_json_get(&j->kids[i], "id"));
    if (x && !strcmp(x, id)) v = jnum(&j->kids[i], key);
  }
  tf_json_free(j);
  return v;
}
/* the segment files of a recording in its outbox folder, joined; the sizes in seg_n[1..] */
static uint8_t *tk_segments(const char *dir, const char *id, size_t *total, size_t seg_n[64], int *segs) {
  char *ob = tf_path_join(dir, "outbox");
  uint8_t *all = NULL;
  size_t n = 0;
  int k = 0;
  for (int s = 1; s < 64; ++s) {
    char name[32];
    snprintf(name, sizeof name, "seg-%05d.opus", s);
    char *p = join3(ob, id, name);
    size_t sn = 0;
    char *d = read_text(p, &sn);
    free(p);
    if (!d) break;
    all = (uint8_t *)realloc(all, n + sn + 1);
    memcpy(all + n, d, sn);
    free(d);
    n += sn;
    seg_n[s] = sn;
    k = s;
  }
  free(ob);
  *total = n;
  *segs = k;
  return all;
}

/* The checks of the 2.0.1 stream test on a joined stream: every page's CRC, sequence numbers from 0 without a
 * hole, one serial, BOS on page 0 only, EOS on the last page only (none when want_eos is 0), no packet continued
 * across pages, OpusHead on page 0 only and OpusTags on page 1 only, granules = 960 x the packets paged so far
 * (the last page trimmed to the encoder's end), and the libopus decode of every packet. */
typedef struct {
  int pages, audio_packets, pre_skip, input_rate, faults;
  uint32_t serial;
  long long last_granule;
  float *pcm; /* the decode, pre-skip dropped and the end trimmed to the last granule */
  long long pcm_n;
} TkStream;
static void tk_stream(const uint8_t *all, size_t all_n, int want_eos, TkStream *o) {
  static OggPage pages[8192];
  static const uint8_t *pk[65536];
  static int pk_len[65536];
  memset(o, 0, sizeof *o);
  int n_pk = 0;
  const int np = parse_pages(all, all_n, pages, 8192, pk, pk_len, 65536, &n_pk);
  if (np < 3) {
    o->faults = 1000;
    return;
  }
  int bad = 0;
  long long g = 0;
  for (int i = 0; i < np; ++i) {
    const OggPage *p = &pages[i];
    bad += !p->crc_ok;
    bad += p->seq != i;
    bad += p->serial != pages[0].serial;
    bad += (i == 0) != !!(p->flags & 0x02);
    bad += (want_eos && i == np - 1) != !!(p->flags & 0x04);
    bad += p->continued;
    const int head = p->first && p->first_len >= 8 && !memcmp(p->first, "OpusHead", 8);
    const int tags = p->first && p->first_len >= 8 && !memcmp(p->first, "OpusTags", 8);
    bad += head != (i == 0);
    bad += tags != (i == 1);
    if (i >= 2) {
      o->audio_packets += p->n_packets;
      g += 960LL * p->n_packets;
      if (i < np - 1 || !want_eos) bad += p->granule != g;
    }
  }
  o->pages = np;
  o->serial = pages[0].serial;
  o->pre_skip = pages[0].first ? pages[0].first[10] | pages[0].first[11] << 8 : -1;
  o->input_rate = pages[0].first ? pages[0].first[12] | pages[0].first[13] << 8 | pages[0].first[14] << 16 | pages[0].first[15] << 24 : -1;
  o->last_granule = pages[np - 1].granule;
  if (want_eos) bad += !(o->last_granule <= g && o->last_granule > g - 960LL * pages[np - 1].n_packets);
  bad += o->audio_packets != n_pk - 2;
  int err = 0;
  OpusDecoder *dec = opus_decoder_create(48000, 1, &err);
  float *out = (float *)malloc(sizeof(float) * (size_t)(960LL * (n_pk > 2 ? n_pk - 2 : 1)));
  long long decoded = 0;
  for (int i = 2; dec && out && i < n_pk; ++i) { /* packets 0 and 1: OpusHead and OpusTags */
    const int k = opus_decode_float(dec, pk[i], pk_len[i], out + decoded, 960, 0);
    if (k != 960) {
      bad++;
      break;
    }
    decoded += k;
  }
  if (dec) opus_decoder_destroy(dec);
  bad += decoded != 960LL * o->audio_packets;
  const long long end = decoded < o->last_granule ? decoded : o->last_granule;
  o->pcm_n = end > o->pre_skip ? end - o->pre_skip : 0;
  o->pcm = (float *)malloc(sizeof(float) * (size_t)(o->pcm_n ? o->pcm_n : 1));
  if (out && o->pcm_n) memcpy(o->pcm, out + o->pre_skip, sizeof(float) * (size_t)o->pcm_n);
  free(out);
  o->faults = bad;
}
/* ffmpeg's decode of the file (s16le, mono, 48 kHz) with the decoder named: the samples and an FNV-1a of the
 * bytes; -1 when ffmpeg fails (no such decoder) */
static long long tk_ffmpeg(const char *file, const char *decoder, uint64_t *hash) {
  char cmd[2400];
  snprintf(cmd, sizeof cmd, "ffmpeg -hide_banner -v error -c:a %s -i \"%s\" -f s16le -ac 1 -ar 48000 -", decoder, file);
  FILE *f = POPEN_R(cmd);
  if (!f) return -1;
  static char buf[65536];
  long long total = 0;
  uint64_t h = FNV0;
  size_t k;
  while ((k = fread(buf, 1, sizeof buf, f)) > 0) {
    total += (long long)k;
    h = fnv1a(h, buf, k);
  }
  const int st = PCLOSE(f);
  *hash = h;
  return st == 0 ? total / 2 : -1;
}

/* ================================================================== 1. the shared vector */
static int takes_vector(const char *vdir, const char *dir) {
  char *vp = tf_path_join(vdir, "takes_join.json");
  TfJson *v = read_json_file(vp);
  free(vp);
  if (!v) return 2;
  CHECK(jnum(v, "default_window_ms") == TF_SHARING_JOIN_MS_DEFAULT && jnum(v, "cap_ms") == TF_SHARING_RECORDING_MAX_S * 1000LL,
        "the vector's constants: window %lld, cap %lld", jnum(v, "default_window_ms"), jnum(v, "cap_ms"));
  const TfJson *cases = tf_json_get(v, "cases");
  int ok_cases = 0, n_events = 0;
  const double t0 = tf__now_s();
  for (int i = 0; cases && i < cases->n; ++i) {
    const int f0 = fails;
    const TfJson *cs = &cases->kids[i], *fl = tf_json_get(cs, "flags"), *ev = tf_json_get(cs, "events"), *ex = tf_json_get(cs, "expect");
    const char *name = tf_json_str(tf_json_get(cs, "name"));
    char d[1100];
    snprintf(d, sizeof d, "%s/vector%d", dir, i);
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
    TfClient *c = takes_client(d, fb.p);
    tf_buf_free(&fb);
    CHECK(c != NULL, "%s: no client", name);
    if (!c) continue;
    char ids[16][33];
    int n_ids = 0;
    long long last = 0;
    for (int k = 0; ev && k < ev->n; ++k) {
      const TfJson *e = &ev->kids[k];
      const char *kind = tf_json_str(tf_json_get(e, "event"));
      n_events++;
      if (kind && !strcmp(kind, "take")) {
        const int rate = (int)jnum(e, "rate");
        last = jnum(e, "end_wall_ms");
        /* silence: the join rules do not hear the audio, and 20 minutes of it encode in a few seconds */
        const int got = tk_take(c, rate, NULL, jnum(e, "samples"), rate, jnum(e, "start_wall_ms"), last, 1);
        CHECK(got >= 0, "%s: take %d did not begin: %s", name, k, tf_last_error() ? tf_last_error() : "");
        tk_new_ids(c, ids, &n_ids, 16);
      } else if (kind && !strcmp(kind, "background")) {
        last = jnum(e, "wall_ms");
        tf__share_now_ms = TK_T0 + last;
        tf__client_background(c);
      } else {
        CHECK(0, "%s: unknown event %s", name, kind ? kind : "(null)");
      }
    }
    /* no take follows: the last recording's window ends */
    tk_check_at(c, last + TF_SHARING_JOIN_MS_MAX + 1);
    CHECK(!tf__share_open(c, NULL, NULL, NULL), "%s: a recording is still open after its window", name);
    CHECK(ex && n_ids == ex->n, "%s: %d recordings, want %d", name, n_ids, ex ? ex->n : -1);
    for (int k = 0; ex && k < ex->n && k < n_ids; ++k) {
      const TfJson *want = &ex->kids[k], *wt = tf_json_get(want, "takes");
      TkRec r;
      tk_rec(d, ids[k], &r);
      CHECK(r.found && r.ended && r.completed, "%s: recording %d found %d ended %d completed %d", name, k, r.found, r.ended, r.completed);
      CHECK(r.rate == (int)jnum(want, "rate"), "%s: recording %d rate %d, want %lld", name, k, r.rate, jnum(want, "rate"));
      CHECK(r.duration_ms == jnum(want, "audio_ms"), "%s: recording %d audio %lld ms, want %lld", name, k, r.duration_ms, jnum(want, "audio_ms"));
      if (!wt || wt->type != TJ_ARR) {
        CHECK(r.takes == NULL, "%s: recording %d has a take list (%s), want none", name, k, r.takes ? r.takes : "");
        CHECK(tk_item(c, ids[k], "takes") == 1, "%s: recording %d lists %lld takes, want 1", name, k, tk_item(c, ids[k], "takes"));
      } else {
        TfBuf wb;
        tf_buf_init(&wb);
        tf_buf_puts(&wb, "{\"takes\":[");
        for (int t = 0; t < wt->n; ++t)
          tf_buf_printf(&wb, "%s{\"start_ms\":%lld,\"gap_ms\":%lld}", t ? "," : "", jnum(&wt->kids[t], "start_ms"), jnum(&wt->kids[t], "gap_ms"));
        tf_buf_puts(&wb, "]}");
        CHECK(r.takes && !strcmp(r.takes, wb.p), "%s: recording %d takes %s, want %s", name, k, r.takes ? r.takes : "(none)", wb.p);
        CHECK(tk_item(c, ids[k], "takes") == wt->n, "%s: recording %d lists %lld takes, want %d", name, k, tk_item(c, ids[k], "takes"), wt->n);
        tf_buf_free(&wb);
      }
      free(r.takes);
    }
    tf__client_close(c);
    ok_cases += fails == f0;
  }
  printf("takes_join.json through the recorder: %d/%d cases (%d events, %.1f s)\n", ok_cases, cases ? cases->n : 0, n_events, tf__now_s() - t0);
  CHECK(cases && cases->n >= 10 && ok_cases == cases->n, "the shared vector: %d of %d cases", ok_cases, cases ? cases->n : 0);
  tf_json_free(v);
  return 0;
}

/* ================================================================== 2. a real stream in three takes */
typedef struct {
  long long at; /* input samples of the whole audio pushed so far */
  int event, times;
} TkLadder;
/* pushes x[from .. from + n) in pieces of `chunk`. The ladder is moved at fixed positions of the whole audio, the
 * same in a three-take run and a one-take run, so both open every segment at the same bitrate. */
static void tk_feed(TfClient *c, TfShareSession *ss, const float *x, long long from, long long n, int chunk, const TkLadder *ev, int n_ev) {
  long long at = from;
  const long long end = from + n;
  while (at < end) {
    long long stop = at + chunk < end ? at + chunk : end;
    for (int i = 0; i < n_ev; ++i)
      if (ev[i].at > at && ev[i].at < stop) stop = ev[i].at;
    tf__sharing_session_push(ss, x + at, (int)(stop - at));
    at = stop;
    for (int i = 0; i < n_ev; ++i)
      if (ev[i].at == at)
        for (int k = 0; k < ev[i].times; ++k) tf__ladder_event(c, ev[i].event, 0);
  }
}

static int tk_three_takes(const char *dir, const char *label, int rate, const float *x, const long long n[3], const TkLadder *ev, int n_ev,
                          const int *want_br, int want_segs) {
  const int f0 = fails;
  const long long total = n[0] + n[1] + n[2], total48 = total * 48000 / rate;
  char da[1100], db[1100];
  snprintf(da, sizeof da, "%s/%s_takes", dir, label);
  snprintf(db, sizeof db, "%s/%s_one", dir, label);
  tf__share_serial = 0x20300a11u;
  /* A: three takes, wall gaps 1.2 s and 0.8 s, the default window */
  TfClient *c = takes_client(da, TK_ON);
  if (!c) return 2;
  char id[33] = "", id2[33] = "";
  long long clock = 0, from = 0;
  const int gaps[3] = {0, 1200, 800};
  char want_takes[200];
  snprintf(want_takes, sizeof want_takes, "{\"takes\":[{\"start_ms\":0,\"gap_ms\":0},{\"start_ms\":%lld,\"gap_ms\":1200},{\"start_ms\":%lld,\"gap_ms\":800}]}",
           n[0] * 1000 / rate, (n[0] + n[1]) * 1000 / rate);
  for (int k = 0; k < 3; ++k) {
    clock += gaps[k];
    tk_check_at(c, clock); /* the timer may fire any time: inside the window it closes nothing */
    TfShareSession *ss = tf__sharing_session_begin(c, rate, k == 1 ? "usb" : "built_in");
    CHECK(ss != NULL, "%s: take %d: %s", label, k + 1, tf_last_error() ? tf_last_error() : "");
    if (!ss) return 1;
    tk_feed(c, ss, x, from, n[k], rate / 25, ev, n_ev); /* 40 ms pieces, as a microphone gives them */
    from += n[k];
    clock += n[k] * 1000 / rate;
    tf__share_now_ms = TK_T0 + clock;
    const int ms = tf__sharing_session_finish(ss, 1);
    CHECK(ms == (int)((double)n[k] / rate * 1000 + 0.5), "%s: take %d finish returned %d ms", label, k + 1, ms);
    int takes = 0;
    long long samples = 0;
    CHECK(tf__share_open(c, id2, &takes, &samples) && takes == k + 1 && samples == from, "%s: after take %d: %d takes, %lld samples open", label,
          k + 1, takes, samples);
    if (k == 0) snprintf(id, sizeof id, "%s", id2);
    CHECK(!strcmp(id, id2) && tf__sharing_outbox_pending(c) == 1, "%s: take %d is in another recording", label, k + 1);
  }
  tk_check_at(c, clock + 3000);
  CHECK(tf__share_open(c, NULL, NULL, NULL), "%s: closed at exactly the window", label);
  tk_check_at(c, clock + 3001);
  CHECK(!tf__share_open(c, NULL, NULL, NULL), "%s: still open after the window", label);
  TkRec r;
  tk_rec(da, id, &r);
  CHECK(r.found && r.ended && r.completed && r.closed == want_segs && r.duration_ms == total * 1000 / rate && r.rate == rate,
        "%s: capture ended %d completed %d segments %d duration %lld rate %d", label, r.ended, r.completed, r.closed, r.duration_ms, r.rate);
  CHECK(!strcmp(r.route, "built_in"), "%s: the session JSON's route is %s, want the first take's", label, r.route);
  CHECK(r.takes && !strcmp(r.takes, want_takes), "%s: takes.json %s, want %s", label, r.takes ? r.takes : "(none)", want_takes);
  free(r.takes);
  CHECK(tk_item(c, id, "takes") == 3 && tk_item(c, id, "segments") == want_segs, "%s: the outbox item: takes %lld, segments %lld", label,
        tk_item(c, id, "takes"), tk_item(c, id, "segments"));
  char *ob = tf_path_join(da, "outbox");
  for (int s = 1; s <= want_segs; ++s)
    CHECK(marker_bitrate(ob, id, s) == want_br[s - 1], "%s: segment %d at %d bit/s, want %d", label, s, marker_bitrate(ob, id, s), want_br[s - 1]);
  free(ob);
  size_t a_n = 0, a_seg[64] = {0};
  int a_segs = 0;
  uint8_t *a = tk_segments(da, id, &a_n, a_seg, &a_segs);
  tf__client_close(c);

  /* B: the same audio as one continuous take (pieces of another size), the same ladder moves, the same serial */
  c = takes_client(db, "{\"contributions\":true,\"share_join_ms\":0}");
  if (!c) return 2;
  TfShareSession *ss = tf__sharing_session_begin(c, rate, "built_in");
  CHECK(ss != NULL, "%s: the continuous take: %s", label, tf_last_error() ? tf_last_error() : "");
  if (!ss) return 1;
  char idb[33] = "";
  only_session(c, idb);
  tk_feed(c, ss, x, 0, total, rate / 16 + 7, ev, n_ev);
  share_finish(c, ss, 1);
  size_t b_n = 0, b_seg[64] = {0};
  int b_segs = 0;
  uint8_t *b = tk_segments(db, idb, &b_n, b_seg, &b_segs);
  tf__client_close(c);
  tf__share_serial = 0;

  /* bytes: every segment file, and so the whole stream */
  CHECK(a_segs == want_segs && b_segs == want_segs, "%s: %d and %d segment files, want %d", label, a_segs, b_segs, want_segs);
  size_t at = 0;
  int same_segs = 0;
  for (int s = 1; s <= want_segs && s <= a_segs && s <= b_segs; ++s) {
    const int same = a_seg[s] == b_seg[s] && !memcmp(a + at, b + at, a_seg[s]);
    CHECK(same, "%s: segment %d differs from the continuous encode (%llu bytes, %llu)", label, s, (unsigned long long)a_seg[s],
          (unsigned long long)b_seg[s]);
    same_segs += same;
    at += a_seg[s];
  }
  CHECK(a && b && a_n == b_n && !memcmp(a, b, a_n), "%s: the joined stream differs from the continuous encode (%llu bytes, %llu)", label,
        (unsigned long long)a_n, (unsigned long long)b_n);
  /* the stream */
  TkStream sa, sb;
  tk_stream(a, a_n, 1, &sa);
  tk_stream(b, b_n, 1, &sb);
  CHECK(sa.faults == 0, "%s: the joined stream: %d page faults (sequence, serial, BOS, EOS, CRC, headers, granules, decode)", label, sa.faults);
  CHECK(sa.serial == 0x20300a11u && sa.input_rate == rate, "%s: serial %08x, OpusHead input rate %d", label, sa.serial, sa.input_rate);
  CHECK(sa.last_granule == sa.pre_skip + total48, "%s: last granule %lld, want %lld", label, sa.last_granule, sa.pre_skip + total48);
  CHECK(sa.pcm_n == total48, "%s: libopus decodes %lld samples, want the takes' %lld", label, sa.pcm_n, total48);
  CHECK(sb.faults == 0 && sb.pcm_n == sa.pcm_n && !memcmp(sa.pcm, sb.pcm, sizeof(float) * (size_t)sa.pcm_n),
        "%s: the decoded PCM differs from the continuous encode's", label);
  printf("three takes, %s: %d + %d + %d ms in ONE recording, %d segments (", label, (int)(n[0] * 1000 / rate), (int)(n[1] * 1000 / rate),
         (int)(n[2] * 1000 / rate), a_segs);
  for (int s = 1; s <= a_segs; ++s) printf("%s%llu", s > 1 ? "+" : "", (unsigned long long)a_seg[s]);
  printf(" bytes), %d pages, %d packets, %lld samples decoded (the takes' %lld)\n", sa.pages, sa.audio_packets, sa.pcm_n, total48);
  printf("  against one continuous take: %d/%d segment files byte-identical, %llu bytes; decoded PCM %s\n", same_segs, want_segs,
         (unsigned long long)b_n, sb.pcm_n == sa.pcm_n && !memcmp(sa.pcm, sb.pcm, sizeof(float) * (size_t)sa.pcm_n) ? "identical" : "DIFFERENT");
  if (have_tool("ffmpeg") && have_tool("ffprobe")) {
    char fa[1200], fbp[1200];
    snprintf(fa, sizeof fa, "%s/%s_takes.opus", dir, label);
    snprintf(fbp, sizeof fbp, "%s/%s_one.opus", dir, label);
    tf_write_file(fa, a, a_n);
    tf_write_file(fbp, b, b_n);
    const double dur = ffprobe_duration(fa);
    uint64_t hn = 0, hl = 0, hbn = 0, hbl = 0;
    const long long nat = tk_ffmpeg(fa, "opus", &hn), lib = tk_ffmpeg(fa, "libopus", &hl);
    const long long natb = tk_ffmpeg(fbp, "opus", &hbn), libb = tk_ffmpeg(fbp, "libopus", &hbl);
    CHECK(dur > 0 && fabs(dur - (double)total / rate) <= 0.020, "%s: ffprobe duration %.6f, want %.6f", label, dur, (double)total / rate);
    CHECK(nat == total48 && natb == nat && hn == hbn, "%s: ffmpeg's opus decoder: %lld samples (continuous %lld)", label, nat, natb);
    CHECK((lib == total48 && libb == lib && hl == hbl) || lib == -1, "%s: ffmpeg's libopus decoder: %lld samples (continuous %lld)", label, lib, libb);
    printf("  ffprobe %.6f s (%+.1f ms); ffmpeg opus %lld and libopus %lld samples, the same PCM as the continuous encode's\n", dur,
           (dur - (double)total / rate) * 1000, nat, lib);
  } else {
    printf("  ffmpeg / ffprobe not on PATH: their checks skipped\n");
  }
  free(sa.pcm);
  free(sb.pcm);
  free(a);
  free(b);
  return fails == f0 ? 0 : 1;
}

/* ================================================================== 3. a gap over the window */
static void tk_two_recordings(const char *dir, const float *clip16) {
  char d[1100];
  snprintf(d, sizeof d, "%s/gap", dir);
  TfClient *c = takes_client(d, TK_ON);
  if (!c) return;
  char ids[4][33];
  int n_ids = 0;
  tk_take(c, 16000, clip16, 80000, 640, 0, 5000, 1);
  tk_new_ids(c, ids, &n_ids, 4);
  tk_take(c, 16000, clip16 + 80000, 48000, 640, 8001, 11001, 1); /* 3001 ms after the first: over the window */
  tk_new_ids(c, ids, &n_ids, 4);
  tk_check_at(c, 11001 + 3001);
  CHECK(n_ids == 2 && !tf__share_open(c, NULL, NULL, NULL), "a gap over the window: %d recordings", n_ids);
  const long long want[2] = {80000, 48000};
  uint32_t serial[2] = {0, 0};
  for (int k = 0; k < 2 && k < n_ids; ++k) {
    TkRec r;
    tk_rec(d, ids[k], &r);
    size_t n = 0, seg[64] = {0};
    int segs = 0;
    uint8_t *all = tk_segments(d, ids[k], &n, seg, &segs);
    TkStream st;
    tk_stream(all, n, 1, &st);
    CHECK(r.ended && r.completed && !r.takes && r.duration_ms == want[k] / 16, "recording %d of 2: ended %d, completed %d, %lld ms, takes %s", k + 1,
          r.ended, r.completed, r.duration_ms, r.takes ? r.takes : "none");
    CHECK(all && st.faults == 0 && st.pcm_n == want[k] * 3 && st.input_rate == 16000, "recording %d of 2: %d page faults, %lld samples decoded (want %lld)",
          k + 1, st.faults, st.pcm_n, want[k] * 3);
    serial[k] = st.serial;
    free(r.takes);
    free(st.pcm);
    free(all);
  }
  CHECK(serial[0] != serial[1], "the two recordings share a serial");
  printf("a gap of 3001 ms: 2 recordings, each a valid stream with its own headers and EOS (5000 and 3000 ms decoded exactly)\n");
  tf__client_close(c);
}

/* ================================================================== 4 and 5: recovery and the take list call */
/* a closed recording of two takes at 48 kHz: 2.5 s, a pause of 500 ms, 1.7 s; clock is where the test's wall clock
 * stands (moved on) */
static const char kTwoTakes[] = "{\"takes\":[{\"start_ms\":0,\"gap_ms\":0},{\"start_ms\":2500,\"gap_ms\":500}]}";
static void tk_two_takes(TfClient *c, const float *x, long long *clock) {
  const long long t = *clock;
  tk_take(c, 48000, x, 120000, 960, t, t + 2500, 1);
  tk_take(c, 48000, x + 120000, 81600, 960, t + 3000, t + 4700, 1);
  tk_check_at(c, t + 4700 + 3001);
  *clock = t + 20000;
}
static int tk_shared_sessions(TfClient *c) { return status_int(c, "sharedSessions"); }
/* One full upload pass that started after this call (the uploader's thread is started if need be); returns the
 * sessions still pending. tf__sharing_outbox_wait does the same but, with an idle uploader, only returns after its
 * timeout: these tests make dozens of passes. */
static int tk_pass(TfClient *c) {
  const int p0 = tf__share_passes(c);
  for (int i = 0; i < 4000 && tf__share_passes(c) < p0 + 2; ++i) {
    tf__sharing_outbox_resume(c);
    SLEEP_MS(5);
  }
  return tf__sharing_outbox_pending(c);
}
/* passes until the stub has seen `calls` take list calls (a few at most) */
static void tk_wait_calls(TfClient *c, int calls) {
  for (int i = 0; i < 6 && g_st.takes_calls < calls; ++i) tk_pass(c);
}

static void tk_recovery(const char *dir, const float *x) {
  char d[1100];
  /* a: the process dies between two takes (in the window after the second take) */
  stub_reset();
  g_st.net_down = 1;
  snprintf(d, sizeof d, "%s/crash_between", dir);
  TfClient *c = takes_client(d, TK_ON);
  char id[33] = "";
  tk_take(c, 48000, x, 168000, 960, 0, 3500, 1);
  tk_take(c, 48000, x + 168000, 144000, 960, 4700, 7700, 1);
  CHECK(tf__share_open(c, id, NULL, NULL), "crash between takes: the recording is not open in its window");
  TkRec r;
  tk_rec(d, id, &r);
  static const char want_a[] = "{\"takes\":[{\"start_ms\":0,\"gap_ms\":0},{\"start_ms\":3500,\"gap_ms\":1200}]}";
  CHECK(r.found && !r.ended && r.takes && !strcmp(r.takes, want_a), "crash between takes: on disk ended %d, takes %s", r.ended, r.takes ? r.takes : "(none)");
  free(r.takes);
  tf__share_crash_open(c);
  tf__client_close(c);
  g_st.net_down = 0;
  c = takes_client(d, TK_ON);
  int left = tk_pass(c);
  tk_wait_calls(c, 1);
  int segs = 0;
  CHECK(left == 0 && g_st.n_s == 1 && !strcmp(g_st.s[0].id, id) && joined_ok(&g_st.s[0], 0, &segs),
        "crash between takes: one recording recovered and uploaded as one stream without EOS (%d left, %d sessions)", left, g_st.n_s);
  CHECK(g_st.takes_calls == 1 && g_st.takes_body[0] && !strcmp(g_st.takes_body[0], want_a) && !strcmp(g_st.takes_id[0], id) &&
            g_st.takes_after_puts[0] == g_st.puts && !g_st.bad_takes,
        "crash between takes: %d take list calls, body %s", g_st.takes_calls, g_st.takes_body[0] ? g_st.takes_body[0] : "(none)");
  CHECK(tk_shared_sessions(c) == 1 && tf__share_lists_waiting(c) == 0, "crash between takes: %d sessions counted", tk_shared_sessions(c));
  printf("crash between two takes: one recording recovered (%d segments, no EOS), uploaded, then its take list %s\n", segs,
         g_st.takes_body[0] ? g_st.takes_body[0] : "(none)");
  tf__client_close(c);

  /* b: the process dies inside the second take, 2 s into it: the take's start is before what was kept */
  stub_reset();
  g_st.net_down = 1;
  snprintf(d, sizeof d, "%s/crash_inside", dir);
  c = takes_client(d, TK_ON);
  tk_take(c, 48000, x, 168000, 960, 0, 3500, 1);
  tk_check_at(c, 4300);
  TfShareSession *ss = tf__sharing_session_begin(c, 48000, "built_in");
  for (int i = 0; ss && i < 100; ++i) tf__sharing_session_push(ss, x + 168000 + (size_t)i * 960, 960);
  tf__share_open(c, id, NULL, NULL);
  tf__share_crash(ss);
  tf__client_close(c);
  g_st.net_down = 0;
  c = takes_client(d, TK_ON);
  left = tk_pass(c);
  tk_wait_calls(c, 1);
  static const char want_b[] = "{\"takes\":[{\"start_ms\":0,\"gap_ms\":0},{\"start_ms\":3500,\"gap_ms\":800}]}";
  CHECK(left == 0 && g_st.n_s == 1 && !strcmp(g_st.s[0].id, id) && joined_ok(&g_st.s[0], 0, &segs) && segs == 5,
        "crash inside the second take: recovered and uploaded (%d left, %d sessions, %d segments)", left, g_st.n_s, segs);
  CHECK(g_st.takes_calls == 1 && g_st.takes_body[0] && !strcmp(g_st.takes_body[0], want_b) && g_st.takes_after_puts[0] == g_st.puts,
        "crash inside the second take: %d take list calls, body %s", g_st.takes_calls, g_st.takes_body[0] ? g_st.takes_body[0] : "(none)");
  printf("crash 2 s into the second take: recovered at its last whole page (%d segments), uploaded with %s\n", segs,
         g_st.takes_body[0] ? g_st.takes_body[0] : "(none)");
  tf__client_close(c);

  /* c: the process dies 0.2 s into the second take: none of that take's audio reached a whole page, so the take is
   * left out, one take is left, and there is no list */
  stub_reset();
  g_st.net_down = 1;
  snprintf(d, sizeof d, "%s/crash_early", dir);
  c = takes_client(d, TK_ON);
  tk_take(c, 48000, x, 168000, 960, 0, 3500, 1);
  tk_check_at(c, 4300);
  ss = tf__sharing_session_begin(c, 48000, "built_in");
  for (int i = 0; ss && i < 10; ++i) tf__sharing_session_push(ss, x + 168000 + (size_t)i * 960, 960);
  tf__share_open(c, id, NULL, NULL);
  CHECK(tk_file(d, id, "takes.json"), "crash early in the second take: no takes.json before the crash");
  tf__share_crash(ss);
  tf__client_close(c);
  g_st.net_down = 0;
  c = takes_client(d, TK_ON);
  left = tk_pass(c);
  tk_pass(c);
  CHECK(left == 0 && g_st.n_s == 1 && joined_ok(&g_st.s[0], 0, &segs) && segs == 3 && g_st.takes_calls == 0 && tk_shared_sessions(c) == 1,
        "crash early in the second take: %d left, %d segments, %d take list calls", left, segs, g_st.takes_calls);
  printf("crash 0.2 s into the second take: %d segments recovered, the lost take left out, no take list sent\n", segs);
  tf__client_close(c);
}

static void tk_list_call(const char *dir, const float *x) {
  char d[1100];
  long long clock = 0;
  int segs = 0;
  /* a: sent once, after the last segment, with the exact body; a single take sends none */
  stub_reset();
  snprintf(d, sizeof d, "%s/call", dir);
  TfClient *c = takes_client(d, TK_ON);
  tk_two_takes(c, x, &clock);
  int left = tk_pass(c);
  tk_wait_calls(c, 1);
  CHECK(left == 0 && g_st.n_s == 1 && joined_ok(&g_st.s[0], 1, &segs) && segs == 5, "the joined recording uploaded as one stream with EOS (%d left, %d segments)",
        left, segs);
  CHECK(g_st.takes_calls == 1 && g_st.takes_body[0] && !strcmp(g_st.takes_body[0], kTwoTakes), "the take list call: %d calls, body %s", g_st.takes_calls,
        g_st.takes_body[0] ? g_st.takes_body[0] : "(none)");
  CHECK(!strcmp(g_st.takes_id[0], g_st.s[0].id) && is_hex32(g_st.takes_contributor[0]) && !strcmp(g_st.takes_contributor[0], g_st.start_contributor) &&
            !g_st.bad_takes,
        "the take list call's id, contributor or headers");
  CHECK(g_st.takes_after_puts[0] == segs && g_st.puts == segs, "the take list went after %d of %d segment PUTs", g_st.takes_after_puts[0], segs);
  CHECK(tf__sharing_outbox_pending(c) == 0 && tf__share_lists_waiting(c) == 0 && tk_shared_sessions(c) == 1 && !tk_file(d, g_st.s[0].id, "item.json"),
        "after the take list: %d pending, %d lists waiting, %d sessions counted", tf__sharing_outbox_pending(c), tf__share_lists_waiting(c),
        tk_shared_sessions(c));
  tk_pass(c);
  CHECK(g_st.takes_calls == 1 && g_st.starts == 1, "the take list was sent %d times", g_st.takes_calls);
  tk_take(c, 48000, x, 120000, 960, clock, clock + 2500, 1);
  tk_check_at(c, clock + 2500 + 3001);
  clock += 20000;
  left = tk_pass(c);
  tk_pass(c);
  CHECK(left == 0 && g_st.n_s == 2 && g_st.takes_calls == 1 && tk_shared_sessions(c) == 2, "a single take: %d take list calls, %d sessions counted",
        g_st.takes_calls, tk_shared_sessions(c));
  printf("take list call: once, after the last of %d segment PUTs, body %s; a single take sends none\n", segs, kTwoTakes);
  tf__client_close(c);

  /* b: 404 (a server without the route): the list is dropped, the session stays uploaded, one log line per client */
  stub_reset();
  g_st.takes_status[0] = g_st.takes_status[1] = 404;
  g_tk_log.n = 0;
  snprintf(d, sizeof d, "%s/call404", dir);
  c = takes_client(d, TK_ON);
  for (int k = 0; k < 2; ++k) {
    tk_two_takes(c, x, &clock);
    left = tk_pass(c);
    tk_wait_calls(c, k + 1);
    CHECK(left == 0 && g_st.takes_calls == k + 1 && tf__share_lists_waiting(c) == 0 && tk_shared_sessions(c) == k + 1,
          "404, recording %d: %d left, %d calls, %d lists waiting, %d sessions counted", k + 1, left, g_st.takes_calls, tf__share_lists_waiting(c),
          tk_shared_sessions(c));
  }
  tf__now_override = (long long)time(NULL) + 7200;
  tk_pass(c);
  tf__now_override = 0;
  int lines = 0;
  for (int i = 0; i < g_tk_log.n && i < 8; ++i) lines += strstr(g_tk_log.line[i], "404") != NULL;
  CHECK(g_st.takes_calls == 2 && lines == 1 && g_st.n_s == 2 && joined_ok(&g_st.s[1], 1, &segs), "404: %d calls in all, %d log lines", g_st.takes_calls, lines);
  printf("take list 404: dropped without a retry, both sessions uploaded and counted, logged once (%s)\n", g_tk_log.n ? g_tk_log.line[0] : "");
  tf__client_close(c);

  /* c: no connection for the call: the session is uploaded and counted, the list waits, survives a restart, and
   * goes later with the same body */
  stub_reset();
  g_st.takes_net_down = 1;
  snprintf(d, sizeof d, "%s/call_offline", dir);
  c = takes_client(d, TK_ON);
  tk_two_takes(c, x, &clock);
  left = tk_pass(c);
  tk_wait_calls(c, 1);
  char id[33];
  snprintf(id, sizeof id, "%s", g_st.n_s ? g_st.s[0].id : "");
  CHECK(left == 0 && g_st.takes_calls == 1 && tf__share_lists_waiting(c) == 1 && tk_shared_sessions(c) == 1 && tf__sharing_bitrate(c) == 192000,
        "no connection for the take list: %d left, %d calls, %d waiting, %d counted, ladder %d", left, g_st.takes_calls, tf__share_lists_waiting(c),
        tk_shared_sessions(c), tf__sharing_bitrate(c));
  CHECK(tk_file(d, id, "item.json") && tk_file(d, id, "takes.json") && !tk_file(d, id, "session.json") && !tk_file(d, id, "capture.json") &&
            !tk_file(d, id, "seg-00001.json"),
        "the waiting list's folder holds something else than item.json and takes.json");
  tf__client_close(c);
  c = takes_client(d, TK_ON); /* a restart */
  left = tk_pass(c);
  CHECK(left == 0 && tf__share_lists_waiting(c) == 1 && g_st.takes_calls == 1 && g_st.starts == 1 && tk_shared_sessions(c) == 1,
        "after a restart: %d waiting, %d calls (the backoff), %d starts", tf__share_lists_waiting(c), g_st.takes_calls, g_st.starts);
  tf__now_override = (long long)time(NULL) + 3600;
  tk_wait_calls(c, 2);
  tf__now_override = 0;
  CHECK(g_st.takes_calls == 2 && g_st.takes_body[1] && !strcmp(g_st.takes_body[1], kTwoTakes) && !strcmp(g_st.takes_body[0], g_st.takes_body[1]) &&
            !strcmp(g_st.takes_contributor[0], g_st.takes_contributor[1]) && tf__share_lists_waiting(c) == 0 && !tk_file(d, id, "item.json"),
        "the retry: %d calls, body %s", g_st.takes_calls, g_st.takes_body[1] ? g_st.takes_body[1] : "(none)");
  printf("take list with no connection: kept across a restart, sent again with the same body (%d calls)\n", g_st.takes_calls);
  tf__client_close(c);

  /* d: 400 keeps it (3.0: it stays pending and is sent again on the outbox's backoff; 2.0.3 dropped it) and
   * queues the bad-request issue once, however often the server refuses; 410 drops it */
  for (int k = 0; k < 2; ++k) {
    stub_reset();
    g_st.takes_status[0] = k ? 410 : 400;
    if (!k) g_st.takes_status[1] = 400; /* refused twice, taken the third time */
    snprintf(d, sizeof d, "%s/call%d", dir, k ? 410 : 400);
    c = takes_client(d, TK_ON);
    tk_two_takes(c, x, &clock);
    left = tk_pass(c);
    tk_wait_calls(c, 1);
    const int waiting1 = tf__share_lists_waiting(c), calls1 = g_st.takes_calls;
    for (int round = 1; round <= 3; ++round) { /* the backoff passes: the list is sent again */
      tf__now_override = (long long)time(NULL) + round * 7200;
      tk_pass(c);
    }
    tf__now_override = 0;
    char *ip = tf_path_join(d, "issues.json"), *issues = read_text(ip, NULL);
    int issue = 0;
    for (const char *at = issues; at && (at = strstr(at, "\"code\":\"bad_request:takes\"")) != NULL; ++at) issue++; /* queued issues, not the day's sent keys */
    CHECK(calls1 == 1 && tk_shared_sessions(c) == 1 && tf__share_lists_waiting(c) == 0 &&
              (k ? left == 0 && waiting1 == 0 && g_st.takes_calls == 1 && issue == 0 : waiting1 == 1 && g_st.takes_calls == 3 && issue == 1),
          "%d: %d left, %d waiting after the first answer, %d calls, %d waiting at the end, %d counted, the issue %d times", k ? 410 : 400, left, waiting1,
          g_st.takes_calls, tf__share_lists_waiting(c), tk_shared_sessions(c), issue);
    free(ip);
    free(issues);
    tf__client_close(c);
  }
  printf("take list 400: kept and sent again until the server takes it (3 calls), the bad_request:takes issue queued once; 410: dropped\n");

  /* e: 401: a new token, then again (invalid_token inside the same call; another code at the next pass) */
  for (int k = 0; k < 2; ++k) {
    stub_reset();
    g_st.takes_status[0] = k ? -401 : 401;
    snprintf(d, sizeof d, "%s/call401_%d", dir, k);
    c = takes_client(d, TK_ON);
    tk_two_takes(c, x, &clock);
    left = tk_pass(c);
    tk_wait_calls(c, 2);
    CHECK(left == 0 && g_st.takes_calls == 2 && g_st.attests == 2 && tf__share_lists_waiting(c) == 0 && !strcmp(g_st.takes_body[1], kTwoTakes),
          "401 (%s): %d calls, %d tokens asked for, %d waiting", k ? "token_expired" : "invalid_token", g_st.takes_calls, g_st.attests,
          tf__share_lists_waiting(c));
    tf__client_close(c);
  }
  printf("take list 401: a new token (2 token requests), then the same body again\n");

  /* f: 503, then 429, then 204: the outbox's backoff between them; the bitrate ladder never moves */
  stub_reset();
  g_st.takes_status[0] = 503;
  g_st.takes_status[1] = 429;
  snprintf(d, sizeof d, "%s/call503", dir);
  c = takes_client(d, TK_ON);
  tk_two_takes(c, x, &clock);
  left = tk_pass(c);
  tk_wait_calls(c, 1);
  tk_pass(c);
  CHECK(left == 0 && g_st.takes_calls == 1 && tf__share_lists_waiting(c) == 1 && tf__sharing_bitrate(c) == 192000,
        "503: %d calls before the backoff ended, %d waiting, ladder %d", g_st.takes_calls, tf__share_lists_waiting(c), tf__sharing_bitrate(c));
  tf__now_override = (long long)time(NULL) + 3600;
  tk_wait_calls(c, 2);
  tk_pass(c);
  CHECK(g_st.takes_calls == 2 && tf__share_lists_waiting(c) == 1, "429: %d calls, %d waiting", g_st.takes_calls, tf__share_lists_waiting(c));
  tf__now_override = (long long)time(NULL) + 3 * 3600;
  tk_wait_calls(c, 3);
  tf__now_override = 0;
  CHECK(g_st.takes_calls == 3 && tf__share_lists_waiting(c) == 0 && !strcmp(g_st.takes_body[0], g_st.takes_body[2]) &&
            !strcmp(g_st.takes_body[1], g_st.takes_body[2]) && tf__sharing_bitrate(c) == 192000 && tk_shared_sessions(c) == 1,
        "503, 429, 204: %d calls, %d waiting, ladder %d", g_st.takes_calls, tf__share_lists_waiting(c), tf__sharing_bitrate(c));
  printf("take list 503 then 429: retried with the backoff, the same body three times, the ladder still at %d\n", tf__sharing_bitrate(c));
  tf__client_close(c);

  /* g: a list that waited 7 days is dropped unsent; withdrawal drops a waiting list at once */
  stub_reset();
  g_st.takes_net_down = 1;
  snprintf(d, sizeof d, "%s/call_old", dir);
  c = takes_client(d, TK_ON);
  tk_two_takes(c, x, &clock);
  tk_pass(c);
  tk_wait_calls(c, 1);
  snprintf(id, sizeof id, "%s", g_st.n_s ? g_st.s[0].id : "");
  CHECK(tf__share_lists_waiting(c) == 1, "a waiting list to age");
  tf__client_close(c);
  {
    /* item.json's "created" set 8 days back, as if the list had waited that long */
    char *ob = tf_path_join(d, "outbox"), *p = join3(ob, id, "item.json"), *t = read_text(p, NULL);
    char *at = t ? strstr(t, "\"created\":") : NULL;
    if (at) {
      TfBuf b;
      tf_buf_init(&b);
      tf_buf_put(&b, t, (size_t)(at - t));
      tf_buf_printf(&b, "\"created\":%lld", (long long)time(NULL) - 8 * 86400);
      at += 10;
      while (*at >= '0' && *at <= '9') at++;
      tf_buf_puts(&b, at);
      tf_write_file(p, b.p, b.n);
      tf_buf_free(&b);
    }
    CHECK(at != NULL, "item.json of a waiting list has no created");
    free(ob);
    free(p);
    free(t);
  }
  c = takes_client(d, TK_ON);
  tk_pass(c);
  CHECK(tf__share_lists_waiting(c) == 0 && g_st.takes_calls == 1 && !tk_file(d, id, "takes.json"), "a list 8 days old: %d waiting, %d calls",
        tf__share_lists_waiting(c), g_st.takes_calls);
  g_st.takes_net_down = 1;
  tk_two_takes(c, x, &clock);
  tk_pass(c);
  tk_wait_calls(c, 2);
  CHECK(tf__share_lists_waiting(c) == 1 && g_st.takes_calls == 2, "a second waiting list: %d waiting", tf__share_lists_waiting(c));
  tf__client_sharing_set(c, 0);
  tf__now_override = (long long)time(NULL) + 3600;
  tk_pass(c);
  tf__now_override = 0;
  CHECK(tf__share_lists_waiting(c) == 0 && g_st.takes_calls == 2, "withdrawal with a list waiting: %d waiting, %d calls", tf__share_lists_waiting(c),
        g_st.takes_calls);
  printf("take list: dropped unsent after 7 days; dropped at once when sharing is turned off\n");
  tf__client_close(c);
}

/* ================================================================== 6. the rest of the rules */
static void tk_rules(const char *dir, const float *x) {
  char d[1100], id[33] = "";
  char ids[8][33];
  int n_ids = 0;
  TkRec r;
  stub_reset();

  /* withdrawal in the join window: the recording ends at once and nothing of it is kept, and the queue (a closed
   * recording that waits to be uploaded; no uploader runs here) is deleted with it */
  snprintf(d, sizeof d, "%s/withdraw", dir);
  TfClient *c = takes_client(d, TK_ON);
  char id_q[33] = "";
  tk_take(c, 48000, x, 144000, 960, 0, 3000, 1);
  tf__share_open(c, id_q, NULL, NULL);
  tk_check_at(c, 3000 + 3001);
  tk_take(c, 48000, x, 144000, 960, 10000, 13000, 1);
  CHECK(tf__share_open(c, id, NULL, NULL) && strcmp(id, id_q) && tf__sharing_outbox_pending(c) == 2 && tk_file(d, id, "seg-00001.opus") &&
            tk_file(d, id_q, "seg-00001.opus"),
        "withdrawal: a queued recording and one that waits in its window (%d pending)", tf__sharing_outbox_pending(c));
  tf__share_now_ms = TK_T0 + 13500;
  tf__client_sharing_set(c, 0);
  CHECK(!tf__share_open(c, NULL, NULL, NULL) && tf__sharing_outbox_pending(c) == 0 && !tk_file(d, id, "seg-00001.opus") && !tk_file(d, id, "session.json") &&
            !tk_file(d, id_q, "seg-00001.opus") && !tk_file(d, id_q, "session.json"),
        "withdrawal in the window: the recording, the queue or their files are still there (%d pending)", tf__sharing_outbox_pending(c));
  CHECK(tk_take(c, 48000, x, 48000, 960, 13600, 14600, 1) == -1 && tf__sharing_outbox_pending(c) == 0, "a take began after the withdrawal");
  /* back on (the settings switch), then off during a joined take: its later audio is not recorded, nothing stays */
  tf__client_sharing_set(c, 1);
  tk_take(c, 48000, x, 144000, 960, 20000, 23000, 1);
  tk_check_at(c, 23500);
  TfShareSession *ss = tf__sharing_session_begin(c, 48000, "built_in");
  for (int i = 0; ss && i < 60; ++i) tf__sharing_session_push(ss, x + (size_t)i * 960, 960);
  CHECK(ss && tf__share_open(c, id, NULL, NULL) && tk_file(d, id, "takes.json"), "a take joined before the withdrawal");
  tf__client_sharing_set(c, 0);
  CHECK(!tf__share_open(c, NULL, NULL, NULL) && tf__sharing_outbox_pending(c) == 0 && !tk_file(d, id, "takes.json"), "withdrawal during a joined take");
  for (int i = 0; ss && i < 60; ++i) tf__sharing_session_push(ss, x + (size_t)i * 960, 960);
  tf__share_now_ms = TK_T0 + 26000;
  CHECK(tf__sharing_session_finish(ss, 1) == 0 && tf__sharing_outbox_pending(c) == 0, "a take that lost its recording to a withdrawal recorded something");
  printf("withdrawal: in the join window and during a joined take the recording ends at once, its files and the queue deleted\n");
  tf__client_close(c);

  /* the signed config stops offering sharing while a recording waits (here: the config expires): the recording
   * ends at the next check, inside its window, and nothing of it is kept */
  snprintf(d, sizeof d, "%s/config_off", dir);
  c = takes_client(d, TK_ON);
  tk_take(c, 48000, x, 144000, 960, 0, 3000, 1);
  CHECK(tf__share_open(c, id, NULL, NULL) && tk_file(d, id, "session.json"), "config off: a recording waits in its window");
  tf__now_override = (long long)time(NULL) + 2 * 86400;
  tk_check_at(c, 3100);
  tf__now_override = 0;
  CHECK(!tf__share_open(c, NULL, NULL, NULL) && !tk_file(d, id, "session.json") && tf__sharing_outbox_pending(c) == 0,
        "a recording waited on after the config stopped offering sharing");
  tf__client_close(c);

  /* background in the window closes the recording (a whole, completed one); the next take starts a new one */
  snprintf(d, sizeof d, "%s/background", dir);
  c = takes_client(d, TK_ON);
  n_ids = 0;
  tk_take(c, 48000, x, 144000, 960, 0, 3000, 1);
  tk_new_ids(c, ids, &n_ids, 8);
  tf__share_now_ms = TK_T0 + 3100;
  CHECK(tf__share_open(c, NULL, NULL, NULL), "background: nothing open before it");
  tf__client_background(c);
  tk_rec(d, ids[0], &r);
  {
    size_t n = 0, seg[64] = {0};
    int segs = 0;
    uint8_t *all = tk_segments(d, ids[0], &n, seg, &segs);
    TkStream st;
    tk_stream(all, n, 1, &st);
    CHECK(!tf__share_open(c, NULL, NULL, NULL) && r.ended && r.completed && r.duration_ms == 3000 && st.faults == 0 && st.pcm_n == 144000,
          "background in the window: open %d, ended %d, completed %d, %lld ms, %d page faults", tf__share_open(c, NULL, NULL, NULL), r.ended, r.completed,
          r.duration_ms, st.faults);
    free(all);
    free(st.pcm);
  }
  tk_take(c, 48000, x, 96000, 960, 3600, 5600, 1); /* 600 ms after the first take: it would have joined */
  tk_new_ids(c, ids, &n_ids, 8);
  CHECK(n_ids == 2 && tf__share_open(c, id, NULL, NULL) && !strcmp(id, ids[1]), "after the background a take joined the closed recording (%d recordings)", n_ids);
  /* background during a take: the take stays whole, the recording closes when it ends, without a window */
  tk_check_at(c, 6000);
  ss = tf__sharing_session_begin(c, 48000, "built_in");
  for (int i = 0; ss && i < 50; ++i) tf__sharing_session_push(ss, x + (size_t)i * 960, 960);
  tf__client_background(c);
  CHECK(tf__share_open(c, NULL, NULL, NULL), "background during a take closed the recording under it");
  for (int i = 50; ss && i < 100; ++i) tf__sharing_session_push(ss, x + (size_t)i * 960, 960);
  tf__share_now_ms = TK_T0 + 8000;
  CHECK(tf__sharing_session_finish(ss, 1) == 2000 && !tf__share_open(c, NULL, NULL, NULL), "background during a take: still open after the take");
  tk_rec(d, ids[1], &r);
  CHECK(r.ended && r.completed && r.duration_ms == 4000 && r.takes && strstr(r.takes, "{\"start_ms\":2000,\"gap_ms\":400}"),
        "background during a take: ended %d, %lld ms, takes %s", r.ended, r.duration_ms, r.takes ? r.takes : "(none)");
  free(r.takes);
  /* the app's own call: no take follows */
  tk_take(c, 48000, x, 96000, 960, 9000, 11000, 1);
  CHECK(tf__share_open(c, NULL, NULL, NULL) && tf__sharing_recording_close(c) == 0 && !tf__share_open(c, NULL, NULL, NULL) &&
            tf__sharing_recording_close(NULL) == TF_ERR_INVALID_ARGUMENT,
        "tf__sharing_recording_close");
  printf("background: closes the waiting recording at once (EOS, completed); during a take, when the take ends\n");
  tf__client_close(c);

  /* share_join_ms 0: never joined; the recording still waits 1500 ms for a tail (3.0; in 2.0.3 it closed with
   * its take). 8000: joined at 7000 and at exactly 8000, not at 8001 */
  snprintf(d, sizeof d, "%s/window0", dir);
  c = takes_client(d, "{\"contributions\":true,\"share_join_ms\":0}");
  n_ids = 0;
  tk_take(c, 48000, x, 96000, 960, 0, 2000, 1);
  CHECK(tf__share_open(c, NULL, NULL, NULL), "window 0: the recording does not wait for its tail");
  tk_new_ids(c, ids, &n_ids, 8);
  tk_check_at(c, 2000 + TF_SHARING_TAIL_WAIT_MS);
  CHECK(tf__share_open(c, NULL, NULL, NULL), "window 0: the recording closed before its tail wait was over");
  tk_check_at(c, 2000 + TF_SHARING_TAIL_WAIT_MS + 1);
  CHECK(!tf__share_open(c, NULL, NULL, NULL), "window 0: the recording is open after its tail wait");
  tk_take(c, 48000, x, 96000, 960, 4000, 6000, 1);
  CHECK(tf__share_open(c, NULL, NULL, NULL), "window 0: the second recording does not wait for its tail");
  tk_new_ids(c, ids, &n_ids, 8);
  tk_take(c, 48000, x, 96000, 960, 6001, 8001, 1); /* 1 ms after the second: inside its tail wait, and not joined */
  tk_new_ids(c, ids, &n_ids, 8);
  tk_rec(d, ids[0], &r);
  CHECK(n_ids == 3 && r.ended && !r.takes && tf__share_join_ms(c) == 0, "window 0: %d recordings", n_ids);
  tf__client_close(c);
  snprintf(d, sizeof d, "%s/window8000", dir);
  c = takes_client(d, "{\"contributions\":true,\"share_join_ms\":8000}");
  n_ids = 0;
  tk_take(c, 48000, x, 96000, 960, 0, 2000, 1);
  tk_take(c, 48000, x, 96000, 960, 9000, 11000, 1);  /* 7000 later */
  tk_take(c, 48000, x, 96000, 960, 19000, 21000, 1); /* exactly 8000 later */
  tk_new_ids(c, ids, &n_ids, 8);
  tk_take(c, 48000, x, 96000, 960, 29001, 31001, 1); /* 8001 later */
  tk_new_ids(c, ids, &n_ids, 8);
  tk_rec(d, ids[0], &r);
  CHECK(n_ids == 2 && tf__share_join_ms(c) == 8000 && r.ended && r.takes &&
            !strcmp(r.takes, "{\"takes\":[{\"start_ms\":0,\"gap_ms\":0},{\"start_ms\":2000,\"gap_ms\":7000},{\"start_ms\":4000,\"gap_ms\":8000}]}"),
        "window 8000: %d recordings, takes %s", n_ids, r.takes ? r.takes : "(none)");
  free(r.takes);
  tf__client_close(c);
  /* what counts as a window: a whole number 0 to 10000; anything else is 3000 */
  static const struct {
    const char *flags;
    int want;
  } win[] = {{TK_ON, 3000},
             {"{\"contributions\":true,\"share_join_ms\":10000}", 10000},
             {"{\"contributions\":true,\"share_join_ms\":1}", 1},
             {"{\"contributions\":true,\"share_join_ms\":10001}", 3000},
             {"{\"contributions\":true,\"share_join_ms\":-1}", 3000},
             {"{\"contributions\":true,\"share_join_ms\":2500.5}", 3000},
             {"{\"contributions\":true,\"share_join_ms\":\"5\"}", 3000},
             {"{\"contributions\":true,\"share_join_ms\":true}", 3000},
             {"{\"contributions\":true,\"share_join_ms\":null}", 3000},
             {"{\"contributions\":true,\"share_join_ms\":[8000]}", 3000}};
  int win_ok = 0;
  for (size_t i = 0; i < sizeof win / sizeof win[0]; ++i) {
    snprintf(d, sizeof d, "%s/window_v%d", dir, (int)i);
    c = takes_client(d, win[i].flags);
    CHECK(tf__share_join_ms(c) == win[i].want, "%s: window %d, want %d", win[i].flags, tf__share_join_ms(c), win[i].want);
    win_ok += tf__share_join_ms(c) == win[i].want;
    tf__client_close(c);
  }
  printf("share_join_ms: 0 never joins, 8000 joins at 7000 and 8000 and not at 8001; %d/%d values read as the window or the default\n", win_ok,
         (int)(sizeof win / sizeof win[0]));

  /* the window's own timer: its thread closes the recording once the clock says the window passed (prompted here,
   * the clock is the test's), and by itself on the real clock with a window of 40 ms */
  tf__client_no_timer = 0;
  snprintf(d, sizeof d, "%s/timer", dir);
  c = takes_client(d, TK_ON);
  tk_take(c, 48000, x, 96000, 960, 0, 2000, 1);
  tf__share_open(c, id, NULL, NULL);
  tf__share_now_ms = TK_T0 + 4999;
  tf__share_timer_poke(c);
  SLEEP_MS(50);
  CHECK(tf__share_open(c, NULL, NULL, NULL), "the timer closed the recording 1 ms before its window ended");
  tf__share_now_ms = TK_T0 + 5001;
  tf__share_timer_poke(c);
  for (int i = 0; i < 500 && tf__share_open(c, NULL, NULL, NULL); ++i) SLEEP_MS(10);
  tk_rec(d, id, &r);
  CHECK(!tf__share_open(c, NULL, NULL, NULL) && r.ended && r.completed, "the timer's thread did not close the recording after its window");
  tf__client_close(c);
  tf__share_now_ms = 0; /* the system's clock */
  snprintf(d, sizeof d, "%s/timer_real", dir);
  c = takes_client(d, "{\"contributions\":true,\"share_join_ms\":40}");
  ss = tf__sharing_session_begin(c, 48000, "built_in");
  for (int i = 0; ss && i < 100; ++i) tf__sharing_session_push(ss, x + (size_t)i * 960, 960);
  tf__sharing_session_finish(ss, 1);
  const double tt = tf__now_s();
  for (int i = 0; i < 1000 && tf__share_open(c, NULL, NULL, NULL); ++i) SLEEP_MS(5);
  CHECK(!tf__share_open(c, NULL, NULL, NULL) && tf__sharing_outbox_pending(c) == 1, "the timer did not close a 40 ms window on the real clock");
  printf("the window's timer: closes after the window on the test's clock, and by itself %.0f ms after a take with a 40 ms window\n",
         (tf__now_s() - tt) * 1000);
  tf__client_close(c);
  tf__client_no_timer = 1;
  tf__share_now_ms = TK_T0;

  /* 256 takes in one recording; the 257th starts a new one */
  snprintf(d, sizeof d, "%s/many", dir);
  c = takes_client(d, TK_ON);
  n_ids = 0;
  long long clock = 0;
  for (int k = 0; k < 257; ++k) {
    tk_take(c, 48000, x + (size_t)k * 480, 480, 480, clock, clock + 10, 1);
    tk_new_ids(c, ids, &n_ids, 8);
    if (k == 255) {
      /* 3.0: a full recording takes no further take, and like every recording it waits 1500 ms for its tail
       * (in 2.0.3 it closed with its 256th take) */
      int in_it = 0;
      CHECK(tf__share_open(c, NULL, &in_it, NULL) && in_it == 256 && n_ids == 1, "256 takes: the full recording does not wait for its tail (%d takes, %d recordings)",
            in_it, n_ids);
    }
    clock += 110;
  }
  tk_rec(d, ids[0], &r);
  {
    TfJson *tj = r.takes ? tf_json_parse(r.takes, strlen(r.takes), NULL) : NULL;
    const TfJson *a = tf_json_get(tj, "takes");
    CHECK(n_ids == 2 && r.ended && r.duration_ms == 2560 && a && a->n == 256 && jnum(&a->kids[255], "start_ms") == 2550 && jnum(&a->kids[255], "gap_ms") == 100 &&
              strlen(r.takes) < 16000 && tk_item(c, ids[0], "takes") == 256,
          "256 takes: %d recordings, %d entries, %d bytes", n_ids, a ? a->n : -1, r.takes ? (int)strlen(r.takes) : -1);
    printf("256 takes: one recording with 256 entries (%d bytes of take list); the 257th take started a new recording\n", r.takes ? (int)strlen(r.takes) : -1);
    tf_json_free(tj);
  }
  free(r.takes);
  tf__client_close(c);

  /* a take without audio adds no entry and does not move the previous take's end; two takes within one millisecond
   * share an entry; another model revision starts a new recording; the route may differ */
  snprintf(d, sizeof d, "%s/edges", dir);
  c = takes_client(d, TK_ON);
  n_ids = 0;
  tk_take(c, 48000, x, 96000, 960, 0, 2000, 1);
  tk_new_ids(c, ids, &n_ids, 8);
  CHECK(tk_take(c, 48000, x, 0, 960, 2500, 2600, 1) == 0, "a take without audio returned milliseconds");
  CHECK(tf__share_open(c, id, NULL, NULL) && !strcmp(id, ids[0]) && !tk_file(d, id, "takes.json"), "a take without audio closed the recording or added an entry");
  tk_take(c, 48000, x, 10, 960, 4900, 4901, 1);   /* 2900 ms after the take with audio: joins, entry {2000, 2900} */
  tk_take(c, 48000, x, 96000, 960, 5000, 7000, 1); /* starts at (96000 + 10) / 48 = 2000 ms too: no entry, its audio joins */
  tk_take(c, 48000, x, 0, 960, 9000, 9100, 1);     /* no audio, inside the window */
  tk_new_ids(c, ids, &n_ids, 8);
  tk_take(c, 48000, x, 96000, 960, 10001, 12001, 1); /* 3001 after the last take WITH audio, 901 after the empty one */
  tk_new_ids(c, ids, &n_ids, 8);
  tk_rec(d, ids[0], &r);
  CHECK(n_ids == 2 && r.ended && r.duration_ms == (96000 + 10 + 96000) * 1000LL / 48000 && r.takes &&
            !strcmp(r.takes, "{\"takes\":[{\"start_ms\":0,\"gap_ms\":0},{\"start_ms\":2000,\"gap_ms\":2900}]}"),
        "takes without audio and within a millisecond: %d recordings, %lld ms, takes %s", n_ids, r.duration_ms, r.takes ? r.takes : "(none)");
  free(r.takes);
  tk_check_at(c, 12001 + 100);
  ss = tf__share_begin(c, 48000, "usb", "v4.0+0123abc", NULL);
  for (int i = 0; ss && i < 100; ++i) tf__sharing_session_push(ss, x + (size_t)i * 960, 960);
  tf__share_now_ms = TK_T0 + 14101;
  tf__sharing_session_finish(ss, 1);
  tk_new_ids(c, ids, &n_ids, 8);
  CHECK(n_ids == 3, "a take of another model revision joined (%d recordings)", n_ids);
  tf__share_now_ms = TK_T0 + 14601;
  ss = tf__share_begin(c, 48000, "bluetooth", "v4.0+0123abc", NULL);
  for (int i = 0; ss && i < 100; ++i) tf__sharing_session_push(ss, x + (size_t)i * 960, 960);
  tf__share_now_ms = TK_T0 + 16601;
  tf__sharing_session_finish(ss, 1);
  tk_new_ids(c, ids, &n_ids, 8);
  tk_rec(d, ids[2], &r);
  CHECK(n_ids == 3 && !strcmp(r.route, "usb") && r.takes && strstr(r.takes, "{\"start_ms\":2000,\"gap_ms\":500}"),
        "the same revision on another route: %d recordings, route %s, takes %s", n_ids, r.route, r.takes ? r.takes : "(none)");
  free(r.takes);
  printf("edges: a take without audio adds no entry and keeps the previous end; one entry per millisecond; another model revision starts a new recording\n");
  tf__client_close(c);

  /* the server capped the upload while the recording waited (the uploader drops the session): the next take does
   * not join the dropped recording, it starts a new one */
  stub_reset();
  g_st.start_error = "segment_cap";
  snprintf(d, sizeof d, "%s/dropped", dir);
  c = takes_client(d, TK_ON);
  tk_take(c, 48000, x, 72000, 960, 0, 1500, 1);
  tf__share_open(c, id, NULL, NULL);
  tk_pass(c);
  CHECK(g_st.starts == 1 && !tk_file(d, id, "session.json") && tf__share_open(c, NULL, NULL, NULL), "the capped upload was not dropped (%d starts)", g_st.starts);
  g_st.start_error = NULL;
  tk_take(c, 48000, x, 72000, 960, 2000, 3500, 1);
  char id_new[33] = "";
  CHECK(tf__share_open(c, id_new, NULL, NULL) && strcmp(id, id_new) && tk_file(d, id_new, "session.json") && !tk_file(d, id_new, "takes.json") &&
            !tk_file(d, id, "capture.json"),
        "a take joined a recording the uploader had dropped");
  tk_check_at(c, 3500 + 3001);
  CHECK(tk_pass(c) == 0 && g_st.n_s == 1 && !strcmp(g_st.s[0].id, id_new) && g_st.takes_calls == 0, "the recording after a dropped one did not upload");
  printf("a recording the uploader dropped (segment_cap) is not joined: the next take starts a new one\n");
  tf__client_close(c);
}

/* the 2 s minimum of a cancelled session is on the recording's total; completed is the last take's; the consent
 * ask after a finished take leaves the recording open (the real segment plan: no segment closes here) */
static void tk_partial(const char *dir, const float *x) {
  char d[1100];
  char ids[8][33];
  int n_ids = 0;
  TkRec r;
  snprintf(d, sizeof d, "%s/partial", dir);
  TfClient *c = takes_client(d, TK_ON);
  tk_take(c, 48000, x, 57600, 960, 0, 1200, 0); /* 1.2 s, cancelled */
  tk_new_ids(c, ids, &n_ids, 8);
  CHECK(tf__share_open(c, NULL, NULL, NULL), "a cancelled 1.2 s take did not wait for a next take");
  feedback_session(c); /* the moment the SDK may ask: it does not close the recording */
  CHECK(tf__share_open(c, NULL, NULL, NULL), "the session-finished call closed the recording");
  tk_take(c, 48000, x, 48000, 960, 1700, 2700, 0); /* 1.0 s, cancelled: 2.2 s in all */
  tk_check_at(c, 2700 + 3001);
  tk_rec(d, ids[0], &r);
  CHECK(n_ids == 1 && r.ended && !r.completed && r.duration_ms == 2200 && r.takes, "two cancelled takes of 2.2 s in all: ended %d, completed %d, %lld ms",
        r.ended, r.completed, r.duration_ms);
  free(r.takes);
  tk_take(c, 48000, x, 57600, 960, 10000, 11200, 0); /* 1.2 s, cancelled, alone */
  tk_new_ids(c, ids, &n_ids, 8);
  tk_check_at(c, 11200 + 3001);
  CHECK(n_ids == 2 && !tk_file(d, ids[1], "session.json") && tf__sharing_outbox_pending(c) == 1, "a cancelled 1.2 s take alone was kept");
  tk_take(c, 48000, x, 144000, 960, 20000, 23000, 1); /* completed */
  tk_new_ids(c, ids, &n_ids, 8);
  tk_take(c, 48000, x, 24000, 960, 23500, 24000, 0); /* then a cancelled one */
  tk_check_at(c, 24000 + 3001);
  tk_rec(d, ids[2], &r);
  CHECK(r.ended && !r.completed && r.duration_ms == 3500, "completed take then a cancelled one: completed %d, %lld ms", r.completed, r.duration_ms);
  free(r.takes);
  printf("partial takes: the 2 s minimum counts the recording's total (1.2 s + 1.0 s kept, 1.2 s alone dropped); completed is the last take's\n");
  tf__client_close(c);
}

/* the 20-minute cap, live: with at most 30 s of room a joining take is held until it ends (it joins if it fits)
 * or outgrows the room (the recording closes without it); with more room it joins at once and stays whole */
static void tk_cap(const char *dir) {
  char d[1100];
  char ids[8][33];
  int n_ids = 0;
  TkRec r;
  const int S = 16000;
  const double t0 = tf__now_s();
  snprintf(d, sizeof d, "%s/cap", dir);
  TfClient *c = takes_client(d, TK_ON);
  tk_take(c, S, NULL, 1199LL * S, S, 0, 1199000, 1); /* 19 min 59 s */
  tk_new_ids(c, ids, &n_ids, 8);
  /* 500 ms fit in the 1000 ms of room: held, joined when the take ends */
  tk_check_at(c, 1199500);
  TfShareSession *ss = tf__sharing_session_begin(c, S, "built_in");
  tf__sharing_session_push(ss, g_tk_zero, S / 2);
  long long samples = 0;
  char id[33] = "";
  CHECK(tf__share_open(c, id, NULL, &samples) && samples == 1199LL * S && !tk_file(d, id, "takes.json"), "a held take was written before it ended");
  tf__share_now_ms = TK_T0 + 1200000;
  CHECK(tf__sharing_session_finish(ss, 1) == 500, "the held take's milliseconds");
  CHECK(tf__share_open(c, NULL, NULL, &samples) && samples == 1199LL * S + S / 2, "the held take did not join when it ended");
  /* 400 ms more fit (1199.9 s); then 200 ms do not: the recording closes without them, a new one starts */
  tk_take(c, S, NULL, S * 4 / 10, S, 1200300, 1200700, 1);
  tk_take(c, S, NULL, S * 2 / 10, S, 1201000, 1201200, 1);
  tk_new_ids(c, ids, &n_ids, 8);
  tk_rec(d, ids[0], &r);
  CHECK(n_ids == 2 && r.ended && r.completed && r.duration_ms == 1199900 && r.takes &&
            !strcmp(r.takes, "{\"takes\":[{\"start_ms\":0,\"gap_ms\":0},{\"start_ms\":1199000,\"gap_ms\":500},{\"start_ms\":1199500,\"gap_ms\":300}]}"),
        "the hold under the cap: %d recordings, %lld ms, takes %s", n_ids, r.duration_ms, r.takes ? r.takes : "(none)");
  free(r.takes);
  tk_check_at(c, 1201200 + 3001);
  tk_rec(d, ids[1], &r);
  CHECK(r.ended && r.duration_ms == 200 && !r.takes, "the take that did not fit: %lld ms in its own recording", r.duration_ms);
  free(r.takes);
  tf__client_close(c);
  /* more than 30 s of room: the take is written as it comes. Where the recording reaches 20 minutes it closes as
   * a whole recording and the take goes on in a new one at its next sample (3.0; in 2.0.3 the take
   * ran past 20 minutes and stayed whole) */
  snprintf(d, sizeof d, "%s/cap_over", dir);
  c = takes_client(d, TK_ON);
  n_ids = 0;
  tk_take(c, S, NULL, 1150LL * S, S, 0, 1150000, 1);
  tk_new_ids(c, ids, &n_ids, 8);
  tk_check_at(c, 1151000);
  ss = tf__sharing_session_begin(c, S, "built_in");
  tf__sharing_session_push(ss, g_tk_zero, S);
  CHECK(tf__share_open(c, id, NULL, &samples) && samples == 1151LL * S && tk_file(d, id, "takes.json"), "a take with 50 s of room was held");
  for (int i = 1; i < 60; ++i) tf__sharing_session_push(ss, g_tk_zero, S);
  /* 50 s filled the first recording, which closed; the other 10 s are in a new one, which is open */
  char id2[33] = "";
  tk_new_ids(c, ids, &n_ids, 8);
  CHECK(n_ids == 2 && tf__share_open(c, id2, NULL, &samples) && samples == 10LL * S && strcmp(id2, id) && !strcmp(id2, ids[1]),
        "the take did not go on in a new recording at the cap: %d recordings, %lld samples in the open one", n_ids, samples);
  tk_rec(d, ids[0], &r);
  CHECK(r.ended && r.completed && r.duration_ms == 1200000 && r.takes &&
            !strcmp(r.takes, "{\"takes\":[{\"start_ms\":0,\"gap_ms\":0},{\"start_ms\":1150000,\"gap_ms\":1000}]}"),
        "the recording that reached the cap: ended %d completed %d, %lld ms, takes %s", r.ended, r.completed, r.duration_ms, r.takes ? r.takes : "(none)");
  free(r.takes);
  tf__share_now_ms = TK_T0 + 1211000;
  CHECK(tf__sharing_session_finish(ss, 1) == 60000 && tf__share_open(c, NULL, NULL, NULL), "the continuation does not wait for a take");
  tk_take(c, S, NULL, S, S, 1211500, 1212500, 1); /* joins the continuation */
  tk_check_at(c, 1212500 + 3001);
  tk_new_ids(c, ids, &n_ids, 8);
  tk_rec(d, ids[1], &r);
  char *ob2 = tf_path_join(d, "outbox"), *sp2 = join3(ob2, ids[1], "session.json");
  TfJson *sj2 = read_json_file(sp2);
  CHECK(n_ids == 2 && r.ended && r.completed && r.duration_ms == 11000 && r.takes &&
            !strcmp(r.takes, "{\"takes\":[{\"start_ms\":0,\"gap_ms\":0},{\"start_ms\":10000,\"gap_ms\":500}]}") && jnum(sj2, "take_seq") == 2 &&
            tf_json_get(sj2, "gap_ms_prev") && jnum(sj2, "gap_ms_prev") == 0,
        "the continuation: %d recordings, %lld ms, takes %s, take_seq %lld", n_ids, r.duration_ms, r.takes ? r.takes : "(none)", jnum(sj2, "take_seq"));
  tf_json_free(sj2);
  free(sp2);
  free(ob2);
  free(r.takes);
  tf__client_close(c);
  printf("the 20-minute cap: a held take joins when it fits (1199.9 s) and starts a new recording when it does not; a take with room fills the recording "
         "to 1200 s, which closes, and goes on in a new one (take_seq 2, gap_ms_prev 0) (%.1f s)\n", tf__now_s() - t0);
}

static int takes_tests(const char *vdir, const char *dir) {
  tf_mkdirs(dir);
  tf_mutex_init(&g_st.m);
  stub_reset();
  tf_http_set_transport(stub, NULL); /* parts 1 to 3 start no uploader: nothing should go out */
  tf__share_plan[0] = 10, tf__share_plan[1] = 20, tf__share_plan[2] = 40, tf__share_plan[3] = 60;
  tf__share_now_ms = TK_T0;

  /* 1 */
  const int vr = takes_vector(vdir, dir);
  if (vr) return vr;
  fflush(stdout);

  /* 2: the self-test clip (16 kHz), repeated to 75 s: at its own rate through the resampler, cut at odd sample
   * counts, the ladder moved during segments 1, 2 and 3; then as 48 kHz, the first take exactly segment 1 */
  size_t clip_bytes = 0;
  char *cp = tf_path_join(vdir, "selftest_16k.f32");
  float *clip = (float *)read_text(cp, &clip_bytes);
  free(cp);
  if (!clip) return 2;
  const int clip_n = (int)(clip_bytes / 4);
  const long long long_n = 75LL * 48000 + 1000;
  float *x = (float *)malloc(sizeof(float) * (size_t)long_n);
  for (long long i = 0; i < long_n; ++i) x[i] = clip[i % clip_n];
  free(clip);
  {
    const long long n16[3] = {17LL * 16000 + 333, 31LL * 16000 + 171, 75LL * 16000 + 137 - (17LL * 16000 + 333) - (31LL * 16000 + 171)};
    const TkLadder ev[3] = {{5LL * 16000, 0, 1}, {20LL * 16000, 0, 2}, {50LL * 16000, 1, 3}}; /* 96k, 32k, 64k */
    const int br[4] = {192000, 96000, 32000, 64000};
    tk_three_takes(dir, "16 kHz through the resampler", 16000, x, n16, ev, 3, br, 4);
    const long long n48[3] = {480000, 592333, 33LL * 48000 + 7 - 480000 - 592333};
    const int br48[3] = {192000, 192000, 192000};
    tk_three_takes(dir, "48 kHz, take 1 ends on a segment boundary", 48000, x, n48, NULL, 0, br48, 3);
  }
  CHECK(g_st.starts == 0 && g_st.puts == 0 && g_st.takes_calls == 0, "something was sent without an uploader");

  /* 3 */
  tk_two_recordings(dir, x);
  fflush(stdout);

  /* 4 to 6 with 1 s segments and noise at 48 kHz, against the stub qlab-api and R2 */
  tf__share_plan[0] = tf__share_plan[1] = tf__share_plan[2] = tf__share_plan[3] = 1;
  float *y = (float *)malloc(sizeof(float) * 48000 * 8);
  lcg_noise(y, 48000 * 8, 0.2);
  tk_recovery(dir, y);
  fflush(stdout);
  tk_list_call(dir, y);
  fflush(stdout);
  tk_rules(dir, y);
  fflush(stdout);
  tf__share_plan[0] = 10, tf__share_plan[1] = 20, tf__share_plan[2] = 40, tf__share_plan[3] = 60;
  tk_partial(dir, y);
  fflush(stdout);
  tk_cap(dir);

  tf__share_now_ms = 0;
  tf_http_set_transport(NULL, NULL);
  stub_reset();
  free(x);
  free(y);
  printf("takes in a row (SDK 2.0.3): %s\n", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}
