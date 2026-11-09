/* Bit-exactness and single-thread realtime for the native int8 kernel.
 *
 * The kernel's result must equal the engine's int8 linear to the bit. This computes an independent
 * integer golden (sum q*w - zp*rowsum, the engine's exact arithmetic) and asserts the kernel matches
 * over the model's shapes, then times a MAC-weighted spread of the real layer shapes for one 480 ms
 * chunk's int8 work (369.3M multiply-adds) single-thread, and prints the realtime factor. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ns(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart * 1e9 / (double)f.QuadPart; }
#else
static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
#endif

#include "zf_gemm_vnni.h"

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static int rnd(int lo, int hi) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return lo + (int)(rng % (uint64_t)(hi - lo + 1)); }

static int check_shape(int m, int n, int k, int a_zp) {
  const int kp = zf_vnni_kpad(k);
  int8_t *w = malloc((size_t)n * k), *packed = malloc(zf_vnni_packed_bytes(n, k));
  uint8_t *q = calloc((size_t)m * kp, 1);
  int32_t *rs = malloc((size_t)zf_vnni_row_sums_count(n) * 4), *out = malloc((size_t)m * n * 4), *gold = malloc((size_t)m * n * 4);
  for (int i = 0; i < n * k; ++i) w[i] = (int8_t)rnd(-127, 127);
  for (int i = 0; i < m; ++i) for (int c = 0; c < k; ++c) q[(size_t)i * kp + c] = (uint8_t)rnd(0, 255);
  zf_vnni_pack(w, n, k, packed, rs);
  for (int i = 0; i < m; ++i) for (int j = 0; j < n; ++j) {
    int64_t s = 0, r = 0;
    for (int c = 0; c < k; ++c) { s += (int32_t)q[(size_t)i * kp + c] * (int32_t)w[(size_t)j * k + c]; r += w[(size_t)j * k + c]; }
    gold[(size_t)i * n + j] = (int32_t)(s - (int64_t)a_zp * r);
  }
  memset(out, 0xCD, (size_t)m * n * 4);
  zf_vnni_linear_u8(q, m, kp, packed, rs, n, k, a_zp, out, n);
  int bad = 0;
  for (int i = 0; i < m * n && !bad; ++i) if (out[i] != gold[i]) { printf("MISMATCH m=%d n=%d k=%d @%d %d!=%d\n", m, n, k, i, out[i], gold[i]); bad = 1; }
  free(w); free(packed); free(q); free(rs); free(out); free(gold);
  return bad;
}

typedef struct { uint8_t *q; int8_t *packed; int32_t *rs, *out; int m, n, k, kp; } Mat;
static void make(Mat *t, int m, int n, int k) {
  t->m = m; t->n = n; t->k = k; t->kp = zf_vnni_kpad(k);
  int8_t *w = malloc((size_t)n * k);
  t->q = calloc((size_t)m * t->kp, 1); t->packed = malloc(zf_vnni_packed_bytes(n, k));
  t->rs = malloc((size_t)zf_vnni_row_sums_count(n) * 4); t->out = malloc((size_t)m * n * 4);
  for (int i = 0; i < n * k; ++i) w[i] = (int8_t)rnd(-127, 127);
  for (int i = 0; i < m; ++i) for (int c = 0; c < k; ++c) t->q[(size_t)i * t->kp + c] = (uint8_t)rnd(0, 255);
  zf_vnni_pack(w, n, k, t->packed, t->rs); free(w);
}
static double bench(int m, int n, int k, int iters) {
  Mat t; make(&t, m, n, k);
  const double a = now_ns(); volatile int s = 0;
  for (int r = 0; r < iters; ++r) { zf_vnni_linear_u8(t.q, m, t.kp, t.packed, t.rs, n, k, 100, t.out, n); s += t.out[0]; }
  double ns = now_ns() - a; (void)s;
  free(t.q); free(t.packed); free(t.rs); free(t.out);
  return (double)m * n * k * iters / ns;
}

int main(void) {
  const int ms[] = {1,2,3,4,6,7,12,24,32,100}, ns[] = {16,32,251,512}, ks[] = {4,31,80,192,512};
  const int zps[] = {0,1,64,128,255};
  int fails = 0;
  const int best = zf_int8_cpu_tier();
  printf("cpu int8 tier: %s\n", zf_int8_tier_name(best));
  if (best < ZF_INT8_AVX512VNNI) printf("SKIP: avx512vnni (this CPU or OS does not have it; not tested here)\n");
  for (int tier = best; tier >= ZF_INT8_SCALAR; --tier) {
    if (!zf_int8_tier_available(tier)) {
      printf("SKIP: %s (this CPU or OS does not have it; not tested here)\n", zf_int8_tier_name(tier));
      continue;
    }
    zf_int8_set_tier(tier);
    int f = 0, cases = 0;
    for (size_t i=0;i<sizeof ms/sizeof*ms;++i) for (size_t j=0;j<sizeof ns/sizeof*ns;++j) for (size_t l=0;l<sizeof ks/sizeof*ks;++l) {
      f += check_shape(ms[i], ns[j], ks[l], zps[(i+j+l)%(sizeof zps/sizeof*zps)]); ++cases;
    }
    printf("%s: %-10s == integer golden, %d shapes, %d failures\n", f?"FAIL":"PASS", zf_int8_tier_name(tier), cases, f);
    fails += f;
  }
  zf_int8_set_tier(best);

  /* single-thread chunk: MAC-weighted spread of the model's real int8 linear shapes */
  const int shapes[][4] = {
    {48,192,192,8},{48,512,192,4},{48,192,512,4},{24,256,256,8},{24,512,256,4},{24,256,512,4},
    {12,384,384,18},{12,1024,384,6},{12,384,1024,6},{6,512,512,32},{6,2048,512,8},{6,512,2048,8},
    {12,384,384,18},{12,1024,384,6},{12,384,1024,6},{24,256,256,8},{24,512,256,4},{24,256,512,4},
  };
  const int nsh = (int)(sizeof shapes/sizeof shapes[0]);
  Mat *mats = malloc((size_t)nsh * sizeof(Mat)); double macs = 0;
  for (int s=0;s<nsh;++s) { make(&mats[s], shapes[s][0], shapes[s][1], shapes[s][2]); macs += (double)shapes[s][0]*shapes[s][1]*shapes[s][2]*shapes[s][3]; }
  const int iters = 100; const double a = now_ns(); volatile int sink=0;
  for (int it=0;it<iters;++it) for (int s=0;s<nsh;++s) for (int c=0;c<shapes[s][3];++c) { zf_vnni_linear_u8(mats[s].q, mats[s].m, mats[s].kp, mats[s].packed, mats[s].rs, mats[s].n, mats[s].k, 100, mats[s].out, mats[s].n); sink+=mats[s].out[0]; }
  (void)sink;
  double ms_chunk = (now_ns() - a)/iters/1e6;
  for (int s=0;s<nsh;++s){ free(mats[s].q); free(mats[s].packed); free(mats[s].rs); free(mats[s].out);} free(mats);

  printf("\nsingle-thread throughput (%s):\n", zf_int8_tier_name(zf_int8_tier()));
  printf("  24x512x512 : %.1f GMAC/s\n", bench(24,512,512,4000));
  printf("  6x512x512  : %.1f GMAC/s\n", bench(6,512,512,4000));
  printf("  12x384x384 : %.1f GMAC/s\n", bench(12,384,384,4000));
  printf("\nchunk int8 (%.0fM MACs) single-thread: %.2f ms  ->  RTF %.4f, %.0fx realtime (480 ms chunk)\n",
         macs/1e6, ms_chunk, ms_chunk/480.0, 480.0/ms_chunk);
  return fails ? 1 : 0;
}
