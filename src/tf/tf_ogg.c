/* The shared-session Ogg Opus muxer (PLATFORM_CLIENT.md section 10; RFC 7845), written exactly as the Swift
 * reference (tajweedformer-sdk-swift Sharing/OggOpus.swift) so every SDK writes the same bytes for the same
 * packets: one stream per session; page 0 OpusHead (BOS), page 1 OpusTags; audio
 * pages of up to 50 packets (fewer when 255 lacing values would not hold them), granule = 960 x packets paged;
 * segments cut at page boundaries after 500, 1000, 2000, then 3000 packets (10, 20, 40, then 60 s); the last page
 * EOS with the end-trimmed granule. The encoder is a seam: tf_ogg_packet takes encoded packets. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"
#include "tf_ogg.h"

static uint32_t crc_table[256];
static int crc_ready;
static uint32_t ogg_crc(const uint8_t *d, size_t n) {
  if (!crc_ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t r = i << 24;
      for (int k = 0; k < 8; ++k) r = r & 0x80000000u ? (r << 1) ^ 0x04c11db7u : r << 1;
      crc_table[i] = r;
    }
    crc_ready = 1;
  }
  uint32_t c = 0;
  for (size_t i = 0; i < n; ++i) c = (c << 8) ^ crc_table[((c >> 24) & 0xff) ^ d[i]];
  return c;
}

/* seconds per segment: 10, 20, 40, then 60 (tests shorten it; the cut is always at a page boundary) */
int tf__share_plan[4] = {10, 20, 40, 60};

static int seg_limit(int n) { return tf__share_plan[(n < 4 ? n : 4) - 1] * 1000 / 20; }

static int write_page(TfOgg *o, uint8_t *const *pk, const int *len, int count, long long granule, uint8_t flags, int audio) {
  uint8_t lace[255];
  int nl = 0;
  size_t body = 0;
  for (int i = 0; i < count; ++i) {
    int n = len[i];
    while (n >= 255) {
      lace[nl++] = 255;
      n -= 255;
    }
    lace[nl++] = (uint8_t)n;
    body += (size_t)len[i];
  }
  const size_t total = 27 + (size_t)nl + body;
  uint8_t *d = (uint8_t *)malloc(total);
  if (!d) return -1;
  memcpy(d, "OggS", 4);
  d[4] = 0;
  d[5] = flags;
  for (int i = 0; i < 8; ++i) d[6 + i] = (uint8_t)((unsigned long long)granule >> (8 * i));
  for (int i = 0; i < 4; ++i) d[14 + i] = (uint8_t)(o->serial >> (8 * i));
  for (int i = 0; i < 4; ++i) d[18 + i] = (uint8_t)(o->sequence >> (8 * i));
  memset(d + 22, 0, 4);
  d[26] = (uint8_t)nl;
  memcpy(d + 27, lace, (size_t)nl);
  size_t at = 27 + (size_t)nl;
  for (int i = 0; i < count; ++i) {
    memcpy(d + at, pk[i], (size_t)len[i]);
    at += (size_t)len[i];
  }
  const uint32_t crc = ogg_crc(d, total);
  for (int i = 0; i < 4; ++i) d[22 + i] = (uint8_t)(crc >> (8 * i));
  if (!o->file) {
    o->segment++;
    o->segment_packets = 0;
    char name[64];
    snprintf(name, sizeof name, "seg-%05d.opus", o->segment);
    char *p = tf_path_join(o->dir, name);
    o->file = p ? fopen(p, "wb") : NULL;
    free(p);
    if (!o->file) {
      free(d);
      return -1;
    }
  }
  /* flushed page by page: after a crash the file ends at a page boundary, or one torn page the outbox cuts off */
  const int ok = fwrite(d, 1, total, o->file) == total && fflush(o->file) == 0;
  free(d);
  if (!ok) return -1;
  o->sequence++;
  o->last_granule = granule;
  if (audio) {
    o->paged += count;
    o->segment_packets += count;
    if (!(flags & 0x04) && o->segment_packets >= seg_limit(o->segment)) {
      fclose(o->file);
      o->file = NULL;
      if (o->on_segment) o->on_segment(o->user, o->segment, 0);
    }
  }
  return 0;
}

int tf_ogg_open(TfOgg *o, const char *dir, uint32_t serial, int pre_skip, int input_rate, const char *vendor) {
  memset(o, 0, sizeof *o);
  snprintf(o->dir, sizeof o->dir, "%s", dir);
  o->serial = serial;
  o->pre_skip = pre_skip;
  uint8_t head[19];
  memcpy(head, "OpusHead", 8);
  head[8] = 1;
  head[9] = 1;
  head[10] = (uint8_t)pre_skip;
  head[11] = (uint8_t)(pre_skip >> 8);
  for (int i = 0; i < 4; ++i) head[12 + i] = (uint8_t)((uint32_t)input_rate >> (8 * i));
  head[16] = head[17] = 0;
  head[18] = 0;
  const size_t vl = strlen(vendor);
  uint8_t *tags = (uint8_t *)malloc(16 + vl);
  if (!tags) return -1;
  memcpy(tags, "OpusTags", 8);
  for (int i = 0; i < 4; ++i) tags[8 + i] = (uint8_t)((uint32_t)vl >> (8 * i));
  memcpy(tags + 12, vendor, vl);
  memset(tags + 12 + vl, 0, 4);
  uint8_t *p0 = head, *p1 = tags;
  int l0 = 19, l1 = (int)(16 + vl);
  int rc = write_page(o, &p0, &l0, 1, 0, 0x02, 0);
  if (!rc) rc = write_page(o, &p1, &l1, 1, 0, 0, 0);
  free(tags);
  return rc;
}

long long tf_ogg_segment_start(int n) {
  long long at = 0;
  for (int k = 1; k < n; ++k) at += seg_limit(k);
  return at;
}

/* page the queued packets; `final`: everything, the last page EOS with `final_granule` (RFC 7845 end trim);
 * `more`: everything too, because the caller knows more packets follow (a segment boundary) */
static int write_pages(TfOgg *o, int final, long long final_granule, int more) {
  while (o->n_q > 0) {
    const int next = o->file ? o->segment : o->segment + 1;
    const int used = o->file ? o->segment_packets : 0;
    const int room = seg_limit(next) - used;
    const int want = room < 50 ? room : 50;
    int take = 0, lacing = 0;
    while (take < (want < o->n_q ? want : o->n_q)) {
      const int l = o->q_len[take] / 255 + 1;
      if (lacing + l > 255) break;
      lacing += l;
      take++;
    }
    /* a page is written before the end only when packets follow it, so the last page is always the EOS page
     * (the same bytes as muxing the whole session at once, as the vector's generator does) */
    if (!final && !more && take >= o->n_q) return 0;
    const int last = final && take == o->n_q;
    long long granule = (o->paged + take) * 960LL;
    if (last) {
      long long g = final_granule < granule ? final_granule : granule;
      if (g < o->paged * 960LL) g = o->paged * 960LL;
      granule = g;
    }
    if (write_page(o, o->q, o->q_len, take, granule, last ? 0x04 : 0, 1)) return -1;
    for (int i = 0; i < take; ++i) free(o->q[i]);
    memmove(o->q, o->q + take, sizeof(o->q[0]) * (size_t)(o->n_q - take));
    memmove(o->q_len, o->q_len + take, sizeof(o->q_len[0]) * (size_t)(o->n_q - take));
    o->n_q -= take;
  }
  return 0;
}

int tf_ogg_packet(TfOgg *o, const uint8_t *data, int n) {
  if (o->n_q >= TF_OGG_QUEUE) return -1;
  uint8_t *p = (uint8_t *)malloc((size_t)n + 1);
  if (!p) return -1;
  memcpy(p, data, (size_t)n);
  o->q[o->n_q] = p;
  o->q_len[o->n_q++] = n;
  return o->n_q > 50 ? write_pages(o, 0, 0, 0) : 0;
}

/* Called when the next packet is the first of a new segment (so packets follow): the queued packets, all of
 * the closing segment, are paged now and the segment closes (on_segment) before the next one's first packet is
 * encoded. The pages are the same bytes as without the call: the segment's room already ends a page there. */
int tf_ogg_flush(TfOgg *o) { return write_pages(o, 0, 0, 1); }

int tf_ogg_finish(TfOgg *o, long long final_granule) {
  const int rc = write_pages(o, 1, final_granule, 0);
  if (o->file) {
    fclose(o->file);
    o->file = NULL;
    if (o->on_segment) o->on_segment(o->user, o->segment, 1);
  }
  return rc;
}

void tf_ogg_abandon(TfOgg *o) {
  if (o->file) fclose(o->file);
  o->file = NULL;
  for (int i = 0; i < o->n_q; ++i) free(o->q[i]);
  o->n_q = 0;
}
