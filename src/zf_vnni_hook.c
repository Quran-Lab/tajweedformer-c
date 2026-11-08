/* The native int8 accelerator behind the engine's weak zf_s8_linear_hook (engine/src/kernels.c).
 *
 * The engine offers every int8 linear and lowered convolution to this hook before its own kernel:
 * rows [row_begin, row_end) against unit blocks [block_begin, block_end) (ZF_S8_UNIT_BLOCK units
 * each), written to out[i * ldc + j - j_shift]. This computes exactly that int32 on the best x86
 * tier present (AVX-512 VNNI, AVX-VNNI, AVX2; see zf_gemm_vnni.h), any sub-range, from any thread,
 * and returns 1; it returns 0 (the engine's kernel runs) only when ZF_NATIVE_INT8=off or memory runs
 * out. Integer arithmetic throughout, so it cannot move a bit: test/dropin_check.c and
 * test/hooked_check.c hold every tier to the engine's kernel, and the engine's own conformance sets
 * hold the whole decode to ONNX Runtime.
 *
 * The kernels need their own weight layout. Rather than change the engine's loader, each weight is
 * re-packed lazily from the engine's packed panels (reconstructed to row major, then packed) the
 * first time it is seen and cached for the life of the process. The cache is a lock-free open-
 * addressed table keyed by the engine's panel pointer: readers take no lock; inserting takes one. A
 * fingerprint of the panel's first and last bytes and its shape guards against a pointer reused for
 * another weight after a model is destroyed; a stale entry is replaced (the old copy is kept, never
 * freed, so a reader still holding it stays valid). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK g_lock = SRWLOCK_INIT;
#define CACHE_LOCK() AcquireSRWLockExclusive(&g_lock)
#define CACHE_UNLOCK() ReleaseSRWLockExclusive(&g_lock)
#else
#include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define CACHE_LOCK() pthread_mutex_lock(&g_lock)
#define CACHE_UNLOCK() pthread_mutex_unlock(&g_lock)
#endif

#include "zf/kernels.h"     /* ZfWeightsS8, ZF_S8_UNIT_BLOCK, ZF_S8_K_GROUP */
#include "zf_gemm_vnni.h"

int zf_int8_stats_on(void);
void zf_int8_count(int slot);

typedef struct {
  int n, k, kp;
  uint64_t sig;
  int8_t *packed;
  int32_t *row_sums;
} Packed;

/* A few hundred weights per model and context; a server may hold several models at once. */
#define CACHE_SLOTS 16384
static struct { const int8_t *key; Packed *val; } g_cache[CACHE_SLOTS];

static uint64_t weight_sig(const ZfWeightsS8 *w) {
  const size_t bytes = zf_weights_s8_packed_bytes(w->n, w->k);
  uint64_t h = 1469598103934665603ull ^ ((uint64_t)w->n << 32) ^ ((uint64_t)w->k << 8) ^ (uint64_t)w->k_pad;
  const size_t take = bytes < 64 ? bytes : 64;
  for (size_t i = 0; i < take; ++i) { h ^= (uint8_t)w->packed[i]; h *= 1099511628211ull; }
  for (size_t i = bytes - take; i < bytes; ++i) { h ^= (uint8_t)w->packed[i]; h *= 1099511628211ull; }
  return h;
}

static size_t slot_of(const void *p) {
  uint64_t x = (uint64_t)(uintptr_t)p;
  x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
  return (size_t)(x & (CACHE_SLOTS - 1));
}

/* Row-major weights from the engine's panels: unit j, tap c ->
 * packed[(j/UB)*UB*k_pad + (c/KG)*UB*KG + (j%UB)*KG + (c%KG)]. */
static Packed *build(const ZfWeightsS8 *w, uint64_t sig) {
  const int n = w->n, k = w->k, kp = w->k_pad;
  const int UB = ZF_S8_UNIT_BLOCK, KG = ZF_S8_K_GROUP;
  Packed *p = (Packed *)malloc(sizeof *p);
  int8_t *w_nk = (int8_t *)malloc((size_t)n * k);
  int8_t *packed = (int8_t *)malloc(zf_vnni_packed_bytes(n, k));
  int32_t *row_sums = (int32_t *)malloc((size_t)zf_vnni_row_sums_count(n) * sizeof(int32_t));
  if (!p || !w_nk || !packed || !row_sums) { free(p); free(w_nk); free(packed); free(row_sums); return NULL; }
  for (int j = 0; j < n; ++j)
    for (int c = 0; c < k; ++c)
      w_nk[(size_t)j * k + c] =
          w->packed[(size_t)(j / UB) * UB * kp + (size_t)(c / KG) * UB * KG + (size_t)(j % UB) * KG + (c % KG)];
  zf_vnni_pack(w_nk, n, k, packed, row_sums);
  free(w_nk);
  p->n = n; p->k = k; p->kp = kp; p->sig = sig; p->packed = packed; p->row_sums = row_sums;
  return p;
}

static int matches(const Packed *p, const ZfWeightsS8 *w, uint64_t sig) {
  return p && p->n == w->n && p->k == w->k && p->kp == w->k_pad && p->sig == sig;
}

static const Packed *lookup(const ZfWeightsS8 *w) {
  const uint64_t sig = weight_sig(w);
  const size_t h = slot_of(w->packed);
  for (size_t probe = 0; probe < CACHE_SLOTS; ++probe) {
    const size_t s = (h + probe) & (CACHE_SLOTS - 1);
    const int8_t *key = __atomic_load_n(&g_cache[s].key, __ATOMIC_ACQUIRE);
    if (!key) break;                                   /* not cached yet */
    if (key == w->packed) {
      const Packed *p = __atomic_load_n(&g_cache[s].val, __ATOMIC_ACQUIRE);
      if (matches(p, w, sig)) return p;
      break;                                           /* stale: rebuild under the lock */
    }
  }
  CACHE_LOCK();
  const Packed *result = NULL;
  for (size_t probe = 0; probe < CACHE_SLOTS; ++probe) {
    const size_t s = (h + probe) & (CACHE_SLOTS - 1);
    const int8_t *key = g_cache[s].key;
    if (key && key != w->packed) continue;
    const Packed *cur = key ? g_cache[s].val : NULL;
    if (matches(cur, w, sig)) { result = cur; break; }  /* another thread got here first */
    Packed *p = build(w, sig);
    if (!p) break;
    __atomic_store_n(&g_cache[s].val, p, __ATOMIC_RELEASE);
    if (!key) __atomic_store_n(&g_cache[s].key, w->packed, __ATOMIC_RELEASE);
    result = p;
    break;
  }
  CACHE_UNLOCK();
  return result;
}

int zf_s8_linear_hook(const ZfWeightsS8 *w, const int8_t *a, int row_begin, int row_end, int lda,
                      int a_zero_point, int32_t *out, int ldc, int block_begin, int block_end,
                      int j_shift) {
  const int tier = zf_int8_hook_tier();
  const int stats = zf_int8_stats_on();
  if (tier < 0) { if (stats) zf_int8_count(4); return 0; }
  const Packed *p = lookup(w);
  if (!p) { if (stats) zf_int8_count(4); return 0; }
  const int m = row_end - row_begin;
  if (stats) zf_int8_count(tier);
  if (m <= 0) return 1;
  int c0 = block_begin * ZF_S8_UNIT_BLOCK, c1 = block_end * ZF_S8_UNIT_BLOCK;
  if (c1 > w->n) c1 = w->n;
  zf_vnni_linear_s8_cols(a + (size_t)row_begin * lda, m, lda, p->packed, p->row_sums, w->n, w->k,
                         a_zero_point, out + (size_t)row_begin * ldc, ldc, c0, c1, j_shift);
  return 1;
}
