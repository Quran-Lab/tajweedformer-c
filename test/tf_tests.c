/* tf_tests: the tajweedformer library's tests. The public API (include/tajweedformer.h) where a test is about what
 * an app sees, and the internals (src/tf/tf_internal.h, linked from the static library) where it is about a
 * piece behind it. Some runs write what the library produced for the checkers in test/ (check_outputs.py, the
 * engine's v4_conformance_check.py) to compare with the reference files.
 *
 *   tf_tests crypto
 *   tf_tests errors <data dir>                  the embedded error catalogue (3.0: tf_30_tests.inc)
 *   tf_tests vectors30 <data dir> <work dir>    the shared 3.0 vectors: manifest, speech, settled, telemetry, names, ...
 *   tf_tests api <work dir> <data dir>          the public Client: one per process, options, diagnose, storage
 *   tf_tests model30 <local30 dir> <key_hex> <work dir> [<configs.json> [full|prod]]   the local 3.0 model folders:
 *                                               Model, Session, listen, reference, the Client's download path
 *   tf_tests share30 <data dir> <work dir>      shared recordings in 3.0: sharing_30.json, the ring, the tail wait
 *   tf_tests net <url> <work dir>               the model download through the real HTTP code, against the stand-in
 *                                               server that test/net_stand_in.py starts (3.1.3: tf_313_net_tests.inc)
 *   tf_tests conformance <folder> <key_hex|-> <clips.txt> <out.jsonl> [heads_mask [context]]
 *   tf_tests layers <fixture_dir> <c16 folder> <c24 folder> <out.jsonl>     the decoded layer and top-K on heads from files
 *   tf_tests folder <folder> <key_hex|->        open a folder, print the info JSON or the error's code
 *   tf_tests ogg | codec | sharing | upload | session_share | client | download   (tf_client_tests.inc, tf_share_tests.inc)
 *   tf_tests bitrate | consent | telemetry | sdk_latest                           (tf_201_tests.inc)
 *   tf_tests manifest <data dir> <work dir>     the real signed V4 manifests (tf_manifest_tests.inc)
 *   tf_tests takes <vectors dir> <work dir>     takes in a row share one recording (tf_203_tests.inc) */
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
#include "zf/stream.h"
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

/* ------------------------------------------------------------------ conformance */
/* Every output of every chunk through the public Session (tf_session_create, tf_session_push, on_chunk,
 * tf_session_finish, tf_session_heads), fingerprinted for the engine's bit-exact checker; a new Session per
 * clip, as an app runs take after take. */
static long long g_conf_values, g_conf_chunks; /* a conformance run's totals: values fingerprinted, chunks */
typedef struct {
  uint64_t *fnv; /* per chunk x 12 */
  int chunks, cap;
  uint64_t logp;
  unsigned mask;
  int bad_mask;
  /* what every chunk must satisfy (SDK 3.0): the decoded layer covers the take so far, settled never goes down,
   * only the last chunk is final */
  int frames, settled, finals, bad_decoded, bad_backend, last_final;
} ConfRun;

static void on_chunk(void *user, const TfChunk *c) {
  ConfRun *r = (ConfRun *)user;
  if (r->chunks == r->cap) {
    r->cap = r->cap ? 2 * r->cap : 64;
    r->fnv = (uint64_t *)realloc(r->fnv, (size_t)r->cap * 12 * sizeof(uint64_t));
  }
  for (int k = 0; k < 12; ++k) {
    const int h = tf_head_from_name(kOut[k]);
    TfHead hd;
    const int have = tf_heads_get(c->heads, kOut[k], &hd) == 0;
    const int want = (r->mask >> h) & 1;
    if (have != want) r->bad_mask++;
    const size_t n = have ? (size_t)hd.frames * hd.classes : 0;
    g_conf_values += (long long)n;
    g_conf_chunks += k == 0;
    r->fnv[(size_t)r->chunks * 12 + k] = have ? fnv1a(FNV0, hd.values, n * sizeof(float)) : 0;
    if (h == TF_HEAD_LOG_PROBS && have) {
      r->logp = fnv1a(r->logp, hd.values, n * sizeof(float));
      r->frames += hd.frames;
    }
  }
  tf__clear_error(); /* a head that is not held is asked for on purpose above */
  const TfDecoded *d = c->decoded;
  if (!d || d->n_frames != r->frames || c->first_frame + (c->end_ms - c->start_ms) / 40 != r->frames || c->index != r->chunks ||
      d->settled < r->settled || d->settled > d->n_tokens || (d->is_final && d->settled != d->n_tokens))
    r->bad_decoded++;
  if (d) r->settled = d->settled;
  if (d && d->is_final) r->finals++;
  r->last_final = d && d->is_final;
  if (!c->backend.kind || strcmp(c->backend.kind, "cpu") || strcmp(c->backend.name, "cpu") || strcmp(c->backend.stage, "cpu") || c->backend.reason)
    r->bad_backend++;
  r->chunks++;
}

static TfModel *open_folder(const char *dir, const char *key, int no_engine, const char *ctx) {
  TfModelOpen o;
  memset(&o, 0, sizeof o);
  o.trusted_key_hex = strcmp(key, "-") ? key : NULL;
  o.allow_unsigned = strcmp(key, "-") == 0;
  o.no_engine = no_engine;
  o.context = ctx;
  TfModel *m = tf__model_open(dir, &o);
  if (!m) fprintf(stderr, "open %s: %s (%s)\n", dir, tf_last_error(), tf_error_name(tf_last_error_code()));
  return m;
}

/* the head names of a mask, separated by commas (TfSessionOptions.heads) */
static void heads_list(unsigned mask, char *out, size_t n) {
  out[0] = 0;
  for (int h = 0; h < TF_HEAD_COUNT; ++h)
    if (mask & TF_HEAD_BIT(h)) snprintf(out + strlen(out), n - strlen(out), "%s%s", out[0] ? "," : "", tf_head_info(h)->name);
}

static int conformance(const char *dir, const char *key, const char *list, const char *outp, unsigned mask, const char *ctx) {
  TfModel *m = open_folder(dir, key, 0, ctx);
  if (!m) return 2;
  ConfRun r;
  memset(&r, 0, sizeof r);
  r.mask = mask ? mask : TF_HEADS_DEFAULT;
  char names[512];
  heads_list(r.mask, names, sizeof names);
  TfSessionOptions so;
  memset(&so, 0, sizeof so);
  so.heads = mask ? (mask == TF_HEADS_ALL ? "all" : names) : NULL;
  so.context = ctx;
  so.on_chunk = on_chunk;
  so.user = &r;
  FILE *lf = fopen(list, "r"), *out = fopen(outp, "w");
  if (!lf || !out) return 2;
  char line[4096];
  int clips = 0, greedy_ok = 0, top2_ok = 0, topk_ok = 0, topk_checks = 0, joined_ok = 0;
  double audio_s = 0, wall = 0;
  while (fgets(line, sizeof line, lf)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
    if (!len) continue;
    char *pcm = NULL;
    size_t bytes = 0;
    if (tf_read_file(line, &pcm, &bytes)) {
      fprintf(stderr, "cannot read %s\n", line);
      return 2;
    }
    const int n = (int)(bytes / 4);
    TfSession *s = tf_session_create(m, &so);
    if (!s) {
      fprintf(stderr, "session: %s\n", tf_last_error());
      return 2;
    }
    if (clips == 0) {
      /* one open Session per Model */
      CHECK(tf_session_create(m, &so) == NULL && tf_last_error_code() == TF_ERR_SESSION_OPEN, "a second open session was allowed");
      CHECK(tf_session_heads(s) == NULL, "the take's Heads were given before the take ended");
    }
    r.chunks = r.frames = r.settled = r.finals = 0;
    r.logp = FNV0;
    const double t0 = tf__now_s();
    for (int at = 0; at < n; at += 320)
      if (tf_session_push(s, (const float *)pcm + at, n - at < 320 ? n - at : 320) < 0) {
        fprintf(stderr, "push: %s\n", tf_last_error());
        return 3;
      }
    if (tf_session_finish(s) < 0) {
      fprintf(stderr, "finish: %s\n", tf_last_error());
      return 3;
    }
    wall += tf__now_s() - t0;
    audio_s += n / 16000.0;
    free(pcm);
    CHECK(r.finals == 1 && r.last_final, "%s: %d chunks carried is_final (the last one: %d), want exactly the last", line, r.finals, r.last_final);
    CHECK(tf_session_push(s, (const float *)&wall, 0) == TF_ERR_SESSION_CLOSED, "%s: a push after finish was taken", line);
    const TfHeads *h = tf_session_heads(s);
    if (!h) {
      fprintf(stderr, "heads: %s\n", tf_last_error());
      return 3;
    }
    /* tokens = the decoded layer's token ids; they must equal the engine's greedy and tf_heads_greedy */
    const TfDecoded *d = tf_heads_decoded(h);
    const TfToken *tk = d->tokens, *gk = NULL;
    const int nt = d->n_tokens;
    int g[4096];
    const int ng = zf_tajweed_stream_greedy(tf__session_engine(s), g, 4096);
    const int nh = tf_heads_greedy(h, "log_probs", &gk);
    int same = ng == nt && nh == nt;
    for (int i = 0; same && i < nt; ++i) same = g[i] == tk[i].id && gk[i].id == tk[i].id && gk[i].first_ms == tk[i].first_ms && gk[i].last_ms == tk[i].last_ms;
    greedy_ok += same;
    CHECK(same, "%s: decoded tokens (%d) differ from the engine's greedy (%d) or tf_heads_greedy (%d)", line, nt, ng, nh);
    CHECK(d->is_final && d->settled == nt, "%s: the take's decoded layer is not final with every token settled", line);
    TfHead lp;
    tf_heads_get(h, "log_probs", &lp);
    /* the take's heads are the chunks joined */
    {
      const int ok = lp.frames == r.frames && fnv1a(FNV0, lp.values, (size_t)lp.frames * lp.classes * sizeof(float)) == r.logp;
      joined_ok += ok;
      CHECK(ok, "%s: the take's log_probs are not its chunks joined", line);
    }
    /* the decoded layer's top-2 comes from the engine (zf_tajweed_stream_top2): it must equal a plain C scan
     * of log_probs (lower id on ties), and its margin the double difference of the two float log-probs */
    {
      const TfFrame *fr = d->frames;
      const int nf = d->n_frames;
      int bad = nf != lp.frames;
      for (int t = 0; !bad && t < nf; ++t) {
        const float *row = lp.values + (size_t)t * ZF_TAJWEED_VOCAB;
        int a1 = 0;
        for (int i = 1; i < ZF_TAJWEED_VOCAB; ++i)
          if (row[i] > row[a1]) a1 = i;
        int b1 = a1 == 0 ? 1 : 0;
        for (int i = 0; i < ZF_TAJWEED_VOCAB; ++i)
          if (i != a1 && row[i] > row[b1]) b1 = i;
        bad = fr[t].top1 != a1 || fr[t].runner_up != b1 || fr[t].top1_log_prob != row[a1] || fr[t].runner_up_log_prob != row[b1] ||
              fr[t].margin != (double)row[a1] - (double)row[b1];
      }
      top2_ok += !bad;
      CHECK(!bad, "%s: the engine's top-2 differs from the C scan of log_probs", line);
    }
    /* top-K from the engine's stream (zf_tajweed_stream_topk) == a C stable argsort of the kept heads, for every
     * ranked head at k 2 and k = cols, on the first 5 clips of an all-heads run */
    if (r.mask == TF_HEADS_ALL && clips < 5) {
      for (int hd = 0; hd < TF_HEAD_COUNT; ++hd) {
        const TfHeadInfo *hi = tf_head_info(hd);
        if (!strcmp(hi->kind, "logit")) continue;
        TfHead one;
        tf_heads_get(h, hi->name, &one);
        const float *v = one.values;
        const int frames = one.frames, cols = hi->width;
        for (int kk = 0; kk < 2; ++kk) {
          const int k = kk == 0 ? 2 : cols;
          TfTopK *tkk = tf__session_topk(s, hd, k);
          int bad = !tkk || !v || tkk->frames != frames || tkk->k != k;
          int order[40];
          for (int f = 0; !bad && f < frames; ++f) {
            const float *row = v + (size_t)f * cols;
            for (int i = 0; i < cols; ++i) order[i] = i;
            for (int i = 1; i < cols; ++i) { /* insertion sort: stable, highest first */
              const int x = order[i];
              int j = i - 1;
              while (j >= 0 && row[order[j]] < row[x]) {
                order[j + 1] = order[j];
                --j;
              }
              order[j + 1] = x;
            }
            for (int j = 0; !bad && j < k; ++j) {
              const size_t i = (size_t)f * k + j;
              const double mg = j + 1 < cols ? (double)row[order[j]] - (double)row[order[j + 1]] : 0.0;
              bad = tkk->ids[i] != order[j] || tkk->values[i] != row[order[j]] || tkk->margins[i] != mg;
            }
          }
          topk_checks++;
          topk_ok += !bad;
          CHECK(!bad, "%s: stream top-K of %s at k %d differs from a C argsort", line, hi->name, k);
          tf__topk_free(tkk);
        }
      }
    }
    const char *base = strrchr(line, '/');
    const char *b2 = strrchr(line, '\\');
    if (b2 > base) base = b2;
    base = base ? base + 1 : line;
    char clip[256];
    snprintf(clip, sizeof clip, "%s", base);
    char *dot = strrchr(clip, '.');
    if (dot) *dot = 0;
    fprintf(out, "{\"clip\": \"%s\", \"chunks\": %d, \"tokens\": [", clip, r.chunks);
    for (int i = 0; i < nt; ++i) fprintf(out, i ? ", %d" : "%d", tk[i].id);
    fprintf(out, "], \"logp_fnv\": \"%016llx\", \"out_fnv\": {", (unsigned long long)r.logp);
    for (int k = 0; k < 12; ++k) {
      fprintf(out, "%s\"%s\": [", k ? ", " : "", kOut[k]);
      for (int c = 0; c < r.chunks; ++c) fprintf(out, c ? ", \"%016llx\"" : "\"%016llx\"", (unsigned long long)r.fnv[(size_t)c * 12 + k]);
      fprintf(out, "]");
    }
    fprintf(out, "}}\n");
    tf_session_destroy(s);
    clips++;
  }
  fclose(lf);
  fclose(out);
  CHECK(r.bad_mask == 0, "head selection: %d chunk heads present/absent against the mask", r.bad_mask);
  CHECK(r.bad_decoded == 0, "%d chunks whose decoded layer does not cover the take so far, or whose settled count went down", r.bad_decoded);
  CHECK(r.bad_backend == 0, "%d chunks whose backend is not the CPU engine", r.bad_backend);
  printf("conformance run: %d clips, engine top-2 == C scan on %d, take == chunks joined on %d\n", clips, top2_ok, joined_ok);
  if (topk_checks)
    printf("conformance run: stream top-K == C argsort on %d/%d (each ranked head at k 2 and k = cols, first 5 clips)\n", topk_ok,
           topk_checks);
  printf("conformance run: %d clips, greedy == decoded on %d, head mask 0x%03x %s, RTF %.3f (%.1f s audio)\n", clips, greedy_ok,
         r.mask, r.bad_mask ? "VIOLATED" : "respected", audio_s > 0 ? wall / audio_s : 0, audio_s);
  printf("conformance totals: %d clips, %lld chunks, %lld values, %.1f s, peak memory %lld MB\n", clips, g_conf_chunks, g_conf_values, wall,
         own_peak_memory() >> 20);
  tf_model_close(m);
  free(r.fnv);
  return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ top-K on real takes */
/* Each clip through a Session with top_k = k and the head log_probs, pushed 20 ms at a time: what an app gets. One
 * JSON line per clip for test/check_outputs.py `topk`, which compares it with the contract's
 * vectors/topk_takes.json: the SHA-256 of the take's log_probs (so a different engine output is told apart from
 * a different ranking), then every frame's ranked ids and, as bit patterns so nothing is rounded on the way, the
 * float log-probs (8 hex digits) and the double margins (16 hex digits). */
static int topk_takes(const char *dir, const char *key, const char *list, const char *outp, const char *ctx, int k) {
  TfModel *m = open_folder(dir, key, 0, ctx);
  if (!m) return 2;
  TfSessionOptions so;
  memset(&so, 0, sizeof so);
  so.heads = "log_probs";
  so.context = ctx;
  so.top_k = k;
  FILE *lf = fopen(list, "r"), *out = fopen(outp, "w");
  if (!lf || !out) return 2;
  char line[4096];
  int clips = 0;
  long long frames = 0;
  while (fgets(line, sizeof line, lf)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
    if (!len) continue;
    char *pcm = NULL;
    size_t bytes = 0;
    if (tf_read_file(line, &pcm, &bytes)) {
      fprintf(stderr, "cannot read %s\n", line);
      return 2;
    }
    const int n = (int)(bytes / 4);
    TfSession *s = tf_session_create(m, &so);
    if (!s) {
      fprintf(stderr, "session: %s\n", tf_last_error());
      return 2;
    }
    for (int at = 0; at < n; at += 320)
      if (tf_session_push(s, (const float *)pcm + at, n - at < 320 ? n - at : 320) < 0) {
        fprintf(stderr, "push: %s\n", tf_last_error());
        return 3;
      }
    if (tf_session_finish(s) < 0) {
      fprintf(stderr, "finish: %s\n", tf_last_error());
      return 3;
    }
    free(pcm);
    const TfHeads *h = tf_session_heads(s);
    const TfDecoded *d = h ? tf_heads_decoded(h) : NULL;
    TfHead lp;
    if (!h || !d || tf_heads_get(h, "log_probs", &lp) != 0) {
      fprintf(stderr, "heads: %s\n", tf_last_error());
      return 3;
    }
    CHECK(d->top_k == k && d->n_frames == lp.frames && lp.classes == ZF_TAJWEED_VOCAB && d->ranked_ids && d->ranked_log_probs && d->ranked_margins,
          "%s: the take's decoded layer is not ranked %d deep over its %d frames", line, k, lp.frames);
    if (d->top_k != k || d->n_frames != lp.frames || !d->ranked_ids) return 1;
    uint8_t dg[32];
    char hex[65];
    tf_sha256(lp.values, (size_t)lp.frames * lp.classes * sizeof(float), dg);
    tf_hex_encode(dg, 32, hex);
    const char *base = line + len;
    while (base > line && base[-1] != '/' && base[-1] != '\\') --base;
    const char *dot = strrchr(base, '.');
    fprintf(out, "{\"clip\":\"%.*s\",\"context\":\"%s\",\"k\":%d,\"frames\":%d,\"log_probs_sha256\":\"%s\",\"ids\":[", (int)(dot ? dot - base : (long)strlen(base)),
            base, ctx, k, d->n_frames, hex);
    const size_t total = (size_t)d->n_frames * (size_t)k;
    for (size_t i = 0; i < total; ++i) fprintf(out, "%s%d", i ? "," : "", d->ranked_ids[i]);
    fprintf(out, "],\"log_probs_bits\":[");
    for (size_t i = 0; i < total; ++i) {
      uint32_t u;
      memcpy(&u, &d->ranked_log_probs[i], 4);
      fprintf(out, "%s\"%08x\"", i ? "," : "", (unsigned)u);
    }
    fprintf(out, "],\"margins_bits\":[");
    for (size_t i = 0; i < total; ++i) {
      uint64_t u;
      memcpy(&u, &d->ranked_margins[i], 8);
      fprintf(out, "%s\"%016llx\"", i ? "," : "", (unsigned long long)u);
    }
    fprintf(out, "]}\n");
    frames += d->n_frames;
    tf_session_destroy(s);
    ++clips;
  }
  fclose(lf);
  fclose(out);
  tf_model_close(m);
  printf("topk run: %s, %d deep: %d clips, %lld frames\n", ctx, k, clips, frames);
  return fails || !clips ? 1 : 0;
}

/* ------------------------------------------------------------------ the decoded layer and top-K on heads from files */
static const char *const kOrder[12] = {"log_probs", "inter1_log_probs", "inter4_log_probs", "id_log_probs", "timing_log_probs",
                                       "gem", "attr_tafkheem", "attr_ghunna", "attr_qalqalah", "attr_face", "boundary", "conf"};

/* the fixture's head files through tf__decode and tf__topk, written for test/check_outputs.py to compare with the
 * shared layers vectors (the numbers the other SDKs' decoded layers are held to) */
static int layers(const char *fx, const char *dir16, const char *dir24, const char *outp) {
  char *path = tf_path_join(fx, "vectors.json"), *text = NULL;
  size_t n = 0;
  if (tf_read_file(path, &text, &n)) return 2;
  free(path);
  TfJson *v = tf_json_parse(text, n, NULL);
  free(text);
  if (!v) return 2;
  TfModel *m16 = open_folder(dir16, "-", 1, NULL), *m24 = open_folder(dir24, "-", 1, NULL);
  if (!m16 || !m24) return 2;
  const TfJson *vec = tf_json_get(v, "vectors");
  FILE *out = fopen(outp, "w");
  int done = 0;
  for (int i = 0; i < vec->n; ++i) {
    const TfJson *x = &vec->kids[i];
    const char *ctx = tf_json_str(tf_json_get(x, "context"));
    TfModel *m = !strcmp(ctx, "c16_l64") ? m16 : m24;
    const int n40 = (int)tf_json_get(x, "n40")->num, n20 = (int)tf_json_get(x, "n20")->num;
    char *hp = tf_path_join(fx, tf_json_str(tf_json_get(x, "heads"))), *raw = NULL;
    size_t bytes;
    if (tf_read_file(hp, &raw, &bytes)) return 2;
    free(hp);
    ZfTajweedHeads h;
    memset(&h, 0, sizeof h);
    h.n40 = n40;
    h.n20 = n20;
    h.vocab = 38;
    const float *at = (const float *)raw;
    for (int k = 0; k < 12; ++k) {
      const int hd = tf_head_from_name(kOrder[k]);
      const TfHeadInfo *hi = tf_head_info(hd);
      const float **slot = NULL;
      switch (hd) {
        case TF_HEAD_LOG_PROBS: slot = &h.log_probs; break;
        case TF_HEAD_INTER1: slot = &h.inter1_log_probs; break;
        case TF_HEAD_INTER4: slot = &h.inter4_log_probs; break;
        case TF_HEAD_ID: slot = &h.id_log_probs; break;
        case TF_HEAD_TIMING: slot = &h.timing_log_probs; break;
        case TF_HEAD_GEM: slot = &h.gem; break;
        case TF_HEAD_TAFKHEEM: slot = &h.attr_tafkheem; break;
        case TF_HEAD_GHUNNA: slot = &h.attr_ghunna; break;
        case TF_HEAD_QALQALAH: slot = &h.attr_qalqalah; break;
        case TF_HEAD_FACE: slot = &h.attr_face; break;
        case TF_HEAD_BOUNDARY: slot = &h.boundary; break;
        case TF_HEAD_CONF: slot = &h.conf; break;
      }
      *slot = at;
      at += (size_t)(hi->rate_ms == 40 ? n40 : n20) * hi->width;
    }
    CHECK((size_t)((const char *)at - raw) == bytes, "%s: heads file size", tf_json_str(tf_json_get(x, "name")));
    /* the same heads through the public Heads object: its decoded layer is the one tf__decode gives */
    TfHeads view;
    tf__heads_view(&view, &h, TF_HEADS_ALL, m);
    view.top_k = 2;
    const TfDecoded *pd = tf_heads_decoded(&view);
    TfDecoded *d = tf__decode(m, &h, NULL, 2);
    CHECK(d && pd && pd->n_tokens == d->n_tokens && !strcmp(pd->text, d->text) && pd->n_frames == n40, "%s: tf_heads_decoded differs from tf__decode",
          tf_json_str(tf_json_get(x, "name")));
    fprintf(out, "{\"name\":\"%s\",\"context\":\"%s\",\"decoded\":%s,", tf_json_str(tf_json_get(x, "name")), ctx, tf_decoded_json(d, 1));
    tf__decoded_free(d);
    tf__heads_release(&view);
    /* top-K through tf__topk (layers_ref.py "topk": k 3 for the token heads, the full ranking of the class heads,
     * and log_probs_full8 = all 38 on the first 8 frames) */
    fprintf(out, "\"topk\":{");
    for (int hd = 0, first = 1; hd < TF_HEAD_COUNT; ++hd) {
      const TfHeadInfo *hi = tf_head_info(hd);
      if (!strcmp(hi->kind, "logit")) continue;
      TfTopK *tk = tf__topk(&h, hd, hi->n_classes ? hi->width : 3);
      CHECK(tk != NULL, "tf__topk %s: %s", hi->name, tf_last_error());
      if (tk) fprintf(out, "%s\"%s\":%s", first ? "" : ",", hi->name, tf__topk_json(tk));
      first = 0;
      tf__topk_free(tk);
    }
    {
      ZfTajweedHeads h8 = h;
      h8.n40 = n40 < 8 ? n40 : 8;
      h8.n20 = 2 * h8.n40;
      TfTopK *tk = tf__topk(&h8, TF_HEAD_LOG_PROBS, 38);
      CHECK(tk != NULL, "tf__topk full: %s", tf_last_error());
      if (tk) fprintf(out, ",\"log_probs_full8\":%s", tf__topk_json(tk));
      tf__topk_free(tk);
    }
    fprintf(out, "}}\n");
    free(raw);
    done++;
  }
  fclose(out);
  tf_model_close(m16);
  tf_model_close(m24);
  tf_json_free(v);
  printf("layers run: %d takes decoded\n", done);
  return fails ? 1 : 0;
}

#include "tf_client_tests.inc"
#include "tf_share_tests.inc"
#include "tf_201_tests.inc"
#include "tf_203_tests.inc"
#include "tf_301_tests.inc"
#include "tf_manifest_tests.inc"
#include "tf_30_tests.inc"
#include "tf_30_api_tests.inc"
#include "tf_30_model_tests.inc"
#include "tf_30_share_tests.inc"
#include "tf_313_net_tests.inc"

static int folder(const char *dir, const char *key) {
  TfModel *m = open_folder(dir, key, 1, NULL);
  if (!m) {
    printf("%s\n", tf_error_name(tf_last_error_code()));
    return 1;
  }
  printf("%s\n", tf_model_info_json(m));
  tf_model_close(m);
  return 0;
}

/* ------------------------------------------------------------------ pinning (engine de88490) */
/* Streams made and destroyed one after another on ONE thread with pinning ON (no_pin 0 explicitly) keep their
 * full engine team (zf_stream_threads_active == min(threads, the process's cores) every time) and leave the
 * calling thread's affinity mask as they found it. The bug this guards: a pinned caller's mask was never
 * restored, so every later stream got a one-core team. Checked at the engine's stream level (the team) and
 * through tf_stream (the mask, and the same tokens every take). */
#if defined(_WIN32)
typedef DWORD_PTR PinMask;
static PinMask pin_thread_mask(void) {
  DWORD_PTR proc = 0, sys = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
  const DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), proc);
  if (old) SetThreadAffinityMask(GetCurrentThread(), old);
  return old;
}
static int pin_process_cores(void) {
  DWORD_PTR proc = 0, sys = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
  int n = 0;
  for (; proc; proc >>= 1) n += (int)(proc & 1);
  return n;
}
#elif defined(__linux__)
typedef cpu_set_t PinMask;
static PinMask pin_thread_mask(void) {
  PinMask m;
  CPU_ZERO(&m);
  sched_getaffinity(0, sizeof m, &m);
  return m;
}
static int pin_process_cores(void) {
  PinMask m;
  CPU_ZERO(&m);
  sched_getaffinity(getpid(), sizeof m, &m);
  return CPU_COUNT(&m);
}
#else
typedef int PinMask;
static PinMask pin_thread_mask(void) { return 0; }
static int pin_process_cores(void) { return 1; }
#endif

static int pin_tests(const char *dir, const char *key, const char *list, int threads, int rounds) {
  /* the first clip of the list */
  FILE *lf = fopen(list, "r");
  char line[4096] = "";
  if (!lf || !fgets(line, sizeof line, lf)) return 2;
  fclose(lf);
  size_t len = strlen(line);
  while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
  char *pcm = NULL;
  size_t bytes = 0;
  if (tf_read_file(line, &pcm, &bytes)) return 2;
  const int n = (int)(bytes / 4);
  const float *x = (const float *)pcm;
  const int cores = pin_process_cores();
  const int expect = threads < cores ? threads : cores;
  const PinMask original = pin_thread_mask();
  int bad = 0;

  /* 1. the engine's stream (zf_stream), pinned, `rounds` in a row, then two alive at once */
  char *model = tf_path_join(dir, "c16_l64/model_engine2.onnx");
  ZfToken toks[512];
  ZfStream *both[2] = {NULL, NULL};
  for (int r = 0; r < rounds + 2; ++r) {
    ZfStreamOptions o = zf_stream_default_options();
    o.threads = threads;
    o.no_pin = 0; /* pinned, whatever the platform's default */
    const char *err = NULL;
    ZfStream *s = zf_stream_create(model, &o, &err);
    if (!s) {
      fprintf(stderr, "zf_stream_create: %s\n", err ? err : "?");
      return 3;
    }
    for (int at = 0; at < n; at += 320) zf_stream_push_audio(s, x + at, n - at < 320 ? n - at : 320, toks, 512);
    zf_stream_finish_audio(s, toks, 512);
    const int team = zf_stream_threads_active(s);
    if (r < rounds) {
      zf_stream_destroy(s);
      const PinMask after = pin_thread_mask();
      const int restored = memcmp(&after, &original, sizeof after) == 0;
      const int ok = team == expect && restored;
      bad += !ok;
      printf("engine stream %d (pinned, %d threads): team %d of %d expected, caller's mask %s: %s\n", r, threads, team, expect,
             restored ? "restored" : "NOT restored", ok ? "ok" : "FAIL");
    } else {
      both[r - rounds] = s;
      bad += team != expect;
      printf("engine stream %d alive with %d other: team %d of %d expected: %s\n", r, r - rounds, team, expect,
             team == expect ? "ok" : "FAIL");
    }
  }
  zf_stream_destroy(both[1]);
  zf_stream_destroy(both[0]);
  {
    const PinMask after = pin_thread_mask();
    const int restored = memcmp(&after, &original, sizeof after) == 0;
    bad += !restored;
    printf("two engine streams at once, destroyed: caller's mask %s\n", restored ? "restored" : "NOT restored");
  }
  free(model);

  /* 2. through the SDK: a model opened pinned and with `threads` threads; Sessions made, used and destroyed in a
   * row on this thread give the same tokens and leave the mask as it was */
  TfModelOpen mo;
  memset(&mo, 0, sizeof mo);
  mo.trusted_key_hex = strcmp(key, "-") ? key : NULL;
  mo.allow_unsigned = strcmp(key, "-") == 0;
  mo.threads = threads;
  mo.pin = 1;
  TfModel *m = tf__model_open(dir, &mo);
  if (!m) {
    fprintf(stderr, "open: %s\n", tf_last_error());
    return 3;
  }
  int first[1024], nfirst = -1;
  for (int r = 0; r < rounds; ++r) {
    TfSession *s = tf_session_create(m, NULL);
    if (!s) return 3;
    const double t0 = tf__now_s();
    for (int at = 0; at < n; at += 320) tf_session_push(s, x + at, n - at < 320 ? n - at : 320);
    tf_session_finish(s);
    const double dt = tf__now_s() - t0;
    int ids[1024];
    const TfToken *tk = NULL;
    const int ni = tf_heads_greedy(tf_session_heads(s), NULL, &tk);
    for (int i = 0; i < ni && i < 1024; ++i) ids[i] = tk[i].id;
    tf_session_destroy(s);
    const PinMask after = pin_thread_mask();
    const int restored = memcmp(&after, &original, sizeof after) == 0;
    int same = 1;
    if (nfirst < 0) {
      nfirst = ni;
      memcpy(first, ids, sizeof(int) * (size_t)(ni < 1024 ? ni : 1024));
    } else {
      same = ni == nfirst && !memcmp(first, ids, sizeof(int) * (size_t)(ni < 1024 ? ni : 1024));
    }
    const int ok = restored && same;
    bad += !ok;
    printf("session %d (pinned, %d threads): %.0f ms, %d tokens%s, caller's mask %s: %s\n", r, threads, dt * 1e3, ni,
           same ? "" : " (DIFFERENT)", restored ? "restored" : "NOT restored", ok ? "ok" : "FAIL");
  }
  tf_model_close(m);
  free(pcm);
  printf("pinning: %s\n", bad ? "FAIL" : "all ok");
  return bad ? 1 : 0;
}

int main(int argc, char **argv) {
  cap_own_memory();
  tf__client_no_timer = 1; /* the replayed exchanges and stubs run on the test's thread; the telemetry test starts the timer itself */
  if (argc >= 2 && !strcmp(argv[1], "crypto")) return crypto_tests();
  if (argc >= 3 && !strcmp(argv[1], "errors")) return errors_tests(argv[2]);
  if (argc >= 4 && !strcmp(argv[1], "vectors30")) return vectors30_tests(argv[2], argv[3]);
  if (argc >= 4 && !strcmp(argv[1], "api")) return api_tests(argv[2], argv[3]);
  if (argc >= 4 && !strcmp(argv[1], "net")) return net_tests(argv[2], argv[3]);
  if (argc >= 4 && !strcmp(argv[1], "share30")) return share30_tests(argv[2], argv[3]);
  if (argc >= 5 && !strcmp(argv[1], "model30")) return model30_tests(argv[2], argv[3], argv[4], argc > 5 ? argv[5] : NULL, argc > 6 ? argv[6] : NULL);
  if (argc >= 5 && !strcmp(argv[1], "pin"))
    return pin_tests(argv[2], argv[3], argv[4], argc > 5 ? atoi(argv[5]) : 4, argc > 6 ? atoi(argv[6]) : 6);
  if (argc >= 6 && !strcmp(argv[1], "conformance"))
    return conformance(argv[2], argv[3], argv[4], argv[5], argc > 6 ? (unsigned)strtoul(argv[6], NULL, 0) : 0,
                       argc > 7 ? argv[7] : NULL);
  if (argc >= 8 && !strcmp(argv[1], "topk")) return topk_takes(argv[2], argv[3], argv[4], argv[5], argv[6], atoi(argv[7]));
  if (argc >= 6 && !strcmp(argv[1], "layers")) return layers(argv[2], argv[3], argv[4], argv[5]);
  if (argc >= 4 && !strcmp(argv[1], "folder")) return folder(argv[2], argv[3]);
  if (argc >= 5 && !strcmp(argv[1], "download")) return download_tests(argv[2], argv[3], argv[4]);
  if (argc >= 5 && !strcmp(argv[1], "sharing")) return sharing_tests(argv[2], argv[3], argv[4]);
  if (argc >= 4 && !strcmp(argv[1], "ogg")) return ogg_tests(argv[2], argv[3]);
  if (argc >= 4 && !strcmp(argv[1], "codec")) return codec_tests(argv[2], argv[3]);
  if (argc >= 3 && !strcmp(argv[1], "upload")) return upload_tests(argv[2]);
  if (argc >= 6 && !strcmp(argv[1], "session_share")) return session_share_tests(argv[2], argv[3], argv[4], argv[5]);
  if (argc >= 4 && !strcmp(argv[1], "client")) return client_tests(argv[2], argv[3]);
  if (argc >= 4 && !strcmp(argv[1], "bitrate")) return bitrate_tests(argv[2], argv[3]);
  if (argc >= 5 && !strcmp(argv[1], "consent")) return consent_tests(argv[2], argv[3], argv[4]);
  if (argc >= 4 && !strcmp(argv[1], "sdk_latest")) return sdk_latest_tests(argv[2], argv[3]);
  if (argc >= 4 && !strcmp(argv[1], "manifest")) return manifest_tests(argv[2], argv[3]);
  if (argc >= 4 && !strcmp(argv[1], "takes")) return takes_tests(argv[2], argv[3]);
  if (argc >= 3 && !strcmp(argv[1], "sharing_off")) return sharing_off_tests(argv[2], argc > 3 ? atoi(argv[3]) : 20, argc > 4 ? atoi(argv[4]) : 1);
  if (argc >= 3 && !strcmp(argv[1], "telemetry")) return telemetry_tests(argv[2]);
  fprintf(stderr, "usage: see the comment at the top of test/tf_tests.c\n");
  return 2;
}
