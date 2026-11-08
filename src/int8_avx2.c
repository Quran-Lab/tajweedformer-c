/* The AVX2 tier (compiled with -mavx2; called only when the CPU and OS have it). */
#include <immintrin.h>

#include "int8_common.h"

/* AVX2: VPMADDUBSW(|a|, sign(w, a)) then VPMADDWD by ones, on the SIGNED activation a = q - 128.
 * |a| <= 128 and |w| <= 127, so a pair sums to at most 32512: no int16 saturation. The result is
 * sum a*w + (128 - zp) * sum w, the same integer as the VNNI tiers' sum q*w - zp * sum w. */
#define AVX2_STEP(acc, absa, sa, w) \
  acc = _mm256_add_epi32(acc, _mm256_madd_epi16(_mm256_maddubs_epi16(absa, _mm256_sign_epi8(w, sa)), ones))
void zf_int8_k_avx2(const uint8_t *q, int m, int ldq, const int8_t *packed, const int32_t *rs, int kp,
                   int zp, int32_t *out, int ldc, int xu, int c0, int c1, int jsh) {
  const int g = kp / ZF_VNNI_K4;
  const __m256i vcorr = _mm256_set1_epi32(128 - zp);
  const __m256i ones = _mm256_set1_epi16(1);
  const int32_t xm = (xu ^ 0x80) * 0x01010101; /* to the signed activation */
  for (int t = c0 / ZF_VNNI_NT; t * ZF_VNNI_NT < c1; ++t) {
    const int8_t *panel = packed + (size_t)t * g * 64;
    const int base = t * ZF_VNNI_NT, lo = imax(base, c0), hi = imin(base + ZF_VNNI_NT, c1);
    const int full = lo == base && hi == base + ZF_VNNI_NT;
    const __m256i corrL = _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(rs + base)), vcorr);
    const __m256i corrH = _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(rs + base + 8)), vcorr);
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
        const __m256i s0 = _mm256_set1_epi32(load32(q0 + o) ^ xm), s1 = _mm256_set1_epi32(load32(q1 + o) ^ xm);
        const __m256i s2 = _mm256_set1_epi32(load32(q2 + o) ^ xm), s3 = _mm256_set1_epi32(load32(q3 + o) ^ xm);
        const __m256i b0 = _mm256_abs_epi8(s0), b1 = _mm256_abs_epi8(s1);
        const __m256i b2 = _mm256_abs_epi8(s2), b3 = _mm256_abs_epi8(s3);
        AVX2_STEP(l0, b0, s0, wl); AVX2_STEP(h0, b0, s0, wh);
        AVX2_STEP(l1, b1, s1, wl); AVX2_STEP(h1, b1, s1, wh);
        AVX2_STEP(l2, b2, s2, wl); AVX2_STEP(h2, b2, s2, wh);
        AVX2_STEP(l3, b3, s3, wl); AVX2_STEP(h3, b3, s3, wh);
      }
      const __m256i L[4] = {_mm256_add_epi32(l0, corrL), _mm256_add_epi32(l1, corrL),
                            _mm256_add_epi32(l2, corrL), _mm256_add_epi32(l3, corrL)};
      const __m256i H[4] = {_mm256_add_epi32(h0, corrH), _mm256_add_epi32(h1, corrH),
                            _mm256_add_epi32(h2, corrH), _mm256_add_epi32(h3, corrH)};
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
