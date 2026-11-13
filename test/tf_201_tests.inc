/* Included by tf_tests.c after the sharing tests: what SDK 2.0.1 adds.
 *
 *   bitrate <vectors dir> <work dir>                    the shared bitrate ladder vector; a real 150 s stream whose
 *                                                       segments use different rungs (pages, CRCs, granules, the
 *                                                       libopus decode, markers, bytes per second; ffprobe and
 *                                                       ffmpeg's two decoders when ffmpeg is on PATH); the outbox's
 *                                                       failures and successes moving the next segment's bitrate
 *   consent <vectors dir> <fixtures.json> <work dir>    the shared consent text vector and refused inputs; the
 *                                                       app's own wording: version, pause, update ask, session JSON,
 *                                                       no capture before a grant, withdrawal and deletion; the decks
 *   sdk_latest <configs.json> <work dir> [<fx folder>] the signed config's sdk_latest: one debug warning per client when
 *                                                       newer, silent in release builds and for equal, older, malformed
 *   telemetry <work dir> [<fx folder> <fixture dir>]    deltas: sealing, report_id on retry, 400 and 503, midnight,
 *                                                       the 15-minute and background send points, the timer, the
 *                                                       2.0.1 fields from a scored result, share counters, register
 *                                                       once per UTC day, the kill switch */
#include <opus.h>
#ifndef _WIN32
#include <dirent.h>
#endif
extern int tf__client_no_timer;
int tf__ladder_event(TfClient *c, int event, double backlog_s);
void tf__telemetry_add(TfClient *c, const char *field, long long n);

static char *read_text(const char *path, size_t *n) {
  char *t = NULL;
  size_t k = 0;
  if (tf_read_file(path, &t, &k)) return NULL;
  if (n) *n = k;
  return t;
}
static TfJson *read_json_file(const char *path) {
  size_t n;
  char *t = read_text(path, &n);
  TfJson *j = t ? tf_json_parse(t, n, NULL) : NULL;
  free(t);
  return j;
}
static char *join3(const char *a, const char *b, const char *c) {
  char *x = tf_path_join(a, b);
  char *y = x && c ? tf_path_join(x, c) : NULL;
  if (!c) return x;
  free(x);
  return y;
}
static int is_hex32(const char *s) {
  if (!s || strlen(s) != 32) return 0;
  for (; *s; ++s)
    if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
  return 1;
}

/* ================================================================== bitrate */
typedef struct {
  int seq, flags, n_packets, first_len;
  uint32_t serial;
  long long granule;
  const uint8_t *first;
  int crc_ok, continued;
} OggPage;
/* the pages of a byte stream; packets[] gets every packet (pointer, length) in order */
static int parse_pages(const uint8_t *d, size_t n, OggPage *pages, int max_pages, const uint8_t **pk, int *pk_len, int max_pk, int *n_pk) {
  size_t at = 0;
  int np = 0;
  *n_pk = 0;
  while (at < n) {
    if (at + 27 > n || memcmp(d + at, "OggS", 4) || np >= max_pages) return -1;
    const int ns = d[at + 26];
    size_t body = 0;
    for (int i = 0; i < ns; ++i) body += d[at + 27 + i];
    const size_t len = 27 + (size_t)ns + body;
    if (at + len > n) return -1;
    OggPage *p = &pages[np++];
    memset(p, 0, sizeof *p);
    p->flags = d[at + 5];
    p->granule = 0;
    for (int i = 7; i >= 0; --i) p->granule = (p->granule << 8) | d[at + 6 + i];
    p->serial = (uint32_t)d[at + 14] | (uint32_t)d[at + 15] << 8 | (uint32_t)d[at + 16] << 16 | (uint32_t)d[at + 17] << 24;
    p->seq = (int)((uint32_t)d[at + 18] | (uint32_t)d[at + 19] << 8 | (uint32_t)d[at + 20] << 16 | (uint32_t)d[at + 21] << 24);
    uint8_t *t = (uint8_t *)malloc(len);
    memcpy(t, d + at, len);
    const uint32_t want = (uint32_t)t[22] | (uint32_t)t[23] << 8 | (uint32_t)t[24] << 16 | (uint32_t)t[25] << 24;
    memset(t + 22, 0, 4);
    uint32_t c = 0;
    for (size_t i = 0; i < len; ++i) {
      c ^= (uint32_t)t[i] << 24;
      for (int k = 0; k < 8; ++k) c = c & 0x80000000u ? (c << 1) ^ 0x04c11db7u : c << 1;
    }
    free(t);
    p->crc_ok = c == want;
    const uint8_t *b = d + at + 27 + ns;
    size_t off = 0, cur = 0;
    for (int i = 0; i < ns; ++i) {
      cur += d[at + 27 + i];
      if (d[at + 27 + i] < 255) {
        if (p->n_packets == 0) {
          p->first = b + off;
          p->first_len = (int)cur;
        }
        if (*n_pk < max_pk) {
          pk[*n_pk] = b + off;
          pk_len[*n_pk] = (int)cur;
          (*n_pk)++;
        }
        p->n_packets++;
        off += cur;
        cur = 0;
      }
    }
    p->continued = cur != 0; /* a packet continued on the next page: our muxer never does that */
    at += len;
  }
  return np;
}

#ifdef _WIN32
#define POPEN_R(cmd) _popen(cmd, "rb")
#define PCLOSE(f) _pclose(f)
#define NULLDEV "NUL"
#else
#define POPEN_R(cmd) popen(cmd, "r")
#define PCLOSE(f) pclose(f)
#define NULLDEV "/dev/null"
#endif
static int have_tool(const char *tool) {
  char cmd[256];
  snprintf(cmd, sizeof cmd, "%s -hide_banner -version > " NULLDEV " 2>&1", tool);
  return system(cmd) == 0;
}
/* bytes ffmpeg writes for the file decoded to s16le mono 48 kHz with the decoder named (opus: ffmpeg's own,
 * libopus) */
static long long ffmpeg_samples(const char *file, const char *decoder) {
  char cmd[2400];
  snprintf(cmd, sizeof cmd, "ffmpeg -hide_banner -v error -c:a %s -i \"%s\" -f s16le -ac 1 -ar 48000 -", decoder, file);
  FILE *f = POPEN_R(cmd);
  if (!f) return -1;
  char buf[65536];
  long long total = 0;
  size_t k;
  while ((k = fread(buf, 1, sizeof buf, f)) > 0) total += (long long)k;
  const int st = PCLOSE(f);
  return st == 0 ? total / 2 : -1;
}
static double ffprobe_duration(const char *file) {
  char cmd[2400];
  snprintf(cmd, sizeof cmd, "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \"%s\"", file);
  FILE *f = POPEN_R(cmd);
  if (!f) return -1;
  char line[128] = "";
  if (!fgets(line, sizeof line, f)) line[0] = 0;
  PCLOSE(f);
  return line[0] ? atof(line) : -1;
}

static int marker_bitrate(const char *outbox, const char *id, int n) {
  char name[32];
  snprintf(name, sizeof name, "seg-%05d.json", n);
  char *p = join3(outbox, id, name);
  TfJson *j = read_json_file(p);
  free(p);
  const TfJson *b = tf_json_get(j, "bitrate");
  const int v = b && b->type == TJ_NUM ? (int)b->num : -1;
  tf_json_free(j);
  return v;
}
static int only_session(TfClient *c, char id[33]) {
  char *js = tf__sharing_outbox_json(c);
  TfJson *j = js ? tf_json_parse(js, strlen(js), NULL) : NULL;
  free(js);
  const int ok = j && j->type == TJ_ARR && j->n == 1 && tf_json_str(tf_json_get(&j->kids[0], "id"));
  if (ok) snprintf(id, 33, "%s", tf_json_str(tf_json_get(&j->kids[0], "id")));
  tf_json_free(j);
  return ok;
}

static int bitrate_tests(const char *vdir, const char *dir) {
  tf_mkdirs(dir);
  /* 1. the shared vector: every case on a fresh ladder, every step's expect */
  {
    char *vp = tf_path_join(vdir, "bitrate_ladder.json");
    TfJson *v = read_json_file(vp);
    free(vp);
    if (!v) return 2;
    char *d1 = tf_path_join(dir, "ladder");
    TfClient *c = share_client(d1);
    const TfJson *cases = tf_json_get(v, "cases");
    int steps = 0, ok = 0;
    for (int i = 0; c && cases && i < cases->n; ++i) {
      tf__ladder_event(c, -1, 0);
      CHECK(tf__sharing_bitrate(c) == 192000, "a fresh ladder starts at 192000");
      const TfJson *st = tf_json_get(&cases->kids[i], "steps");
      for (int k = 0; st && k < st->n; ++k) {
        const char *ev = tf_json_str(tf_json_get(&st->kids[k], "event"));
        const TfJson *bl = tf_json_get(&st->kids[k], "backlog_s");
        const int e = !strcmp(ev, "upload_failed") ? 0 : !strcmp(ev, "upload_ok") ? 1 : 2;
        const int got = tf__ladder_event(c, e, bl ? bl->num : 0);
        const int want = (int)tf_json_get(&st->kids[k], "expect")->num;
        steps++;
        ok += got == want;
        CHECK(got == want, "%s, step %d (%s): %d, want %d", tf_json_str(tf_json_get(&cases->kids[i], "name")), k + 1, ev, got, want);
      }
    }
    printf("bitrate ladder vs the shared vector: %d/%d steps in %d cases\n", ok, steps, cases ? cases->n : 0);
    tf__client_close(c);
    tf_json_free(v);
    free(d1);
  }

  /* 2. a real 150 s stream through the capture (libopus, the self-test clip repeated, fed as 48 kHz), its five
   * segments at 192, 96, 32, 64 and 192 kbit/s */
  tf_mutex_init(&g_st.m);
  stub_reset();
  tf_http_set_transport(stub, NULL); /* nothing should go out: the uploader is never started here */
  tf__share_plan[0] = 10, tf__share_plan[1] = 20, tf__share_plan[2] = 40, tf__share_plan[3] = 60;
  size_t clip_bytes = 0;
  char *cp = tf_path_join(vdir, "selftest_16k.f32");
  float *clip = (float *)read_text(cp, &clip_bytes);
  free(cp);
  if (!clip) return 2;
  const int clip_n = (int)(clip_bytes / 4);
  const int total = 150 * 48000;
  float *x = (float *)malloc(sizeof(float) * (size_t)total);
  for (int i = 0; i < total; ++i) x[i] = clip[i % clip_n];
  free(clip);
  char *d2 = tf_path_join(dir, "stream");
  TfClient *c = share_client(d2);
  CHECK(c && tf__client_sharing_answer(c, 1) == 0 && tf__client_sharing_state(c), "consent");
  char *outbox = tf_path_join(d2, "outbox");
  TfShareSession *ss = tf__sharing_session_begin(c, 48000, "built_in");
  CHECK(ss != NULL, "session: %s", tf_last_error() ? tf_last_error() : "");
  if (!ss) return 1;
  char id[33] = "";
  only_session(c, id);
  /* segment k starts at packet 500, 1500, 3500, 6500: before the packet that opens it, the ladder is moved; the
   * capture's own segment_closed events (backlog 30, 70, then 60 s once segments 1 to 3 went up) do not move it */
  const int starts[5] = {0, 500, 1500, 3500, 6500};
  const int want_br[5] = {192000, 96000, 32000, 64000, 192000};
  uint8_t *segs[8] = {0};
  size_t seg_n[8] = {0};
  int frame = 0;
  for (int k = 1; k <= 4; ++k) {
    for (; frame < starts[k]; ++frame) tf__sharing_session_push(ss, x + (size_t)frame * 960, 960);
    if (k == 1) tf__ladder_event(c, 0, 0);                                         /* -> 96000 */
    if (k == 2) tf__ladder_event(c, 0, 0), tf__ladder_event(c, 0, 0);              /* -> 32000 */
    if (k == 3)
      for (int i = 0; i < 3; ++i) tf__ladder_event(c, 1, 0);                       /* -> 64000 */
    if (k == 4) {
      /* segments 1 to 3 "uploaded": kept for the stream check, removed as the uploader does after a 2xx */
      for (int s = 1; s <= 3; ++s) {
        char name[32];
        snprintf(name, sizeof name, "seg-%05d.opus", s);
        char *p = join3(outbox, id, name);
        segs[s] = (uint8_t *)read_text(p, &seg_n[s]);
        CHECK(segs[s] != NULL, "segment %d closed before segment %d opened", s, k + 1);
        remove(p);
        free(p);
      }
      for (int i = 0; i < 6; ++i) tf__ladder_event(c, 1, 0);                       /* -> 96000 -> 192000 */
    }
  }
  for (; frame < total / 960; ++frame) tf__sharing_session_push(ss, x + (size_t)frame * 960, 960);
  const int ms = share_finish(c, ss, 1);
  CHECK(ms == 150000, "shared %d ms", ms);
  for (int s = 4; s <= 5; ++s) {
    char name[32];
    snprintf(name, sizeof name, "seg-%05d.opus", s);
    char *p = join3(outbox, id, name);
    segs[s] = (uint8_t *)read_text(p, &seg_n[s]);
    free(p);
  }
  {
    char name[32];
    snprintf(name, sizeof name, "seg-%05d.opus", 6);
    char *p = join3(outbox, id, name);
    CHECK(!tf_file_exists(p), "a sixth segment");
    free(p);
  }
  /* the markers and the outbox item */
  int markers_ok = 0;
  for (int s = 1; s <= 5; ++s) {
    const int b = marker_bitrate(outbox, id, s);
    CHECK(b == want_br[s - 1], "segment %d marker bitrate %d, want %d", s, b, want_br[s - 1]);
    markers_ok += b == want_br[s - 1];
  }
  {
    char *js = tf__sharing_outbox_json(c);
    char want[128];
    snprintf(want, sizeof want, "\"segmentBitrates\":[%d,%d,%d,%d,%d]", want_br[0], want_br[1], want_br[2], want_br[3], want_br[4]);
    CHECK(js && strstr(js, want), "outbox item: %s", js ? js : "NULL");
    free(js);
  }
  /* the stream joined */
  size_t all_n = 0;
  for (int s = 1; s <= 5; ++s) all_n += seg_n[s];
  uint8_t *all = (uint8_t *)malloc(all_n + 1);
  size_t at = 0;
  for (int s = 1; s <= 5; ++s) {
    if (segs[s]) memcpy(all + at, segs[s], seg_n[s]);
    at += seg_n[s];
  }
  static OggPage pages[4096];
  static const uint8_t *pk[16384];
  static int pk_len[16384];
  int n_pk = 0;
  const int np = parse_pages(all, all_n, pages, 4096, pk, pk_len, 16384, &n_pk);
  CHECK(np > 4, "pages: %d", np);
  int bad = 0;
  long long g = 0;
  int audio_packets = 0;
  for (int i = 0; i < np; ++i) {
    const OggPage *p = &pages[i];
    bad += !p->crc_ok;
    bad += p->seq != i;
    bad += p->serial != pages[0].serial;
    bad += (i == 0) != !!(p->flags & 0x02);
    bad += (i == np - 1) != !!(p->flags & 0x04);
    bad += p->continued;
    const int head = p->first && p->first_len >= 8 && !memcmp(p->first, "OpusHead", 8);
    const int tags = p->first && p->first_len >= 8 && !memcmp(p->first, "OpusTags", 8);
    bad += head != (i == 0);
    bad += tags != (i == 1);
    if (i >= 2) {
      audio_packets += p->n_packets;
      g += 960LL * p->n_packets;
      if (i < np - 1) bad += p->granule != g;
    }
  }
  CHECK(bad == 0, "the joined stream: %d page faults (sequence, serial, BOS, EOS, CRC, headers, granules)", bad);
  const int pre_skip = pages[0].first ? pages[0].first[10] | pages[0].first[11] << 8 : -1;
  const long long last_g = pages[np - 1].granule;
  CHECK(last_g == pre_skip + (long long)total, "last granule %lld, want the encoder's final position %lld", last_g, pre_skip + (long long)total);
  CHECK(last_g <= g && last_g > g - 960LL * pages[np - 1].n_packets, "the last page trims the end padding");
  /* libopus decode of the whole stream: pre-skip and end trimmed by the granule, exactly the input length */
  long long decoded = 0;
  {
    int err = 0;
    OpusDecoder *dec = opus_decoder_create(48000, 1, &err);
    static float pcm[5760];
    for (int i = 2; dec && i < n_pk; ++i) { /* packets 0 and 1: OpusHead and OpusTags */
      const int k = opus_decode_float(dec, pk[i], pk_len[i], pcm, 5760, 0);
      if (k < 0) {
        bad++;
        break;
      }
      decoded += k;
    }
    if (dec) opus_decoder_destroy(dec);
  }
  const long long out_n = (decoded < last_g ? decoded : last_g) - pre_skip;
  CHECK(audio_packets == n_pk - 2 && decoded == 960LL * audio_packets, "decoded %lld samples from %d packets", decoded, audio_packets);
  CHECK(out_n == total, "libopus decode: %lld samples, want %d", out_n, total);
  /* bytes per second per segment follow the rungs */
  double kbps[6] = {0};
  printf("bitrate stream: %d pages, %d audio packets, serial %08x, pre-skip %d, last granule %lld, libopus decode %lld samples (input %d)\n", np,
         audio_packets, pages[0].serial, pre_skip, last_g, out_n, total);
  for (int s = 1; s <= 5; ++s) {
    /* granule differences: segment 5 ends at the trimmed last granule */
    const double secs = s < 5 ? (starts[s] - starts[s - 1]) * 0.02 : (double)(last_g - 960LL * starts[4]) / 48000.0;
    kbps[s] = seg_n[s] * 8.0 / secs / 1000.0;
    printf("  segment %d: %d bit/s, %llu bytes, %.2f s, %.1f kbit/s\n", s, want_br[s - 1], (unsigned long long)seg_n[s], secs, kbps[s]);
  }
  CHECK(kbps[3] < kbps[4] && kbps[4] < kbps[2] && kbps[2] < kbps[1] && kbps[2] < kbps[5], "kbit/s do not follow the rungs");
  CHECK(kbps[3] < 0.5 * kbps[1] && kbps[3] < 0.5 * kbps[5], "the 32k segment is not clearly smaller than the 192k ones");
  /* ffprobe and ffmpeg's two decoders, where ffmpeg is on PATH */
  if (have_tool("ffmpeg") && have_tool("ffprobe")) {
    char *f = tf_path_join(dir, "stream.opus");
    tf_write_file(f, all, all_n);
    const double dur = ffprobe_duration(f);
    const long long nat = ffmpeg_samples(f, "opus"), lib = ffmpeg_samples(f, "libopus");
    printf("  ffprobe duration %.6f s (input %.6f s, %+.1f ms); ffmpeg decoders: native %lld, libopus %lld samples\n", dur, total / 48000.0,
           (dur - total / 48000.0) * 1000, nat, lib);
    CHECK(dur > 0 && fabs(dur - total / 48000.0) <= 0.020, "ffprobe duration %.6f", dur);
    CHECK(nat == total, "ffmpeg native opus decoder: %lld samples", nat);
    CHECK(lib == total || lib == -1, "ffmpeg libopus decoder: %lld samples", lib);
    if (lib == -1) printf("  (this ffmpeg has no libopus decoder)\n");
    free(f);
  } else {
    printf("  ffmpeg / ffprobe not on PATH: their checks skipped\n");
  }
  printf("bitrate stream: markers %d/5 at their rungs\n", markers_ok);
  free(all);
  for (int s = 1; s <= 5; ++s) free(segs[s]);
  tf__client_close(c);
  free(outbox);
  free(d2);
  free(x);

  /* 3. the wiring: the outbox's network failure lowers the NEXT segment of a running capture; three successes
   * raise it again (1 s segments, the stub qlab-api and R2) */
  stub_reset();
  tf__share_plan[0] = tf__share_plan[1] = tf__share_plan[2] = tf__share_plan[3] = 1;
  float *y = (float *)malloc(sizeof(float) * 48000 * 20);
  lcg_noise(y, 48000 * 20, 0.2);
  char *d3 = tf_path_join(dir, "wiring");
  c = share_client(d3);
  tf__client_sharing_answer(c, 1);
  tf__sharing_outbox_resume(c);
  g_st.put_status = 503;
  ss = tf__sharing_session_begin(c, 48000, NULL);
  int fr = 0;
  for (; fr < 51; ++fr) tf__sharing_session_push(ss, y + (size_t)fr * 960, 960); /* segment 1 closed, 2 opened */
  for (int i = 0; i < 1000 && tf__sharing_bitrate(c) != 96000; ++i) SLEEP_MS(10);
  CHECK(tf__sharing_bitrate(c) == 96000, "after the 503: %d (%d PUTs failed)", tf__sharing_bitrate(c), g_st.put_failed);
  for (; fr < 101; ++fr) tf__sharing_session_push(ss, y + (size_t)fr * 960, 960); /* segment 3 opened */
  char *ob = tf_path_join(d3, "outbox");
  char wid[33] = "";
  only_session(c, wid);
  const int b1 = marker_bitrate(ob, wid, 1), b2 = marker_bitrate(ob, wid, 2), b3 = marker_bitrate(ob, wid, 3);
  /* segment 2 opened while the uploader was trying segment 1 (either side of the failure); segment 3 after it */
  CHECK(b1 == 192000 && (b2 == 192000 || b2 == 96000) && b3 == 96000, "segments 1, 2, 3 at %d, %d, %d (want 192000, either, 96000)", b1, b2, b3);
  /* the network is back: the retry is due (clock moved past the backoff), three PUTs succeed */
  g_st.put_status = 0;
  tf__now_override = (long long)time(NULL) + 3600;
  tf__sharing_outbox_resume(c);
  for (; fr < 151; ++fr) tf__sharing_session_push(ss, y + (size_t)fr * 960, 960); /* segment 3 closed, 4 opened */
  tf__sharing_outbox_resume(c);
  for (int i = 0; i < 1000 && tf__sharing_bitrate(c) != 192000; ++i) SLEEP_MS(10);
  CHECK(tf__sharing_bitrate(c) == 192000, "after the successes: %d (%d PUTs)", tf__sharing_bitrate(c), g_st.puts);
  const int opened_up = fr / 50 + 1; /* the segment the next boundary opens */
  for (; fr < 50 * opened_up + 1; ++fr) tf__sharing_session_push(ss, y + (size_t)fr * 960, 960);
  const int b_up = marker_bitrate(ob, wid, opened_up + 1);
  CHECK(b_up == 192000, "segment %d after the successes at %d", opened_up + 1, b_up);
  share_finish(c, ss, 1);
  const int left = tf__sharing_outbox_wait(c, 20000);
  int segs_up = 0;
  CHECK(left == 0 && g_st.n_s == 1 && joined_ok(&g_st.s[0], 1, &segs_up), "the wired session uploaded as one stream (%d left)", left);
  printf("bitrate wiring: a 503 PUT took the next segment to 96000 (segments 1-3: %d %d %d); %d PUTs later segment %d opened at %d; %d segments "
         "uploaded as one stream\n", b1, b2, b3, g_st.puts, opened_up + 1, b_up, segs_up);
  tf__now_override = 0;
  tf__client_close(c);
  tf_http_set_transport(NULL, NULL);
  stub_reset();
  free(ob);
  free(d3);
  free(y);
  printf("bitrate: %s\n", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}

/* ================================================================== consent wording */
typedef struct {
  int calls, reask;
  char version[40];
  char *copy;
} AskRec;
static void on_ask(void *u, const TfConsentRequest *r) {
  AskRec *a = (AskRec *)u;
  a->calls++;
  a->reask = r->reask;
  snprintf(a->version, sizeof a->version, "%s", r->consent_version ? r->consent_version : "");
  free(a->copy);
  a->copy = NULL;
  if (r->copy_json) {
    const size_t n = strlen(r->copy_json) + 1;
    a->copy = (char *)malloc(n);
    memcpy(a->copy, r->copy_json, n);
  }
}
static TfClient *consent_client_lang(const char *dir, AskRec *rec, const char *lang) {
  seed_config(dir, "{\"contributions\":true}");
  TfClientConfig o = tf__client_default_config();
  o.language = lang;
  o.key = "qlk_v1_local_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "org.example.Sharing";
  o.platform = "linux";
  o.api_url = "http://localhost:8787";
  o.storage_dir = dir;
  o.telemetry = 0;
  if (rec) {
    o.on_consent = on_ask;
    o.user = rec;
  }
  return tf__client_open(&o);
}
static TfClient *consent_client(const char *dir, AskRec *rec) { return consent_client_lang(dir, rec, NULL); }
static void feedback_session(TfClient *c) {
  TfSessionStats ss;
  memset(&ss, 0, sizeof ss);
  ss.feedback_shown = 1;
  tf__client_session_finished(c, &ss);
}
/* the session JSON of the only session in the outbox */
static TfJson *session_body(TfClient *c, const char *dir) {
  char id[33];
  if (!only_session(c, id)) return NULL;
  char *ob = tf_path_join(dir, "outbox"), *p = join3(ob, id, "session.json");
  TfJson *j = read_json_file(p);
  free(ob);
  free(p);
  return j;
}
static int status_has(TfClient *c, const char *what) {
  const char *s = tf__client_sharing_status_json(c);
  return s && strstr(s, what) != NULL;
}
static int status_int(TfClient *c, const char *key) {
  const char *s = tf__client_sharing_status_json(c);
  TfJson *j = s ? tf_json_parse(s, strlen(s), NULL) : NULL;
  const TfJson *v = tf_json_get(j, key);
  const int n = v && v->type == TJ_NUM ? (int)v->num : -1;
  tf_json_free(j);
  return n;
}
/* a capture opened now carries this consent_version (and this consent_text_sha256, or none for NULL) in its session
 * JSON; the empty capture is then dropped (under 2 s), so the outbox is as before */
static int session_version_is(TfClient *c, const char *dir, const char *version, const char *hash) {
  TfShareSession *ss = tf__sharing_session_begin(c, 48000, NULL);
  if (!ss) return 0;
  TfJson *sj = session_body(c, dir);
  const char *v = tf_json_str(tf_json_get(sj, "consent_version")), *h = tf_json_str(tf_json_get(sj, "consent_text_sha256"));
  const int ok = sj && v && !strcmp(v, version) && (hash ? h && !strcmp(h, hash) : !tf_json_get(sj, "consent_text_sha256"));
  if (!ok) printf("  session JSON: consent_version %s, consent_text_sha256 %s\n", v ? v : "(none)", h ? h : "(none)");
  tf_json_free(sj);
  tf__sharing_session_finish(ss, 0);
  return ok;
}
static int keys_sorted(const TfJson *j) {
  for (int i = 1; j && i < j->n; ++i)
    if (strcmp(j->kids[i - 1].key, j->kids[i].key) >= 0) return 0;
  return j != NULL;
}

/* removed_2026-11-12.json, {lang: {string key: {"bytes", "sha256"}}}: one entry's length and hash; 0 when it is
 * not well formed */
static size_t gone_entry(const TfJson *e, uint8_t want[32]) {
  const TfJson *b = tf_json_get(e, "bytes");
  const char *hex = tf_json_str(tf_json_get(e, "sha256"));
  return b && b->type == TJ_NUM && b->num >= 24 && hex && strlen(hex) == 64 && !tf_hex_decode(hex, want, 32) ? (size_t)b->num : 0;
}
static int gone_entries(const TfJson *gone) {
  int n = 0;
  uint8_t want[32];
  for (int l = 0; gone && l < gone->n; ++l)
    for (int k = 0; k < gone->kids[l].n; ++k) n += gone_entry(&gone->kids[l].kids[k], want) > 0;
  return n;
}
/* how many windows of the UTF-8 bytes of `s` have the length and the SHA-256 of an entry (an entry equal to the
 * one before it in its language, the same sentence in both strings, is searched once) */
static int gone_windows(const TfJson *gone, const char *s) {
  const size_t n = strlen(s);
  int hits = 0;
  for (int l = 0; gone && l < gone->n; ++l) {
    uint8_t before[32] = {0};
    for (int k = 0; k < gone->kids[l].n; ++k) {
      uint8_t want[32], got[32];
      const size_t w = gone_entry(&gone->kids[l].kids[k], want);
      if (!w || (k > 0 && !memcmp(want, before, 32))) continue;
      memcpy(before, want, 32);
      for (size_t at = 0; at + w <= n; ++at) {
        tf_sha256(s + at, w, got);
        hits += !memcmp(got, want, 32);
      }
    }
  }
  return hits;
}
/* the same over every string anywhere in the JSON value `j` (a copy deck: its strings, and the strings in them) */
static int gone_in(const TfJson *j, const TfJson *gone) {
  if (!j) return 0;
  int n = j->type == TJ_STR && j->str ? gone_windows(gone, j->str) : 0;
  if (j->type == TJ_OBJ || j->type == TJ_ARR)
    for (int i = 0; i < j->n; ++i) n += gone_in(&j->kids[i], gone);
  return n;
}

static int consent_tests(const char *vdir, const char *fx_path, const char *dir) {
  tf_mkdirs(dir);
  char *vp = tf_path_join(vdir, "consent_text.json");
  TfJson *v = read_json_file(vp);
  free(vp);
  if (!v) return 2;
  /* 1. the shared vector: version, hash and canonical bytes for every case; every refused input refused */
  const TfJson *cases = tf_json_get(v, "cases");
  int ok = 0;
  char d[1100];
  for (int i = 0; cases && i < cases->n; ++i) {
    const TfJson *cs = &cases->kids[i], *ov = tf_json_get(cs, "overrides");
    snprintf(d, sizeof d, "%s/vector%d", dir, i);
    TfClient *c = consent_client(d, NULL);
    const char *keys[16], *texts[16];
    int n = 0;
    for (int k = 0; ov && k < ov->n && n < 16; ++k, ++n) {
      keys[n] = ov->kids[k].key;
      texts[n] = ov->kids[k].str;
    }
    const int rc = tf_sharing_set_wording(c, keys, texts, n);
    const char *wv = tf_json_str(tf_json_get(cs, "consent_version")), *wh = tf_json_str(tf_json_get(cs, "consent_text_sha256"));
    const char *wc = tf_json_str(tf_json_get(cs, "canonical_utf8_hex"));
    const char *gv = tf__sharing_consent_version(c), *gh = tf__sharing_consent_sha256(c), *gt = tf__sharing_consent_text(c);
    char *hex = NULL;
    if (gt) {
      hex = (char *)malloc(2 * strlen(gt) + 1);
      tf_hex_encode((const uint8_t *)gt, strlen(gt), hex);
    }
    const int good = rc == 0 && gv && !strcmp(gv, wv) && (wh ? gh && !strcmp(gh, wh) : !gh) && (wc ? hex && !strcmp(hex, wc) : !gt);
    CHECK(good, "consent vector %s: rc %d, version %s (want %s), hash %s", tf_json_str(tf_json_get(cs, "name")), rc, gv ? gv : "NULL", wv,
          gh ? gh : "NULL");
    ok += good;
    free(hex);
    tf__client_close(c);
  }
  const TfJson *refused = tf_json_get(v, "refused");
  int ref_ok = 0;
  {
    snprintf(d, sizeof d, "%s/refused", dir);
    TfClient *c = consent_client(d, NULL);
    const char *k0[] = {"sheet.title"}, *t0[] = {"Help other learners"};
    tf_sharing_set_wording(c, k0, t0, 1);
    char before[40];
    snprintf(before, sizeof before, "%s", tf__sharing_consent_version(c));
    for (int i = 0; refused && i < refused->n; ++i) {
      const TfJson *ov = tf_json_get(&refused->kids[i], "overrides");
      const char *k1[1] = {ov->kids[0].key}, *t1[1] = {ov->kids[0].str};
      const int rc = tf_sharing_set_wording(c, k1, t1, 1);
      const int good = rc == TF_ERR_INVALID_ARGUMENT && !strcmp(tf__sharing_consent_version(c), before);
      CHECK(good, "refused input (%s) was accepted or changed the version", tf_json_str(tf_json_get(&refused->kids[i], "why")));
      ref_ok += good;
    }
    const char *k2[] = {"sheet.title", "sheet.title"}, *t2[] = {"a", "b"};
    const char *k3[] = {"impact.minutes"}, *t3[] = {"x"};
    const char *k4[] = {"sheet.title"}, *t4[] = {"\xc2\xa0\xe3\x80\x80"}; /* no-break space, ideographic space */
    CHECK(tf_sharing_set_wording(c, k2, t2, 2) == TF_ERR_INVALID_ARGUMENT, "a key given twice was accepted");
    CHECK(tf_sharing_set_wording(c, k3, t3, 1) == TF_ERR_INVALID_ARGUMENT, "the plural object itself was accepted as a key");
    CHECK(tf_sharing_set_wording(c, k4, t4, 1) == TF_ERR_INVALID_ARGUMENT, "Unicode white space was accepted as a text");
    CHECK(!strcmp(tf__sharing_consent_version(c), before), "a refused call changed the version");
    tf__client_close(c);
  }
  printf("consent text vs the shared vector: %d/%d cases, %d/%d refused inputs refused\n", ok, cases ? cases->n : 0, ref_ok, refused ? refused->n : 0);
  tf_json_free(v);

  /* 2. the flow: a deck grant, then the app's own wording: no ask, sharing stays on, the session JSON keeps the deck's
   * version; a new learner under the wording records custom-...; a wording change after a custom grant keeps the
   * custom version and its hash (also across a restart); no capture before a grant; deletion under the older
   * version; the settings switch records the version in force */
  AskRec rec;
  memset(&rec, 0, sizeof rec);
  snprintf(d, sizeof d, "%s/flow", dir);
  TfClient *c = consent_client(d, &rec);
  CHECK(tf__sharing_session_begin(c, 48000, NULL) == NULL, "default path: a capture before any answer");
  feedback_session(c);
  CHECK(rec.calls == 1 && rec.reask == 0 && !strcmp(rec.version, "share-2026-11-09"), "the first ask: %d calls, version %s", rec.calls, rec.version);
  CHECK(rec.copy && !strcmp(rec.copy, tf__sharing_deck_json("en")), "the ask's copy is the bundled deck without own wording");
  tf__client_sharing_answer(c, 1);
  CHECK(tf__client_sharing_state(c) == 1, "granted under the deck");
  CHECK(session_version_is(c, d, "share-2026-11-09", NULL), "without own wording the session JSON has the deck's version and no hash");
  static const char *const kw[] = {"sheet.what", "impact.minutes.other"};
  static const char *const tw[] = {"The audio of your recitations in this app and basic technical details. Nothing else.",
                                   "You shared {minutes} minutes. Thank you."};
  static const char *const tw2[] = {"The audio of your recitations in this app and the app's version. Nothing else.",
                                    "You have shared {minutes} minutes. Thank you."};
  CHECK(tf_sharing_set_wording(c, kw, tw, 2) == 0, "own wording: %s", tf_last_error() ? tf_last_error() : "");
  const char *custom = tf__sharing_consent_version(c), *hash = tf__sharing_consent_sha256(c);
  CHECK(custom && !strncmp(custom, "custom-", 7) && strlen(custom) == 31 && hash && strlen(hash) == 64 && !strncmp(custom + 7, hash, 24),
        "custom version %s", custom ? custom : "NULL");
  char custom_v[40], hash_v[65];
  snprintf(custom_v, sizeof custom_v, "%s", custom ? custom : "");
  snprintf(hash_v, sizeof hash_v, "%s", hash ? hash : "");
  {
    const char *copy = tf__client_sharing_copy_json(c);
    char want_v[64];
    snprintf(want_v, sizeof want_v, "\"consent_version\":\"%s\"", custom_v);
    CHECK(copy && strstr(copy, tw[0]) && strstr(copy, tw[1]) && strstr(copy, want_v) && strstr(copy, "\"one\":"), "the client's copy: %.200s",
          copy ? copy : "NULL");
  }
  for (int i = 0; i < 30; ++i) feedback_session(c);
  CHECK(tf__client_sharing_ask_now(c) == 0, "ask_now after a yes");
  CHECK(rec.calls == 1, "own wording after a deck grant asked again: %d calls", rec.calls);
  CHECK(tf__client_sharing_state(c) == 1 && status_has(c, "\"consentVersion\":\"share-2026-11-09\"") && status_has(c, "\"updateNeeded\":false") &&
            status_has(c, "\"pendingAsk\":false"),
        "own wording after a deck grant paused sharing: %s", tf__client_sharing_status_json(c));
  CHECK(session_version_is(c, d, "share-2026-11-09", NULL), "own wording after a deck grant: the session JSON lost the deck's version");
  /* clearing the wording again changes nothing for this learner */
  CHECK(tf_sharing_set_wording(c, NULL, NULL, 0) == 0 && !strcmp(tf__sharing_consent_version(c), "share-2026-11-09") &&
            tf__sharing_consent_sha256(c) == NULL && tf__sharing_consent_text(c) == NULL,
        "cleared wording");
  CHECK(tf__client_sharing_state(c) == 1 && rec.calls == 1, "a cleared wording paused or asked");
  tf__client_close(c);

  /* a new learner under the wording records custom-...; then the app changes its wording: no ask, the original
   * custom version and hash in the JSON, across a restart; deletion under that older version */
  memset(&rec, 0, sizeof rec);
  snprintf(d, sizeof d, "%s/custom", dir);
  c = consent_client(d, &rec);
  tf_sharing_set_wording(c, kw, tw, 2);
  CHECK(tf__sharing_session_begin(c, 48000, NULL) == NULL, "own wording: a capture before any answer");
  feedback_session(c);
  CHECK(rec.calls == 1 && !strcmp(rec.version, custom_v) && rec.copy && strstr(rec.copy, tw[0]), "the ask under own wording: %d calls, version %s",
        rec.calls, rec.version);
  tf__client_sharing_answer(c, 1);
  {
    char want[200];
    snprintf(want, sizeof want, "\"consentVersion\":\"%s\",\"consentTextSha256\":\"%s\"", custom_v, hash_v);
    CHECK(tf__client_sharing_state(c) == 1 && status_has(c, want), "status after the grant: %s", tf__client_sharing_status_json(c));
  }
  CHECK(tf_sharing_set_wording(c, kw, tw2, 2) == 0, "the second wording");
  char custom2[40], hash2[65];
  snprintf(custom2, sizeof custom2, "%s", tf__sharing_consent_version(c));
  snprintf(hash2, sizeof hash2, "%s", tf__sharing_consent_sha256(c) ? tf__sharing_consent_sha256(c) : "");
  CHECK(strcmp(custom2, custom_v) && !strncmp(custom2, "custom-", 7), "the second wording has its own version: %s", custom2);
  for (int i = 0; i < 30; ++i) feedback_session(c);
  CHECK(rec.calls == 1 && tf__client_sharing_state(c) == 1, "a wording change after a custom grant: %d calls, state %d", rec.calls,
        tf__client_sharing_state(c));
  TfShareSession *ss = tf__sharing_session_begin(c, 48000, NULL);
  {
    TfJson *sj = session_body(c, d);
    CHECK(sj && tf_json_str(tf_json_get(sj, "consent_text_sha256")) && !strcmp(tf_json_get(sj, "consent_text_sha256")->str, hash_v) &&
              tf_json_str(tf_json_get(sj, "consent_version")) && !strcmp(tf_json_get(sj, "consent_version")->str, custom_v) && keys_sorted(sj),
          "after a wording change the session JSON carries the version and hash agreed to, keys sorted");
    tf_json_free(sj);
  }
  float *noise = (float *)malloc(sizeof(float) * 48000 * 3);
  lcg_noise(noise, 48000 * 3, 0.2);
  tf__sharing_session_push(ss, noise, 48000 * 3);
  tf__sharing_session_finish(ss, 1);
  CHECK(tf__sharing_outbox_pending(c) == 1, "one session pending under the older custom version");
  tf__client_close(c);
  c = consent_client(d, &rec);
  tf_sharing_set_wording(c, kw, tw2, 2);
  for (int i = 0; i < 10; ++i) feedback_session(c);
  CHECK(rec.calls == 1 && tf__client_sharing_state(c) == 1 && !strcmp(tf__sharing_consent_version(c), custom2), "a restart under the new wording: %d calls",
        rec.calls);
  CHECK(tf__sharing_outbox_pending(c) == 1, "the session recorded under the older custom version survived the restart");
  int deleted = 0;
  {
    char *text = NULL;
    size_t n;
    if (!tf_read_file(fx_path, &text, &n)) {
      g_rp.fx = tf_json_parse(text, n, NULL);
      free(text);
      tf_http_set_transport(replay_sharing, NULL);
      char code[33];
      CHECK(tf__client_sharing_recovery_code(c, code) == 0, "a contributor exists");
      CHECK(tf__client_sharing_delete(c) == 0, "delete under an older custom version: %s", tf_last_error() ? tf_last_error() : "");
      CHECK(tf__client_sharing_recovery_code(c, code) != 0 && tf__sharing_outbox_pending(c) == 0, "the contributor or the queue survived the deletion");
      deleted = 1;
      tf_http_set_transport(NULL, NULL);
      tf_json_free(g_rp.fx);
      g_rp.fx = NULL;
    }
  }
  /* 3.0.1: a deletion the server took turns sharing off, as the settings switch does, and nobody is asked again;
   * the learner turns it on again with the switch */
  CHECK(deleted && tf__client_sharing_state(c) == 0 && status_has(c, "\"consent\":\"final\"") && status_has(c, "\"pendingAsk\":false") && rec.calls == 1,
        "an accepted deletion left sharing on, or asked: %s", tf__client_sharing_status_json(c));
  for (int i = 0; i < 30; ++i) feedback_session(c);
  CHECK(tf__client_sharing_ask_now(c) == 0 && rec.calls == 1 && tf__client_sharing_state(c) == 0, "asked after the deletion: %d calls", rec.calls);
  tf__client_sharing_set(c, 1);
  CHECK(tf__client_sharing_state(c) == 1 && rec.calls == 1, "the switch after a deletion: %s", tf__client_sharing_status_json(c));
  /* withdrawal: never asked again; the settings switch records the version in force then */
  tf__client_sharing_set(c, 0);
  for (int i = 0; i < 30; ++i) feedback_session(c);
  CHECK(tf__client_sharing_ask_now(c) == 0 && rec.calls == 1 && tf__client_sharing_state(c) == 0, "asked after sharing was turned off: %d calls", rec.calls);
  tf__client_sharing_set(c, 1);
  {
    char want[200];
    snprintf(want, sizeof want, "\"consentVersion\":\"%s\",\"consentTextSha256\":\"%s\"", custom2, hash2);
    CHECK(tf__client_sharing_state(c) == 1 && status_has(c, want), "the settings switch grants the wording in force: %s", tf__client_sharing_status_json(c));
  }
  CHECK(session_version_is(c, d, custom2, hash2), "the settings switch's grant in the session JSON");
  tf__client_close(c);

  /* 3. upgrades: a 2.0.0 sharing.json (the exact 2.0.0 format; 2.0.0 recorded consentVersion with every grant, and
   * its decks were share-2026-11-07), one without a version, a 2.0.1 grant to an earlier deck, and a recording-fix
   * build's paused grant, a 3.0 grant to the deck of 2026-11-09, and a 3.1 grant to the deck of 2026-11-12. With
   * this SDK's deck in force: no ask before or after sessions, sharing on, the stored version in the session
   * JSON, across a restart. */
  static const struct {
    const char *name, *state, *want;
  } up[] = {
      {"2.0.0 grant",
       "{\"v\":1,\"consent\":\"granted\",\"asks\":1,\"lastAskSession\":2,\"sessions\":5,\"thanked\":true,\"decidedAt\":1762500000,"
       "\"consentVersion\":\"share-2026-11-07\",\"guardian\":false,\"sharedSessions\":2,\"pendingAsk\":0}",
       "share-2026-11-07"},
      {"2.0.0 grant without a version",
       "{\"v\":1,\"consent\":\"granted\",\"asks\":1,\"lastAskSession\":2,\"sessions\":5,\"thanked\":false,\"decidedAt\":1762500000,"
       "\"consentVersion\":null,\"guardian\":false,\"sharedSessions\":0,\"pendingAsk\":0}",
       "share-2026-11-07"},
      {"2.0.1 grant to an earlier deck",
       "{\"v\":1,\"consent\":\"granted\",\"asks\":1,\"lastAskSession\":1,\"sessions\":3,\"thanked\":false,\"decidedAt\":1762600000,"
       "\"consentVersion\":\"share-2026-11-08\",\"consentTextSha256\":null,\"guardian\":false,\"sharedSessions\":0,\"pendingAsk\":0}",
       "share-2026-11-08"},
      /* SPEC 11.8: a yes given in 3.0.x (share-2026-11-09) is kept with its version and not asked again */
      {"a 3.0 grant to the deck of 2026-11-09",
       "{\"v\":1,\"consent\":\"granted\",\"asks\":1,\"lastAskSession\":1,\"sessions\":3,\"thanked\":false,\"decidedAt\":1762700000,"
       "\"consentVersion\":\"share-2026-11-09\",\"consentTextSha256\":null,\"guardian\":false,\"sharedSessions\":1,\"pendingAsk\":0}",
       "share-2026-11-09"},
      /* SPEC 11.8: a yes given in 3.1.0 or 3.1.1 to the deck of 2026-11-12, whose name is
       * NEWER than the deck this SDK bundles (share-2026-11-09 again). The state is the file 3.1.0 writes. It is
       * kept with its version: not asked again, and the next recording carries share-2026-11-12 */
      {"a 3.1 grant to the deck of 2026-11-12",
       "{\"v\":1,\"consent\":\"granted\",\"asks\":1,\"lastAskSession\":1,\"sessions\":4,\"thanked\":false,\"decidedAt\":1762950000,"
       "\"consentVersion\":\"share-2026-11-12\",\"consentTextSha256\":null,\"guardian\":false,\"sharedSessions\":1,\"pendingAsk\":0,\"stamp\":1}",
       "share-2026-11-12"},
      {"a recording-fix build's paused grant",
       "{\"v\":1,\"consent\":\"unknown\",\"asks\":0,\"lastAskSession\":1,\"sessions\":4,\"thanked\":false,\"decidedAt\":1762500000,"
       "\"consentVersion\":\"share-2026-11-07\",\"consentTextSha256\":null,\"guardian\":false,\"sharedSessions\":0,\"pendingAsk\":0,\"update\":1}",
       "share-2026-11-07"},
  };
  int up_ok = 0;
  for (int k = 0; k < 6; ++k) {
    const int f0 = fails;
    snprintf(d, sizeof d, "%s/upgrade%d", dir, k);
    seed_config(d, "{\"contributions\":true}");
    char *sp = tf_path_join(d, "sharing.json");
    tf_write_file(sp, up[k].state, strlen(up[k].state));
    free(sp);
    memset(&rec, 0, sizeof rec);
    c = consent_client(d, &rec);
    char want[80];
    snprintf(want, sizeof want, "\"consentVersion\":\"%s\"", up[k].want);
    CHECK(rec.calls == 0 && tf__client_sharing_state(c) == 1 && status_has(c, want), "%s: at start %d calls, %s", up[k].name, rec.calls,
          tf__client_sharing_status_json(c));
    for (int i = 0; i < 12; ++i) feedback_session(c);
    tf__client_sharing_ask_now(c);
    CHECK(rec.calls == 0 && tf__client_sharing_state(c) == 1 && status_has(c, want) && status_has(c, "\"pendingAsk\":false"), "%s: after sessions %d calls, %s",
          up[k].name, rec.calls, tf__client_sharing_status_json(c));
    CHECK(session_version_is(c, d, up[k].want, NULL), "%s: the session JSON does not carry %s", up[k].name, up[k].want);
    tf__client_close(c);
    c = consent_client(d, NULL); /* no callback: a pending ask would show in the status */
    feedback_session(c);
    CHECK(tf__client_sharing_state(c) == 1 && status_has(c, want) && status_has(c, "\"pendingAsk\":false"), "%s: after a restart %s", up[k].name,
          tf__client_sharing_status_json(c));
    tf__client_sharing_ask_now(c);
    CHECK(status_has(c, "\"pendingAsk\":false") && session_version_is(c, d, up[k].want, NULL), "%s: after a restart and ask-now, an ask is pending or the session JSON does not carry %s",
          up[k].name, up[k].want);
    if (k == 0) {
      /* withdrawal under 2.0.0's version: uploads stop, the queue goes, no ask; the settings switch then records the
       * version in force */
      ss = tf__sharing_session_begin(c, 48000, NULL);
      tf__sharing_session_push(ss, noise, 48000 * 3);
      tf__sharing_session_finish(ss, 1);
      CHECK(tf__sharing_outbox_pending(c) == 1, "one session pending under 2.0.0's version");
      tf__client_sharing_set(c, 0);
      CHECK(tf__client_sharing_state(c) == 0 && tf__sharing_outbox_pending(c) == 0, "withdrawal under 2.0.0's version");
      for (int i = 0; i < 20; ++i) feedback_session(c);
      CHECK(status_has(c, "\"pendingAsk\":false") && status_has(c, "\"consent\":\"final\""), "asked after the withdrawal: %s", tf__client_sharing_status_json(c));
      tf__client_sharing_set(c, 1);
      CHECK(tf__client_sharing_state(c) == 1 && status_has(c, "\"consentVersion\":\"share-2026-11-09\""), "the settings switch after an upgrade: %s",
            tf__client_sharing_status_json(c));
      CHECK(session_version_is(c, d, "share-2026-11-09", NULL), "the settings switch's grant in the session JSON");
    }
    tf__client_close(c);
    up_ok += fails == f0;
  }
  /* 3b. SDK 3.1.0 (SPEC 15.3, contract 1.10.0): a refusal that stops the model removes the models and nothing
   * else. Here with sharing on and one recording not sent yet: after the model host's refusal the sharing state,
   * the unsent recording, the device key and the remembered refusal are as they were, and the model files are
   * gone; the same after a restart. (The models are stand-ins; the real download is in the model tests.) */
  {
    char sp2[1300], mf[1400], mr[1400], mc[1400];
    snprintf(d, sizeof d, "%s/stop_removes", dir);
    seed_config(d, "{\"contributions\":true}");
    snprintf(sp2, sizeof sp2, "%s/sharing.json", d);
    tf_write_file(sp2, up[0].state, strlen(up[0].state));
    c = consent_client(d, NULL);
    ss = tf__sharing_session_begin(c, 48000, NULL);
    tf__sharing_session_push(ss, noise, 48000 * 3);
    tf__sharing_session_finish(ss, 1);
    snprintf(mf, sizeof mf, "%s/models/cccccccccccccccccccccccccccccccccccccccc", d);
    tf_mkdirs(mf);
    snprintf(mf, sizeof mf, "%s/models/cccccccccccccccccccccccccccccccccccccccc/manifest.json", d);
    tf_write_file(mf, "{}", 2);
    snprintf(mr, sizeof mr, "%s/models/cccccccccccccccccccccccccccccccccccccccc.install", d);
    tf_write_file(mr, "installed 0000000000000000000000000000000000000000000000000000000000000000\n", 75);
    snprintf(mc, sizeof mc, "%s/models/current.json", d);
    tf_write_file(mc, "{}", 2);
    uint8_t k1[32], k2[32];
    memset(k2, 0, sizeof k2);
    const int key_made = tf__install_key(d, 1, k1) == 0;
    size_t n1 = 0, n2 = 0;
    char *s1 = read_text(sp2, &n1);
    CHECK(key_made && tf__sharing_outbox_pending(c) == 1 && tf__client_sharing_state(c) == 1 && tf_file_exists(mf), "before the refusal: one recording pending, sharing on");
    tf__client_refuse(c, "key_revoked"); /* what a model request refused with this reason does */
    char *s2 = read_text(sp2, &n2);
    CHECK(!tf_file_exists(mf) && !tf_file_exists(mr) && !tf_file_exists(mc), "the refusal left model files: manifest %d, record %d, current.json %d", tf_file_exists(mf),
          tf_file_exists(mr), tf_file_exists(mc));
    CHECK(tf__sharing_outbox_pending(c) == 1 && tf__client_sharing_state(c) == 1 && s1 && s2 && n1 == n2 && !memcmp(s1, s2, n1) &&
              tf__install_key(d, 0, k2) == 0 && !memcmp(k1, k2, 32) && !strcmp(tf__client_gate(c), "key_revoked"),
          "the refusal touched more than the models: %d recordings pending, sharing %d, the sharing state %s, the device key %s, remembered \"%s\"",
          tf__sharing_outbox_pending(c), tf__client_sharing_state(c), s1 && s2 && n1 == n2 && !memcmp(s1, s2, n1) ? "unchanged" : "CHANGED",
          !memcmp(k1, k2, 32) ? "unchanged" : "CHANGED", tf__client_gate(c));
    free(s1);
    free(s2);
    tf__client_close(c);
    /* the refusal is in the state the Client left (this test's client maker writes a fresh state at every open, so
     * it is read here; the model tests start a Client on a remembered refusal) */
    char st2[1300];
    snprintf(st2, sizeof st2, "%s/client.json", d);
    s1 = read_text(st2, &n1);
    CHECK(s1 && strstr(s1, "\"refused\":\"key_revoked\"") && strstr(s1, "\"remove_models\":false"), "the state after the refusal: %s", s1 ? s1 : "(none)");
    free(s1);
    c = consent_client(d, NULL);
    CHECK(tf__sharing_outbox_pending(c) == 1 && tf__client_sharing_state(c) == 1 && !tf_file_exists(mf), "after a restart: %d recordings pending, sharing %d",
          tf__sharing_outbox_pending(c), tf__client_sharing_state(c));
    tf__client_sharing_set(c, 0); /* the queue goes, as the other tests leave it */
    tf__client_close(c);
    printf("a refusal that stops the model removed the model files and nothing else: the sharing state, one unsent recording, the device key and the "
           "remembered refusal stayed, also across a restart\n");
  }
  free(noise);
  CHECK(up_ok == 6, "%d of 6 stored answers of other decks were kept with their version and without an ask", up_ok);
  printf("upgrades kept the grant with no ask (2.0.0 and earlier decks, the decks of 2026-11-09 and 2026-11-12 among them): %d/6\n", up_ok);

  /* 4. three sheets per install, ever. "Not now" three times: asks at sessions 1, 8 and 15 (7 apart), then none in
   * 40 sessions and none after a restart; the settings switch still turns sharing on. */
  memset(&rec, 0, sizeof rec);
  snprintf(d, sizeof d, "%s/notnow", dir);
  c = consent_client(d, &rec);
  int at[8], na = 0, reasks[8];
  for (int i = 1; i <= 40; ++i) {
    const int before = rec.calls;
    feedback_session(c);
    if (rec.calls != before) {
      if (na < 8) at[na] = i, reasks[na] = rec.reask;
      na++;
      tf__client_sharing_answer(c, 0);
    }
  }
  CHECK(na == 3 && at[0] == 1 && at[1] == 8 && at[2] == 15 && reasks[0] == 0 && reasks[1] == 1 && reasks[2] == 2,
        "Not now: %d asks, at %d %d %d", na, na > 0 ? at[0] : -1, na > 1 ? at[1] : -1, na > 2 ? at[2] : -1);
  CHECK(status_has(c, "\"consent\":\"final\"") && status_has(c, "\"asks\":3"), "after the third Not now: %s", tf__client_sharing_status_json(c));
  CHECK(tf__client_sharing_ask_now(c) == 0 && rec.calls == 3, "ask_now after three sheets");
  tf__client_close(c);
  c = consent_client(d, &rec);
  for (int i = 0; i < 30; ++i) feedback_session(c);
  CHECK(rec.calls == 3 && tf__client_sharing_state(c) == 0, "a fourth sheet after a restart: %d calls", rec.calls);
  tf__client_sharing_set(c, 1);
  CHECK(tf__client_sharing_state(c) == 1 && status_has(c, "\"consentVersion\":\"share-2026-11-09\""), "the settings switch after three Not now");
  tf__client_close(c);

  /* sheets left unanswered (no callback, the pending ask never answered) count too: three, 7 sessions apart */
  snprintf(d, sizeof d, "%s/unanswered", dir);
  c = consent_client(d, NULL);
  na = 0;
  for (int i = 1; i <= 40; ++i) {
    const int before = status_int(c, "asks");
    feedback_session(c);
    if (status_int(c, "asks") != before) {
      if (na < 8) at[na] = i;
      na++;
    }
  }
  CHECK(na == 3 && at[0] == 1 && at[1] == 8 && at[2] == 15, "unanswered: %d asks, at %d %d %d", na, na > 0 ? at[0] : -1, na > 1 ? at[1] : -1,
        na > 2 ? at[2] : -1);
  tf__client_close(c);
  c = consent_client(d, NULL);
  for (int i = 0; i < 20; ++i) feedback_session(c);
  tf__client_sharing_ask_now(c);
  CHECK(status_int(c, "asks") == 3, "unanswered after a restart: %d asks", status_int(c, "asks"));
  tf__client_close(c);

  /* sharing turned off before any sheet: never asked; the settings switch records the wording in force */
  memset(&rec, 0, sizeof rec);
  snprintf(d, sizeof d, "%s/off", dir);
  c = consent_client(d, &rec);
  tf__client_sharing_set(c, 0);
  for (int i = 0; i < 30; ++i) feedback_session(c);
  CHECK(tf__client_sharing_ask_now(c) == 0 && rec.calls == 0, "asked after sharing was turned off first: %d calls", rec.calls);
  tf_sharing_set_wording(c, kw, tw, 2);
  tf__client_sharing_set(c, 1);
  CHECK(session_version_is(c, d, custom_v, hash_v), "the settings switch under own wording records it");
  tf__client_close(c);

  /* the ask count carried over from 2.0.0: two sheets and a "Not now" leave one more ask at most, 7 sessions after
   * the last; 2.0.0's unanswered asks (it asked at every session) leave none */
  memset(&rec, 0, sizeof rec);
  snprintf(d, sizeof d, "%s/upgrade_asks", dir);
  {
    seed_config(d, "{\"contributions\":true}");
    char *sp = tf_path_join(d, "sharing.json");
    const char *st = "{\"v\":1,\"consent\":\"declined\",\"asks\":2,\"lastAskSession\":9,\"sessions\":12,\"thanked\":false,\"decidedAt\":null,"
                     "\"consentVersion\":null,\"guardian\":false,\"sharedSessions\":0,\"pendingAsk\":0}";
    tf_write_file(sp, st, strlen(st));
    free(sp);
  }
  c = consent_client(d, &rec);
  na = 0;
  for (int i = 13; i <= 50; ++i) {
    const int before = rec.calls;
    feedback_session(c);
    if (rec.calls != before) {
      if (na < 8) at[na] = i, reasks[na] = rec.reask;
      na++;
      tf__client_sharing_answer(c, 0);
    }
  }
  CHECK(na == 1 && at[0] == 16 && reasks[0] == 2 && status_has(c, "\"consent\":\"final\"") && status_has(c, "\"asks\":3"),
        "2.0.0's two asks: %d more, at %d, %s", na, na ? at[0] : -1, tf__client_sharing_status_json(c));
  tf__client_close(c);
  c = consent_client(d, &rec);
  for (int i = 0; i < 20; ++i) feedback_session(c);
  CHECK(rec.calls == 1, "2.0.0's two asks after a restart: %d calls", rec.calls);
  tf__client_close(c);
  memset(&rec, 0, sizeof rec);
  snprintf(d, sizeof d, "%s/upgrade_unanswered", dir);
  {
    seed_config(d, "{\"contributions\":true}");
    char *sp = tf_path_join(d, "sharing.json");
    const char *st = "{\"v\":1,\"consent\":\"unknown\",\"asks\":5,\"lastAskSession\":6,\"sessions\":6,\"thanked\":false,\"decidedAt\":null,"
                     "\"consentVersion\":null,\"guardian\":false,\"sharedSessions\":0,\"pendingAsk\":1}";
    tf_write_file(sp, st, strlen(st));
    free(sp);
  }
  c = consent_client(d, &rec);
  for (int i = 0; i < 30; ++i) feedback_session(c);
  tf__client_sharing_ask_now(c);
  CHECK(rec.calls == 0 && tf__client_sharing_state(c) == 0, "2.0.0's five unanswered asks: %d more", rec.calls);
  tf__client_close(c);
  free(rec.copy);
  /* 5. the decks: eight languages, version 2026-11-13.1, consent share-2026-11-09, the new keys, no {days} */
  static const char *const langs[] = {"ar", "bn", "en", "fr", "id", "ms", "tr", "ur"};
  int decks = 0, shown_ok = 0, clean = 0;
  /* SPEC 11.8: the sentence that deck 2026-11-12.1 added is in no string an app is handed.
   * The test data holds its length and SHA-256 per language and string, never its text
   * (tools/gen_removed_sentence.py, from the contract's archived deck) */
  char *gone_path = tf_path_join(vdir, "removed_2026-11-12.json");
  TfJson *gone_doc = read_json_file(gone_path);
  free(gone_path);
  const TfJson *gone = tf_json_get(gone_doc, "removed");
  CHECK(gone && gone->type == TJ_OBJ && gone->n == 8 && gone_entries(gone) == 16, "removed_2026-11-12.json does not hold a length and a hash for two strings in each of eight languages");
  for (int i = 0; i < 8; ++i) {
    const char *t = tf__sharing_deck_json(langs[i]);
    TfJson *j = t ? tf_json_parse(t, strlen(t), NULL) : NULL;
    const TfJson *s = tf_json_get(j, "strings");
    const int good = j && !strcmp(tf_json_str(tf_json_get(j, "lang")), langs[i]) && !strcmp(tf_json_str(tf_json_get(j, "version")), "2026-11-13.1") &&
                     !strcmp(tf_json_str(tf_json_get(j, "consent_version")), "share-2026-11-09") && tf_json_get(s, "recovery.keep") &&
                     tf_json_get(s, "delete.failed.network") && tf_json_get(s, "delete.retry") && !strstr(t, "{days}");
    CHECK(good, "deck %s", langs[i]);
    /* the bundled deck is the contract's file, byte for byte (sharing_copy/READY has these hashes) */
    static const char *const kDeckSha[] = {
      "03fc6aa2eed9134b5b5780ba2abcc5590c043024d0c6daf829c876742519f599",
      "e6b0a844386437838adcd0c3693ae39f93d40e5c3dba3b614c7f71574028a6b6",
      "3bac94a18f4691e7fb51021e9aa4f241d409a6dc4f8f89ec22a82761ebd75051",
      "9895b3eaf3e67afa250629b9ea3d93613d0298168d068fb6aba2e00450dda7ca",
      "f0621036f69d8f3754a0dfffea58b6c99e13a22c8356ba4cece0b20c3ba9abd0",
      "2c15cf8515885ef1bebe59117c61926dfd93ce406100fe2434e4b10d6c8e3e9d",
      "227783a34b2fdf6870ebd5d32eafb9e3ebcf496bed44797de87b571744200099",
      "755e2b9873180336b7e06bb44c0a9910441d8ee59dced42142c8f420ae77875a"};
    uint8_t dh[32];
    char dhex[65];
    tf_sha256(t ? t : "", t ? strlen(t) : 0, dh);
    tf_hex_encode(dh, 32, dhex);
    CHECK(!strcmp(dhex, kDeckSha[i]), "deck %s is not the contract's file: sha256 %s", langs[i], dhex);
    /* SPEC 11.8: what the app is handed to show (the request's copy_json for a client of this language) holds
     * the deck's sheet.who and what.detail whole, word for word */
    {
      char dd[1200];
      snprintf(dd, sizeof dd, "%s/who_%s", dir, langs[i]);
      AskRec wr;
      memset(&wr, 0, sizeof wr);
      TfClient *wc = consent_client_lang(dd, &wr, langs[i]);
      feedback_session(wc);
      TfJson *shown = wr.copy ? tf_json_parse(wr.copy, strlen(wr.copy), NULL) : NULL;
      const TfJson *ss2 = tf_json_get(shown, "strings");
      const char *who = tf_json_str(tf_json_get(s, "sheet.who")), *detail = tf_json_str(tf_json_get(s, "what.detail"));
      const char *who2 = tf_json_str(tf_json_get(ss2, "sheet.who")), *detail2 = tf_json_str(tf_json_get(ss2, "what.detail"));
      const int same = wr.calls == 1 && who && who2 && !strcmp(who, who2) && detail && detail2 && !strcmp(detail, detail2) && strlen(who) > 40 &&
                       !strcmp(wr.version, "share-2026-11-09");
      CHECK(same, "the sheet's lines for %s: %d asks, version %s, sheet.who %s", langs[i], wr.calls, wr.version, who2 ? "differs from the deck's" : "is missing");
      shown_ok += same;
      /* no window of any string of what the app is handed, nor of the bundled deck, has the length and the hash
       * of that sentence, in any of the eight languages */
      const int held = gone_in(shown, gone), held_deck = gone_in(j, gone);
      CHECK(shown && j && held == 0 && held_deck == 0, "%s: the sentence that deck 2026-11-12.1 added is in %d places of the copy handed over and %d of the bundled deck",
            langs[i], held, held_deck);
      clean += shown && j && held == 0 && held_deck == 0;
      tf_json_free(shown);
      tf__client_close(wc);
      free(wr.copy);
    }
    decks += good;
    tf_json_free(j);
  }
  CHECK(shown_ok == 8, "%d of 8 languages are handed the deck's sheet.who and what.detail whole", shown_ok);
  CHECK(clean == 8, "%d of 8 languages are handed a copy without the sentence that deck 2026-11-12.1 added", clean);
  tf_json_free(gone_doc);
  printf("copy decks: %d/8 at 2026-11-13.1 (share-2026-11-09), the contract's bytes, recovery and delete.failed keys, no {days}; the sheet's "
         "sheet.who and what.detail handed over whole: %d/8; without the sentence that deck 2026-11-12.1 added: %d/8\n", decks, shown_ok, clean);
  printf("consent (own text, a yes kept across decks, wording and upgrades, three sheets at most, session JSON, no capture before a grant, withdrawal, deletion): %s\n", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}

/* ================================================================== telemetry */
static struct {
  TfMutex m;
  int events_status, register_status;
  int n_events, n_register, n_register_new;
  char *bodies[64];
} g_ts;
static int tstub(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  const char *path = strstr(q->url, "/v1/");
  tf_mutex_lock(&g_ts.m);
  if (path && !strncmp(path, "/v1/attest", 10)) {
    reply(r, 200, "{\"token\":\"qlt_v1.test.sig\",\"expires_in\":86400,\"attested\":false}");
  } else if (path && !strncmp(path, "/v1/register", 12)) {
    g_ts.n_register++;
    if (q->body && strstr((const char *)q->body, "\"new_install\":true")) g_ts.n_register_new++;
    reply(r, g_ts.register_status ? g_ts.register_status : 200, "{\"ok\":true,\"app_id\":\"a\",\"store_status\":\"unknown\"}");
  } else if (path && !strncmp(path, "/v1/events", 10)) {
    if (g_ts.n_events < 64) {
      g_ts.bodies[g_ts.n_events] = (char *)malloc(q->body_n + 1);
      memcpy(g_ts.bodies[g_ts.n_events], q->body, q->body_n);
      g_ts.bodies[g_ts.n_events][q->body_n] = 0;
    }
    g_ts.n_events++;
    const int st = g_ts.events_status ? g_ts.events_status : 202;
    reply(r, st, st == 202 ? "{\"ok\":true,\"points\":1}" : "{\"error\":\"test\"}");
  } else if (path && !strncmp(path, "/v1/issues", 10)) {
    reply(r, 202, "{\"ok\":true}");
  } else {
    reply(r, 404, "{\"error\":\"not_found\"}");
  }
  tf_mutex_unlock(&g_ts.m);
  return 0;
}
static int events_seen(void) {
  tf_mutex_lock(&g_ts.m);
  const int n = g_ts.n_events;
  tf_mutex_unlock(&g_ts.m);
  return n;
}
static TfJson *event_body(int i) { return i >= 0 && i < 64 && g_ts.bodies[i] ? tf_json_parse(g_ts.bodies[i], strlen(g_ts.bodies[i]), NULL) : NULL; }
static long long jnum(const TfJson *o, const char *k) {
  const TfJson *v = tf_json_get(o, k);
  return v && v->type == TJ_NUM ? (long long)v->num : 0;
}
static TfClient *tele_client(const char *dir, int telemetry, const char *flags, void (*cb)(void *, const TfConsentRequest *), void *user) {
  tf_mkdirs(dir);
  if (flags) seed_config(dir, flags);
  TfClientConfig o = tf__client_default_config();
  o.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "org.example.Telemetry";
  o.platform = "linux";
  o.app_version = "3.2";
  o.api_url = "https://api.test";
  o.storage_dir = dir;
  o.telemetry = telemetry;
  o.on_consent = cb;
  o.user = user;
  return tf__client_open(&o);
}
static int count_files(const char *dir, const char *prefix) {
  char names[256][64];
  int n = 0, k = 0;
#ifdef _WIN32
  char pat[1100];
  snprintf(pat, sizeof pat, "%s\\%s*", dir, prefix);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat, &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do k++;
    while (FindNextFileA(h, &fd));
    FindClose(h);
  }
  (void)names;
  (void)n;
#else
  DIR *dd = opendir(dir);
  struct dirent *e;
  while (dd && (e = readdir(dd)))
    if (!strncmp(e->d_name, prefix, strlen(prefix))) k++;
  if (dd) closedir(dd);
  (void)names;
  (void)n;
#endif
  return k;
}
static void plain_session(TfClient *c) {
  TfSessionStats ss;
  memset(&ss, 0, sizeof ss);
  ss.audio_s = 5;
  ss.compute_s = 0.5;
  ss.chunks = 10;
  tf__client_session_finished(c, &ss);
}
static void t_ask(void *u, const TfConsentRequest *r) {
  (void)r;
  (*(int *)u)++;
}

static int telemetry_tests(const char *dir) {
  tf_mkdirs(dir);
  tf_mutex_init(&g_ts.m);
  tf_http_set_transport(tstub, NULL);
  tf__client_no_timer = 1;
  const long long D = 1794182400; /* 2026-11-09 00:00:00 UTC */
  char d[1100];

  /* 1. sealing: a 503 keeps the delta, the retry sends the same bytes (same report_id), a 202 deletes it */
  tf__now_override = D + 12 * 3600;
  snprintf(d, sizeof d, "%s/seal", dir);
  TfClient *c = tele_client(d, 1, NULL, NULL, NULL);
  g_ts.events_status = 503;
  CHECK(tf__client_background(c) == 0 && events_seen() == 1, "503: %d events", events_seen());
  TfJson *b0 = event_body(0);
  CHECK(b0 && is_hex32(tf_json_str(tf_json_get(b0, "report_id"))) && jnum(b0, "launches") == 1 && !strcmp(tf_json_str(tf_json_get(b0, "day")), "2026-11-09") &&
            !strcmp(tf_json_str(tf_json_get(b0, "sdk_version")), TF_SDK_VERSION) && tf_json_get(b0, "cohorts") && tf_json_get(b0, "cohorts")->n == 0,
        "the launch delta: %s", g_ts.bodies[0] ? g_ts.bodies[0] : "NULL");
  CHECK(count_files(d, "report-") == 1, "the delta is kept on disk");
  tf__client_background(c);
  CHECK(events_seen() == 2 && g_ts.bodies[1] && !strcmp(g_ts.bodies[0], g_ts.bodies[1]), "the retry is not the same bytes");
  g_ts.events_status = 202;
  CHECK(tf__client_background(c) == 1 && events_seen() == 3 && !strcmp(g_ts.bodies[0], g_ts.bodies[2]), "202: sent once more, same bytes");
  CHECK(count_files(d, "report-") == 0, "a 202 left the delta on disk");
  tf__client_background(c);
  CHECK(events_seen() == 3, "an empty delta was sent");
  /* a 400 drops (and queues the bad_request issue) */
  plain_session(c);
  g_ts.events_status = 400;
  tf__client_background(c);
  CHECK(events_seen() == 4 && count_files(d, "report-") == 0, "a 400 kept the delta");
  {
    char *ip = tf_path_join(d, "issues.json");
    char *t = read_text(ip, NULL);
    CHECK(t && strstr(t, "bad_request:events"), "no bad_request issue after a 400");
    free(t);
    free(ip);
  }
  /* 413 drops too */
  plain_session(c);
  g_ts.events_status = 413;
  tf__client_background(c);
  CHECK(events_seen() == 5 && count_files(d, "report-") == 0, "a 413 kept the delta");
  g_ts.events_status = 202;
  printf("deltas: 503 kept, retry byte-identical (report_id %s), 202 deleted, 400 and 413 dropped\n", tf_json_str(tf_json_get(b0, "report_id")));
  tf_json_free(b0);

  /* 2. no delta spans midnight */
  tf__now_override = D + 86400 - 60;
  plain_session(c);
  tf__now_override = D + 86400 + 60;
  plain_session(c);
  int e0 = events_seen();
  tf__client_background(c);
  CHECK(events_seen() == e0 + 2, "midnight: %d deltas", events_seen() - e0);
  {
    TfJson *x = event_body(e0), *y = event_body(e0 + 1);
    const TfJson *cx = tf_json_get(x, "cohorts"), *cy = tf_json_get(y, "cohorts");
    CHECK(x && y && !strcmp(tf_json_str(tf_json_get(x, "day")), "2026-11-09") && !strcmp(tf_json_str(tf_json_get(y, "day")), "2026-11-10") &&
              strcmp(tf_json_str(tf_json_get(x, "report_id")), tf_json_str(tf_json_get(y, "report_id"))) && cx && cx->n == 1 && cy && cy->n == 1 &&
              jnum(&cx->kids[0], "sessions") == 1 && jnum(&cy->kids[0], "sessions") == 1,
          "midnight: one delta per day, each with its own session");
    printf("midnight: %s with 1 session, %s with 1 session, different report_ids\n", tf_json_str(tf_json_get(x, "day")), tf_json_str(tf_json_get(y, "day")));
    tf_json_free(x);
    tf_json_free(y);
  }
  /* closing the client is a send point */
  plain_session(c);
  e0 = events_seen();
  tf__client_close(c);
  CHECK(events_seen() == e0 + 1, "close: %d deltas", events_seen() - e0);

  /* 3. the 15-minute send point (the next SDK call), then the client's own timer */
  tf__now_override = D + 10 * 3600;
  snprintf(d, sizeof d, "%s/fifteen", dir);
  c = tele_client(d, 1, NULL, NULL, NULL);
  tf__client_background(c);
  e0 = events_seen();
  tf__now_override += 60;
  plain_session(c);
  CHECK(count_files(d, "report-") == 0, "sealed before 15 minutes");
  tf__now_override += 15 * 60;
  plain_session(c);
  CHECK(count_files(d, "report-") == 1 && events_seen() == e0, "the 15-minute seal (%d sealed, %d sent)", count_files(d, "report-"), events_seen() - e0);
  CHECK(tf__client_background(c) == 1 && events_seen() == e0 + 1, "background sends the sealed delta");
  {
    TfJson *x = event_body(e0);
    const TfJson *cx = tf_json_get(x, "cohorts");
    CHECK(cx && cx->n == 1 && jnum(&cx->kids[0], "sessions") == 2, "the 15-minute delta holds both sessions");
    tf_json_free(x);
  }
  tf__client_close(c);
  tf__client_no_timer = 0;
  snprintf(d, sizeof d, "%s/timer", dir);
  c = tele_client(d, 1, NULL, NULL, NULL);
  tf__client_launch(c);
  e0 = events_seen();
  /* a session counted, then 15 minutes pass: whichever looks first (the timer thread, or the next call, which wakes
   * it), the timer sends exactly one delta */
  plain_session(c);
  tf__now_override += 15 * 60 + 1;
  plain_session(c);
  for (int i = 0; i < 500 && events_seen() == e0; ++i) SLEEP_MS(10);
  SLEEP_MS(100);
  CHECK(events_seen() == e0 + 1, "the timer sent %d deltas, want 1 (%d sealed on disk)", events_seen() - e0, count_files(d, "report-"));
  tf__client_close(c);
  tf__client_no_timer = 1;
  printf("send points: 15 minutes (sealed by the next call, sent by the timer), background, close, launch\n");

  /* 5. share counters: the pending ask the app's own sheet reads, and the ask callback */
  snprintf(d, sizeof d, "%s/share_pending", dir);
  c = tele_client(d, 1, "{\"contributions\":true}", NULL, NULL);
  tf__client_background(c);
  e0 = events_seen();
  feedback_session(c);
  CHECK(strstr(tf__client_sharing_status_json(c), "\"pendingAsk\":true"), "no pending ask");
  tf__client_sharing_answer(c, 0);
  tf__client_background(c);
  {
    TfJson *b = event_body(e0);
    CHECK(jnum(b, "share_asked") == 1 && jnum(b, "share_no") == 1 && !tf_json_get(b, "share_yes"), "pending ask counters: %s",
          g_ts.bodies[e0] ? g_ts.bodies[e0] : "NULL");
    tf_json_free(b);
  }
  tf__client_close(c);
  int asked = 0;
  snprintf(d, sizeof d, "%s/share_callback", dir);
  c = tele_client(d, 1, "{\"contributions\":true}", t_ask, &asked);
  tf__client_background(c);
  e0 = events_seen();
  feedback_session(c);
  tf__client_sharing_answer(c, 1);
  tf__client_sharing_set(c, 1); /* the settings switch: not a sheet answer */
  tf__client_background(c);
  {
    TfJson *b = event_body(e0);
    CHECK(asked == 1 && jnum(b, "share_asked") == 1 && jnum(b, "share_yes") == 1 && !tf_json_get(b, "share_no"), "callback counters: %s",
          g_ts.bodies[e0] ? g_ts.bodies[e0] : "NULL");
    tf_json_free(b);
  }
  tf__client_close(c);
  printf("share counters: pending ask asked 1 / no 1; ask callback asked 1 / yes 1 (the settings switch not counted)\n");

  /* 6. register once per UTC day; a failed attempt keeps new_install and tries again */
  tf__now_override = D + 3600;
  snprintf(d, sizeof d, "%s/register", dir);
  g_ts.n_register = g_ts.n_register_new = 0;
  c = tele_client(d, 1, NULL, NULL, NULL);
  tf__client_launch(c);
  tf__client_register(c);
  tf__client_close(c);
  c = tele_client(d, 1, NULL, NULL, NULL);
  tf__client_launch(c);
  CHECK(g_ts.n_register == 1 && g_ts.n_register_new == 1, "the same day: %d registers", g_ts.n_register);
  tf__now_override = D + 86400 + 3600;
  tf__client_launch(c);
  tf__client_register(c);
  CHECK(g_ts.n_register == 2 && g_ts.n_register_new == 1, "the next day: %d registers (%d with new_install)", g_ts.n_register, g_ts.n_register_new);
  tf__client_close(c);
  snprintf(d, sizeof d, "%s/register_fail", dir);
  g_ts.n_register = g_ts.n_register_new = 0;
  g_ts.register_status = 500;
  c = tele_client(d, 1, NULL, NULL, NULL);
  tf__client_launch(c);
  tf__client_register(c);
  g_ts.register_status = 200;
  tf__client_register(c);
  tf__client_register(c);
  CHECK(g_ts.n_register == 3 && g_ts.n_register_new == 3, "failed registers: %d calls, %d with new_install", g_ts.n_register, g_ts.n_register_new);
  tf__client_close(c);
  g_ts.register_status = 0;
  printf("register: once per UTC day (2 calls over two days), retried after a 500 with new_install kept\n");

  /* 7. the developer's opt-out: nothing counted, nothing sent */
  snprintf(d, sizeof d, "%s/off", dir);
  c = tele_client(d, 0, NULL, NULL, NULL);
  e0 = events_seen();
  plain_session(c);
  tf__client_background(c);
  tf__client_close(c);
  CHECK(events_seen() == e0 && count_files(d, "telemetry-") == 0 && count_files(d, "report-") == 0, "telemetry off sent or kept something");

  for (int i = 0; i < 64; ++i) free(g_ts.bodies[i]);
  tf_http_set_transport(NULL, NULL);
  tf__now_override = 0;
  printf("telemetry deltas: %s\n", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}

/* ================================================================== sdk_latest in the signed config */
static struct {
  TfJson *fx;
  const char *serve; /* the config served at /v1/config */
  int lines, level, config_calls;
  char last[800];
} g_sl;
static const TfJson *sl_config(const char *name) {
  const TfJson *cs = tf_json_get(g_sl.fx, "configs");
  for (int i = 0; cs && i < cs->n; ++i)
    if (!strcmp(tf_json_str(tf_json_get(&cs->kids[i], "name")), name)) return &cs->kids[i];
  return NULL;
}
static int sl_stub(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  const char *path = strstr(q->url, "/v1/");
  if (path && !strncmp(path, "/v1/attest", 10)) {
    reply(r, 200, "{\"token\":\"qlt_v1.test.sig\",\"expires_in\":86400,\"attested\":false}");
  } else if (path && !strncmp(path, "/v1/register", 12)) {
    reply(r, 200, "{\"ok\":true,\"app_id\":\"a\",\"store_status\":\"unknown\"}");
  } else if (path && !strncmp(path, "/v1/config", 10)) {
    g_sl.config_calls++;
    const TfJson *cfg = sl_config(g_sl.serve);
    if (!cfg) return tf__fail(TF__ERR_NETWORK, "no such test config");
    reply(r, 200, tf_json_str(tf_json_get(cfg, "body")));
    snprintf(r->signature, sizeof r->signature, "%s", tf_json_str(tf_json_get(cfg, "signature")));
    snprintf(r->key_id, sizeof r->key_id, "%s", tf_json_str(tf_json_get(g_sl.fx, "key_id")));
  } else {
    reply(r, 202, "{\"ok\":true,\"points\":1}");
  }
  return 0;
}
static void sl_log(void *u, int level, const char *msg) {
  (void)u;
  g_sl.lines++;
  g_sl.level = level;
  snprintf(g_sl.last, sizeof g_sl.last, "%s", msg);
}
static TfClient *sl_client(const char *dir, int debug) {
  TfClientConfig o = tf__client_default_config();
  o.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "org.example.Latest";
  o.platform = "linux";
  o.api_url = "https://api.test";
  o.storage_dir = dir;
  o.trusted_key_hex = tf_json_str(tf_json_get(g_sl.fx, "public_key_hex"));
  o.debug_build = debug;
  o.on_log = sl_log;
  return tf__client_open(&o);
}

/* sdk_latest: a newer version logs one warning per client (two configs, one line), in debug builds only; equal, older,
 * absent and malformed fields log nothing and are no error; an added top-level field passes verification (the
 * signature covers the raw bytes) and does not disturb the settings the config feeds */
static int sdk_latest_tests(const char *configs, const char *dir) {
  g_sl.fx = read_json_file(configs);
  if (!g_sl.fx) return 2;
  tf_mkdirs(dir);
  tf_http_set_transport(sl_stub, NULL);
  static const char want[] = "Quran Lab SDK 3.1.3 is available (recommended), you have " TF_SDK_VERSION ": https://github.com/Quran-Lab/tajweedformer-c/releases";
  char d[1100];
  /* newer: one line at launch; a second newer config (a refresh) adds none */
  snprintf(d, sizeof d, "%s/newer", dir);
  g_sl.serve = "newer";
  TfClient *c = sl_client(d, 1);
  int rc = tf__client_launch(c);
  CHECK(rc == 0 && tf__client_config_json(c) && strstr(tf__client_config_json(c), "\"sdk_latest\""), "the config with sdk_latest was not accepted: %d %s",
        rc, tf_last_error() ? tf_last_error() : "");
  CHECK(g_sl.lines == 1 && g_sl.level == TF_LOG_WARNING && !strcmp(g_sl.last, want), "newer: %d lines, level %d: %s", g_sl.lines, g_sl.level, g_sl.last);
  g_sl.serve = "newer major";
  CHECK(tf__client_refresh_config(c) == 0 && strstr(tf__client_config_json(c), "\"4.0\""), "the second config");
  CHECK(g_sl.lines == 1, "two configs gave %d lines", g_sl.lines);
  tf__client_close(c);
  printf("sdk_latest newer: \"%s\" once for two configs (warning level)\n", g_sl.last);
  /* the next launch (a new client) says it again */
  g_sl.lines = 0;
  c = sl_client(d, 1);
  tf__client_launch(c);
  CHECK(g_sl.lines == 1 && strstr(g_sl.last, "SDK 4.0 is available (required)"), "the next launch: %d lines: %s", g_sl.lines, g_sl.last);
  tf__client_close(c);
  /* a release build (debug_build 0): silent */
  g_sl.lines = 0;
  g_sl.serve = "newer";
  snprintf(d, sizeof d, "%s/release", dir);
  c = sl_client(d, 0);
  tf__client_launch(c);
  tf__client_refresh_config(c);
  CHECK(g_sl.lines == 0 && tf__client_config_json(c), "a release build logged %d lines", g_sl.lines);
  tf__client_close(c);
  /* equal, older, absent, malformed: nothing logged, no error, the config accepted */
  static const char *const quiet[] = {"equal", "older", "absent", "junk version", "prerelease version", "empty version", "version a number",
                                      "no severity", "no url", "url not a string", "not an object", "null"};
  int ok = 0;
  for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; ++i) {
    g_sl.lines = 0;
    g_sl.serve = quiet[i];
    snprintf(d, sizeof d, "%s/quiet%d", dir, (int)i);
    c = sl_client(d, 1);
    rc = tf__client_launch(c);
    const int good = rc == 0 && tf__client_config_json(c) != NULL && g_sl.lines == 0;
    CHECK(good, "%s: rc %d, %d lines (%s)", quiet[i], rc, g_sl.lines, g_sl.last);
    ok += good;
    tf__client_close(c);
  }
  printf("sdk_latest quiet: %d/%d (equal, older, absent, malformed: no line, no error, config accepted); release build: no line\n", ok,
         (int)(sizeof quiet / sizeof quiet[0]));
  tf_http_set_transport(NULL, NULL);
  tf_json_free(g_sl.fx);
  printf("sdk_latest: %s\n", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}
