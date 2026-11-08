/* gzip (RFC 1952) decoding of layout-3 model files, streamed from a file to a file: a compact DEFLATE (RFC 1951)
 * decoder in the manner of Joergen Ibsen's tinf (zlib licence), with a 32 KB window, so no zlib is needed on
 * Windows. The decoded bytes are then checked against the manifest's decoded size and SHA-256 by the caller,
 * and the gzip trailer's CRC-32 and length are checked here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

typedef struct {
  unsigned short counts[16], symbols[288];
} Tree;

typedef struct {
  FILE *in, *out;
  unsigned bitbuf;
  int bitcount;
  int eof;
  unsigned char win[32768];
  unsigned wpos;
  unsigned long long total;
  unsigned crc;
  int err;
} Inf;

static unsigned crc_tab[256];
static void crc_init(void) {
  if (crc_tab[1]) return;
  for (unsigned i = 0; i < 256; ++i) {
    unsigned c = i;
    for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc_tab[i] = c;
  }
}

static int getbyte(Inf *s) {
  const int c = fgetc(s->in);
  if (c == EOF) {
    s->eof = 1;
    return 0;
  }
  return c;
}
static unsigned getbits(Inf *s, int n) {
  while (s->bitcount < n) {
    s->bitbuf |= (unsigned)getbyte(s) << s->bitcount;
    s->bitcount += 8;
  }
  const unsigned v = s->bitbuf & ((1u << n) - 1u);
  s->bitbuf >>= n;
  s->bitcount -= n;
  return v;
}
static void put(Inf *s, unsigned char b) {
  s->win[s->wpos++ & 32767] = b;
  s->crc = crc_tab[(s->crc ^ b) & 0xff] ^ (s->crc >> 8);
  s->total++;
  if ((s->wpos & 32767) == 0 && fwrite(s->win, 1, 32768, s->out) != 32768) s->err = 1;
}
static void flush_out(Inf *s) {
  const unsigned n = s->wpos & 32767;
  if (n && fwrite(s->win, 1, n, s->out) != n) s->err = 1;
}

static int build(Tree *t, const unsigned char *lengths, int num) {
  unsigned short offs[16];
  memset(t->counts, 0, sizeof t->counts);
  for (int i = 0; i < num; ++i) t->counts[lengths[i]]++;
  t->counts[0] = 0;
  unsigned avail = 1;
  for (int i = 1; i < 16; ++i) {
    avail = 2 * avail - t->counts[i];
    if ((int)avail < 0 && t->counts[i]) return -1;
  }
  unsigned sum = 0;
  for (int i = 0; i < 16; ++i) {
    offs[i] = (unsigned short)sum;
    sum += t->counts[i];
  }
  for (int i = 0; i < num; ++i)
    if (lengths[i]) t->symbols[offs[lengths[i]]++] = (unsigned short)i;
  return 0;
}
static int decode_sym(Inf *s, const Tree *t) {
  int base = 0, offs = 0;
  for (int len = 1; len < 16; ++len) {
    offs = 2 * offs + (int)getbits(s, 1);
    if (offs < t->counts[len]) return t->symbols[base + offs];
    base += t->counts[len];
    offs -= t->counts[len];
  }
  s->err = 1;
  return 0;
}

static const unsigned short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const unsigned char kLenBits[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const unsigned char kDistBits[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int block(Inf *s, const Tree *lt, const Tree *dt) {
  for (;;) {
    int sym = decode_sym(s, lt);
    if (s->err || s->eof) return -1;
    if (sym < 256) {
      put(s, (unsigned char)sym);
      continue;
    }
    if (sym == 256) return 0;
    sym -= 257;
    if (sym >= 29) return -1;
    const unsigned len = kLenBase[sym] + getbits(s, kLenBits[sym]);
    const int ds = decode_sym(s, dt);
    if (ds >= 30) return -1;
    const unsigned dist = kDistBase[ds] + getbits(s, kDistBits[ds]);
    if (dist > s->total || dist > 32768) return -1;
    for (unsigned i = 0; i < len; ++i) put(s, s->win[(s->wpos - dist) & 32767]);
  }
}

static int dynamic(Inf *s, Tree *lt, Tree *dt) {
  static const unsigned char order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  unsigned char lengths[288 + 32];
  const unsigned hlit = getbits(s, 5) + 257, hdist = getbits(s, 5) + 1, hclen = getbits(s, 4) + 4;
  memset(lengths, 0, 19);
  for (unsigned i = 0; i < hclen; ++i) lengths[order[i]] = (unsigned char)getbits(s, 3);
  Tree ct;
  if (build(&ct, lengths, 19)) return -1;
  for (unsigned n = 0; n < hlit + hdist;) {
    const int sym = decode_sym(s, &ct);
    if (s->err) return -1;
    unsigned rep = 0, val = 0;
    if (sym < 16) {
      lengths[n++] = (unsigned char)sym;
      continue;
    }
    if (sym == 16) {
      if (!n) return -1;
      val = lengths[n - 1];
      rep = getbits(s, 2) + 3;
    } else if (sym == 17) rep = getbits(s, 3) + 3;
    else rep = getbits(s, 7) + 11;
    if (n + rep > hlit + hdist) return -1;
    while (rep--) lengths[n++] = (unsigned char)val;
  }
  if (build(lt, lengths, (int)hlit) || build(dt, lengths + hlit, (int)hdist)) return -1;
  return 0;
}

/* in: a .gz file; out: the decoded file. 0, or -1 for a malformed stream (CRC-32 and length checked). */
int tf_gunzip_file(const char *in_path, const char *out_path) {
  crc_init();
  Inf *s = (Inf *)calloc(1, sizeof *s);
  if (!s) return -1;
  s->crc = 0xffffffffu;
  s->in = fopen(in_path, "rb");
  s->out = s->in ? fopen(out_path, "wb") : NULL;
  int rc = -1;
  if (!s->in || !s->out) goto done;
  unsigned char h[10];
  if (fread(h, 1, 10, s->in) != 10 || h[0] != 0x1f || h[1] != 0x8b || h[2] != 8) goto done;
  const int flg = h[3];
  if (flg & 4) {
    const int xlen = getbyte(s) | getbyte(s) << 8;
    for (int i = 0; i < xlen; ++i) getbyte(s);
  }
  if (flg & 8)
    while (getbyte(s) && !s->eof) {}
  if (flg & 16)
    while (getbyte(s) && !s->eof) {}
  if (flg & 2) getbyte(s), getbyte(s);
  Tree lt, dt;
  for (int last = 0; !last;) {
    last = (int)getbits(s, 1);
    const unsigned type = getbits(s, 2);
    if (type == 0) {
      s->bitbuf = 0;
      s->bitcount = 0;
      const unsigned len = (unsigned)(getbyte(s) | getbyte(s) << 8), nlen = (unsigned)(getbyte(s) | getbyte(s) << 8);
      if ((len ^ 0xffffu) != nlen) goto done;
      for (unsigned i = 0; i < len && !s->eof; ++i) put(s, (unsigned char)getbyte(s));
    } else if (type == 1) {
      unsigned char l[288 + 32];
      memset(l, 8, 144);
      memset(l + 144, 9, 112);
      memset(l + 256, 7, 24);
      memset(l + 280, 8, 8);
      build(&lt, l, 288);
      memset(l, 5, 30);
      build(&dt, l, 30);
      if (block(s, &lt, &dt)) goto done;
    } else if (type == 2) {
      if (dynamic(s, &lt, &dt) || block(s, &lt, &dt)) goto done;
    } else {
      goto done;
    }
    if (s->err || s->eof) goto done;
  }
  flush_out(s);
  {
    unsigned char t[8];
    s->bitbuf = 0;
    s->bitcount = 0;
    if (fread(t, 1, 8, s->in) != 8) goto done;
    const unsigned crc = (unsigned)t[0] | (unsigned)t[1] << 8 | (unsigned)t[2] << 16 | (unsigned)t[3] << 24;
    const unsigned isize = (unsigned)t[4] | (unsigned)t[5] << 8 | (unsigned)t[6] << 16 | (unsigned)t[7] << 24;
    if (crc != (s->crc ^ 0xffffffffu) || isize != (unsigned)(s->total & 0xffffffffu)) goto done;
  }
  rc = s->err ? -1 : 0;
done:
  if (s->in) fclose(s->in);
  if (s->out && fclose(s->out)) rc = -1;
  if (rc) remove(out_path);
  free(s);
  return rc;
}
