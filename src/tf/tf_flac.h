/* Shared recordings in the lossless form (SHARING_40.md section 19; internal): the FLAC frame encoder of 19.4.3 and
 * the Ogg FLAC muxer of 19.4.1, 19.4.2 and 19.4.4. */
#ifndef TF_FLAC_H
#define TF_FLAC_H
#include <stdint.h>
#include <stdio.h>

#define TF_FLAC_BLOCK 4096

/* 19.3: a float sample to the 16-bit integer the encoder takes (x * 32768, NaN 0, clamped, a tie to the even one) */
int16_t tf_pcm16(float x);

/* One frame of a mono 16-bit block of 1 to 4096 samples, numbered `number`, at `rate`: its bytes in *out (the caller
 * frees) and its kind (0 constant, 1 verbatim, 2 + order fixed). 0, or -1 (no memory). */
int tf_flac_frame(const int16_t *x, int n, uint32_t number, int rate, uint8_t **out, size_t *out_n, int *kind);

/* 19.4.4: the frames of segment k (1, 2, ...): ceil(T * rate / 4096), T 10, 20, 40, then 60 seconds */
long long tf_flac_frames_per_segment(int rate, int k);

/* a segment closed: its number and the granule of its last page */
typedef void (*TfFlacSegmentFn)(void *user, int n, long long granule);
typedef struct {
  char dir[1024], ext[8];
  uint32_t serial, seq, frame_no;
  int rate;
  int segment, seg_frames;   /* the segment written now (from 1) and its frames so far */
  FILE *file;
  int16_t block[TF_FLAC_BLOCK];
  int nb;                    /* samples waiting for a whole block */
  uint8_t *held;             /* the newest frame, held until the next one exists or the stream stops (19.4.4) */
  size_t held_n;
  long long held_granule, granule;
  int kinds[7];              /* frames written by kind (tests) */
  TfFlacSegmentFn on_segment;
  void *user;
} TfFlacMux;

/* Writes seg-00001<ext>, seg-00002<ext>, ... into `dir`; segment 1 starts with the two header pages now. 0 or -1. */
int tf_flac_open(TfFlacMux *m, const char *dir, const char *ext, uint32_t serial, int rate, const char *vendor);
int tf_flac_push(TfFlacMux *m, const int16_t *x, long long n);
/* the stop: the samples left over as the last frame, and the end-of-stream page; the last segment closes. A stream
 * with no sample has no audio page: -1, and the caller drops the session. */
int tf_flac_finish(TfFlacMux *m);
void tf_flac_abandon(TfFlacMux *m);
#endif
