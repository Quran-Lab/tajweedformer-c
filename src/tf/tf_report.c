/* Error reports. Every TfError that reaches the app (but the five codes of the app's own misuse) and
 * every silent failure of 24.1's list becomes one POST /v1/issues body of kind "error": the stage of the CAUSE, a
 * reason token, the HTTP status, the platform error's name and code, the count, the first launch and the first report,
 * and the device's memory tier, free space and ABI. One report per install, UTC day and signature, at most 16
 * signatures a day and 32 queued, 7 days kept (24.5); sent at the launch after the config, at every telemetry send
 * point and 30 s after a new report (24.6).
 *
 * Never in a report: a message, a path, a URL, an identifier, an IP, a time finer than the UTC day. Every string
 * field is a closed set or a token checked here; a name or code that is not a token is left out, never cut.
 *
 * How a C failure finds its cause: the place that sees it (a request's answer, the transport, a verification, the
 * Core, the engine) records it on the thread (tf__cause). The next catalogue code set on the thread (tf__fail) takes
 * that cause when the cause can lead to it by 24.4 (tf__rep_code_for for any of the three on-device states), and
 * tf__raise, where a public call hands a code to the app, makes the report with it. A code no cause fits is reported
 * with the stage and reason its catalogue entry stands for (default_cause). The queue and the reported list are
 * errors.json in the storage folder (tools/errors40.py Reporter, replayed by tf40_tests errors). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tf_internal.h"
#include "tf_thread.h"

/* ------------------------------------------------------------------ 24.3, 24.4: the code the app gets */
static int is(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static int one_of(const char *s, const char *const *list) {
  for (; s && *list; ++list)
    if (!strcmp(s, *list)) return 1;
  return 0;
}

const char *tf__rep_code_for(const char *stage, const char *reason, int status, int on_device, const char *ec) {
  (void)status;
  if (!stage || !reason) return "none";
  if (!ec) ec = "";
  if (is(stage, "sharing") || is(stage, "events")) return "none";
  if (is(reason, "failover")) return "none"; /* 4.0.1 (28.7): answered on another host after the first was unreachable */
  static const char *const unattested[] = {"unsupported", "attestation_failed", "integrity_failed", "turnstile", NULL};
  /* 24.3 (69326fb): a token request refused 403 lease_integrity_failed is server_rejected, as at the lease */
  if (is(stage, "attest") && is(reason, "integrity_failed") && on_device != TF_REP_COMPLETE) return "server_rejected";
  if (is(stage, "attest") && (on_device == TF_REP_COMPLETE || one_of(reason, unattested))) return "none"; /* 24.4 rule 3a */
  if (is(stage, "storage")) {
    if (is(reason, "quota")) return "storage_full";
    static const char *const failed[] = {"read", "write", "corrupt", NULL};
    return one_of(reason, failed) ? "storage_failed" : "none";
  }
  if (is(stage, "verify")) {
    if (is(reason, "hash")) return "checksum_mismatch";
    if (is(reason, "signature")) return "signature_invalid";
    if (is(reason, "missing")) return "model_missing";
    if (is(reason, "invalid")) return "model_invalid";
    if (is(reason, "incompatible")) return "model_incompatible";
    return "none";
  }
  if (is(stage, "runtime")) {
    static const char *const lacks[] = {"wasm_unsupported", "simd", "threads", "webcrypto", "ed25519", "native_library", NULL};
    if (one_of(reason, lacks)) return "runtime_unsupported";
    return is(reason, "worker") ? "none" : "engine_failed";
  }
  if (is(stage, "model_open")) {
    if (is(reason, "incompatible")) return "model_incompatible";
    if (is(reason, "invalid") || is(reason, "decrypt")) return "model_invalid";
    if (is(reason, "missing")) return "model_missing";
    return "engine_failed";
  }
  if (is(stage, "accelerator")) return is(reason, "unavailable") ? "runtime_unsupported" : "engine_failed";
  if (is(stage, "audio")) {
    if (is(reason, "permission_denied")) return "microphone_denied";
    return is(reason, "interrupted") ? "interrupted" : "microphone_unavailable";
  }
  if (is(stage, "session")) return "engine_failed";
  /* the network stages: config, register, attest (rule 3a: the token gates the config), lease, manifest, download */
  static const char *const key_reasons[] = {"key_revoked", "key_unknown", NULL};
  if (is(stage, "lease")) {
    /* rule 3: only a stop the Core verified is a stop; rule 1: no valid lease and no answer is needs_connection,
     * whatever is on the device (a protected build does not open without a lease) */
    if (is(reason, "grace")) return "none"; /* 18.4.7.1: an expired lease opened in its grace, reported silently */
    if (is(reason, "stopped")) return one_of(ec, key_reasons) ? "invalid_key" : "server_rejected";
    if (is(reason, "refused")) return is(ec, "sdk_too_old") ? "sdk_too_old" : "server_rejected";
    if (is(reason, "network") || is(reason, "expired")) return "needs_connection";
    if (one_of(reason, unattested) || is(reason, "invalid")) return "server_rejected";
    return "download_failed";
  }
  if (is(reason, "refused")) {
    static const char *const key[] = {"key_revoked", "key_unknown", "invalid_key", "unauthorized", NULL};
    if (one_of(ec, key)) return "invalid_key";
    return is(ec, "sdk_too_old") ? "sdk_too_old" : "server_rejected";
  }
  static const char *const goes_on[] = {"config", "register", "attest", "manifest", NULL};
  if (on_device == TF_REP_COMPLETE && one_of(stage, goes_on)) return "none"; /* the call goes on from what is here */
  if (is(stage, "register")) return "none";
  if (is(reason, "network")) return on_device == TF_REP_NONE ? "needs_connection" : "download_failed";
  if (is(stage, "config") && is(reason, "unavailable")) return "model_unavailable";
  if (is(stage, "manifest")) {
    if (is(reason, "signature")) return "signature_invalid";
    if (is(reason, "parse")) return "model_invalid";
    if (is(reason, "incompatible")) return "model_incompatible";
  }
  if (is(stage, "download")) {
    if (is(reason, "hash")) return "checksum_mismatch";
    if (is(reason, "signature")) return "signature_invalid";
    if (is(reason, "quota")) return "storage_full";
    if (is(reason, "storage")) return "storage_failed";
  }
  return "download_failed";
}

/* 24.1: never the app's own misuse, never a silent plain network failure */
int tf__rep_reported(const char *reason, const char *code) {
  static const char *const misuse[] = {"invalid_argument", "client_exists", "session_open", "session_closed", "cancelled", NULL};
  if (!code || one_of(code, misuse)) return 0;
  return !(is(code, "none") && is(reason, "network"));
}

/* 24.8: a failed download's failure_reasons name */
const char *tf__rep_failure_reason(const char *reason) {
  static const char *const net[] = {"network", "timeout", NULL};
  static const char *const http[] = {"http", "rate_limited", "refused", "redirect", "invalid_answer", NULL};
  static const char *const store[] = {"storage", "quota", NULL};
  if (one_of(reason, net)) return "network";
  if (one_of(reason, http)) return "http";
  if (is(reason, "hash")) return "hash_mismatch";
  if (is(reason, "signature")) return "signature";
  return one_of(reason, store) ? "storage" : "other";
}

/* 24.5: the signature string, and the first 16 bytes of its SHA-256 in hex (the server's group key) */
void tf__rep_signature(const char *stage, const char *reason, const char *code, int status, char *out, size_t n) {
  snprintf(out, n, "error\n%s\n%s\n%s\n%d", stage ? stage : "", reason ? reason : "", code ? code : "", status > 0 ? status : 0);
}
void tf__rep_signature_hex(const char *stage, const char *reason, const char *code, int status, char hex[33]) {
  char s[200];
  uint8_t h[32];
  tf__rep_signature(stage, reason, code, status, s, sizeof s);
  tf_sha256(s, strlen(s), h);
  tf_hex_encode(h, 16, hex);
}

/* 24.7 (and the native volume's free space the same way) */
const char *tf__rep_storage_free(long long b) {
  return b < 100000000LL ? "lt_100m" : b < 500000000LL ? "lt_500m" : b < 1000000000LL ? "lt_1g" : b < 5000000000LL ? "lt_5g" : "ge_5g";
}

/* the tokens of 24.2 (a field that is not one is left out): reason [a-z][a-z0-9_]{0,31}, error_name
 * [A-Za-z][A-Za-z0-9_.:$-]{0,47}, error_code [A-Za-z0-9_.:-]{1,32} */
static int chars_ok(const char *s, size_t max, const char *punct) {
  if (!s || !*s || strlen(s) > max) return 0;
  for (const char *p = s; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr(punct, *p))) return 0;
  return 1;
}
static int reason_ok(const char *s) {
  if (!s || !(s[0] >= 'a' && s[0] <= 'z') || strlen(s) > 32) return 0;
  for (const char *p = s; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return 0;
  return 1;
}
static int name_ok(const char *s) { return s && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')) && chars_ok(s, 48, "_.:$-"); }
static int code_ok(const char *s) { return chars_ok(s, 32, "_.:-"); }

/* A 403 lease_integrity_failed (PROTECTED_MODEL.md 18.4.10) names the failed check with `why` (verdict, downgrade,
 * core) and, for a verdict, `check` (ERRORS_40.md 24.3, contract 69326fb; as the Android SDK, sdk-4.0.0 38ffa96).
 * Only tokens of 24.2 are read: a why or check that is not one is left out, never echoed. */
int tf__integrity_code(const char *why, const char *check, char out[33]) {
  out[0] = 0;
  if (!reason_ok(why)) return 0;
  if (reason_ok(check) && strlen(why) + 1 + strlen(check) <= 32) snprintf(out, 33, "%s:%s", why, check);
  else snprintf(out, 33, "%s", why);
  return 1;
}
void tf__integrity_detail(const char *why, const char *check, char *out, size_t n) {
  const char *w = reason_ok(why) ? why : NULL, *c = reason_ok(check) ? check : NULL;
  const char *words = "the install could not be verified";
  if (is(w, "verdict")) {
    words = is(c, "boot")       ? "this phone's bootloader is unlocked or its boot is not verified, and Quran Lab gives such a phone no lease"
            : is(c, "digest")   ? "the app's signing certificate is not one declared for this app on sdk.quranlab.ai (a device lab that re-signs the APK needs its digest on a development key)"
            : is(c, "software") ? "the phone's key attestation does not come from secure hardware"
            : is(c, "origin")   ? "the install's key was not made inside the phone's secure hardware"
            : is(c, "purpose")  ? "the install's key does not have the purpose the SDK made it with"
            : is(c, "package") || is(c, "package_mismatch") ? "the attested package name is not this app's"
            : is(c, "recorded") ? "a failed check of this install, recorded in the last 24 hours, still stands"
            : is(c, "device_integrity") ? "Play Integrity does not confirm this device's integrity"
            : is(c, "unlicensed")       ? "Google Play says this install is not licensed"
            : c && !strncmp(c, "app_", 4) ? "Play Integrity does not recognize this app as Google Play delivers it"
                                          : "the install's attestation did not pass";
  } else if (is(w, "downgrade")) {
    words = "this install attested with a stronger check before and did not this time";
  } else if (is(w, "core")) {
    words = "the Quran Lab Core in this app could not be verified";
  }
  snprintf(out, n, "%s (lease_integrity_failed%s%s%s%s)", words, w ? ", why " : "", w ? w : "", c ? ", check " : "", c ? c : "");
}
/* ------------------------------------------------------------------ the cause on this thread */
typedef struct {
  int set;   /* a cause is recorded */
  int bound; /* the catalogue code that took it (0: none yet) */
  int force; /* the next catalogue code takes it whatever 24.4 says (a code the contract gives that pair elsewhere) */
  int status, retries;
  char stage[16], reason[33], name[49], code[33];
  char role[8]; /* 4.0.1 (28.7): the role of the request the cause is, "" for none */
  int fallback; /* 0 none, 1 false, 2 true */
} Cause;
static _Thread_local Cause t_cause;
typedef char tf__cause_fits_err_state[sizeof(Cause) <= sizeof(((TfErrState *)0)->cause) ? 1 : -1];
_Thread_local int tf__app_call;
/* 4.0.1 (ENDPOINTS_40.md 28.7, ERRORS_40.md 24.2): a cause that is a request to Quran Lab's hosts carries the role of
 * that request (the thread's last, tf__ep_note) and whether more than one host was asked; never a host name */
static void host_of(Cause *k) {
  static const char *const net[] = {"config", "register", "attest", "lease", "manifest", "download", "sharing", "events", NULL};
  static const char *const local[] = {"storage", "quota", NULL};
  int role, fb;
  k->role[0] = 0;
  k->fallback = 0;
  if (!one_of(k->stage, net) || one_of(k->reason, local) || !tf__ep_noted(&role, &fb)) return;
  snprintf(k->role, sizeof k->role, "%s", tf__ep_role_name(role));
  k->fallback = fb ? 2 : 1;
}

void tf__cause_clear(void) { memset(&t_cause, 0, sizeof t_cause); }
/* 4.0.1: the cause crosses threads with the error it belongs to (tf__err_save, tf__err_restore) */
void tf__cause_save(void *out, size_t n) {
  if (n >= sizeof t_cause) memcpy(out, &t_cause, sizeof t_cause);
}
void tf__cause_restore(const void *in, size_t n) {
  if (n >= sizeof t_cause) memcpy(&t_cause, in, sizeof t_cause);
}
void tf__cause(const char *stage, const char *reason, int status, const char *error_name, const char *error_code) {
  tf__cause_clear();
  if (!stage || strlen(stage) >= sizeof t_cause.stage || !reason_ok(reason)) return;
  t_cause.set = 1;
  snprintf(t_cause.stage, sizeof t_cause.stage, "%s", stage);
  snprintf(t_cause.reason, sizeof t_cause.reason, "%s", reason);
  t_cause.status = status >= 100 && status <= 599 ? status : 0;
  t_cause.retries = -1;
  if (name_ok(error_name)) snprintf(t_cause.name, sizeof t_cause.name, "%s", error_name);
  if (code_ok(error_code)) snprintf(t_cause.code, sizeof t_cause.code, "%s", error_code);
  host_of(&t_cause);
}
void tf__cause_force(void) { t_cause.force = t_cause.set; }
/* the code just set on the thread has this cause (where the cause is known only after the code was set) */
void tf__cause_now(int code, const char *stage, const char *reason, int status, const char *error_name, const char *error_code) {
  tf__cause(stage, reason, status, error_name, error_code);
  if (t_cause.set && code < 0) t_cause.bound = code;
}
void tf__cause_retries(int n) {
  if (t_cause.set) t_cause.retries = n < 0 ? -1 : n > 1000 ? 1000 : n;
}
const char *tf__cause_reason(void) { return t_cause.set ? t_cause.reason : ""; }

/* 1 when the cause can lead to this code (24.4) on some state of the device */
static int fits(const Cause *k, const char *name) {
  for (int d = TF_REP_NONE; d <= TF_REP_COMPLETE; ++d)
    if (is(tf__rep_code_for(k->stage, k->reason, k->status, d, k->code), name)) return 1;
  return 0;
}
/* tf_util.c, whenever a catalogue code is set on the thread */
void tf__cause_bind(int code) {
  const char *name = tf_error_name(code);
  if (!t_cause.set || !name || code >= 0) return;
  if (t_cause.force || fits(&t_cause, name)) {
    t_cause.bound = code;
    t_cause.force = 0;
  }
}

/* 24.3: the reason of an answer that failed, and its error code: the body's reason when it is a stop reason of 15.3,
 * else its error (a token, or nothing) */
const char *tf__rep_answer_reason(int status, const char *body, size_t n, char code[33]) {
  code[0] = 0;
  TfJson *j = body && n ? tf_json_parse(body, n, NULL) : NULL;
  const char *e = tf_json_str(tf_json_get(j, "error")), *why = tf_json_str(tf_json_get(j, "reason"));
  /* 24.3: a 403 lease_integrity_failed (18.4.10) is integrity_failed, its error_code the check */
  if (status == 403 && is(e, "lease_integrity_failed")) {
    if (!tf__integrity_code(tf_json_str(tf_json_get(j, "why")), tf_json_str(tf_json_get(j, "check")), code)) snprintf(code, 33, "%s", e);
    tf_json_free(j);
    return "integrity_failed";
  }
  if (why && tf__stop_reason(why) && code_ok(why)) snprintf(code, 33, "%s", why);
  else if (code_ok(e)) snprintf(code, 33, "%s", e);
  tf_json_free(j);
  if (status >= 300 && status < 400) return "redirect";
  if (status == 429) return "rate_limited";
  if ((status == 401 || status == 403) && code[0]) return "refused";
  if (status >= 200 && status < 300) return "invalid_answer";
  return "http";
}
void tf__cause_answer(const char *stage, int status, const char *body, size_t n) {
  char code[33];
  const char *reason = tf__rep_answer_reason(status, body, n, code);
  /* 24.3: the lease route's 503 lease_unavailable, a dependency down with no last good value */
  if (is(stage, "lease") && status == 503 && is(code, "lease_unavailable")) reason = "lease_unavailable";
  tf__cause(stage, reason, status, NULL, code);
}
/* a request that got no answer it could take: no connection, a timeout or stall, a cancel (a download ended by the
 * Client's end), or an answer that was not the one asked for (its status) */
void tf__cause_transport(const char *stage, int status) {
  const char *name = NULL, *code = NULL;
  const int kind = tf__http_failure(&name, &code);
  const int dl = is(stage, "download") || is(stage, "manifest");
  /* 24.3: `timeout` is an answer that began and then stalled; a time limit reached before any answer (no status) is
   * `network`, like every request that got no answer (until 11-18 it read `timeout`, which 24.4 maps to download_failed
   * while the call failed with needs_connection, so the report lost its cause and read download, network: Stage B) */
  const char *reason = kind == TF__FAIL_TIMEOUT ? (status > 0 ? "timeout" : "network")
                       : kind == TF__FAIL_ANSWER ? "invalid_answer" : kind == TF__FAIL_CANCELLED && dl ? "aborted" : "network";
  tf__cause(stage, reason, kind == TF__FAIL_ANSWER ? status : 0, name, code);
}
/* the cause in force, reported as a silent failure when no app call of this thread will report it */
void tf__cause_silent(void) {
  if (t_cause.set && !tf__app_call) tf__rep_silent(t_cause.stage, t_cause.reason, t_cause.status, t_cause.name, t_cause.code);
}

/* the stage and reason a code stands for when no cause was recorded for it */
static void default_cause(const char *name, Cause *k) {
  static const char *const map[][3] = {
      {"needs_connection", "download", "network"}, {"download_failed", "download", "other"}, {"storage_full", "storage", "quota"},
      {"storage_failed", "storage", "write"},      {"model_missing", "verify", "missing"},   {"model_invalid", "verify", "invalid"},
      {"signature_invalid", "verify", "signature"}, {"checksum_mismatch", "verify", "hash"}, {"model_incompatible", "model_open", "incompatible"},
      {"runtime_unsupported", "runtime", "other"}, {"engine_failed", "model_open", "engine"}, {"invalid_key", "config", "refused"},
      {"server_rejected", "config", "refused"},    {"sdk_too_old", "config", "refused"},     {"model_unavailable", "config", "unavailable"},
      {"reference_missing", "verify", "missing"},  {"sharing_unavailable", "sharing", "other"}};
  memset(k, 0, sizeof *k);
  k->set = 1;
  k->retries = -1;
  snprintf(k->stage, sizeof k->stage, "runtime");
  snprintf(k->reason, sizeof k->reason, "other");
  for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i)
    if (is(map[i][0], name)) {
      snprintf(k->stage, sizeof k->stage, "%s", map[i][1]);
      snprintf(k->reason, sizeof k->reason, "%s", map[i][2]);
    }
}
/* ------------------------------------------------------------------ 24.5, 24.6: the queue (tools/errors40.py Reporter) */
static long long dnum(const char *day) { return tf__day_num(day); }
static void ds_add(TfRepState *r, const char *day, const char *sig, int sent) {
  for (int i = 0; i < r->nds; ++i)
    if (is(r->ds[i].day, day) && is(r->ds[i].sig, sig)) {
      r->ds[i].sent = sent;
      return;
    }
  if (r->nds == TF_REP_DAYSIGS) memmove(&r->ds[0], &r->ds[1], sizeof r->ds[0] * --r->nds); /* never with the caps; kept bounded */
  snprintf(r->ds[r->nds].day, sizeof r->ds[0].day, "%s", day);
  snprintf(r->ds[r->nds].sig, sizeof r->ds[0].sig, "%s", sig);
  r->ds[r->nds++].sent = sent;
}
static void ds_remove(TfRepState *r, const char *day, const char *sig) {
  for (int i = 0; i < r->nds; ++i)
    if (is(r->ds[i].day, day) && is(r->ds[i].sig, sig)) {
      memmove(&r->ds[i], &r->ds[i + 1], sizeof r->ds[0] * (size_t)(r->nds - i - 1));
      r->nds--;
      return;
    }
}
/* an occurrence's own fields (each later one of the day replaces them), each a token or one of its closed set */
static void copy_fields(TfRepItem *it, const TfRepOcc *o) {
  static const char *const ram[] = {"lt2", "2_4", "4_8", "8_16", "ge16", NULL};
  static const char *const space[] = {"lt_100m", "lt_500m", "lt_1g", "lt_5g", "ge_5g", NULL};
  it->retries = o->retries >= 0 && o->retries <= 1000 ? o->retries : -1;
  snprintf(it->name, sizeof it->name, "%s", name_ok(o->name) ? o->name : "");
  snprintf(it->ecode, sizeof it->ecode, "%s", code_ok(o->ecode) ? o->ecode : "");
  snprintf(it->ram, sizeof it->ram, "%s", one_of(o->ram, ram) ? o->ram : "");
  snprintf(it->free_, sizeof it->free_, "%s", one_of(o->free_, space) ? o->free_ : "");
  snprintf(it->revision, sizeof it->revision, "%s", o->revision && strlen(o->revision) < sizeof it->revision && tf__is_model_version(o->revision) ? o->revision : "");
  const int hex40 = o->build && strlen(o->build) == 40 && strspn(o->build, "0123456789abcdef") == 40;
  snprintf(it->build, sizeof it->build, "%s", hex40 ? o->build : "");
  snprintf(it->role, sizeof it->role, "%s", is(o->role, "api") || is(o->role, "models") ? o->role : "");
  it->fallback = it->role[0] && o->fallback >= 1 && o->fallback <= 2 ? o->fallback : 0;
}

/* An occurrence on `day`; 1 when it made a new report. */
int tf__rep_occur(TfRepState *r, const char *day, const TfRepOcc *o) {
  if (r->off || !reason_ok(o->stage) || strlen(o->stage) >= 16 || !reason_ok(o->reason) || !reason_ok(o->code) || !tf__rep_reported(o->reason, o->code)) return 0;
  char sig[33];
  tf__rep_signature_hex(o->stage, o->reason, o->code, o->status, sig);
  const long long dn = dnum(day);
  int k = 0;
  for (int i = 0; i < r->nds; ++i)
    if (dn - dnum(r->ds[i].day) <= TF_REP_KEEP) r->ds[k++] = r->ds[i]; /* the days more than 7 back are forgotten */
  r->nds = k;
  int today = 0, at = -1;
  for (int i = 0; i < r->nds; ++i)
    if (is(r->ds[i].day, day)) {
      today++;
      if (is(r->ds[i].sig, sig)) at = i;
    }
  if (at >= 0 && r->ds[at].sent) return 0; /* accepted today already: counted nowhere */
  if (at >= 0) {
    for (int i = 0; i < r->nq; ++i)
      if (is(r->q[i].day, day) && is(r->q[i].sig, sig)) {
        TfRepItem *it = &r->q[i];
        it->count = it->count < TF_REP_COUNT_MAX ? it->count + 1 : TF_REP_COUNT_MAX;
        it->first_launch = it->first_launch || o->first_launch;
        copy_fields(it, o);
        return 0;
      }
    ds_remove(r, day, sig); /* queued without a report (one dropped while reports were off): made again below */
    today--;
  }
  if (today >= TF_REP_DAILY) return 0;
  int first = 1;
  for (int i = 0; i < r->nrep; ++i)
    if (is(r->rep[i].sig, sig)) first = dn - dnum(r->rep[i].day) > TF_REP_FIRST_AGAIN;
  for (int i = 0; i < r->nq; ++i)
    if (is(r->q[i].sig, sig)) first = 0;
  TfRepItem *it = &r->q[r->nq++];
  memset(it, 0, sizeof *it);
  snprintf(it->day, sizeof it->day, "%s", day);
  snprintf(it->stage, sizeof it->stage, "%s", o->stage);
  snprintf(it->reason, sizeof it->reason, "%s", o->reason);
  snprintf(it->code, sizeof it->code, "%s", o->code);
  snprintf(it->sig, sizeof it->sig, "%s", sig);
  it->status = o->status >= 100 && o->status <= 599 ? o->status : 0;
  it->count = 1;
  it->first_report = first;
  it->first_launch = o->first_launch != 0;
  copy_fields(it, o);
  ds_add(r, day, sig, 0);
  while (r->nq > TF_REP_QUEUE) { /* the oldest go first */
    ds_remove(r, r->q[0].day, r->q[0].sig);
    memmove(&r->q[0], &r->q[1], sizeof r->q[0] * (size_t)--r->nq);
  }
  return 1;
}

/* flags.issues false: nothing is recorded and the queue is dropped */
void tf__rep_off(TfRepState *r, int off) {
  if (off && !r->off) {
    for (int i = 0; i < r->nq; ++i) ds_remove(r, r->q[i].day, r->q[i].sig);
    r->nq = 0;
  }
  r->off = off != 0;
}

/* a report answered 202 (sent: its signature goes on the reported list, the oldest of 64 goes) or 400/413 (dropped):
 * out of the queue, and its signature counts as done for its day */
static void settle(TfRepState *r, const TfRepItem *it, int a) {
  for (int i = 0; i < r->nq; ++i)
    if (is(r->q[i].day, it->day) && is(r->q[i].sig, it->sig)) {
      memmove(&r->q[i], &r->q[i + 1], sizeof r->q[0] * (size_t)(r->nq - i - 1));
      r->nq--;
      break;
    }
  ds_add(r, it->day, it->sig, 1);
  if (a != 202) return;
  for (int i = 0; i < r->nrep; ++i)
    if (is(r->rep[i].sig, it->sig)) {
      memmove(&r->rep[i], &r->rep[i + 1], sizeof r->rep[0] * (size_t)(r->nrep - i - 1));
      r->nrep--;
      break;
    }
  if (r->nrep == TF_REP_REPORTED) memmove(&r->rep[0], &r->rep[1], sizeof r->rep[0] * (size_t)--r->nrep);
  snprintf(r->rep[r->nrep].sig, sizeof r->rep[0].sig, "%s", it->sig);
  snprintf(r->rep[r->nrep++].day, sizeof r->rep[0].day, "%s", it->day);
}

/* A send point on `today`: each queued report, oldest first, one request each. 202 sent (its signature goes on the
 * reported list); 400 or 413 dropped; anything else (429, 5xx, no answer: 0) kept, and the send point stops. `post`
 * returning -1 stops without an answer (the tests' answers ran out). Returns the requests made; `made` gets them. */
int tf__rep_send_queue(TfRepState *r, const char *today, TfRepPost post, void *user, TfRepItem *made, int max_made) {
  const long long tn = dnum(today);
  int k = 0;
  for (int i = 0; i < r->nq; ++i)
    if (tn - dnum(r->q[i].day) <= TF_REP_KEEP) r->q[k++] = r->q[i]; /* the server takes 7 days */
  r->nq = k;
  int n = 0;
  while (r->nq > 0) {
    TfRepItem it = r->q[0];
    const int a = post(user, &it);
    if (a < 0) break;
    it.answer = a;
    if (made && n < max_made) made[n] = it;
    n++;
    if (a != 202 && a != 400 && a != 413) break;
    settle(r, &it, a);
  }
  return n;
}
/* ------------------------------------------------------------------ errors.json: the queue and both lists */
static const char *jstr(const TfJson *o, const char *k) {
  const char *s = tf_json_str(tf_json_get(o, k));
  return s ? s : "";
}
static int jint(const TfJson *o, const char *k, int dflt) {
  const TfJson *v = tf_json_get(o, k);
  return v && v->type == TJ_NUM ? (int)v->num : v && v->type == TJ_BOOL ? v->boolean : dflt;
}
static int is_day(const char *s) { return s && dnum(s) > -1000000; }
static int is_sig(const char *s) { return s && strlen(s) == 32 && strspn(s, "0123456789abcdef") == 32; }
/* what a file holds is checked as it is read: a field that is not what the SDK writes is left out (or the entry) */
void tf__rep_from_json(TfRepState *r, const char *text, size_t n) {
  const int off = r->off;
  memset(r, 0, sizeof *r);
  r->off = off;
  TfJson *j = text ? tf_json_parse(text, n, NULL) : NULL;
  const TfJson *q = tf_json_get(j, "queue"), *d = tf_json_get(j, "days"), *p = tf_json_get(j, "reported");
  for (int i = 0; q && q->type == TJ_ARR && i < q->n && r->nq < TF_REP_QUEUE; ++i) {
    const TfJson *x = &q->kids[i];
    const char *day = jstr(x, "day"), *stage = jstr(x, "stage"), *reason = jstr(x, "reason"), *code = jstr(x, "code");
    if (!is_day(day) || !reason_ok(stage) || strlen(stage) >= 16 || !reason_ok(reason) || !reason_ok(code)) continue;
    TfRepOcc o = {stage, reason, code, jint(x, "status", 0), 0, jint(x, "retries", -1), jstr(x, "name"), jstr(x, "ecode"),
                  jstr(x, "ram"), jstr(x, "free"), jstr(x, "revision"), jstr(x, "build"), jstr(x, "host_role"), jint(x, "fallback", 0)};
    TfRepItem *it = &r->q[r->nq++];
    memset(it, 0, sizeof *it);
    snprintf(it->day, sizeof it->day, "%s", day);
    snprintf(it->stage, sizeof it->stage, "%s", stage);
    snprintf(it->reason, sizeof it->reason, "%s", reason);
    snprintf(it->code, sizeof it->code, "%s", code);
    tf__rep_signature_hex(stage, reason, code, o.status, it->sig);
    it->status = o.status >= 100 && o.status <= 599 ? o.status : 0;
    const int count = jint(x, "count", 1);
    it->count = count < 1 ? 1 : count > TF_REP_COUNT_MAX ? TF_REP_COUNT_MAX : count;
    it->first_report = jint(x, "first_report", 0) != 0;
    it->first_launch = jint(x, "first_launch", 0) != 0;
    copy_fields(it, &o);
  }
  for (int i = 0; d && d->type == TJ_ARR && i < d->n; ++i) {
    const TfJson *x = &d->kids[i];
    if (x->type != TJ_ARR || x->n != 3 || !is_day(tf_json_str(&x->kids[0])) || !is_sig(tf_json_str(&x->kids[1]))) continue;
    ds_add(r, x->kids[0].str, x->kids[1].str, x->kids[2].type == TJ_NUM ? x->kids[2].num != 0 : x->kids[2].boolean);
  }
  for (int i = 0; p && p->type == TJ_ARR && i < p->n && r->nrep < TF_REP_REPORTED; ++i) {
    const TfJson *x = &p->kids[i];
    if (x->type != TJ_ARR || x->n != 2 || !is_sig(tf_json_str(&x->kids[0])) || !is_day(tf_json_str(&x->kids[1]))) continue;
    snprintf(r->rep[r->nrep].sig, sizeof r->rep[0].sig, "%s", x->kids[0].str);
    snprintf(r->rep[r->nrep++].day, sizeof r->rep[0].day, "%s", x->kids[1].str);
  }
  tf_json_free(j);
}
char *tf__rep_to_json(const TfRepState *r) {
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"v\":1,\"queue\":[");
  for (int i = 0; i < r->nq; ++i) {
    const TfRepItem *it = &r->q[i];
    tf_buf_printf(&b, "%s{\"day\":\"%s\",\"stage\":\"%s\",\"reason\":\"%s\",\"code\":\"%s\",\"status\":%d,\"count\":%d,\"first_report\":%d,"
                      "\"first_launch\":%d,\"retries\":%d,\"name\":\"%s\",\"ecode\":\"%s\",\"ram\":\"%s\",\"free\":\"%s\",\"revision\":\"%s\",\"build\":\"%s\"",
                  i ? "," : "", it->day, it->stage, it->reason, it->code, it->status, it->count, it->first_report, it->first_launch, it->retries,
                  it->name, it->ecode, it->ram, it->free_, it->revision, it->build);
    if (it->role[0]) tf_buf_printf(&b, ",\"host_role\":\"%s\",\"fallback\":%d", it->role, it->fallback);
    tf_buf_puts(&b, "}");
  }
  tf_buf_puts(&b, "],\"days\":[");
  for (int i = 0; i < r->nds; ++i) tf_buf_printf(&b, "%s[\"%s\",\"%s\",%d]", i ? "," : "", r->ds[i].day, r->ds[i].sig, r->ds[i].sent);
  tf_buf_puts(&b, "],\"reported\":[");
  for (int i = 0; i < r->nrep; ++i) tf_buf_printf(&b, "%s[\"%s\",\"%s\"]", i ? "," : "", r->rep[i].sig, r->rep[i].day);
  tf_buf_puts(&b, "]}");
  return tf_buf_take(&b);
}
/* ------------------------------------------------------------------ the process's reporter */
/* One per process, as the Client: a failure on any thread (an app call, a session, the uploader, the timer) records
 * here, and nothing is recorded while no Client is attached (there is nowhere to keep it and nothing to send it).
 * g_lock guards the state and is never held over a request; g_send makes one send point at a time. The 30 s send
 * time is read by the Client's timer without the lock (the timer holds its own lock when it reads it). */
static volatile char g_spin;
static int g_inited;
static TfMutex g_lock, g_send;
static struct {
  TfClient *c;
  char path[1100], dir[1024];
  int first_process, background;
  char revision[24], build[41];
  TfRepState st;
} g;
static long long g_due; /* atomic: unix seconds of the 30 s send, 0 for none */
static void glock(void) {
  while (__atomic_test_and_set(&g_spin, __ATOMIC_ACQUIRE)) {
  }
  if (!g_inited) {
    tf_mutex_init(&g_lock);
    tf_mutex_init(&g_send);
    g_inited = 1;
  }
  __atomic_clear(&g_spin, __ATOMIC_RELEASE);
  tf_mutex_lock(&g_lock);
}
static void gunlock(void) { tf_mutex_unlock(&g_lock); }
static long long now_s(void) { return tf__now_override ? tf__now_override : (long long)time(NULL); }
static void today(char out[11]) { tf__day_str(now_s() / 86400, out); }
static void save(void) {
  char *t = tf__rep_to_json(&g.st);
  if (t && g.path[0]) tf_write_file(g.path, t, strlen(t)); /* a store that does not write: the state lives in memory */
  free(t);
}

void tf__rep_attach(TfClient *c, int first_process) {
  glock();
  memset(&g, 0, sizeof g);
  g.c = c;
  g.first_process = first_process != 0;
  const char *dir = tf__client_dir(c);
  char *p = dir ? tf_path_join(dir, "errors.json") : NULL, *text = NULL;
  size_t n = 0;
  snprintf(g.dir, sizeof g.dir, "%s", dir ? dir : "");
  snprintf(g.path, sizeof g.path, "%s", p ? p : "");
  if (p && !tf_read_file(p, &text, &n)) tf__rep_from_json(&g.st, text, n);
  free(text);
  free(p);
  __atomic_store_n(&g_due, 0, __ATOMIC_SEQ_CST);
  gunlock();
  tf__rep_config(c);
}
void tf__rep_detach(TfClient *c) {
  glock();
  if (g.c == c) {
    memset(&g, 0, sizeof g);
    __atomic_store_n(&g_due, 0, __ATOMIC_SEQ_CST);
  }
  gunlock();
}
/* the signed config in force: flags.issues false turns reports off (nothing recorded, the queue dropped) */
void tf__rep_config(TfClient *c) {
  const int off = !tf__client_flag(c, "issues", 1);
  glock();
  if (g.c == c && off != g.st.off) {
    tf__rep_off(&g.st, off);
    save();
  }
  gunlock();
}
void tf__rep_model(const char *version, const char *build) {
  glock();
  snprintf(g.revision, sizeof g.revision, "%s", version && strlen(version) < sizeof g.revision ? version : "");
  snprintf(g.build, sizeof g.build, "%s", build && strlen(build) == 40 ? build : "");
  gunlock();
}
void tf__rep_foreground(int fg) {
  glock();
  g.background = !fg;
  gunlock();
}
long long tf__rep_due(void) { return __atomic_load_n(&g_due, __ATOMIC_SEQ_CST); }

static void occur(const Cause *k, const char *code) {
  char rf[8] = "";
  glock();
  if (!g.c) {
    gunlock();
    return;
  }
  const long long fb = g.dir[0] ? tf__free_bytes(g.dir) : -1;
  if (fb >= 0) snprintf(rf, sizeof rf, "%s", tf__rep_storage_free(fb));
  TfRepOcc o = {k->stage, k->reason, code, k->status, g.first_process, k->retries, k->name, k->code, tf__client_ram_tier(), rf, g.revision, g.build,
                k->role[0] ? k->role : NULL, k->fallback};
  char day[11];
  today(day);
  const int made = tf__rep_occur(&g.st, day, &o);
  save();
  /* 24.6: a new report goes 30 s later while the app is in the foreground (one time for the SDK) */
  if (made && !g.background && !tf__rep_due()) {
    __atomic_store_n(&g_due, now_s() + 30, __ATOMIC_SEQ_CST);
    tf__client_rep_wake(g.c);
  }
  gunlock();
}
/* tf__raise: `code` reached the app */
void tf__rep_raised(int code) {
  const char *name = tf_error_name(code);
  if (name && code < 0 && tf__rep_reported(NULL, name)) {
    Cause k;
    if (t_cause.set && t_cause.bound == code) k = t_cause;
    else default_cause(name, &k);
    occur(&k, name);
  }
  tf__cause_clear();
}
/* a failure the app is not told of (24.1 item 2): code none; a plain network failure is never one */
void tf__rep_silent(const char *stage, const char *reason, int status, const char *error_name, const char *error_code) {
  if (!tf__rep_reported(reason, "none")) return;
  Cause k;
  memset(&k, 0, sizeof k);
  if (!stage || strlen(stage) >= sizeof k.stage || !reason_ok(reason)) return;
  k.set = 1;
  k.retries = -1;
  snprintf(k.stage, sizeof k.stage, "%s", stage);
  snprintf(k.reason, sizeof k.reason, "%s", reason);
  k.status = status >= 100 && status <= 599 ? status : 0;
  if (name_ok(error_name)) snprintf(k.name, sizeof k.name, "%s", error_name);
  if (code_ok(error_code)) snprintf(k.code, sizeof k.code, "%s", error_code);
  host_of(&k);
  occur(&k, "none");
}
/* ------------------------------------------------------------------ 24.2: the body, 24.6: sending */
/* The section 7 top level and the closed `error` object; every field the SDK cannot know is left out */
char *tf__rep_body(TfClient *c, const TfRepItem *it) {
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "{\"day\":\"%s\",\"platform\":", it->day);
  tf_buf_json_str(&b, tf__client_platform(c));
  tf_buf_puts(&b, ",\"sdk_version\":\"" TF_SDK_VERSION "\",\"app_version\":");
  tf_buf_json_str(&b, tf__client_options(c)->app_version ? tf__client_options(c)->app_version : "0");
  tf_buf_printf(&b, ",\"os_version\":\"%d\",\"device_class\":\"x86_64\"", tf__client_os_major());
  if (it->revision[0]) tf_buf_printf(&b, ",\"revision\":\"%s\"", it->revision);
  tf_buf_printf(&b, ",\"kind\":\"error\",\"code\":\"%s\",\"error\":{\"stage\":\"%s\",\"reason\":\"%s\"", it->code, it->stage, it->reason);
  if (it->status) tf_buf_printf(&b, ",\"http_status\":%d", it->status);
  if (it->name[0]) tf_buf_printf(&b, ",\"error_name\":\"%s\"", it->name);
  if (it->ecode[0]) tf_buf_printf(&b, ",\"error_code\":\"%s\"", it->ecode);
  tf_buf_printf(&b, ",\"count\":%d", it->count);
  if (it->retries >= 0) tf_buf_printf(&b, ",\"retries\":%d", it->retries);
  tf_buf_printf(&b, ",\"first_launch\":%s,\"first_report\":%s", it->first_launch ? "true" : "false", it->first_report ? "true" : "false");
  if (it->build[0]) tf_buf_printf(&b, ",\"build\":\"%s\"", it->build);
  if (it->ram[0]) tf_buf_printf(&b, ",\"ram_tier\":\"%s\"", it->ram);
  if (it->free_[0]) tf_buf_printf(&b, ",\"storage_free\":\"%s\"", it->free_);
  /* 4.0.1 (ENDPOINTS_40.md 28.7): the role of the request the cause is, and whether more than one host was asked */
  if (it->role[0]) tf_buf_printf(&b, ",\"host_role\":\"%s\"", it->role);
  if (it->role[0] && it->fallback) tf_buf_printf(&b, ",\"fallback\":%s", it->fallback == 2 ? "true" : "false");
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
  tf_buf_puts(&b, ",\"abi\":\"x86_64\"");
#endif
  tf_buf_puts(&b, "}}");
  return tf_buf_take(&b);
}

/* one request; the answer's status, 0 without one (no connection, the launch's calls stopped: kept) */
static int post_http(void *user, const TfRepItem *it) {
  TfClient *c = (TfClient *)user;
  char *body = tf__rep_body(c, it);
  if (!body) return 0;
  TfHttpResponse r;
  memset(&r, 0, sizeof r);
  const int rc = tf__client_call(c, "POST", "/v1/issues", body, 1, NULL, &r);
  free(body);
  const int st = rc ? 0 : r.status;
  tf_http_response_free(&r);
  return st;
}

/* A send point: the queued reports, oldest first, each answered before the next. The requests run on a copy of the
 * queue with the lock let go (an occurrence meanwhile is never held up), then each answer is settled on the queue.
 * A failure to send a report is never reported, and the thread's error and cause are left as they were. Returns the
 * reports accepted. */
int tf__rep_send(TfClient *c) {
  glock(); /* g_send is always taken before g_lock */
  gunlock();
  tf_mutex_lock(&g_send);
  glock();
  if (g.c != c || g.st.off || !g.st.nq) {
    __atomic_store_n(&g_due, 0, __ATOMIC_SEQ_CST);
    gunlock();
    tf_mutex_unlock(&g_send);
    return 0;
  }
  TfRepState *copy = (TfRepState *)malloc(sizeof *copy);
  TfRepItem *made = (TfRepItem *)malloc(sizeof *made * TF_REP_QUEUE);
  if (copy) *copy = g.st;
  __atomic_store_n(&g_due, 0, __ATOMIC_SEQ_CST);
  gunlock();
  char day[11];
  today(day);
  const Cause keep = t_cause;
  const int n = copy && made ? tf__rep_send_queue(copy, day, post_http, c, made, TF_REP_QUEUE) : 0;
  t_cause = keep;
  glock();
  int sent = 0;
  if (g.c == c) {
    const long long tn = dnum(day);
    int k = 0;
    for (int i = 0; i < g.st.nq; ++i)
      if (tn - dnum(g.st.q[i].day) <= TF_REP_KEEP) g.st.q[k++] = g.st.q[i];
    g.st.nq = k;
    for (int i = 0; i < n && i < TF_REP_QUEUE; ++i)
      if (made[i].answer == 202 || made[i].answer == 400 || made[i].answer == 413) {
        settle(&g.st, &made[i], made[i].answer);
        sent += made[i].answer == 202;
      }
    save();
  }
  gunlock();
  tf_mutex_unlock(&g_send);
  free(copy);
  free(made);
  return sent;
}

/* tests: the reporter's state as errors.json holds it (caller frees) */
char *tf__rep_state_json(void) {
  glock();
  char *t = tf__rep_to_json(&g.st);
  gunlock();
  return t;
}
