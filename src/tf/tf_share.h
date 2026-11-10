/* Recitation sharing internals: resampler, encoder, capture, outbox (internal). */
#ifndef TF_SHARE_H
#define TF_SHARE_H
#include <stdint.h>

#define TF_SHARE_FRAME 960 /* 20 ms at 48 kHz */

typedef int (*TfSampleSink)(void *user, const float *x, int n);
typedef int (*TfPacketSink)(void *user, const uint8_t *p, int n);

typedef struct {
  long L, M;
  int H;
  double *taps;
  double *buf;
  int len, cap;
  long long base, k, in_count;
  int ended;
} TfResampler;
int tf_rs_init(TfResampler *r, int input_rate);
void tf_rs_free(TfResampler *r);
int tf_rs_process(TfResampler *r, const float *x, int n, TfSampleSink sink, void *user);
int tf_rs_flush(TfResampler *r, TfSampleSink sink, void *user);

/* called before each packet is encoded, with the packet's index from 0 (the capture opens a segment there and
 * sets its bitrate); nonzero stops the encode */
typedef int (*TfPacketHook)(void *user, long long packet);
typedef struct {
  void *enc;
  int pre_skip;
  float frame[TF_SHARE_FRAME];
  int filled;
  long long samples_in;
  int finished;
  long long packets;     /* packets encoded so far */
  int bitrate;           /* the bitrate in force (bit/s) */
  TfPacketHook before;   /* optional */
  void *before_user;
} TfEncoder;
int tf_enc_init(TfEncoder *e, int bitrate);
/* OPUS_SET_BITRATE for the packets encoded from now on (the capture calls it from the hook, before a segment's
 * first packet, so every packet of a segment has one bitrate) */
int tf_enc_set_bitrate(TfEncoder *e, int bitrate);
void tf_enc_free(TfEncoder *e);
int tf_enc_encode(TfEncoder *e, const float *pcm, int n, TfPacketSink sink, void *user);
int tf_enc_flush(TfEncoder *e, TfPacketSink sink, void *user);
long long tf_enc_final_granule(const TfEncoder *e);
const char *tf_opus_version(void);

/* the segment plan in seconds (10, 20, 40, then 60); tests may shorten it */
extern int tf__share_plan[4];
#endif
