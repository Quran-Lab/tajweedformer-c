/* The audio side of recitation sharing (internal): the 48 kHz resampler and the Opus encoder, the same as the
 * web SDK's web/sharing/resampler.js and encoder.js (and the Swift reference's ShareResampler / LibOpusEncoder
 * contracts): PLATFORM_CLIENT.md section 10.2.
 *
 * Resampler: a streaming rational polyphase resampler with a Kaiser-windowed sinc (32 zero crossings a side,
 * beta 9), silence assumed before the first sample (no leading delay), output exactly ceil(n * 48000 / rate)
 * once flushed. Arithmetic in double, the web port's order, so both write the same samples.
 *
 * Encoder: libopus, application AUDIO (2049), the bitrate given (the adaptive ladder's rung: 192000, 96000, 64000 or
 * 32000; the capture changes it with tf_enc_set_bitrate before the first packet of a segment), complexity 10,
 * VBR unconstrained, fullband, signal auto, lsb depth 24, no FEC, no DTX, 20 ms frames (960 samples at 48 kHz).
 * Pre-skip = the encoder's lookahead (312 at 48 kHz); the flush encodes the buffered samples and the lookahead,
 * padded with silence to whole packets; the final granule = pre-skip + the samples encoded. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <opus.h>

#include "tf_share.h"

/* ------------------------------------------------------------------ resampler */
#define RS_ZEROS 32
#define RS_BETA 9.0

static long gcd_l(long a, long b) {
  while (b) {
    const long t = a % b;
    a = b;
    b = t;
  }
  return a;
}
static double bessel_i0(double x) {
  double s = 1, t = 1;
  for (int k = 1; k < 60; ++k) {
    t *= (x / (2 * k)) * (x / (2 * k));
    s += t;
    if (t < 1e-21 * s) break;
  }
  return s;
}

int tf_rs_init(TfResampler *r, int input_rate) {
  memset(r, 0, sizeof *r);
  if (input_rate < 8000 || input_rate > 384000) return -1;
  const long g = gcd_l(48000, input_rate);
  r->L = 48000 / g;
  r->M = input_rate / g;
  const double fc = (r->L < r->M ? (double)r->L / (double)r->M : 1.0) * 0.97;
  r->H = (int)ceil(RS_ZEROS / fc);
  const int H = r->H;
  r->taps = (double *)malloc(sizeof(double) * (size_t)r->L * 2 * (size_t)H);
  if (!r->taps) return -1;
  const double i0b = bessel_i0(RS_BETA);
  const double PI = 3.141592653589793;
  for (long p = 0; p < r->L; ++p) {
    const double frac = (double)p / (double)r->L;
    double *row = r->taps + (size_t)p * 2 * (size_t)H;
    for (int j = 0; j < 2 * H; ++j) {
      const double t = (double)(j - (H - 1)) - frac;
      const double x = t * fc;
      const double sinc = x == 0 ? 1 : sin(PI * x) / (PI * x);
      const double rr = t / H;
      const double w = fabs(rr) >= 1 ? 0 : bessel_i0(RS_BETA * sqrt(1 - rr * rr)) / i0b;
      row[j] = fc * sinc * w;
    }
  }
  r->cap = 4096 + 2 * H;
  r->buf = (double *)calloc((size_t)r->cap, sizeof(double));
  if (!r->buf) {
    free(r->taps);
    return -1;
  }
  r->len = H - 1;
  r->base = -(long long)(H - 1);
  return 0;
}
void tf_rs_free(TfResampler *r) {
  free(r->taps);
  free(r->buf);
  memset(r, 0, sizeof *r);
}
static int rs_push(TfResampler *r, const float *x, const double *xd, int n) {
  if (r->len + n > r->cap) {
    int nc = r->cap * 2 > r->len + n ? r->cap * 2 : r->len + n;
    double *nb = (double *)realloc(r->buf, sizeof(double) * (size_t)nc);
    if (!nb) return -1;
    r->buf = nb;
    r->cap = nc;
  }
  for (int i = 0; i < n; ++i) r->buf[r->len + i] = x ? (double)x[i] : xd[i];
  r->len += n;
  return 0;
}
/* every output whose window lies inside the buffer (or up to `limit` outputs in all when limit >= 0) */
static int rs_run(TfResampler *r, long long limit, TfSampleSink sink, void *user) {
  const long L = r->L, M = r->M;
  const int H = r->H;
  float out[1024];
  int n = 0;
  for (;;) {
    if (limit >= 0 && r->k >= limit) break;
    const long long num = r->k * M;
    const long long i = num / L;
    const long p = (long)(num - i * L);
    const long long lo = i - (H - 1) - r->base;
    if (lo + 2 * H > r->len) break;
    const double *row = r->taps + (size_t)p * 2 * (size_t)H;
    const double *b = r->buf + lo;
    double s = 0;
    for (int j = 0; j < 2 * H; ++j) s += b[j] * row[j];
    out[n++] = (float)s;
    if (n == 1024) {
      if (sink(user, out, n)) return -1;
      n = 0;
    }
    r->k++;
  }
  if (n && sink(user, out, n)) return -1;
  const long long need = (r->k * M) / L - (H - 1) - r->base;
  if (need > 4096) {
    memmove(r->buf, r->buf + need, sizeof(double) * (size_t)(r->len - need));
    r->len -= (int)need;
    r->base += need;
  }
  return 0;
}
int tf_rs_process(TfResampler *r, const float *x, int n, TfSampleSink sink, void *user) {
  if (r->ended || n <= 0) return 0;
  r->in_count += n;
  if (rs_push(r, x, NULL, n)) return -1;
  return rs_run(r, -1, sink, user);
}
int tf_rs_flush(TfResampler *r, TfSampleSink sink, void *user) {
  if (r->ended) return 0;
  r->ended = 1;
  const long long total = (r->in_count * r->L + r->M - 1) / r->M;
  const int z = 2 * r->H + 1;
  double *zeros = (double *)calloc((size_t)z, sizeof(double));
  if (!zeros) return -1;
  const int rc = rs_push(r, NULL, zeros, z);
  free(zeros);
  return rc ? rc : rs_run(r, total, sink, user);
}

/* ------------------------------------------------------------------ encoder */
int tf_enc_init(TfEncoder *e, int bitrate) {
  memset(e, 0, sizeof *e);
  int err = 0;
  OpusEncoder *enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &err);
  if (!enc || err != OPUS_OK) return -1;
  /* the Swift/web order of requests */
  const int req[9][2] = {
      {OPUS_SET_BITRATE_REQUEST, bitrate},          {OPUS_SET_COMPLEXITY_REQUEST, 10},
      {OPUS_SET_VBR_REQUEST, 1},                    {OPUS_SET_VBR_CONSTRAINT_REQUEST, 0},
      {OPUS_SET_BANDWIDTH_REQUEST, OPUS_BANDWIDTH_FULLBAND}, {OPUS_SET_SIGNAL_REQUEST, OPUS_AUTO},
      {OPUS_SET_LSB_DEPTH_REQUEST, 24},             {OPUS_SET_INBAND_FEC_REQUEST, 0},
      {OPUS_SET_DTX_REQUEST, 0},
  };
  for (int i = 0; i < 9; ++i)
    if (opus_encoder_ctl(enc, req[i][0], (opus_int32)req[i][1]) != OPUS_OK) {
      opus_encoder_destroy(enc);
      return -1;
    }
  opus_int32 look = 0;
  opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&look));
  e->enc = enc;
  e->pre_skip = (int)look; /* 48 kHz in, so already in 48 kHz samples */
  e->bitrate = bitrate;
  return 0;
}
int tf_enc_set_bitrate(TfEncoder *e, int bitrate) {
  if (!e->enc) return -1;
  if (bitrate == e->bitrate) return 0;
  if (opus_encoder_ctl((OpusEncoder *)e->enc, OPUS_SET_BITRATE_REQUEST, (opus_int32)bitrate) != OPUS_OK) return -1;
  e->bitrate = bitrate;
  return 0;
}
void tf_enc_free(TfEncoder *e) {
  if (e->enc) opus_encoder_destroy((OpusEncoder *)e->enc);
  e->enc = NULL;
}
static int enc_frame(TfEncoder *e, TfPacketSink sink, void *user) {
  unsigned char out[4000];
  if (e->before && e->before(e->before_user, e->packets)) return -1;
  e->packets++;
  const int n = opus_encode_float((OpusEncoder *)e->enc, e->frame, TF_SHARE_FRAME, out, (opus_int32)sizeof out);
  e->filled = 0;
  if (n <= 0) return -1;
  return sink(user, out, n);
}
int tf_enc_encode(TfEncoder *e, const float *pcm, int n, TfPacketSink sink, void *user) {
  if (e->finished) return -1;
  int i = 0;
  while (i < n) {
    int k = TF_SHARE_FRAME - e->filled;
    if (k > n - i) k = n - i;
    for (int j = 0; j < k; ++j) {
      const float v = pcm[i + j];
      e->frame[e->filled + j] = isfinite(v) ? v : 0.0f;
    }
    e->filled += k;
    i += k;
    if (e->filled == TF_SHARE_FRAME && enc_frame(e, sink, user)) return -1;
  }
  e->samples_in += n;
  return 0;
}
int tf_enc_flush(TfEncoder *e, TfPacketSink sink, void *user) {
  if (e->finished) return -1;
  e->finished = 1;
  int owed = e->filled + e->pre_skip;
  while (owed > 0) {
    for (int j = e->filled; j < TF_SHARE_FRAME; ++j) e->frame[j] = 0.0f;
    e->filled = TF_SHARE_FRAME;
    if (enc_frame(e, sink, user)) return -1;
    owed -= TF_SHARE_FRAME;
  }
  return 0;
}
long long tf_enc_final_granule(const TfEncoder *e) { return (long long)e->pre_skip + e->samples_in; }
const char *tf_opus_version(void) { return opus_get_version_string(); }
