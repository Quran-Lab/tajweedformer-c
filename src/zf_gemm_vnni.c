/* The native int8 kernels: AVX-512 VNNI, AVX-VNNI (256-bit), AVX2 and scalar, one weight layout,
 * the same integer from each (see include/zf_gemm_vnni.h for why each is exact). */
#include "int8_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#error "zipformer-native needs GCC or clang: the engine cannot be built by cl.exe"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define ZF_X86 1
#else
#define ZF_X86 0
#endif

static int round_up(int v, int m) { return (v + m - 1) / m * m; }
int zf_vnni_kpad(int k) { return round_up(k, ZF_VNNI_K4); }
int zf_vnni_row_sums_count(int n) { return round_up(n, ZF_VNNI_NT); }
size_t zf_vnni_packed_bytes(int n, int k) {
  return (size_t)(round_up(n, ZF_VNNI_NT) / ZF_VNNI_NT) * (zf_vnni_kpad(k) / ZF_VNNI_K4) * ZF_VNNI_NT * ZF_VNNI_K4;
}

/* --- runtime feature detection, CPU and OS (XSAVE state enabled for the registers) ---
 *
 * The bits, as the Intel SDM (Vol. 2A, CPUID; Vol. 1 13.3, XCR0) and AMD's APM define them. A tier is used only
 * when the CPU reports every instruction set its kernel needs AND the OS saves the registers it touches:
 *   leaf 0:            EAX = the largest standard leaf; leaf 7 is read only when it is >= 7
 *   leaf 1, ECX:       bit 27 OSXSAVE (XGETBV usable), bit 28 AVX
 *   XCR0 (XGETBV 0):   bit 1 SSE (XMM), bit 2 AVX (YMM upper halves): both for AVX2 and AVX-VNNI;
 *                      plus bit 5 opmask, bit 6 ZMM_Hi256, bit 7 Hi16_ZMM for AVX-512 (mask 0xE6)
 *   leaf 7 subleaf 0:  EAX = the largest subleaf; EBX bit 5 AVX2, bit 16 AVX512F, bit 30 AVX512BW,
 *                      bit 31 AVX512VL; ECX bit 11 AVX512_VNNI
 *   leaf 7 subleaf 1:  EAX bit 4 AVX-VNNI (read only when subleaf 1 exists)
 * AVX-512 VNNI tier: AVX512F + AVX512BW + AVX512VL + AVX512_VNNI + OSXSAVE + XCR0 0xE6.
 * AVX-VNNI tier:     AVX-VNNI + AVX2 + OSXSAVE + XCR0 bits 1, 2.
 * AVX2 tier:         AVX2 + OSXSAVE + XCR0 bits 1, 2 (FMA is not needed: the kernels are integer).
 * Only the per-tier translation units are compiled with those instruction sets, and nothing in them runs
 * unless its tier was selected here (no initialisers, no shared code). Checked on AVX-VNNI and AVX2 machines;
 * the AVX-512 VNNI selection is UNVERIFIED on AVX-512 hardware (none was available). */
#if ZF_X86
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static void cpuidex(int leaf, int sub, unsigned r[4]) {
  int v[4];
  __cpuidex(v, leaf, sub);
  for (int i = 0; i < 4; ++i) r[i] = (unsigned)v[i];
}
static unsigned long long xgetbv0(void) { return _xgetbv(0); }
#else
static void cpuidex(int leaf, int sub, unsigned r[4]) {
  unsigned a, b, c, d;
  __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(sub));
  r[0] = a; r[1] = b; r[2] = c; r[3] = d;
}
/* the raw XGETBV encoding, so no -mxsave target attribute is needed (it runs only after OSXSAVE is seen) */
static unsigned long long xgetbv0(void) {
  unsigned a, d;
  __asm__ __volatile__(".byte 0x0f, 0x01, 0xd0" : "=a"(a), "=d"(d) : "c"(0));
  return ((unsigned long long)d << 32) | a;
}
#endif
#endif

typedef struct {
  unsigned max_leaf, max_sub7, l1_ecx, l7_ebx, l7_ecx, l7s1_eax;
  unsigned long long xcr0;
  int read_xcr0, read_l7, read_l7s1;
} CpuBits;

static void read_bits(CpuBits *c) {
  memset(c, 0, sizeof *c);
#if ZF_X86
  unsigned r[4];
  cpuidex(0, 0, r);
  c->max_leaf = r[0];
  if (c->max_leaf >= 1) {
    cpuidex(1, 0, r);
    c->l1_ecx = r[2];
  }
  if ((c->l1_ecx >> 27) & 1) {
    c->xcr0 = xgetbv0();
    c->read_xcr0 = 1;
  }
  if (c->max_leaf >= 7) {
    cpuidex(7, 0, r);
    c->max_sub7 = r[0];
    c->l7_ebx = r[1];
    c->l7_ecx = r[2];
    c->read_l7 = 1;
    if (c->max_sub7 >= 1) {
      cpuidex(7, 1, r);
      c->l7s1_eax = r[0];
      c->read_l7s1 = 1;
    }
  }
#endif
}

/* Whether this CPU and OS can run a tier. The tiers are not nested: Ice Lake and Zen 4 have AVX-512 VNNI but
 * not AVX-VNNI, so a tier below the best one may still be missing. */
static int tier_ok(const CpuBits *c, int tier) {
  if (tier == ZF_INT8_SCALAR) return 1;
  const int osxsave = (c->l1_ecx >> 27) & 1;
  if (!osxsave || !c->read_xcr0 || !c->read_l7) return 0;
  const int ymm = (c->xcr0 & 0x6) == 0x6, zmm = (c->xcr0 & 0xE6) == 0xE6;
  const int avx2 = (c->l7_ebx >> 5) & 1;
  const int f = (c->l7_ebx >> 16) & 1, bw = (c->l7_ebx >> 30) & 1, vl = (c->l7_ebx >> 31) & 1, vnni = (c->l7_ecx >> 11) & 1;
  const int avxvnni = c->read_l7s1 && ((c->l7s1_eax >> 4) & 1);
  switch (tier) {
    case ZF_INT8_AVX512VNNI: return f && bw && vl && vnni && zmm;
    case ZF_INT8_AVXVNNI: return avxvnni && avx2 && ymm;
    case ZF_INT8_AVX2: return avx2 && ymm;
    default: return 0;
  }
}

static int tier_of(const CpuBits *c) {
  int t = ZF_INT8_AVX512VNNI;
  while (t > ZF_INT8_SCALAR && !tier_ok(c, t)) --t;
  return t;
}

static int detect_tier(void) {
  CpuBits c;
  read_bits(&c);
  return tier_of(&c);
}

/* The decision and the raw bits, for diagnostics (test/cpu_detect). Returns the tier. */
int zf_int8_cpu_report(char *buf, size_t n) {
  CpuBits c;
  read_bits(&c);
  const int t = tier_of(&c);
  if (buf && n)
    snprintf(buf, n,
             "tier=%s max_leaf=%u leaf1.ecx=0x%08x (osxsave=%u avx=%u) xcr0=0x%llx%s (xmm=%u ymm=%u opmask=%u zmm_hi256=%u hi16_zmm=%u) "
             "leaf7.0: max_sub=%u ebx=0x%08x ecx=0x%08x (avx2=%u avx512f=%u avx512bw=%u avx512vl=%u avx512_vnni=%u) "
             "leaf7.1: eax=0x%08x%s (avx_vnni=%u)",
             zf_int8_tier_name(t), c.max_leaf, c.l1_ecx, (c.l1_ecx >> 27) & 1, (c.l1_ecx >> 28) & 1, c.xcr0,
             c.read_xcr0 ? "" : " (not read)", (unsigned)(c.xcr0 >> 1) & 1, (unsigned)(c.xcr0 >> 2) & 1,
             (unsigned)(c.xcr0 >> 5) & 1, (unsigned)(c.xcr0 >> 6) & 1, (unsigned)(c.xcr0 >> 7) & 1, c.max_sub7, c.l7_ebx,
             c.l7_ecx, (c.l7_ebx >> 5) & 1, (c.l7_ebx >> 16) & 1, (c.l7_ebx >> 30) & 1, (c.l7_ebx >> 31) & 1,
             (c.l7_ecx >> 11) & 1, c.l7s1_eax, c.read_l7s1 ? "" : " (not read)", (c.l7s1_eax >> 4) & 1);
  return t;
}

static int g_cpu_tier = -1, g_tier = -2;
int zf_int8_cpu_tier(void) {
  int t = __atomic_load_n(&g_cpu_tier, __ATOMIC_ACQUIRE);
  if (t < 0) { t = detect_tier(); __atomic_store_n(&g_cpu_tier, t, __ATOMIC_RELEASE); }
  return t;
}
int zf_vnni_available(void) { return zf_int8_cpu_tier() == ZF_INT8_AVX512VNNI; }

static const char *const k_names[] = {"scalar", "avx2", "avxvnni", "avx512vnni"};
const char *zf_int8_tier_name(int tier) {
  return tier >= 0 && tier <= ZF_INT8_AVX512VNNI ? k_names[tier] : "off";
}

int zf_int8_tier_available(int tier) {
  if (tier < ZF_INT8_SCALAR || tier > ZF_INT8_AVX512VNNI) return 0;
  CpuBits c;
  read_bits(&c);
  return tier_ok(&c, tier);
}

/* The tier to run for a request: the widest available one not above it (-1, "off", stays). */
static int usable_tier(int tier) {
  while (tier > ZF_INT8_SCALAR && !zf_int8_tier_available(tier)) --tier;
  return tier;
}

int zf_int8_set_tier(int tier) {
  if (tier > ZF_INT8_AVX512VNNI) tier = ZF_INT8_AVX512VNNI;
  if (tier < -1) tier = -1;
  tier = usable_tier(tier);
  __atomic_store_n(&g_tier, tier, __ATOMIC_RELEASE);
  return tier;
}

/* -1 is "off": the hook declines and the engine's own kernel runs (zf_int8_tier never returns it
 * for the direct entry points, which then run scalar). */
static int tier_from_env(void) {
  const int best = zf_int8_cpu_tier();
  const char *e = getenv("ZF_NATIVE_INT8");
  if (!e || !*e) return best;
  if (!strcmp(e, "off") || !strcmp(e, "engine")) return -1;
  for (int t = 0; t <= ZF_INT8_AVX512VNNI; ++t)
    if (!strcmp(e, k_names[t])) return usable_tier(t);
  return best;
}
static int tier_or_off(void) {
  int t = __atomic_load_n(&g_tier, __ATOMIC_ACQUIRE);
  if (t == -2) { t = tier_from_env(); __atomic_store_n(&g_tier, t, __ATOMIC_RELEASE); }
  return t;
}
int zf_int8_tier(void) { const int t = tier_or_off(); return t < 0 ? ZF_INT8_SCALAR : t; }
/* for the hook (zf_vnni_hook.c): the tier, or -1 to decline */
int zf_int8_hook_tier(void) { return tier_or_off(); }

/* --- statistics (ZF_NATIVE_STATS=1) --- */
static long long g_calls[5];
static int g_stats = -1;
static void print_stats(void) {
  fprintf(stderr, "zf_native int8 hook: cpu=%s tier=%s calls avx512vnni=%lld avxvnni=%lld avx2=%lld scalar=%lld declined=%lld\n",
          zf_int8_tier_name(zf_int8_cpu_tier()), zf_int8_tier_name(tier_or_off()),
          g_calls[3], g_calls[2], g_calls[1], g_calls[0], g_calls[4]);
}
int zf_int8_stats_on(void) {
  int s = __atomic_load_n(&g_stats, __ATOMIC_ACQUIRE);
  if (s < 0) {
    const char *e = getenv("ZF_NATIVE_STATS");
    s = (e && *e == '1') ? 1 : 0;
    int expected = -1;
    if (__atomic_compare_exchange_n(&g_stats, &expected, s, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      if (s) atexit(print_stats);
    } else {
      s = expected;
    }
  }
  return s;
}
void zf_int8_count(int slot) { __atomic_fetch_add(&g_calls[slot], 1, __ATOMIC_RELAXED); }
long long zf_int8_hook_calls(int tier) { return tier >= 0 && tier <= 4 ? __atomic_load_n(&g_calls[tier], __ATOMIC_RELAXED) : 0; }

void zf_vnni_pack(const int8_t *w, int n, int k, int8_t *packed, int32_t *row_sums) {
  const int kp = zf_vnni_kpad(k), nt = round_up(n, ZF_VNNI_NT) / ZF_VNNI_NT, g = kp / ZF_VNNI_K4;
  memset(packed, 0, zf_vnni_packed_bytes(n, k));
  for (int j = 0; j < nt * ZF_VNNI_NT; ++j) row_sums[j] = 0;
  for (int j = 0; j < n; ++j) { int32_t s = 0; for (int c = 0; c < k; ++c) s += w[(size_t)j * k + c]; row_sums[j] = s; }
  for (int t = 0; t < nt; ++t)
    for (int gi = 0; gi < g; ++gi)
      for (int c = 0; c < ZF_VNNI_NT; ++c)
        for (int kk = 0; kk < ZF_VNNI_K4; ++kk) {
          const int col = t * ZF_VNNI_NT + c, kidx = gi * ZF_VNNI_K4 + kk;
          packed[((size_t)t * g + gi) * ZF_VNNI_NT * ZF_VNNI_K4 + (size_t)c * ZF_VNNI_K4 + kk] =
              (col < n && kidx < k) ? w[(size_t)col * k + kidx] : 0;
        }
}

/* The byte transform applied to each activation byte to get the uint8 q: 0 for uint8 input, 0x80
 * for the engine's shifted int8 (a = q - 128). */
#define XOR_U8 0
#define XOR_S8 0x80

/* Scalar: identical integer arithmetic, for CPUs without AVX2 and for verification. */
static void k_scalar(const uint8_t *q, int m, int ldq, const int8_t *packed, const int32_t *rs, int kp,
                     int zp, int32_t *out, int ldc, int xu, int c0, int c1, int jsh) {
  const int g = kp / ZF_VNNI_K4;
  for (int t = c0 / ZF_VNNI_NT; t * ZF_VNNI_NT < c1; ++t) {
    const int8_t *panel = packed + (size_t)t * g * ZF_VNNI_NT * ZF_VNNI_K4;
    const int base = t * ZF_VNNI_NT, lo = imax(base, c0), hi = imin(base + ZF_VNNI_NT, c1);
    for (int i = 0; i < m; ++i)
      for (int c = lo; c < hi; ++c) {
        int32_t acc = 0;
        for (int gi = 0; gi < g; ++gi)
          for (int kk = 0; kk < ZF_VNNI_K4; ++kk)
            acc += (int32_t)(uint8_t)(q[(size_t)i * ldq + gi * ZF_VNNI_K4 + kk] ^ xu) *
                   (int32_t)panel[(size_t)gi * ZF_VNNI_NT * ZF_VNNI_K4 + (size_t)(c - base) * ZF_VNNI_K4 + kk];
        out[(size_t)i * ldc + c - jsh] = (int32_t)((uint32_t)acc - (uint32_t)zp * (uint32_t)rs[c]);
      }
  }
}


static void dispatch(int tier, const uint8_t *q, int m, int ldq, const int8_t *packed, const int32_t *rs,
                     int n, int k, int a_zp, int32_t *out, int ldc, int xu, int c0, int c1, int jsh) {
  const int kp = zf_vnni_kpad(k);
  if (c1 > n) c1 = n;
  if (c0 < 0) c0 = 0;
  if (m <= 0 || c0 >= c1) return;
#if ZF_X86
  switch (tier) {
    case ZF_INT8_AVX512VNNI: zf_int8_k_avx512(q, m, ldq, packed, rs, kp, a_zp, out, ldc, xu, c0, c1, jsh); return;
    case ZF_INT8_AVXVNNI: zf_int8_k_avxvnni(q, m, ldq, packed, rs, kp, a_zp, out, ldc, xu, c0, c1, jsh); return;
    case ZF_INT8_AVX2: zf_int8_k_avx2(q, m, ldq, packed, rs, kp, a_zp, out, ldc, xu, c0, c1, jsh); return;
    default: break;
  }
#else
  (void)tier;
#endif
  k_scalar(q, m, ldq, packed, rs, kp, a_zp, out, ldc, xu, c0, c1, jsh);
}

void zf_vnni_linear_u8(const uint8_t *q, int m, int ldq, const int8_t *packed, const int32_t *row_sums,
                       int n, int k, int a_zero_point, int32_t *out, int ldc) {
  dispatch(zf_int8_tier(), q, m, ldq, packed, row_sums, n, k, a_zero_point, out, ldc, XOR_U8, 0, n, 0);
}

void zf_vnni_linear_s8(const int8_t *a, int m, int lda, const int8_t *packed, const int32_t *row_sums,
                       int n, int k, int a_zero_point, int32_t *out, int ldc) {
  dispatch(zf_int8_tier(), (const uint8_t *)a, m, lda, packed, row_sums, n, k, a_zero_point, out, ldc,
           XOR_S8, 0, n, 0);
}

void zf_vnni_linear_s8_cols(const int8_t *a, int m, int lda, const int8_t *packed,
                            const int32_t *row_sums, int n, int k, int a_zero_point, int32_t *out,
                            int ldc, int c0, int c1, int j_shift) {
  dispatch(zf_int8_tier(), (const uint8_t *)a, m, lda, packed, row_sums, n, k, a_zero_point, out, ldc,
           XOR_S8, c0, c1, j_shift);
}
