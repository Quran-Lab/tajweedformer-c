/* The shared-session Ogg Opus muxer (internal). */
#ifndef TF_OGG_H
#define TF_OGG_H
#include <stdint.h>
#include <stdio.h>

#define TF_OGG_QUEUE 512
typedef void (*TfOggSegmentFn)(void *user, int n, int last);
typedef struct {
  char dir[1024];
  uint32_t serial, sequence;
  int pre_skip;
  int segment, segment_packets;
  long long paged, last_granule;
  FILE *file;
  uint8_t *q[TF_OGG_QUEUE];
  int q_len[TF_OGG_QUEUE];
  int n_q;
  TfOggSegmentFn on_segment;
  void *user;
} TfOgg;

/* Writes seg-00001.opus ... into `dir`. */
int tf_ogg_open(TfOgg *o, const char *dir, uint32_t serial, int pre_skip, int input_rate, const char *vendor);
int tf_ogg_packet(TfOgg *o, const uint8_t *data, int n);
/* the packets queued are paged now (more follow: a segment boundary); the closing segment's on_segment runs */
int tf_ogg_flush(TfOgg *o);
/* the packet index (from 0) that starts segment n (1-based) under the segment plan */
long long tf_ogg_segment_start(int n);
int tf_ogg_finish(TfOgg *o, long long final_granule);
void tf_ogg_abandon(TfOgg *o);
#endif
