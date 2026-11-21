/* tf_tests: the library's 3.x tests that need no model, kept for SDK 4.0 (the client, the signed config, sharing,
 * consent, the bitrate ladder, sdk_latest, the error catalogue, the shared 3.0 vectors of the decoded layer). The
 * model, the engine and the folder tests of 3.x are gone with the engine: SDK 4.0's own are test/tf40_tests.c.
 *
 *   tf_tests crypto
 *   tf_tests errors <data dir>
 *   tf_tests vectors30 <data dir> <work dir>
 *   tf_tests ogg | codec | sharing | upload | client | bitrate | consent | sdk_latest   (as in 3.x)
 *   tf_tests join_window <work>   flags.share_join_ms read as 0 to 60000 (SDK 4.0, contract section 23)
 *
 * The work folder (the last argument) is emptied before the test runs. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* sched_getaffinity, CPU_COUNT (the pinning test) */
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tajweedformer.h"
#include "tf_internal.h"
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

/* No test may need more than 2 GB: the process caps its own memory before anything runs, so a test that runs away
 * fails on an allocation instead of taking the machine with it (Windows: a job object's limit on committed memory;
 * Linux: RLIMIT_DATA, the private writable memory). Where the cap cannot be set the tests run without it. */
#define TEST_MEMORY_CAP (2048ull << 20)
static void cap_own_memory(void) {
#if defined(_WIN32)
  HANDLE job = CreateJobObjectW(NULL, NULL);
  if (!job) return;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
  memset(&lim, 0, sizeof lim);
  lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
  lim.ProcessMemoryLimit = (SIZE_T)TEST_MEMORY_CAP;
  if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim)) AssignProcessToJobObject(job, GetCurrentProcess());
#elif defined(__linux__)
  struct rlimit rl;
  if (!getrlimit(RLIMIT_DATA, &rl) && (rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= TEST_MEMORY_CAP)) {
    rl.rlim_cur = (rlim_t)TEST_MEMORY_CAP;
    setrlimit(RLIMIT_DATA, &rl);
  }
#endif
}
/* the memory this process has committed, in bytes (0 where it cannot be read) */
static long long own_memory(void) {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pc;
  memset(&pc, 0, sizeof pc);
  return K32GetProcessMemoryInfo(GetCurrentProcess(), &pc, sizeof pc) ? (long long)pc.PagefileUsage : 0;
#elif defined(__linux__)
  long long pages = 0, resident = 0;
  FILE *f = fopen("/proc/self/statm", "r");
  if (!f) return 0;
  if (fscanf(f, "%lld %lld", &pages, &resident) != 2) resident = 0;
  fclose(f);
  return resident * (long long)sysconf(_SC_PAGESIZE);
#else
  return 0;
#endif
}
/* the most memory this process ever had committed (Windows) or resident (Linux), in bytes; 0 when unknown */
static long long own_peak_memory(void) {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pc;
  memset(&pc, 0, sizeof pc);
  return K32GetProcessMemoryInfo(GetCurrentProcess(), &pc, sizeof pc) ? (long long)pc.PeakPagefileUsage : 0;
#elif defined(__linux__)
  char line[160];
  long long kb = 0;
  FILE *f = fopen("/proc/self/status", "r");
  while (f && fgets(line, sizeof line, f))
    if (!strncmp(line, "VmHWM:", 6)) kb = atoll(line + 6);
  if (f) fclose(f);
  return kb * 1024;
#else
  return 0;
#endif
}

static const char *const kOut[12] = {"attr_face", "attr_ghunna", "attr_qalqalah", "attr_tafkheem", "boundary", "conf",
                                     "gem", "id_log_probs", "inter1_log_probs", "inter4_log_probs", "log_probs",
                                     "timing_log_probs"};
#define FNV0 1469598103934665603ull
static uint64_t fnv1a(uint64_t h, const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

static int fails;
#define CHECK(c, ...)                                  \
  do {                                                 \
    if (!(c)) {                                        \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                    \
      fprintf(stderr, "\n");                           \
      fails++;                                         \
    }                                                  \
  } while (0)

/* ------------------------------------------------------------------ crypto */
static void hex(const char *h, uint8_t *o, size_t n) {
  if (tf_hex_decode(h, o, n)) {
    fprintf(stderr, "bad hex in test\n");
    exit(2);
  }
}
static int crypto_tests(void) {
  uint8_t d[64];
  char x[129];
  tf_sha256("abc", 3, d);
  tf_hex_encode(d, 32, x);
  CHECK(!strcmp(x, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc) %s", x);
  tf_sha512("abc", 3, d);
  tf_hex_encode(d, 64, x);
  CHECK(!strcmp(x, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"),
        "sha512(abc) %s", x);
  {
    /* 1,000,000 x 'a' across block boundaries */
    char *a = (char *)malloc(1000000);
    memset(a, 'a', 1000000);
    TfSha256 c;
    tf_sha256_init(&c);
    for (int i = 0; i < 1000000; i += 777) tf_sha256_update(&c, a + i, i + 777 > 1000000 ? 1000000 - i : 777);
    tf_sha256_final(&c, d);
    tf_hex_encode(d, 32, x);
    CHECK(!strcmp(x, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256(1M a) %s", x);
    free(a);
  }
  /* RFC 8032 section 7.1, tests 1, 2 and 3 */
  static const struct { const char *pk, *msg, *sig; } v[] = {
      {"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
       "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
      {"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
       "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
      {"fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
       "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
  };
  for (size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
    uint8_t pk[32], sig[64], msg[8];
    hex(v[i].pk, pk, 32);
    hex(v[i].sig, sig, 64);
    const size_t n = strlen(v[i].msg) / 2;
    hex(v[i].msg, msg, n);
    CHECK(tf_ed25519_verify(sig, msg, n, pk) == 1, "RFC 8032 vector %d does not verify", (int)i + 1);
    sig[5] ^= 1;
    CHECK(tf_ed25519_verify(sig, msg, n, pk) == 0, "RFC 8032 vector %d verifies with a flipped bit", (int)i + 1);
    sig[5] ^= 1;
    if (n) {
      msg[0] ^= 0x80;
      CHECK(tf_ed25519_verify(sig, msg, n, pk) == 0, "RFC 8032 vector %d verifies a changed message", (int)i + 1);
    }
  }
  /* base64 */
  uint8_t b[8];
  CHECK(tf_base64_decode("aGVsbG8=", b, sizeof b) == 5 && !memcmp(b, "hello", 5), "base64");
  CHECK(tf_base64_decode("aGVsbG8", b, sizeof b) == 5, "base64 unpadded");
  /* JSON round trip */
  const char *j = "{\"a\":[1,2.5,-3e-2,true,null,\"x\\u00e9\\n\"],\"b\":{\"c\":\"d\"}}";
  TfJson *p = tf_json_parse(j, strlen(j), NULL);
  CHECK(p != NULL, "json parse");
  if (p) {
    TfBuf o;
    tf_buf_init(&o);
    tf_json_write(&o, p);
    CHECK(o.p && !strcmp(o.p, "{\"a\":[1,2.5,-0.03,true,null,\"x\xc3\xa9\\n\"],\"b\":{\"c\":\"d\"}}"), "json write %s", o.p);
    tf_buf_free(&o);
    tf_json_free(p);
  }
  CHECK(tf_json_parse("{\"a\":}", 6, NULL) == NULL, "bad json accepted");
  printf("crypto: %s\n", fails ? "FAIL" : "ok (sha256, sha512, RFC 8032 x3, base64, json)");
  return fails ? 1 : 0;
}

#define TF_NO_ENGINE 1 /* the 3.x engine and model-folder tests of the includes are left out */
#include "tf_client_tests.inc"
#include "tf_share_tests.inc"
#include "tf_201_tests.inc"
#include "tf_30_tests.inc"

/* Every run starts from an empty work folder, as tf40_tests' do. A test that meets the folder of an earlier run (ctest
 * --repeat, or one test run again without tf_work_clean) finds that run's outbox, consent, ask and counters, and fails
 * for no fault of the code: the bitrate stream then sees two recordings and reads none, the upload test counts two
 * shared sessions, the consent test has no first ask. */
static int fresh_work(int argc, char **argv) {
  static const struct {
    const char *name;
    int argc, work; /* the arguments the test needs, and the one that is its work folder */
  } k[] = {{"vectors30", 4, 3}, {"sharing", 5, 4}, {"ogg", 4, 3},    {"codec", 4, 3},      {"upload", 3, 2},   {"join_window", 3, 2},
           {"client", 4, 3},    {"bitrate", 4, 3}, {"consent", 5, 4}, {"sdk_latest", 4, 3}, {"telemetry", 3, 2}};
  for (size_t i = 0; argc >= 2 && i < sizeof k / sizeof k[0]; ++i)
    if (argc >= k[i].argc && !strcmp(argv[1], k[i].name)) {
      /* a folder that will not go (a file still held open) stops the run here, not as a test failure later */
      if (tf__remove_tree(argv[k[i].work]) || tf_mkdirs(argv[k[i].work])) {
        fprintf(stderr, "the work folder %s could not be emptied%c", argv[k[i].work], 10);
        return 2;
      }
    }
  return 0;
}

int main(int argc, char **argv) {
  cap_own_memory();
  tf__client_no_timer = 1; /* the replayed exchanges and stubs run on the test's thread */
  if (fresh_work(argc, argv)) return 2;
  if (argc >= 2 && !strcmp(argv[1], "crypto")) return crypto_tests();
  if (argc >= 3 && !strcmp(argv[1], "errors")) return errors_tests(argv[2]);
  if (argc >= 4 && !strcmp(argv[1], "vectors30")) return vectors30_tests(argv[2], argv[3]);
  if (argc >= 5 && !strcmp(argv[1], "sharing")) return sharing_tests(argv[2], argv[3], argv[4]);
  if (argc >= 4 && !strcmp(argv[1], "ogg")) return ogg_tests(argv[2], argv[3]);
  if (argc >= 4 && !strcmp(argv[1], "codec")) return codec_tests(argv[2], argv[3]);
  if (argc >= 3 && !strcmp(argv[1], "upload")) return upload_tests(argv[2]);
  if (argc >= 3 && !strcmp(argv[1], "join_window")) return join_window_tests(argv[2]);
  if (argc >= 4 && !strcmp(argv[1], "client")) return client_tests(argv[2], argv[3]);
  if (argc >= 4 && !strcmp(argv[1], "bitrate")) return bitrate_tests(argv[2], argv[3]);
  if (argc >= 5 && !strcmp(argv[1], "consent")) return consent_tests(argv[2], argv[3], argv[4]);
  if (argc >= 4 && !strcmp(argv[1], "sdk_latest")) return sdk_latest_tests(argv[2], argv[3]);
  if (argc >= 3 && !strcmp(argv[1], "telemetry")) return telemetry_tests(argv[2]);
  fprintf(stderr, "usage: see the comment at the top of test/tf_tests.c%c", 10);
  return 2;
}
