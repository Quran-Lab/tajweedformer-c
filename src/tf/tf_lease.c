/* The protected model on the Client's side (PROTECTED_MODEL.md 18.4): one closed Core per Client (qlc_core), the
 * install's keys (tf_ikey.c), the platform table (tf_platform.c), and the lease's network half.
 *
 *   attest   POST /v1/attest with the developer key: {platform, bundle_id, install_key, install_sign_key,
 *            install_sig, install_hw} (18.4.3). Desktop sends no challenge (the server refuses one without an
 *            attestation), so install_sig is the Core's qlc_attest_sig over "quranlab-attest 1\n-\n<install hash>\n".
 *            A NEW agreement key at every attest: the Core is made again for the pending key, signs, and on a 200
 *            the key is committed with the certificate; on any failure the new key is deleted and the old kept.
 *            The answer's token becomes the Client's app token.
 *   lease    GET /v1/attest/challenge with the app token, qlc_lease_ask, POST /v1/model-key (18.4.4), and the
 *            answer's binary lease into qlc_lease_accept with the signed manifest and the build (18.4.5). The answer's
 *            certificate is kept; its `base` is where the build's containers are (the 4.0 config names none), kept
 *            with the build in <storage>/install/lease.json so a later launch reads the files from there too.
 *   piece    The answer's data piece (18.6.7, R9) goes to qlc_lease_piece AFTER qlc_lease_accept, as the Core's
 *            interface says (quranlab-core cd24aa3: "Hand it over after qlc_lease_accept: it is in force from the
 *            next qlc_lease_ask"). The Core marks the piece in force at an accept as the last good one; a piece
 *            handed before the accept would be marked good before any proof under it was checked, and a refused
 *            proof under it could then never go back (the contract's 18.4.5 says "before": reported).
 *   refusals 18.4.10: every refusal body of this route goes to the Core (qlc_refusal_accept). A stop counts only when
 *            the Core verifies its signature (QLC_E_STOPPED); then the Client's refusal is remembered and the models
 *            removed, as 15.3. An unsigned stop here is a failed request, nothing more. Any other refusal while a new
 *            data piece is in force sends the Core back to its last good piece (18.6.7); a proof refused under a
 *            piece (403 lease_integrity_failed, why core) is asked once more, under that last good piece.
 *            install_cert_invalid: attest again, repeat once. challenge_invalid: a new challenge, repeat once.
 *            invalid_token without a reason: the Client renews the token and repeats once (tf_client.c). 429, 5xx
 *            and a redirect are never refusals: try later. The rest as the table says.
 *   refresh  qlc_tick on the Client's timer; when the Core says the refresh is due, a new lease in the background,
 *            retried after 1, 5 and 30 minutes, then 2 and 4 hours, then every 6 hours (18.4.7). A valid lease never
 *            waits for a refresh; a model already open keeps running when its lease ends (15.3).
 *   grace    18.4.7.1: every attempt that brings no lease is told to the Core (qlc_lease_attempt) by how it ended, read
 *            from the answer the attempt's last request got (tf__http_answer): refused for a 4xx other than 429 whose
 *            body is JSON with an `error` string, after 18.4.10's retries inside the attempt; unreachable for anything
 *            else (no answer, a stall, a 5xx, a 429, a redirect, a 4xx that is not the API's, a 2xx that is not a
 *            lease the Core accepts). An attempt that asked nothing of the server is not told, nor a verified stop (the
 *            Core cleared the lease). Past exp the Core opens the lease it holds for 24 hours more while an attempt
 *            ended unreachable and none refused; a model open past exp makes an attempt first and waits at most 10 s
 *            for it (a request still out then is cut: unreachable, and the Client's timer asks again at its next
 *            tick), and an unreachable outcome of this process less than a minute old serves it without a new one.
 *            Each open in grace is a silent report (lease, grace) with the status and reason that ended the attempt.
 *
 * Desktop installs are the unattested class: the server gives them `att none` certificates and 24-hour leases, and
 * never accelerator programs (18.4.7, 18.10 question 13). A TPM key changes only `hw` in the certificate. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tf_internal.h"
#include "tf_thread.h"

void tf_b64url(const uint8_t *in, size_t n, char *out);

struct TfPm {
  TfMutex lock;
  TfIkey *ik;
  TfPlat *plat;
  qlc_core *core;
  int refs;           /* the Client, and each open Core model */
  int open_models;
  int client_gone;
  char build[41];     /* the build of the lease this install last had accepted (lease.json) */
  char base[512];     /* where that build's containers are (the lease answer's base; lease.json) */
  char *rec;          /* <storage>/install/lease.json */
  long long next_try; /* the background refresh's next attempt (unix s) */
  int fails;
  int route_off;      /* the lease route answered 404 not_found in this launch */
  int open_err;       /* why the key store or the Core could not be made (0 when they were) */
  /* 18.4.7.1, this process's lease attempts */
  TfCond cv;           /* an attempt ended */
  int attempting;      /* attempts running now */
  double att_end;      /* when the last attempt that brought no lease ended (tf__now_s), 0 for none */
  int att_unreachable; /* it ended unreachable */
  int att_code;        /* the code a call failed with for it */
  int att_status;      /* the status of the answer that ended it, 0 for none */
  char att_reason[33]; /* its 24.3 reason (a token of up to 32: lease_unavailable is 17) */
};
double tf__lease_wait_s = 10, tf__lease_reuse_s = 60;

static long long now_s(void) { return tf__now_override ? tf__now_override : (long long)time(NULL); }

/* A model folder named by a lease answer is used only on Quran Lab's own hosts, over https (as the Apple SDK): from
 * 4.0.1 a host of the allow-list of ENDPOINTS_40.md 28.1 (quranlab.ai, tajweedformer.com, quran-lab.com; 4.0.0 took
 * *.quranlab.ai only), a path that starts and ends with '/', printable ASCII, at most 511 characters, no port, user,
 * query, fragment, backslash, space or ".." (28.4). Its path is the folder every model request of the build asks the
 * models hosts for. */
static int base_ok(const char *b) { return tf__ep_base_parts(b, NULL, NULL, 0) || (b && strlen(b) < 512 && tf__lease_test_base && !strncmp(b, tf__lease_test_base, strlen(tf__lease_test_base))); }

/* lease.json: {"build":"<40 hex>","base":"<the answer's base>"}, the lease the Core holds. Data only: the Core checks
 * its own lease's build at install and open, so a changed record can only cost a refused install or a new lease. */
static void rec_load(TfPm *p) {
  char *text = NULL;
  size_t n = 0;
  TfJson *j = p->rec && !tf_read_file(p->rec, &text, &n) ? tf_json_parse(text, n, NULL) : NULL;
  const char *b = tf_json_str(tf_json_get(j, "build")), *u = tf_json_str(tf_json_get(j, "base"));
  if (b && strlen(b) == 40 && strspn(b, "0123456789abcdef") == 40) {
    snprintf(p->build, sizeof p->build, "%s", b);
    if (u && (base_ok(u) || tf__lease_test_base) && strlen(u) < sizeof p->base) snprintf(p->base, sizeof p->base, "%s", u);
  }
  tf_json_free(j);
  free(text);
}
static void rec_save(TfPm *p) {
  if (!p->rec) return;
  if (!p->build[0]) {
    remove(p->rec);
    return;
  }
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "{\"build\":\"%s\",\"base\":", p->build);
  tf_buf_json_str(&b, p->base);
  tf_buf_puts(&b, "}\n");
  if (b.p) tf_write_file(p->rec, b.p, b.n);
  tf_buf_free(&b);
}

/* qlc_detail's codes by name (qlcore.h QLC_D_*), for the messages */
static const char *lease_detail(int d) {
  switch (d) {
  case QLC_D_LEASE_FORMAT: return "format";
  case QLC_D_LEASE_SIGNATURE: return "signature";
  case QLC_D_LEASE_INSTALL: return "another install";
  case QLC_D_LEASE_RELEASE: return "another Core release";
  case QLC_D_LEASE_BUILD: return "another build";
  case QLC_D_LEASE_KEY_ID: return "another key id";
  case QLC_D_LEASE_MODEL: return "another model";
  case QLC_D_LEASE_EXPIRED: return "expired";
  case QLC_D_LEASE_OUTER_TAG: return "outer wrap";
  case QLC_D_LEASE_INNER_TAG: return "inner wrap";
  case QLC_D_LEASE_SITE: return "site";
  case QLC_D_LEASE_UNWRAP: return "unwrap";
  case QLC_D_LEASE_NO_ACCEL: return "no accelerator";
  default: return "other";
  }
}
static const char *file_detail(int d) {
  static const char *const n[] = {"other", "magic", "version", "header length", "form", "suite", "payload", "record shift", "reserved",
                                  "variant", "size", "path hash", "key id", "index", "tag", "plaintext SHA-256", "notice"};
  return d >= QLC_D_FILE_MAGIC && d <= QLC_D_FILE_NOTICE ? n[d] : n[0];
}
int tf__pm_lease_expired(int status, int detail) { return status == QLC_E_NEEDS_CONNECTION || (status == QLC_E_LEASE && detail == QLC_D_LEASE_EXPIRED); }

/* 24.3: a Core status as the cause of the error it maps to (error_name the status's name, error_code its detail, else
 * its number). The stage is where the Core was asked: installing a container (download), else what the status says. */
static _Thread_local const char *t_core_stage;
/* ERRORS_40.md 24.3 (Gate C, contract 7f097b9): set while a failed qlc_model_open is mapped (tf__pm_open) */
static _Thread_local int t_core_open;
static int pm_error_code(int status, int detail);
static const char *core_name(int status) {
  static const char *const n[] = {"QLC_OK", "QLC_E_ARG", "QLC_E_NOMEM", "QLC_E_STORAGE", "QLC_E_MANIFEST", "QLC_E_NEEDS_CONNECTION",
                                  "QLC_E_LEASE", "QLC_E_FILE", "QLC_E_ENGINE", "QLC_E_STOPPED", "QLC_E_KEYSTORE", "QLC_E_BUSY"};
  return status <= 0 && status >= QLC_E_BUSY ? n[-status] : "QLC_E";
}
static int core_cause(int rc, int status, int detail) {
  const char *stage = "verify", *reason = "invalid";
  switch (status) {
  case QLC_E_NOMEM: stage = "model_open", reason = "memory"; break;
  case QLC_E_STORAGE: reason = "missing"; break;
  case QLC_E_NEEDS_CONNECTION: stage = "lease", reason = "network"; break;
  case QLC_E_LEASE: stage = "lease", reason = detail == QLC_D_LEASE_EXPIRED ? "expired" : "invalid"; break;
  case QLC_E_FILE: reason = "hash"; break;
  case QLC_E_ENGINE: stage = "model_open", reason = "engine"; break;
  case QLC_E_KEYSTORE: stage = "storage", reason = "read"; break;
  case QLC_E_STOPPED: stage = "lease", reason = "refused"; break;
  default: break;
  }
  if (t_core_stage && status == QLC_E_FILE) stage = t_core_stage;
  char code[16];
  snprintf(code, sizeof code, "%d", detail ? detail : status);
  /* 24.3 (4.0.1, Gate C): when qlc_model_open fails with a status this SDK answers engine_failed for, error_code is the
   * status's name without QLC_E_, in lower case: QLC_E_NOMEM is reason memory, nomem; QLC_E_ENGINE is reason engine,
   * engine (errors_40.json core_open). C answers QLC_E_BUSY with session_open, never reported, and QLC_E_ARG with
   * model_invalid, so only these two carry it. Elsewhere (an install, a lease) error_code stays the detail or number. */
  if (t_core_open && (status == QLC_E_NOMEM || status == QLC_E_ENGINE)) {
    const char *n = core_name(status) + 6; /* past "QLC_E_" */
    size_t i = 0;
    for (; n[i] && i + 1 < sizeof code; ++i) code[i] = (char)(n[i] >= 'A' && n[i] <= 'Z' ? n[i] + 32 : n[i]);
    code[i] = 0;
  }
  tf__cause_now(rc, stage, reason, 0, core_name(status), code);
  return rc;
}
int tf__pm_error(int status, int detail) {
  const int rc = status == QLC_OK ? 0 : status == QLC_E_BUSY ? tf__fail(TF_ERR_SESSION_OPEN, NULL) : pm_error_code(status, detail);
  return rc && status != QLC_E_BUSY ? core_cause(rc, status, detail) : rc;
}
static int pm_error_code(int status, int detail) {
  switch (status) {
  case QLC_OK: return 0;
  case QLC_E_NEEDS_CONNECTION: return tf__fail(TF_ERR_NEEDS_CONNECTION, NULL);
  case QLC_E_MANIFEST: return tf__fail(TF_ERR_MODEL_INVALID, "the build's manifest is not one the Core takes (%d)", detail);
  case QLC_E_LEASE:
    /* a lease past its time needs a new one: a connection, as with none (15.3) */
    if (detail == QLC_D_LEASE_EXPIRED) return tf__fail(TF_ERR_NEEDS_CONNECTION, NULL);
    return tf__fail(TF_ERR_MODEL_INVALID, "the lease was refused (%s, %d)", lease_detail(detail), detail);
  case QLC_E_FILE: return tf__fail(TF_ERR_CHECKSUM_MISMATCH, "an encrypted model file was refused (%s, %d)", file_detail(detail), detail);
  case QLC_E_STORAGE: return tf__fail(TF_ERR_MODEL_MISSING, "a model file is not installed on this device");
  case QLC_E_ENGINE: return tf__fail(TF_ERR_ENGINE_FAILED, "the engine could not open the model");
  case QLC_E_KEYSTORE: return tf__fail(TF_ERR_STORAGE_FAILED, "the install key store failed");
  case QLC_E_STOPPED: return tf__fail(TF_ERR_INVALID_KEY, NULL);
  case QLC_E_BUSY: return tf__fail(TF_ERR_SESSION_OPEN, NULL);
  case QLC_E_NOMEM: return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  default: return tf__fail(TF_ERR_MODEL_INVALID, "the Core refused the call (%d)", status);
  }
}

/* ------------------------------------------------------------------ the host */
static TfPm **slot(TfClient *c) { return (TfPm **)tf__client_pm_slot(c); }

static int core_make(TfPm *p, int pending) {
  if (p->core) {
    if (p->open_models) return QLC_E_BUSY;
    qlc_destroy(p->core);
    p->core = NULL;
  }
  size_t n = 0;
  uint8_t *st = tf__plat_state(p->plat, &n);
  int status = 0;
  p->core = qlc_create(tf__plat_table(p->plat, pending), st, n, &status);
  if (st) memset(st, 0, n);
  free(st);
  return p->core ? 0 : status ? status : QLC_E_NOMEM;
}

/* the Client's Core host, made at first use: the install keys (made when there are none), the platform table, the
 * Core with the state it saved last. NULL and an error when the key store cannot be used. */
TfPm *tf__pm(TfClient *c) {
  if (!c) return NULL;
  TfPm **s = slot(c);
  if (*s) return *s;
  TfPm *p = (TfPm *)calloc(1, sizeof *p);
  if (!p) {
    tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
    return NULL;
  }
  tf_mutex_init(&p->lock);
  tf_cond_init(&p->cv);
  p->refs = 1;
  p->ik = tf__ikey_open(tf__client_dir(c), 1);
  p->plat = p->ik ? tf__plat_new(tf__client_dir(c), p->ik) : NULL;
  const int st = p->ik && p->plat ? core_make(p, 0) : 0;
  if (!p->ik || !p->plat || st) {
    tf__plat_free(p->plat);
    tf__ikey_close(p->ik);
    tf_cond_destroy(&p->cv);
    tf_mutex_destroy(&p->lock);
    free(p);
    const int rc = tf__fail(TF_ERR_STORAGE_FAILED, "the install key store cannot be used (%s)", tf__client_dir(c));
    /* 24.3: the key store that cannot be read, or the Core that refused to start (runtime core, its status) */
    char code[16];
    snprintf(code, sizeof code, "%d", st);
    if (st) tf__cause_now(rc, "runtime", "core", 0, core_name(st), code);
    else tf__cause_now(rc, "storage", "read", 0, NULL, NULL);
    return NULL;
  }
  char *inst = tf_path_join(tf__client_dir(c), "install");
  p->rec = inst ? tf_path_join(inst, "lease.json") : NULL;
  free(inst);
  rec_load(p);
  *s = p;
  return p;
}
/* The Client's Core host when this install already has its keys (a model was installed in some launch), made now if
 * needed; NULL when it has none. For the learner's days, which live in the Core's journal (Core request R10): an
 * install that never had a model has no days, and no keys are made for them. */
TfPm *tf__pm_existing(TfClient *c) {
  if (!c) return NULL;
  if (*slot(c)) return *slot(c);
  char *inst = tf_path_join(tf__client_dir(c), "install"), *id = inst ? tf_path_join(inst, "identity.json") : NULL;
  const int have = tf__ikey_test_pkcs8 || (id && tf_file_exists(id));
  free(id);
  free(inst);
  if (!have) return NULL;
  return tf__pm(c);
}
qlc_core *tf__pm_core(TfPm *p) { return p ? p->core : NULL; }
TfIkey *tf__pm_ikey(TfPm *p) { return p ? p->ik : NULL; }
TfPlat *tf__pm_plat(TfPm *p) { return p ? p->plat : NULL; }
/* where the files of `build` are: the base of the lease answer for that build, or NULL (the default host's URLs) */
int tf__pm_base(TfPm *p, const char *build, char *out, size_t cap) {
  if (!p || !build || !out || !cap) return 0;
  tf_mutex_lock(&p->lock);
  const int ok = p->base[0] && !strcmp(p->build, build) && strlen(p->base) < cap;
  if (ok) snprintf(out, cap, "%s", p->base);
  tf_mutex_unlock(&p->lock);
  return ok;
}

static void pm_free(TfPm *p) {
  qlc_destroy(p->core);
  tf__plat_free(p->plat);
  tf__ikey_close(p->ik);
  tf_cond_destroy(&p->cv);
  tf_mutex_destroy(&p->lock);
  free(p->rec);
  free(p);
}
void tf__pm_retain(TfPm *p) {
  tf_mutex_lock(&p->lock);
  p->refs++;
  tf_mutex_unlock(&p->lock);
}
void tf__pm_release(TfPm *p) {
  tf_mutex_lock(&p->lock);
  const int last = --p->refs == 0;
  tf_mutex_unlock(&p->lock);
  if (last) pm_free(p);
}
/* the Client goes: the host stays while a model of its Core is open */
void tf__pm_client_gone(TfClient *c) {
  TfPm **s = slot(c), *p = *s;
  *s = NULL;
  if (!p) return;
  tf_mutex_lock(&p->lock);
  p->client_gone = 1;
  const int last = --p->refs == 0;
  tf_mutex_unlock(&p->lock);
  if (last) pm_free(p);
}

/* ------------------------------------------------------------------ attest */
static void b64u(const uint8_t *in, size_t n, char *out) { tf_b64url(in, n, out); }

/* 24.3: model_unavailable from an answer of the attest or lease routes (the route off, a malformed request), whose
 * pair 24.4 would read as a failed download: the code is the route's own rule (18.4), the answer its cause */
static int off_cause(int rc, const char *stage, int status, const char *code) {
  tf__cause_now(rc, stage, "http", status, NULL, code);
  return rc;
}

static int attest_taken(void *user, const TfHttpResponse *r);
/* An attest with the install's keys: a new agreement key, its certificate, and the Client's token. Needs no Core
 * model open (the Core is made again for the new key). 0, or an error (the old key and certificate stay). */
int tf__pm_attest(TfClient *c, TfPm *p) {
  tf_mutex_lock(&p->lock);
  if (p->open_models) {
    tf_mutex_unlock(&p->lock);
    return tf__fail(TF__ERR_NOT_AVAILABLE, "a new install certificate waits until the model is closed");
  }
  int rc = tf__ikey_rotate(p->ik) ? tf__fail(TF_ERR_STORAGE_FAILED, "the install key store could not make a key") : 0;
  int st = rc ? 0 : core_make(p, 1);
  if (!rc && st) rc = tf__pm_error(st, 0);
  char sig[128] = "";
  if (!rc && (st = qlc_attest_sig(p->core, NULL, sig, sizeof sig)) != QLC_OK) rc = tf__pm_error(st, qlc_detail(p->core));
  TfBuf b;
  tf_buf_init(&b);
  if (!rc) {
    char ka[130], ks[130];
    size_t n = 0;
    b64u(tf__ikey_spki(p->ik, 2, &n), 91, ka);
    b64u(tf__ikey_spki(p->ik, 1, &n), 91, ks);
    tf_buf_puts(&b, "{\"platform\":");
    tf_buf_json_str(&b, tf__client_platform(c));
    tf_buf_puts(&b, ",\"bundle_id\":");
    tf_buf_json_str(&b, tf__client_app(c));
    tf_buf_printf(&b, ",\"install_key\":\"%s\",\"install_sign_key\":\"%s\",\"install_sig\":\"%s\",\"install_hw\":%d}", ka, ks, sig,
                  tf__ikey_hw(p->ik));
  }
  tf_mutex_unlock(&p->lock);
  TfHttpResponse r;
  memset(&r, 0, sizeof r);
  if (!rc) rc = tf__client_call_taken(c, "POST", "/v1/attest", b.p, 0, attest_taken, NULL, &r);
  tf_buf_free(&b);
  TfJson *j = !rc && r.status == 200 ? tf_json_parse(r.body, r.body_n, NULL) : NULL;
  const char *cert = tf_json_str(tf_json_get(j, "install_cert")), *token = tf_json_str(tf_json_get(j, "token"));
  const TfJson *ex = tf_json_get(j, "expires_in");
  if (!rc && r.status != 200) {
    const TfJson *e = r.body ? tf_json_parse(r.body, r.body_n, NULL) : NULL;
    const char *code = tf_json_str(tf_json_get(e, "error"));
    if (r.status == 400 && code && !strcmp(code, "invalid_body")) {
      /* the server takes no install keys (the protected model is off there): no lease can come */
      p->route_off = 1;
      rc = off_cause(tf__fail(TF_ERR_MODEL_UNAVAILABLE, NULL), "attest", 400, "invalid_body");
    } else if (r.status == 403 && code && !strcmp(code, "lease_integrity_failed")) {
      /* the token request refused for an integrity check: server_rejected naming it, as at the lease (24.3, 69326fb);
       * the cause (attest, integrity_failed, why:check) was recorded with the answer (tf__cause_answer) */
      char detail[400];
      tf__integrity_detail(tf_json_str(tf_json_get(e, "why")), tf_json_str(tf_json_get(e, "check")), detail, sizeof detail);
      rc = tf__fail(TF_ERR_SERVER_REJECTED, "%s", detail);
    } else {
      rc = tf__fail(TF__ERR_HTTP, "attest: HTTP %d %s", r.status, code ? code : "");
    }
    tf_json_free((TfJson *)e);
  } else if (!rc && (!cert || !token)) {
    tf__cause("attest", "invalid_answer", 200, NULL, NULL);
    rc = tf__fail(TF__ERR_HTTP, "attest: no install certificate in the answer");
  }
  tf_mutex_lock(&p->lock);
  if (!rc && tf__ikey_commit(p->ik, cert)) rc = tf__fail(TF_ERR_STORAGE_FAILED, "the install certificate cannot be kept");
  if (rc) tf__ikey_discard(p->ik);
  st = core_make(p, 0);
  tf_mutex_unlock(&p->lock);
  if (!rc && st) rc = tf__pm_error(st, 0);
  if (!rc) tf__client_set_token(c, token, ex && ex->type == TJ_NUM ? (long long)ex->num : 3600);
  tf_json_free(j);
  tf_http_response_free(&r);
  return rc;
}

/* ------------------------------------------------------------------ the lease */
/* A refusal of the lease route (18.4.10: a 4xx answer with a body, 429 aside) goes to the Core whatever it says. The
 * Core acts on a stop only when it verifies its signature (a pinned key; this install, key, reason and challenge;
 * within 10 minutes): then it is the Client's refusal and the models go, as 15.3. Any other refusal while a new data
 * piece is in force sends the Core back to its last good piece for the next ask (18.6.7). 1 when it was a stop. */
static int refusal(TfClient *c, TfPm *p, const TfHttpResponse *r) {
  if (r->status < 400 || r->status > 499 || r->status == 429 || !r->body || !r->body_n) return 0;
  const char *kid = tf__ikey_key_id(p->ik);
  tf_mutex_lock(&p->lock);
  const int st = qlc_refusal_accept(p->core, r->body, r->body_n, kid ? kid : "");
  if (st == QLC_E_STOPPED) {
    p->build[0] = p->base[0] = 0; /* the Core cleared its lease */
    rec_save(p);
  }
  tf_mutex_unlock(&p->lock);
  if (st != QLC_E_STOPPED) return 0;
  char why[48];
  if (!tf__body_stop_reason(r->body, r->body_n, why)) snprintf(why, sizeof why, "%s", "invalid_key");
  tf__client_refuse_api(c, why);
  return 1;
}

static int http_code(const TfHttpResponse *r, char out[48], char why[48], char check[48]) {
  out[0] = why[0] = check[0] = 0;
  TfJson *j = r->body ? tf_json_parse(r->body, r->body_n, NULL) : NULL;
  const char *e = tf_json_str(tf_json_get(j, "error")), *w = tf_json_str(tf_json_get(j, "why")), *k = tf_json_str(tf_json_get(j, "check"));
  if (e) snprintf(out, 48, "%s", e);
  if (w) snprintf(why, 48, "%s", w);
  if (k) snprintf(check, 48, "%s", k);
  tf_json_free(j);
  return out[0] != 0;
}

/* 17.8 `lease`, the cause of a failed request: 1 offline, 2 refused, 3 expired, 4 clock, 5 attestation, 6 other */
static _Thread_local int t_cause;
/* the attempt ended with a stop the Core verified (18.4.10) */
static _Thread_local int t_stopped;
/* the Retry-After of the answer that ended the attempt (the lease route's 503 lease_unavailable: 30), 0 for none */
static _Thread_local int t_retry_after;
/* 28.3, 28.5: the answers of the lease's routes that are taken on a host; any other 2xx is that host unreachable and
 * the next api host is asked (a challenge from one host is good with a lease request that another host answers) */
static int challenge_taken(void *user, const TfHttpResponse *r) {
  (void)user;
  TfJson *j = r->status == 200 ? tf_json_parse(r->body, r->body_n, NULL) : NULL;
  const char *ch = tf_json_str(tf_json_get(j, "challenge"));
  const int ok = ch && *ch && strlen(ch) < 80;
  tf_json_free(j);
  return ok;
}
static int attest_taken(void *user, const TfHttpResponse *r) {
  (void)user;
  TfJson *j = r->status == 200 ? tf_json_parse(r->body, r->body_n, NULL) : NULL;
  const int ok = tf_json_str(tf_json_get(j, "install_cert")) && tf_json_str(tf_json_get(j, "token"));
  tf_json_free(j);
  return ok;
}
/* A lease answer (18.4.5) on one host: taken only when its base is a folder of 28.4 (or absent) and the Core accepts
 * its lease, which it does here, at once, with the piece after it (the file's comment). A 200 that is not that is the
 * host unreachable (28.3, 18.4.7.1's table); the first such answer is remembered, so the request that every host left
 * unreachable fails as its first host's answer did. */
typedef struct {
  TfClient *c;
  TfPm *p;
  const char *manifest, *build;
  size_t mn;
  int accepted; /* the Core accepted a lease */
  int bad;      /* the first 200 that was not taken: 1 no lease, 2 a base off the allow-list, 3 a lease the Core refused */
  int a, d;     /* the Core's status and detail for 3 */
} LeaseTake;
static int lease_taken(void *user, const TfHttpResponse *r) {
  LeaseTake *t = (LeaseTake *)user;
  if (r->status != 200) return 0;
  TfJson *j = tf_json_parse(r->body, r->body_n, NULL);
  const char *lease64 = tf_json_str(tf_json_get(j, "lease")), *base = tf_json_str(tf_json_get(j, "base"));
  const char *cert = tf_json_str(tf_json_get(j, "install_cert")), *piece64 = tf_json_str(tf_json_get(j, "piece"));
  uint8_t lease[4096], piece[4096];
  const int ln = lease64 && strlen(lease64) < 5400 ? tf_base64_decode(lease64, lease, sizeof lease) : -1;
  int ok = 0, why = 0, a = 0, d = 0;
  if (ln <= 0) {
    why = 1;
  } else if (base && !base_ok(base) && !tf__lease_test_base) {
    why = 2; /* 28.4: a base off the allow-list is not a lease answer (4.0.0 took the lease and dropped the base) */
  } else {
    TfPm *p = t->p;
    tf_mutex_lock(&p->lock);
    int64_t exp = 0, ra = 0;
    a = qlc_lease_accept(p->core, lease, (size_t)ln, t->manifest, t->mn, t->build, &exp, &ra);
    d = qlc_detail(p->core);
    if (a == QLC_OK) {
      /* the data piece after the accept (the file's comment): the lease was granted under the piece in force, which
       * the accept made the last good one; this one is in force from the next ask. A piece that does not verify
       * changes nothing. */
      if (piece64 && strlen(piece64) < 5400) {
        const int pn = tf_base64_decode(piece64, piece, sizeof piece);
        if (pn > 0) qlc_lease_piece(p->core, piece, (size_t)pn);
      }
      snprintf(p->build, sizeof p->build, "%s", t->build);
      p->base[0] = 0;
      if (base && strlen(base) < sizeof p->base) snprintf(p->base, sizeof p->base, "%s", base);
      rec_save(p);
      if (cert) tf__ikey_set_cert(p->ik, cert);
      p->fails = 0;
      p->next_try = 0;
      ok = t->accepted = 1;
    } else {
      why = 3;
    }
    tf_mutex_unlock(&p->lock);
  }
  memset(lease, 0, sizeof lease);
  tf_json_free(j);
  if (!ok && !t->bad) {
    t->bad = why;
    t->a = a;
    t->d = d;
  }
  return ok;
}
/* one lease request; 0 with the lease accepted, 1 to repeat once (a new challenge or certificate), or an error */
static int lease_once(TfClient *c, TfPm *p, const char *manifest, size_t mn, const TfJson *m, const char *build, int *renewed) {
  t_cause = 1;
  const TfJson *enc = tf_json_get(m, "encryption");
  const char *key_id = tf_json_str(tf_json_get(enc, "key_id"));
  const char *model = tf_json_str(tf_json_get(m, "model_id")), *version = tf_json_str(tf_json_get(m, "version"));
  if (!key_id || !model || !version) return tf__fail(TF_ERR_MODEL_INCOMPATIBLE, "the build is not encrypted: SDK 4 runs encrypted builds only");
  /* the challenge, with the app token (18.4.1 step 3) */
  TfHttpResponse r;
  memset(&r, 0, sizeof r);
  int rc = tf__client_call_taken(c, "GET", "/v1/attest/challenge", NULL, 1, challenge_taken, NULL, &r);
  if (rc) {
    t_retry_after = r.retry_after; /* 28.5: every host unreachable: the largest Retry-After any host gave */
    tf_http_response_free(&r);
    return rc;
  }
  TfJson *j = r.status == 200 ? tf_json_parse(r.body, r.body_n, NULL) : NULL;
  char challenge[80] = "";
  const char *ch = tf_json_str(tf_json_get(j, "challenge"));
  if (ch && strlen(ch) < sizeof challenge) snprintf(challenge, sizeof challenge, "%s", ch);
  tf_json_free(j);
  if (!challenge[0]) {
    t_retry_after = r.retry_after;
    t_cause = r.status == 404 || r.status == 401 || r.status == 403 ? 2 : 1;
    if (r.status == 200) tf__cause("lease", "invalid_answer", 200, NULL, NULL); /* a 200 without a challenge */
    rc = r.status == 404 ? off_cause(tf__fail(TF_ERR_MODEL_UNAVAILABLE, NULL), "lease", 404, NULL) : tf__fail(TF__ERR_HTTP, "challenge: HTTP %d", r.status);
    tf_http_response_free(&r);
    return rc;
  }
  tf_http_response_free(&r);
  /* the Core's part of the request */
  char core_json[512], sig[128];
  tf_mutex_lock(&p->lock);
  const int st = qlc_lease_ask(p->core, challenge, build, core_json, sizeof core_json, sig, sizeof sig);
  const int detail = qlc_detail(p->core);
  tf_mutex_unlock(&p->lock);
  if (st) {
    t_cause = 6;
    return tf__pm_error(st, detail);
  }
  /* the data piece the proof was made under (core.piece, 0 without one) */
  TfJson *cj = tf_json_parse(core_json, strlen(core_json), NULL);
  const TfJson *pc = tf_json_get(cj, "piece");
  const int under_piece = pc && pc->type == TJ_NUM && pc->num > 0;
  tf_json_free(cj);
  char ka[130], ks[130];
  size_t n = 0;
  b64u(tf__ikey_spki(p->ik, 0, &n), 91, ka);
  b64u(tf__ikey_spki(p->ik, 1, &n), 91, ks);
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"v\":1,\"platform\":");
  tf_buf_json_str(&b, tf__client_platform(c));
  tf_buf_puts(&b, ",\"install_cert\":");
  tf_buf_json_str(&b, tf__ikey_cert(p->ik));
  tf_buf_printf(&b, ",\"install_key\":\"%s\",\"install_sign_key\":\"%s\",\"challenge\":", ka, ks);
  tf_buf_json_str(&b, challenge);
  tf_buf_printf(&b, ",\"install_sig\":\"%s\",\"model\":", sig);
  tf_buf_json_str(&b, model);
  tf_buf_puts(&b, ",\"version\":");
  tf_buf_json_str(&b, version);
  tf_buf_printf(&b, ",\"build\":\"%s\",\"key_id\":", build);
  tf_buf_json_str(&b, key_id);
  tf_buf_printf(&b, ",\"core\":%s}", core_json);
  memset(&r, 0, sizeof r);
  LeaseTake lt;
  memset(&lt, 0, sizeof lt);
  lt.c = c;
  lt.p = p;
  lt.manifest = manifest;
  lt.mn = mn;
  lt.build = build;
  rc = b.p ? tf__client_call_lease(c, b.p, lease_taken, &lt, &r) : tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  tf_buf_free(&b);
  if (rc) {
    t_retry_after = r.retry_after;
    tf_http_response_free(&r);
    return rc;
  }
  if (r.status != 200) {
    char code[48], why[48], check[48];
    http_code(&r, code, why, check);
    t_retry_after = r.retry_after; /* a 503 lease_unavailable is unreachable: asked again after it, never a refusal */
    t_cause = r.status == 429 || r.status >= 500 || r.status < 400 ? 1 : !strcmp(code, "install_cert_invalid") || !strcmp(code, "lease_attestation_required") ? 5
              : r.status == 400 || !strcmp(code, "challenge_invalid") ? 6 : 2;
    if (refusal(c, p, &r)) {
      /* 24.4 rule 3: a stop the Core verified (reason stopped; invalid_key or server_rejected by 15.3) */
      t_stopped = 1;
      rc = tf__fail_gate(c);
      tf__cause_now(rc, "lease", "stopped", r.status, NULL, tf__client_gate(c));
    } else if (r.status == 401 && !strcmp(code, "install_cert_invalid") && !*renewed) {
      tf_mutex_lock(&p->lock);
      tf__ikey_drop_cert(p->ik);
      tf_mutex_unlock(&p->lock);
      *renewed = 1;
      rc = 1;
    } else if (r.status == 401 && !strcmp(code, "challenge_invalid") && !*renewed) {
      *renewed = 1;
      rc = 1;
    } else if (r.status == 403 && !strcmp(code, "lease_integrity_failed") && !strcmp(why, "core") && under_piece && !*renewed) {
      /* 18.6.7: the proof under a new data piece was refused; the Core went back to its last good piece (above), and
       * the lease is asked once more under it. A piece that was already the last good one stays: the repeat is
       * refused the same way and the request fails. */
      *renewed = 1;
      rc = 1;
    } else if (r.status == 403 && !strcmp(code, "lease_integrity_failed")) {
      /* 18.4.10: the refusal names the failed check; the developer's message says it in words with the server's
       * tokens, the report's error_code is why:check (ERRORS_40.md 24.3, contract 69326fb); not a stop */
      char detail[400], ec[33];
      tf__integrity_detail(why, check, detail, sizeof detail);
      if (!tf__integrity_code(why, check, ec)) snprintf(ec, sizeof ec, "%s", code);
      rc = tf__fail(TF_ERR_SERVER_REJECTED, "%s", detail);
      tf__cause_now(rc, "lease", "integrity_failed", r.status, NULL, ec);
    } else if (r.status == 403 && !strcmp(code, "sdk_too_old")) {
      TfJson *e = tf_json_parse(r.body, r.body_n, NULL);
      rc = tf__fail_sdk_too_old(tf_json_str(tf_json_get(e, "min_sdk")) ? tf_json_str(tf_json_get(e, "min_sdk")) : "");
      tf_json_free(e);
    } else if (r.status == 404) {
      if (!strcmp(code, "not_found")) p->route_off = 1;
      rc = off_cause(tf__fail(TF_ERR_MODEL_UNAVAILABLE, NULL), "lease", 404, code);
    } else if (r.status == 400) {
      tf__client_report_issue(c, "update_failed", "lease_request", NULL); /* an SDK bug: reported, never retried as it is */
      rc = off_cause(tf__fail(TF_ERR_MODEL_UNAVAILABLE, "the lease request was refused as malformed (%s)", why[0] ? why : code), "lease", 400, code);
    } else if (r.status >= 400 && r.status < 500 && r.status != 429 && code[0]) {
      /* 24.4 rule 3: a refusal the Core did not verify as a stop (an unsigned one too) is server_rejected, reason
       * refused with the server's error (the integrity check's own reason for lease_integrity_failed). Only the API's
       * own answer is a refusal: a 4xx without its JSON error (a proxy's, a firewall's or a captive portal's page) is
       * http, tried later as a 5xx is (24.3; 18.4.7.1 counts it unreachable; Stage B, staging, 11-18: an HTML 403 read
       * refused) */
      rc = tf__fail(TF_ERR_SERVER_REJECTED, "%s%s%s", code[0] ? code : "refused", why[0] ? ": " : "", why);
      tf__cause_now(rc, "lease", "refused", r.status, NULL, code);
    } else {
      /* 429, 5xx, a redirect: try later (18.4.7's backoff), never a refusal */
      rc = tf__fail(TF__ERR_HTTP, "lease: HTTP %d %s", r.status, code);
    }
    tf_http_response_free(&r);
    return rc;
  }
  /* a 200: the Core accepted its lease on the host that gave it (lease_taken), or every host was unreachable and this
   * is the first host's answer, which fails as it did there */
  if (!lt.accepted) {
    if (lt.bad == 3) {
      t_cause = tf__pm_lease_expired(lt.a, lt.d) ? 3 : 6;
      rc = tf__pm_error(lt.a, lt.d);
    } else if (lt.bad == 2) {
      tf__cause("lease", "invalid_answer", 200, NULL, "invalid_response");
      rc = tf__fail(TF__ERR_HTTP, "invalid_response: the lease answer names a model folder that is not on Quran Lab's hosts; nothing was downloaded");
    } else {
      tf__cause("lease", "invalid_answer", 200, NULL, NULL); /* a 200 that is not what was asked for */
      rc = tf__fail(TF__ERR_HTTP, "lease: no lease in the answer");
    }
  }
  tf_http_response_free(&r);
  return rc;
}

/* The lease for this build, so its files can be installed and opened: kept when the Core holds a valid one for it
 * and its refresh is not due; else asked for now (a refresh that is due goes to the background when the lease in
 * hand is still valid). Without a network a valid lease for this build is enough. 0, or an error. */
/* ------------------------------------------------------------------ grace (18.4.7.1) */
/* the 24.3 reason of the failure that ended an attempt, as the report of an open in grace names it */
static const char *grace_reason(int st) {
  static const char *const ok[] = {"network", "timeout", "http", "rate_limited", "redirect", "invalid_answer", "invalid", "lease_unavailable"};
  const char *r = tf__cause_reason();
  for (size_t i = 0; i < sizeof ok / sizeof ok[0]; ++i)
    if (!strcmp(r, ok[i])) return ok[i];
  if (!st) return "network";
  if (st == 429) return "rate_limited";
  if (st >= 300 && st < 400) return "redirect";
  if (st >= 200 && st < 300) return "invalid"; /* a lease the Core refused */
  return "http";
}
/* An attempt that brought no lease, told to the Core by how it ended (the file's comment). seq0: this thread's
 * request count before it; code: the code a call fails with for it. */
static void attempt_ended(TfPm *p, unsigned seq0, int code, int stopped) {
  int st = 0, api = 0;
  const unsigned seq = tf__http_answer(&st, &api);
  if (stopped || seq == seq0) return;
  const int refused = st >= 400 && st < 500 && st != 429 && api;
  tf_mutex_lock(&p->lock);
  qlc_lease_attempt(p->core, refused ? QLC_ATTEMPT_REFUSED : QLC_ATTEMPT_UNREACHABLE);
  p->att_end = tf__now_s();
  p->att_unreachable = !refused;
  p->att_code = code;
  p->att_status = st;
  snprintf(p->att_reason, sizeof p->att_reason, "%s", grace_reason(st));
  tf_mutex_unlock(&p->lock);
}
/* the code of a request that failed in the transport: needs_connection for no answer, download_failed for an answer
 * that began and then stalled (24.3 `timeout`, the cause this thread recorded) */
static int net_code(void) { return !strcmp(tf__cause_reason(), "timeout") ? TF_ERR_DOWNLOAD_FAILED : TF_ERR_NEEDS_CONNECTION; }
static void attempt_begin(TfPm *p) {
  tf_mutex_lock(&p->lock);
  p->attempting++;
  tf_mutex_unlock(&p->lock);
}
static void attempt_done(TfPm *p, int leased) {
  tf_mutex_lock(&p->lock);
  if (leased) p->att_end = 0; /* the Core cleared what it kept */
  p->attempting--;
  tf_cond_signal(&p->cv);
  tf_mutex_unlock(&p->lock);
}
long long tf__pm_next_try(TfPm *p) { return p ? p->next_try : 0; }
int tf__pm_grace(TfPm *p, long long *until) {
  int64_t u = 0;
  if (!p) return 0;
  tf_mutex_lock(&p->lock);
  const int g = qlc_lease_grace(p->core, &u) == 1;
  tf_mutex_unlock(&p->lock);
  if (until) *until = (long long)u;
  return g;
}
/* 24.1 item 2: an open in grace, silently reported with the answer that ended the attempt */
static void grace_report(TfPm *p) {
  tf_mutex_lock(&p->lock);
  const int st = p->att_status;
  char why[33]; /* 16 until 11-18: lease_unavailable reached the report cut to lease_unavailab (Stage B) */
  snprintf(why, sizeof why, "%s", p->att_reason[0] ? p->att_reason : "network");
  tf_mutex_unlock(&p->lock);
  tf__rep_silent("lease", "grace", st, NULL, why);
}

int tf__pm_lease(TfClient *c, TfPm *p, const char *manifest, size_t mn, const TfJson *m, const char *build) {
  if (!p) return tf__error_code() ? tf__error_code() : tf__fail(TF_ERR_STORAGE_FAILED, "no install key store");
  qlc_lease_status s;
  tf_mutex_lock(&p->lock);
  qlc_tick(p->core, &s);
  const int same = !strcmp(p->build, build);
  tf_mutex_unlock(&p->lock);
  if (s.stopped && !*tf__client_gate(c)) {
    /* a stop an earlier run verified: the key is accepted again only through a new lease */
  } else if (s.valid && same) {
    if (s.refresh_due && !p->next_try) p->next_try = now_s(); /* the timer refreshes it */
    return 0;
  }
  if (p->route_off) return off_cause(tf__fail(TF_ERR_MODEL_UNAVAILABLE, NULL), "lease", 404, "not_found");
  /* 18.4.7.1: this build's lease has expired. An unreachable outcome of this process less than a minute old serves the
   * open without a new request; else it waits for an attempt already running, then makes its own, all within ONE wait
   * of at most 10 s (`until`). An attempt of another thread still running at `until` is unreachable for this open (its
   * answer is used when it comes: a lease accepted, a refusal told to the Core). Until 11-18 the open waited 10 s for
   * the running attempt, then gave its own attempt 10 s more, and a second call of the same open waited for the
   * running one again: 32 s on staging behind a hanging lease request (Stage B). */
  const int expired = s.has_lease && !s.valid && !s.stopped && same;
  double until = 0;
  if (expired) {
    tf_mutex_lock(&p->lock);
    until = tf__now_s() + tf__lease_wait_s;
    int reuse = p->att_end > 0 && p->att_unreachable && tf__now_s() - p->att_end < tf__lease_reuse_s;
    if (!reuse) {
      while (p->attempting && tf__now_s() < until) tf_cond_wait_ms(&p->cv, &p->lock, (long)((until - tf__now_s()) * 1000) + 1);
      reuse = p->att_end > 0 && p->att_unreachable && tf__now_s() - p->att_end < tf__lease_reuse_s;
      if (!reuse && p->attempting) {
        qlc_lease_attempt(p->core, QLC_ATTEMPT_UNREACHABLE);
        p->att_end = tf__now_s();
        p->att_unreachable = 1;
        p->att_code = TF_ERR_NEEDS_CONNECTION;
        p->att_status = 0;
        snprintf(p->att_reason, sizeof p->att_reason, "%s", "timeout");
        reuse = 1;
      }
    }
    const int code = p->att_code, st = p->att_status;
    char why[33];
    snprintf(why, sizeof why, "%s", p->att_reason[0] ? p->att_reason : "network");
    tf_mutex_unlock(&p->lock);
    if (reuse) {
      if (tf__pm_grace(p, NULL)) return 0; /* the open reports it (tf__pm_open) */
      const int rc = code == TF_ERR_DOWNLOAD_FAILED ? tf__fail(code, "the lease request of less than a minute ago brought no lease")
                                                   : tf__fail(TF_ERR_NEEDS_CONNECTION, NULL);
      /* 24.3: the cause is the attempt this open reuses (until 11-18 it had none, and the report read download, other) */
      tf__cause_now(rc, "lease", why, st, NULL, !strcmp(why, "lease_unavailable") ? why : NULL);
      return rc;
    }
  }
  attempt_begin(p);
  const unsigned seq0 = tf__http_answer(NULL, NULL);
  if (expired) tf__client_deadline(until);
  t_stopped = 0;
  t_retry_after = 0;
  int renewed = 0, rc = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const char *cert = tf__ikey_cert(p->ik);
    if (!cert || tf__ikey_cert_exp(p->ik) - now_s() < 3600) {
      rc = tf__pm_attest(c, p);
      if (rc) {
        t_cause = rc == TF__ERR_NETWORK ? 1 : 5;
        break;
      }
    }
    rc = lease_once(c, p, manifest, mn, m, build, &renewed);
    if (rc != 1) break;
  }
  tf__client_deadline(0);
  if (rc == 1) rc = tf__fail(TF_ERR_SERVER_REJECTED, "the lease was refused twice"); /* rule 3: a refusal, not a stop */
  tf__client_count_lease(c, rc ? t_cause : 0); /* 17.8 lease: a request was made */
  const int stopped = t_stopped;
  /* no answer, or try later: a valid lease of this build still opens it */
  if (rc && (rc == TF__ERR_NETWORK || rc == TF__ERR_HTTP) && s.valid && same) {
    attempt_ended(p, seq0, rc == TF__ERR_NETWORK ? net_code() : TF_ERR_DOWNLOAD_FAILED, stopped);
    attempt_done(p, 0);
    tf__clear_error();
    return 0;
  }
  /* 24.4 rule 1: needs_connection only when no answer came at all; an answer that began and then stalled (24.3
   * `timeout`) is an answer, download_failed by rule 2 (until 11-18 it was needs_connection, and the report lost its
   * cause: Stage B, staging) */
  if (rc == TF__ERR_NETWORK) {
    const int nc = net_code();
    rc = tf__fail(nc, nc == TF_ERR_DOWNLOAD_FAILED ? "the lease answer began and then stalled" : NULL);
  }
  /* 24.4 rule 2: a server's answer that is not a refusal (429, 5xx, a redirect, a 200 without a lease) is
   * download_failed, never the server_rejected an internal HTTP error would read as: try again later */
  if (rc == TF__ERR_HTTP) rc = tf__error_as(TF_ERR_DOWNLOAD_FAILED);
  if (rc) attempt_ended(p, seq0, rc, stopped);
  attempt_done(p, !rc);
  /* an answer that named when to ask again: the timer's next attempt is not before it */
  if (rc && t_retry_after > 0) p->next_try = now_s() + t_retry_after;
  /* 18.4.7.1: the expired lease opens in its grace (the Core decides: an attempt ended unreachable since the lease,
   * none refused, before exp + 86400); a request the 10 s cut is asked again at the timer's next tick */
  if (rc && !stopped && same && tf__pm_grace(p, NULL)) {
    if (expired && t_retry_after <= 0) p->next_try = now_s();
    tf__clear_error();
    return 0; /* the open reports it (tf__pm_open) */
  }
  return rc;
}

/* the delays of 18.4.7 after a failed background refresh */
static long long backoff(int fails) {
  static const long long d[] = {60, 300, 1800, 7200, 14400, 21600};
  return d[fails < 6 ? fails : 5];
}

/* On the Client's timer: the Core's tick (the lease's clock, the day's audio, the recited journal, the saved state),
 * and the refresh when it is due. The manifest of the build in use is the one on disk. */
void tf__pm_tick(TfClient *c, const char *build_dir, const char *build) {
  TfPm *p = *slot(c);
  if (!p) return;
  qlc_lease_status s;
  tf_mutex_lock(&p->lock);
  qlc_tick(p->core, &s);
  tf_mutex_unlock(&p->lock);
  if (!build_dir || !build || !s.has_lease || s.stopped || p->route_off || !(s.refresh_due || !s.valid)) return;
  if (p->next_try && now_s() < p->next_try) return;
  char *mp = tf_path_join(build_dir, "manifest.json"), *text = NULL;
  size_t n = 0;
  TfJson *m = mp && !tf_read_file(mp, &text, &n) ? tf_json_parse(text, n, NULL) : NULL;
  free(mp);
  int rc = -1;
  if (m) {
    int renewed = 0;
    attempt_begin(p);
    const unsigned seq0 = tf__http_answer(NULL, NULL);
    t_stopped = 0;
    t_retry_after = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!tf__ikey_cert(p->ik) || tf__ikey_cert_exp(p->ik) - now_s() < 3600) {
        rc = tf__pm_attest(c, p); /* waits until the model is closed */
        if (rc) break;
      }
      rc = lease_once(c, p, text, n, m, build, &renewed);
      if (rc != 1) break;
    }
    /* 18.4.7.1: a background attempt that brought no lease is told to the Core too */
    if (rc && rc != TF__ERR_NOT_AVAILABLE)
      attempt_ended(p, seq0, rc == TF__ERR_NETWORK ? TF_ERR_NEEDS_CONNECTION : rc == 1 ? TF_ERR_SERVER_REJECTED : TF_ERR_DOWNLOAD_FAILED, t_stopped);
    attempt_done(p, !rc);
  }
  if (m && rc != TF__ERR_NOT_AVAILABLE) tf__client_count_lease(c, rc ? (rc == 1 ? 6 : t_cause) : 0);
  if (rc && rc != TF__ERR_NOT_AVAILABLE) tf__cause_silent(); /* 24.1: the background refresh failed (not on the network) */
  if (rc) {
    /* 18.4.7's backoff; an answer's Retry-After (503 lease_unavailable) sets the next attempt instead */
    p->next_try = now_s() + (t_retry_after > 0 ? t_retry_after : backoff(p->fails));
    if (p->fails < 100) p->fails++;
  } else {
    p->next_try = 0;
  }
  tf__clear_error(); /* a background refresh is never the app's error */
  tf_json_free(m);
  free(text);
}

/* ------------------------------------------------------------------ install and open */
int tf__pm_install(TfPm *p, const char *build_dir, const char *manifest, size_t mn, const char *decoded_path) {
  tf_mutex_lock(&p->lock);
  tf__plat_set_build(p->plat, build_dir);
  const int st = qlc_install(p->core, manifest, mn, decoded_path);
  const int d = qlc_detail(p->core);
  tf_mutex_unlock(&p->lock);
  t_core_stage = "download"; /* a container the Core refuses at its install is the download's */
  const int rc = st ? tf__pm_error(st, d) : 0;
  t_core_stage = NULL;
  return rc;
}

qlc_model *tf__pm_open(TfPm *p, const char *build_dir, const char *manifest, size_t mn, const char *context, int threads, int rate, int pin) {
  qlc_open_options o = qlc_open_default_options();
  o.context = context;
  o.threads = threads;
  o.sample_rate = rate == 16000 ? 0 : rate;
  o.no_pin = pin ? (pin < 0 ? 1 : 0) : -1;
  int st = 0;
  tf_mutex_lock(&p->lock);
  tf__plat_set_build(p->plat, build_dir);
  qlc_model *m = tf__pm_open_fail_status ? NULL : qlc_model_open(p->core, manifest, mn, &o, &st);
  const int d = tf__pm_open_fail_status ? 0 : qlc_detail(p->core);
  if (tf__pm_open_fail_status) st = tf__pm_open_fail_status;
  if (m) {
    p->open_models++;
    p->refs++;
  }
  tf_mutex_unlock(&p->lock);
  if (!m) {
    t_core_open = 1; /* 24.3: the open's status names the report's error_code */
    tf__pm_error(st, d);
    t_core_open = 0;
  } else if (tf__pm_grace(p, NULL)) {
    grace_report(p); /* 24.1 item 2: each open in grace (18.4.7.1) */
  }
  return m;
}

void tf__pm_close(TfPm *p, qlc_model *m) {
  if (!p || !m) return;
  qlc_model_close(m);
  tf_mutex_lock(&p->lock);
  p->open_models--;
  const int last = --p->refs == 0;
  tf_mutex_unlock(&p->lock);
  if (last) pm_free(p);
}

int tf__pm_status(TfPm *p, qlc_lease_status *s) {
  memset(s, 0, sizeof *s);
  if (!p) return -1;
  tf_mutex_lock(&p->lock);
  const int rc = qlc_tick(p->core, s);
  tf_mutex_unlock(&p->lock);
  return rc;
}

void tf__pm_lock(TfPm *p) { tf_mutex_lock(&p->lock); }
void tf__pm_unlock(TfPm *p) { tf_mutex_unlock(&p->lock); }

const char *tf__lease_test_base; /* tests: a stand-in server's base (http://127.0.0.1) is taken */
int tf__pm_open_fail_status;     /* tests: qlc_model_open is not called and fails with this status (0: never) */
