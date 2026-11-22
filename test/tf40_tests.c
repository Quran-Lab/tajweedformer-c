/* tf40_tests: SDK 4.0's own tests, through this library and the closed Core (a prebuilt qlcore.dll / qlcore.so built
 * as the contract's TEST release 4.0.0-c1: its keys come from the contract's test roots, no server trusts them).
 *
 *   tf40_tests ikey      <vectors> <work>   the install key store: TPM or software KSP (Windows), the key file (Linux):
 *                                           agreement, signing, not exportable, a new agreement key per attest
 *   tf40_tests lease     <vectors> <work>   lease.json's ECDH leases into the Core through this SDK's key store and
 *                                           clocks (the outer wrap opens only with the right ECDH bytes): accepted,
 *                                           refused, re-anchored (accept_newer)
 *   tf40_tests clock     <vectors> <work>   clock.json's rule through this SDK's wall clock, boot clock and boot id
 *   tf40_tests container <vectors> <work>   manifest_40.json's containers installed by the Core through this SDK's
 *                                           storage: device forms, no plaintext on disk, a changed byte refused
 *   tf40_tests attest    <vectors> <work>   install_cert.json's attest_proof cases, and qlc_attest_sig with this SDK's
 *                                           key store against the same rule
 *   tf40_tests flow      <fixture> <work>   the whole flow against a stand-in server (make_flow40.py): config,
 *                                           attest with install_sig, challenge, the lease request's closed schema and
 *                                           signature, download, install, signed and unsigned stops, retries; the
 *                                           4.0 config's key id and the files from the lease answer's base; the data
 *                                           piece (piece.json) and a proof refused once under a new piece
 *   tf40_tests grace     <fixture> <vectors> <work>   18.4.7.1: grace.json's 37 attempt outcomes through the SDK and
 *                                           the Core; the grace on the Core's clock, 503 lease_unavailable and its
 *                                           Retry-After, a refusal ending it, a 7-day lease of a desktop install
 *   tf40_tests grace_vectors <vectors> <grace40> <work>   grace.json's override_leases and 15 sequences through this
 *                                           SDK's key store, clocks and saved state and the Core (make_grace40.py)
 *   tf40_tests lease_clock <fixture> <work>   the staging clock case of 11-15: on one boot moved past exp, a lease
 *                                           from a server whose clock stayed behind is refused as expired, one from
 *                                           a server whose clock moved with the device's re-anchors and opens
 *   tf40_tests recited   -        <work>    the recited range's precedence and the wire form (17.1)
 *   tf40_tests outcome   -        <work>    17.9's arguments and counts
 *   tf40_tests learner   -        <work>    section D's automatic groups (17.8, 17.9): learner, learner_gap and streak
 *                                           once per active day, take_s, session_s (a killed session closed at the
 *                                           next start), hour_speech_s, juz_ayahs, repeat_ayahs, first and repeat,
 *                                           judged_tenure, the weekly trend, tenure boundaries
 *   tf40_tests switches  <fixture> <work>   a signed config with every group off (flags.metrics): none is sent,
 *                                           and with none off: every one is
 *   tf40_tests json_leak -        <work>    the JSON parser frees a half-parsed child (quranlab-core sdk-patches/0001)
 *   tf40_tests flac      <vectors> <work>   the lossless form (SHARING_40.md 19): flac16k byte for byte, every
 *                                           flac_cases.json case, pcm16.json, the start bodies of upload_start.json
 *   tf40_tests share_ask <vectors> <work>   share_ask.json (SHARE_ASK_40.md 20): every config case and every step of
 *                                           every case through the port of decide
 *   tf40_tests share_moments <fixture> <work>   the ask's moments through the SDK on a fake clock: the settle, the
 *                                           re-ask sheet in a card's place, the card, its open, a take that ends it
 *   tf40_tests share_flac <fixture> <work>   a shared recording through the recorder: FLAC at 16 kHz with
 *                                           flags.share_flac (the encoder's very bytes), Opus at 48 kHz without
 *   tf40_tests piece_order <fixture> <vectors> <work>   piece_order.json's four sequences (spec fix 16) through this
 *                                           SDK's Core host with real model opens, on the encrypted test build
 *   tf40_tests conformance <fixture> <clips.txt> <out.jsonl>   the 35 clips through the 4.0 C SDK on an encrypted
 *                                           test build (make_flow40.py --testbuild): every output of every chunk,
 *                                           for the engine's v4_conformance_check.py
 *   tf40_tests eval_units <test/data/eval> <work>   tajweedformer-eval's own parts, no model (tf40_eval_tests.inc)
 *   tf40_tests eval <fixture> <clips.txt> <work>    tajweedformer-eval on five of the 35 clips with references, on the
 *                                           encrypted test build, then offline; no plaintext of a model file on
 *                                           disk after both (tf40_eval_tests.inc)
 *   tf40_tests errors <test/data/sdk_4.0.0> <work>  error reports (ERRORS_40.md 24): the contract's errors_40.json
 *                                           codes, queue, telemetry and signatures (tf40_errors_tests.inc)
 *   tf40_tests errors_flow <fixture> <work> [<test/data/sdk_4.0.0>]   each reported stage from its failure against the
 *                                           stand-in server: the code the app gets and the report it sends; with the
 *                                           vectors, errors_40.json core_open from a failed model open (24.3, Gate C)
 *                                           (tf40_errors_tests.inc)
 *   tf40_tests app27 <fixture> <work>       4.0.1, APP_VERSION_40.md 27.3: app_version and app_build on the config
 *                                           request, register's app_version, tf_client_create2 and its ext's size
 *   tf40_tests endpoints <vectors> <fixture> <work>   4.0.1, ENDPOINTS_40.md 28.8: endpoints_40.json replayed on this SDK's code
 *                                           (hosts, config_lists, bases, answers, requests, attempts, first_manifest,
 *                                           downloads with downloads_if_range), and standby_40.json's answers read by
 *                                           28.3's table (tf40_endpoints_tests.inc)
 *   tf40_tests loopback <fixture> <work>    4.0.1, 28.8's checks on this platform's real HTTP code (WinHTTP, libcurl)
 *                                           against stand-in servers on 127.0.0.1 (tf40_endpoints_tests.inc)
 *   tf40_tests requests <fixture> <work>    4.0.2, SPEC 16.7: one request per model file, a resume from the cut, a
 *                                           partial file completed (tf40_requests_tests.inc)
 *   tf40_tests hosts <nowait40> <test/data/sdk_4.0.0> <work>   4.0.2, STEERING_40.md 34.6: the per-host counts
 *                                           (tf40_requests_tests.inc)
 *   tf40_tests load <fixture> <work>        4.0.2 under load: the lease's retries (PROTECTED_MODEL.md 18.4.7.3) and
 *                                           a 429 (ENDPOINTS_40.md 28.10) on fixed clocks (tf40_load_tests.inc)
 *   tf40_tests no_wait <flow40> <nowait40> <test/data/sdk_4.0.0> <work>   4.0.2, NO_WAIT_40.md (section 33): the
 *                                           no_wait_40.json cases that name c against the stand-in, the platform's
 *                                           connection report set to each case's (tf40_nowait_tests.inc)
 *   tf40_tests lifetime <nowait40> <work>   4.0.2: a call after an object's end, one Core per storage folder, the
 *                                           1000-round destroy stress (tf40_lifetime_tests.inc)
 *   tf40_tests lifetime_open <fixture> <work>   4.0.3, on the encrypted test build: the Core's one open model is the
 *                                           folder's; a Client made after another's destroy opens beside its kept
 *                                           Model (tf40_lifetime_tests.inc)
 * <vectors>: the contract's sdk_4.0.0/vectors. Nothing here reaches a network: every request goes to the stand-in. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tajweedformer.h"
#include "tf_flac.h"
#include "tf_internal.h"
#include "tf_test_net.h"
#include "tf_thread.h"
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

int tf_p256_verify(const uint8_t pub[65], const void *msg, size_t n, const uint8_t sig[64]);

static int fails, checks;
#define CHECK(c, ...)                                      \
  do {                                                     \
    checks++;                                              \
    if (!(c)) {                                            \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                        \
      fprintf(stderr, "\n");                               \
      fails++;                                             \
    }                                                      \
  } while (0)

/* ------------------------------------------------------------------ helpers */
static TfJson *load_json(const char *dir, const char *name) {
  char *p = tf_path_join(dir, name), *t = NULL;
  size_t n = 0;
  TfJson *j = p && !tf_read_file(p, &t, &n) ? tf_json_parse(t, n, NULL) : NULL;
  if (!j) fprintf(stderr, "cannot read %s\n", p ? p : name);
  free(p);
  free(t);
  return j;
}
static const char *S(const TfJson *o, const char *k) {
  const char *s = tf_json_str(tf_json_get(o, k));
  return s ? s : "";
}
static long long N(const TfJson *o, const char *k) {
  const TfJson *v = tf_json_get(o, k);
  return v && v->type == TJ_NUM ? (long long)v->num : 0;
}
static size_t unhex(const char *h, uint8_t **out) {
  const size_t n = strlen(h) / 2;
  *out = (uint8_t *)malloc(n + 1);
  if (n && tf_hex_decode(h, *out, n)) fprintf(stderr, "bad hex\n");
  return n;
}
static size_t unb64(const char *s, uint8_t **out) {
  *out = (uint8_t *)malloc(strlen(s) + 4);
  const int n = tf_base64_decode(s, *out, strlen(s) + 4);
  return n > 0 ? (size_t)n : 0;
}
/* the contract's stream(label, n): SHA-256(label || u32 LE counter), concatenated */
static void stream_bytes(const char *label, uint8_t *out, size_t n) {
  for (uint32_t i = 0; n; ++i) {
    TfSha256 c;
    uint8_t ctr[4] = {(uint8_t)i, (uint8_t)(i >> 8), (uint8_t)(i >> 16), (uint8_t)(i >> 24)}, h[32];
    tf_sha256_init(&c);
    tf_sha256_update(&c, label, strlen(label));
    tf_sha256_update(&c, ctr, 4);
    tf_sha256_final(&c, h);
    const size_t k = n < 32 ? n : 32;
    memcpy(out, h, k);
    out += k;
    n -= k;
  }
}
/* DER ECDSA (30 L 02 Lr r 02 Ls s) to 64 bytes r || s; 0 or -1 */
static int der_to_raw(const uint8_t *d, size_t n, uint8_t sig[64]) {
  if (n == 64) {
    memcpy(sig, d, 64);
    return 0;
  }
  if (n < 8 || d[0] != 0x30) return -1;
  size_t at = 2;
  memset(sig, 0, 64);
  for (int part = 0; part < 2; ++part) {
    if (at + 2 > n || d[at] != 2) return -1;
    size_t l = d[at + 1];
    at += 2;
    const uint8_t *v = d + at;
    if (at + l > n) return -1;
    while (l > 32 && *v == 0) v++, l--;
    if (l > 32) return -1;
    memcpy(sig + part * 32 + (32 - l), v, l);
    at += (v - (d + at)) + l;
    at = (size_t)((v + l) - d);
  }
  return 0;
}
static void spki_point(const uint8_t *spki, uint8_t pub[65]) { memcpy(pub, spki + 26, 65); }
static void rmrf(const char *p) { tf__remove_tree(p); }
/* 1 when a file under dir holds the needle */
static int tree_holds(const char *dir, const uint8_t *needle, size_t nn, int depth) {
  char (*names)[256] = (char (*)[256])malloc(256 * 256);
  const int n = names ? tf__list_dir(dir, names, 256) : 0;
  int hit = 0;
  for (int i = 0; i < n && !hit; ++i) {
    char *p = tf_path_join(dir, names[i]), *b = NULL;
    size_t bn = 0;
    if (p && !tf_read_file(p, &b, &bn)) {
      for (size_t k = 0; k + nn <= bn && !hit; ++k) hit = !memcmp(b + k, needle, nn);
    } else if (p && depth < 8) {
      hit = tree_holds(p, needle, nn, depth + 1);
    }
    free(b);
    free(p);
  }
  free(names);
  return hit;
}
static long long own_memory(void) {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pc;
  memset(&pc, 0, sizeof pc);
  return K32GetProcessMemoryInfo(GetCurrentProcess(), &pc, sizeof pc) ? (long long)pc.PagefileUsage : 0;
#else
  long long pages = 0, resident = 0;
  FILE *f = fopen("/proc/self/statm", "r");
  if (!f) return 0;
  if (fscanf(f, "%lld %lld", &pages, &resident) != 2) resident = 0;
  fclose(f);
  return resident * (long long)sysconf(_SC_PAGESIZE);
#endif
}
static int done(const char *what) {
  printf("%s: %s (%d checks)\n", what, fails ? "FAIL" : "ok", checks);
  return fails ? 1 : 0;
}

/* a Client for the internal tests: no timer, a storage folder of its own, the test signing key */
static TfClient *test_client(const char *work, const char *key_hex) {
  TfClientConfig o = tf__client_default_config();
  o.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "com.example.recite";
  o.storage_dir = work;
  o.trusted_key_hex = key_hex;
  return tf__client_open(&o);
}

/* The analytics tests' install key: the learner's days are kept in the Core's journal (qlc_days_*, Core request R10),
 * so these tests run with a Core, on this fixed software test key (P-256, scalar SHA-256("quranlab-c-sdk-test-days 1")
 * mod n; never in a key store, never trusted by a server), so no test leaves a key in the TPM or the KSP. */
static const char kDaysKeyHex[] =
    "308187020100301306072a8648ce3d020106082a8648ce3d030107046d306b0201010420d3f67dc818a335b18027ae2546dbe25fe1fd06eb50f45e"
    "700ecf1359f0b7becaa144034200049d8e0982b227a8dcf41904f3db6ec34b03039975b0aef7c144ba19e0cbf932278b17e6538fc6a5106fe740b4"
    "92b100dbdc97376fc632491f8efc559b52d85bb6";
static void days_key(int on) {
  static uint8_t *k;
  static size_t n;
  if (!k) n = unhex(kDaysKeyHex, &k);
  tf__ikey_test_pkcs8 = on ? k : NULL;
  tf__ikey_test_pkcs8_n = on ? n : 0;
}

/* ------------------------------------------------------------------ ikey */
static int t_ikey(const char *work) {
  rmrf(work);
  tf_mkdirs(work);
#ifdef _WIN32
  const int kinds[2] = {0, TF_IKEY_KSP};
#else
  const int kinds[1] = {0};
#endif
  for (size_t ki = 0; ki < sizeof kinds / sizeof kinds[0]; ++ki) {
    tf__ikey_force_kind = kinds[ki];
    char da[1100], db[1100];
    snprintf(da, sizeof da, "%s/a%d", work, (int)ki);
    snprintf(db, sizeof db, "%s/b%d", work, (int)ki);
    tf_mkdirs(da);
    tf_mkdirs(db);
    TfIkey *a = tf__ikey_open(da, 1), *b = tf__ikey_open(db, 1);
    CHECK(a && b, "the key store opens and makes keys (%s)", ki ? "forced KSP" : "platform's choice");
    if (!a || !b) continue;
    printf("  key store: %s (install_hw %d)\n", tf__ikey_kind_name(a), tf__ikey_hw(a));
#ifdef _WIN32
    if (ki == 1) CHECK(tf__ikey_kind(a) == TF_IKEY_KSP && !tf__ikey_hw(a), "the software KSP when the TPM is not used");
    CHECK(tf__ikey_exportable(a) == 0, "no private key of the store can be exported");
#else
    {
      char kf[1200];
      snprintf(kf, sizeof kf, "%s/install/key.bin", da);
      struct stat st;
      CHECK(!stat(kf, &st) && (st.st_mode & 0777) == 0600, "the key file is mode 0600");
      uint8_t mk[32];
      /* a machine without a machine id (a container) has no machine key: the key file is then in the clear, 0600 */
      /* (an image's /etc/machine-id may be there and empty: no id, as tf__machine_key reads it) */
      struct stat ms;
      const int has_id = (!stat("/etc/machine-id", &ms) && ms.st_size > 1) || (!stat("/var/lib/dbus/machine-id", &ms) && ms.st_size > 1);
      if (has_id) CHECK(!tf__machine_key(mk), "the machine key exists on this machine");
      else CHECK(tf__machine_key(mk) == -1, "no machine id here (a container): no machine key, the key file 0600 in the clear");
      if (!has_id) printf("  no machine id on this machine: the key file is not sealed (README)\n");
    }
#endif
    size_t n = 0;
    const uint8_t *sa = tf__ikey_spki(a, 0, &n), *sb = tf__ikey_spki(b, 0, &n);
    CHECK(sa && n == 91 && sa[0] == 0x30 && sa[26] == 4, "the agreement key's SPKI is P-256 DER");
    uint8_t pa[65], pb[65], za[32], zb[32];
    spki_point(sa, pa);
    spki_point(sb, pb);
    CHECK(!tf__ikey_agree(a, 0, pb, 65, za) && !tf__ikey_agree(b, 0, pa, 65, zb) && !memcmp(za, zb, 32), "ECDH both ways gives one secret");
    const char msg[] = "quranlab-attest 1\n-\n0123\n";
    uint8_t sig[72];
    size_t sl = sizeof sig;
    uint8_t ps[65];
    spki_point(tf__ikey_spki(a, 1, &n), ps);
    CHECK(!tf__ikey_sign(a, 0, (const uint8_t *)msg, strlen(msg), sig, &sl) && sl == 64 && tf_p256_verify(ps, msg, strlen(msg), sig),
          "the signing key's ECDSA P-256 SHA-256 verifies");
    /* a new agreement key per attest */
    uint8_t before[91];
    memcpy(before, sa, 91);
    CHECK(!tf__ikey_rotate(a) && tf__ikey_spki(a, 2, &n) && memcmp(tf__ikey_spki(a, 2, &n), before, 91), "rotate makes a new agreement key");
    uint8_t pending[91];
    memcpy(pending, tf__ikey_spki(a, 2, &n), 91);
    /* the attest's own proof (18.4.3): while a new key is pending, the signature made for the attest (tf__ikey_sign,
     * pending) verifies with the signing key the attest names (tf__ikey_spki 1). A file key store signs with its pending
     * key; until 11-18 the old key was named, and production refused every Linux install's first attest (Stage B) */
    {
      uint8_t ps2[65], sig2[72];
      size_t sl2 = sizeof sig2;
      spki_point(tf__ikey_spki(a, 1, &n), ps2);
      CHECK(!tf__ikey_sign(a, 1, (const uint8_t *)msg, strlen(msg), sig2, &sl2) && sl2 == 64 && tf_p256_verify(ps2, msg, strlen(msg), sig2),
            "while a key is pending, the attest's signature verifies with the signing key the attest names");
    }
    CHECK(!tf__ikey_agree(a, 1, pb, 65, za), "the pending key agrees");
    tf__ikey_discard(a);
    CHECK(!memcmp(tf__ikey_spki(a, 0, &n), before, 91), "a discarded attest keeps the old key");
    CHECK(!tf__ikey_rotate(a) && !tf__ikey_commit(a, NULL), "commit");
    uint8_t now[91];
    memcpy(now, tf__ikey_spki(a, 0, &n), 91);
    CHECK(memcmp(now, before, 91), "after the attest the agreement key is new");
    tf__ikey_close(a);
    a = tf__ikey_open(da, 0);
    CHECK(a && !memcmp(tf__ikey_spki(a, 0, &n), now, 91), "the keys are found again after a restart");
    if (a) {
      spki_point(tf__ikey_spki(a, 0, &n), pa);
      CHECK(!tf__ikey_agree(b, 0, pa, 65, zb) && !tf__ikey_agree(a, 0, pb, 65, za) && !memcmp(za, zb, 32), "the reopened key agrees");
    }
    /* the platform table reports a software key store as signal 16 */
    TfPlat *pl = tf__plat_new(da, a);
    const qlc_platform *t = pl ? tf__plat_table(pl, 0) : NULL;
    CHECK(t && t->signals == (tf__ikey_hw(a) ? 0u : 16u), "signal 16 exactly when the key store is software");
    tf__plat_free(pl);
    tf__ikey_destroy_keys(a);
    tf__ikey_destroy_keys(b);
    tf__ikey_close(a);
    tf__ikey_close(b);
  }
  tf__ikey_force_kind = 0;
  return done("ikey");
}

/* ------------------------------------------------------------------ lease, clock, container */
typedef struct {
  TfClient *c;
  TfPm *p;
  char *manifest;
  size_t mn;
  char build[41];
} Host;

/* a Client whose install key is the vectors' ECDH test key, and manifest_40.json's manifest as bytes */
static int host_open(Host *h, const char *vec, const char *work) {
  memset(h, 0, sizeof *h);
  TfJson *lj = load_json(vec, "lease.json"), *mj = load_json(vec, "manifest_40.json");
  if (!lj || !mj) return -1;
  static uint8_t *pk8;
  static size_t pk8n;
  free(pk8);
  pk8n = unb64(S(tf_json_get(lj, "install_ecdh"), "pkcs8_b64u"), &pk8);
  tf__ikey_test_pkcs8 = pk8;
  tf__ikey_test_pkcs8_n = pk8n;
  rmrf(work);
  tf_mkdirs(work);
  h->c = test_client(work, S(tf_json_get(lj, "signer"), "public_hex"));
  h->p = h->c ? tf__pm(h->c) : NULL;
  TfBuf b;
  tf_buf_init(&b);
  tf_json_write(&b, tf_json_get(mj, "manifest"));
  h->manifest = tf_buf_take(&b);
  h->mn = h->manifest ? strlen(h->manifest) : 0;
  snprintf(h->build, sizeof h->build, "%s", S(lj, "build"));
  tf_json_free(lj);
  tf_json_free(mj);
  if (h->p) {
    char bd[1100];
    snprintf(bd, sizeof bd, "%s/models/%s", work, h->build);
    tf_mkdirs(bd);
    tf__plat_set_build(tf__pm_plat(h->p), bd);
  }
  return h->p ? 0 : -1;
}
static void host_close(Host *h) {
  /* a host on the platform's own key store (a TPM key): its keys are removed, so no test leaves one behind */
  if (h->p && !tf__ikey_test_pkcs8) tf__ikey_destroy_keys(tf__pm_ikey(h->p));
  if (h->c) tf__client_close(h->c);
  free(h->manifest);
  tf__ikey_test_pkcs8 = NULL;
  tf__plat_test_wall_ms = tf__plat_test_boot_ms = -1;
  tf__plat_test_boot_id = NULL;
}
static int accept_hex(Host *h, const char *lease_hex, int64_t *exp) {
  uint8_t *l = NULL;
  const size_t n = unhex(lease_hex, &l);
  int64_t ra = 0, e = 0;
  const int st = qlc_lease_accept(tf__pm_core(h->p), l, n, h->manifest, h->mn, h->build, &e, &ra);
  free(l);
  if (exp) *exp = e;
  return st;
}
/* 4.0.2 (18.4.7.3 item 5): a new process that starts after the retry time the last one kept: the kept row's time is
 * now (the SDK's own clock does not move with the Core's test clock) */
static void lease_row_passed(const char *work) {
  char p[1200];
  snprintf(p, sizeof p, "%s/install/lease_row.json", work);
  char *text = NULL;
  size_t n = 0;
  TfJson *j = !tf_read_file(p, &text, &n) ? tf_json_parse(text, n, NULL) : NULL;
  const TfJson *f = tf_json_get(j, "fails");
  if (f && f->type == TJ_NUM) {
    char out[96];
    const int k = snprintf(out, sizeof out, "{\"fails\":%d,\"next_try\":%lld}\n", (int)f->num, (long long)time(NULL));
    tf_write_file(p, out, (size_t)k);
  }
  tf_json_free(j);
  free(text);
}
static void set_clock(long long wall_s, long long boot_ms, const char *boot_id) {
  tf__plat_test_wall_ms = wall_s * 1000;
  tf__plat_test_boot_ms = boot_ms;
  tf__plat_test_boot_id = boot_id;
}

static int t_lease(const char *vec, const char *work) {
  TfJson *lj = load_json(vec, "lease.json");
  if (!lj) return 2;
  const long long now = N(lj, "now");
  int accepted = 0, refused = 0, newer = 0, rsa = 0;
  const TfJson *acc = tf_json_get(lj, "accept"), *ref = tf_json_get(lj, "refuse"), *nw = tf_json_get(lj, "accept_newer");
  for (int i = 0; acc && i < acc->n; ++i) {
    const TfJson *c = &acc->kids[i];
    /* the accepted leases with the platform's counter on (Windows: the registry value beside the folder), so a state
     * saved and opened again goes through it; removed after */
    tf__plat_test_no_counter = 0;
    if (N(c, "wrap") != 1) {
      rsa++; /* desktop keys are P-256: the RSA wrap is Android's (18.4.2) */
      continue;
    }
    Host h;
    char w[1100];
    snprintf(w, sizeof w, "%s/a%d", work, i);
    if (host_open(&h, vec, w)) return 2;
    set_clock(now, 1000, "b1");
    int64_t exp = 0;
    const int st = accept_hex(&h, S(c, "lease_hex"), &exp);
    CHECK(st == QLC_OK && exp == N(tf_json_get(c, "fields"), "exp"), "lease %s: accepted (%d, detail %d)", S(c, "name"), st, qlc_detail(tf__pm_core(h.p)));
    qlc_lease_status ls;
    tf__pm_status(h.p, &ls);
    CHECK(ls.valid && ls.has_lease, "lease %s: valid after accept", S(c, "name"));
    accepted += st == QLC_OK;
    /* the state the Core saved through this SDK opens again: a new Core of the same install has the lease */
    tf__client_close(h.c);
    h.c = test_client(w, S(tf_json_get(lj, "signer"), "public_hex"));
    TfPm *p2 = h.c ? tf__pm(h.c) : NULL;
    tf__pm_status(p2, &ls);
    CHECK(ls.valid, "lease %s: the saved state gives the lease back to a new Core", S(c, "name"));
    if (p2) tf__plat_counter_reset(tf__pm_plat(p2));
    host_close(&h);
  }
  tf__plat_test_no_counter = 1;
  int core_only = 0;
  for (int i = 0; ref && i < ref->n; ++i) {
    const TfJson *c = &ref->kids[i];
    const TfJson *cx = tf_json_get(c, "context");
    /* a case whose context names another Core release cannot be made with this Core (its release is fixed): the
     * Core's own vectors run it */
    if (tf_json_get(cx, "release")) {
      core_only++;
      continue;
    }
    Host h;
    char w[1100];
    snprintf(w, sizeof w, "%s/r%d", work, i);
    if (tf_json_get(cx, "install_hash_hex")) {
      /* another install: this SDK's own key store (another key than the lease is wrapped for) */
      if (host_open(&h, vec, w)) return 2;
      tf__client_close(h.c);
      tf__ikey_test_pkcs8 = NULL;
      h.c = test_client(w, NULL);
      h.p = h.c ? tf__pm(h.c) : NULL;
      if (!h.p) return 2;
    } else if (host_open(&h, vec, w)) {
      return 2;
    }
    if (tf_json_get(cx, "build")) snprintf(h.build, sizeof h.build, "%s", S(cx, "build"));
    if (tf_json_get(cx, "key_id_hex") || tf_json_get(cx, "model") || tf_json_get(cx, "version")) {
      /* another key id, model or version: a manifest that names it (its signature then fails too) */
      char *k = strstr(h.manifest, S(tf_json_get(tf_json_get(load_json(vec, "manifest_40.json"), "manifest"), "encryption"), "key_id"));
      if (k) k[0] = k[0] == '0' ? '1' : '0';
    }
    /* the Core's clock first, from the vectors' first lease, at the context's time: a later lease is held to it */
    int64_t exp0 = 0;
    const long long cnow = tf_json_get(cx, "now") ? N(cx, "now") : now;
    const long long iat0 = N(tf_json_get(&acc->kids[0], "fields"), "iat");
    if (!tf_json_get(cx, "install_hash_hex")) {
      set_clock(now, 1000, "b1");
      accept_hex(&h, S(&acc->kids[0], "lease_hex"), &exp0);
    }
    set_clock(cnow, 1000 + (cnow > iat0 ? (cnow - iat0) * 1000 : 0), "b1");
    const int st = accept_hex(&h, S(c, "lease_hex"), NULL);
    CHECK(st != QLC_OK, "lease refusal %s (%s): refused", S(c, "name"), S(c, "refuse"));
    qlc_lease_status ls;
    tf__pm_status(h.p, &ls);
    CHECK(exp0 ? ls.exp == exp0 : !ls.has_lease, "lease refusal %s: the lease held before is unchanged", S(c, "name"));
    refused += st != QLC_OK;
    host_close(&h);
  }
  for (int i = 0; nw && i < nw->n; ++i) {
    const TfJson *c = &nw->kids[i];
    Host h;
    char w[1100];
    snprintf(w, sizeof w, "%s/n%d", work, i);
    if (host_open(&h, vec, w)) return 2;
    /* the Core's clock first anchored by the vectors' first lease, then the newer one */
    set_clock(now, 1000, "b1");
    accept_hex(&h, S(&acc->kids[0], "lease_hex"), NULL);
    set_clock(N(tf_json_get(c, "context"), "now") ? N(tf_json_get(c, "context"), "now") : now, 1000, "b1");
    const int st = accept_hex(&h, S(c, "lease_hex"), NULL);
    qlc_lease_status ls;
    tf__pm_status(h.p, &ls);
    CHECK(st == QLC_OK && ls.now == N(c, "anchor"), "lease %s: accepted, the clock at its iat (%lld, want %lld)", S(c, "name"), (long long)ls.now,
          N(c, "anchor"));
    newer += st == QLC_OK;
    host_close(&h);
  }
  printf("  leases: %d accepted, %d refused, %d newer re-anchored, %d RSA (not a desktop key) skipped, %d for another Core release left to "
         "the Core's own vectors\n", accepted, refused, newer, rsa, core_only);
  tf_json_free(lj);
  return done("lease");
}

/* the clock rule of clock.json, as a reference (in seconds): what the Core must answer to this SDK's clocks */
typedef struct {
  int has;
  long long floor, now, last_boot, offset, exp;
  char boot_id[16];
} RefClock;
static void ref_check(RefClock *r, long long wall, long long boot, const char *bid, long long *now, long long *floor) {
  if (!strcmp(r->boot_id, bid)) {
    r->floor += boot - r->last_boot;
    r->now += boot - r->last_boot;
  } else {
    r->floor += boot;
    const long long a = r->now + boot, b = wall + r->offset;
    r->now = a > b ? a : b;
  }
  if (r->now < r->floor) r->now = r->floor;
  r->last_boot = boot;
  snprintf(r->boot_id, sizeof r->boot_id, "%s", bid);
  *now = r->now;
  *floor = r->floor;
}
static int t_clock(const char *vec, const char *work) {
  TfJson *lj = load_json(vec, "lease.json"), *cj = load_json(vec, "clock.json");
  if (!lj || !cj) return 2;
  tf__plat_test_no_counter = 1;
  const TfJson *a0 = &tf_json_get(lj, "accept")->kids[0];
  const long long iat = N(tf_json_get(a0, "fields"), "iat"), exp = N(tf_json_get(a0, "fields"), "exp");
  /* the steps: clock.json's situations, laid on the vectors' lease (its iat and exp) */
  static const struct {
    long long dwall, boot;
    const char *bid;
    const char *what;
  } steps[] = {
      {50, 1000, "b1", "the lease"},
      {3650, 4600, "b1", "an hour later, the same boot"},
      {-86400 * 30, 8200, "b1", "the wall clock moved back a month: nothing changes on the same boot"},
      {86400 * 300, 11800, "b1", "the wall clock moved forward: nothing changes on the same boot"},
      {7300, 60000, "b2", "a reboot: the stored time plus the new boot, or the corrected wall clock"},
      {86400 * 3, 61000, "b2", "the wall clock days ahead on that boot: not read"},
      {86400 * 3, 1000, "b3", "a reboot with the wall clock days ahead: the lease ends early (needs a connection)"},
      {600, 2000, "b3", "back on that boot"},
  };
  Host h;
  if (host_open(&h, vec, work)) return 2;
  RefClock ref;
  memset(&ref, 0, sizeof ref);
  for (size_t i = 0; i < sizeof steps / sizeof steps[0]; ++i) {
    set_clock(iat + steps[i].dwall, steps[i].boot, steps[i].bid);
    long long rnow, rfloor;
    if (i == 0) {
      const int st = accept_hex(&h, S(a0, "lease_hex"), NULL);
      CHECK(st == QLC_OK, "clock: the lease is accepted");
      ref.has = 1;
      ref.floor = ref.now = iat; /* a first lease anchors at its own iat, whatever the device's clock says */
      ref.offset = iat - (iat + steps[i].dwall);
      ref.last_boot = steps[i].boot / 1000;
      ref.exp = exp;
      snprintf(ref.boot_id, sizeof ref.boot_id, "%s", steps[i].bid);
      rnow = rfloor = iat;
    } else {
      ref_check(&ref, iat + steps[i].dwall, steps[i].boot / 1000, steps[i].bid, &rnow, &rfloor);
    }
    qlc_lease_status ls;
    tf__pm_status(h.p, &ls);
    const int want_valid = rnow < exp;
    CHECK(ls.now == rnow && ls.valid == want_valid, "clock step %d (%s): now %lld valid %d, the rule says %lld %d", (int)i, steps[i].what,
          (long long)ls.now, ls.valid, rnow, want_valid);
  }
  printf("  %d steps on this SDK's clocks; clock.json's %d sequences run in the Core's own vectors\n", (int)(sizeof steps / sizeof steps[0]),
         tf_json_get(cj, "sequences")->n);
  host_close(&h);
  tf_json_free(lj);
  tf_json_free(cj);
  return done("clock");
}

static int t_container(const char *vec, const char *work) {
  TfJson *lj = load_json(vec, "lease.json"), *mj = load_json(vec, "manifest_40.json");
  if (!lj || !mj) return 2;
  tf__plat_test_no_counter = 1;
  Host h;
  if (host_open(&h, vec, work)) return 2;
  set_clock(N(lj, "now"), 1000, "b1");
  CHECK(accept_hex(&h, S(&tf_json_get(lj, "accept")->kids[0], "lease_hex"), NULL) == QLC_OK, "container: the lease");
  char bd[1100];
  snprintf(bd, sizeof bd, "%s/models/%s", work, h.build);
  const TfJson *files = tf_json_get(tf_json_get(mj, "manifest"), "files"), *hx = tf_json_get(mj, "containers_hex");
  int installed = 0;
  for (int i = 0; i < files->n; ++i) {
    const TfJson *f = &files->kids[i];
    if (strcmp(S(f, "encoding"), "qle1")) continue;
    /* the desktop profile: files for every platform only (an Apple or Android program is never installed here) */
    const TfJson *pl = tf_json_get(f, "platforms");
    if (!pl || pl->type != TJ_STR || strcmp(pl->str, "*")) continue;
    const char *dp = S(tf_json_get(f, "decoded"), "path");
    char *cdn = tf__plat_path(tf__pm_plat(h.p), QLC_FORM_CDN, dp), *dev = tf__plat_path(tf__pm_plat(h.p), QLC_FORM_DEVICE, dp);
    uint8_t *b = NULL;
    const size_t n = unhex(S(hx, S(f, "path")), &b);
    char *dir = (char *)malloc(strlen(cdn) + 1);
    strcpy(dir, cdn);
    *strrchr(dir, '/') = 0;
    tf_mkdirs(dir);
    free(dir);
    tf_write_file(cdn, b, n);
    const int rc = tf__pm_install(h.p, bd, h.manifest, h.mn, dp);
    const long long dsz = N(tf_json_get(f, "decoded"), "size"), recs = dsz ? (dsz + (1 << 20) - 1) >> 20 : 1;
    char *data = NULL;
    size_t dn = 0;
    CHECK(rc == 0 && !tf_read_file(dev, &data, &dn) && (long long)dn == 1024 + dsz + 16 * recs, "container %s: installed as a device form of %lld bytes (%d)",
          dp, 1024 + dsz + 16 * recs, rc);
    installed += rc == 0;
    free(data);
    /* a changed byte in the CDN form (inside the first record) is refused, and nothing half written stays */
    if (n > 1100) {
      b[1030] ^= 1;
      tf_write_file(cdn, b, n);
      tf_file_exists(dev) ? remove(dev) : 0;
      const int bad = tf__pm_install(h.p, bd, h.manifest, h.mn, dp);
      CHECK(bad == TF_ERR_CHECKSUM_MISMATCH && !tf_file_exists(dev), "container %s: a changed byte is refused (%d)", dp, bad);
    }
    free(b);
    free(cdn);
    free(dev);
  }
  /* no plaintext of the weights anywhere in the SDK's folder (manifest_40's layers.zfw is stream("layers.zfw", 3000)) */
  uint8_t plain[3000];
  stream_bytes("layers.zfw", plain, sizeof plain);
  CHECK(!tree_holds(work, plain + 1000, 32, 0), "no plaintext of a model file is on disk");
  printf("  %d containers installed through the Core into this SDK's storage\n", installed);
  host_close(&h);
  tf_json_free(lj);
  tf_json_free(mj);
  return done("container");
}

static int t_attest(const char *vec, const char *work) {
  TfJson *cj = load_json(vec, "install_cert.json");
  if (!cj) return 2;
  const TfJson *cases = tf_json_get(tf_json_get(cj, "attest_proof"), "cases");
  int agree = 0;
  for (int i = 0; cases && i < cases->n; ++i) {
    const TfJson *c = &cases->kids[i];
    uint8_t *ik = NULL, *sk = NULL, *sg = NULL, h[32], raw[64], pub[65];
    const size_t ikn = unb64(S(c, "install_key_b64u"), &ik), skn = unb64(S(c, "install_sign_key_b64u"), &sk), sgn = unb64(S(c, "install_sig_b64u"), &sg);
    tf_sha256(ik, ikn, h);
    char hx[65], msg[400];
    tf_hex_encode(h, 32, hx);
    const char *ch = tf_json_str(tf_json_get(c, "challenge"));
    snprintf(msg, sizeof msg, "quranlab-attest 1\n%s\n%s\n", ch ? ch : "-", hx);
    int ok = skn == 91 && !der_to_raw(sg, sgn, raw);
    if (ok) {
      spki_point(sk, pub);
      ok = tf_p256_verify(pub, msg, strlen(msg), raw);
    }
    const TfJson *v = tf_json_get(c, "valid");
    const int want = v && v->type == TJ_BOOL && v->boolean;
    CHECK(ok == want, "attest_proof %s: %d, want %d", S(c, "name"), ok, want);
    agree += ok == want;
    free(ik);
    free(sk);
    free(sg);
  }
  /* the Core's qlc_attest_sig with this SDK's key store (the platform's own store), by the same rule, without a
   * challenge (desktop) */
  rmrf(work);
  tf_mkdirs(work);
  TfClient *c = test_client(work, NULL);
  TfPm *p = c ? tf__pm(c) : NULL;
  char sig[160] = "";
  CHECK(p && qlc_attest_sig(tf__pm_core(p), NULL, sig, sizeof sig) == QLC_OK, "qlc_attest_sig");
  if (p) {
    size_t n = 0;
    uint8_t h[32], raw[64], pub[65], *sg = NULL;
    tf_sha256(tf__ikey_spki(tf__pm_ikey(p), 0, &n), 91, h);
    char hx[65], msg[200];
    tf_hex_encode(h, 32, hx);
    snprintf(msg, sizeof msg, "quranlab-attest 1\n-\n%s\n", hx);
    const size_t sgn = unb64(sig, &sg);
    spki_point(tf__ikey_spki(tf__pm_ikey(p), 1, &n), pub);
    CHECK(!der_to_raw(sg, sgn, raw) && tf_p256_verify(pub, msg, strlen(msg), raw), "the Core's install_sig verifies over the agreement key's hash");
    free(sg);
    tf__ikey_destroy_keys(tf__pm_ikey(p));
  }
  if (c) tf__client_close(c);
  printf("  attest_proof: %d of %d cases as the vectors say\n", agree, cases ? cases->n : 0);
  tf_json_free(cj);
  return done("attest");
}

/* ------------------------------------------------------------------ the stand-in server */
enum { LEASE_OK = 0, LEASE_SIGNED_STOP, LEASE_UNSIGNED_STOP, LEASE_CERT_INVALID_ONCE, LEASE_CHALLENGE_INVALID_ONCE, LEASE_ROUTE_OFF, LEASE_SIGNED_STOP_NOW,
       LEASE_PIECE, LEASE_NO_BASE, LEASE_OFF_BASE, LEASE_LATER, LEASE_HOST_BASE };
typedef struct {
  TfJson *fx;
  int lease_mode, once_done;
  int attests, attest_keys, bad_attest_sig, challenges, leases, bad_lease, bad_lease_sig, files, no_token;
  int named[16], n_named;              /* core.piece of each lease request, in order (-1: none) */
  int manifest_host, manifest_base;    /* the manifest read from the model host's URL / from the lease's base */
  int files_host, files_base;          /* the files read from the model host's URL / from the lease's base */
  int files_at_lease;                  /* LEASE_HOST_BASE: the files asked for when the lease answer went out */
  const char *cfg;                     /* the fixture's config to answer ("config" when NULL) */
  char last_install_key[200];
  /* the error reports' tests (tf40_errors_tests.inc): a request whose URL holds fail_url is answered fail_status with
   * fail_body (fail_status 0: no answer at all); corrupt changes a byte of every model file served; the error reports
   * the stand-in took (202, or issues_status) */
  const char *fail_url, *fail_body;
  int fail_status, corrupt, issues_status;
  /* fail_times: only the first fail_times requests of fail_url fail (0: every one); fail_done counts them */
  int fail_times, fail_done;
  /* fail_with: only a request whose JSON body has this member fails (NULL: any; "install_key": the attest of the
   * install keys, not the app token's) */
  const char *fail_with;
  char err_bodies[8][2048];
  int n_err;
  int err_host[8];                     /* the api host (index of k_api_hosts) each error report was sent to */
  int api_asked[4];                    /* requests to each api host (4.0.1: a failover asks the next) */
  char last_config[600], last_register[1200]; /* section 27: the last config request's path and query, register's body */
  int configs;
  int script_host[32];                 /* 4.0.1: the api host each scripted answer was given on (endpoints_40 attempts) */
  /* tf40_tests grace: one grace.json outcome's answers, given in order to the requests of their routes (token, attest,
   * challenge, model-key); a request of another route, or past the last, is answered as usual. retry_after: the
   * Retry-After of a scripted answer. */
  const TfJson *script;
  int at, retry_after;
  /* a request whose URL holds slow_url is answered 503 after slow_ms (another thread's call in flight: grace_busy) */
  const char *slow_url;
  int slow_ms;
  /* a request whose URL holds stall_url gets an answer that begins (200) and then stalls: the transport's timeout */
  const char *stall_url;
  /* 4.0.2 (tf40_tests no_wait): every request's URL, in the order the stand-in took them (under g_f_lock) */
  char req[160][200];
  int n_req;
  long long req_from[160]; /* 4.0.2 (SPEC 16.7): each request's Range (bytes=N-), 0 for none */
  int req_ifr[160];        /* and whether it sent If-Range */
  /* 4.0.2 (SPEC 16.7): the next cut_n answers for a model file whose URL holds cut_url end half way (a cut transfer);
   * every model file's answer carries a strong ETag and, for a range, its Content-Range */
  const char *cut_url;
  int cut_n, cut_pct; /* a cut keeps cut_pct percent of the answer (0: half) */
  /* 4.0.2 (STEERING_40.md 34.6): a second failing URL (fail2_status 0: no answer); and known timings on the calling
   * thread's clock (tf__dl_clock = fake_clock): every answer's status ttfb_ms after it is asked, a model file's body at
   * rate_kBps (1000 bytes) */
  const char *fail2_url;
  int fail2_status;
  double ttfb_ms, rate_kBps;
  /* 4.0.2 (tf40_tests load, the Client's close): a request whose URL holds hold_url waits hold_ms before it is answered,
   * and a cancel of its flag ends it at once as the transport's would; holding: 1 while it waits, 2 cancelled, 3 answered */
  const char *hold_url;
  int hold_ms, holding;
} Fake;
static Fake g_f;
static _Thread_local double t_fake_clock; /* seconds, on each thread its own (a lane's requests run on its thread) */
static double fake_clock(void) { return t_fake_clock; }
/* 4.0.1: the downloader's lanes ask for files from several threads at once, beside the lease on the caller's thread:
 * the model host's counts are kept under this lock */
static TfMutex g_f_lock;
static int g_f_lock_ready;
static int fake_files(void) {
  tf_mutex_lock(&g_f_lock);
  const int n = g_f.files;
  tf_mutex_unlock(&g_f_lock);
  return n;
}
static char *dupz(const char *s, size_t n) {
  char *p = (char *)malloc(n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}
static void answer(TfHttpResponse *r, int status, const char *body) {
  r->status = status;
  r->body = dupz(body, strlen(body));
  r->body_n = strlen(body);
}
static int has_header(const TfHttpRequest *q, const char *prefix) {
  for (const char *const *h = q->headers; h && *h; ++h)
    if (!strncmp(*h, prefix, strlen(prefix))) return 1;
  return 0;
}
/* ECDSA of a SPKI over a text: what the server checks for install_sig */
static int sig_ok(const char *spki_b64u, const char *sig_b64u, const char *text) {
  uint8_t *sk = NULL, *sg = NULL, raw[64], pub[65];
  const size_t skn = unb64(spki_b64u, &sk), sgn = unb64(sig_b64u, &sg);
  int ok = skn == 91 && !der_to_raw(sg, sgn, raw);
  if (ok) {
    spki_point(sk, pub);
    ok = tf_p256_verify(pub, text, strlen(text), raw);
  }
  free(sk);
  free(sg);
  return ok;
}
/* grace.json's answers (18.4.7.1): `answer` none or stalled (no answer), late (none within the time the open waits:
 * the request's whole time, then none), else a status with a body of its kind; a 2xx lease answer the Core accepts is
 * the install's next lease (lease_later), one it refuses has a changed byte (lease_bad); a stop the Core verifies is
 * the signed stop made at the moment the test replays the outcomes at (refusal_signed_grace) */
/* 4.0.1 (ENDPOINTS_40.md 28.1): the stand-in answers the API on each of the api role's built-in hosts, in their order;
 * any other host is a model host. A request's api host is counted (api_asked) and its path is what follows it. */
static const char *const k_api_hosts[] = {"api.quranlab.ai", "api-b.tajweedformer.com", "api-g.tajweedformer.com", "api-a.tajweedformer.com"};
#define N_API_HOSTS 4
static int api_host_of(const char *url) {
  const char *h = strncmp(url, "https://", 8) ? NULL : url + 8;
  for (int i = 0; h && i < N_API_HOSTS; ++i) {
    const size_t n = strlen(k_api_hosts[i]);
    if (!strncmp(h, k_api_hosts[i], n) && h[n] == '/') return i;
  }
  return -1;
}
static const char *api_path(const char *url) {
  const int i = api_host_of(url);
  return i < 0 ? NULL : url + 8 + strlen(k_api_hosts[i]);
}
static const char *route_of(const TfHttpRequest *q, const TfJson *b) {
  const char *path = api_path(q->url);
  if (!path) return NULL;
  if (!strcmp(path, "/v1/attest")) return tf_json_get(b, "install_key") ? "attest" : "token";
  if (!strcmp(path, "/v1/attest/challenge")) return "challenge";
  if (!strcmp(path, "/v1/model-key")) return "model-key";
  return NULL;
}
static int scripted(const TfHttpRequest *q, TfHttpResponse *r, const TfJson *a) {
  const TfJson *fx = g_f.fx;
  const char *ans = S(a, "answer");
  if (*ans) {
    if (!strcmp(ans, "late")) tf__sleep_ms((q->timeout_s > 0 ? q->timeout_s : 1) * 1000);
    return tf__fail(TF__ERR_NETWORK, "the stand-in gives no answer (%s)", ans);
  }
  const int st = (int)N(a, "status");
  const char *body = S(a, "body"), *err = S(a, "error"), *why = S(a, "reason"), *core = S(a, "core"), *route = S(a, "route");
  TfBuf o;
  tf_buf_init(&o);
  if (!strcmp(body, "html")) {
    tf_buf_puts(&o, "<html><body>a page that is not the API's</body></html>");
  } else if (!strcmp(body, "json")) {
    if (!strcmp(core, "stop")) tf_json_write(&o, tf_json_get(fx, "refusal_signed_grace"));
    else if (st == 200 && !strcmp(route, "challenge")) tf_buf_printf(&o, "{\"challenge\":\"%s\",\"expires_in\":300}", S(fx, "challenge"));
    else if (st == 200 && !strcmp(route, "token")) tf_buf_puts(&o, "{\"token\":\"qlt_v1.test\",\"expires_in\":3600,\"attested\":false}");
    else if (st == 200 && !strcmp(route, "attest"))
      tf_buf_printf(&o, "{\"token\":\"qlt_v1.test\",\"expires_in\":3600,\"attested\":false,\"install_cert\":\"%s\"}", S(fx, "cert"));
    else if (st == 200 && !strcmp(route, "model-key")) tf_json_write(&o, tf_json_get(fx, !strcmp(core, "refused") ? "lease_bad" : "lease_later"));
    else {
      tf_buf_puts(&o, "{");
      if (*err) {
        tf_buf_puts(&o, "\"error\":");
        tf_buf_json_str(&o, err);
        if (*why) {
          tf_buf_puts(&o, ",\"reason\":");
          tf_buf_json_str(&o, why);
        }
      } else {
        tf_buf_puts(&o, "\"message\":\"no error string\"");
      }
      tf_buf_puts(&o, "}");
    }
  }
  answer(r, st, o.p ? o.p : "");
  /* an answer's own Retry-After (endpoints_40.json attempts), else the test's */
  r->retry_after = tf_json_get(a, "retry_after") && tf_json_get(a, "retry_after")->type == TJ_NUM ? (int)N(a, "retry_after") : g_f.retry_after;
  tf_buf_free(&o);
  return 0;
}
static int fake(const TfHttpRequest *q, TfHttpResponse *r, void *user) {
  (void)user;
  memset(r, 0, sizeof *r);
  const TfJson *fx = g_f.fx;
  const int host = api_host_of(q->url);
  if (host >= 0) g_f.api_asked[host]++;
  tf_mutex_lock(&g_f_lock);
  if (g_f.n_req < 160) {
    g_f.req_from[g_f.n_req] = q->range_from > 0 ? q->range_from : 0;
    g_f.req_ifr[g_f.n_req] = q->if_range && *q->if_range;
    snprintf(g_f.req[g_f.n_req++], sizeof g_f.req[0], "%s", q->url);
  }
  tf_mutex_unlock(&g_f_lock);
  TfJson *b = q->body ? tf_json_parse((const char *)q->body, q->body_n, NULL) : NULL;
  int rc = 0;
  if (g_f.hold_url && strstr(q->url, g_f.hold_url)) {
    __atomic_store_n(&g_f.holding, 1, __ATOMIC_SEQ_CST);
    for (int waited = 0; waited < g_f.hold_ms; waited += 10) {
      if (q->cancel && __atomic_load_n(q->cancel, __ATOMIC_SEQ_CST)) {
        tf_json_free(b);
        __atomic_store_n(&g_f.holding, 2, __ATOMIC_SEQ_CST);
        tf__http_note(TF__FAIL_CANCELLED, "ERROR_WINHTTP_OPERATION_CANCELLED", 0);
        return tf__fail(TF__ERR_NETWORK, "cancelled");
      }
      tf__sleep_ms(10);
    }
    __atomic_store_n(&g_f.holding, 3, __ATOMIC_SEQ_CST);
  }
  if (g_f.stall_url && strstr(q->url, g_f.stall_url)) {
    tf_json_free(b);
    r->status = 200;
    tf__http_note(TF__FAIL_TIMEOUT, "ERROR_WINHTTP_TIMEOUT", 12002);
    return tf__fail(TF__ERR_NETWORK, "the stand-in's answer stalled");
  }
  if (g_f.slow_url && strstr(q->url, g_f.slow_url)) {
    tf_json_free(b);
    tf__sleep_ms(g_f.slow_ms);
    answer(r, 503, "{}");
    return 0;
  }
  t_fake_clock += g_f.ttfb_ms / 1000.0; /* the status comes ttfb_ms after the request */
  if (g_f.fail2_url && strstr(q->url, g_f.fail2_url)) {
    tf_json_free(b);
    if (!g_f.fail2_status) return tf__fail(TF__ERR_NETWORK, "the stand-in does not answer");
    answer(r, g_f.fail2_status, "{}");
    return 0;
  }
  if (g_f.fail_url && strstr(q->url, g_f.fail_url) && (!g_f.fail_with || tf_json_get(b, g_f.fail_with)) &&
      (!g_f.fail_times || g_f.fail_done++ < g_f.fail_times)) {
    tf_json_free(b);
    if (!g_f.fail_status) return tf__fail(TF__ERR_NETWORK, "the stand-in does not answer");
    answer(r, g_f.fail_status, g_f.fail_body ? g_f.fail_body : "{}");
    r->retry_after = g_f.retry_after;
    return 0;
  }
  if (g_f.script && g_f.at < g_f.script->n) {
    const char *route = route_of(q, b);
    const TfJson *a = &g_f.script->kids[g_f.at];
    if (route && !strcmp(route, S(a, "route"))) {
      if (g_f.at < 32) g_f.script_host[g_f.at] = host;
      g_f.at++;
      if (!strcmp(route, "challenge")) g_f.challenges++;
      if (!strcmp(route, "model-key")) g_f.leases++;
      tf_json_free(b);
      return scripted(q, r, a);
    }
  }
  if (strstr(q->url, "/v1/issues") && q->body && strstr((const char *)q->body, "\"kind\":\"error\"")) {
    if (g_f.n_err < 8) {
      g_f.err_host[g_f.n_err] = host;
      snprintf(g_f.err_bodies[g_f.n_err++], sizeof g_f.err_bodies[0], "%.*s", (int)q->body_n, (const char *)q->body);
    }
    answer(r, g_f.issues_status ? g_f.issues_status : 202, "{\"ok\":true}");
    tf_json_free(b);
    return 0;
  }
  if (host >= 0) {
    const char *path = api_path(q->url);
    if (!strcmp(path, "/v1/attest")) {
      g_f.attests++;
      if (tf_json_get(b, "install_key")) {
        g_f.attest_keys++;
        uint8_t *ik = NULL, h[32];
        const size_t ikn = unb64(S(b, "install_key"), &ik);
        tf_sha256(ik, ikn, h);
        free(ik);
        char hx[65], text[200];
        tf_hex_encode(h, 32, hx);
        snprintf(text, sizeof text, "quranlab-attest 1\n-\n%s\n", hx);
        /* 18.4.3, as the server: install_sig over the agreement key's hash, no challenge on desktop, install_hw 0 or 1 */
        const TfJson *hw = tf_json_get(b, "install_hw");
        if (!sig_ok(S(b, "install_sign_key"), S(b, "install_sig"), text) || tf_json_get(b, "challenge") || !hw || hw->type != TJ_NUM || hw->num < 0 ||
            hw->num > 1) {
          g_f.bad_attest_sig++;
          answer(r, 401, "{\"error\":\"install_sig_invalid\"}");
        } else {
          snprintf(g_f.last_install_key, sizeof g_f.last_install_key, "%s", S(b, "install_key"));
          char out[4096];
          snprintf(out, sizeof out, "{\"token\":\"qlt_v1.test\",\"expires_in\":3600,\"attested\":false,\"install_cert\":\"%s\"}", S(fx, "cert"));
          answer(r, 200, out);
        }
      } else {
        answer(r, 200, "{\"token\":\"qlt_v1.test\",\"expires_in\":3600,\"attested\":false}");
      }
    } else if (!strncmp(path, "/v1/config", 10)) {
      g_f.configs++;
      snprintf(g_f.last_config, sizeof g_f.last_config, "%s", path);
      char kb[64], ks[64];
      snprintf(kb, sizeof kb, "%s_body", g_f.cfg ? g_f.cfg : "config");
      snprintf(ks, sizeof ks, "%s_sig", g_f.cfg ? g_f.cfg : "config");
      answer(r, 200, S(fx, kb));
      snprintf(r->signature, sizeof r->signature, "%s", S(fx, ks));
      snprintf(r->key_id, sizeof r->key_id, "%s", S(fx, "config_kid"));
      snprintf(r->etag, sizeof r->etag, "\"e1\"");
    } else if (!strcmp(path, "/v1/attest/challenge")) {
      g_f.challenges++;
      g_f.no_token += !has_header(q, "authorization: Bearer ");
      char out[200];
      snprintf(out, sizeof out, "{\"challenge\":\"%s\",\"expires_in\":300}", S(fx, "challenge"));
      answer(r, 200, out);
    } else if (!strcmp(path, "/v1/model-key")) {
      g_f.leases++;
      g_f.no_token += !has_header(q, "authorization: Bearer ");
      /* the closed schema of 18.4.4 (with spec fix 12's core.piece), and install_sig over the challenge and build */
      static const char *const top[] = {"v", "platform", "install_cert", "install_key", "install_sign_key", "challenge", "install_sig", "model",
                                        "version", "build", "key_id", "core"};
      static const char *const core[] = {"release", "target", "proof", "signals", "piece"};
      int shape = b && b->type == TJ_OBJ && b->n == 12;
      for (size_t k = 0; shape && k < 12; ++k) shape = tf_json_get(b, top[k]) != NULL;
      const TfJson *co = tf_json_get(b, "core");
      shape = shape && co && co->type == TJ_OBJ && co->n == 5;
      for (size_t k = 0; shape && k < 5; ++k) shape = tf_json_get(co, core[k]) != NULL;
      shape = shape && !strcmp(S(co, "release"), "4.0.0-c1") && strlen(S(co, "proof")) == 22 && !strcmp(S(b, "platform"), tf__platform_name()) &&
              !strcmp(S(b, "build"), S(fx, "build")) && !strcmp(S(b, "install_cert"), S(fx, "cert"));
      g_f.bad_lease += !shape;
      const TfJson *pn = tf_json_get(co, "piece");
      if (g_f.n_named < 16) g_f.named[g_f.n_named++] = pn && pn->type == TJ_NUM ? (int)pn->num : -1;
      char text[300];
      snprintf(text, sizeof text, "quranlab-lease-ask 1\n%s\n%s\n", S(b, "challenge"), S(b, "build"));
      if (!sig_ok(S(b, "install_sign_key"), S(b, "install_sig"), text)) g_f.bad_lease_sig++;
      TfBuf o;
      tf_buf_init(&o);
      if (g_f.lease_mode == LEASE_SIGNED_STOP || g_f.lease_mode == LEASE_SIGNED_STOP_NOW || g_f.lease_mode == LEASE_UNSIGNED_STOP) {
        tf_json_write(&o, tf_json_get(fx, g_f.lease_mode == LEASE_SIGNED_STOP ? "refusal_signed"
                                          : g_f.lease_mode == LEASE_SIGNED_STOP_NOW ? "refusal_signed_now" : "refusal_unsigned"));
        answer(r, 401, o.p);
      } else if (g_f.lease_mode == LEASE_CERT_INVALID_ONCE && !g_f.once_done) {
        g_f.once_done = 1;
        answer(r, 401, "{\"error\":\"install_cert_invalid\",\"why\":\"expired\"}");
      } else if (g_f.lease_mode == LEASE_CHALLENGE_INVALID_ONCE && !g_f.once_done) {
        g_f.once_done = 1;
        answer(r, 401, "{\"error\":\"challenge_invalid\"}");
      } else if (g_f.lease_mode == LEASE_ROUTE_OFF) {
        answer(r, 404, "{\"error\":\"not_found\"}");
      } else if (g_f.lease_mode == LEASE_PIECE && g_f.named[g_f.n_named - 1] == 8 && !g_f.once_done) {
        /* the server's answer to a proof that does not match the digest of the piece it names (18.6.3, 18.6.7) */
        g_f.once_done = 1;
        answer(r, 403, "{\"error\":\"lease_integrity_failed\",\"message\":\"the Core could not be verified\",\"why\":\"core\"}");
      } else if (g_f.lease_mode == LEASE_NO_BASE || g_f.lease_mode == LEASE_OFF_BASE || g_f.lease_mode == LEASE_HOST_BASE) {
        /* the lease with no model folder, with one on a host that only looks like Quran Lab's, or (4.0.1, production's
         * shape) the build's own folder on the model host; that answer waits until the files have come (2 s at most),
         * so a downloader that fetches them only after the lease shows here */
        if (g_f.lease_mode == LEASE_HOST_BASE) {
          for (int i = 0; i < 200 && fake_files() < 8; ++i) tf__sleep_ms(10);
          g_f.files_at_lease = fake_files();
        }
        const TfJson *l = tf_json_get(fx, "lease");
        tf_buf_printf(&o, "{\"v\":1,\"lease\":\"%s\",\"iat\":%lld,\"exp\":%lld,\"refresh_after\":%lld,\"install_cert\":\"%s\"", S(l, "lease"),
                      (long long)tf_json_get(l, "iat")->num, (long long)tf_json_get(l, "exp")->num, (long long)tf_json_get(l, "refresh_after")->num,
                      S(l, "install_cert"));
        if (g_f.lease_mode == LEASE_OFF_BASE)
          tf_buf_printf(&o, ",\"base\":\"https://cdn.quranlab.ai.example.net/models/Quran-Lab/tajweedformer-v4/%s/sdk/v4.0/\"", S(fx, "build"));
        if (g_f.lease_mode == LEASE_HOST_BASE)
          tf_buf_printf(&o, ",\"base\":\"https://cdn.quranlab.ai/models/Quran-Lab/tajweedformer-v4/%s/sdk/v4.0/\"", S(fx, "build"));
        tf_buf_puts(&o, "}");
        answer(r, 200, o.p);
      } else if (g_f.lease_mode == LEASE_PIECE) {
        /* the lease, and the piece of this install's rollout: piece 7 first, then piece 8; the fifth answer carries a
         * piece with a changed signature byte (piece.json refused), which the Core must refuse */
        tf_json_write(&o, tf_json_get(fx, "lease"));
        const char *pc = S(tf_json_get(fx, "pieces"), g_f.leases == 1 ? "piece7" : g_f.leases == 5 ? "piece_bad" : "piece8");
        if (o.p && o.n && o.p[o.n - 1] == '}') {
          o.n--;
          o.p[o.n] = 0;
          tf_buf_printf(&o, ",\"piece\":\"%s\"}", pc);
        }
        answer(r, 200, o.p);
      } else {
        /* LEASE_LATER: the install's next lease, issued 30 hours after the first (make_flow40.py lease_later) */
        tf_json_write(&o, tf_json_get(fx, g_f.lease_mode == LEASE_LATER ? "lease_later" : "lease"));
        answer(r, 200, o.p);
      }
      tf_buf_free(&o);
    } else {
      if (!strcmp(path, "/v1/register")) snprintf(g_f.last_register, sizeof g_f.last_register, "%.*s", (int)q->body_n, (const char *)q->body);
      answer(r, 200, "{}"); /* register, telemetry, issues */
    }
  } else {
    /* the model host: {cdn}/{repo}/{build}/sdk/v4.0/{path}, and the lease answer's base (another folder) */
    const char *rel = strstr(q->url, "/sdk/v4.0/");
    const char *lb = S(tf_json_get(fx, "lease"), "base");
    const int from_base = !strncmp(q->url, lb, strlen(lb));
    tf_mutex_lock(&g_f_lock);
    g_f.no_token += !has_header(q, "authorization: Bearer ");
    tf_mutex_unlock(&g_f_lock);
    if (!rel || !strstr(q->url, S(fx, "build"))) {
      answer(r, 404, "{}");
    } else {
      rel += 10;
      char *data = NULL;
      size_t n = 0;
      if (!strcmp(rel, "manifest.json")) {
        data = dupz(S(fx, "manifest"), strlen(S(fx, "manifest")));
        n = strlen(data);
        tf_mutex_lock(&g_f_lock);
        if (from_base) g_f.manifest_base++;
        else g_f.manifest_host++;
        tf_mutex_unlock(&g_f_lock);
      } else if (tf_json_get(fx, "cdn_dir")) {
        char *p = tf_path_join(S(fx, "cdn_dir"), rel);
        if (!p || tf_read_file(p, &data, &n)) data = NULL;
        free(p);
      } else if (tf_json_get(tf_json_get(fx, "files"), rel)) {
        uint8_t *bb = NULL;
        n = unhex(S(tf_json_get(fx, "files"), rel), &bb);
        data = (char *)bb;
      }
      if (!data) {
        answer(r, 404, "{}");
      } else if (!q->download_file) {
        r->status = 200;
        r->body = data;
        r->body_n = n;
        data = NULL;
      } else {
        tf_mutex_lock(&g_f_lock);
        g_f.files++;
        if (from_base) g_f.files_base++;
        else g_f.files_host++;
        tf_mutex_unlock(&g_f_lock);
        const long long from = q->range_from > 0 ? q->range_from : 0;
        if (g_f.corrupt && n > 0) data[n - 1] ^= 1; /* not the file the manifest names */
        int cut = 0;
        tf_mutex_lock(&g_f_lock);
        if (g_f.cut_url && strstr(q->url, g_f.cut_url) && g_f.cut_n > 0) {
          g_f.cut_n--;
          cut = 1;
        }
        tf_mutex_unlock(&g_f_lock);
        size_t len = n - (size_t)from;
        if (cut) len = g_f.cut_pct > 0 ? len * (size_t)g_f.cut_pct / 100 : len / 2;
        /* the head first, as the real transports give it: the status, the length, the range, a strong ETag */
        r->status = from > 0 ? 206 : 200;
        r->content_length = (long long)(n - (size_t)from);
        r->range_start = r->range_total = -1;
        if (from > 0) r->range_start = from, r->range_total = (long long)n;
        snprintf(r->etag, sizeof r->etag, "\"e-%.16s\"", rel);
        if (q->accept && !q->accept(q->bytes_user, r)) len = 0;
        else if (q->on_head) q->on_head(q->bytes_user, r);
        if (g_f.rate_kBps > 0) t_fake_clock += (double)len / 1000.0 / g_f.rate_kBps; /* the body at its rate */
        FILE *f = len ? fopen(q->download_file, from > 0 ? "ab" : "wb") : NULL;
        if (f) {
          fwrite(data + from, 1, len, f);
          fclose(f);
        }
        if (len && q->on_data) q->on_data(q->bytes_user, data + from, len);
        if (len && q->on_bytes) q->on_bytes(q->bytes_user, (long long)len);
        if (cut) rc = tf__fail(TF__ERR_NETWORK, "connection reset (simulated)");
      }
      free(data);
    }
  }
  tf_json_free(b);
  return rc;
}

static TfClient *flow_client_in(const char *work) {
  TfClientOptions o;
  memset(&o, 0, sizeof o);
  o.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "com.example.recite";
  o.storage = work;
  TfClient *c = tf__client_create_with_key(&o, S(g_f.fx, "signer_hex"));
  if (!c) fprintf(stderr, "client: %s\n", tf_last_error());
  return c;
}
static TfClient *flow_client(const char *work) {
  rmrf(work);
  return flow_client_in(work);
}
static void fake_reset(int mode) {
  if (!g_f_lock_ready) {
    tf_mutex_init(&g_f_lock);
    g_f_lock_ready = 1;
  }
  TfJson *fx = g_f.fx;
  memset(&g_f, 0, sizeof g_f);
  g_f.fx = fx;
  g_f.lease_mode = mode;
}
static TfJson *flow_setup(const char *fixture) {
  TfJson *fx = load_json(fixture, "flow.json");
  if (!fx) return NULL;
  static uint8_t *pk8;
  static size_t pk8n;
  free(pk8);
  pk8n = unb64(S(fx, "install_pkcs8"), &pk8);
  tf__ikey_test_pkcs8 = pk8;
  tf__ikey_test_pkcs8_n = pk8n;
  tf__client_no_timer = 1;
  tf__plat_test_no_counter = 1;
  if (!g_f_lock_ready) {
    tf_mutex_init(&g_f_lock);
    g_f_lock_ready = 1;
  }
  tf_http_set_transport(fake, NULL);
  g_f.fx = fx;
  return fx;
}

static int t_flow(const char *fixture, const char *work) {
  TfJson *fx = flow_setup(fixture);
  if (!fx) return 2;
  char w[1100], models[1200];
  /* 1. the whole way: config, attest (install_sig), challenge, the lease, download, install; the vector build's
   * three-token vocabulary is refused at open, as it must be */
  fake_reset(LEASE_OK);
  snprintf(w, sizeof w, "%s/ok", work);
  TfClient *c = flow_client(w);
  TfModel *m = c ? tf_client_model(c, NULL) : NULL;
  const int err = tf_last_error_code();
  CHECK(!m && err == TF_ERR_MODEL_INCOMPATIBLE, "flow: the vector build is refused for its vocabulary only (%s: %s)", tf_error_name(err), tf_last_error());
  CHECK(g_f.attest_keys == 1 && !g_f.bad_attest_sig, "flow: one attest with the install keys, its install_sig good (%d, bad %d)", g_f.attest_keys,
        g_f.bad_attest_sig);
  CHECK(g_f.challenges == 1 && g_f.leases == 1 && !g_f.bad_lease && !g_f.bad_lease_sig, "flow: one challenge, one lease request of 18.4.4's shape (bad %d, sig %d)",
        g_f.bad_lease, g_f.bad_lease_sig);
  CHECK(!g_f.no_token, "flow: the app token on the challenge, the lease and every model request");
  /* 4.0.1: the files are fetched from the build's folder on the model host while the lease is asked; this fixture's
   * lease answer names another folder, so what came from the host is dropped and every file comes from that folder
   * (production names the host's folder: the LEASE_HOST_BASE run below) */
  CHECK(g_f.manifest_host == 1 && !g_f.manifest_base && g_f.files_base == 8 && g_f.files == g_f.files_base + g_f.files_host,
        "flow: the manifest from the model host, the eight files from the lease answer's base (host %d first, base %d of %d)", g_f.files_host,
        g_f.files_base, g_f.files);
  CHECK(g_f.n_named == 1 && g_f.named[0] == 0, "flow: the first lease request names no data piece (core.piece 0)");
  {
    char *lj = NULL;
    size_t ln = 0;
    char p[1200];
    snprintf(p, sizeof p, "%s/install/lease.json", w);
    CHECK(!tf_read_file(p, &lj, &ln) && strstr(lj, S(fx, "build")) && strstr(lj, "/models/lease/"), "flow: the lease's build and base are kept (%s)",
          lj ? lj : "none");
    free(lj);
  }
  /* every container was installed: the device forms are there, the CDN forms are gone, no plaintext anywhere */
  snprintf(models, sizeof models, "%s/models/%s", w, S(fx, "build"));
  const TfJson *mf = tf_json_parse(S(fx, "manifest"), strlen(S(fx, "manifest")), NULL);
  const TfJson *files = tf_json_get(mf, "files");
  int devs = 0, cdns = 0;
  for (int i = 0; files && i < files->n; ++i) {
    const TfJson *f = &files->kids[i];
    if (strcmp(S(f, "encoding"), "qle1") || strcmp(S(f, "need"), "required")) continue;
    if (strcmp(S(f, "soc"), "*")) continue;
    char p[1400];
    snprintf(p, sizeof p, "%s/dev/%s.qld", models, S(tf_json_get(f, "decoded"), "path"));
    devs += tf_file_exists(p);
    snprintf(p, sizeof p, "%s/cdn/%s.qle", models, S(tf_json_get(f, "decoded"), "path"));
    cdns += tf_file_exists(p);
  }
  CHECK(devs >= 6 && cdns == 0, "flow: %d device forms installed, %d CDN forms left", devs, cdns);
  {
    /* nothing of what came from the host stays: no partial file beside a CDN form */
    char p[1400];
    snprintf(p, sizeof p, "%s/cdn/layers.zfw.qle.part", models);
    CHECK(!tf_file_exists(p), "flow: no partial file of the host's fetch left");
  }
  {
    uint8_t *mk = NULL;
    const size_t mkn = unhex(S(fx, "plain_marker"), &mk);
    CHECK(mkn == 32 && !tree_holds(w, mk, mkn, 0), "flow: no plaintext of a model file in the SDK's folder");
    free(mk);
  }
  const char *dj = c ? tf_client_diagnose_json(c) : NULL;
  {
    char *tb = c ? tf__client_day_body(c) : NULL;
    CHECK(tb && strstr(tb, "\"lease\":{\"ok\":1}"), "flow: the lease request is counted (17.8 lease): %s", tb && strstr(tb, "\"lease\"") ? strstr(tb, "\"lease\"") : "(none)");
    free(tb);
  }
  const char *pj = dj ? strstr(dj, "\"protected\"") : NULL;
  CHECK(pj && strstr(pj, "\"lease\":\"valid\"") && strstr(pj, "\"keyStore\":\"file\""), "flow: the diagnosis names the lease and the key store: %s",
        pj ? pj : "(none)");
  /* DIAGNOSIS_40 (section 25): the class from flags bit 3, the refresh time, no grace end outside the grace */
  CHECK(pj && (strstr(pj, "\"leaseClass\":\"attested\"") || strstr(pj, "\"leaseClass\":\"standard\"")) && strstr(pj, "\"refreshAfter\":") &&
          !strstr(pj, "\"refreshAfter\":null") && strstr(pj, "\"graceUntil\":null"),
        "flow: the diagnosis names the lease's class and refresh time, and no grace: %s", pj ? pj : "(none)");
  tf_json_free((TfJson *)mf);
  if (c) tf_client_destroy(c);

  /* a stop on the first lease request, signed for this install and challenge now: refused, remembered, removed */
  fake_reset(LEASE_SIGNED_STOP_NOW);
  snprintf(w, sizeof w, "%s/stop", work);
  c = flow_client(w);
  m = c ? tf_client_model(c, NULL) : NULL;
  CHECK(!m && tf_last_error_code() == TF_ERR_INVALID_KEY, "flow: a signed stop is the app's invalid_key (%s)", tf_error_name(tf_last_error_code()));
  snprintf(models, sizeof models, "%s/models", w);
  CHECK(!tf_file_exists(models) || tf__dir_bytes(models) == 0, "flow: after a signed stop no model file is on the device");
  {
    char *cj = NULL;
    size_t cn = 0;
    char p[1200];
    snprintf(p, sizeof p, "%s/client.json", w);
    CHECK(!tf_read_file(p, &cj, &cn) && strstr(cj, "\"refused\":\"key_revoked\""), "flow: the stop is remembered on the device");
    free(cj);
  }
  if (c) tf_client_destroy(c);

  /* an unsigned stop on the lease route is not a stop (18.4.10): nothing is removed or remembered */
  fake_reset(LEASE_UNSIGNED_STOP);
  snprintf(w, sizeof w, "%s/unsigned", work);
  c = flow_client(w);
  m = c ? tf_client_model(c, NULL) : NULL;
  CHECK(!m && tf_last_error_code() != TF_ERR_INVALID_KEY, "flow: an unsigned stop on the lease route is a failed request (%s)",
        tf_error_name(tf_last_error_code()));
  {
    char *cj = NULL;
    size_t cn = 0;
    char p[1200];
    snprintf(p, sizeof p, "%s/client.json", w);
    CHECK(!tf_read_file(p, &cj, &cn) && strstr(cj, "\"refused\":\"\""), "flow: an unsigned stop is not remembered");
    free(cj);
  }
  if (c) tf_client_destroy(c);

  /* install_cert_invalid: attest again (a new certificate), the lease asked once more */
  fake_reset(LEASE_CERT_INVALID_ONCE);
  snprintf(w, sizeof w, "%s/cert", work);
  c = flow_client(w);
  m = c ? tf_client_model(c, NULL) : NULL;
  CHECK(g_f.attest_keys == 2 && g_f.leases == 2 && tf_last_error_code() == TF_ERR_MODEL_INCOMPATIBLE,
        "flow: install_cert_invalid attests again and repeats once (attests %d, leases %d, %s)", g_f.attest_keys, g_f.leases,
        tf_error_name(tf_last_error_code()));
  if (c) tf_client_destroy(c);

  /* challenge_invalid: a new challenge, the lease asked once more */
  fake_reset(LEASE_CHALLENGE_INVALID_ONCE);
  snprintf(w, sizeof w, "%s/challenge", work);
  c = flow_client(w);
  m = c ? tf_client_model(c, NULL) : NULL;
  CHECK(g_f.challenges == 2 && g_f.leases == 2 && tf_last_error_code() == TF_ERR_MODEL_INCOMPATIBLE,
        "flow: challenge_invalid asks a new challenge and repeats once (challenges %d, leases %d)", g_f.challenges, g_f.leases);
  if (c) tf_client_destroy(c);

  /* the lease route off (404 not_found): no model, model_unavailable */
  fake_reset(LEASE_ROUTE_OFF);
  snprintf(w, sizeof w, "%s/off", work);
  c = flow_client(w);
  m = c ? tf_client_model(c, NULL) : NULL;
  CHECK(!m && tf_last_error_code() == TF_ERR_MODEL_UNAVAILABLE, "flow: the route off is model_unavailable (%s)", tf_error_name(tf_last_error_code()));
  if (c) tf_client_destroy(c);

  /* 4.0.1, production's shape: the lease answer names the build's folder on the model host. The files come from it
   * while the lease is asked (the stand-in's lease answer waits for them), each once, and are installed after it */
  {
    fake_reset(LEASE_HOST_BASE);
    snprintf(w, sizeof w, "%s/hostbase", work);
    c = flow_client(w);
    m = c ? tf_client_model(c, NULL) : NULL;
    CHECK(!m && tf_last_error_code() == TF_ERR_MODEL_INCOMPATIBLE, "flow, host folder: the vector build opens as far as its vocabulary (%s: %s)",
          tf_error_name(tf_last_error_code()), tf_last_error());
    CHECK(g_f.leases == 1 && g_f.files == 8 && g_f.files_host == 8 && g_f.files_at_lease == 8,
          "flow, host folder: the eight files once each, all asked before the lease answer came (%d, %d at the lease)", g_f.files, g_f.files_at_lease);
    char md[1200], p[1400];
    snprintf(md, sizeof md, "%s/models/%s", w, S(fx, "build"));
    snprintf(p, sizeof p, "%s/dev/layers.zfw.qld", md);
    const int dev = tf_file_exists(p);
    snprintf(p, sizeof p, "%s/cdn/layers.zfw.qle", md);
    CHECK(dev && !tf_file_exists(p), "flow, host folder: installed by the Core after the lease, no CDN form left");
    if (c) tf_client_destroy(c);
  }

  /* a lease answer without a model folder, and one whose folder is off Quran Lab's hosts: the lease is taken, no file
   * is installed and none is kept (4.0.1: what came from the host's folder while the lease was asked is removed; never
   * a file from the answer's other host), and the call fails as an invalid response */
  for (int mode = LEASE_NO_BASE; mode <= LEASE_OFF_BASE; ++mode) {
    fake_reset(mode);
    snprintf(w, sizeof w, "%s/%s", work, mode == LEASE_NO_BASE ? "nobase" : "offbase");
    c = flow_client(w);
    m = c ? tf_client_model(c, NULL) : NULL;
    /* 4.0.1 (28.4): a base off the allow-list is no lease answer (unreachable on that host, so the same request goes to
     * each api host in turn: download_failed when no host gives one); an answer without a base is accepted on the first
     * host. Either way nothing is installed or kept: the files fetched beside the lease are removed */
    const int want_code = mode == LEASE_NO_BASE ? TF_ERR_SERVER_REJECTED : TF_ERR_DOWNLOAD_FAILED;
    const int want_leases = mode == LEASE_NO_BASE ? 1 : N_API_HOSTS;
    char md[1200], p[1400];
    snprintf(md, sizeof md, "%s/models/%s", w, S(fx, "build"));
    int kept = 0;
    static const char *const left[] = {"cdn/layers.zfw.qle", "cdn/layers.zfw.qle.part", "cdn/tokens.txt.qle", "dev/layers.zfw.qld", "calibration.json"};
    for (size_t k = 0; k < sizeof left / sizeof left[0]; ++k) {
      snprintf(p, sizeof p, "%s/%s", md, left[k]);
      kept += tf_file_exists(p);
    }
    CHECK(!m && tf_last_error_code() == want_code && strstr(tf_last_error(), "invalid_response") && g_f.leases == want_leases && !g_f.files_base &&
              !kept,
          "flow: a lease answer %s installs and keeps nothing (%s: %s; leases %d, from its folder %d, %d files kept)",
          mode == LEASE_NO_BASE ? "without a base" : "with a base off quranlab.ai", tf_error_name(tf_last_error_code()), tf_last_error(), g_f.leases,
          g_f.files_base, kept);
    if (c) tf_client_destroy(c);
  }

  /* the 4.0 config (qlab-api sdk4-server): a build without a key id is not one for 4.0, nothing is fetched */
  fake_reset(LEASE_OK);
  g_f.cfg = "config_nokid";
  snprintf(w, sizeof w, "%s/nokid", work);
  c = flow_client(w);
  m = c ? tf_client_model(c, NULL) : NULL;
  CHECK(!m && tf_last_error_code() == TF_ERR_MODEL_UNAVAILABLE && !g_f.manifest_host && !g_f.leases,
        "flow: a config without the build's key id names no build for 4.0 (%s, manifests %d, leases %d)", tf_error_name(tf_last_error_code()),
        g_f.manifest_host, g_f.leases);
  if (c) tf_client_destroy(c);
  /* another key id than the signed manifest's: refused before the lease route is asked */
  fake_reset(LEASE_OK);
  g_f.cfg = "config_badkid";
  snprintf(w, sizeof w, "%s/badkid", work);
  c = flow_client(w);
  m = c ? tf_client_model(c, NULL) : NULL;
  CHECK(!m && tf_last_error_code() == TF_ERR_MODEL_INVALID && g_f.manifest_host == 1 && !g_f.challenges && !g_f.leases && !g_f.files,
        "flow: a manifest whose key id is not the config's is refused before the lease (%s: %s; leases %d, files %d)",
        tf_error_name(tf_last_error_code()), tf_last_error(), g_f.leases, g_f.files);
  if (c) tf_client_destroy(c);

  /* the data piece (18.6.7, spec fix 16; piece.json): each answer's piece goes to the Core right after the accept,
   * and the Core keeps it pending until the next model open. The vector build never opens a model (no real model),
   * so every ask stays under no piece however many pieces were handed, the bad one included. The four sequences of
   * piece_order.json, with real opens between the leases, run in `tf40_tests piece_order` on the encrypted test build.
   * The refreshes run on the Client's timer (tf__store_tick) with the Core's clocks moved past the lease's refresh
   * time: the same lease is answered each time, so each tick is one refresh. */
  fake_reset(LEASE_PIECE);
  snprintf(w, sizeof w, "%s/piece", work);
  const long long iat = (long long)tf_json_get(tf_json_get(fx, "lease"), "iat")->num;
  set_clock(iat + 60, 5000000, "boot-piece");
  c = flow_client(w);
  m = c ? tf_client_model(c, NULL) : NULL;
  CHECK(g_f.leases == 1 && g_f.n_named == 1 && g_f.named[0] == 0, "piece: the first lease asks under no piece (%d)", g_f.named[0]);
  set_clock(iat + 13 * 3600, 5000000 + (13 * 3600 - 60) * 1000LL, "boot-piece");
  for (int t = 0; t < 4 && c; ++t) tf__store_tick(c); /* leases 2 to 5: pieces 8, 8, 8 and the bad one handed */
  int any = 0;
  for (int i = 0; i < g_f.n_named; ++i) any |= g_f.named[i] != 0;
  CHECK(g_f.leases == 5 && !any, "piece: without a model open no handed piece comes into force (leases %d, every ask under none: %s)",
        g_f.leases, any ? "no" : "yes");
  {
    qlc_lease_status ls;
    tf__pm_status(c ? tf__pm(c) : NULL, &ls);
    CHECK(ls.valid && !ls.stopped, "piece: the refreshed lease is in force");
    char *tb = c ? tf__client_day_body(c) : NULL;
    CHECK(tb && strstr(tb, "\"lease\":{\"ok\":5}"), "piece: five lease requests made and accepted (17.8 lease): %s",
          tb && strstr(tb, "\"lease\"") ? strstr(tb, "\"lease\"") : "(none)");
    free(tb);
  }
  if (c) tf_client_destroy(c);
  /* an installed app whose launch's first config answer refuses (a key revoked, an app blocked) while the next would
   * serve: the app gets the refusal's own error (invalid_key, server_rejected). Stage B in production (11-18) saw a
   * launch whose model call then said download_failed "network calls are stopped for this launch" after a 401 and a
   * 304 from two isolates in the minute after the change; this stand-in does not reproduce that interleaving yet */
  {
    const char *bodies[2] = {"{\"error\":\"invalid_token\",\"message\":\"token is invalid or expired\",\"reason\":\"key_revoked\"}",
                             "{\"error\":\"app_blocked\",\"message\":\"this app is blocked\",\"reason\":\"app_blocked\"}"};
    const int statuses[2] = {401, 403}, want[2] = {TF_ERR_INVALID_KEY, TF_ERR_SERVER_REJECTED};
    for (int k = 0; k < 2; ++k) {
      snprintf(w, sizeof w, "%s/oncerefused%d", work, k);
      /* an install with its model, its lease and a stored config (the next config answer is 304) */
      fake_reset(LEASE_OK);
      TfClient *c0 = flow_client(w);
      TfModel *m0 = c0 ? tf_client_model(c0, NULL) : NULL;
      /* (the vector build is installed, then refused at open for its vocabulary, as in step 1) */
      CHECK(!m0 && tf_last_error_code() == TF_ERR_MODEL_INCOMPATIBLE, "a config refused once: the install first (%s)", tf_error_name(tf_last_error_code()));
      if (m0) tf_model_close(m0);
      if (c0) tf_client_destroy(c0);
      fake_reset(LEASE_OK);
      g_f.fail_url = "/v1/config", g_f.fail_status = statuses[k], g_f.fail_body = bodies[k], g_f.fail_times = 1;
      TfClient *c = flow_client_in(w);
      TfModel *m = c ? tf_client_model(c, NULL) : NULL;
      const int got = tf_last_error_code();
      CHECK(!m && got == want[k], "a config refused once (%d) while the next answer serves: the app gets %s, want %s (%s)", statuses[k], tf_error_name(got),
            tf_error_name(want[k]), tf_last_error() ? tf_last_error() : "");
      if (m) tf_model_close(m);
      g_f.fail_url = NULL;
      if (c) tf_client_destroy(c);
    }
  }
  tf__plat_test_wall_ms = tf__plat_test_boot_ms = -1;
  tf__plat_test_boot_id = NULL;
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(fx);
  return done("flow");
}

/* ------------------------------------------------------------------ the staging clock case */
/* The staging run of 11-15 (qlab-staging evidence/c/clock.jsonl, its driver's clock mode): one boot id, the boot clock
 * moved by hand past lease 2's exp, offline, then online again 3 hours past it: the server answered /v1/model-key with
 * 200 and a new lease, and the model still failed with needs_connection. The new lease's iat and exp were the staging
 * server's real time (iat 1794781610, exp 1794867118), while the Core's clock, anchored at lease 1's iat and moved
 * only by the boot clock on that boot (18.4.7: now = last_now + the boot clock, never below floor), stood 3 hours past
 * lease 2's exp (1794877915): a lease is held to max(floor, iat), which was past its exp, so the Core refused it as
 * expired. The driver moved the device's clock and not the server's; on a real device the boot clock never runs ahead
 * of real time, so floor never passes the server's time and a lease answered after an expiry anchors at its own iat.
 * Both here, on one boot, through the SDK and the test Core (step 1 through tf_client_model; steps 2 to 4 through the
 * Client's lease host, tf__pm_lease, which every model open calls: with the vector build on the device the SDK refuses
 * its three-token vocabulary before it would ask for a lease):
 *   1. a fresh install online: lease 1, the Core's clock at its iat;
 *   2. offline, the boot clock 2 h past exp: no lease request answered; with the grace of 18.4.7.1 (Core f006676)
 *      the attempt is unreachable and the open goes on in the lease's grace (it was needs_connection before);
 *   3. online 3 h past exp, the server's clock where it was (lease 1 answered again, as staging's was 3 h behind the
 *      driver's): 200, refused by the Core as expired: the staging case, reproduced. A 2xx whose lease the Core
 *      refuses is unreachable for the grace, so the open still goes on in it (staging's run had no grace yet);
 *   4. the same moment, the server's clock moved on with the device's (the next lease, issued 30 h after the first):
 *      accepted, the Core's clock re-anchored at its iat, the lease valid. */
static int t_lease_clock(const char *fixture, const char *work) {
  TfJson *fx = flow_setup(fixture);
  if (!fx) return 2;
  const TfJson *l1 = tf_json_get(fx, "lease"), *l2 = tf_json_get(fx, "lease_later");
  CHECK(l2 != NULL, "lease_clock: the fixture has lease_later (make_flow40.py)");
  const long long iat = N(l1, "iat"), exp = N(l1, "exp"), iat2 = N(l2, "iat"), exp2 = N(l2, "exp");
  const long long b0 = 3600000; /* an hour after this boot */
  const char *mt = S(fx, "manifest"), *build = S(fx, "build");
  TfJson *mj = tf_json_parse(mt, strlen(mt), NULL);
  char w[1100];
  snprintf(w, sizeof w, "%s/clock", work);
  qlc_lease_status ls;
  /* 1 */
  fake_reset(LEASE_OK);
  set_clock(iat + 60, b0, "boot-staging-clock");
  TfClient *c = flow_client(w);
  TfModel *m = c ? tf_client_model(c, NULL) : NULL;
  CHECK(!m && tf_last_error_code() == TF_ERR_MODEL_INCOMPATIBLE && g_f.leases == 1, "lease_clock 1: lease 1 accepted, the open reached (%s, leases %d)",
        tf_error_name(tf_last_error_code()), g_f.leases);
  tf__pm_status(c ? tf__pm(c) : NULL, &ls);
  CHECK(ls.valid && ls.now == iat && ls.exp == exp, "lease_clock 1: the Core's clock at lease 1's iat (now %lld, iat %lld, exp %lld)", (long long)ls.now, iat,
        (long long)ls.exp);
  if (c) tf_client_destroy(c);
  /* 2 */
  fake_reset(LEASE_OK);
  g_f.fail_url = "https://"; /* no answer from anything */
  tf__plat_test_boot_ms = b0 + (exp - iat + 7200) * 1000;
  c = flow_client_in(w);
  int rc = c ? tf__pm_lease(c, tf__pm(c), mt, strlen(mt), mj, build) : -1;
  CHECK(rc == 0 && !g_f.leases && tf__pm_grace(tf__pm(c), NULL), "lease_clock 2: offline 2 h past exp, the open goes on in the grace (%s)",
        rc ? tf_error_name(rc) : "ok");
  tf__pm_status(c ? tf__pm(c) : NULL, &ls);
  CHECK(!ls.valid && ls.now == exp + 7200, "lease_clock 2: the Core's clock 2 h past exp (now - exp %lld)", (long long)(ls.now - exp));
  {
    /* DIAGNOSIS_40 (section 25): in the grace, its end exp + 86400, and the expired lease's exp */
    char want[96];
    const char *dj2 = c ? tf_client_diagnose_json(c) : NULL;
    const char *pj2 = dj2 ? strstr(dj2, "\"protected\"") : NULL;
    snprintf(want, sizeof want, "\"graceUntil\":%lld", (long long)(exp + 86400));
    CHECK(pj2 && strstr(pj2, "\"lease\":\"grace\"") && strstr(pj2, want), "lease_clock 2: the diagnosis says grace until exp + 86400: %s",
          pj2 ? pj2 : "(none)");
  }
  if (c) tf_client_destroy(c);
  /* 3 */
  fake_reset(LEASE_OK);
  tf__plat_test_boot_ms = b0 + (exp - iat + 10800) * 1000;
  lease_row_passed(w); /* an hour on: the retry time step 2 kept (18.4.7.3, 600 to 1800 s) has passed */
  c = flow_client_in(w);
  rc = c ? tf__pm_lease(c, tf__pm(c), mt, strlen(mt), mj, build) : -1;
  /* 4.0.1 (ENDPOINTS_40.md 28.5): a lease the Core refuses is that host unreachable, so the same request goes to each
   * api host in turn; every one gives the same expired lease, the attempt ends unreachable, and the open is in the grace */
  CHECK(rc == 0 && g_f.leases == N_API_HOSTS && tf__pm_grace(tf__pm(c), NULL),
        "lease_clock 3: the server's clock 3 h behind the device's: its lease answered 200 and refused as expired on every api host, the open in the grace (%s, leases %d)",
        rc ? tf_error_name(rc) : "ok", g_f.leases);
  tf__pm_status(c ? tf__pm(c) : NULL, &ls);
  CHECK(!ls.valid && ls.now == exp + 10800 && ls.exp == exp, "lease_clock 3: nothing moved (now - exp %lld, the lease held is lease 1's)",
        (long long)(ls.now - exp));
  {
    char *tb = c ? tf__client_day_body(c) : NULL;
    CHECK(tb && strstr(tb, "\"expired\":1"), "lease_clock 3: the request counted with cause expired (17.8 lease): %s",
          tb && strstr(tb, "\"lease\"") ? strstr(tb, "\"lease\"") : "(none)");
    free(tb);
  }
  if (c) tf_client_destroy(c);
  /* 4 */
  fake_reset(LEASE_LATER);
  lease_row_passed(w);
  c = flow_client_in(w);
  rc = c ? tf__pm_lease(c, tf__pm(c), mt, strlen(mt), mj, build) : -1;
  CHECK(rc == 0 && g_f.leases == 1, "lease_clock 4: the server's clock with the device's: the next lease accepted (%s, leases %d)", rc ? tf_error_name(rc) : "ok",
        g_f.leases);
  tf__pm_status(c ? tf__pm(c) : NULL, &ls);
  CHECK(ls.valid && ls.now == iat2 && ls.exp == exp2, "lease_clock 4: the Core's clock re-anchored at the new lease's iat (now %lld, iat %lld, exp %lld)",
        (long long)ls.now, iat2, (long long)ls.exp);
  if (c) tf_client_destroy(c);
  tf__plat_test_wall_ms = tf__plat_test_boot_ms = -1;
  tf__plat_test_boot_id = NULL;
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(mj);
  tf_json_free(fx);
  return done("lease_clock");
}

/* ------------------------------------------------------------------ grace (18.4.7.1) */
/* Grace when Quran Lab cannot be reached, through this SDK and the Core:
 *   outcomes: each of grace.json's 37 attempts, on a fresh install whose lease expired 2 hours ago: the stand-in gives
 *     the case's answers in order to the requests of their routes, and tf__pm_lease (what every model open calls)
 *     must end as the case says: lease (a valid lease), stop (invalid_key, the Core's lease gone), refused (no grace:
 *     the call fails) or unreachable (the open goes on in the grace);
 *   then on one install, on the Core's clock: a 503 lease_unavailable with Retry-After 30 opens in grace with until =
 *     exp + 86400, the next attempt not before 30 s; a second open within a minute asks nothing; a new process
 *     offline keeps the grace; the end at exp + 86400; a refusal ends it for good (an unreachable attempt after it
 *     gives none back); a new lease clears it; the background refresh takes the Retry-After as its next try; and a
 *     7-day lease of this unattested desktop install (the server's lease_grace capped_7d) is held to its own exp. */
/* another thread's API call that holds the client's lock while the stand-in takes slow_ms to answer it */
static void busy_call(void *arg) {
  TfHttpResponse r;
  memset(&r, 0, sizeof r);
  tf__client_call((TfClient *)arg, "GET", "/v1/slow-test", NULL, 0, NULL, &r);
  tf_http_response_free(&r);
}
/* another thread's model open past exp, whose lease attempt waits slow_ms for its model-key answer */
static struct {
  TfClient *c;
  TfPm *p;
  const char *mt, *build;
  const TfJson *mj;
} g_busy_args;
static void busy_lease(void *arg) {
  (void)arg;
  tf__pm_lease(g_busy_args.c, g_busy_args.p, g_busy_args.mt, strlen(g_busy_args.mt), g_busy_args.mj, g_busy_args.build);
  tf__clear_error();
}
static TfClient *grace_install(const char *w, long long iat, long long b0) {
  fake_reset(LEASE_OK);
  set_clock(iat + 60, b0, "boot-grace");
  TfClient *c = flow_client(w);
  if (c) tf_client_model(c, NULL); /* lease 1 accepted (the vector build's vocabulary is refused after it) */
  if (c) tf_client_destroy(c);
  return c;
}
static int t_grace(const char *fixture, const char *vec, const char *work) {
  TfJson *fx = flow_setup(fixture), *gv = load_json(vec, "grace.json");
  if (!fx || !gv) return 2;
  const TfJson *l1 = tf_json_get(fx, "lease"), *l7 = tf_json_get(fx, "lease_7d"), *later = tf_json_get(fx, "lease_later");
  CHECK(tf_json_get(fx, "lease_bad") && tf_json_get(fx, "refusal_signed_grace") && l7 && later, "grace: the fixture has the grace data (make_flow40.py)");
  const long long iat = N(l1, "iat"), exp = N(l1, "exp");
  const long long b0 = 3600000, t0 = exp + 7200; /* 2 h past exp */
  const char *mt = S(fx, "manifest"), *build = S(fx, "build");
  TfJson *mj = tf_json_parse(mt, strlen(mt), NULL);
  char w[1100];
  qlc_lease_status ls;
  long long until = 0;
  /* the outcomes */
  const TfJson *oc = tf_json_get(gv, "outcomes");
  int n = 0, ok = 0;
  tf__lease_wait_s = 1; /* `late`: no outcome within the open's wait (10 s in use; 1 s here) */
  for (int i = 0; oc && i < oc->n; ++i) {
    const TfJson *cs = &oc->kids[i];
    snprintf(w, sizeof w, "%s/outcome%02d", work, i);
    grace_install(w, iat, b0);
    tf__plat_test_boot_ms = b0 + (t0 - iat) * 1000;
    TfClient *c = flow_client_in(w);
    fake_reset(LEASE_OK);
    g_f.script = tf_json_get(cs, "answers");
    const int rc = c ? tf__pm_lease(c, tf__pm(c), mt, strlen(mt), mj, build) : -1;
    tf__pm_status(c ? tf__pm(c) : NULL, &ls);
    const int grace = c ? tf__pm_grace(tf__pm(c), NULL) : 0;
    const char *want = S(cs, "outcome");
    int good;
    if (!strcmp(want, "lease")) good = rc == 0 && ls.valid && !grace;
    else if (!strcmp(want, "stop")) good = rc == TF_ERR_INVALID_KEY && !ls.has_lease && !grace;
    else if (!strcmp(want, "refused")) good = rc != 0 && !grace;
    else good = rc == 0 && grace && !ls.valid; /* unreachable */
    n++;
    ok += good;
    CHECK(good, "grace outcome %d (%s): want %s, got %s (valid %d, grace %d, answers given %d of %d)", i, S(cs, "name"), want, rc ? tf_error_name(rc) : "ok",
          ls.valid, grace, g_f.at, g_f.script ? g_f.script->n : 0);
    g_f.script = NULL;
    if (c) tf_client_destroy(c);
  }
  printf("  grace.json outcomes: %d of %d as the contract says\n", ok, n);
  tf__lease_wait_s = 10;
  /* one install on the Core's clock */
  snprintf(w, sizeof w, "%s/clock", work);
  grace_install(w, iat, b0);
  tf__plat_test_boot_ms = b0 + (t0 - iat) * 1000;
  TfClient *c = flow_client_in(w);
  TfPm *p = c ? tf__pm(c) : NULL;
  fake_reset(LEASE_OK);
  g_f.fail_url = "/v1/model-key";
  g_f.fail_status = 503;
  g_f.fail_body = "{\"error\":\"lease_unavailable\",\"message\":\"the lease service is unavailable\"}";
  g_f.retry_after = 30;
  const long long before = (long long)time(NULL);
  int rc = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
  CHECK(rc == 0 && tf__pm_grace(p, &until) && until == exp + 86400 && g_f.leases == 0,
        "grace: 503 lease_unavailable past exp opens in grace (%s, until - exp %lld)", rc ? tf_error_name(rc) : "ok", until - exp);
  /* 18.4.7.3 (4.0.2): the wait is drawn from 600 to 1800 s; the Retry-After of 30 s is a floor below it (4.0.1: 30 s) */
  CHECK(tf__pm_next_try(p) >= before + 600 && tf__pm_next_try(p) <= (long long)time(NULL) + 1800,
        "grace: the next attempt 600 to 1800 s on, not the Retry-After's 30 (in %lld s)", tf__pm_next_try(p) - (long long)time(NULL));
  {
    const char *dj = tf_client_diagnose_json(c);
    CHECK(dj && strstr(dj, "\"lease\":\"grace\""), "grace: the diagnosis names the grace");
  }
  const int ch = g_f.challenges;
  rc = tf__pm_lease(c, p, mt, strlen(mt), mj, build);
  CHECK(rc == 0 && g_f.challenges == ch, "grace: a second open within a minute asks nothing (%s, challenges %d then %d)", rc ? tf_error_name(rc) : "ok", ch,
        g_f.challenges);
  if (c) tf_client_destroy(c);
  /* a new process, offline, 20 h past exp: still in grace */
  fake_reset(LEASE_OK);
  g_f.fail_url = "https://";
  tf__plat_test_boot_ms = b0 + (exp + 20 * 3600 - iat) * 1000;
  c = flow_client_in(w);
  p = c ? tf__pm(c) : NULL;
  rc = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
  CHECK(rc == 0 && tf__pm_grace(p, NULL), "grace: a new process offline 20 h past exp keeps the grace (%s)", rc ? tf_error_name(rc) : "ok");
  if (c) tf_client_destroy(c);
  /* the end: exp + 86400 */
  tf__plat_test_boot_ms = b0 + (exp + 86400 - iat) * 1000;
  c = flow_client_in(w);
  p = c ? tf__pm(c) : NULL;
  rc = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
  tf__pm_status(p, &ls);
  CHECK(rc == TF_ERR_NEEDS_CONNECTION && !tf__pm_grace(p, NULL), "grace: none at exp + 86400 (%s)", tf_error_name(rc));
  if (c) tf_client_destroy(c);
  /* past the grace, a second open within the minute reuses the first attempt's outcome, and now its cause too (Stage B,
   * production, 11-18: the reused one was reported as download, other, and a 503's reason lease_unavailable was kept in
   * 16 bytes, so a grace open's report named it lease_unavailab): both opens fail download_failed with the cause lease,
   * lease_unavailable, the second with no request */
  {
    snprintf(w, sizeof w, "%s/reuse", work);
    grace_install(w, iat, b0);
    tf__plat_test_boot_ms = b0 + (exp + 86400 + 600 - iat) * 1000;
    c = flow_client_in(w);
    p = c ? tf__pm(c) : NULL;
    fake_reset(LEASE_OK);
    g_f.fail_url = "/v1/model-key";
    g_f.fail_status = 503;
    g_f.fail_body = "{\"error\":\"lease_unavailable\"}";
    g_f.retry_after = 30;
    const int rc1 = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
    char why1[40];
    snprintf(why1, sizeof why1, "%s", tf__cause_reason());
    const int asked = g_f.challenges;
    tf__clear_error();
    const int rc2 = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
    char why2[40];
    snprintf(why2, sizeof why2, "%s", tf__cause_reason());
    CHECK(rc1 == TF_ERR_DOWNLOAD_FAILED && rc2 == TF_ERR_DOWNLOAD_FAILED && !strcmp(why1, "lease_unavailable") && !strcmp(why2, "lease_unavailable") &&
              g_f.challenges == asked && !tf__pm_grace(p, NULL),
          "grace: past it, a reused 503 outcome keeps its cause (%s %s, %s %s, requests %d then %d)", tf_error_name(rc1), why1, tf_error_name(rc2), why2,
          asked, g_f.challenges);
    if (c) tf_client_destroy(c);
  }
  /* a refusal ends it for good: another install, 503 then a 403 sdk_too_old, then 503 again */
  snprintf(w, sizeof w, "%s/refused", work);
  grace_install(w, iat, b0);
  tf__plat_test_boot_ms = b0 + (t0 - iat) * 1000;
  c = flow_client_in(w);
  p = c ? tf__pm(c) : NULL;
  fake_reset(LEASE_OK);
  g_f.fail_url = "/v1/model-key";
  g_f.fail_status = 503;
  rc = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
  CHECK(rc == 0 && tf__pm_grace(p, NULL), "grace: an unreachable attempt opens it");
  tf__lease_reuse_s = 0; /* every open asks */
  /* 18.4.7.3 (4.0.2): an open in the grace waits for the row's next attempt; these opens come after it */
  tf__now_override = tf__pm_next_try(p);
  g_f.fail_status = 403;
  g_f.fail_body = "{\"error\":\"sdk_too_old\",\"min_sdk\":\"4.1.0\"}";
  rc = tf__pm_lease(c, p, mt, strlen(mt), mj, build);
  CHECK(rc == TF_ERR_SDK_TOO_OLD && !tf__pm_grace(p, NULL), "grace: a refusal ends it (%s)", tf_error_name(rc));
  g_f.fail_status = 503;
  g_f.fail_body = NULL;
  tf__now_override = tf__pm_next_try(p) > tf__now_override ? tf__pm_next_try(p) : tf__now_override;
  rc = tf__pm_lease(c, p, mt, strlen(mt), mj, build);
  CHECK(rc == TF_ERR_DOWNLOAD_FAILED && !tf__pm_grace(p, NULL), "grace: an unreachable attempt after a refusal gives none back (%s)", tf_error_name(rc));
  /* a new lease clears it all */
  fake_reset(LEASE_LATER);
  rc = tf__pm_lease(c, p, mt, strlen(mt), mj, build);
  tf__pm_status(p, &ls);
  CHECK(rc == 0 && ls.valid && ls.exp == N(later, "exp") && !tf__pm_grace(p, NULL) && tf__pm_fails(p) == 0,
        "grace: a new lease clears it, and the row (%s)", rc ? tf_error_name(rc) : "ok");
  tf__lease_reuse_s = 60;
  tf__now_override = 0;
  /* the background refresh: lease_later past its refresh_after, still valid; a 503 with Retry-After 30 sets the next try */
  tf__plat_test_boot_ms = b0 + (t0 - iat) * 1000 + (N(later, "refresh_after") - N(later, "iat") + 60) * 1000;
  fake_reset(LEASE_OK);
  g_f.fail_url = "/v1/model-key";
  g_f.fail_status = 503;
  g_f.fail_body = "{\"error\":\"lease_unavailable\"}";
  g_f.retry_after = 30;
  tf__store_tick(c);
  tf__pm_status(p, &ls);
  CHECK(g_f.leases == 0 && ls.valid && ls.refresh_due && tf__pm_next_try(p) >= (long long)time(NULL) + 599 && tf__pm_next_try(p) <= (long long)time(NULL) + 1800,
        "grace: the background refresh's 503 lease_unavailable waits 600 to 1800 s, its Retry-After of 30 a floor (next try in %lld s, valid %d)",
        tf__pm_next_try(p) - (long long)time(NULL), ls.valid);
  if (c) tf_client_destroy(c);
  /* a 7-day lease of this unattested desktop install (lease_grace capped_7d): valid 3 days on, as its exp says */
  snprintf(w, sizeof w, "%s/seven", work);
  fake_reset(LEASE_OK);
  set_clock(iat + 60, b0, "boot-seven");
  {
    /* the first lease answer is the 7-day one */
    c = flow_client(w);
    p = c ? tf__pm(c) : NULL;
    TfBuf o;
    tf_buf_init(&o);
    tf_json_write(&o, l7);
    g_f.fail_url = "/v1/model-key";
    g_f.fail_status = 200;
    g_f.fail_body = o.p;
    rc = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
    tf__pm_status(p, &ls);
    CHECK(rc == 0 && ls.valid && ls.exp == N(l7, "exp"), "grace: a 7-day lease is accepted for an unattested install (exp - iat %lld s)", (long long)(ls.exp - iat));
    tf__plat_test_boot_ms = b0 + 3 * 86400 * 1000LL;
    g_f.fail_url = "https://"; /* offline */
    const int lk = g_f.leases;
    rc = tf__pm_lease(c, p, mt, strlen(mt), mj, build);
    tf__pm_status(p, &ls);
    CHECK(rc == 0 && ls.valid && !tf__pm_grace(p, NULL) && g_f.leases == lk, "grace: 3 days on it is still valid, offline, no request (%s)",
          rc ? tf_error_name(rc) : "ok");
    tf_buf_free(&o);
    if (c) tf_client_destroy(c);
  }
  /* an open past exp while another thread's API call is in flight (the background refresh, a config refresh: each holds
   * the client's lock for its request, up to 30 s): the open still waits at most its wait (Stage B, staging, 11-18: 32 s
   * behind a hanging lease request, where the contract says 10; 1 s here) and opens in the grace */
  {
    snprintf(w, sizeof w, "%s/busy", work);
    grace_install(w, iat, b0);
    tf__plat_test_boot_ms = b0 + (t0 - iat) * 1000;
    c = flow_client_in(w);
    p = c ? tf__pm(c) : NULL;
    fake_reset(LEASE_OK);
    g_f.slow_url = "/v1/slow-test";
    g_f.slow_ms = 4000;
    g_f.fail_url = "/v1/model-key";
    g_f.fail_status = 503;
    g_f.fail_body = "{\"error\":\"lease_unavailable\"}";
    tf__lease_wait_s = 1;
    TfThread th;
    const int started = c && !tf_thread_start(&th, busy_call, c);
    tf__sleep_ms(300); /* the other call holds the client's lock */
    const double s0 = tf__now_s();
    rc = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
    const double took = tf__now_s() - s0;
    printf("  busy: the open took %.2f s\n", took);
    CHECK(started && rc == 0 && tf__pm_grace(p, NULL) && took < 2.5,
          "grace: an open past exp behind another thread's call in flight waits at most its wait and opens in grace (%s, %.1f s, the wait 1 s, the other call 4 s)",
          rc ? tf_error_name(rc) : "ok", took);
    if (started) tf_thread_join(th);
    tf__lease_wait_s = 10;
    if (c) tf_client_destroy(c);
  }
  /* an open past exp while another thread's lease attempt is in flight (the background refresh at a launch; another
   * open): the open waits for it within its one wait, then counts it unreachable and opens in grace; the same open's
   * next call reuses that outcome at once (Stage B, staging, 11-18: 10 s, then 10 s more for its own attempt, then the
   * running one waited for again: 32 s) */
  {
    snprintf(w, sizeof w, "%s/busy2", work);
    grace_install(w, iat, b0);
    tf__plat_test_boot_ms = b0 + (t0 - iat) * 1000;
    c = flow_client_in(w);
    p = c ? tf__pm(c) : NULL;
    fake_reset(LEASE_OK);
    g_f.slow_url = "/v1/model-key";
    g_f.slow_ms = 4000;
    tf__lease_wait_s = 1;
    g_busy_args.c = c;
    g_busy_args.p = p;
    g_busy_args.mt = mt;
    g_busy_args.mj = mj;
    g_busy_args.build = build;
    TfThread th;
    const int started = c && !tf_thread_start(&th, busy_lease, &g_busy_args);
    tf__sleep_ms(300); /* the other thread's attempt is waiting for its model-key answer */
    double s0 = tf__now_s();
    rc = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
    const double took = tf__now_s() - s0;
    s0 = tf__now_s();
    const int rc2 = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
    const double took2 = tf__now_s() - s0;
    printf("  busy2: the open took %.2f s, its next call %.2f s\n", took, took2);
    CHECK(started && rc == 0 && rc2 == 0 && tf__pm_grace(p, NULL) && took < 2.5 && took2 < 0.5,
          "grace: an open past exp while another attempt is in flight opens in grace within its one wait, and its next call at once (%s %s, %.1f s, %.1f s)",
          rc ? tf_error_name(rc) : "ok", rc2 ? tf_error_name(rc2) : "ok", took, took2);
    if (started) tf_thread_join(th);
    tf__lease_wait_s = 10;
    if (c) tf_client_destroy(c);
  }
  /* a fresh install's lease request (no lease to fall back on): no answer at all is needs_connection (24.4 rule 1); an
   * answer that began and then stalled is download_failed (rule 2), reported as lease, timeout (Stage B, staging, 11-18:
   * it was needs_connection with the report download, network) */
  {
    int got[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
      snprintf(w, sizeof w, "%s/fresh%d", work, k);
      fake_reset(LEASE_OK);
      set_clock(iat + 60, b0, k ? "boot-fresh1" : "boot-fresh0");
      c = flow_client(w);
      p = c ? tf__pm(c) : NULL;
      if (k) g_f.stall_url = "/v1/model-key";
      else {
        g_f.fail_url = "/v1/model-key";
        g_f.fail_status = 0;
      }
      got[k] = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
      if (k) CHECK(!strcmp(tf__cause_reason(), "timeout"), "grace: the stalled lease answer's cause is timeout (%s)", tf__cause_reason());
      if (c) tf_client_destroy(c);
    }
    CHECK(got[0] == TF_ERR_NEEDS_CONNECTION && got[1] == TF_ERR_DOWNLOAD_FAILED,
          "grace: a fresh install's lease with no answer is needs_connection (%s), with an answer that stalled download_failed (%s)",
          tf_error_name(got[0]), tf_error_name(got[1]));
  }
  /* a lease answer of 403 that is not the API's (an HTML page: a proxy, a firewall, a captive portal) is http, never a
   * refusal: a fresh install gets download_failed with the cause lease, http; the API's own JSON 403 stays a refusal,
   * server_rejected with the cause refused (24.3, 24.4; Stage B, staging, 11-18: the HTML 403 read refused) */
  {
    int got[2] = {0, 0};
    char why[2][24] = {"", ""};
    for (int k = 0; k < 2; ++k) {
      snprintf(w, sizeof w, "%s/html403_%d", work, k);
      fake_reset(LEASE_OK);
      set_clock(iat + 60, b0, k ? "boot-json403" : "boot-html403");
      c = flow_client(w);
      p = c ? tf__pm(c) : NULL;
      g_f.fail_url = "/v1/model-key";
      g_f.fail_status = 403;
      g_f.fail_body = k ? "{\"error\":\"app_not_allowed\",\"message\":\"not for this app\"}" : "<html><body>a page that is not the API's</body></html>";
      got[k] = c ? tf__pm_lease(c, p, mt, strlen(mt), mj, build) : -1;
      snprintf(why[k], sizeof why[k], "%s", tf__cause_reason());
      if (c) tf_client_destroy(c);
    }
    CHECK(got[0] == TF_ERR_DOWNLOAD_FAILED && !strcmp(why[0], "http") && got[1] == TF_ERR_SERVER_REJECTED && !strcmp(why[1], "refused"),
          "grace: a lease 403 with an HTML page is download_failed, cause http (%s, %s); the API's JSON 403 server_rejected, cause refused (%s, %s)",
          tf_error_name(got[0]), why[0], tf_error_name(got[1]), why[1]);
  }
  tf__plat_test_wall_ms = tf__plat_test_boot_ms = -1;
  tf__plat_test_boot_id = NULL;
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(mj);
  tf_json_free(gv);
  tf_json_free(fx);
  return done("grace");
}

/* grace.json's override_leases (18.4.7.2: the leases of the server's lease_grace switch, a 7-day expiry on an
 * unattested install among them) and its 15 sequences (18.4.7.1), through this SDK's key store, clocks and saved state
 * and the Core: qlc_lease_accept, qlc_lease_attempt, qlc_refusal_accept, a restart (the state saved and handed back to a
 * new Core), and at each check the Core's now, qlc_lease_grace's until and the state, which a model open must agree
 * with (QLC_E_NEEDS_CONNECTION exactly when the state is needs_connection). The sequences' leases and stops are made by
 * test/make_grace40.py for lease.json's ECDH install. */
static const char *grace_state(Host *h, qlc_lease_status *ls, long long *until, int *open_st) {
  int64_t u = 0;
  tf__pm_status(h->p, ls);
  const int g = qlc_lease_grace(tf__pm_core(h->p), &u) == 1;
  *until = (long long)u;
  qlc_open_options o = qlc_open_default_options();
  int st = 0;
  qlc_model *m = qlc_model_open(tf__pm_core(h->p), h->manifest, h->mn, &o, &st);
  if (m) qlc_model_close(m);
  *open_st = m ? QLC_OK : st;
  return ls->stopped ? "stopped" : ls->valid ? "valid" : g ? "grace" : "needs_connection";
}
static int t_grace_vectors(const char *vec, const char *fixture, const char *work) {
  TfJson *gv = load_json(vec, "grace.json"), *gs = load_json(fixture, "grace_seq.json"), *lj = load_json(vec, "lease.json");
  if (!gv || !gs || !lj) return 2;
  tf__plat_test_no_counter = 1;
  const char *signer = S(tf_json_get(lj, "signer"), "public_hex");
  int checks = 0, good = 0;
  /* the override leases */
  const TfJson *ol = tf_json_get(gv, "override_leases");
  for (int i = 0; ol && i < ol->n; ++i) {
    const TfJson *o = &ol->kids[i], *f = tf_json_get(o, "fields");
    Host h;
    char w[1100];
    snprintf(w, sizeof w, "%s/o%d", work, i);
    if (host_open(&h, vec, w)) return 2;
    const long long iat = N(f, "iat");
    set_clock(N(o, "accept_at"), 1000, "b1");
    int64_t exp = 0;
    const int st = accept_hex(&h, S(o, "lease_hex"), &exp);
    CHECK((st == QLC_OK) == !strcmp(S(o, "expect"), "accept") && (st != QLC_OK || exp == N(f, "exp")), "override %s: %s (%d, exp %lld)", S(o, "name"),
          S(o, "expect"), st, (long long)exp);
    int told = 0;
    const TfJson *cs = tf_json_get(o, "checks");
    for (int k = 0; cs && k < cs->n; ++k) {
      const TfJson *ck = &cs->kids[k];
      const TfJson *ab = tf_json_get(ck, "attempt_before");
      if (ab && ab->type == TJ_BOOL && ab->boolean && !told) {
        qlc_lease_attempt(tf__pm_core(h.p), QLC_ATTEMPT_UNREACHABLE);
        told = 1;
      }
      set_clock(N(o, "accept_at") + (N(ck, "at") - iat), 1000 + (N(ck, "at") - iat) * 1000, "b1");
      qlc_lease_status ls;
      long long until;
      int os;
      const char *state = grace_state(&h, &ls, &until, &os);
      const int ok = !strcmp(state, S(ck, "state")) && ls.now == N(ck, "at") && (os == QLC_E_NEEDS_CONNECTION) == !strcmp(state, "needs_connection");
      checks++;
      good += ok;
      CHECK(ok, "override %s at %lld: %s, want %s (now %lld, open %d)", S(o, "name"), N(ck, "at"), state, S(ck, "state"), (long long)ls.now, os);
    }
    host_close(&h);
  }
  const int override_checks = checks;
  /* the sequences */
  const TfJson *sq = tf_json_get(gs, "sequences");
  const char *ch = S(gs, "challenge"), *key = S(gs, "key");
  for (int i = 0; sq && i < sq->n; ++i) {
    const TfJson *s = &sq->kids[i], *steps = tf_json_get(s, "steps");
    Host h;
    char w[1100];
    snprintf(w, sizeof w, "%s/s%d", work, i);
    if (host_open(&h, vec, w)) return 2;
    for (int k = 0; steps && k < steps->n; ++k) {
      const TfJson *st = &steps->kids[k];
      if (tf_json_get(st, "lease")) {
        set_clock(N(st, "wall"), N(st, "boot") * 1000, S(st, "boot_id")); /* grace.json: boot in seconds */
        const int a = accept_hex(&h, S(st, "lease_hex"), NULL);
        const TfJson *acc = tf_json_get(st, "accepted");
        const int want = acc && acc->type == TJ_BOOL && acc->boolean;
        qlc_lease_status ls;
        tf__pm_status(h.p, &ls);
        const int ok = (a == QLC_OK) == want && (!want || ls.now == N(st, "anchor"));
        checks++;
        good += ok;
        CHECK(ok, "sequence %s, step %d: the lease %s (%d, now %lld)", S(s, "name"), k, want ? "accepted" : "refused", a, (long long)ls.now);
      } else if (tf_json_get(st, "attempt")) {
        const int r = qlc_lease_attempt(tf__pm_core(h.p), !strcmp(S(st, "attempt"), "refused") ? QLC_ATTEMPT_REFUSED : QLC_ATTEMPT_UNREACHABLE);
        const char *res = S(st, "result");
        const int want = !strcmp(res, "ok") ? QLC_OK : !strcmp(res, "stopped") ? QLC_E_STOPPED : QLC_E_NEEDS_CONNECTION;
        checks++;
        good += r == want;
        CHECK(r == want, "sequence %s, step %d: attempt %s gives %d, want %s", S(s, "name"), k, S(st, "attempt"), r, res);
      } else if (tf_json_get(st, "stop")) {
        char cj[512], sig[128];
        qlc_lease_ask(tf__pm_core(h.p), ch, h.build, cj, sizeof cj, sig, sizeof sig);
        TfBuf b;
        tf_buf_init(&b);
        tf_json_write(&b, tf_json_get(st, "refusal"));
        const int r = qlc_refusal_accept(tf__pm_core(h.p), b.p, b.n, key);
        tf_buf_free(&b);
        checks++;
        good += r == QLC_E_STOPPED;
        CHECK(r == QLC_E_STOPPED, "sequence %s, step %d: a verified stop (%d)", S(s, "name"), k, r);
      } else if (tf_json_get(st, "restart")) {
        /* the state the Core saved through this SDK, handed back to a new Core */
        tf__client_close(h.c);
        h.c = test_client(w, signer);
        h.p = h.c ? tf__pm(h.c) : NULL;
        if (!h.p) return 2;
        char bd[1100];
        snprintf(bd, sizeof bd, "%s/models/%s", w, h.build);
        tf__plat_set_build(tf__pm_plat(h.p), bd);
      } else if (tf_json_get(st, "check")) {
        const TfJson *c = tf_json_get(st, "check");
        set_clock(N(c, "wall"), N(c, "boot") * 1000, S(c, "boot_id"));
        qlc_lease_status ls;
        long long until;
        int os;
        const char *state = grace_state(&h, &ls, &until, &os);
        /* now 0 in the vectors: no lease, so no clock anchored yet */
        const int ok = !strcmp(state, S(st, "state")) && (ls.now == N(st, "now") || (!N(st, "now") && !ls.has_lease)) && until == N(st, "until") &&
                       (os == QLC_E_NEEDS_CONNECTION) == !strcmp(state, "needs_connection");
        checks++;
        good += ok;
        CHECK(ok, "sequence %s, step %d: %s now %lld until %lld, want %s now %lld until %lld (open %d)", S(s, "name"), k, state, (long long)ls.now, until,
              S(st, "state"), N(st, "now"), N(st, "until"), os);
      }
    }
    host_close(&h);
  }
  printf("  grace.json: %d override lease checks, %d sequence steps (%d sequences), %d of %d as the contract says\n", override_checks,
         checks - override_checks, sq ? sq->n : 0, good, checks);
  tf_json_free(lj);
  tf_json_free(gs);
  tf_json_free(gv);
  return done("grace_vectors");
}

/* ------------------------------------------------------------------ the lossless form (SHARING_40.md 19) */
/* tools/sharing40.py test_signal: integer-only test audio, the same in every language */
static int16_t *test_signal(long long n, int seed) {
  int16_t *out = (int16_t *)malloc(sizeof(int16_t) * (size_t)(n ? n : 1));
  uint32_t st = (uint32_t)seed;
  long long walk = 0, a = 20000, b = 0, c = 30000, d = 0;
#define ASR(v, k) ((v) >= 0 ? (v) >> (k) : -((-(v)-1) >> (k)) - 1)
  for (long long i = 0; out && i < n; ++i) {
    st = st * 1664525u + 1013904223u;
    const long long noise = (long long)(st >> 16) - 32768;
    walk += ASR(noise, 11);
    walk = walk < -30000 ? -30000 : walk > 30000 ? 30000 : walk;
    a -= ASR(b * 1029, 16);
    b += ASR(a * 1029, 16);
    c -= ASR(d * 6863, 16);
    d += ASR(c * 6863, 16);
    const long long t = i % 160, tri = 150 * (t < 40 ? t : t < 120 ? 80 - t : t - 160);
    const long long m = (i / 4096 + seed) % 16;
    long long v;
    if (m == 0) v = 0;
    else if (m == 1) v = -1234;
    else if (m == 2) v = noise;
    else if (m == 3) v = ASR(noise, 9);
    else if (m == 4) v = walk;
    else if (m == 5 || m == 9) v = tri + ASR(noise, 13);
    else if (m == 6 || m == 10) v = a;
    else if (m == 7 || m == 11) v = c;
    else if (m == 8) v = (i / 32) % 2 == 0 ? 8192 : -8192;
    else v = ASR(noise, 14);
    out[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
  }
#undef ASR
  return out;
}
/* tools/sharing40.py case_pcm: the two cases given by a formula */
static int16_t *case_pcm(const char *name, long long n, int seed) {
  if (!strcmp(name, "r88200_wasted") || !strcmp(name, "r24000_extremes")) {
    int16_t *o = (int16_t *)malloc(sizeof(int16_t) * (size_t)n);
    for (long long i = 0; o && i < n; ++i) {
      long long v;
      if (!strcmp(name, "r88200_wasted")) v = i < 6000 ? ((i * 12) % 4000 - 2000) * 4 : -4096;
      else v = i < 2000 ? ((i / 50) % 2 ? 32767 : -32768) : i < 3000 ? (i % 2 ? 32767 : -32768) : (i % 7) * 9000 - 27000;
      o[i] = (int16_t)v;
    }
    return o;
  }
  return test_signal(n, seed);
}
static void pcm_sha(const int16_t *x, long long n, char hex[65]) {
  uint8_t *b = (uint8_t *)malloc((size_t)n * 2 + 1), h[32];
  for (long long i = 0; b && i < n; ++i) b[2 * i] = (uint8_t)(x[i] & 0xFF), b[2 * i + 1] = (uint8_t)((uint16_t)x[i] >> 8);
  tf_sha256(b, (size_t)n * 2, h);
  tf_hex_encode(h, 32, hex);
  free(b);
}
static int g_flac_segs[64], g_flac_nsegs;
static long long g_flac_granule[64];
static void flac_seg(void *user, int n, long long granule) {
  (void)user;
  if (g_flac_nsegs < 64) g_flac_segs[g_flac_nsegs] = n, g_flac_granule[g_flac_nsegs++] = granule;
}
/* a session through the encoder and the muxer into `dir`, in pieces of odd sizes (the encoder keeps its block) */
static int flac_session(const char *dir, const int16_t *pcm, long long n, int rate, uint32_t serial, const char *vendor, TfFlacMux *m) {
  rmrf(dir);
  tf_mkdirs(dir);
  g_flac_nsegs = 0;
  memset(m, 0, sizeof *m);
  m->on_segment = flac_seg;
  if (tf_flac_open(m, dir, ".oga", serial, rate, vendor)) return -1;
  for (long long at = 0; at < n;) {
    const long long k = n - at < 1777 ? n - at : 1777;
    if (tf_flac_push(m, pcm + at, k)) return -1;
    at += k;
  }
  return tf_flac_finish(m);
}
static char *seg_cat(const char *dir, int nsegs, size_t *len) {
  TfBuf b;
  tf_buf_init(&b);
  for (int k = 1; k <= nsegs; ++k) {
    char name[32], *t = NULL;
    size_t tn = 0;
    snprintf(name, sizeof name, "seg-%05d.oga", k);
    char *p = tf_path_join(dir, name);
    if (p && !tf_read_file(p, &t, &tn)) tf_buf_put(&b, t, tn);
    free(t);
    free(p);
  }
  *len = b.n;
  return tf_buf_take(&b);
}
static int t_flac(const char *vec, const char *work) {
  char sub[1100];
  snprintf(sub, sizeof sub, "%s/sharing40", vec);
  /* 1. flac16k: 75 s at 16 kHz in four segments, byte for byte */
  {
    char d[1100];
    snprintf(d, sizeof d, "%s/flac16k", sub);
    TfJson *ex = load_json(d, "expected.json");
    const TfJson *in = tf_json_get(ex, "input");
    const long long n = (long long)tf_json_get(in, "samples")->num;
    const int rate = (int)tf_json_get(in, "rate")->num, seed = (int)tf_json_get(in, "seed")->num;
    int16_t *pcm = test_signal(n, seed);
    char sha[65];
    pcm_sha(pcm, n, sha);
    CHECK(!strcmp(sha, S(in, "pcm_sha256")), "flac16k: the input is test_signal's (%s)", sha);
    char out[1100];
    snprintf(out, sizeof out, "%s/flac16k", work);
    TfFlacMux m;
    const int rc = flac_session(out, pcm, n, rate, (uint32_t)tf_json_get(in, "serial")->num, S(in, "vendor"), &m);
    const TfJson *segs = tf_json_get(ex, "segments");
    CHECK(!rc && segs && g_flac_nsegs == segs->n, "flac16k: %d segments (the vector's %d)", g_flac_nsegs, segs ? segs->n : -1);
    int same = 0;
    for (int k = 0; segs && k < segs->n; ++k) {
      char *a = NULL, *b = NULL, *pa = tf_path_join(out, S(&segs->kids[k], "file")), *pb = tf_path_join(d, S(&segs->kids[k], "file"));
      size_t an = 0, bn = 0;
      const int ok = pa && pb && !tf_read_file(pa, &a, &an) && !tf_read_file(pb, &b, &bn) && an == bn && !memcmp(a, b, an);
      CHECK(ok, "flac16k: %s byte for byte (%lu against %lu bytes)", S(&segs->kids[k], "file"), (unsigned long)an, (unsigned long)bn);
      same += ok;
      free(a);
      free(b);
      free(pa);
      free(pb);
    }
    const TfJson *kinds = tf_json_get(tf_json_get(ex, "frames"), "kinds");
    static const char *const kn[7] = {"constant", "verbatim", "fixed0", "fixed1", "fixed2", "fixed3", "fixed4"};
    int kinds_ok = 1;
    for (int k = 0; k < 7; ++k) kinds_ok &= m.kinds[k] == (tf_json_get(kinds, kn[k]) ? (int)tf_json_get(kinds, kn[k])->num : 0);
    CHECK(kinds_ok, "flac16k: every frame kind as the vector counts them");
    CHECK(g_flac_nsegs == 4 && g_flac_granule[3] == (long long)tf_json_get(ex, "final_granule")->num, "flac16k: the last granule is the session's length");
    printf("  flac16k: %d of %d segments byte for byte\n", same, segs ? segs->n : 0);
    free(pcm);
    tf_json_free(ex);
  }
  /* 2. flac_cases.json: other rates and edge cases, the whole output by SHA-256 and, when small, in full */
  {
    TfJson *fc = load_json(sub, "flac_cases.json");
    const TfJson *cases = tf_json_get(fc, "cases");
    int ok_cases = 0;
    for (int i = 0; cases && i < cases->n; ++i) {
      const TfJson *cs = &cases->kids[i];
      const char *name = S(cs, "name");
      const int rate = (int)tf_json_get(cs, "rate")->num;
      const long long n = (long long)tf_json_get(cs, "samples")->num;
      const TfJson *ts = tf_json_get(tf_json_get(cs, "input"), "test_signal");
      int16_t *pcm = case_pcm(name, n, ts ? (int)tf_json_get(ts, "seed")->num : 0);
      char sha[65];
      pcm_sha(pcm, n, sha);
      char out[1100];
      snprintf(out, sizeof out, "%s/case_%s", work, name);
      TfFlacMux m;
      const int rc = flac_session(out, pcm, n, rate, (uint32_t)tf_json_get(cs, "serial")->num, S(cs, "vendor"), &m);
      size_t len = 0;
      char *whole = seg_cat(out, g_flac_nsegs, &len);
      uint8_t h[32];
      char hx[65];
      tf_sha256(whole, len, h);
      tf_hex_encode(h, 32, hx);
      int ok = !rc && !strcmp(sha, S(cs, "pcm_sha256")) && len == (size_t)tf_json_get(cs, "bytes")->num && !strcmp(hx, S(cs, "sha256"));
      const TfJson *segs = tf_json_get(cs, "segments");
      ok = ok && segs && segs->n == g_flac_nsegs;
      const char *b64 = tf_json_str(tf_json_get(cs, "ogg_flac_base64"));
      if (ok && b64) {
        uint8_t *raw = NULL;
        const size_t rn = unb64(b64, &raw);
        ok = rn == len && !memcmp(raw, whole, len);
        free(raw);
      }
      CHECK(ok, "flac case %s: %lu bytes, sha256 %s (the vector's %s)", name, (unsigned long)len, hx, S(cs, "sha256"));
      ok_cases += ok;
      free(whole);
      free(pcm);
    }
    printf("  flac_cases: %d of %d\n", ok_cases, cases ? cases->n : 0);
    CHECK(cases && ok_cases == cases->n, "flac_cases: every case");
    tf_json_free(fc);
  }
  /* 3. pcm16.json: the float to 16-bit rule */
  {
    TfJson *pj = load_json(sub, "pcm16.json");
    const TfJson *cases = tf_json_get(pj, "cases");
    int ok = 0;
    for (int i = 0; cases && i < cases->n; ++i) {
      const uint32_t bits = (uint32_t)strtoul(S(&cases->kids[i], "f32"), NULL, 16);
      float x;
      memcpy(&x, &bits, 4);
      const int got = tf_pcm16(x), want = (int)tf_json_get(&cases->kids[i], "int16")->num;
      CHECK(got == want, "pcm16 %s: %d (the vector's %d)", S(&cases->kids[i], "f32"), got, want);
      ok += got == want;
    }
    printf("  pcm16: %d of %d\n", ok, cases ? cases->n : 0);
    tf_json_free(pj);
  }
  /* 4. upload_start.json: the start body of a lossless session and of an Opus one, the shapes the server accepts */
  {
    TfJson *uj = load_json(sub, "upload_start.json");
    const TfJson *acc = tf_json_get(uj, "accepted");
    for (int f = 0; f < 2; ++f) {
      char *body = tf__share_session_json_test(f ? 16000 : 48000, f);
      TfJson *bj = body ? tf_json_parse(body, strlen(body), NULL) : NULL;
      const TfJson *au = tf_json_get(bj, "audio");
      /* the accepted body of the same codec: the same audio keys, none more, none fewer */
      const TfJson *ref = NULL;
      for (int i = 0; acc && i < acc->n && !ref; ++i) {
        const TfJson *a2 = tf_json_get(tf_json_get(&acc->kids[i], "body"), "audio");
        if (!strcmp(S(a2, "codec"), f ? "flac" : "opus") && tf_json_get(a2, "input_sample_rate") && tf_json_get(a2, "input_route")) ref = a2;
      }
      int keys_ok = au && ref && au->n == ref->n;
      for (int i = 0; keys_ok && i < ref->n; ++i) keys_ok = tf_json_get(au, ref->kids[i].key) != NULL;
      CHECK(keys_ok, "upload_start: the %s audio block has the accepted body's keys (%s)", f ? "flac" : "opus", body ? body : "none");
      if (f)
        CHECK(au && !strcmp(S(au, "codec"), "flac") && !strcmp(S(au, "container"), "ogg") && tf_json_get(au, "sample_rate")->num == 16000 &&
                  tf_json_get(au, "input_sample_rate")->num == 16000 && tf_json_get(au, "bits_per_sample")->num == 16 &&
                  tf_json_get(au, "channels")->num == 1,
              "upload_start: flac at its capture rate, 16-bit, mono");
      else
        CHECK(au && !strcmp(S(au, "codec"), "opus") && tf_json_get(au, "sample_rate")->num == 48000 && !tf_json_get(au, "bits_per_sample"),
              "upload_start: opus at 48 kHz, no bits_per_sample");
      CHECK(bj && !tf_json_get(bj, "device_model"), "upload_start: no device_model on Windows and Linux (19.2)");
      tf_json_free(bj);
      free(body);
    }
    tf_json_free(uj);
  }
  return done("flac");
}

/* ------------------------------------------------------------------ the ask (SHARE_ASK_40.md 20) */
/* two JSON values equal: objects whatever their key order, numbers by value */
static int json_equal(const TfJson *a, const TfJson *b) {
  if (!a || !b) return a == b;
  if (a->type != b->type) return 0;
  switch (a->type) {
  case TJ_NUM: return a->num == b->num;
  case TJ_STR: return !strcmp(a->str, b->str);
  case TJ_BOOL: return a->boolean == b->boolean;
  case TJ_ARR:
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; ++i)
      if (!json_equal(&a->kids[i], &b->kids[i])) return 0;
    return 1;
  case TJ_OBJ:
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; ++i)
      if (!json_equal(&a->kids[i], tf_json_get(b, a->kids[i].key))) return 0;
    return 1;
  default: return 1;
  }
}
static const char *kAction[3] = {"none", "sheet", "card"};
/* every config case through the config reading, every case's steps through the decision (action, why, the whole
 * new state, the app part included) */
static int t_share_ask(const char *vec, const char *work) {
  (void)work;
  TfJson *v = load_json(vec, "share_ask.json");
  if (!v) return 2;
  const TfJson *cc = tf_json_get(v, "config_cases");
  int cfg_ok = 0;
  for (int i = 0; cc && i < cc->n; ++i) {
    const TfJson *k = &cc->kids[i], *res = tf_json_get(k, "resolved");
    TfAskConfig c;
    tf__ask_config(tf_json_get(k, "raw"), &c);
    char got[256];
    snprintf(got, sizeof got,
             "{\"first_min_sessions\":%d,\"first_min_hours\":%d,\"reask_days\":[%d,%d],\"reask_sessions\":[%d,%d],\"good_session_min_s\":%d,\"max_asks\":%d}",
             c.first_min_sessions, c.first_min_hours, c.reask_days[0], c.reask_days[1], c.reask_sessions[0], c.reask_sessions[1],
             c.good_session_min_s, c.max_asks);
    TfJson *g = tf_json_parse(got, strlen(got), NULL);
    const int ok = json_equal(g, res);
    CHECK(ok, "share_ask config %s: %s", S(k, "name"), got);
    cfg_ok += ok;
    tf_json_free(g);
  }
  const TfJson *cases = tf_json_get(v, "cases");
  int steps = 0, steps_ok = 0, cases_ok = 0;
  for (int i = 0; cases && i < cases->n; ++i) {
    const TfJson *cs = &cases->kids[i], *st0 = tf_json_get(cs, "state"), *seq = tf_json_get(cs, "steps");
    TfAskState st;
    memset(&st, 0, sizeof st);
    for (int k = 0; st0 && k < st0->n; ++k) tf__ask_read_field(&st, st0->kids[k].key, &st0->kids[k]);
    const TfJson *cfg = tf_json_get(cs, "config");
    int all = 1;
    for (int j = 0; seq && j < seq->n; ++j) {
      const TfJson *sp = &seq->kids[j], *set = tf_json_get(sp, "set"), *now = tf_json_get(sp, "now");
      for (int k = 0; set && k < set->n; ++k) tf__ask_read_field(&st, set->kids[k].key, &set->kids[k]);
      TfAskEvent ev;
      tf__ask_event_of(tf_json_get(sp, "event"), &ev);
      const char *why = NULL;
      const int action = tf__ask_decide(&st, &ev, cfg && cfg->type == TJ_OBJ ? cfg : NULL, (long long)tf_json_get(now, "ms")->num,
                                        (int)tf_json_get(now, "tz_min")->num, &why);
      TfBuf b;
      tf_buf_init(&b);
      tf__ask_state_json(&st, &b);
      TfJson *got = tf_json_parse(b.p, b.n, NULL);
      const char *want_why = tf_json_str(tf_json_get(sp, "why"));
      const int ok = !strcmp(kAction[action], S(sp, "action")) && ((!why && !want_why) || (why && want_why && !strcmp(why, want_why))) &&
                     json_equal(got, tf_json_get(sp, "state"));
      if (!ok)
        CHECK(0, "share_ask %s, step %d: action %s why %s state %s", S(cs, "name"), j, kAction[action], why ? why : "null", b.p);
      steps++;
      steps_ok += ok;
      all &= ok;
      tf_json_free(got);
      tf_buf_free(&b);
    }
    cases_ok += all;
  }
  printf("  share_ask: %d of %d config cases, %d of %d cases (%d of %d steps)\n", cfg_ok, cc ? cc->n : 0, cases_ok, cases ? cases->n : 0, steps_ok,
         steps);
  CHECK(cc && cfg_ok == cc->n && cases && cases_ok == cases->n, "share_ask: every config case and every case");
  tf_json_free(v);
  return done("share_ask");
}

static const char *g_cfg_body, *g_cfg_sig, *g_cfg_kid; /* the capture transport's config (below) */
static int capture(const TfHttpRequest *q, TfHttpResponse *r, void *u);
/* The moments through the SDK (20.6, 20.10), on a fake clock with the settle checked by hand (tests have no timer): the
 * first ask handed after the settle, and a take before it drops it; the re-ask as the 3.x re-ask sheet in a card's
 * place when the deck has no card strings (a deck before 2026-11-15.1, through the test hook); with the bundled deck's
 * card strings, the card handed with its lines, gone with a take, and opened into the sheet, where the yes is given. */
static int g_asks_n, g_ask_kind, g_ask_reask;
static char g_card_line[256], g_card_open[128], g_card_not_now[128];
static void on_consent_rec(void *user, const TfConsentRequest *r) {
  (void)user;
  g_asks_n++;
  g_ask_kind = r->kind;
  g_ask_reask = r->reask;
  snprintf(g_card_line, sizeof g_card_line, "%s", r->card_line ? r->card_line : "");
  snprintf(g_card_open, sizeof g_card_open, "%s", r->card_open ? r->card_open : "");
  snprintf(g_card_not_now, sizeof g_card_not_now, "%s", r->card_not_now ? r->card_not_now : "");
}
static void take(TfClient *c, int end, int frames) {
  tf__sharing_take_start(c);
  tf__sharing_session(c, end, frames);
}
static int t_share_moments(const char *fixture, const char *work) {
  TfJson *fx = load_json(fixture, "flow.json");
  if (!fx) return 2;
  tf__client_no_timer = 1;
  tf_http_set_transport(capture, NULL);
  g_cfg_body = S(fx, "config_share_body");
  g_cfg_sig = S(fx, "config_share_sig");
  g_cfg_kid = S(fx, "config_kid");
  const long long T0 = 1800000000000LL, H = 3600000LL, D = 86400000LL;
  tf__ask_tz_min = 0;
  for (int pass = 0; pass < 2; ++pass) {
    char w[1100];
    snprintf(w, sizeof w, "%s/%s", work, pass ? "card" : "sheet");
    rmrf(w);
    tf_mkdirs(w);
    tf__ask_now_ms = T0;
    TfClientConfig o = tf__client_default_config();
    o.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
    o.app_id = "com.example.recite";
    o.storage_dir = w;
    o.trusted_key_hex = S(fx, "signer_hex");
    o.on_consent = on_consent_rec;
    TfClient *c = tf__client_open(&o);
    CHECK(c && tf__client_refresh_config(c) == 0 && tf_sharing_is_available(c), "moments: the config offers sharing");
    /* the English deck's card strings (2026-11-15.1); pass 0 stands in a deck without them */
    static const char *const card[3] = {"Your recitation today could help another learner tomorrow.", "See how it helps", "Not now"};
    static const char *const none[3] = {NULL, NULL, NULL};
    tf__sharing_test_card = pass ? NULL : none;
    g_asks_n = 0;
    /* inside the first 24 hours: three good takes ask nothing */
    tf__ask_now_ms = T0 + 2 * H;
    for (int k = 0; k < 3; ++k) take(c, 0, 1000);
    tf__ask_now_ms += 5000;
    tf__sharing_settle_check(c);
    CHECK(g_asks_n == 0, "moments: nothing within 24 hours of the install");
    /* a new app session after the first day: the third good take of 40 s brings the first ask, after the settle */
    tf__ask_now_ms = T0 + 26 * H;
    {
      TfAskEvent ev;
      memset(&ev, 0, sizeof ev);
      ev.type = TF_ASK_EV_LAUNCH;
      tf__sharing_event(c, &ev, NULL);
    }
    take(c, 0, 1000);
    take(c, 0, 1000);
    tf__sharing_settle_check(c);
    CHECK(g_asks_n == 0, "moments: not before the settle");
    take(c, 0, 1000); /* a take before the settle: the ask waits for this take's end */
    tf__ask_now_ms += 2999;
    tf__sharing_settle_check(c);
    CHECK(g_asks_n == 0, "moments: 2999 ms is not the settle");
    tf__ask_now_ms += 1;
    tf__sharing_settle_check(c);
    CHECK(g_asks_n == 1 && g_ask_kind == TF_ASK_SHEET && g_ask_reask == 0, "moments: the first ask, the full sheet, after the 3 s settle (%d)", g_asks_n);
    TfSharing st;
    tf_sharing_status(c, &st);
    CHECK(st.asks == 0, "moments: an ask counts once it is on screen");
    CHECK(tf_sharing_shown(c) == 1, "moments: shown");
    tf_sharing_status(c, &st);
    CHECK(st.asks == 1, "moments: the first ask counted");
    CHECK(tf_sharing_answer(c, 0) == 0 && !tf_sharing_is_on(c), "moments: not now");
    /* 14 days and 7 sessions later, a new app session and a good minute: the re-ask */
    tf__ask_now_ms = T0 + 26 * H + 14 * D + H;
    {
      TfAskEvent ev;
      memset(&ev, 0, sizeof ev);
      ev.type = TF_ASK_EV_LAUNCH;
      tf__sharing_event(c, &ev, NULL);
    }
    for (int k = 0; k < 7; ++k) take(c, 0, 1000);
    tf__ask_now_ms += 3000;
    tf__sharing_settle_check(c);
    if (!pass) {
      /* no card strings in the deck: the 3.x re-ask sheet in the card's place, opened into the sheet once shown */
      CHECK(g_asks_n == 2 && g_ask_kind == TF_ASK_SHEET && g_ask_reask == 1, "moments: a re-ask without card strings is the re-ask sheet (%d, kind %d, reask %d)",
            g_asks_n, g_ask_kind, g_ask_reask);
      CHECK(tf_sharing_shown(c) == 1, "moments: the re-ask sheet shown");
      CHECK(tf_sharing_answer(c, 1) == 0 && tf_sharing_is_on(c), "moments: yes on the re-ask sheet");
      tf_sharing_status(c, &st);
      CHECK(st.asks == 2, "moments: the card it stood for is one ask");
    } else {
      CHECK(g_asks_n == 2 && g_ask_kind == TF_ASK_CARD && !strcmp(g_card_line, card[0]) && !strcmp(g_card_open, card[1]) &&
                !strcmp(g_card_not_now, card[2]),
            "moments: the re-ask is a card with the bundled deck's lines (%s | %s | %s)", g_card_line, g_card_open, g_card_not_now);
      CHECK(tf_sharing_shown(c) == 1, "moments: the card shown");
      CHECK(tf_sharing_answer(c, 1) == TF_ERR_INVALID_ARGUMENT, "moments: a card has no yes");
      tf__sharing_take_start(c); /* a take starts: the card goes, Not now */
      tf_sharing_status(c, &st);
      CHECK(st.asks == 2 && !st.pending_ask && !tf_sharing_is_on(c), "moments: a card gone with a take is Not now, one of the three");
      /* the third ask, 30 days and 10 sessions later: a card opened into the sheet */
      tf__ask_now_ms += 30 * D + H;
      {
        TfAskEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.type = TF_ASK_EV_LAUNCH;
        tf__sharing_event(c, &ev, NULL);
      }
      tf__sharing_session(c, 0, 1000);
      for (int k = 0; k < 10; ++k) take(c, 0, 1000);
      tf__ask_now_ms += 3000;
      tf__sharing_settle_check(c);
      CHECK(g_asks_n == 3 && g_ask_kind == TF_ASK_CARD, "moments: the last card");
      CHECK(tf_sharing_shown(c) == 1 && tf_sharing_open(c) == 0 && g_asks_n == 4 && g_ask_kind == TF_ASK_SHEET && g_ask_reask == 0,
            "moments: See how it helps opens the full sheet, without the re-ask line");
      CHECK(tf_sharing_answer(c, 1) == 0 && tf_sharing_is_on(c), "moments: yes on the sheet the card opened");
      tf_sharing_status(c, &st);
      CHECK(st.asks == 3, "moments: a card opened into the sheet is one ask");
      CHECK(tf_sharing_ask(c) == 0, "moments: after a yes, ask() asks nothing");
    }
    /* sharing.json carries 4.0's clocks */
    char *sj = NULL;
    size_t sn = 0;
    char sp[1200];
    snprintf(sp, sizeof sp, "%s/sharing.json", w);
    CHECK(!tf_read_file(sp, &sj, &sn) && strstr(sj, "\"installedAt\":1800000000000") && strstr(sj, "\"lastAskAt\":") && strstr(sj, "\"lastAskDay\":"),
          "moments: sharing.json keeps installedAt, lastAskAt and lastAskDay (%s)", sj ? sj : "none");
    free(sj);
    tf__client_close(c);
    tf__sharing_test_card = NULL;
  }
  tf__ask_now_ms = 0;
  tf__ask_tz_min = -100000;
  g_cfg_body = NULL;
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(fx);
  return done("share_moments");
}

/* A shared recording through the SDK's recorder (tf_share.c), with the learner's yes: at 16 kHz with the signed
 * config's flags.share_flac true it is Ogg FLAC at 16 kHz, its segments together the very bytes the encoder and the
 * muxer give for the same samples (the floats as pcm16 reads them back), its session JSON says flac; without the flag
 * the same capture is Opus at 48 kHz as before. */
static int t_share_flac(const char *fixture, const char *work) {
  TfJson *fx = load_json(fixture, "flow.json");
  if (!fx) return 2;
  tf__client_no_timer = 1;
  tf_http_set_transport(capture, NULL);
  g_cfg_kid = S(fx, "config_kid");
  const long long n = 16000LL * 25; /* 25 s: segment 1 (40 frames) closes, segment 2 ends the stream */
  int16_t *pcm = test_signal(n, 3);
  float *f = (float *)malloc(sizeof(float) * (size_t)n);
  for (long long i = 0; i < n; ++i) f[i] = (float)pcm[i] / 32768.0f; /* exact: pcm16 gives pcm back */
  for (int pass = 0; pass < 2; ++pass) {
    char w[1100], outbox[1200];
    snprintf(w, sizeof w, "%s/%s", work, pass ? "opus" : "flac");
    rmrf(w);
    tf_mkdirs(w);
    g_cfg_body = S(fx, pass ? "config_share_body" : "config_flac_body");
    g_cfg_sig = S(fx, pass ? "config_share_sig" : "config_flac_sig");
    TfClient *c = test_client(w, S(fx, "signer_hex"));
    CHECK(c && tf__client_refresh_config(c) == 0, "share_flac: the config is in force");
    tf__client_sharing_answer(c, 1); /* the learner's yes */
    tf__share_serial = 0x0F1AC000u + 16000;
    TfShareSession *ss = c ? tf__sharing_session_begin(c, 16000, "built_in") : NULL;
    CHECK(ss != NULL, "share_flac: a recording begins (%s)", ss ? "ok" : tf_last_error());
    for (long long at = 0; ss && at < n; at += 1600) tf__sharing_session_push(ss, f + at, (int)(n - at < 1600 ? n - at : 1600));
    if (ss) tf__sharing_session_finish(ss, 1);
    if (c) tf__sharing_recording_close(c);
    snprintf(outbox, sizeof outbox, "%s/outbox", w);
    char names[8][256];
    const int nn = tf__list_dir(outbox, names, 8);
    char sess[1300] = "";
    for (int i = 0; i < nn; ++i)
      if (strlen(names[i]) == 32) snprintf(sess, sizeof sess, "%s/%s", outbox, names[i]);
    TfJson *sj = load_json(sess, "session.json"), *cap = load_json(sess, "capture.json");
    const TfJson *au = tf_json_get(sj, "audio");
    if (!pass) {
      CHECK(au && !strcmp(S(au, "codec"), "flac") && tf_json_get(au, "sample_rate")->num == 16000 && tf_json_get(au, "bits_per_sample")->num == 16,
            "share_flac: flags.share_flac at 16 kHz: a FLAC session at 16 kHz");
      CHECK(cap && !strcmp(S(cap, "codec"), "flac") && tf_json_get(cap, "closed")->num == 2 && tf_json_get(cap, "duration_ms")->num == 25000,
            "share_flac: capture.json says flac, two segments, 25 s");
      /* the same samples through the encoder and the muxer: the same bytes */
      char ref[1100];
      snprintf(ref, sizeof ref, "%s/reference", work);
      TfFlacMux m;
      const int rc = flac_session(ref, pcm, n, 16000, 0x0F1AC000u + 16000, "QuranLab SDK " TF_SDK_VERSION, &m);
      size_t ln = 0, rn = 0;
      char *want = seg_cat(ref, g_flac_nsegs, &rn);
      TfBuf got;
      tf_buf_init(&got);
      for (int k = 1; k <= 2; ++k) {
        char name[32], *t = NULL;
        size_t tn = 0;
        snprintf(name, sizeof name, "seg-%05d.opus", k);
        char *pth = tf_path_join(sess, name);
        if (pth && !tf_read_file(pth, &t, &tn)) tf_buf_put(&got, t, tn);
        free(t);
        free(pth);
      }
      ln = got.n;
      CHECK(!rc && g_flac_nsegs == 2 && ln == rn && ln > 0 && !memcmp(got.p, want, ln),
            "share_flac: the recorder's segments are the encoder's bytes (%lu against %lu)", (unsigned long)ln, (unsigned long)rn);
      CHECK(ln > 4 && !memcmp(got.p, "OggS", 4) && !memcmp(got.p + 28, "\x7f" "FLAC", 5), "share_flac: an Ogg FLAC stream");
      tf_buf_free(&got);
      free(want);
    } else {
      CHECK(au && !strcmp(S(au, "codec"), "opus") && tf_json_get(au, "sample_rate")->num == 48000 && tf_json_get(au, "input_sample_rate")->num == 16000 &&
                !tf_json_get(au, "bits_per_sample"),
            "share_flac: without the flag the 16 kHz capture is Opus at 48 kHz, as before");
    }
    tf_json_free(sj);
    tf_json_free(cap);
    tf__share_serial = 0;
    if (c) tf__client_close(c);
  }
  free(f);
  free(pcm);
  g_cfg_body = NULL;
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(fx);
  return done("share_flac");
}

/* ------------------------------------------------------------------ piece_order.json (spec fix 16) */
/* Every sequence of the contract's piece_order.json through this SDK's Core host (its platform, key store, saved
 * state and model opens) on the encrypted test build (make_flow40.py --testbuild): each sequence in a new storage
 * folder, the build downloaded and installed through the stand-in server first (its lease asked under no piece, with
 * no piece in the answer), then the steps as the vector names them: "ask" qlc_lease_ask (its core.piece must be the
 * one wanted), "accept" qlc_lease_accept of the fixture's lease, "piece" qlc_lease_piece of the named piece right
 * after it, "refuse" an unsigned refusal through qlc_refusal_accept, "busy" nothing, "open" and "close" the build's
 * model through tf__pm_open and tf__pm_close (a real engine open), "open_fails" an open of a context the build does
 * not have, "restart" the process ending: the saved state as it was, the Client destroyed (its open model closed) and
 * a new one made on the same folder with that state. */
static int t_piece_order(const char *fixture, const char *vectors, const char *work) {
  TfJson *fx = flow_setup(fixture), *po = load_json(vectors, "piece_order.json");
  if (!fx || !po) return 2;
  const TfJson *seqs = tf_json_get(po, "sequences"), *pieces = tf_json_get(fx, "order_pieces");
  uint8_t *lease = NULL;
  const size_t ln = unb64(S(tf_json_get(fx, "lease"), "lease"), &lease);
  const char *man = S(fx, "manifest"), *build = S(fx, "build");
  const size_t mn = strlen(man);
  static const char *const refusal = "{\"error\":\"lease_integrity_failed\",\"message\":\"the Core could not be verified\",\"why\":\"core\"}";
  int sequences = 0;
  for (int q = 0; seqs && q < seqs->n; ++q) {
    const TfJson *sq = &seqs->kids[q], *steps = tf_json_get(sq, "steps");
    char w[1100], models[1300], state[1300], keep[1300];
    snprintf(w, sizeof w, "%s/seq%d", work, q + 1);
    fake_reset(LEASE_OK);
    TfClient *c = flow_client(w);
    TfModel *m = c ? tf_client_model(c, NULL) : NULL;
    CHECK(m != NULL, "piece_order %d: the test build is installed (%s)", q + 1, m ? "ok" : tf_last_error());
    if (m) tf_model_close(m);
    snprintf(models, sizeof models, "%s/models/%s", w, build);
    snprintf(state, sizeof state, "%s/core/state.bin", w);
    snprintf(keep, sizeof keep, "%s/state.kept", w);
    qlc_model *open = NULL;
    char asks[256] = "", want[256] = "";
    int bad = 0;
    for (int i = 0; c && steps && i < steps->n; ++i) {
      const TfJson *st = &steps->kids[i];
      const char *op = S(st, "op");
      TfPm *p = tf__pm(c);
      if (!p) {
        bad = 1;
        break;
      }
      if (!strcmp(op, "ask")) {
        char cj[512], sig[160];
        tf__pm_lock(p);
        const int rc = qlc_lease_ask(tf__pm_core(p), S(fx, "challenge"), build, cj, sizeof cj, sig, sizeof sig);
        tf__pm_unlock(p);
        TfJson *j = rc ? NULL : tf_json_parse(cj, strlen(cj), NULL);
        const TfJson *pc = tf_json_get(j, "piece");
        const int got = pc && pc->type == TJ_NUM ? (int)pc->num : -1;
        tf_json_free(j);
        const int wanted = (int)tf_json_get(st, "want")->num;
        snprintf(asks + strlen(asks), sizeof asks - strlen(asks), "%s%d", asks[0] ? " " : "", got);
        snprintf(want + strlen(want), sizeof want - strlen(want), "%s%d", want[0] ? " " : "", wanted);
        bad |= got != wanted;
      } else if (!strcmp(op, "accept")) {
        int64_t e = 0, ra = 0;
        tf__pm_lock(p);
        tf__plat_set_build(tf__pm_plat(p), models);
        const int rc = qlc_lease_accept(tf__pm_core(p), lease, ln, man, mn, build, &e, &ra);
        tf__pm_unlock(p);
        CHECK(rc == QLC_OK, "piece_order %d step %d: the lease is accepted (%d)", q + 1, i, rc);
      } else if (!strcmp(op, "piece")) {
        uint8_t *pb = NULL;
        const size_t pn = unb64(S(pieces, S(st, "name")), &pb);
        tf__pm_lock(p);
        qlc_lease_piece(tf__pm_core(p), pb, pn); /* the bad signature is refused, and changes nothing */
        tf__pm_unlock(p);
        free(pb);
      } else if (!strcmp(op, "refuse")) {
        tf__pm_lock(p);
        qlc_refusal_accept(tf__pm_core(p), refusal, strlen(refusal), "apps-one");
        tf__pm_unlock(p);
      } else if (!strcmp(op, "open")) {
        open = tf__pm_open(p, models, man, mn, "c16_l64", 1, 16000, 0);
        CHECK(open != NULL, "piece_order %d step %d: the model opens (%s)", q + 1, i, open ? "ok" : tf_last_error());
      } else if (!strcmp(op, "open_fails")) {
        qlc_model *x = tf__pm_open(p, models, man, mn, "c99_l99", 1, 16000, 0);
        CHECK(x == NULL, "piece_order %d step %d: an open that fails", q + 1, i);
        if (x) tf__pm_close(p, x);
      } else if (!strcmp(op, "close")) {
        if (open) tf__pm_close(p, open);
        open = NULL;
      } else if (!strcmp(op, "restart")) {
        /* the process ends: what the Core saved last is what the next one starts from, whatever the close below
         * writes (a killed process closes nothing) */
        char *sb = NULL;
        size_t sn = 0;
        const int have = !tf_read_file(state, &sb, &sn);
        if (have) tf_write_file(keep, sb, sn);
        if (open) tf__pm_close(p, open);
        open = NULL;
        tf_client_destroy(c);
        if (have) tf_write_file(state, sb, sn);
        free(sb);
        remove(keep);
        c = test_client(w, S(fx, "signer_hex"));
      } else if (strcmp(op, "busy")) {
        CHECK(0, "piece_order %d step %d: an op this test does not know: %s", q + 1, i, op);
      }
    }
    CHECK(!bad && !strcmp(asks, want), "piece_order %d (%s): asks %s, the vector's %s", q + 1, S(sq, "name"), asks, want);
    printf("  piece_order %d: asks %s\n", q + 1, asks);
    sequences++;
    if (open && c) tf__pm_close(tf__pm(c), open);
    if (c) tf_client_destroy(c);
  }
  CHECK(sequences == 4, "piece_order: the four sequences (%d)", sequences);
  free(lease);
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(po);
  tf_json_free(fx);
  return done("piece_order");
}

/* ------------------------------------------------------------------ the recited range and 17.9 */
static int t_recited(const char *work) {
  rmrf(work);
  tf_mkdirs(work);
  days_key(1);
  TfClient *c = test_client(work, NULL);
  if (!c) return 2;
  TfRecitedCount k;
  memset(&k, 0, sizeof k);
  k.source = 1, k.surah = 2, k.first = 255, k.last = 257, k.take_id = 0x1111;
  tf__client_count_take_range(c, &k);
  tf__client_count_take_range(c, &k); /* the same take again: counted once */
  memset(&k, 0, sizeof k);
  k.source = 2, k.surah = 1, k.first = 1, k.last = 7, k.take_id = 0x2222;
  tf__client_count_take_range(c, &k);
  memset(&k, 0, sizeof k);
  k.take_id = 0x3333; /* the detector found nothing and the app reported nothing */
  tf__client_count_take_range(c, &k);
  CHECK(tf__client_take_counted(c, 0x1111) && tf__client_take_counted(c, 0x2222) && !tf__client_take_counted(c, 0x4444), "take ids are kept");
  char *body = tf__client_day_body(c);
  CHECK(body && strstr(body, "\"ayahs\":3") && strstr(body, "\"surah_ayahs\":{\"2\":3}"), "measured: ayahs and surah_ayahs (%s)", body);
  CHECK(body && strstr(body, "\"app_ayahs\":7") && strstr(body, "\"app_surah_ayahs\":{\"1\":7}"), "the app's fallback: app_ayahs");
  CHECK(body && strstr(body, "\"reported\":[\"app_ayahs\",\"app_surah_ayahs\"]"), "only the app's fields are listed in reported");
  CHECK(body && !strstr(body, "coverage") && !strstr(body, "take_ids"), "no coverage, no take id on the wire");
  CHECK(body && strstr(body, "\"recited_takes\":{\"measured\":1,\"none\":1,\"app\":1}"),
        "recited_takes on by default, its counts that are not zero (%s)", body ? strstr(body, "\"recited_takes\"") : "");
  free(body);
  /* the session's precedence: the app's report is not counted for a take the detector counted */
  TfRecited app;
  memset(&app, 0, sizeof app);
  app.surah = 3, app.ayah_from = 1, app.ayah_to = 2;
  qlc_recited_result res;
  memset(&res, 0, sizeof res);
  tf__recited_finish(c, NULL, NULL, 0, &app, NULL); /* no detector: the app's report counts */
  body = tf__client_day_body(c);
  CHECK(body && strstr(body, "\"app_ayahs\":9"), "a take without a measured range counts the app's report");
  free(body);
  tf__client_close(c);

  /* R5 (quranlab-core cd24aa3): the session context's part and a refrain tie (17.8). A take the context decided counts
   * in recited_takes.context; a take counted through a tie counts toward a juz only when every copy lies in it (its
   * surah inside one juz, no context), and never as a repeat */
  {
    char w2[1100];
    snprintf(w2, sizeof w2, "%s/r5", work);
    tf_mkdirs(w2);
    c = test_client(w2, NULL);
    TfRecitedCount t;
    memset(&t, 0, sizeof t);
    t.source = 1, t.surah = 55, t.first = 13, t.last = 13, t.ties = 30, t.take_id = 0xA1; /* Ar-Rahman's refrain: juz 27 only */
    tf__client_count_take_range(c, &t);
    t.take_id = 0xA2; /* the same refrain again in the session: not a repeat */
    tf__client_count_take_range(c, &t);
    memset(&t, 0, sizeof t);
    t.source = 1, t.surah = 2, t.first = 1, t.last = 2, t.ties = 1, t.take_id = 0xA3; /* al-Baqarah spans three juz */
    tf__client_count_take_range(c, &t);
    memset(&t, 0, sizeof t);
    t.source = 1, t.surah = 1, t.first = 1, t.last = 7, t.context = 1, t.take_id = 0xA4; /* decided by the context */
    tf__client_count_take_range(c, &t);
    char *b2 = c ? tf__client_day_body(c) : NULL;
    CHECK(b2 && strstr(b2, "\"recited_takes\":{\"measured\":4,\"context\":1}"), "recited_takes.context counts a take the context decided (%s)",
          b2 && strstr(b2, "\"recited_takes\"") ? strstr(b2, "\"recited_takes\"") : "");
    CHECK(b2 && strstr(b2, "\"juz_ayahs\":{\"1\":7,\"27\":2}"), "a tie take in a juz only when its surah lies in one (%s)",
          b2 && strstr(b2, "\"juz_ayahs\"") ? strstr(b2, "\"juz_ayahs\"") : "");
    CHECK(b2 && !strstr(b2, "\"repeat_ayahs\"") && strstr(b2, "\"ayahs\":11"), "a tie take is never a repeat, its ayahs counted (%s)", b2 ? b2 : "");
    free(b2);
    tf__client_close(c);
  }
  days_key(0);
  return done("recited");
}

static int t_outcome(const char *work) {
  rmrf(work);
  tf_mkdirs(work);
  days_key(1);
  TfRecited r;
  memset(&r, 0, sizeof r);
  const char *rules2[] = {"madd", "qalqalah"};
  const char *bad[] = {"Madd"};
  TfRecitedOutcome inc = {0, 2, rules2, 2}, cor = {1, 0, NULL, 0}, cor_bad = {1, 1, NULL, 0}, inc_bad = {0, 0, bad, 1}, many = {0, 1001, NULL, 0};
  TfRecitedAyah one = {255, {1, 0, NULL, 0}}, out = {300, {1, 0, NULL, 0}};
  TfRecitedAyah two[2] = {{255, {1, 0, NULL, 0}}, {255, {0, 1, NULL, 0}}};
  CHECK(tf__recited_outcome(&r, 2, 255, 257, &inc, &one, 1) == 0, "the contract's example is taken");
  CHECK(tf__recited_outcome(&r, 2, 255, 257, &cor_bad, NULL, 0) == TF_ERR_INVALID_ARGUMENT, "correct with mistakes is refused");
  CHECK(tf__recited_outcome(&r, 2, 255, 257, &inc_bad, NULL, 0) == TF_ERR_INVALID_ARGUMENT, "a bad rule id is refused");
  CHECK(tf__recited_outcome(&r, 2, 255, 257, &many, NULL, 0) == TF_ERR_INVALID_ARGUMENT, "more than 1000 mistakes is refused");
  CHECK(tf__recited_outcome(&r, 2, 255, 257, &cor, &out, 1) == TF_ERR_INVALID_ARGUMENT, "an ayah outside the range is refused");
  CHECK(tf__recited_outcome(&r, 2, 255, 257, &cor, two, 2) == TF_ERR_INVALID_ARGUMENT, "an ayah given twice is refused");
  CHECK(r.whole.set && !r.whole.correct && r.whole.mistakes == 2 && r.n_per == 1, "a refused call changed nothing");
  /* the counts: an app-counted take, judged: 255 correct (its own), 256 and 257 incorrect (the range's) */
  TfClient *c = test_client(work, NULL);
  r.surah = 2, r.ayah_from = 255, r.ayah_to = 257;
  tf__recited_finish(c, NULL, NULL, 0, &r, NULL);
  char day[11], p[1200], *text = NULL;
  size_t n = 0;
  time_t t = time(NULL);
  struct tm g;
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  strftime(day, sizeof day, "%Y-%m-%d", &g);
  snprintf(p, sizeof p, "%s/telemetry-%s.json", work, day);
  CHECK(!tf_read_file(p, &text, &n) && strstr(text, "\"judged\":{\"ayahs\":3,\"correct\":1,\"incorrect\":2,\"mistakes\":2"),
        "17.9's counts are kept on the device: %s", text ? strstr(text, "\"judged\"") : "(none)");
  CHECK(text && strstr(text, "\"rule_checks\":{\"madd\":2,\"qalqalah\":2}"), "each rule of an incorrect ayah adds to rule_checks");
  free(text);
  char *body = tf__client_day_body(c);
  CHECK(body && strstr(body, "\"judged\":{\"ayahs\":3,\"correct\":1,\"incorrect\":2") && strstr(body, "\"judged_surah_ayahs\":{\"2\":3}") &&
            strstr(body, "\"correct_surah_ayahs\":{\"2\":1}") && strstr(body, "\"judged\",\"judged_surah_ayahs\",\"correct_surah_ayahs\",\"judged_tenure\"]"),
        "17.9's counts on the wire by default, listed in reported");
  free(body);
  tf__recited_clear(&r);
  tf__client_close(c);
  days_key(0);
  return done("outcome");
}

static int t_json_leak(void) {
  /* documents that stop inside a nested child that already holds allocations (strings, arrays, objects): before the
   * fix each lost the child; 20,000 of them would leak far more than the bound below */
  static const char *const docs[] = {"{\"a\":[\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\",{\"b\":\"yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\",\"c\":[1,2,3",
                                     "{\"k\":{\"x\":\"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\",\"y\":[[[[\"q\"]]]],\"z\":",
                                     "[{\"a\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},{\"b\":[\"bbbbbbbbbbbbbbbbbbbbbbbb\",tru"};
  for (int i = 0; i < 2000; ++i) tf_json_free(tf_json_parse(docs[i % 3], strlen(docs[i % 3]), NULL)); /* the heap settles */
  const long long before = own_memory();
  for (int i = 0; i < 20000; ++i) {
    TfJson *j = tf_json_parse(docs[i % 3], strlen(docs[i % 3]), NULL);
    CHECK(j == NULL, "a malformed document is refused");
    tf_json_free(j);
  }
  const long long grew = own_memory() - before;
  CHECK(grew < (2LL << 20), "20,000 malformed documents leave %lld bytes behind (under 2 MB; the old parser lost a child each)", grew);
  printf("  memory after 20,000 malformed documents: %+lld bytes\n", grew);
  return done("json_leak");
}


/* ------------------------------------------------------------------ section D: the automatic groups */
#define DAY1 (1789948800LL + 12 * 3600) /* 2026-09-21 12:00 UTC, a Monday */
static char g_bodies[16][16384];
static int g_nb;
static const char *g_cfg_body, *g_cfg_sig, *g_cfg_kid;
static int capture(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  memset(r, 0, sizeof *r);
  if (strstr(q->url, "/v1/events")) {
    if (g_nb < 16) snprintf(g_bodies[g_nb++], sizeof g_bodies[0], "%.*s", (int)q->body_n, (const char *)q->body);
    answer(r, 202, "{}");
  } else if (strstr(q->url, "/v1/attest")) {
    answer(r, 200, "{\"token\":\"qlt_v1.test\",\"expires_in\":3600,\"attested\":false}");
  } else if (strstr(q->url, "/v1/config") && g_cfg_body) {
    answer(r, 200, g_cfg_body);
    snprintf(r->signature, sizeof r->signature, "%s", g_cfg_sig);
    snprintf(r->key_id, sizeof r->key_id, "%s", g_cfg_kid);
  } else {
    answer(r, 200, "{}");
  }
  return 0;
}
/* a send point: every delta sealed now; the one of `day` (today when NULL), or "" */
static const char *flush_body(TfClient *c, const char *day) {
  g_nb = 0;
  tf__client_flush(c, 1);
  char want[40], today[11];
  if (!day) {
    time_t t = (time_t)tf__now_override;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    strftime(today, sizeof today, "%Y-%m-%d", &g);
    day = today;
  }
  snprintf(want, sizeof want, "\"day\":\"%s\"", day);
  for (int i = 0; i < g_nb; ++i)
    if (strstr(g_bodies[i], want)) return g_bodies[i];
  return "";
}
static void a_take(TfClient *c, double secs, int h1, long long ms1, int h2, long long ms2) {
  TfSessionStats st;
  memset(&st, 0, sizeof st);
  st.version = "4.1.0";
  st.build = "0123456789012345678901234567890123456789";
  st.context = "c16_l64";
  st.audio_s = secs;
  st.audio_ms = (long long)(secs * 1000);
  st.chunks = 1;
  st.compute_s = 0.01;
  if (h1 >= 0) st.hour_ms[h1] += ms1;
  if (h2 >= 0) st.hour_ms[h2] += ms2;
  tf__client_session_finished(c, &st);
}
static void a_range(TfClient *c, int source, int surah, int first, int last, const TfRecited *app) {
  TfRecitedCount k;
  memset(&k, 0, sizeof k);
  k.source = source, k.surah = surah, k.first = first, k.last = last, k.app = app;
  tf__client_count_take_range(c, &k);
}
static int has(const char *body, const char *s) { return strstr(body, s) != NULL; }

static int t_learner(const char *work) {
  rmrf(work);
  tf_mkdirs(work);
  tf__client_no_timer = 1;
  tf__learner_test_tz_s = 0; /* local time = UTC: the streak's days are the UTC days here */
  tf_http_set_transport(capture, NULL);
  tf__now_override = DAY1;
  days_key(1);
  TfClient *c = test_client(work, NULL);
  if (!c) return 2;
  /* learner, learner_gap, streak: once per active day */
  a_take(c, 30, -1, 0, -1, 0);
  const char *b = flush_body(c, NULL);
  CHECK(has(b, "\"learner\":{\"active\":1,\"new\":1,\"d1\":0,\"d7\":0,\"d30\":0}") && has(b, "\"learner_gap\":{\"new\":1}") && has(b, "\"streak\":{\"1\":1}"),
        "learner: the first day (%s)", b);
  a_take(c, 30, -1, 0, -1, 0);
  b = flush_body(c, NULL);
  CHECK(*b && !has(b, "\"learner\"") && !has(b, "learner_gap"), "learner: the second delta of a day carries no learner facts");
  tf__now_override = DAY1 + 86400;
  a_take(c, 30, -1, 0, -1, 0);
  b = flush_body(c, NULL);
  CHECK(has(b, "\"d1\":1") && has(b, "\"learner_gap\":{\"1\":1}") && has(b, "\"streak\":{\"2_3\":1}"), "learner: the next day: d1, a gap of 1, a streak of 2 (%s)", b);
  tf__now_override = DAY1 + 7 * 86400;
  a_take(c, 30, -1, 0, -1, 0);
  b = flush_body(c, NULL);
  CHECK(has(b, "\"d7\":1") && has(b, "\"learner_gap\":{\"6\":1}") && has(b, "\"streak\":{\"1\":1}"), "learner: a week on, five days away: d7, a gap of 6, the streak again");
  tf__now_override = DAY1 + 47 * 86400;
  a_take(c, 30, -1, 0, -1, 0);
  b = flush_body(c, NULL);
  CHECK(has(b, "\"learner_gap\":{\"gt_30\":1}") && has(b, "\"new\":0"), "learner: 40 days away: gt_30");
  tf__client_close(c);
  rmrf(work);
  tf_mkdirs(work);
  tf__now_override = DAY1;
  c = test_client(work, NULL);
  /* take_s, session_s, hour_speech_s */
  a_take(c, 90, 9, 1200, 10, 400);
  a_take(c, 30, 10, 600, -1, 0);
  tf__client_session_close(c);
  b = flush_body(c, NULL);
  CHECK(has(b, "\"take_s\":{\"le_5\":0,\"le_15\":0,\"le_30\":1,\"le_60\":0,\"le_120\":1,"), "take_s: 90 s and 30 s (%s)", b);
  CHECK(has(b, "\"session_s\":{\"le_60\":0,\"le_300\":1,"), "session_s: one session of 120 s");
  CHECK(has(b, "\"hour_speech_s\":{\"9\":1.2,\"10\":1}"), "hour_speech_s by the local hour");
  /* a killed session is closed at the next start */
  a_take(c, 700, -1, 0, -1, 0);
  tf__learner_test_kill = 1;
  tf__client_close(c);
  tf__learner_test_kill = 0;
  c = test_client(work, NULL);
  b = flush_body(c, NULL);
  CHECK(has(b, "\"session_s\":{\"le_60\":0,\"le_300\":0,\"le_600\":0,\"le_1200\":1,"), "a session a kill left open is closed at the next start (%s)", b);
  /* juz_ayahs and repeat_ayahs: measured only; a repeat within the session, none after it closed */
  a_range(c, 1, 2, 141, 142, NULL);
  a_range(c, 1, 1, 1, 7, NULL);
  a_range(c, 1, 1, 1, 7, NULL);
  a_range(c, 2, 2, 255, 257, NULL); /* the app's: no juz, no repeat */
  b = flush_body(c, NULL);
  CHECK(has(b, "\"juz_ayahs\":{\"1\":15,\"2\":1}") && has(b, "\"repeat_ayahs\":7"), "juz_ayahs and repeat_ayahs (%s)", b);
  tf__client_session_close(c);
  a_range(c, 1, 1, 1, 7, NULL);
  b = flush_body(c, NULL);
  CHECK(!has(b, "repeat_ayahs"), "a new session: no repeat");
  /* 17.9: first and repeat, judged_tenure, listed in reported */
  TfRecited app;
  memset(&app, 0, sizeof app);
  TfPerAyah per = {2, {1, 0, 1, 0, {{0}}}};
  app.surah = 112, app.ayah_from = 1, app.ayah_to = 4;
  app.whole.set = 1, app.whole.correct = 1;
  app.per = &per, app.n_per = 1;
  a_range(c, 2, 112, 1, 4, &app);
  a_range(c, 2, 112, 1, 4, &app);
  b = flush_body(c, NULL);
  CHECK(has(b, "\"judged\":{\"ayahs\":8,\"correct\":6,\"incorrect\":2,\"mistakes\":2,\"first\":4,\"first_correct\":3,\"repeat\":4,\"repeat_correct\":3}"),
        "17.9: first and repeat in the session (%s)", b);
  CHECK(has(b, "\"judged_tenure\":{\"w1\":{\"judged\":8,\"correct\":6}}"), "judged_tenure on the wire");
  CHECK(has(b, "\"judged_surah_ayahs\",\"correct_surah_ayahs\",\"judged_tenure\""), "judged_tenure is in reported (%s)", strstr(b, "\"reported\""));
  tf__client_close(c);
  /* the journal's rules: tenure boundaries, Mondays, the weekly trend */
  TfLearner j;
  memset(&j, 0, sizeof j);
  snprintf(j.first, sizeof j.first, "2026-09-21"); /* tenure reads the first active day only */
  static const struct {
    int n, want;
  } ten[] = {{0, 0}, {6, 0}, {7, 1}, {27, 1}, {28, 2}, {89, 2}, {90, 3}};
  for (size_t i = 0; i < sizeof ten / sizeof ten[0]; ++i) {
    char d[11];
    tf__day_str(tf__day_num("2026-09-21") + ten[i].n, d);
    CHECK(tf__learner_tenure(&j, d) == ten[i].want, "tenure on day %d", ten[i].n);
  }
  char m[11];
  tf__day_monday("2026-11-14", m);
  CHECK(!strcmp(m, "2026-11-09"), "the Monday of 2026-11-14: %s", m);
  tf__day_monday("2026-11-16", m);
  CHECK(!strcmp(m, "2026-11-16"), "a Monday is its own");
  static const struct {
    int lj, lc, pj, pc;
    const char *want;
  } tr[] = {{40, 36, 40, 30, "better"}, {40, 30, 40, 36, "worse"}, {40, 31, 40, 30, "same"}, {19, 19, 40, 30, "not_enough"}};
  for (size_t i = 0; i < sizeof tr / sizeof tr[0]; ++i) {
    TfLearner t;
    memset(&t, 0, sizeof t);
    t.n_weeks = 2;
    snprintf(t.week[0], 11, "2026-11-02"); /* the week that ended */
    t.week_judged[0] = tr[i].lj, t.week_correct[0] = tr[i].lc;
    snprintf(t.week[1], 11, "2026-10-26"); /* one of the four before */
    t.week_judged[1] = tr[i].pj, t.week_correct[1] = tr[i].pc;
    char v[16] = "", last[11];
    const int got = tf__learner_trend(&t, "2026-11-10", v, last);
    CHECK(got && !strcmp(v, tr[i].want) && !strcmp(last, "2026-11-02"), "trend %s: got %s", tr[i].want, v);
    if (i == 0) {
      snprintf(t.trend_week, 11, "%s", last);
      CHECK(!tf__learner_trend(&t, "2026-11-15", v, last), "a trend once per week");
    }
  }
  {
    TfLearner t;
    memset(&t, 0, sizeof t);
    char v[16], last[11];
    CHECK(!tf__learner_trend(&t, "2026-11-10", v, last), "an install that judges nothing sends no trend");
  }
  /* the trend on the wire: judged ayahs in two weeks, sealed in the week after */
  rmrf(work);
  tf_mkdirs(work);
  tf__now_override = DAY1 - 14 * 86400; /* two weeks before: the four weeks before the last one */
  c = test_client(work, NULL);
  app.whole.correct = 1;
  app.n_per = 0;
  for (int i = 0; i < 6; ++i) a_range(c, 2, 112, 1, 4, &app); /* 24 judged, all correct */
  flush_body(c, NULL);
  tf__now_override = DAY1 - 7 * 86400; /* the last week: 24 judged, half correct */
  app.whole.correct = 0;
  for (int i = 0; i < 3; ++i) a_range(c, 2, 112, 1, 4, &app);
  app.whole.correct = 1;
  for (int i = 0; i < 3; ++i) a_range(c, 2, 112, 1, 4, &app);
  flush_body(c, NULL);
  tf__now_override = DAY1;
  a_take(c, 30, -1, 0, -1, 0);
  b = flush_body(c, NULL);
  CHECK(has(b, "\"trend\":{\"worse\":1}") && has(b, "\"trend\"]"), "the weekly trend on the wire, listed in reported (%s)", b);
  a_take(c, 30, -1, 0, -1, 0);
  b = flush_body(c, NULL);
  CHECK(!has(b, "\"trend\""), "the trend once per week");
  tf__client_close(c);
  tf__now_override = 0;
  tf__learner_test_tz_s = -999999;
  tf_http_set_transport(tf_test_net, NULL);
  days_key(0);
  return done("learner");
}

/* the switches: a signed config with every group off sends none of them; the same config without them sends all */
static int t_switches(const char *fixture, const char *work) {
  TfJson *fx = load_json(fixture, "flow.json");
  if (!fx) return 2;
  tf__client_no_timer = 1;
  tf__learner_test_tz_s = 0;
  tf_http_set_transport(capture, NULL);
  days_key(1);
  static const char *const groups[] = {"\"juz_ayahs\"", "\"hour_speech_s\"", "\"take_s\"", "\"session_s\"", "\"learner\"", "\"learner_gap\"", "\"streak\"",
                                       "\"repeat_ayahs\"", "\"recited_takes\"", "\"judged\"", "\"judged_surah_ayahs\"", "\"correct_surah_ayahs\"",
                                       "\"judged_tenure\""};
  for (int off = 0; off < 2; ++off) {
    char w[1100];
    snprintf(w, sizeof w, "%s/%s", work, off ? "off" : "on");
    rmrf(w);
    tf_mkdirs(w);
    tf__now_override = DAY1;
    g_cfg_body = S(fx, off ? "config_off_body" : "config_body");
    g_cfg_sig = S(fx, off ? "config_off_sig" : "config_sig");
    g_cfg_kid = S(fx, "config_kid");
    TfClient *c = test_client(w, S(fx, "signer_hex"));
    CHECK(c && tf__client_refresh_config(c) == 0 && tf__client_config_json(c), "the signed config is in force (%s)", off ? "groups off" : "no switch");
    CHECK(tf__client_metric_on(c, "juz") == !off && tf__client_metric_on(c, "judged") == !off, "flags.metrics read as the config says");
    a_take(c, 30, 9, 1000, -1, 0);
    a_range(c, 1, 1, 1, 7, NULL);
    a_range(c, 1, 1, 1, 7, NULL);
    TfRecited app;
    memset(&app, 0, sizeof app);
    app.surah = 1, app.ayah_from = 1, app.ayah_to = 7, app.whole.set = 1, app.whole.correct = 1;
    TfRecitedCount k;
    memset(&k, 0, sizeof k);
    k.source = 1, k.surah = 1, k.first = 1, k.last = 7, k.take_id = 77, k.app = &app;
    tf__client_count_take_range(c, &k);
    tf__client_session_close(c);
    const char *b = flush_body(c, NULL);
    int present = 0;
    for (size_t i = 0; i < sizeof groups / sizeof groups[0]; ++i) present += has(b, groups[i]);
    if (off)
      CHECK(present == 0, "groups off: none of their fields is sent (%s)", b);
    else
      CHECK(present == (int)(sizeof groups / sizeof groups[0]), "no switch: every group is sent (%d of %d): %s", present,
            (int)(sizeof groups / sizeof groups[0]), b);
    CHECK(has(b, "\"ayahs\":21") && has(b, "\"surah_ayahs\":{\"1\":21}"), "the measured ayahs are not a group: always sent");
    tf__client_close(c);
  }
  g_cfg_body = NULL;
  tf__now_override = 0;
  tf__learner_test_tz_s = -999999;
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(fx);
  days_key(0);
  return done("switches");
}

/* ------------------------------------------------------------------ conformance */
static const char *const kOut[12] = {"attr_face", "attr_ghunna", "attr_qalqalah", "attr_tafkheem", "boundary", "conf",
                                     "gem", "id_log_probs", "inter1_log_probs", "inter4_log_probs", "log_probs", "timing_log_probs"};
#define FNV0 1469598103934665603ull
static uint64_t fnv1a(uint64_t h, const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}
typedef struct {
  uint64_t *fnv;
  int chunks, cap, frames, finals, last_final;
  uint64_t logp;
} ConfRun;
static void on_chunk(void *user, const TfChunk *c) {
  ConfRun *r = (ConfRun *)user;
  if (r->chunks == r->cap) {
    r->cap = r->cap ? 2 * r->cap : 64;
    r->fnv = (uint64_t *)realloc(r->fnv, (size_t)r->cap * 12 * sizeof(uint64_t));
  }
  for (int k = 0; k < 12; ++k) {
    TfHead hd;
    const int have = tf_heads_get(c->heads, kOut[k], &hd) == 0;
    const size_t n = have ? (size_t)hd.frames * hd.classes : 0;
    r->fnv[(size_t)r->chunks * 12 + k] = have ? fnv1a(FNV0, hd.values, n * sizeof(float)) : 0;
    if (!strcmp(kOut[k], "log_probs") && have) {
      r->logp = fnv1a(r->logp, hd.values, n * sizeof(float));
      r->frames += hd.frames;
    }
  }
  tf__clear_error();
  if (c->decoded && c->decoded->is_final) r->finals++;
  r->last_final = c->decoded && c->decoded->is_final;
  r->chunks++;
}
static int t_conformance(const char *fixture, const char *list, const char *outp, const char *work) {
  TfJson *fx = flow_setup(fixture);
  if (!fx) return 2;
  fake_reset(LEASE_OK);
  TfClient *c = flow_client(work);
  const double t0 = tf__now_s();
  TfModel *m = c ? tf_client_model(c, NULL) : NULL;
  if (!m) {
    fprintf(stderr, "model: %s (%s)\n", tf_last_error(), tf_error_name(tf_last_error_code()));
    return 2;
  }
  printf("conformance: the encrypted test build downloaded, installed and opened through the Core in %.1f s (%s, key store %s)\n",
         tf__now_s() - t0, tf_model_version(m), tf__ikey_kind_name(tf__pm_ikey(tf__model_pm(m))));
  CHECK(g_f.attest_keys == 1 && !g_f.bad_attest_sig && g_f.leases == 1 && !g_f.bad_lease && !g_f.bad_lease_sig,
        "conformance: one attest and one lease, each as the server checks it");
  ConfRun r;
  memset(&r, 0, sizeof r);
  TfSessionOptions so;
  memset(&so, 0, sizeof so);
  so.heads = "all";
  so.on_chunk = on_chunk;
  so.user = &r;
  FILE *lf = fopen(list, "r"), *out = fopen(outp, "w");
  if (!lf || !out) return 2;
  char line[4096];
  int clips = 0, greedy_ok = 0, top2_ok = 0;
  double audio_s = 0, wall = 0;
  while (fgets(line, sizeof line, lf)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
    if (!len) continue;
    char *pcm = NULL;
    size_t bytes = 0;
    if (tf_read_file(line, &pcm, &bytes)) return 2;
    const int n = (int)(bytes / 4);
    TfSession *s = tf_session_create(m, &so);
    if (!s) {
      fprintf(stderr, "session: %s\n", tf_last_error());
      return 2;
    }
    r.chunks = r.frames = r.finals = 0;
    r.logp = FNV0;
    const double t1 = tf__now_s();
    for (int at = 0; at < n; at += 320)
      if (tf_session_push(s, (const float *)pcm + at, n - at < 320 ? n - at : 320) < 0) return 3;
    if (tf_session_finish(s) < 0) return 3;
    wall += tf__now_s() - t1;
    audio_s += n / 16000.0;
    free(pcm);
    CHECK(r.finals == 1 && r.last_final, "%s: exactly the last chunk is final", line);
    const TfHeads *h = tf_session_heads(s);
    if (!h) return 3;
    const TfDecoded *d = tf_heads_decoded(h);
    int g[4096];
    const int ng = qlc_stream_greedy(tf__session_stream(s), g, 4096);
    int same = ng == d->n_tokens;
    for (int i = 0; same && i < ng; ++i) same = g[i] == d->tokens[i].id;
    greedy_ok += same;
    CHECK(same, "%s: the decoded tokens are the Core's greedy ids", line);
    /* the top-2 the SDK ranks (tf_rank.c) equals a plain C scan of log_probs, margin in double */
    TfHead lp;
    tf_heads_get(h, "log_probs", &lp);
    int bad = d->n_frames != lp.frames;
    for (int t = 0; !bad && t < d->n_frames; ++t) {
      const float *row = lp.values + (size_t)t * TF_VOCAB;
      int a1 = 0;
      for (int i = 1; i < TF_VOCAB; ++i)
        if (row[i] > row[a1]) a1 = i;
      int b1 = a1 == 0 ? 1 : 0;
      for (int i = 0; i < TF_VOCAB; ++i)
        if (i != a1 && row[i] > row[b1]) b1 = i;
      bad = d->frames[t].top1 != a1 || d->frames[t].runner_up != b1 || d->frames[t].margin != (double)row[a1] - (double)row[b1];
    }
    top2_ok += !bad;
    const char *base = strrchr(line, '/');
    const char *b2 = strrchr(line, '\\');
    if (b2 > base) base = b2;
    base = base ? base + 1 : line;
    char clip[256];
    snprintf(clip, sizeof clip, "%s", base);
    char *dot = strrchr(clip, '.');
    if (dot) *dot = 0;
    fprintf(out, "{\"clip\": \"%s\", \"chunks\": %d, \"tokens\": [", clip, r.chunks);
    for (int i = 0; i < d->n_tokens; ++i) fprintf(out, i ? ", %d" : "%d", d->tokens[i].id);
    fprintf(out, "], \"logp_fnv\": \"%016llx\", \"out_fnv\": {", (unsigned long long)r.logp);
    for (int k = 0; k < 12; ++k) {
      fprintf(out, "%s\"%s\": [", k ? ", " : "", kOut[k]);
      for (int q = 0; q < r.chunks; ++q) fprintf(out, q ? ", \"%016llx\"" : "\"%016llx\"", (unsigned long long)r.fnv[(size_t)q * 12 + k]);
      fprintf(out, "]");
    }
    fprintf(out, "}}\n");
    tf_session_destroy(s);
    clips++;
  }
  fclose(lf);
  fclose(out);
  printf("conformance run: %d clips, greedy == decoded on %d, top-2 == C scan on %d, RTF %.3f (%.1f s audio)\n", clips, greedy_ok, top2_ok,
         audio_s > 0 ? wall / audio_s : 0, audio_s);
  /* the reference data (R3): missing until fetched; fetched (downloaded, installed by the Core with the engine
   * closed); then every ayah's units and words through the Core, fingerprinted as quranlab-core's qlc_bench --reference
   * does (n and the word count, the units, each word's first unit, as ints), for test/reference_fnv.py's plaintext
   * reading of the same table */
  {
    TfReference *ref = tf_model_reference(m);
    TfUnits u;
    CHECK(ref && !tf_reference_is_downloaded(ref) && tf_reference_units(ref, 1, 1, &u) == TF_ERR_REFERENCE_MISSING,
          "reference: the build has one, missing until fetched");
    const int frc = ref ? tf_reference_fetch(ref) : -1;
    CHECK(frc == 0 && tf_reference_is_downloaded(ref), "reference: fetched and installed (%s)", frc ? tf_last_error() : "ok");
    uint64_t h = FNV0;
    int ayahs = 0, bad = 0;
    for (int su = 1; su <= 114 && frc == 0; ++su)
      for (int a = 1; a <= tf__ayah_count(su); ++a) {
        if (tf_reference_units(ref, su, a, &u)) {
          bad++;
          continue;
        }
        int starts[512];
        for (int k = 0; k < u.n; ++k)
          if (u.word[k] >= 0 && u.word[k] < 512 && (k == 0 || u.word[k] != u.word[k - 1])) starts[u.word[k]] = k;
        const int head2[2] = {u.n, u.n_words};
        h = fnv1a(h, head2, sizeof head2);
        h = fnv1a(h, u.ids, (size_t)u.n * sizeof(int));
        h = fnv1a(h, starts, (size_t)u.n_words * sizeof(int));
        ayahs++;
      }
    CHECK(ayahs == 6236 && !bad, "reference: every ayah read (%d, %d refused)", ayahs, bad);
    CHECK(tf_reference_units(ref, 115, 1, &u) == TF_ERR_INVALID_ARGUMENT && tf_reference_units(ref, 1, 8, &u) == TF_ERR_INVALID_ARGUMENT,
          "reference: a surah or ayah that does not exist");
    printf("reference: %d ayahs, ref_fnv %016llx\n", ayahs, (unsigned long long)h);
  }
  /* nothing of the plaintext on disk after a whole run: the folder holds containers, the state and public files */
  tf_model_close(m);
  tf_client_destroy(c);
  tf_http_set_transport(tf_test_net, NULL);
  tf_json_free(fx);
  return done("conformance");
}

#include "tf40_eval_tests.inc"
#include "tf40_errors_tests.inc"
#include "tf40_endpoints_tests.inc"
#include "tf40_lanes_tests.inc"
#include "tf40_qos_tests.inc"
#include "tf40_nowait_tests.inc"
#include "tf40_load_tests.inc"
#include "tf40_requests_tests.inc"
#include "tf40_lifetime_tests.inc"

int main(int argc, char **argv) {
  tf_http_set_transport(tf_test_net, NULL); /* no test reaches a host but this machine (tf_test_net.h) */
  if (argc < 4 && !(argc >= 3 && !strcmp(argv[1], "json_leak"))) {
    fprintf(stderr, "usage: tf40_tests <test> <vectors|fixture|-> <work dir> (see the file's comment)\n");
    return 2;
  }
  const char *t = argv[1];
  if (!strcmp(t, "ikey")) return t_ikey(argv[3]);
  if (!strcmp(t, "lease")) return t_lease(argv[2], argv[3]);
  if (!strcmp(t, "clock")) return t_clock(argv[2], argv[3]);
  if (!strcmp(t, "container")) return t_container(argv[2], argv[3]);
  if (!strcmp(t, "attest")) return t_attest(argv[2], argv[3]);
  if (!strcmp(t, "flow")) return t_flow(argv[2], argv[3]);
  if (!strcmp(t, "lease_clock")) return t_lease_clock(argv[2], argv[3]);
  if (!strcmp(t, "grace") && argc >= 5) return t_grace(argv[2], argv[3], argv[4]);
  if (!strcmp(t, "grace_vectors") && argc >= 5) return t_grace_vectors(argv[2], argv[3], argv[4]);
  if (!strcmp(t, "recited")) return t_recited(argv[3]);
  if (!strcmp(t, "outcome")) return t_outcome(argv[3]);
  if (!strcmp(t, "json_leak")) return t_json_leak();
  if (!strcmp(t, "learner")) return t_learner(argv[3]);
  if (!strcmp(t, "switches")) return t_switches(argv[2], argv[3]);
  if (!strcmp(t, "conformance") && argc >= 6) return t_conformance(argv[2], argv[3], argv[4], argv[5]);
  if (!strcmp(t, "piece_order") && argc >= 5) return t_piece_order(argv[2], argv[3], argv[4]);
  if (!strcmp(t, "flac")) return t_flac(argv[2], argv[3]);
  if (!strcmp(t, "share_ask")) return t_share_ask(argv[2], argv[3]);
  if (!strcmp(t, "share_moments")) return t_share_moments(argv[2], argv[3]);
  if (!strcmp(t, "share_flac")) return t_share_flac(argv[2], argv[3]);
  if (!strcmp(t, "eval_units")) return t_eval_units(argv[2], argv[3]);
  if (!strcmp(t, "eval") && argc >= 5) return t_eval(argv[2], argv[3], argv[4]);
  if (!strcmp(t, "errors")) return t_errors(argv[2], argv[3]);
  if (!strcmp(t, "errors_flow")) return t_errors_flow(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);
  if (!strcmp(t, "app27")) return t_app27(argv[2], argv[3]);
  if (!strcmp(t, "endpoints") && argc >= 5) return t_endpoints(argv[2], argv[3], argv[4]);
  if (!strcmp(t, "loopback")) return t_loopback(argv[2], argv[3]);
  if (!strcmp(t, "lanes")) return t_lanes(argv[2], argv[3]);
  if (!strcmp(t, "qos")) return t_qos();
  if (!strcmp(t, "no_wait") && argc >= 6) return t_no_wait(argv[2], argv[3], argv[4], argv[5]);
  if (!strcmp(t, "load")) return t_load(argv[2], argv[3]);
  if (!strcmp(t, "requests")) return t_requests(argv[2], argv[3]);
  if (!strcmp(t, "hosts") && argc >= 5) return t_hosts(argv[2], argv[3], argv[4]);
  if (!strcmp(t, "lifetime") && argc >= 4) return t_lifetime(argv[2], argv[3]);
  if (!strcmp(t, "lifetime_open") && argc >= 4) return t_lifetime_open(argv[2], argv[3]);
  fprintf(stderr, "unknown test %s\n", t);
  return 2;
}
