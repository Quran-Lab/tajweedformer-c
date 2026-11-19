/* Shared recordings in the lossless form (SHARING_40.md section 19): a session captured at any rate but 48 kHz goes
 * up as one Ogg FLAC stream at its capture rate, mono, 16-bit, while the signed config's flags.share_flac is true.
 *
 * The frame (19.4.3) is libFLAC 1.5.0's fixed-predictor path written out in integers, as the contract's reference
 * (tools/sharing40.py encode_frame) has it: block size 4096; the wasted bits shared by every sample; verbatim, or
 * constant, or the fixed predictor of the order its sums of |error| pick, with Rice partitions of order 0 to 4 whose
 * parameters come from each partition's sum without a search; the smallest estimate wins, a later kind only when
 * strictly smaller. Integer arithmetic only, so every SDK on every CPU writes the same bytes (the vectors pin them).
 *
 * The stream (19.4.1, 19.4.2): page 0 the mapping packet with STREAMINFO (total samples and MD5 unknown), page 1 the
 * comment packet (vendor "QuranLab SDK <version>", no comment), then one page per frame, granule = the samples through
 * that frame, the last page end-of-stream. Segments (19.4.4): ceil(T * rate / 4096) frames for T 10, 20, 40, then 60
 * seconds; the newest frame's page is held until the next frame exists or the stream stops, so a segment closes once
 * it has its frames and the frame after them exists, the last segment always ends with the end-of-stream page, and no
 * segment or page is ever empty.
 *
 * Tried and rejected (the section's 19.10): libFLAC's LPC levels (floating point with SIMD and FMA: not the same bytes
 * on every CPU); native FLAC (no end mark: a session could never be marked complete). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tf_flac.h"
#include "tf_internal.h"

#define BPS 16
#define MAX_PARTITION_ORDER 4
#define U32MAX 0xFFFFFFFFull

/* ------------------------------------------------------------------ 19.3 */
int16_t tf_pcm16(float x) {
  if (x != x) return 0;
  const float v = x * 32768.0f; /* exact: a power of two */
  if (v >= 32767.0f) return 32767;
  if (v <= -32768.0f) return -32768;
  return (int16_t)lrintf(v); /* the default rounding: to nearest, a tie to the even one */
}

/* ------------------------------------------------------------------ CRCs (RFC 9639 9.1.8, 9.3; RFC 3533 6) */
static uint8_t crc8(const uint8_t *b, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; ++i) {
    c ^= b[i];
    for (int k = 0; k < 8; ++k) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
  }
  return c;
}
static uint16_t crc16(const uint8_t *b, size_t n) {
  static uint16_t t[256];
  if (!t[1])
    for (int i = 0; i < 256; ++i) {
      uint16_t r = (uint16_t)(i << 8);
      for (int k = 0; k < 8; ++k) r = (uint16_t)(r & 0x8000 ? (r << 1) ^ 0x8005 : r << 1);
      t[i] = r;
    }
  uint16_t c = 0;
  for (size_t i = 0; i < n; ++i) c = (uint16_t)((c << 8) ^ t[(c >> 8) ^ b[i]]);
  return c;
}
static uint32_t ogg_crc(const uint8_t *b, size_t n) {
  static uint32_t t[256];
  if (!t[1])
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t r = i << 24;
      for (int k = 0; k < 8; ++k) r = r & 0x80000000u ? (r << 1) ^ 0x04c11db7u : r << 1;
      t[i] = r;
    }
  uint32_t c = 0;
  for (size_t i = 0; i < n; ++i) c = (c << 8) ^ t[(c >> 24) ^ b[i]];
  return c;
}

/* ------------------------------------------------------------------ bits, most significant first */
typedef struct {
  uint8_t *p;
  size_t n, cap;
  uint64_t acc;
  int bits; /* in acc, below 8 after every put */
  int oom;
} Bits;
static void put_byte(Bits *b, uint8_t v) {
  if (b->n == b->cap) {
    const size_t nc = b->cap ? b->cap * 2 : 1024;
    uint8_t *q = (uint8_t *)realloc(b->p, nc);
    if (!q) {
      b->oom = 1;
      return;
    }
    b->p = q;
    b->cap = nc;
  }
  b->p[b->n++] = v;
}
static void put(Bits *b, uint32_t v, int n) {
  if (n <= 0) return;
  b->acc = (b->acc << n) | (v & (n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1)));
  b->bits += n;
  while (b->bits >= 8) {
    b->bits -= 8;
    put_byte(b, (uint8_t)(b->acc >> b->bits));
  }
  b->acc &= (1ull << b->bits) - 1;
}
static void put_signed(Bits *b, int32_t v, int n) { put(b, (uint32_t)v, n); }
static void unary(Bits *b, uint32_t q) {
  while (q >= 24) {
    put(b, 0, 24);
    q -= 24;
  }
  put(b, 1, (int)q + 1);
}
static void pad(Bits *b) {
  if (b->bits) put(b, 0, 8 - b->bits);
}

/* ------------------------------------------------------------------ 19.4.3 the frame */
static int32_t asr(int32_t v, int s) { return v >= 0 ? v >> s : -(int32_t)((-(int64_t)v - 1) >> s) - 1; } /* floor */

static int block_code(int n, int *hint_bits) {
  static const int sizes[] = {192, 576, 1152, 2304, 4608, 256, 512, 1024, 2048, 4096};
  static const int codes[] = {1, 2, 3, 4, 5, 8, 9, 10, 11, 12};
  *hint_bits = 0;
  for (int i = 0; i < 10; ++i)
    if (n == sizes[i]) return codes[i];
  *hint_bits = n <= 256 ? 8 : 16;
  return n <= 256 ? 6 : 7;
}
static int rate_code(int rate, uint32_t *hint, int *hint_bits) {
  static const int rates[] = {88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
  static const int codes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  *hint_bits = 0;
  for (int i = 0; i < 11; ++i)
    if (rate == rates[i]) return codes[i];
  if (rate <= 255000 && rate % 1000 == 0) {
    *hint = (uint32_t)(rate / 1000);
    *hint_bits = 8;
    return 12;
  }
  if (rate <= 655350 && rate % 10 == 0) {
    *hint = (uint32_t)(rate / 10);
    *hint_bits = 16;
    return 14;
  }
  *hint = (uint32_t)rate;
  *hint_bits = 16;
  return 13;
}
/* RFC 9639 9.1.5: the frame number coded like UTF-8 */
static void utf8_number(Bits *b, uint32_t v) {
  if (v < 0x80) {
    put(b, v, 8);
    return;
  }
  static const uint32_t limit[] = {0x800, 0x10000, 0x200000, 0x4000000, 0x80000000u};
  static const uint32_t lead[] = {0xC0, 0xE0, 0xF0, 0xF8, 0xFC};
  int nb = 2;
  while (nb < 6 && v >= limit[nb - 2]) ++nb;
  put(b, lead[nb - 2] | (v >> (6 * (nb - 1))), 8);
  for (int i = nb - 2; i >= 0; --i) put(b, 0x80 | ((v >> (6 * i)) & 0x3F), 8);
}

static int wasted_bits(const int16_t *x, int n) {
  int32_t acc = 0;
  for (int i = 0; i < n; ++i) {
    acc |= x[i];
    if (acc & 1) break;
  }
  if (acc == 0) return 0;
  int w = 0;
  uint32_t u = (uint32_t)acc;
  while (!(u & 1)) {
    u >>= 1;
    w++;
  }
  return w < BPS ? w : BPS;
}

/* the fixed predictor's residual of order `order` for x[order .. n-1] */
static int32_t residual(const int32_t *x, int i, int order) {
  switch (order) {
  case 0: return x[i];
  case 1: return x[i] - x[i - 1];
  case 2: return x[i] - 2 * x[i - 1] + x[i - 2];
  case 3: return x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3];
  default: return x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4];
  }
}

/* libFLAC set_partitioned_rice_: the estimate of one partition order, and each partition's parameter */
static uint64_t rice_estimate(const uint64_t *sums, int parts, int n, int order, int po, int *params) {
  uint64_t bits = 2 + 4;
  const int base = n >> po;
  const uint64_t div_base = 0x40000u / (uint64_t)base;
  for (int p = 0; p < parts; ++p) {
    const int cnt = p == 0 ? base - order : base;
    const uint64_t div = p == 0 ? 0x40000u / (uint64_t)cnt : div_base;
    const uint64_t s = sums[p];
    const uint64_t q = s >= 2 ? ((s - 1) * div) >> 18 : 0;
    int k = 0;
    for (uint64_t t = q; t; t >>= 1) ++k;
    if (k >= 15) k = 14;
    uint64_t pb = 4 + (uint64_t)(1 + k) * (uint64_t)cnt + (k ? (s >> (k - 1)) : (s << 1)) - (uint64_t)(cnt >> 1);
    if (pb > U32MAX) pb = U32MAX;
    bits = pb < U32MAX - bits ? bits + pb : U32MAX;
    params[p] = k;
  }
  return bits;
}

int tf_flac_frame(const int16_t *xs, int n, uint32_t number, int rate, uint8_t **out, size_t *out_n, int *kind_out) {
  *out = NULL;
  *out_n = 0;
  if (n < 1 || n > TF_FLAC_BLOCK) return -1;
  const int w = wasted_bits(xs, n), sbps = BPS - w;
  int32_t x[TF_FLAC_BLOCK], *res = NULL;
  for (int i = 0; i < n; ++i) x[i] = w ? asr(xs[i], w) : xs[i];
  int tz = 0;
  while (!((n >> tz) & 1)) ++tz;
  const int limit = tz < MAX_PARTITION_ORDER ? tz : MAX_PARTITION_ORDER;
  /* 0 verbatim, 1 constant, 2 fixed */
  int kind = 0, order = 0, po = 0, params[1 << MAX_PARTITION_ORDER];
  uint64_t best = 8 + (uint64_t)w + (uint64_t)n * (uint64_t)sbps;
  if (n > 4) {
    int64_t t[5] = {0, 0, 0, 0, 0};
    for (int i = 4; i < n; ++i)
      for (int o = 0; o < 5; ++o) {
        const int64_t e = residual(x, i, o);
        t[o] += e < 0 ? -e : e;
      }
    const int64_t m1234 = t[1] < t[2] ? (t[1] < t[3] ? (t[1] < t[4] ? t[1] : t[4]) : (t[3] < t[4] ? t[3] : t[4]))
                                      : (t[2] < t[3] ? (t[2] < t[4] ? t[2] : t[4]) : (t[3] < t[4] ? t[3] : t[4]));
    const int64_t m234 = t[2] < t[3] ? (t[2] < t[4] ? t[2] : t[4]) : (t[3] < t[4] ? t[3] : t[4]);
    const int64_t m34 = t[3] < t[4] ? t[3] : t[4];
    const int guess = t[0] <= m1234 ? 0 : t[1] <= m234 ? 1 : t[2] <= m34 ? 2 : t[3] <= t[4] ? 3 : 4;
    int same = t[1] == 0;
    for (int i = 1; same && i < n; ++i) same = x[i] == x[0];
    if (same) {
      const uint64_t bits = 8 + (uint64_t)w + (uint64_t)sbps;
      if (bits < best) best = bits, kind = 1;
    } else {
      res = (int32_t *)malloc(sizeof(int32_t) * (size_t)n);
      if (!res) return -1;
      for (int i = guess; i < n; ++i) res[i - guess] = residual(x, i, guess);
      /* libFLAC find_best_partition_order_: from the highest order allowed down to 0, a tie keeps the higher */
      int max_po = limit;
      while (max_po > 0 && (n >> max_po) <= guess) --max_po;
      uint64_t sums[MAX_PARTITION_ORDER + 1][1 << MAX_PARTITION_ORDER];
      const int dps = n >> max_po;
      for (int p = 0, at = 0; p < (1 << max_po); ++p) {
        const int cnt = p == 0 ? dps - guess : dps;
        uint64_t sum = 0;
        for (int i = 0; i < cnt; ++i) sum += (uint64_t)(res[at + i] < 0 ? -(int64_t)res[at + i] : res[at + i]);
        sums[max_po][p] = sum;
        at += cnt;
      }
      for (int o = max_po - 1; o >= 0; --o)
        for (int i = 0; i < (1 << o); ++i) sums[o][i] = sums[o + 1][2 * i] + sums[o + 1][2 * i + 1];
      uint64_t rbits = 0;
      int bpo = -1, bparams[1 << MAX_PARTITION_ORDER], tmp[1 << MAX_PARTITION_ORDER];
      for (int o = max_po; o >= 0; --o) {
        const uint64_t bits = rice_estimate(sums[o], 1 << o, n, guess, o, tmp);
        if (bpo < 0 || bits < rbits) {
          rbits = bits;
          bpo = o;
          memcpy(bparams, tmp, sizeof(int) * (size_t)(1 << o));
        }
      }
      uint64_t est = 8 + (uint64_t)w + (uint64_t)guess * (uint64_t)sbps;
      est = rbits < U32MAX - est ? est + rbits : U32MAX;
      if (est < best) {
        best = est;
        kind = 2;
        order = guess;
        po = bpo;
        memcpy(params, bparams, sizeof(int) * (size_t)(1 << bpo));
      }
    }
  }
  Bits b;
  memset(&b, 0, sizeof b);
  /* the header */
  int hint_bits = 0, rhint_bits = 0;
  uint32_t rhint = 0;
  const int bc = block_code(n, &hint_bits);
  const int rc = rate_code(rate, &rhint, &rhint_bits);
  put(&b, 0x3FFE, 14);
  put(&b, 0, 1);
  put(&b, 0, 1); /* fixed block size: the header carries the frame number */
  put(&b, (uint32_t)bc, 4);
  put(&b, (uint32_t)rc, 4);
  put(&b, 0, 4); /* one channel */
  put(&b, 4, 3); /* 16 bits a sample */
  put(&b, 0, 1);
  utf8_number(&b, number);
  if (hint_bits) put(&b, (uint32_t)(n - 1), hint_bits);
  if (rhint_bits) put(&b, rhint, rhint_bits);
  put(&b, b.oom ? 0 : crc8(b.p, b.n), 8);
  /* the subframe */
  const uint32_t flag = w ? 1 : 0;
  if (kind == 1) {
    put(&b, 0x00 | flag, 8);
    if (w) unary(&b, (uint32_t)(w - 1));
    put_signed(&b, x[0], sbps);
  } else if (kind == 0) {
    put(&b, 0x02 | flag, 8);
    if (w) unary(&b, (uint32_t)(w - 1));
    for (int i = 0; i < n; ++i) put_signed(&b, x[i], sbps);
  } else {
    put(&b, 0x10 | ((uint32_t)order << 1) | flag, 8);
    if (w) unary(&b, (uint32_t)(w - 1));
    for (int i = 0; i < order; ++i) put_signed(&b, x[i], sbps);
    put(&b, 0, 2); /* Rice, 4-bit parameters (16-bit samples never need more than 14) */
    put(&b, (uint32_t)po, 4);
    const int base = n >> po;
    for (int p = 0, at = 0; p < (1 << po); ++p) {
      const int cnt = p == 0 ? base - order : base;
      const int k = params[p];
      put(&b, (uint32_t)k, 4);
      for (int i = 0; i < cnt; ++i) {
        const int32_t r = res[at + i];
        const uint32_t u = r >= 0 ? (uint32_t)r << 1 : ((uint32_t)(-(int64_t)r) << 1) - 1;
        unary(&b, u >> k);
        put(&b, u & ((1u << k) - 1), k);
      }
      at += cnt;
    }
  }
  free(res);
  pad(&b);
  const uint16_t c16 = b.oom ? 0 : crc16(b.p, b.n);
  put(&b, c16 >> 8, 8);
  put(&b, c16 & 0xFF, 8);
  if (b.oom) {
    free(b.p);
    return -1;
  }
  *out = b.p;
  *out_n = b.n;
  if (kind_out) *kind_out = kind == 2 ? 2 + order : kind == 1 ? 0 : 1;
  return 0;
}

/* ------------------------------------------------------------------ 19.4.1, 19.4.2, 19.4.4 the stream */
long long tf_flac_frames_per_segment(int rate, int k) {
  static const int secs[4] = {10, 20, 40, 60};
  const int s = secs[(k < 4 ? k : 4) - 1];
  return ((long long)s * rate + TF_FLAC_BLOCK - 1) / TF_FLAC_BLOCK;
}

static int seg_open(TfFlacMux *m) {
  char name[32];
  snprintf(name, sizeof name, "seg-%05d%s", m->segment, m->ext);
  char *p = tf_path_join(m->dir, name);
  m->file = p ? fopen(p, "wb") : NULL;
  free(p);
  return m->file ? 0 : -1;
}
/* one page of one whole packet into the segment being written; the segment closes after its last page */
static int page(TfFlacMux *m, const uint8_t *packet, size_t n, long long granule, int flags, int frame) {
  if (n / 255 + 1 > 255) return -1; /* a frame larger than one page: never with 16-bit blocks of 4096 */
  if (!m->file && seg_open(m)) return -1;
  uint8_t head[27 + 255];
  const int lace = (int)(n / 255) + 1;
  memcpy(head, "OggS", 4);
  head[4] = 0;
  head[5] = (uint8_t)flags;
  for (int i = 0; i < 8; ++i) head[6 + i] = (uint8_t)((unsigned long long)granule >> (8 * i));
  for (int i = 0; i < 4; ++i) head[14 + i] = (uint8_t)(m->serial >> (8 * i)), head[18 + i] = (uint8_t)(m->seq >> (8 * i));
  memset(head + 22, 0, 4);
  head[26] = (uint8_t)lace;
  for (int i = 0; i < lace - 1; ++i) head[27 + i] = 255;
  head[27 + lace - 1] = (uint8_t)(n % 255);
  const size_t hn = 27 + (size_t)lace;
  uint8_t *pg = (uint8_t *)malloc(hn + n);
  if (!pg) return -1;
  memcpy(pg, head, hn);
  memcpy(pg + hn, packet, n);
  const uint32_t c = ogg_crc(pg, hn + n);
  for (int i = 0; i < 4; ++i) pg[22 + i] = (uint8_t)(c >> (8 * i));
  const int ok = fwrite(pg, 1, hn + n, m->file) == hn + n && fflush(m->file) == 0;
  free(pg);
  if (!ok) return -1;
  m->seq++;
  if (!frame) return 0;
  m->seg_frames++;
  if (m->seg_frames == tf_flac_frames_per_segment(m->rate, m->segment) || (flags & 0x04)) {
    const int closed = fclose(m->file);
    m->file = NULL;
    if (closed) return -1;
    if (m->on_segment) m->on_segment(m->user, m->segment, granule);
    m->segment++;
    m->seg_frames = 0;
  }
  return 0;
}

int tf_flac_open(TfFlacMux *m, const char *dir, const char *ext, uint32_t serial, int rate, const char *vendor) {
  TfFlacSegmentFn cb = m->on_segment;
  void *user = m->user;
  memset(m, 0, sizeof *m);
  m->on_segment = cb;
  m->user = user;
  if (rate < 8000 || rate > 192000 || rate == 48000 || strlen(dir) >= sizeof m->dir || strlen(ext) >= sizeof m->ext) return -1;
  snprintf(m->dir, sizeof m->dir, "%s", dir);
  snprintf(m->ext, sizeof m->ext, "%s", ext);
  m->serial = serial;
  m->rate = rate;
  m->segment = 1;
  /* page 0: 0x7F FLAC, mapping 1.0, one header packet follows, fLaC, STREAMINFO (not the last block) */
  uint8_t h[51];
  memcpy(h, "\x7f" "FLAC\x01\x00\x00\x01" "fLaC\x00\x00\x00\x22", 17);
  uint8_t *si = h + 17;
  si[0] = TF_FLAC_BLOCK >> 8, si[1] = TF_FLAC_BLOCK & 0xFF, si[2] = TF_FLAC_BLOCK >> 8, si[3] = TF_FLAC_BLOCK & 0xFF;
  memset(si + 4, 0, 6); /* the frame sizes: unknown */
  const uint64_t packed = ((uint64_t)rate << 44) | ((uint64_t)(BPS - 1) << 36);
  for (int i = 0; i < 8; ++i) si[10 + i] = (uint8_t)(packed >> (56 - 8 * i));
  memset(si + 18, 0, 16); /* MD5: unknown */
  /* page 1: the last metadata block, VORBIS_COMMENT: the vendor and no comment */
  const size_t vl = strlen(vendor), cl = 4 + vl + 4;
  uint8_t *cm = (uint8_t *)malloc(4 + cl);
  if (!cm) return -1;
  cm[0] = 0x84;
  cm[1] = (uint8_t)(cl >> 16), cm[2] = (uint8_t)(cl >> 8), cm[3] = (uint8_t)cl;
  for (int i = 0; i < 4; ++i) cm[4 + i] = (uint8_t)(vl >> (8 * i));
  memcpy(cm + 8, vendor, vl);
  memset(cm + 8 + vl, 0, 4);
  const int rc = page(m, h, sizeof h, 0, 0x02, 0) || page(m, cm, 4 + cl, 0, 0x00, 0);
  free(cm);
  return rc ? -1 : 0;
}

/* a whole frame encoded: the page held before it goes out (not the last), and this one is held */
static int frame_out(TfFlacMux *m, int n) {
  uint8_t *f = NULL;
  size_t fn = 0;
  int kind = 0;
  if (tf_flac_frame(m->block, n, m->frame_no, m->rate, &f, &fn, &kind)) return -1;
  m->frame_no++;
  m->granule += n;
  m->kinds[kind]++;
  int rc = 0;
  if (m->held) rc = page(m, m->held, m->held_n, m->held_granule, 0x00, 1);
  free(m->held);
  m->held = f;
  m->held_n = fn;
  m->held_granule = m->granule;
  m->nb = 0;
  return rc;
}

int tf_flac_push(TfFlacMux *m, const int16_t *x, long long n) {
  while (n > 0) {
    const int k = (int)(n < TF_FLAC_BLOCK - m->nb ? n : TF_FLAC_BLOCK - m->nb);
    memcpy(m->block + m->nb, x, sizeof(int16_t) * (size_t)k);
    m->nb += k;
    x += k;
    n -= k;
    if (m->nb == TF_FLAC_BLOCK && frame_out(m, TF_FLAC_BLOCK)) return -1;
  }
  return 0;
}

int tf_flac_finish(TfFlacMux *m) {
  if (m->nb > 0 && frame_out(m, m->nb)) return -1;
  if (!m->held) return -1; /* no sample: no audio page */
  const int rc = page(m, m->held, m->held_n, m->held_granule, 0x04, 1);
  free(m->held);
  m->held = NULL;
  return rc;
}

void tf_flac_abandon(TfFlacMux *m) {
  if (m->file) fclose(m->file);
  m->file = NULL;
  free(m->held);
  m->held = NULL;
}
