/* SHA-256, SHA-512, Ed25519 signature verification, hex and base64.
 *
 * Ed25519 verification follows TweetNaCl (Bernstein, van Gastel, Janssen, Lange, Schwabe, Smetsers;
 * public domain) field and group arithmetic; the curve constants (d, 2d, sqrt(-1), the base point) are
 * computed at first use from their definitions instead of being typed in, and the scalar s is checked
 * to be below the group order (RFC 8032 5.1.7). Only verification is needed: no secret is handled,
 * so constant time does not matter here. tests/tf_crypto_test.c checks the RFC 8032 vectors. */
#include <stdio.h>
#include <string.h>

#include "tf_internal.h"
#include "tf_shaconst.h"

/* ------------------------------------------------------------------ SHA-256 */
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = hh + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    const uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
void tf_sha256_init(TfSha256 *c) {
  memcpy(c->h, H256, sizeof c->h);
  c->len = 0;
  c->fill = 0;
}
void tf_sha256_update(TfSha256 *c, const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  c->len += n;
  while (n) {
    if (c->fill == 0 && n >= 64) {
      sha256_block(c->h, p);
      p += 64;
      n -= 64;
      continue;
    }
    size_t k = 64 - c->fill;
    if (k > n) k = n;
    memcpy(c->buf + c->fill, p, k);
    c->fill += k;
    p += k;
    n -= k;
    if (c->fill == 64) {
      sha256_block(c->h, c->buf);
      c->fill = 0;
    }
  }
}
void tf_sha256_final(TfSha256 *c, uint8_t out[32]) {
  const uint64_t bits = c->len * 8;
  uint8_t pad = 0x80;
  tf_sha256_update(c, &pad, 1);
  pad = 0;
  while (c->fill != 56) tf_sha256_update(c, &pad, 1);
  uint8_t l[8];
  for (int i = 0; i < 8; ++i) l[i] = (uint8_t)(bits >> (56 - 8 * i));
  tf_sha256_update(c, l, 8);
  for (int i = 0; i < 8; ++i) {
    out[4 * i] = (uint8_t)(c->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)c->h[i];
  }
}
void tf_sha256(const void *data, size_t n, uint8_t out[32]) {
  TfSha256 c;
  tf_sha256_init(&c);
  tf_sha256_update(&c, data, n);
  tf_sha256_final(&c, out);
}
int tf_sha256_file(const char *path, uint8_t out[32], long long *size) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  TfSha256 c;
  tf_sha256_init(&c);
  unsigned char buf[1 << 16];
  size_t k;
  long long total = 0;
  while ((k = fread(buf, 1, sizeof buf, f)) > 0) {
    tf_sha256_update(&c, buf, k);
    total += (long long)k;
  }
  const int err = ferror(f);
  fclose(f);
  if (err) return -1;
  tf_sha256_final(&c, out);
  if (size) *size = total;
  return 0;
}

/* ------------------------------------------------------------------ SHA-512 */
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
static void sha512_block(uint64_t h[8], const uint8_t *p) {
  uint64_t w[80];
  for (int i = 0; i < 16; ++i) {
    uint64_t v = 0;
    for (int k = 0; k < 8; ++k) v = v << 8 | p[8 * i + k];
    w[i] = v;
  }
  for (int i = 16; i < 80; ++i) {
    const uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
    const uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 80; ++i) {
    const uint64_t t1 = hh + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
    const uint64_t t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
typedef struct {
  uint64_t h[8];
  uint64_t len;
  uint8_t buf[128];
  size_t fill;
} Sha512;
static void sha512_init(Sha512 *c) {
  memcpy(c->h, H512, sizeof c->h);
  c->len = 0;
  c->fill = 0;
}
static void sha512_update(Sha512 *c, const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  c->len += n;
  while (n) {
    size_t k = 128 - c->fill;
    if (k > n) k = n;
    memcpy(c->buf + c->fill, p, k);
    c->fill += k;
    p += k;
    n -= k;
    if (c->fill == 128) {
      sha512_block(c->h, c->buf);
      c->fill = 0;
    }
  }
}
static void sha512_final(Sha512 *c, uint8_t out[64]) {
  const uint64_t bits = c->len * 8;
  uint8_t pad = 0x80;
  sha512_update(c, &pad, 1);
  pad = 0;
  while (c->fill != 112) sha512_update(c, &pad, 1);
  uint8_t l[16] = {0};
  for (int i = 0; i < 8; ++i) l[8 + i] = (uint8_t)(bits >> (56 - 8 * i));
  sha512_update(c, l, 16);
  for (int i = 0; i < 8; ++i)
    for (int k = 0; k < 8; ++k) out[8 * i + k] = (uint8_t)(c->h[i] >> (56 - 8 * k));
}
void tf_sha512(const void *data, size_t n, uint8_t out[64]) {
  Sha512 c;
  sha512_init(&c);
  sha512_update(&c, data, n);
  sha512_final(&c, out);
}

/* ------------------------------------------------------------------ Ed25519 (TweetNaCl arithmetic) */
typedef int64_t gf[16];
static gf gf0, gf1, D, D2, X, Y, I;
static int consts_ready;

static void set25519(gf r, const gf a) { for (int i = 0; i < 16; ++i) r[i] = a[i]; }
static void car25519(gf o) {
  for (int i = 0; i < 16; ++i) {
    o[i] += (int64_t)1 << 16;
    const int64_t c = o[i] >> 16;
    o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
    o[i] -= c * 65536;
  }
}
static void sel25519(gf p, gf q, int b) {
  const int64_t c = ~((int64_t)b - 1);
  for (int i = 0; i < 16; ++i) {
    const int64_t t = c & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}
static void pack25519(uint8_t *o, const gf n) {
  gf m, t;
  set25519(t, n);
  car25519(t);
  car25519(t);
  car25519(t);
  for (int j = 0; j < 2; ++j) {
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; ++i) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    const int b = (int)((m[15] >> 16) & 1);
    m[14] &= 0xffff;
    sel25519(t, m, 1 - b);
  }
  for (int i = 0; i < 16; ++i) {
    o[2 * i] = (uint8_t)(t[i] & 0xff);
    o[2 * i + 1] = (uint8_t)(t[i] >> 8);
  }
}
static int neq25519(const gf a, const gf b) {
  uint8_t c[32], d[32];
  pack25519(c, a);
  pack25519(d, b);
  return memcmp(c, d, 32) != 0;
}
static uint8_t par25519(const gf a) {
  uint8_t d[32];
  pack25519(d, a);
  return d[0] & 1;
}
static void unpack25519(gf o, const uint8_t *n) {
  for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
  o[15] &= 0x7fff;
}
static void fA(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
static void fZ(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }
static void fM(gf o, const gf a, const gf b) {
  int64_t t[31];
  for (int i = 0; i < 31; ++i) t[i] = 0;
  for (int i = 0; i < 16; ++i)
    for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
  for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
  for (int i = 0; i < 16; ++i) o[i] = t[i];
  car25519(o);
  car25519(o);
}
static void fS(gf o, const gf a) { fM(o, a, a); }
static void inv25519(gf o, const gf i) {
  gf c;
  set25519(c, i);
  for (int a = 253; a >= 0; --a) {
    fS(c, c);
    if (a != 2 && a != 4) fM(c, c, i);
  }
  set25519(o, c);
}
static void pow2523(gf o, const gf i) {
  gf c;
  set25519(c, i);
  for (int a = 250; a >= 0; --a) {
    fS(c, c);
    if (a != 1) fM(c, c, i);
  }
  set25519(o, c);
}
/* o = b^e, e little endian 32 bytes */
static void powgf(gf o, const gf b, const uint8_t e[32]) {
  gf r;
  set25519(r, gf1);
  for (int bit = 255; bit >= 0; --bit) {
    fS(r, r);
    if ((e[bit / 8] >> (bit & 7)) & 1) fM(r, r, b);
  }
  set25519(o, r);
}
static void gf_small(gf o, int64_t v) {
  for (int i = 0; i < 16; ++i) o[i] = 0;
  o[0] = v & 0xffff;
  o[1] = v >> 16;
}

static void add(gf p[4], gf q[4]) {
  gf a, b, c, d, t, e, f, g, h;
  fZ(a, p[1], p[0]);
  fZ(t, q[1], q[0]);
  fM(a, a, t);
  fA(b, p[0], p[1]);
  fA(t, q[0], q[1]);
  fM(b, b, t);
  fM(c, p[3], q[3]);
  fM(c, c, D2);
  fM(d, p[2], q[2]);
  fA(d, d, d);
  fZ(e, b, a);
  fZ(f, d, c);
  fA(g, d, c);
  fA(h, b, a);
  fM(p[0], e, f);
  fM(p[1], h, g);
  fM(p[2], g, f);
  fM(p[3], e, h);
}
static void cswap(gf p[4], gf q[4], uint8_t b) {
  for (int i = 0; i < 4; ++i) sel25519(p[i], q[i], b);
}
static void pack(uint8_t *r, gf p[4]) {
  gf tx, ty, zi;
  inv25519(zi, p[2]);
  fM(tx, p[0], zi);
  fM(ty, p[1], zi);
  pack25519(r, ty);
  r[31] ^= (uint8_t)(par25519(tx) << 7);
}
static void scalarmult(gf p[4], gf q[4], const uint8_t *s) {
  set25519(p[0], gf0);
  set25519(p[1], gf1);
  set25519(p[2], gf1);
  set25519(p[3], gf0);
  for (int i = 255; i >= 0; --i) {
    const uint8_t b = (s[i / 8] >> (i & 7)) & 1;
    cswap(p, q, b);
    add(q, p);
    add(p, p);
    cswap(p, q, b);
  }
}
static void scalarbase(gf p[4], const uint8_t *s) {
  gf q[4];
  set25519(q[0], X);
  set25519(q[1], Y);
  set25519(q[2], gf1);
  fM(q[3], X, Y);
  scalarmult(p, q, s);
}
static int unpackneg(gf r[4], const uint8_t p[32]) {
  gf t, chk, num, den, den2, den4, den6;
  set25519(r[2], gf1);
  unpack25519(r[1], p);
  fS(num, r[1]);
  fM(den, num, D);
  fZ(num, num, r[2]);
  fA(den, r[2], den);
  fS(den2, den);
  fS(den4, den2);
  fM(den6, den4, den2);
  fM(t, den6, num);
  fM(t, t, den);
  pow2523(t, t);
  fM(t, t, num);
  fM(t, t, den);
  fM(t, t, den);
  fM(r[0], t, den);
  fS(chk, r[0]);
  fM(chk, chk, den);
  if (neq25519(chk, num)) fM(r[0], r[0], I);
  fS(chk, r[0]);
  fM(chk, chk, den);
  if (neq25519(chk, num)) return -1;
  if (par25519(r[0]) == (p[31] >> 7)) fZ(r[0], gf0, r[0]);
  fM(r[3], r[0], r[1]);
  return 0;
}

static void init_consts(void) {
  if (consts_ready) return;
  gf a, b;
  gf_small(gf0, 0);
  gf_small(gf1, 1);
  /* d = -121665 / 121666 */
  gf_small(a, 121665);
  gf_small(b, 121666);
  inv25519(b, b);
  fM(D, a, b);
  fZ(D, gf0, D);
  fA(D2, D, D);
  /* sqrt(-1) = 2^((p - 1) / 4), (p - 1) / 4 = 2^253 - 5 */
  uint8_t e[32];
  memset(e, 0xff, sizeof e);
  e[0] = 0xfb;
  e[31] = 0x1f;
  gf_small(a, 2);
  powgf(I, a, e);
  /* the base point: y = 4/5, x the even root */
  gf_small(a, 4);
  gf_small(b, 5);
  inv25519(b, b);
  fM(Y, a, b);
  uint8_t yb[32];
  pack25519(yb, Y); /* sign bit 0 */
  gf pt[4];
  unpackneg(pt, yb); /* gives -x for the even x */
  fZ(X, gf0, pt[0]);
  consts_ready = 1;
}

static const int64_t Lord[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                 0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
                                 0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};
static void modL(uint8_t *r, int64_t x[64]) {
  int64_t carry;
  int i, j;
  for (i = 63; i >= 32; --i) {
    carry = 0;
    for (j = i - 32; j < i - 12; ++j) {
      x[j] += carry - 16 * x[i] * Lord[j - (i - 32)];
      carry = (x[j] + 128) >> 8;
      x[j] -= carry * 256;
    }
    x[j] += carry;
    x[i] = 0;
  }
  carry = 0;
  for (j = 0; j < 32; ++j) {
    x[j] += carry - (x[31] >> 4) * Lord[j];
    carry = x[j] >> 8;
    x[j] &= 255;
  }
  for (j = 0; j < 32; ++j) x[j] -= carry * Lord[j];
  for (i = 0; i < 32; ++i) {
    x[i + 1] += x[i] >> 8;
    r[i] = (uint8_t)(x[i] & 255);
  }
}
static void reduce(uint8_t *r) {
  int64_t x[64];
  for (int i = 0; i < 64; ++i) x[i] = (uint64_t)r[i];
  for (int i = 0; i < 64; ++i) r[i] = 0;
  modL(r, x);
}
/* s < L, both little endian */
static int scalar_below_order(const uint8_t s[32]) {
  for (int i = 31; i >= 0; --i) {
    if (s[i] < Lord[i]) return 1;
    if (s[i] > Lord[i]) return 0;
  }
  return 0;
}

int tf_ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t pk[32]) {
  init_consts();
  if (!scalar_below_order(sig + 32)) return 0;
  gf p[4], q[4];
  if (unpackneg(q, pk)) return 0;
  /* h = SHA-512(R || A || M) mod L */
  Sha512 c;
  sha512_init(&c);
  sha512_update(&c, sig, 32);
  sha512_update(&c, pk, 32);
  sha512_update(&c, msg, n);
  uint8_t h[64];
  sha512_final(&c, h);
  reduce(h);
  scalarmult(p, q, h);
  scalarbase(q, sig + 32);
  add(p, q);
  uint8_t t[32];
  pack(t, p);
  return memcmp(sig, t, 32) == 0;
}

/* ------------------------------------------------------------------ hex, base64 */
static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
int tf_hex_decode(const char *hex, uint8_t *out, size_t n) {
  if (!hex || strlen(hex) != 2 * n) return -1;
  for (size_t i = 0; i < n; ++i) {
    const int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
    if (a < 0 || b < 0) return -1;
    out[i] = (uint8_t)(a << 4 | b);
  }
  return 0;
}
void tf_hex_encode(const uint8_t *in, size_t n, char *out) {
  static const char d[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = d[in[i] >> 4];
    out[2 * i + 1] = d[in[i] & 15];
  }
  out[2 * n] = 0;
}
static int b64val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}
int tf_base64_decode(const char *s, uint8_t *out, size_t cap) {
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (const char *p = s; *p; ++p) {
    if (*p == '=') {
      for (const char *q = p; *q; ++q)
        if (*q != '=') return -1;
      break;
    }
    const int v = b64val((unsigned char)*p);
    if (v < 0) return -1;
    acc = acc << 6 | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return -1;
      out[o++] = (uint8_t)(acc >> bits);
    }
  }
  return (int)o;
}
