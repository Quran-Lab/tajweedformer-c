/* Validates the wired path: the engine's zf_linear_s8 and zf_linear_s8_tile, routed through the
 * native hook, must equal an independent integer golden, on every tier this CPU has. Also checks
 * that the hook really took the calls (a weak symbol that silently loses to the engine's default
 * would pass the arithmetic and leave the decode on the engine's kernel). Linked: the engine library
 * + the hook object (strong zf_s8_linear_hook) + the native kernels. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zf/kernels.h"
#include "zf_gemm_vnni.h"

static uint64_t rng = 0xcafef00dd15ea5e5ull;
static int rnd(int lo, int hi) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return lo + (int)(rng % (uint64_t)(hi - lo + 1)); }

static int check(int m, int n, int k, int a_zp) {
  const int lda = (k + 15) & ~15;
  int8_t *w_nk = malloc((size_t)n * k), *a = calloc((size_t)m * lda, 1);
  for (int j = 0; j < n; ++j) for (int c = 0; c < k; ++c) w_nk[(size_t)j * k + c] = (int8_t)rnd(-127, 127);
  for (int i = 0; i < m; ++i) for (int c = 0; c < k; ++c) a[(size_t)i * lda + c] = (int8_t)(rnd(0, 255) - 128);
  int8_t *ep = malloc(zf_weights_s8_packed_bytes(n, k));
  int32_t *ers = malloc((size_t)zf_weights_s8_row_sums_count(n) * 4);
  ZfWeightsS8 w; zf_weights_s8_pack(w_nk, n, k, ep, ers, &w);
  int32_t *out = malloc((size_t)m * n * 4), *gold = malloc((size_t)m * n * 4);
  const int32_t corr = 128 - a_zp;
  for (int i = 0; i < m; ++i) for (int j = 0; j < n; ++j) {
    int64_t s = 0, r = 0;
    for (int c = 0; c < k; ++c) { s += (int32_t)a[(size_t)i * lda + c] * (int32_t)w_nk[(size_t)j * k + c]; r += w_nk[(size_t)j * k + c]; }
    gold[(size_t)i * n + j] = (int32_t)(s + (int64_t)corr * r);
  }
  memset(out, 0xCD, (size_t)m * n * 4);
  zf_linear_s8(a, m, lda, &w, a_zp, out, n);   /* engine entry -> hook -> native kernel */
  int bad = 0;
  for (int i = 0; i < m * n && !bad; ++i) if (out[i] != gold[i]) { printf("HOOKED MISMATCH m=%d n=%d k=%d @%d %d!=%d\n", m, n, k, i, out[i], gold[i]); bad = 1; }
  if (!bad && w.n_blocks > 1) {           /* a pooled sub-range into a shifted tile */
    const int bb = rnd(0, w.n_blocks - 1), be = rnd(bb + 1, w.n_blocks);
    const int c0 = bb * ZF_S8_UNIT_BLOCK, c1 = be * ZF_S8_UNIT_BLOCK < n ? be * ZF_S8_UNIT_BLOCK : n;
    const int cols = be * ZF_S8_UNIT_BLOCK - c0;
    int32_t *t = malloc((size_t)m * cols * 4);
    memset(t, 0xCD, (size_t)m * cols * 4);
    zf_linear_s8_tile(a, m, lda, &w, a_zp, t, cols, bb, be, c0);
    for (int i = 0; i < m && !bad; ++i)
      for (int j = c0; j < c1 && !bad; ++j)
        if (t[(size_t)i * cols + j - c0] != gold[(size_t)i * n + j]) { printf("HOOKED TILE MISMATCH m=%d n=%d k=%d [%d,%d)\n", m, n, k, bb, be); bad = 1; }
    free(t);
  }
  free(w_nk); free(a); free(ep); free(ers); free(out); free(gold);
  return bad;
}

int main(void) {
#ifdef _WIN32
  _putenv("ZF_NATIVE_STATS=1");
#else
  setenv("ZF_NATIVE_STATS", "1", 1);
#endif
  const int best = zf_int8_cpu_tier();
  const int ms[] = {1,3,4,6,12,24,32,48,100,300}, ns[] = {4,16,38,192,251,512}, ks[] = {9,16,80,192,512}, zps[] = {0,1,128,255};
  int fails = 0;
  for (int tier = best; tier >= ZF_INT8_SCALAR; --tier) {
    if (!zf_int8_tier_available(tier)) {
      printf("SKIP: %s (this CPU or OS does not have it; not tested here)\n", zf_int8_tier_name(tier));
      continue;
    }
    zf_int8_set_tier(tier);
    const long long before = zf_int8_hook_calls(tier);
    int f = 0, cases = 0;
    for (size_t i=0;i<sizeof ms/sizeof*ms;++i) for (size_t j=0;j<sizeof ns/sizeof*ns;++j) for (size_t l=0;l<sizeof ks/sizeof*ks;++l) {
      f += check(ms[i], ns[j], ks[l], zps[(i+j+l)%(sizeof zps/sizeof*zps)]); ++cases;
    }
    const long long took = zf_int8_hook_calls(tier) - before;
    if (took <= 0) { printf("FAIL: the hook took no calls on %s (the engine's weak default won the link)\n", zf_int8_tier_name(tier)); ++f; }
    printf("%s: engine zf_linear_s8 via the hook on %-10s == golden over %d shapes, %lld hook calls, %d failures\n",
           f?"FAIL":"PASS", zf_int8_tier_name(tier), cases, took, f);
    fails += f;
  }
  return fails ? 1 : 0;
}
