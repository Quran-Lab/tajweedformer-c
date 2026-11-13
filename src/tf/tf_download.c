/* The model downloader: the folder's signed manifest, then the CPU set (the tokens, the calibration, the engine
 * files of the contexts asked for and the sample clips; never the expected table of 2.x builds, CoreML or NPU
 * files, and the reference data only when it is asked for by its role), each file resumed with HTTP Range from
 * its .part and checked against its size and SHA-256, then moved into place. Layout 3 (manifest format 3): the
 * profile comes from the tags, gzip files are checked as stored, decoded and checked again, and only the decoded
 * file is kept. The manifest is written last, so a folder with a manifest.json is complete for what was asked.
 *
 * 3.1.3: every request of one download goes on one kept connection; a request ends when no byte arrives for 30 s
 * (it has no limit on its whole time); and a file whose transfer breaks is resumed inside the call from its .part,
 * with the validator of the answer that started it, at most five times (fetch_raw).
 *
 * SPEC 2a: files come from the build URL ({base}/{repo}/{build}/{folder}/{path}, immutable); the manifest must name
 * its version in the signed text, the version must be the one the signed config named, and the manifest's SHA-256
 * must start with the build the config named. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

int tf_gunzip_file(const char *in_path, const char *out_path);

int tf__download_stall_ms = 30000;
int tf__download_pause_ms = 1000;

TfDownload tf__download_defaults(void) {
  TfDownload o;
  memset(&o, 0, sizeof o);
  o.cdn_url = "https://cdn.quranlab.ai/models";
  o.repo = "Quran-Lab/tajweedformer-v4";
  o.folder = "sdk/v4.0";
  return o;
}

typedef struct {
  const TfDownload *o;
  long long done, total; /* bytes as stored, over the files asked for */
  TfHttpConn *conn;      /* 3.1.3: the one connection to the model host, kept from the manifest to the last file */
} Run;
/* one request for a file: what the transport's callbacks need */
typedef struct {
  Run *run;
  const char *part;       /* the partial file the body goes to; NULL for the manifest */
  const char *whole;      /* where a whole file sent for a range goes, beside the partial file */
  long long from, before; /* the request's first byte; run->done before this file */
  long long size;         /* the file's size, from the manifest */
  char etag[80];          /* the validator of a whole file that came for a range, until it is in place */
} Xfer;

/* The network failures of one file that one call rides out: after each the call waits (tf__download_pause_ms,
 * doubling: 1, 2, 4, 8, 16 s) and resumes from the partial file; the failure after the fifth resume is the
 * call's error. Progress does not start the count again: with it a server that sends a little and breaks off
 * each time could hold one call for hours, and nothing is lost at the bound, because the partial file and its
 * validator stay and the next call goes on from them with five resumes of its own. */
#define TF_DOWNLOAD_RESUMES 5

static long long file_size(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  const long long n = ftell(f);
  fclose(f);
  return n;
}

static int ctx_wanted(const char *ctx, size_t n, const char *contexts) {
  if (!contexts) return 1;
  for (const char *s = contexts; *s;) {
    const char *e = strchr(s, ',');
    const size_t k = e ? (size_t)(e - s) : strlen(s);
    if (k == n && !strncmp(s, ctx, n)) return 1;
    if (!e) break;
    s = e + 1;
  }
  return 0;
}
/* layout 1/2 (manifest format 2): top-level files and engine context folders; the expected table of a 2.x folder
 * is never fetched */
static int in_profile2(const char *path, const char *contexts, const char *only_role) {
  if (only_role) return 0;
  if (!strncmp(path, "coreml/", 7) || !strncmp(path, "npu/", 4) || !strcmp(path, "expected_tj3.zfx")) return 0;
  const char *slash = strchr(path, '/');
  if (!slash) return 1;
  if (strchr(slash + 1, '/')) return 0;
  if (!strncmp(path, "selftest/", 9)) return 1;
  return ctx_wanted(path, (size_t)(slash - path), contexts);
}
/* layout 3: 1 when a format 3 entry belongs to what is asked for, 0 when not, -1 when its platforms field is
 * malformed. platforms is "*" (every platform; what make_manifest3.py writes and the signed text carries) or a
 * list of platform names (a list may also hold "*"). The profile: platform native or *, soc *, need required,
 * context * or one asked for; plus the sample clips (role selftest, optional in the manifest); minus the roles
 * expected (2.x builds: the scorer's table, never read by 3.0) and reference (fetched only by its role). */
static int in_profile3(const TfJson *f, const char *contexts, const char *only_role) {
  const char *need = tf_json_str(tf_json_get(f, "need")), *cx = tf_json_str(tf_json_get(f, "context"));
  const char *soc = tf_json_str(tf_json_get(f, "soc")), *role = tf_json_str(tf_json_get(f, "role"));
  const TfJson *pl = tf_json_get(f, "platforms");
  int plat = 0;
  if (pl && pl->type == TJ_STR) {
    if (strcmp(pl->str, "*")) return -1;
    plat = 1;
  } else if (pl && pl->type == TJ_ARR) {
    for (int k = 0; k < pl->n; ++k) {
      if (!tf_json_str(&pl->kids[k])) return -1;
      if (!strcmp(pl->kids[k].str, "native") || !strcmp(pl->kids[k].str, "*")) plat = 1;
    }
  } else {
    return -1;
  }
  if (!plat || (soc && strcmp(soc, "*"))) return 0;
  const int ctx_ok = !cx || !strcmp(cx, "*") || ctx_wanted(cx, strlen(cx), contexts);
  if (only_role) return role && !strcmp(role, only_role) && ctx_ok;
  if (role && (!strcmp(role, "expected") || !strcmp(role, "reference"))) return 0;
  if (role && !strcmp(role, "selftest")) return ctx_ok;
  if (!need || strcmp(need, "required")) return 0;
  return ctx_ok;
}

static int file_ok(const char *full, long long size, const char *sha_hex) {
  uint8_t want[32], got[32];
  long long n = 0;
  return !tf_hex_decode(sha_hex, want, 32) && !tf_sha256_file(full, got, &n) && n == size && !memcmp(got, want, 32);
}

/* {base}/{repo}/{revision}/{folder}/{rel}: Quran Lab's model hosting (base https://cdn.quranlab.ai/models). A bare cdn.quranlab.ai
 * base, or a regional {region}.cdn.quranlab.ai, keeps the old shape {cdn}/hf/{repo}/..., served for every revision.
 * Returns 0, or -1 when the URL does not fit in n bytes. */
int tf__model_url(char *out, size_t n, const char *base, const char *repo, const char *revision, const char *folder, const char *rel) {
  static const char *const legacy[] = {"cdn", "weur.cdn", "eeur.cdn", "enam.cdn", "wnam.cdn", "apac.cdn", "oc.cdn"};
  size_t bl = strlen(base);
  while (bl && base[bl - 1] == '/') bl--;
  const char *hf = "";
  for (size_t i = 0; i < sizeof legacy / sizeof legacy[0]; ++i) {
    char want[64];
    snprintf(want, sizeof want, "https://%s.quranlab.ai", legacy[i]);
    if (bl == strlen(want) && !strncmp(base, want, bl)) hf = "/hf";
  }
  const int w = snprintf(out, n, "%.*s%s/%s/%s/%s/%s", (int)bl, base, hf, repo, revision, folder, rel);
  return w < 0 || (size_t)w >= n ? -1 : 0;
}

/* bytes of the files asked for that are here: the progress callback's count */
static void add_done(Run *run, long long n) {
  run->done += n;
  if (run->o->progress) run->o->progress(run->o->user, run->done > run->total ? run->total : run->done, run->total);
}
/* bytes that came over the network in this call (the telemetry's downloads entry counts them) */
static void net_bytes(void *user, long long n) {
  Run *run = ((Xfer *)user)->run;
  if (run->o->fetched) *run->o->fetched += n;
  add_done(run, n);
}

/* The validator of a partial file: the ETag of the answer that started it, kept beside it as <file>.part.etag.
 * A resume sends it as If-Range with the Range, so a server whose file is no longer that one answers 200 with
 * the whole file, which replaces the partial file, and not with bytes that do not belong to it. Only a strong
 * ETag validates a range (RFC 9110, 13.1.5); a weak one is not kept. Without a validator the resume goes with
 * the Range alone, as before 3.1.3: the URL names the build, and the SHA-256 of the whole file is checked
 * before it is moved into place, whatever was sent. That check is the last word in every case. */
static char *etag_path(const char *part) {
  char *p = (char *)malloc(strlen(part) + sizeof(".etag"));
  if (p) snprintf(p, strlen(part) + sizeof(".etag"), "%s.etag", part);
  return p;
}
static void etag_write(const char *part, const char *etag) {
  char *p = etag_path(part);
  if (!p) return;
  if (etag && etag[0] == '"') tf_write_file(p, etag, strlen(etag));
  else remove(p);
  free(p);
}
static void etag_read(const char *part, char out[80]) {
  char *p = etag_path(part), *text = NULL;
  size_t n = 0;
  out[0] = 0;
  if (p && !tf_read_file(p, &text, &n) && text) {
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r' || text[n - 1] == ' ')) n--;
    if (n >= 2 && n < 80 && text[0] == '"') {
      memcpy(out, text, n);
      out[n] = 0;
    }
  }
  free(text);
  free(p);
}
/* the status and the headers of a file request are in, the file is open and no byte of the body is written */
static void on_head(void *user, const TfHttpResponse *r) {
  Xfer *x = (Xfer *)user;
  if (!x->part || r->status != 200) return; /* a 206 continues the partial file: its validator stays */
  if (x->from > 0) {
    /* a whole file for a range request: it is written beside the partial file, which keeps its bytes and
     * its validator until all of the whole file is here (fetch_raw). The count starts again for it */
    snprintf(x->etag, sizeof x->etag, "%s", r->etag);
    x->run->done = x->before;
    add_done(x->run, 0);
    return;
  }
  /* a 200 to a request without a range starts the partial file, with this answer's validator */
  etag_write(x->part, r->etag);
}
/* Is this the answer a file request asked for? Asked before a byte of it is written. A 200 whose length,
 * when the answer names one, is the file's; or a 206, to a request that named a range, whose Content-Range
 * starts at the request's first byte and names the file's size. A 200 of another length, a 206 of another
 * start or total or without a Content-Range, and a 206 nobody asked for are refused: a failed attempt, with
 * nothing written. Every other status passes here (fetch_raw says what it is). */
static int accept_head(void *user, const TfHttpResponse *r) {
  const Xfer *x = (const Xfer *)user;
  if (!x->part) return 1;
  if (r->status == 200) return r->content_length < 0 || r->content_length == x->size;
  if (r->status == 206) return x->from > 0 && r->range_start == x->from && r->range_total == x->size;
  return 1;
}

/* One model request. `net` is set to 1 when it failed on the network (no answer, a connection that ended, a
 * stall): the failures a file request is resumed after. */
static int get(Run *run, const char *rel, Xfer *x, const char *if_range, TfHttpResponse *r, int *net) {
  const TfDownload *o = run->o;
  char url[4096], key[512];
  *net = 0;
  if (tf__model_url(url, sizeof url, o->cdn_url ? o->cdn_url : "https://cdn.quranlab.ai/models", o->repo, o->revision, o->folder, rel))
    return tf__fail(TF_ERR_INVALID_ARGUMENT, "the model URL of %s is too long", rel);
  /* SPEC 15.4: every model request (the manifest, a file, a Range request) carries the install's app token, and
   * in 3.1.0 the key too. Both headers are made here and nowhere else. */
  snprintf(key, sizeof key, "x-qlab-key: %s", o->key ? o->key : "");
  for (int attempt = 0; attempt < 2; ++attempt) {
    char auth[1200] = "", token[1024] = "";
    if (o->client) {
      /* no token, no request */
      if (tf__client_model_token(o->client, attempt > 0, token, sizeof token)) return tf__error_as(TF_ERR_DOWNLOAD_FAILED);
      snprintf(auth, sizeof auth, "authorization: Bearer %s", token);
    }
    const char *hdr[] = {key, auth[0] ? auth : NULL, NULL};
    TfHttpRequest q;
    memset(&q, 0, sizeof q);
    q.method = "GET";
    q.url = url;
    q.headers = hdr;
    q.download_file = x->part;
    q.download_whole = x->whole;
    q.download_size = x->part ? x->size : 0;
    q.accept = accept_head;
    q.range_from = x->from;
    q.if_range = if_range;
    /* 3.1.3: no byte for 30 s ends the request; there is no limit on the whole transfer (it was one hour) */
    q.stall_ms = tf__download_stall_ms;
    q.conn = run->conn;
    q.cancel = o->cancel;
    q.on_head = on_head;
    q.bytes_user = x;
    if (x->part) q.on_bytes = net_bytes;
    if (tf_http(&q, r)) {
      /* a failed write is the device's, not the network's; the .part stays either way */
      if (tf__error_code() == TF__ERR_IO) return tf__error_as(TF_ERR_STORAGE_FAILED);
      *net = !(o->cancel && *o->cancel);
      return tf__error_as(TF_ERR_DOWNLOAD_FAILED);
    }
    /* the protocol this answer came on, for the diagnosis: a record on this device, in no telemetry */
    if (o->client && r->protocol[0]) tf__client_set_download_protocol(o->client, r->protocol);
    /* 401 invalid_token without a reason: the token is renewed once and the request repeated once */
    char why[48];
    if (r->status == 401 && o->client && attempt == 0 && r->body && strstr(r->body, "invalid_token") &&
        !tf__body_stop_reason(r->body, r->body_n, why)) {
      tf_http_response_free(r);
      continue;
    }
    break;
  }
  return 0;
}

/* an error status of the CDN as a catalogue error: the key's answers by their code, anything else a stopped
 * download */
static int http_error(const Run *run, const char *rel, const TfHttpResponse *r) {
  const char *b = r->body ? r->body : "";
  /* SPEC 15.4: a redirect is not followed (tf_http.c). It is a failed download and never a stop: whatever its
   * body says, no refusal is remembered and nothing is removed */
  if (r->status >= 300 && r->status < 400)
    return tf__fail(TF_ERR_DOWNLOAD_FAILED, "the model host answered the request for %s with a redirect (HTTP %d), which is never followed", rel, r->status);
  /* SPEC 15.3: a reason of the list refuses the key or the app, and stops the model on this device */
  char stop[48];
  if (tf__body_stop_reason(r->body, r->body_n, stop)) {
    if (run->o->client) tf__client_refuse(run->o->client, stop);
    return tf__key_reason(stop) ? tf__fail(TF_ERR_INVALID_KEY, NULL)
                                : tf__fail_rejected(stop, run->o->client ? tf__client_app(run->o->client) : NULL);
  }
  /* else the refusal's code: the body's reason when the catalogue knows it, else its error (NAMING section 7) */
  char code[64] = "";
  TfJson *j = r->body ? tf_json_parse(r->body, r->body_n, NULL) : NULL;
  const char *why = tf_json_str(tf_json_get(j, "reason")), *err = tf_json_str(tf_json_get(j, "error"));
  if (why && strlen(why) < sizeof code && tf__reason_known(why)) snprintf(code, sizeof code, "%s", why);
  else if (err && strlen(err) < sizeof code) snprintf(code, sizeof code, "%s", err);
  tf_json_free(j);
  /* model_attestation_required and model_not_allowed are refusals of this request only (SPEC 15.4), and so is
   * any other refusal the catalogue knows that came without a reason: an error with its own words, no stop */
  if ((r->status == 401 || r->status == 403) && tf__reason_known(code))
    return tf__fail_rejected(code, run->o->client ? tf__client_app(run->o->client) : NULL);
  if (r->status == 429 && !strcmp(code, "model_limit"))
    return tf__fail(TF_ERR_DOWNLOAD_FAILED, "Quran Lab's daily limit for model downloads is reached; try again later");
  if (r->status == 503 && !strcmp(code, "network_blocked"))
    return tf__fail(TF_ERR_DOWNLOAD_FAILED, "Quran Lab does not serve this network at the moment; try again later or from another network");
  if (r->status == 401 && !strcmp(code, "invalid_token")) return tf__fail(TF_ERR_DOWNLOAD_FAILED, "Quran Lab did not take this install's token for %s", rel);
  if (r->status == 403 && strstr(b, "sdk_too_old")) return tf__fail_sdk_too_old(NULL);
  /* a 401 without a reason: a key the CDN does not know */
  if (r->status == 401) return tf__fail(TF_ERR_INVALID_KEY, NULL);
  if (r->status == 403) return tf__fail(TF_ERR_SERVER_REJECTED, "HTTP %d for %s", r->status, rel);
  return tf__fail(TF_ERR_DOWNLOAD_FAILED, "HTTP %d for %s", r->status, rel);
}

/* The pause before the `nth` resume (0 for the first). 1 when the download was cancelled, at once. */
static int pause_before_resume(const TfDownload *o, int nth) {
  long long ms = (long long)tf__download_pause_ms << nth;
  while (ms > 0 && !(o->cancel && *o->cancel)) {
    const int step = ms > 50 ? 50 : (int)ms;
    tf__sleep_ms(step);
    ms -= step;
  }
  return o->cancel && *o->cancel;
}

/* download `rel` (stored bytes: size, sha) to `to` through `to`.part, resuming; 0 when `to` is verified.
 *
 * 3.1.3: a failure of the network does not end the call. Resumed, up to TF_DOWNLOAD_RESUMES times with a pause
 * that grows: no answer, a connection that ended, a stall, an answer that ended before its last byte, and a 5xx.
 * Never asked again in this call: an answer with a stop reason (SPEC 15.3), any 401, 403 or 429 (beyond the one
 * token renewal in get), a 503 network_blocked (the server names an hour), a redirect (never followed: SPEC
 * 15.4), any other status, a failed write.
 *
 * Only the answer that was asked for is taken: a 200 of the file's length, or a 206 whose Content-Range starts
 * at the request's first byte and names the file's size (accept_head, before a byte is written). Any other 2xx
 * (a 204, a 200 of another length, a 206 of another start or total, an answer that brings no byte) is a failed
 * attempt: it counts against the resumes and the pause applies, so nothing is asked for ever. A failed attempt
 * never takes a byte from the partial file: a whole file sent for a range is written beside it (.part.new) and
 * takes its place only when all of it has come. The partial file is removed in two cases only: a 416 (the
 * server has no such range: the bytes here are past its file), and a file of the full size whose SHA-256 is not
 * the manifest's (every byte of it came in an answer that was the one asked for, and it is still not the
 * file: no part of it can be told from another, and kept it would fail every later call the same way). */
static int fetch_raw(Run *run, const char *rel, long long size, const char *sha, const char *to) {
  if (file_ok(to, size, sha)) {
    add_done(run, size);
    return 0;
  }
  const size_t tn = strlen(to);
  char *part = (char *)malloc(tn + sizeof(".part")), *whole = (char *)malloc(tn + sizeof(".part.new"));
  if (!part || !whole) {
    free(part);
    free(whole);
    return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  }
  snprintf(part, tn + sizeof(".part"), "%s.part", to);
  snprintf(whole, tn + sizeof(".part.new"), "%s.part.new", to);
  int rc = 0, resumes = 0, mismatches = 0;
  const long long before = run->done;
  for (;;) {
    /* a whole file beside the partial file, left by a call that ended between its two steps: it is the
     * partial file when it has the size and nothing else is there; else it goes */
    if (file_size(whole) == size && file_size(part) < 0) rename(whole, part);
    else remove(whole);
    long long from = file_size(part);
    if (from < 0 || from > size) {
      remove(part);
      from = 0;
    }
    if (from == 0) etag_write(part, NULL); /* no bytes, so nothing to validate */
    run->done = before;
    add_done(run, from); /* what an earlier request already fetched */
    /* resumed, as the telemetry counts it: this call went on from a partial file an earlier call left. A resume
     * inside the call is not counted (it would make every download that rode out one break a resumed one) */
    if (from > 0 && from < size && resumes == 0 && run->o->resumed) *run->o->resumed = 1;
    if (from < size) {
      char tag[80] = "";
      if (from > 0) etag_read(part, tag);
      Xfer x;
      memset(&x, 0, sizeof x);
      x.run = run;
      x.part = part;
      x.whole = whole;
      x.from = from;
      x.before = before;
      x.size = size;
      TfHttpResponse r;
      int again = 0; /* 1: a failed attempt of the kind a resume is for */
      rc = get(run, rel, &x, tag[0] ? tag : NULL, &r, &again);
      if (!rc) {
        const int st = r.status;
        char why[48];
        if (st == 200 && from > 0 && file_size(whole) >= 0) {
          /* the whole file came for a range, beside the partial file: all of it, and it takes the partial
           * file's place with its own validator; less, and the partial file stays as it was */
          if (file_size(whole) == size) {
            remove(part);
            if (rename(whole, part)) rc = tf__fail(TF_ERR_STORAGE_FAILED, "cannot put %s in the place of its partial file", rel);
            else etag_write(part, x.etag);
          } else {
            again = 1;
            rc = tf__fail(TF_ERR_DOWNLOAD_FAILED, "the answer for %s ended early", rel);
          }
        } else if (st == 200 || st == 206) {
          /* an answer that ended before the file's last byte, or brought none, without an error of the
           * transport, is a connection that ended */
          if (file_size(part) < size) {
            again = 1;
            rc = tf__fail(TF_ERR_DOWNLOAD_FAILED, "the answer for %s ended early", rel);
          }
        } else if (st >= 200 && st < 300) {
          /* a 204 or any other 2xx is not an answer to a file request */
          again = 1;
          rc = tf__fail(TF_ERR_DOWNLOAD_FAILED, "HTTP %d is not an answer to the request for %s", st, rel);
        } else {
          again = st >= 500 && !tf__body_stop_reason(r.body, r.body_n, why) && !(r.body && strstr(r.body, "network_blocked"));
          /* no answer takes the bytes already here, but a 416: the server has no such range, so they are past
           * its file */
          if (st == 416) {
            remove(part);
            etag_write(part, NULL);
          }
          rc = http_error(run, rel, &r);
        }
        tf_http_response_free(&r);
      }
      if (rc) {
        remove(whole); /* part of a whole file that did not all come */
        /* the failure after the last resume, and every failure a resume is not for, is the call's error */
        if (!again || resumes == TF_DOWNLOAD_RESUMES || pause_before_resume(run->o, resumes)) break;
        resumes++;
        continue;
      }
    }
    if (file_ok(part, size, sha)) {
      remove(to);
      rc = rename(part, to) ? tf__fail(TF_ERR_STORAGE_FAILED, "cannot move %s into place", rel) : 0;
      etag_write(part, NULL);
      run->done = before + size;
      break;
    }
    remove(part); /* the full size and not the file: it goes with its validator, and the file is fetched once more */
    etag_write(part, NULL);
    rc = tf__fail(TF_ERR_CHECKSUM_MISMATCH, "%s", rel);
    if (++mismatches == 2) break;
  }
  free(part);
  free(whole);
  return rc;
}

static void mkparent(const char *full) {
  char *dir = (char *)malloc(strlen(full) + 1);
  if (!dir) return;
  strcpy(dir, full);
  char *sl = strrchr(dir, '/');
  if (sl) {
    *sl = 0;
    tf_mkdirs(dir);
  }
  free(dir);
}

static int fetch_entry(Run *run, const TfJson *f) {
  const TfDownload *o = run->o;
  const char *p = tf_json_str(tf_json_get(f, "path"));
  const long long size = (long long)tf_json_get(f, "size")->num;
  const char *sha = tf_json_str(tf_json_get(f, "sha256"));
  const char *enc = tf_json_str(tf_json_get(f, "encoding"));
  if (!enc || !strcmp(enc, "identity")) {
    char *full = tf_path_join(o->dest, p);
    if (!full) return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
    mkparent(full);
    const int rc = fetch_raw(run, p, size, sha, full);
    free(full);
    return rc;
  }
  if (strcmp(enc, "gzip")) return tf__fail(TF_ERR_MODEL_INVALID, "%s has the unknown encoding %s", p, enc);
  const TfJson *d = tf_json_get(f, "decoded");
  const char *dp = tf_json_str(tf_json_get(d, "path"));
  const long long dsize = (long long)tf_json_get(d, "size")->num;
  const char *dsha = tf_json_str(tf_json_get(d, "sha256"));
  char *dfull = tf_path_join(o->dest, dp), *gz = tf_path_join(o->dest, p);
  int rc = 0;
  if (!dfull || !gz) rc = tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  else if (file_ok(dfull, dsize, dsha)) add_done(run, size);
  else {
    mkparent(gz);
    mkparent(dfull);
    rc = fetch_raw(run, p, size, sha, gz);
    if (!rc) {
      /* sizeof counts the suffix and its NUL; "+ 8" was one byte short (heap corruption for about half of all
       * destination path lengths once gzip files were really fetched, 11-08) */
      char *tmp = (char *)malloc(strlen(dfull) + sizeof(".inflate"));
      if (!tmp) {
        free(dfull);
        free(gz);
        return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
      }
      snprintf(tmp, strlen(dfull) + sizeof(".inflate"), "%s.inflate", dfull);
      if (tf_gunzip_file(gz, tmp) || !file_ok(tmp, dsize, dsha)) {
        remove(tmp);
        remove(gz); /* any mismatch deletes both */
        rc = tf__fail(TF_ERR_CHECKSUM_MISMATCH, "%s", dp);
      } else {
        remove(dfull);
        rc = rename(tmp, dfull) ? tf__fail(TF_ERR_STORAGE_FAILED, "cannot move %s into place", dp) : 0;
        remove(gz);
      }
      free(tmp);
    }
  }
  free(dfull);
  free(gz);
  return rc;
}

/* 1 when the entry's file is already in place and verified */
static int entry_done(const TfDownload *o, const TfJson *f) {
  const char *enc = tf_json_str(tf_json_get(f, "encoding"));
  const TfJson *loc = enc && !strcmp(enc, "gzip") ? tf_json_get(f, "decoded") : f;
  char *full = tf_path_join(o->dest, tf_json_str(tf_json_get(loc, "path")));
  const int ok = full && file_ok(full, (long long)tf_json_get(loc, "size")->num, tf_json_str(tf_json_get(loc, "sha256")));
  free(full);
  return ok;
}

int tf__download_model(const TfDownload *opt) {
  tf__clear_error();
  TfDownload o = opt ? *opt : tf__download_defaults();
  if (!o.cdn_url) o.cdn_url = "https://cdn.quranlab.ai/models";
  if (!o.repo) o.repo = "Quran-Lab/tajweedformer-v4";
  if (!o.folder) o.folder = "sdk/v4.0";
  if (!o.revision || !o.dest || !o.key) return tf__fail(TF_ERR_INVALID_ARGUMENT, "a key, a build and a destination folder are required");
  if (tf_mkdirs(o.dest)) return tf__fail(TF_ERR_STORAGE_FAILED, "cannot create %s", o.dest);
  Run run;
  memset(&run, 0, sizeof run);
  run.o = &o;
  /* 3.1.3: one connection for the whole download, opened by the manifest's request and closed below (without
   * one, each request opens its own, as before) */
  run.conn = tf__http_conn_open();
  TfHttpResponse r;
  /* The manifest is asked once: there is no partial file to go on from, nothing is lost when the call ends
   * here, and a start with a model on disk is not held up when the model host does not answer. */
  Xfer mx;
  memset(&mx, 0, sizeof mx);
  mx.run = &run;
  int net = 0;
  int rc = get(&run, "manifest.json", &mx, NULL, &r, &net);
  if (!rc && r.status != 200) {
    rc = http_error(&run, "manifest.json", &r);
    tf_http_response_free(&r);
  }
  if (rc) {
    tf__http_conn_close(run.conn);
    return rc;
  }
  /* the same acceptance checks, in the same order, as an open from disk (tf__manifest_accept) */
  TfJson *m = NULL;
  rc = tf__manifest_accept(r.body, r.body_n, o.check_build ? o.revision : NULL, o.want_version,
                           o.trusted_key_hex ? o.trusted_key_hex : TF_QURANLAB_KEY_HEX, 0, &m, NULL, NULL, NULL);
  const int format = rc ? 0 : (int)tf_json_get(m, "format")->num;
  const TfJson *files = rc ? NULL : tf_json_get(m, "files");
  if (!rc && (!files || files->type != TJ_ARR)) rc = tf__fail(TF_ERR_MODEL_INVALID, "manifest.json lists no files");
  for (int i = 0; !rc && i < files->n; ++i) {
    const TfJson *f = &files->kids[i];
    const char *p = tf_json_str(tf_json_get(f, "path"));
    const TfJson *d = tf_json_get(f, "decoded");
    const char *dp = d ? tf_json_str(tf_json_get(d, "path")) : NULL;
    const char *enc = tf_json_str(tf_json_get(f, "encoding"));
    if (!p || strstr(p, "..") || p[0] == '/' || strchr(p, '\\') || (dp && (strstr(dp, "..") || dp[0] == '/' || strchr(dp, '\\'))) ||
        !tf_json_is_int(tf_json_get(f, "size")) || !tf_json_str(tf_json_get(f, "sha256")) ||
        (enc && !strcmp(enc, "gzip") && (!dp || !tf_json_is_int(tf_json_get(d, "size")) || !tf_json_str(tf_json_get(d, "sha256")))))
      rc = tf__fail(TF_ERR_MODEL_INVALID, "manifest.json lists a path that is not inside the folder");
  }
  /* a context this build does not have (the config's inference.context is written for every build): the build's
   * default instead, the same choice the Model makes (inference.context in the shared vectors) */
  char fallback[32] = "";
  if (!rc && o.contexts && !o.only_role) {
    int any = 0;
    for (int i = 0; !any && i < files->n; ++i) {
      const TfJson *f = &files->kids[i];
      const char *p = tf_json_str(tf_json_get(f, "path")), *cx = tf_json_str(tf_json_get(f, "context")), *slash = strchr(p, '/');
      if (format == 3) any = cx && strcmp(cx, "*") && ctx_wanted(cx, strlen(cx), o.contexts);
      else any = slash && ctx_wanted(p, (size_t)(slash - p), o.contexts);
    }
    if (!any) {
      const char *dc = tf_json_str(tf_json_get(m, "default_context"));
      snprintf(fallback, sizeof fallback, "%s", dc ? dc : "c16_l64");
      o.contexts = fallback;
    }
  }
  /* what is asked for, and how many bytes of it (as stored) */
  char *want = !rc ? (char *)calloc((size_t)files->n + 1, 1) : NULL;
  if (!rc && !want) rc = tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  long long missing = 0, room = 0;
  for (int i = 0; !rc && i < files->n; ++i) {
    const TfJson *f = &files->kids[i];
    const int w = format == 3 ? in_profile3(f, o.contexts, o.only_role) : in_profile2(tf_json_str(tf_json_get(f, "path")), o.contexts, o.only_role);
    if (w < 0) {
      rc = tf__fail(TF_ERR_MODEL_INVALID, "manifest.json: files[%d].platforms is neither \"*\" nor a list of platform names", i);
      break;
    }
    want[i] = (char)w;
    if (!w) continue;
    const long long size = (long long)tf_json_get(f, "size")->num;
    run.total += size;
    if (!entry_done(&o, f)) {
      const TfJson *d = tf_json_get(f, "decoded");
      const long long dsize = d ? (long long)tf_json_get(d, "size")->num : 0;
      missing += size;
      if (dsize > room) room = dsize; /* a gzip file and its decoded copy are on disk together for a moment */
    }
  }
  if (!rc && o.only_role && run.total == 0) rc = tf__fail(TF_ERR_MODEL_MISSING, "this build has no %s data", o.only_role);
  if (!rc && missing > 0) {
    /* SPEC 13.4: not enough space is its own error, before any byte is fetched */
    const long long avail = tf__free_bytes(o.dest);
    if (avail >= 0 && avail < missing + room) rc = tf__fail_storage_full(missing + room, avail);
  }
  if (!rc && o.progress) o.progress(o.user, 0, run.total);
  for (int i = 0; !rc && i < files->n; ++i)
    if (want[i]) rc = fetch_entry(&run, &files->kids[i]);
  free(want);
  if (!rc) {
    char *mp = tf_path_join(o.dest, "manifest.json");
    if (!mp || tf_write_file(mp, r.body, r.body_n)) rc = tf__fail(TF_ERR_STORAGE_FAILED, "cannot write manifest.json in %s", o.dest);
    free(mp);
    if (!rc && o.progress) o.progress(o.user, run.total, run.total);
  }
  tf_json_free(m);
  tf_http_response_free(&r);
  tf__http_conn_close(run.conn);
  return rc;
}
