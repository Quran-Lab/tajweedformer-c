/* The AVX-512 VNNI tier (compiled with -mavx512f -mavx512bw -mavx512vnni; called only when the CPU
 * and OS have them). */
#include <immintrin.h>

#include "int8_common.h"

/* AVX-512 VNNI: one 16-unit tile per register, four rows at a time. */
void zf_int8_k_avx512(const uint8_t *q, int m, int ldq, const int8_t *packed, const int32_t *rs, int kp,
                     int zp, int32_t *out, int ldc, int xu, int c0, int c1, int jsh) {
  const int g = kp / ZF_VNNI_K4;
  const __m512i vzp = _mm512_set1_epi32(zp);
  const int32_t xm = xu * 0x01010101;
  for (int t = c0 / ZF_VNNI_NT; t * ZF_VNNI_NT < c1; ++t) {
    const int8_t *panel = packed + (size_t)t * g * 64;
    const int base = t * ZF_VNNI_NT, lo = imax(base, c0), hi = imin(base + ZF_VNNI_NT, c1);
    const int full = lo == base && hi == base + ZF_VNNI_NT;
    const __m512i corr = _mm512_mullo_epi32(_mm512_loadu_si512((const void *)(rs + base)), vzp);
    for (int i = 0; i < m; i += 4) {
      const int rows = imin(4, m - i);
      const uint8_t *q0 = q + (size_t)i * ldq;
      const uint8_t *q1 = rows > 1 ? q0 + ldq : q0, *q2 = rows > 2 ? q0 + 2 * (size_t)ldq : q0,
                    *q3 = rows > 3 ? q0 + 3 * (size_t)ldq : q0;
      __m512i a0 = _mm512_setzero_si512(), a1 = a0, a2 = a0, a3 = a0;
      for (int gi = 0; gi < g; ++gi) {
        const __m512i wv = _mm512_loadu_si512((const void *)(panel + (size_t)gi * 64));
        const int o = gi * ZF_VNNI_K4;
        a0 = _mm512_dpbusd_epi32(a0, _mm512_set1_epi32(load32(q0 + o) ^ xm), wv);
        a1 = _mm512_dpbusd_epi32(a1, _mm512_set1_epi32(load32(q1 + o) ^ xm), wv);
        a2 = _mm512_dpbusd_epi32(a2, _mm512_set1_epi32(load32(q2 + o) ^ xm), wv);
        a3 = _mm512_dpbusd_epi32(a3, _mm512_set1_epi32(load32(q3 + o) ^ xm), wv);
      }
      const __m512i r[4] = {_mm512_sub_epi32(a0, corr), _mm512_sub_epi32(a1, corr),
                            _mm512_sub_epi32(a2, corr), _mm512_sub_epi32(a3, corr)};
      if (full) {
        for (int x = 0; x < rows; ++x) _mm512_storeu_si512((void *)(out + (size_t)(i + x) * ldc + base - jsh), r[x]);
      } else {
        int32_t tmp[4][ZF_VNNI_NT];
        for (int x = 0; x < rows; ++x) _mm512_storeu_si512((void *)tmp[x], r[x]);
        put_rows(out + (size_t)i * ldc, ldc, rows, base, lo, hi, jsh, tmp);
      }
    }
  }
}

