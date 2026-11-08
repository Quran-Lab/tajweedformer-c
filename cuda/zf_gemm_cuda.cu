/* CUDA int8 GEMM via dp4a, bit-exact to the engine's int8 linear (integer arithmetic).
 *
 * UNTESTED on this development box (no GPU present); it compiles with nvcc and is written to be
 * correct by construction: dp4a is an exact signed int8x4 dot, and the accumulation is integer, so
 * the int32 result matches the CPU kernel bit for bit. Verify against zf_vnni_linear_u8 on a machine
 * with a GPU before relying on it. Requires compute capability 6.1+ (dp4a).
 *
 * Weight layout on device: [n][k_pad] int8, k padded to a multiple of 4, so each thread reads a
 * row's k as int32 quads for dp4a. One thread computes one output (i, j). This is the simple,
 * obviously-correct tiling; a production kernel would tile with shared memory, but the arithmetic
 * and its exactness would not change. */
#include <cuda_runtime.h>
#include <stdint.h>
#include <string.h>

#include "zf_gemm_cuda.h"

#define K4 4
static int round_up(int v, int m) { return (v + m - 1) / m * m; }

extern "C" int zf_cuda_available(void) {
  int n = 0;
  return (cudaGetDeviceCount(&n) == cudaSuccess && n > 0) ? 1 : 0;
}

/* device state: cached weights for the last (ptr, n, k) so repeated calls on the same layer do not
 * re-upload. A real engine would pack every weight once at load; this mirrors that with a 1-entry
 * cache to keep the interface a drop-in for the CPU kernel. */
static const int8_t *g_w_key = nullptr;
static int g_n = 0, g_k = 0, g_kpad = 0;
static int8_t *g_dw = nullptr;   /* device weights [n][kpad] */
static int32_t *g_drs = nullptr; /* device row sums [n] */
static int8_t *g_da = nullptr;   /* device activations */
static int32_t *g_dout = nullptr;
static size_t g_a_cap = 0, g_out_cap = 0;

extern "C" void zf_cuda_reset(void) {
  cudaFree(g_dw); cudaFree(g_drs); cudaFree(g_da); cudaFree(g_dout);
  g_dw = nullptr; g_drs = nullptr; g_da = nullptr; g_dout = nullptr;
  g_w_key = nullptr; g_n = g_k = g_kpad = 0; g_a_cap = g_out_cap = 0;
}

__global__ void gemm_dp4a(const int8_t *a, int m, int lda, const int8_t *w, const int32_t *rs,
                          int n, int kpad, int corr, int32_t *out, int ldc) {
  const int i = blockIdx.y * blockDim.y + threadIdx.y; /* output row */
  const int j = blockIdx.x * blockDim.x + threadIdx.x; /* output unit */
  if (i >= m || j >= n) return;
  const int32_t *aq = (const int32_t *)(a + (size_t)i * lda);
  const int32_t *wq = (const int32_t *)(w + (size_t)j * kpad);
  int acc = 0;
  for (int q = 0; q < kpad / K4; ++q) acc = __dp4a(aq[q], wq[q], acc);
  out[(size_t)i * ldc + j] = acc + corr * rs[j];
}

static int upload_weights(const int8_t *w_nk, int n, int k) {
  const int kpad = round_up(k, K4);
  if (g_w_key == w_nk && g_n == n && g_k == k) return 0; /* cached */
  cudaFree(g_dw); cudaFree(g_drs);
  if (cudaMalloc(&g_dw, (size_t)n * kpad) != cudaSuccess) return 1;
  if (cudaMalloc(&g_drs, (size_t)n * sizeof(int32_t)) != cudaSuccess) return 1;
  int8_t *hw = (int8_t *)calloc((size_t)n * kpad, 1);
  int32_t *hrs = (int32_t *)malloc((size_t)n * sizeof(int32_t));
  for (int j = 0; j < n; ++j) {
    int32_t s = 0;
    for (int c = 0; c < k; ++c) { hw[(size_t)j * kpad + c] = w_nk[(size_t)j * k + c]; s += w_nk[(size_t)j * k + c]; }
    hrs[j] = s;
  }
  cudaMemcpy(g_dw, hw, (size_t)n * kpad, cudaMemcpyHostToDevice);
  cudaMemcpy(g_drs, hrs, (size_t)n * sizeof(int32_t), cudaMemcpyHostToDevice);
  free(hw); free(hrs);
  g_w_key = w_nk; g_n = n; g_k = k; g_kpad = kpad;
  return 0;
}

extern "C" int zf_cuda_linear_s8(const int8_t *a, int m, int lda, const int8_t *w_nk, int n, int k,
                                 int a_zero_point, int32_t *out, int ldc) {
  if (upload_weights(w_nk, n, k)) return 1;
  const int kpad = g_kpad;
  const size_t abytes = (size_t)m * lda, obytes = (size_t)m * n * sizeof(int32_t);
  if (abytes > g_a_cap) { cudaFree(g_da); if (cudaMalloc(&g_da, abytes) != cudaSuccess) return 1; g_a_cap = abytes; }
  if (obytes > g_out_cap) { cudaFree(g_dout); if (cudaMalloc(&g_dout, obytes) != cudaSuccess) return 1; g_out_cap = obytes; }
  cudaMemcpy(g_da, a, abytes, cudaMemcpyHostToDevice);
  dim3 block(16, 16), grid((n + 15) / 16, (m + 15) / 16);
  gemm_dp4a<<<grid, block>>>(g_da, m, lda, g_dw, g_drs, n, kpad, 128 - a_zero_point, g_dout, n);
  if (cudaGetLastError() != cudaSuccess) return 1;
  /* out is [m][n]; ldc may differ, copy row by row if so */
  if (ldc == n) { cudaMemcpy(out, g_dout, obytes, cudaMemcpyDeviceToHost); }
  else for (int i = 0; i < m; ++i) cudaMemcpy(out + (size_t)i * ldc, g_dout + (size_t)i * n, (size_t)n * sizeof(int32_t), cudaMemcpyDeviceToHost);
  return (cudaDeviceSynchronize() == cudaSuccess) ? 0 : 1;
}
