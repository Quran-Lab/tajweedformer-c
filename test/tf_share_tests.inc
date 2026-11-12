/* Included by tf_tests.c: the shared-session upload (PLATFORM_CLIENT.md section 10). The codec against the web
 * SDK's encode vector, the 48 kHz
 * resampler's contract, and the upload through the library's API against a stub qlab-api and R2 (a test HTTP
 * transport): presigned batches of 20, the next batch at 5 left, a 403 PUT asked again, PUTs with content-type
 * audio/ogg only, a session left by a crash recovered and uploaded at the next launch, sharing off aborting the PUT
 * in flight and deleting the queue, and contributor_key_mismatch rotating the contributor. */
#include <math.h>
#include <time.h>
#include "tf_share.h"
#include "tf_thread.h"
#ifdef _WIN32
#define SLEEP_MS(x) Sleep(x)
#else
#define SLEEP_MS(x) usleep((x) * 1000)
#endif
void tf__share_crash(TfShareSession *x);

static void lcg_noise(float *x, int n, double gain) {
  uint64_t s = 1;
  for (int i = 0; i < n; ++i) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const float u = (float)(s >> 40) / 16777216.0f * 2.0f - 1.0f;
    x[i] = u * (float)gain;
  }
}
static int pk_sink(void *user, const uint8_t *p, int n) { return tf_ogg_packet((TfOgg *)user, p, n); }
typedef struct {
  float *x;
  int n, cap;
} Pcm;
static int pcm_sink(void *user, const float *x, int n) {
  Pcm *p = (Pcm *)user;
  if (p->n + n > p->cap) return -1;
  memcpy(p->x + p->n, x, sizeof(float) * (size_t)n);
  p->n += n;
  return 0;
}

/* Ogg pages of a byte stream: CRC, BOS on the first, EOS only on the last, granules never going back */
static int check_ogg(const uint8_t *d, size_t n, int want_eos, long long *last_granule, int *pages_out) {
  size_t at = 0;
  int pages = 0, bad = 0;
  long long g_prev = 0;
  int eos_seen = 0;
  while (at < n) {
    if (at + 27 > n || memcmp(d + at, "OggS", 4)) return -1;
    const int ns = d[at + 26];
    size_t body = 0;
    for (int i = 0; i < ns; ++i) body += d[at + 27 + i];
    const size_t len = 27 + (size_t)ns + body;
    if (at + len > n) return -1;
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
    if (c != want) bad++;
    const uint8_t flags = d[at + 5];
    if ((pages == 0) != !!(flags & 0x02)) bad++;
    if (eos_seen) bad++;
    if (flags & 0x04) eos_seen = 1;
    long long g = 0;
    for (int i = 7; i >= 0; --i) g = (g << 8) | d[at + 6 + i];
    if (g < g_prev) bad++;
    g_prev = g;
    pages++;
    at += len;
  }
  if (want_eos >= 0 && eos_seen != want_eos) bad++;
  if (last_granule) *last_granule = g_prev;
  if (pages_out) *pages_out = pages;
  return bad;
}

static int codec_tests(const char *vdir, const char *dir) {
  /* 1. the web encode vector: 3 s of the Swift tests' noise at 48 kHz, serial 42 */
  const int n = 48000 * 3;
  float *x = (float *)malloc(sizeof(float) * (size_t)n);
  lcg_noise(x, n, 0.3);
  tf_mkdirs(dir);
  TfEncoder e;
  TfOgg o;
  CHECK(tf_enc_init(&e, TF_SHARING_BITRATE_DEFAULT) == 0, "opus encoder");
  CHECK(e.pre_skip == 312, "pre-skip %d", e.pre_skip);
  char vendor[96];
  snprintf(vendor, sizeof vendor, "QuranLab SDK 2.0.0 %s", tf_opus_version());
  CHECK(!strcmp(tf_opus_version(), "libopus 1.6.1"), "libopus version %s", tf_opus_version());
  CHECK(tf_ogg_open(&o, dir, 42, e.pre_skip, 48000, vendor) == 0, "ogg open");
  CHECK(tf_enc_encode(&e, x, n, pk_sink, &o) == 0, "encode");
  CHECK(tf_enc_flush(&e, pk_sink, &o) == 0, "flush");
  CHECK(tf_ogg_finish(&o, tf_enc_final_granule(&e)) == 0, "ogg finish");
  tf_enc_free(&e);
  char *a = tf_path_join(dir, "seg-00001.opus"), *b = tf_path_join(vdir, "web_noise48k_3s.ogg"), *pa = NULL, *pb = NULL;
  size_t na = 0, nb = 0;
  const int same = !tf_read_file(a, &pa, &na) && !tf_read_file(b, &pb, &nb) && na == nb && !memcmp(pa, pb, na);
  long long g = 0;
  int pages = 0;
  CHECK(pa && check_ogg((const uint8_t *)pa, na, 1, &g, &pages) == 0, "the encoded stream is not valid Ogg");
  CHECK(g == 312 + n, "final granule %lld, want %d", g, 312 + n);
  printf("encoder vs the web vector (web_noise48k_3s.ogg, %llu bytes): %s (ours %llu bytes, %d pages)\n", (unsigned long long)nb,
         same ? "byte-identical" : "DIFFERENT", (unsigned long long)na, pages);
  CHECK(same, "the encoded bytes differ from the web vector");
  free(a);
  free(b);
  free(pa);
  free(pb);

  /* 2. the resampler's contract (the web port's): 44.1 kHz in, exactly ceil(n * 48000 / 44100) out, aligned, a
   * 1 kHz tone through it above 60 dB SNR away from the edges */
  const int in_n = 44100;
  float *tone = (float *)malloc(sizeof(float) * (size_t)in_n);
  for (int i = 0; i < in_n; ++i) tone[i] = (float)(0.5 * sin(2 * 3.141592653589793 * 1000.0 * i / 44100.0));
  TfResampler rs;
  Pcm out = {(float *)malloc(sizeof(float) * 60000), 0, 60000};
  CHECK(tf_rs_init(&rs, 44100) == 0, "resampler");
  for (int i = 0; i < in_n; i += 441) tf_rs_process(&rs, tone + i, 441, pcm_sink, &out);
  tf_rs_flush(&rs, pcm_sink, &out);
  tf_rs_free(&rs);
  CHECK(out.n == 48000, "resampled %d samples, want 48000", out.n);
  double sig = 0, err = 0;
  for (int i = 2000; i < 46000; ++i) {
    const double ref = 0.5 * sin(2 * 3.141592653589793 * 1000.0 * i / 48000.0);
    sig += ref * ref;
    err += (out.x[i] - ref) * (out.x[i] - ref);
  }
  const double snr = 10 * log10(sig / (err > 0 ? err : 1e-30));
  printf("resampler 44.1 -> 48 kHz: %d samples, tone SNR %.1f dB\n", out.n, snr);
  CHECK(snr > 60, "tone SNR %.1f dB", snr);
  free(tone);
  free(out.x);
  free(x);
  return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ the stub qlab-api and R2 */
typedef struct {
  char id[33];
  uint8_t *seg[400];
  size_t seg_n[400];
  int max_n;
} StubSession;
static struct {
  TfMutex m;
  int net_down;
  int fail_403_once_at;  /* a PUT of this segment answers 403 once */
  int put_status;        /* nonzero: every PUT answers this status (the network failing: 503) */
  int stall_puts;        /* PUTs wait until cancelled */
  volatile int put_waiting, put_cancelled;
  const char *start_error; /* POST /v1/uploads answers 409 with this code */
  StubSession s[8];
  int n_s;
  int starts, url_calls, puts, put_403, put_failed;
  int url_from[64];
  int bad_put_headers, bad_session_body;
  /* 2.0.3: POST /v1/uploads/{id}/takes. The k-th call answers takes_status[k] (0: 204; -401: 401 with an error
   * code other than invalid_token); while takes_net_down > 0 a call finds no connection. */
  int takes_calls, takes_status[16], takes_net_down, bad_takes, attests;
  char *takes_body[16];
  char takes_id[16][33], takes_contributor[16][33];
  int takes_after_puts[16]; /* segment PUTs answered 200 before the call */
  char start_contributor[33]; /* the contributor of the last session JSON */
  /* 3.0.1, sharing off during an upload: the first request of kind `hold` ("put", "start", "urls", "takes") waits
   * until `release` or until it is cancelled; once `mark` is set, every upload request that STARTS is counted,
   * and the bytes of audio of a PUT that starts */
  const char *hold;
  int hold_deaf; /* the held request cannot be aborted: it does not look at its cancel flag */
  volatile int held, held_done, held_cancelled, release, mark;
  int after_requests;
  long long after_audio;
  int delete_calls;
  int delete_status; /* nonzero: the deletion answers this status (the server does not take it) */
  /* when the deletion's first request arrived: the upload requests started since `mark`, and whether the held
   * request had ended by then */
  int ask_seen, ask_requests, ask_held_done;
  /* the settings file as it is on disk at that moment (what a process that dies there leaves): `snap_path` is
   * copied into `snap` */
  const char *snap_path;
  char *snap;
  size_t snap_n;
} g_st;
static long long stub_file_size(const char *path) {
  FILE *f = path ? fopen(path, "rb") : NULL;
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fclose(f);
  return n > 0 ? n : 0;
}
static StubSession *stub_session(const char *id) {
  for (int i = 0; i < g_st.n_s; ++i)
    if (!strcmp(g_st.s[i].id, id)) return &g_st.s[i];
  if (g_st.n_s >= 8) return NULL;
  StubSession *s = &g_st.s[g_st.n_s++];
  memset(s, 0, sizeof *s);
  snprintf(s->id, sizeof s->id, "%s", id);
  return s;
}
static void reply(TfHttpResponse *r, int status, const char *body) {
  r->status = status;
  r->body_n = strlen(body);
  r->body = (char *)malloc(r->body_n + 1);
  memcpy(r->body, body, r->body_n + 1);
}
static void urls_body(TfBuf *b, const char *id, int from) {
  tf_buf_printf(b, "{\"id\":\"%s\",\"urls\":[", id);
  for (int n = from; n < from + 20; ++n)
    tf_buf_printf(b, "%s{\"n\":%d,\"url\":\"https://r2.test/qlab-contributions/v1/%s/seg-%05d.opus?X-Amz-Signature=x\",\"expires_at\":%lld}",
                  n > from ? "," : "", n, id, n, (long long)time(NULL) + 7 * 86400);
  tf_buf_puts(b, "]}");
}
static const char *hdr_value(const TfHttpRequest *q, const char *name) {
  const size_t L = strlen(name);
  for (const char *const *h = q->headers; h && *h; ++h)
    if (!_tf_strnicmp(*h, name, L) && (*h)[L] == ':') return *h + L + 2;
  return NULL;
}
static int stub(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  if (g_st.net_down) return tf__fail(TF__ERR_NETWORK, "the network is down (test)");
  const char *path = strstr(q->url, "://");
  path = path ? strchr(path + 3, '/') : NULL;
  if (!path) return tf__fail(TF__ERR_NETWORK, "bad url");
  tf_mutex_lock(&g_st.m);
  int rc = 0;
  {
    const char *kind = strstr(q->url, "https://r2.test/") ? "put"
                       : !strcmp(path, "/v1/uploads") ? "start"
                       : !strncmp(path, "/v1/uploads/", 12) && strlen(path) == 50 && !strcmp(path + 44, "/takes") ? "takes"
                       : !strncmp(path, "/v1/uploads/", 12) ? "urls" : NULL;
    if (kind && g_st.mark) {
      g_st.after_requests++;
      if (!strcmp(kind, "put")) g_st.after_audio += stub_file_size(q->upload_file);
    }
    if (kind && g_st.hold && !g_st.held && !strcmp(kind, g_st.hold)) {
      g_st.held = 1;
      tf_mutex_unlock(&g_st.m);
      int cancelled = 0;
      for (int i = 0; i < 1400 && !g_st.release && !cancelled; ++i) {
        cancelled = !g_st.hold_deaf && q->cancel && *q->cancel;
        if (!cancelled) SLEEP_MS(5);
      }
      tf_mutex_lock(&g_st.m);
      g_st.held_cancelled = cancelled;
      g_st.held_done = 1;
      /* a held PUT that was let go after the call, not cancelled by it: its audio left after the call */
      if (!cancelled && g_st.mark && !strcmp(kind, "put")) g_st.after_audio += stub_file_size(q->upload_file);
      if (cancelled) {
        tf_mutex_unlock(&g_st.m);
        return tf__fail(TF__ERR_NETWORK, "cancelled");
      }
    }
  }
  if (!strcmp(path, "/v1/contributions/challenge")) {
    if (!g_st.ask_seen) {
      g_st.ask_seen = 1;
      g_st.ask_requests = g_st.after_requests;
      g_st.ask_held_done = !g_st.held || g_st.held_done;
      if (g_st.snap_path && !g_st.snap) tf_read_file(g_st.snap_path, &g_st.snap, &g_st.snap_n);
    }
    reply(r, 200, "{\"nonce\":\"nonce-for-the-test\"}");
  } else if (!strcmp(path, "/v1/contributions/delete")) {
    g_st.delete_calls++;
    reply(r, g_st.delete_status ? g_st.delete_status : 202, "{}");
  } else if (strstr(q->url, "https://r2.test/")) {
    /* a presigned PUT: content-type audio/ogg and nothing else */
    int nh = 0;
    for (const char *const *h = q->headers; h && *h; ++h) nh++;
    const char *ct = hdr_value(q, "content-type");
    if (strcmp(q->method, "PUT") || nh != 1 || !ct || strcmp(ct, "audio/ogg")) g_st.bad_put_headers++;
    char id[33] = "";
    int n = 0;
    const char *v = strstr(q->url, "/v1/");
    if (v) sscanf(v + 4, "%32[0-9a-f]/seg-%d.opus", id, &n);
    if (g_st.stall_puts) {
      g_st.put_waiting = 1;
      tf_mutex_unlock(&g_st.m);
      for (int i = 0; i < 400 && !(q->cancel && *q->cancel); ++i) SLEEP_MS(10);
      tf_mutex_lock(&g_st.m);
      g_st.put_cancelled = q->cancel && *q->cancel;
      rc = tf__fail(TF__ERR_NETWORK, "cancelled");
    } else if (g_st.put_status) {
      g_st.put_failed++;
      reply(r, g_st.put_status, "<Error>unavailable</Error>");
    } else if (n == g_st.fail_403_once_at) {
      g_st.fail_403_once_at = 0;
      g_st.put_403++;
      reply(r, 403, "<Error>expired</Error>");
    } else {
      StubSession *s = stub_session(id);
      char *data = NULL;
      size_t dn = 0;
      if (s && n > 0 && n < 400 && q->upload_file && !tf_read_file(q->upload_file, &data, &dn)) {
        free(s->seg[n]);
        s->seg[n] = (uint8_t *)data;
        s->seg_n[n] = dn;
        if (n > s->max_n) s->max_n = n;
        g_st.puts++;
        reply(r, 200, "");
      } else {
        free(data);
        reply(r, 400, "bad put");
      }
    }
  } else if (!strcmp(path, "/v1/attest")) {
    g_st.attests++;
    reply(r, 200, "{\"token\":\"qlt_v1.test.sig\",\"expires_in\":86400,\"attested\":false}");
  } else if (!strncmp(path, "/v1/uploads/", 12) && strlen(path) == 50 && !strcmp(path + 44, "/takes")) {
    /* 2.0.3, the take list of an uploaded recording */
    const int k = g_st.takes_calls++;
    const char *ctr = hdr_value(q, "x-qlab-contributor"), *ct = hdr_value(q, "content-type");
    if (strcmp(q->method, "POST") || !hdr_value(q, "authorization") || !ctr || !ct || strcmp(ct, "application/json")) g_st.bad_takes++;
    if (k < 16) {
      snprintf(g_st.takes_id[k], 33, "%.32s", path + 12);
      snprintf(g_st.takes_contributor[k], 33, "%s", ctr ? ctr : "");
      g_st.takes_body[k] = (char *)malloc(q->body_n + 1);
      memcpy(g_st.takes_body[k], q->body, q->body_n);
      g_st.takes_body[k][q->body_n] = 0;
      g_st.takes_after_puts[k] = g_st.puts;
    }
    const int st = k < 16 ? g_st.takes_status[k] : 0;
    if (g_st.takes_net_down > 0) {
      g_st.takes_net_down--;
      rc = tf__fail(TF__ERR_NETWORK, "the network is down (test)");
    } else if (!st) {
      reply(r, 204, "");
    } else {
      reply(r, st < 0 ? -st : st,
            st == 404 ? "{\"error\":\"not_found\"}" : st == 410 ? "{\"error\":\"upload_deleted\"}" : st == 401 ? "{\"error\":\"invalid_token\"}"
            : st == -401 ? "{\"error\":\"token_expired\"}" : st == 400 ? "{\"error\":\"invalid_body\"}" : "{\"error\":\"unavailable\"}");
    }
  } else if (!strcmp(path, "/v1/uploads")) {
    g_st.starts++;
    const char *id = hdr_value(q, "x-qlab-id");
    TfJson *j = tf_json_parse((const char *)q->body, q->body_n, NULL);
    const char *ck = tf_json_str(tf_json_get(j, "contributor_key")), *rh = tf_json_str(tf_json_get(j, "recovery_hash"));
    const char *sc = tf_json_str(tf_json_get(j, "contributor"));
    snprintf(g_st.start_contributor, sizeof g_st.start_contributor, "%s", sc ? sc : "");
    const TfJson *audio = tf_json_get(j, "audio");
    if (!ck || strlen(ck) != 87 || !rh || strlen(rh) != 64 || !tf_json_get(j, "format") || !audio ||
        tf_json_get(audio, "sample_rate")->num != 48000 || !hdr_value(q, "authorization") || !id || strlen(id) != 32)
      g_st.bad_session_body++;
    /* sorted keys at the top level */
    for (int i = 1; j && i < j->n; ++i)
      if (strcmp(j->kids[i - 1].key, j->kids[i].key) >= 0) g_st.bad_session_body++;
    tf_json_free(j);
    if (g_st.start_error) {
      char b[160];
      snprintf(b, sizeof b, "{\"error\":\"%s\",\"message\":\"test\"}", g_st.start_error);
      reply(r, 409, b);
    } else {
      stub_session(id);
      TfBuf b;
      tf_buf_init(&b);
      urls_body(&b, id, 1);
      reply(r, 201, b.p);
      tf_buf_free(&b);
    }
  } else if (!strncmp(path, "/v1/uploads/", 12)) {
    char id[33] = "";
    int from = 0;
    sscanf(path + 12, "%32[0-9a-f]/urls?from=%d", id, &from);
    if (g_st.url_calls < 64) g_st.url_from[g_st.url_calls] = from;
    g_st.url_calls++;
    if (!hdr_value(q, "x-qlab-contributor")) g_st.bad_session_body++;
    TfBuf b;
    tf_buf_init(&b);
    urls_body(&b, id, from);
    reply(r, 200, b.p);
    tf_buf_free(&b);
  } else {
    reply(r, 404, "{\"error\":\"not_found\"}");
  }
  tf_mutex_unlock(&g_st.m);
  return rc;
}
static void stub_reset(void) {
  for (int i = 0; i < g_st.n_s; ++i)
    for (int k = 0; k < 400; ++k) free(g_st.s[i].seg[k]);
  for (int k = 0; k < 16; ++k) free(g_st.takes_body[k]);
  TfMutex m = g_st.m;
  memset(&g_st, 0, sizeof g_st);
  g_st.m = m;
}
/* the uploaded segments of a session joined: one Ogg Opus stream */
static int joined_ok(const StubSession *s, int want_eos, int *segments) {
  size_t n = 0;
  for (int k = 1; k <= s->max_n; ++k) n += s->seg_n[k];
  uint8_t *d = (uint8_t *)malloc(n + 1);
  size_t at = 0;
  int missing = 0;
  for (int k = 1; k <= s->max_n; ++k) {
    if (!s->seg[k]) missing++;
    else memcpy(d + at, s->seg[k], s->seg_n[k]);
    at += s->seg_n[k];
  }
  int pages = 0;
  const int bad = missing ? -1 : check_ogg(d, n, want_eos, NULL, &pages);
  free(d);
  *segments = s->max_n;
  return bad == 0 && s->max_n > 0;
}

/* 3.0: a recording waits 1500 ms after its take for a tail, also with a join window of 0. The tests written for
 * 2.0.2 want the recording closed when the take ends: they say so, as an app that knows no take follows. */
int tf__share_open(TfClient *c, char id[33], int *takes, long long *samples);
void tf__share_window_check(TfClient *c);
extern long long tf__share_now_ms;
static int share_finish(TfClient *c, TfShareSession *take, int completed) {
  const int ms = tf__sharing_session_finish(take, completed);
  tf__sharing_recording_close(c);
  return ms;
}

static TfClient *share_client(const char *dir) {
  /* 2.0.1: offered only where the signed config says so. 2.0.3: a join window of 0, so a take's recording closes
   * when the take ends, as these tests (written for 2.0.2) expect; joined takes are in tf_203_tests.inc */
  seed_config(dir, "{\"contributions\":true,\"share_join_ms\":0}");
  TfClientConfig o = tf__client_default_config();
  o.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "org.example.ShareTest";
  o.platform = "linux";
  o.api_url = "https://api.test";
  o.storage_dir = dir;
  o.telemetry = 0;
  return tf__client_open(&o);
}

static int upload_tests(const char *dir) {
  tf_mutex_init(&g_st.m);
  tf_mkdirs(dir);
  tf_http_set_transport(stub, NULL);
  tf__share_plan[0] = tf__share_plan[1] = tf__share_plan[2] = tf__share_plan[3] = 1; /* 1 s segments */
  const int rate = 48000;
  float *x = (float *)malloc(sizeof(float) * (size_t)rate * 31);
  lcg_noise(x, rate * 31, 0.2);

  /* A: a session left by a crash (the network down, the process gone mid-session) goes up at the next launch */
  char *da = tf_path_join(dir, "a");
  TfClient *c = share_client(da);
  CHECK(c && tf__client_sharing_answer(c, 1) == 0 && tf__client_sharing_state(c), "consent");
  CHECK(strstr(tf__client_sharing_status_json(c), "\"uploadTrust\":\"capped\"") && strstr(tf__client_sharing_status_json(c), "\"mayUpload\":true"),
        "desktop default tier: %s", tf__client_sharing_status_json(c));
  g_st.net_down = 1;
  TfShareSession *ss = tf__sharing_session_begin(c, rate, "usb");
  CHECK(ss != NULL, "session begin: %s", tf_last_error());
  for (int i = 0; i < 175; ++i) tf__sharing_session_push(ss, x + i * 960, 960); /* 3.5 s */
  tf__share_crash(ss);
  CHECK(tf__sharing_outbox_pending(c) == 1, "one session pending after the crash");
  tf__client_close(c);
  g_st.net_down = 0;
  c = share_client(da);
  int left = tf__sharing_outbox_wait(c, 20000);
  CHECK(left == 0, "%d sessions still pending after the restart", left);
  int segs = 0;
  CHECK(g_st.n_s == 1 && joined_ok(&g_st.s[0], 0, &segs), "the recovered session uploaded as one Ogg stream (no EOS)");
  CHECK(segs >= 3, "recovered session: %d segments", segs);
  printf("crash recovery: %d segments uploaded after the restart, one valid Ogg stream\n", segs);
  CHECK(strstr(tf__client_sharing_status_json(c), "\"sharedSessions\":1"), "shared sessions counted: %s", tf__client_sharing_status_json(c));
  tf__client_close(c);

  /* B: a 30 s session: two URL batches (the next one asked for at 5 left), a 403 asked again, every segment PUT */
  stub_reset();
  g_st.fail_403_once_at = 3;
  char *db = tf_path_join(dir, "b");
  c = share_client(db);
  tf__client_sharing_answer(c, 1);
  tf__sharing_outbox_resume(c);
  ss = tf__sharing_session_begin(c, rate, NULL);
  for (int i = 0; i < 30 * 50; ++i) tf__sharing_session_push(ss, x + i * 960, 960);
  const int ms = share_finish(c, ss, 1);
  CHECK(ms == 30000, "shared %d ms", ms);
  left = tf__sharing_outbox_wait(c, 30000);
  CHECK(left == 0, "%d sessions still pending", left);
  CHECK(g_st.n_s == 1 && joined_ok(&g_st.s[0], 1, &segs), "the session's segments joined are one Ogg stream ending with EOS");
  int batch21 = 0, reask3 = 0; /* the next batch: after the 403's re-ask (3..22) it starts at 23, else at 21 */
  for (int i = 0; i < g_st.url_calls && i < 64; ++i) {
    batch21 += g_st.url_from[i] > 20;
    reask3 += g_st.url_from[i] == 3;
  }
  CHECK(g_st.starts == 1, "%d session starts", g_st.starts);
  CHECK(batch21 == 1, "the next batch (from > 20) asked %d times", batch21);
  CHECK(reask3 == 1 && g_st.put_403 == 1, "the 403 asked again from 3: %d", reask3);
  CHECK(!g_st.bad_put_headers && !g_st.bad_session_body, "put headers %d, bodies %d", g_st.bad_put_headers, g_st.bad_session_body);
  printf("30 s session: %d segments, %d URL calls (the next batch once, from=3 after the 403), %d PUTs\n", segs, g_st.url_calls, g_st.puts);
  tf__client_close(c);

  /* C: sharing off during a PUT: the PUT is aborted and the queue deleted at once */
  stub_reset();
  g_st.stall_puts = 1;
  char *dc = tf_path_join(dir, "c");
  c = share_client(dc);
  tf__client_sharing_answer(c, 1);
  ss = tf__sharing_session_begin(c, rate, NULL);
  for (int i = 0; i < 150; ++i) tf__sharing_session_push(ss, x + i * 960, 960);
  share_finish(c, ss, 1);
  tf__sharing_outbox_resume(c);
  for (int i = 0; i < 500 && !g_st.put_waiting; ++i) SLEEP_MS(10);
  CHECK(g_st.put_waiting, "a PUT was in flight");
  const double t0 = tf__now_s();
  tf__client_sharing_set(c, 0);
  for (int i = 0; i < 300 && !g_st.put_cancelled; ++i) SLEEP_MS(10);
  CHECK(g_st.put_cancelled, "the PUT in flight was not aborted");
  CHECK(tf__sharing_outbox_pending(c) == 0, "the queue was not deleted");
  printf("sharing off: the PUT in flight aborted after %.0f ms, %d sessions left\n", (tf__now_s() - t0) * 1000, tf__sharing_outbox_pending(c));
  CHECK(tf__sharing_session_begin(c, rate, NULL) == NULL, "a session began with sharing off");
  tf__client_close(c);

  /* D: contributor_key_mismatch: the session is dropped and the contributor, key and code are made anew */
  stub_reset();
  g_st.start_error = "contributor_key_mismatch";
  char *dd = tf_path_join(dir, "d");
  c = share_client(dd);
  tf__client_sharing_answer(c, 1);
  ss = tf__sharing_session_begin(c, rate, NULL);
  for (int i = 0; i < 100; ++i) tf__sharing_session_push(ss, x + i * 960, 960);
  share_finish(c, ss, 1);
  char code1[33] = "", code2[33] = "";
  tf__client_sharing_recovery_code(c, code1);
  left = tf__sharing_outbox_wait(c, 10000);
  CHECK(left == 0 && g_st.starts == 1, "dropped after contributor_key_mismatch (%d left, %d starts)", left, g_st.starts);
  CHECK(tf__client_sharing_recovery_code(c, code2) != 0, "the old contributor and recovery code were not forgotten");
  g_st.start_error = NULL;
  ss = tf__sharing_session_begin(c, rate, NULL);
  CHECK(ss && tf__client_sharing_recovery_code(c, code2) == 0 && strcmp(code1, code2), "a new contributor and recovery code");
  share_finish(c, ss, 0);
  tf__client_close(c);

  /* 2.0.1: the ladder decides; the old bitrate call checks its argument and changes nothing */
  c = share_client(dd);
  CHECK(tf__sharing_set_bitrate(c, 24000) == 0 && tf__sharing_set_bitrate(c, 1000) != 0 && tf__sharing_bitrate(c) == 192000,
        "bitrate call: %d", tf__sharing_bitrate(c));
  tf__client_close(c);

  stub_reset();
  tf_http_set_transport(NULL, NULL);
  free(x);
  free(da);
  free(db);
  free(dc);
  free(dd);
  printf("shared-session upload: %s\n", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}

/* the Session's hook: a Session of a Model that belongs to a client shares each take while consent is on (16 kHz
 * input, so the resampler runs), the take's heads are unchanged by it, and nothing is shared without consent */
static int session_share_tests(const char *folder, const char *key, const char *list, const char *dir) {
  FILE *lf = fopen(list, "r");
  char line[4096] = "";
  if (!lf || !fgets(line, sizeof line, lf)) return 2;
  fclose(lf);
  size_t len = strlen(line);
  while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
  char *pcm = NULL;
  size_t bytes = 0;
  if (tf_read_file(line, &pcm, &bytes)) return 2;
  const int n = (int)(bytes / 4);
  tf_mutex_init(&g_st.m);
  stub_reset();
  tf_http_set_transport(stub, NULL);
  tf__share_plan[0] = 10, tf__share_plan[1] = 20, tf__share_plan[2] = 40, tf__share_plan[3] = 60;
  TfClient *c = share_client(dir);
  TfModelOpen mo;
  memset(&mo, 0, sizeof mo);
  mo.trusted_key_hex = strcmp(key, "-") ? key : NULL;
  mo.allow_unsigned = strcmp(key, "-") == 0;
  mo.client = c;
  TfModel *m = tf__model_open(folder, &mo);
  if (!m) return 2;
  int greedy[2][512], ng[2] = {0, 0};
  for (int pass = 0; pass < 2; ++pass) {
    if (pass == 1) tf__client_sharing_answer(c, 1); /* the first take without consent, the second with it */
    TfSession *s = tf_session_create(m, &(TfSessionOptions){.input_route = "wired"});
    CHECK(s != NULL, "session: %s", tf_last_error());
    if (!s) return 2;
    for (int at = 0; at < n; at += 320) tf_session_push(s, (const float *)pcm + at, n - at < 320 ? n - at : 320);
    tf_session_finish(s);
    const TfToken *tk = NULL;
    ng[pass] = tf_heads_greedy(tf_session_heads(s), NULL, &tk);
    for (int i = 0; i < ng[pass] && i < 512; ++i) greedy[pass][i] = tk[i].id;
    long long held = -1;
    if (pass == 1) {
      /* the take has ended: only fed audio could still enter its recording. A push into the ended Session is
       * refused and adds nothing; the recording waits for its tail with exactly the take's samples */
      CHECK(tf_session_push(s, (const float *)pcm, 320) == TF_ERR_SESSION_CLOSED && tf__share_open(c, NULL, NULL, &held) && held == n,
            "after the take ended its recording holds %lld samples, the take had %d", held, n);
      tf__clear_error();
    }
    tf_session_destroy(s);
    if (pass == 1) {
      /* nothing is fed, the 1500 ms run out: the recording closes with the take's samples and nothing else */
      held = -1;
      CHECK(tf__share_open(c, NULL, NULL, &held) && held == n, "after the Session was destroyed its recording holds %lld samples, the take had %d", held, n);
      tf__share_now_ms = (long long)time(NULL) * 1000 + 1000 + TF_SHARING_TAIL_WAIT_MS + 1;
      tf__share_window_check(c);
      tf__share_now_ms = 0;
      CHECK(!tf__share_open(c, NULL, NULL, NULL), "the recording is still open after its tail wait");
    }
    CHECK(tf__sharing_outbox_pending(c) == pass, "pending after take %d: %d", pass + 1, tf__sharing_outbox_pending(c));
  }
  CHECK(ng[0] == ng[1] && !memcmp(greedy[0], greedy[1], sizeof(int) * (size_t)ng[0]), "sharing changed what the engine heard");
  const int left = tf__sharing_outbox_wait(c, 20000);
  int segs = 0;
  CHECK(left == 0 && g_st.n_s == 1 && joined_ok(&g_st.s[0], 1, &segs), "the shared take (%d left, %d sessions)", left, g_st.n_s);
  long long g = 0;
  int pages = 0;
  if (g_st.n_s == 1 && g_st.s[0].seg[1]) {
    check_ogg(g_st.s[0].seg[1], g_st.s[0].seg_n[1], -1, &g, &pages);
    const uint8_t *h = g_st.s[0].seg[1] + 28; /* OpusHead in page 0 */
    const int in_rate = h[12] | h[13] << 8 | h[14] << 16 | h[15] << 24;
    CHECK(in_rate == 16000, "OpusHead input rate %d", in_rate);
    const long long want = 312 + (long long)n * 3; /* 16 kHz -> 48 kHz, plus the pre-skip */
    CHECK(llabs(g - want) <= 960, "final granule %lld, want about %lld", g, want);
    printf("session hook: a %.2f s take at 16 kHz shared as %d segment(s), granule %lld (want %lld); heads unchanged\n", n / 16000.0, segs, g, want);
  }
  tf_model_close(m);
  tf__client_close(c);
  tf_http_set_transport(NULL, NULL);
  stub_reset();
  free(pcm);
  return fails ? 1 : 0;
}
