/* Proof that every native int8 tier is a bit-exact drop-in for the engine's own zf_linear_s8.
 *
 * If zf_vnni_linear_s8 produces the SAME int32 as the engine's zf_linear_s8 for every shape, on the
 * engine's own inputs (shifted int8 activations, the engine's a_zero_point), then routing the decode
 * through it changes not one bit of any layer's output. This links the engine library from the
 * submodule WITHOUT the hook (its weak default declines, so zf_linear_s8 is the engine's kernel) and
 * compares, for each tier this CPU has, over the full width and over pooled sub-ranges of panels
 * (zf_linear_s8_tile with a column shift, what the engine's pool asks the hook for).
 *
 * lda is round_up(k,16) (the engine's k_pad) for both: it is a multiple of 4, and the columns past
 * the real k are zero in the activation and zero in both packings. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zf/kernels.h"     /* the engine's int8 kernel */
#include "zf_gemm_vnni.h"   /* the native kernels */

static uint64_t rng = 0x243f6a8885a308d3ull;
static int rnd(int lo, int hi) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return lo + (int)(rng % (uint64_t)(hi - lo + 1)); }

static int check(int m, int n, int k, int a_zp) {
  const int lda = (k + 15) & ~15;
  int8_t *w_nk = malloc((size_t)n * k);
  int8_t *a = calloc((size_t)m * lda, 1);
  for (int j = 0; j < n; ++j) for (int c = 0; c < k; ++c) w_nk[(size_t)j * k + c] = (int8_t)rnd(-127, 127);
  for (int i = 0; i < m; ++i) for (int c = 0; c < k; ++c) a[(size_t)i * lda + c] = (int8_t)(rnd(0, 255) - 128);

  int8_t *ep = malloc(zf_weights_s8_packed_bytes(n, k));
  int32_t *ers = malloc((size_t)zf_weights_s8_row_sums_count(n) * 4);
  ZfWeightsS8 ew; zf_weights_s8_pack(w_nk, n, k, ep, ers, &ew);
  int32_t *out_eng = malloc((size_t)m * n * 4);
  zf_linear_s8(a, m, lda, &ew, a_zp, out_eng, n);

  int8_t *vp = malloc(zf_vnni_packed_bytes(n, k));
  int32_t *vrs = malloc((size_t)zf_vnni_row_sums_count(n) * 4);
  zf_vnni_pack(w_nk, n, k, vp, vrs);
  int32_t *out_n = malloc((size_t)m * n * 4);
  memset(out_n, 0xCD, (size_t)m * n * 4);
  zf_vnni_linear_s8(a, m, lda, vp, vrs, n, k, a_zp, out_n, n);

  int bad = 0;
  for (int i = 0; i < m * n && !bad; ++i)
    if (out_eng[i] != out_n[i]) {
      printf("DROP-IN MISMATCH m=%d n=%d k=%d zp=%d @%d: engine %d != native %d\n", m, n, k, a_zp, i, out_eng[i], out_n[i]);
      bad = 1;
    }
  /* a sub-range of panels into a compact tile, as the pool asks */
  if (!bad && ew.n_blocks > 1) {
    const int bb = rnd(0, ew.n_blocks - 1), be = rnd(bb + 1, ew.n_blocks);
    const int c0 = bb * ZF_S8_UNIT_BLOCK, c1 = be * ZF_S8_UNIT_BLOCK < n ? be * ZF_S8_UNIT_BLOCK : n;
    const int cols = be * ZF_S8_UNIT_BLOCK - c0;
    int32_t *te = malloc((size_t)m * cols * 4), *tn = malloc((size_t)m * cols * 4);
    memset(te, 0, (size_t)m * cols * 4); memset(tn, 0, (size_t)m * cols * 4);
    zf_linear_s8_tile(a, m, lda, &ew, a_zp, te, cols, bb, be, c0);
    zf_vnni_linear_s8_cols(a, m, lda, vp, vrs, n, k, a_zp, tn, cols, c0, c1, c0);
    for (int i = 0; i < m && !bad; ++i)
      for (int j = 0; j < c1 - c0 && !bad; ++j)
        if (te[(size_t)i * cols + j] != tn[(size_t)i * cols + j]) {
          printf("TILE MISMATCH m=%d n=%d k=%d blocks [%d,%d) @%d,%d\n", m, n, k, bb, be, i, j);
          bad = 1;
        }
    free(te); free(tn);
  }
  free(w_nk); free(a); free(ep); free(ers); free(out_eng); free(vp); free(vrs); free(out_n);
  return bad;
}

int main(void) {
  const int ms[] = {1,2,3,4,6,7,12,24,32,48,100,300};
  const int ns[] = {4,7,16,38,192,251,256,384,512};
  const int ks[] = {9,16,31,32,80,100,192,384,512};
  const int zps[] = {0,1,64,128,200,255};
  int fails = 0;
  const int best = zf_int8_cpu_tier();
  for (int tier = best; tier >= ZF_INT8_SCALAR; --tier) {
    zf_int8_set_tier(tier);
    int f = 0, cases = 0;
    for (size_t i=0;i<sizeof ms/sizeof*ms;++i) for (size_t j=0;j<sizeof ns/sizeof*ns;++j) for (size_t l=0;l<sizeof ks/sizeof*ks;++l) {
      f += check(ms[i], ns[j], ks[l], zps[(i+j+l)%(sizeof zps/sizeof*zps)]); ++cases;
    }
    printf("%s: %-10s == engine zf_linear_s8 over %d shapes (full width + a pooled sub-range each), %d failures\n",
           f?"FAIL":"PASS", zf_int8_tier_name(tier), cases, f);
    fails += f;
  }
  printf("AVX-512 VNNI on this CPU: %s\n", best == ZF_INT8_AVX512VNNI ? "yes, tested above" : "no, NOT tested here");
  return fails ? 1 : 0;
}
