/* CUDA int8 GEMM tier, bit-exact to the engine's int8 export.
 *
 * dp4a (DP4A) does a signed int8x4 dot into int32 exactly, so the GPU path is integer-exact: it
 * reproduces the engine's shifted-int8 sum, sum_k a_k*w_k + (128 - zp)*rowsum with a = q - 128,
 * bit for bit. This is the ONE part of a GPU pipeline that can stay exact; the float layers
 * (BiasNorm, swoosh, softmax) cannot (GPU transcendentals and FMA differ), so a full-GPU forward
 * pass is a fast, approximate tier, while this kernel alone is exact and can be checked against the
 * CPU result on the host.
 *
 * Built only when ZF_CUDA is enabled (needs the CUDA toolkit); the CPU VNNI path is the default and
 * is what the tests here verify. This header is safe to include without CUDA. */
#ifndef ZF_GEMM_CUDA_H
#define ZF_GEMM_CUDA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if a usable CUDA device is present. 0 (and the caller uses the CPU path) otherwise. */
int zf_cuda_available(void);

/* out[i][j] = sum_k a[i][k]*w[j][k] + (128 - a_zero_point)*sum_k w[j][k], the engine's exact int8
 * result, computed on the GPU with dp4a. `a` is [m, lda] shifted int8 (q - 128, pad zero), `w_nk`
 * is [n, k] row-major int8; the kernel packs and uploads weights on first use per (w_nk, n, k).
 * Host pointers; the wrapper manages device memory. Returns 0 on success, non-zero on a CUDA error
 * (the caller should fall back to the CPU kernel). */
int zf_cuda_linear_s8(const int8_t *a, int m, int lda, const int8_t *w_nk, int n, int k,
                      int a_zero_point, int32_t *out, int ldc);

/* Release any cached device weights/buffers. */
void zf_cuda_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* ZF_GEMM_CUDA_H */
