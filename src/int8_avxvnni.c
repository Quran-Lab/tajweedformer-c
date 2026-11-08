/* The AVX-VNNI tier (compiled with -mavx2 -mavxvnni; called only when the CPU and OS have them). */
#include <immintrin.h>

#include "int8_common.h"

/* AVX-VNNI: the same instruction at 256 bits, a tile as two halves of eight units. */
void zf_int8_k_avxvnni(const uint8_t *q, int m, int ldq, const int8_t *packed, const int32_t *rs, int kp,
                      int zp, int32_t *out, int ldc, int xu, int c0, int c1, int jsh) {
  const int g = kp / ZF_VNNI_K4;
  const __m256i vzp = _mm256_set1_epi32(zp);
  const int32_t xm = xu * 0x01010101;
  for (int t = c0 / ZF_VNNI_NT; t * ZF_VNNI_NT < c1; ++t) {
    const int8_t *panel = packed + (size_t)t * g * 64;
    const int base = t * ZF_VNNI_NT, lo = imax(base, c0), hi = imin(base + ZF_VNNI_NT, c1);
    const int full = lo == base && hi == base + ZF_VNNI_NT;
    const __m256i corrL = _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(rs + base)), vzp);
    const __m256i corrH = _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(rs + base + 8)), vzp);
    for (int i = 0; i < m; i += 4) {
      const int rows = imin(4, m - i);
      const uint8_t *q0 = q + (size_t)i * ldq;
      const uint8_t *q1 = rows > 1 ? q0 + ldq : q0, *q2 = rows > 2 ? q0 + 2 * (size_t)ldq : q0,
                    *q3 = rows > 3 ? q0 + 3 * (size_t)ldq : q0;
      __m256i l0 = _mm256_setzero_si256(), l1 = l0, l2 = l0, l3 = l0, h0 = l0, h1 = l0, h2 = l0, h3 = l0;
      for (int gi = 0; gi < g; ++gi) {
        const __m256i wl = _mm256_loadu_si256((const __m256i *)(panel + (size_t)gi * 64));
        const __m256i wh = _mm256_loadu_si256((const __m256i *)(panel + (size_t)gi * 64 + 32));
        const int o = gi * ZF_VNNI_K4;
        const __m256i v0 = _mm256_set1_epi32(load32(q0 + o) ^ xm), v1 = _mm256_set1_epi32(load32(q1 + o) ^ xm);
        const __m256i v2 = _mm256_set1_epi32(load32(q2 + o) ^ xm), v3 = _mm256_set1_epi32(load32(q3 + o) ^ xm);
        l0 = _mm256_dpbusd_avx_epi32(l0, v0, wl); h0 = _mm256_dpbusd_avx_epi32(h0, v0, wh);
        l1 = _mm256_dpbusd_avx_epi32(l1, v1, wl); h1 = _mm256_dpbusd_avx_epi32(h1, v1, wh);
        l2 = _mm256_dpbusd_avx_epi32(l2, v2, wl); h2 = _mm256_dpbusd_avx_epi32(h2, v2, wh);
        l3 = _mm256_dpbusd_avx_epi32(l3, v3, wl); h3 = _mm256_dpbusd_avx_epi32(h3, v3, wh);
      }
      const __m256i L[4] = {_mm256_sub_epi32(l0, corrL), _mm256_sub_epi32(l1, corrL),
                            _mm256_sub_epi32(l2, corrL), _mm256_sub_epi32(l3, corrL)};
      const __m256i H[4] = {_mm256_sub_epi32(h0, corrH), _mm256_sub_epi32(h1, corrH),
                            _mm256_sub_epi32(h2, corrH), _mm256_sub_epi32(h3, corrH)};
      if (full) {
        for (int x = 0; x < rows; ++x) {
          int32_t *o = out + (size_t)(i + x) * ldc + base - jsh;
          _mm256_storeu_si256((__m256i *)o, L[x]);
          _mm256_storeu_si256((__m256i *)(o + 8), H[x]);
        }
      } else {
        int32_t tmp[4][ZF_VNNI_NT];
        for (int x = 0; x < rows; ++x) {
          _mm256_storeu_si256((__m256i *)tmp[x], L[x]);
          _mm256_storeu_si256((__m256i *)(tmp[x] + 8), H[x]);
        }
        put_rows(out + (size_t)i * ldc, ldc, rows, base, lo, hi, jsh, tmp);
      }
    }
  }
}

