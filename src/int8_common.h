/* Shared by the int8 kernels' translation units (zf_gemm_vnni.c and one file per x86 tier, each
 * compiled with its own instruction-set flags so only it may use those instructions). */
#ifndef ZF_INT8_COMMON_H
#define ZF_INT8_COMMON_H
#include <stdint.h>
#include <string.h>

#include "zf_gemm_vnni.h"

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int32_t load32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }

/* A tile's results for up to four rows, written to columns [lo, hi) only. */
static inline void put_rows(int32_t *out, int ldc, int rows, int base, int lo, int hi, int jsh,
                     const int32_t tmp[4][ZF_VNNI_NT]) {
  for (int r = 0; r < rows; ++r)
    for (int c = lo; c < hi; ++c) out[(size_t)r * ldc + c - jsh] = tmp[r][c - base];
}


/* One kernel: rows [0, m) of q (byte transform `xu` makes each byte the uint8 q) against output
 * columns [c0, c1), written to out[i * ldc + j - jsh]. */
#define ZF_INT8_KERNEL(name)                                                                           void name(const uint8_t *q, int m, int ldq, const int8_t *packed, const int32_t *rs, int kp, int zp,             int32_t *out, int ldc, int xu, int c0, int c1, int jsh)
ZF_INT8_KERNEL(zf_int8_k_avx512);
ZF_INT8_KERNEL(zf_int8_k_avxvnni);
ZF_INT8_KERNEL(zf_int8_k_avx2);
#endif
