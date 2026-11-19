/* The decoded layer's two pure rankings, on heads from anywhere: top-K and top-2 (API_LAYERS.md).
 *
 * SDK 4.0: the engine is inside the closed Core, whose interface gives heads and greedy ids only. These two
 * functions are the 3.x engine's own ranking (tajweedformer-sdk 070b0ab tajweed/src/decoded.c, zf_tajweed_topk and
 * zf_tajweed_top2), carried here unchanged so the decoded layer keeps its bits: per row, k passes of a selection
 * over the entries not yet ranked; the next entry is the largest value, the lower id on a tie (a strictly greater
 * value replaces the candidate, scanning ids upwards), which is a stable argsort of the negated row. NaN ranks after
 * every number. The conformance run checks both against a plain C scan and a stable argsort on every clip. */
#include <stddef.h>

#include "tf_internal.h"

#define TF_TOPK_MAX 64

int tf__topk_rows(const float *values, int frames, int cols, int k, int *ids, float *values_out, float *margins) {
  if (!values || !ids || frames < 0 || cols < 1 || cols > TF_TOPK_MAX || k < 1 || k > cols) return -1;
  const int passes = k < cols ? k + 1 : k;
  for (int f = 0; f < frames; ++f) {
    const float *row = values + (size_t)f * cols;
    unsigned char taken[TF_TOPK_MAX] = {0};
    int rank[TF_TOPK_MAX + 1];
    for (int j = 0; j < passes; ++j) {
      int best = -1;
      for (int v = 0; v < cols; ++v) {
        if (taken[v]) continue;
        if (best < 0 || row[v] > row[best] || (row[best] != row[best] && row[v] == row[v])) best = v;
      }
      taken[best] = 1;
      rank[j] = best;
    }
    int *io = ids + (size_t)f * k;
    for (int j = 0; j < k; ++j) {
      io[j] = rank[j];
      if (values_out) values_out[(size_t)f * k + j] = row[rank[j]];
      if (margins) margins[(size_t)f * k + j] = j + 1 < cols ? row[rank[j]] - row[rank[j + 1]] : 0.0f;
    }
  }
  return frames;
}

int tf__top2(const float *lp, int frames, int vocab, TfTop2 *out) {
  if (!lp || !out || frames < 0 || vocab < 2) return -1;
  for (int f = 0; f < frames; ++f) {
    int id[2];
    float val[2], mg[2];
    if (tf__topk_rows(lp + (size_t)f * vocab, 1, vocab, 2, id, val, mg) != 1) return -1;
    out[f].token = id[0];
    out[f].runner_up = id[1];
    out[f].log_prob = val[0];
    out[f].runner_up_log_prob = val[1];
    out[f].margin = val[0] - val[1];
  }
  return frames;
}
