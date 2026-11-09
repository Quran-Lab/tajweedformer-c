/* Native int8 GEMM for the TajweedFormer engine, bit-exact to its int8 export.
 *
 * The engine's int8 linear computes sum_k (q_k - zp) * w_k with q the uint8 DynamicQuantize output.
 * That equals sum_k q_k*w_k - zp*sum_k w_k, an integer, so any kernel that forms the same integer
 * gives the same bits on any machine. Three x86 tiers form it, picked once at run time:
 *
 *   AVX-512 VNNI  VPDPBUSD at 512 bits (u8*s8 -> s32, four-way, no saturation)
 *   AVX-VNNI      the same instruction, VEX-encoded at 256 bits (Alder Lake and later client cores)
 *   AVX2          VPMADDUBSW on |a| and sign(w, a), then VPMADDWD into s32. Exact because the
 *                 activation is the engine's shifted int8 (|a| <= 128) and no weight is -128 (the
 *                 engine's packer refuses one), so a pair of products is at most 2*128*127 = 32512
 *                 and never saturates the int16 lane.
 *
 * A portable scalar path computes the same integer everywhere else. All four share one weight
 * layout: [n/16][k/4][16 units][4 k], k padded to a multiple of 4 with zero weights. */
#ifndef ZF_GEMM_VNNI_H
#define ZF_GEMM_VNNI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Output units packed per tile, and the k group one VPDPBUSD folds. */
#define ZF_VNNI_NT 16
#define ZF_VNNI_K4 4

/* The kernel tiers, slowest first. */
enum { ZF_INT8_SCALAR = 0, ZF_INT8_AVX2 = 1, ZF_INT8_AVXVNNI = 2, ZF_INT8_AVX512VNNI = 3 };

/* 1 if this CPU (and OS) has AVX-512 F + BW + VNNI. Checked once. */
int zf_vnni_available(void);

/* The best tier this CPU and OS support. Checked once. */
int zf_int8_cpu_tier(void);

/* The tier in use: the CPU's best, lowered by the environment variable ZF_NATIVE_INT8 (scalar,
 * avx2, avxvnni, avx512vnni; a tier the CPU lacks falls back to the widest one below it that it has) or by
 * zf_int8_set_tier. Every tier gives the same integers; this exists to test them against each other. */
int zf_int8_tier(void);
/* Whether this CPU and OS can run a tier. The tiers are not nested: Ice Lake and Zen 4 have AVX-512 VNNI but
 * not AVX-VNNI, so a tier below zf_int8_cpu_tier() can be missing. */
int zf_int8_tier_available(int tier);
/* Ask for a tier (lowered to the widest available one not above it); returns the tier now in use. Not
 * thread safe: call it before any kernel runs. */
int zf_int8_set_tier(int tier);
/* The tier the engine's hook uses, or -1 when ZF_NATIVE_INT8=off (the hook declines and the
 * engine's own kernel runs). */
int zf_int8_hook_tier(void);
/* "scalar", "avx2", "avxvnni", "avx512vnni"; "off" for -1. */
const char *zf_int8_tier_name(int tier);
/* The CPU detection's decision and the raw CPUID / XCR0 bits it read, as text. Returns the tier. */
int zf_int8_cpu_report(char *buf, size_t n);

/* Bytes to allocate for packed weights, and the number of row sums (n rounded up to ZF_VNNI_NT). */
size_t zf_vnni_packed_bytes(int n, int k);
int zf_vnni_row_sums_count(int n);

/* Pack row-major weights w[n][k] into the layout above and sum each row. `packed` must hold
 * zf_vnni_packed_bytes(n,k); `row_sums` zf_vnni_row_sums_count(n) ints. */
void zf_vnni_pack(const int8_t *w, int n, int k, int8_t *packed, int32_t *row_sums);

/* out[i][j] = sum_k q[i][k] * w[j][k] - a_zero_point * sum_k w[j][k], the engine's exact int8
 * result. `q` is [m, ldq] uint8 (ldq >= k rounded up to 4, pad zero), `out` has row stride `ldc`.
 * The AVX2 tier needs weights in [-127, 127]; with a -128 weight call this only on the VNNI tiers
 * or the scalar one. */
void zf_vnni_linear_u8(const uint8_t *q, int m, int ldq, const int8_t *packed, const int32_t *row_sums,
                       int n, int k, int a_zero_point, int32_t *out, int ldc);

/* Drop-in for the engine's zf_linear_s8: `a` is the engine's SHIFTED int8 activation (q - 128, at
 * row stride `lda` >= k rounded up to 4, pad zero). Same int32 as the engine, bit for bit. */
void zf_vnni_linear_s8(const int8_t *a, int m, int lda, const int8_t *packed, const int32_t *row_sums,
                       int n, int k, int a_zero_point, int32_t *out, int ldc);

/* The same over output columns [c0, c1) only, written to out[i * ldc + j - j_shift]: what the
 * engine's hook needs for a pooled sub-range of panels. */
void zf_vnni_linear_s8_cols(const int8_t *a, int m, int lda, const int8_t *packed,
                            const int32_t *row_sums, int n, int k, int a_zero_point, int32_t *out,
                            int ldc, int c0, int c1, int j_shift);

/* k rounded up to the ZF_VNNI_K4 group, i.e. the ldq/pad width the kernel expects for `q`. */
int zf_vnni_kpad(int k);

/* Calls the engine's hook took per tier (index by ZF_INT8_*), and calls it declined (index 4).
 * Counted only when the environment has ZF_NATIVE_STATS=1, which also prints them at exit. */
long long zf_int8_hook_calls(int tier);

#ifdef __cplusplus
}
#endif
#endif /* ZF_GEMM_VNNI_H */
