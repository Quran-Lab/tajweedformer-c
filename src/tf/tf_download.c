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
 * SPEC 2a: files come from the build's folder (/models/{repo}/{build}/{folder}/{path}, immutable); the manifest must
 * name its version in the signed text, the version must be the one the signed config named, and the manifest's
 * SHA-256 must start with the build the config named. SDK 4.0 (18.4.5): the config names no base; every file after the
 * lease comes only from the model folder the lease answer named. An answer without one (every answer carries it)
 * downloads nothing: an invalid response (as the Apple SDK, 018935d).
 *
 * SDK 4.0.1 (ENDPOINTS_40.md 28.4): the folder is a PATH, asked of the `models` hosts in their failover order
 * (tf_endpoints.c), manifest and files alike; there is no fixed manifest host (4.0.0 read the manifest from
 * cdn.quranlab.ai whatever the lease said). The manifest comes from the folder of the lease the install holds for the
 * build, else from the folder qlab-api names in every lease answer for it (MODEL_BASE's path, the repo, the build, the
 * config's folder); the files from the lease answer's folder. A lease base whose host is not one of the role's is asked
 * first. A file's resume after an unreachable failure (16.2's five, with their pauses) goes to the next host, cycling,
 * with Range and no If-Range: the validator is kept with the host that gave it and sent to that host only. Size and
 * SHA-256 decide, whatever host served the bytes; the next file starts at the host that answered last.
 *
 * SDK 4.0 (PROTECTED_MODEL.md 18.2): every model file of an encrypted build is a qle1 container. Its CDN form is
 * downloaded as it is served (<dest>/cdn/<decoded path>.qle), checked against the container's signed size and
 * SHA-256 (16.1), and handed to the Core (o->install), which turns it into the device form (<dest>/dev/<decoded
 * path>.qld) and checks the plaintext's SHA-256 inside; the CDN form is deleted after. No plaintext of a model file
 * is ever written. The manifest's acceptance is followed by o->after_manifest (the lease, which the install needs).
 * The build's layout file is installed before the files whose install mark it keys. Plain entries (the calibration,
 * the sample clips) are public data and stored as in 3.x.
 *
 * 4.0.1 (perf_plan row 5): the files of one call download on up to tf__download_lanes (4) lanes at once, each on its
 * own kept connection, the largest first, so the weights start at once and the small files fill the other lanes.
 * Each file is hashed as its bytes are written (a resumed file hashes the bytes it already had first), so no file is
 * read again after its last byte. With a lease hook the lanes start before it, from the build's folder the manifest
 * came from (through the models hosts), while the hook makes the install key (a TPM key on Windows: 1.5 s in Stage B),
 * attests and asks the lease on the caller's thread; nothing is installed before the lease is in, and the files are
 * used only when the lease answer names that same folder (its path: the host does not matter, 28.4). Another folder: what the lanes fetched is dropped and the
 * files come from the lease answer's folder. No lease (an error, or an answer without a folder): every file this
 * call fetched is removed before the error goes on. The Core's installs stay on the caller's thread, one at a time,
 * the layout first; the progress callback is called on the caller's thread. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"
#include "tf_thread.h"

int tf_gunzip_file(const char *in_path, const char *out_path);

/* 24.3: a model file that cannot be written or moved into place is the download's storage failure */
static int on_storage(int rc) {
  tf__cause_now(rc, "download", "storage", 0, NULL, NULL);
  return rc;
}

int tf__download_stall_ms = 30000;
int tf__download_pause_ms = 1000;
int tf__download_lanes = 4;
int tf__download_early = 1;
int tf__download_rereads; /* tests: files read again after their last byte (a transport that gave no on_data) */

TfDownload tf__download_defaults(void) {
  TfDownload o;
  memset(&o, 0, sizeof o);
  o.repo = "Quran-Lab/tajweedformer-v4";
  o.folder = "sdk/v4.0";
  return o;
}

/* 4.0.1: what the lanes of one call share, under `lock`: the progress, the caller's counters, the call's stop */
typedef struct {
  TfMutex lock;
  TfCond cv;             /* a lane finished a file, failed or ended; bytes came */
  const TfDownload *o;   /* the caller's options: fetched and resumed are written under the lock only */
  long long done, total; /* bytes as stored, over the files asked for */
  volatile int stop;     /* every lane's cancel flag: tf__http_cancel(&stop) ends the call's requests in flight */
} Shared;
typedef struct {
  const TfDownload *o;   /* this lane's options: the caller's, with the call's stop as the cancel flag */
  Shared *sh;
  TfHttpConn *conn;      /* 3.1.3: one kept connection to the model host per lane */
  char base[512];        /* where files are read: the lease answer's model folder (ends in '/'), or, before the
                            lease, the build's folder the manifest came from (4.0.1) */
  long long counted;     /* the bytes of the file in hand counted in sh->done */
  int touched;           /* this call wrote bytes of the file in hand */
  /* 4.0.1 (28.4): the models hosts, the folder's path every request asks for, and a base's host that is not one of
   * the role's (asked first; first_origin: a test's stand-in base, reached there) */
  TfEndpoints *ep;
  char path[512], first[TF_EP_HOST], first_origin[300];
} Run;
/* one request for a file: what the transport's callbacks need */
typedef struct {
  Run *run;
  const char *host;       /* 4.0.1: the host this request goes to (the validator of a file is kept with its host) */
  int fallback;           /* 4.0.1: this file has been asked of more than one host (24.2 fallback) */
  const char *part;       /* the partial file the body goes to; NULL for the manifest */
  const char *whole;      /* where a whole file sent for a range goes, beside the partial file */
  long long from;         /* the request's first byte */
  long long size;         /* the file's size, from the manifest */
  char etag[80];          /* the validator of a whole file that came for a range, until it is in place */
  TfSha256 sha;           /* 4.0.1: the file's bytes from its first, as they are written */
  long long hashed;       /* how many went into sha */
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
static int in_profile3(const TfJson *f, const char *contexts, const char *only_role, int want_recited) {
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
  if (role && !strcmp(role, "recited")) return want_recited && ctx_ok; /* 17.1, 17.5: optional in the manifest */
  if (!need || strcmp(need, "required")) return 0;
  return ctx_ok;
}

static int file_ok(const char *full, long long size, const char *sha_hex) {
  uint8_t want[32], got[32];
  long long n = 0;
  return !tf_hex_decode(sha_hex, want, 32) && !tf_sha256_file(full, got, &n) && n == size && !memcmp(got, want, 32);
}

/* {prefix}/{repo}/{revision}/{folder}/: the build's folder as qlab-api names it in every lease answer (MODEL_BASE's
 * path, /models). 4.0.1: a path, asked of the models hosts (28.4); the legacy /hf/ shape of a bare host is gone with
 * the fixed host. Returns 0, or -1 when it does not fit in n bytes. */
int tf__model_folder(char *out, size_t n, const char *prefix, const char *repo, const char *revision, const char *folder) {
  size_t pl = strlen(prefix);
  while (pl && prefix[pl - 1] == '/') pl--;
  const int w = snprintf(out, n, "%.*s/%s/%s/%s/", (int)pl, prefix, repo, revision, folder);
  return w < 0 || (size_t)w >= n ? -1 : 0;
}
/* the folder a base names: its path, and its host when that is not one of the role's (asked first); a path alone is
 * the folder on the models hosts. A base that is no base of 28.4 (a test's stand-in, tf__lease_test_base) is reached
 * at its own origin. 0 when it names none. */
static int folder_of(Run *run, const char *base) {
  char host[TF_EP_HOST], path[512];
  run->first[0] = run->first_origin[0] = 0;
  if (base && base[0] == '/' && strlen(base) < sizeof run->path && base[strlen(base) - 1] == '/' && !strstr(base, "..")) {
    snprintf(run->path, sizeof run->path, "%s", base); /* a folder's path alone: the models hosts in their order */
    return 1;
  }
  if (tf__ep_base_parts(base, host, path, sizeof path)) {
    snprintf(run->path, sizeof run->path, "%s", path);
    snprintf(run->first, sizeof run->first, "%s", host);
    return 1;
  }
  const char *a = base && tf__lease_test_base ? strstr(base, "://") : NULL;
  const char *slash = a ? strchr(a + 3, '/') : NULL;
  if (!slash || strlen(slash) >= sizeof run->path || (size_t)(slash - base) >= sizeof run->first_origin) return 0;
  snprintf(run->first_origin, sizeof run->first_origin, "%.*s", (int)(slash - base), base);
  snprintf(run->first, sizeof run->first, "%.*s", (int)(slash - a - 3), a + 3);
  snprintf(run->path, sizeof run->path, "%s", slash);
  return 1;
}
static void origin_of(const Run *run, const char *host, char *out, size_t n) {
  if (run->first_origin[0] && !strcmp(host, run->first)) snprintf(out, n, "%s", run->first_origin);
  else tf__ep_origin(run->ep, TF_EP_MODELS, host, out, n);
}

/* 4.0.1 (28.4): two bases name the same folder when their paths are the same, whatever host each names */
static int same_folder(const char *a, const char *b) {
  Run x, y;
  memset(&x, 0, sizeof x);
  memset(&y, 0, sizeof y);
  return folder_of(&x, a) && folder_of(&y, b) && !strcmp(x.path, y.path);
}

/* the file in hand counts `v` bytes in the progress (bytes of the files asked for that are here) */
static void set_counted(Run *run, long long v) {
  if (v == run->counted) return;
  Shared *sh = run->sh;
  tf_mutex_lock(&sh->lock);
  sh->done += v - run->counted;
  tf_cond_signal(&sh->cv);
  tf_mutex_unlock(&sh->lock);
  run->counted = v;
}
/* bytes that came over the network in this call (the telemetry's downloads entry counts them) */
static void net_bytes(void *user, long long n) {
  Run *run = ((Xfer *)user)->run;
  Shared *sh = run->sh;
  run->touched = 1;
  tf_mutex_lock(&sh->lock);
  if (sh->o->fetched) *sh->o->fetched += n;
  sh->done += n;
  tf_cond_signal(&sh->cv);
  tf_mutex_unlock(&sh->lock);
  run->counted += n;
}
/* 4.0.1: the same bytes into the file's hash, in the order they were written */
static void on_data(void *user, const void *p, size_t n) {
  Xfer *x = (Xfer *)user;
  tf_sha256_update(&x->sha, p, n);
  x->hashed += (long long)n;
}
/* the first n bytes of a partial file into the hash (a resume hashes what it already has once); -1 when it cannot */
static int hash_prefix(const char *path, long long n, TfSha256 *c) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  unsigned char buf[1 << 16];
  long long left = n;
  while (left > 0) {
    const size_t want = left > (long long)sizeof buf ? sizeof buf : (size_t)left;
    const size_t k = fread(buf, 1, want, f);
    if (!k) break;
    tf_sha256_update(c, buf, k);
    left -= (long long)k;
  }
  fclose(f);
  return left ? -1 : 0;
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
/* 4.0.1 (28.4): an ETag is one store's, and each model host has its own store. The validator is kept with the host
 * that gave it (a second line) and sent to that host only; a resume on another host goes with the Range alone. A
 * validator an earlier SDK kept has no host and is sent to none. */
static void etag_write(const char *part, const char *etag, const char *host) {
  char *p = etag_path(part);
  if (!p) return;
  if (etag && etag[0] == '"' && host && *host) {
    char text[80 + TF_EP_HOST + 2];
    const int n = snprintf(text, sizeof text, "%s\n%s", etag, host);
    if (n > 0 && (size_t)n < sizeof text) tf_write_file(p, text, (size_t)n);
    else remove(p);
  } else {
    remove(p);
  }
  free(p);
}
static void etag_read(const char *part, char out[80], char host[TF_EP_HOST]) {
  char *p = etag_path(part), *text = NULL;
  size_t n = 0;
  out[0] = host[0] = 0;
  if (p && !tf_read_file(p, &text, &n) && text) {
    const char *nl = (const char *)memchr(text, '\n', n);
    size_t en = nl ? (size_t)(nl - text) : n;
    while (en && (text[en - 1] == '\r' || text[en - 1] == ' ')) en--;
    if (en >= 2 && en < 80 && text[0] == '"' && nl) {
      memcpy(out, text, en);
      out[en] = 0;
      size_t hn = n - (size_t)(nl + 1 - text);
      while (hn && (nl[hn] == '\n' || nl[hn] == '\r' || nl[hn] == ' ')) hn--;
      if (hn && hn < TF_EP_HOST) snprintf(host, TF_EP_HOST, "%.*s", (int)hn, nl + 1);
      if (!host[0]) out[0] = 0;
    }
  }
  free(text);
  free(p);
}
/* the status and the headers of a file request are in, the file is open and no byte of the body is written */
/* the status and the headers of a file request are in, the file is open and no byte of the body is written */
static void on_head(void *user, const TfHttpResponse *r) {
  Xfer *x = (Xfer *)user;
  if (!x->part || r->status != 200) return; /* a 206 continues the partial file: its validator stays */
  if (x->from > 0) {
    /* a whole file for a range request: it is written beside the partial file, which keeps its bytes and
     * its validator until all of the whole file is here (fetch_raw). The count and the hash start again for it */
    snprintf(x->etag, sizeof x->etag, "%s", r->etag);
    set_counted(x->run, 0);
    tf_sha256_init(&x->sha);
    x->hashed = 0;
    return;
  }
  /* a 200 to a request without a range starts the partial file, with this answer's validator and its host */
  etag_write(x->part, r->etag, x->host);
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

/* 28.3: a manifest is taken on a host only when it is the signed manifest of the build asked for (the same checks as
 * below, which run again on the answer the request ends on) */
static int manifest_taken(void *user, const TfHttpResponse *r) {
  const Run *run = (const Run *)user;
  const TfDownload *o = run->o;
  if (r->status != 200 || !r->body) return 0;
  TfErrSave *keep = tf__error_save();
  TfJson *m = NULL;
  const int rc = tf__manifest_accept(r->body, r->body_n, o->check_build ? o->revision : NULL, o->want_version,
                                     o->trusted_key_hex ? o->trusted_key_hex : TF_QURANLAB_KEY_HEX, 0, &m, NULL, NULL, NULL);
  tf_json_free(m);
  tf__cause_clear();
  tf__error_restore(keep, 1);
  return rc == 0;
}
/* One model request: the manifest (host NULL: the models hosts in their order, 28.3) or a file's request to one host
 * (fetch_raw picks it, 28.4). `net` is set to 1 when it failed on the network (no answer, a connection that ended, a
 * stall): the failures a file request is resumed after. */
static int get(Run *run, const char *host, const char *rel, Xfer *x, const char *if_range, TfHttpResponse *r, int *net) {
  const TfDownload *o = run->o;
  char url[4096], key[512], path[1600], origin[320];
  *net = 0;
  if (snprintf(path, sizeof path, "%s%s", run->path, rel) >= (int)sizeof path)
    return tf__fail(TF_ERR_INVALID_ARGUMENT, "the model URL of %s is too long", rel);
  if (host) {
    origin_of(run, host, origin, sizeof origin);
    snprintf(url, sizeof url, "%s%s", origin, path);
  }
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
    q.url = host ? url : NULL;
    q.headers = hdr;
    q.download_file = x->part;
    q.download_whole = x->whole;
    q.download_size = x->part ? x->size : 0;
    q.accept = accept_head;
    q.range_from = x->from;
    q.if_range = if_range;
    /* 3.1.3: no byte for 30 s ends the request; there is no limit on the whole transfer (it was one hour) */
    q.stall_ms = tf__download_stall_ms;
    q.head_ms = TF_EP_HEAD_MS; /* 4.0.1 (28.3 item 6): the first byte within 10 s, then the next host */
    q.conn = run->conn;
    q.cancel = o->cancel;
    q.on_head = on_head;
    q.bytes_user = x;
    if (x->part) {
      q.on_bytes = net_bytes;
      q.on_data = on_data;
    }
    int hrc;
    if (host) {
      tf__ep_note(TF_EP_MODELS, x->fallback); /* the token request above was the api role's */
      hrc = tf_http(&q, r);
    } else {
      TfEpReq er;
      memset(&er, 0, sizeof er);
      er.role = TF_EP_MODELS;
      er.path = path;
      er.first = run->first[0] && !run->first_origin[0] ? run->first : NULL;
      er.stage = "manifest";
      er.taken = manifest_taken;
      er.user = run;
      hrc = tf__ep_http(run->ep, &er, &q, r, NULL);
    }
    if (hrc) {
      const char *stage = strcmp(rel, "manifest.json") ? "download" : "manifest";
      /* a failed write is the device's, not the network's; the .part stays either way */
      if (tf__error_code() == TF__ERR_IO) {
        tf__cause(stage, "storage", 0, NULL, NULL);
        return tf__error_as(TF_ERR_STORAGE_FAILED);
      }
      tf__cause_transport(stage, r->status); /* no answer, a stall, a cancel, or not the answer asked for */
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
  /* 24.3: the answer is the cause (redirect, rate_limited, refused with its error, http) */
  const char *stage = strcmp(rel, "manifest.json") ? "download" : "manifest";
  tf__cause_answer(stage, r->status, r->body, r->body_n);
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
  if (r->status == 401 && !strcmp(code, "invalid_token")) {
    /* the token renewed once and still not taken: a failed download, as 15.4 says; the token's own answer, never a
       refusal (ERRORS_40.md 24.3, model_gate.json: no error of its own), so the report's reason is http */
    const int rc = tf__fail(TF_ERR_DOWNLOAD_FAILED, "Quran Lab did not take this install's token for %s", rel);
    tf__cause_now(rc, stage, "http", 401, NULL, "invalid_token");
    return rc;
  }
  if (r->status == 403 && strstr(b, "sdk_too_old")) return tf__fail_sdk_too_old(NULL);
  /* a 401 or 403 without a reason: a key the CDN does not know, a refusal (the answer named nothing: http) */
  if (r->status == 401 || r->status == 403) {
    const int rc = r->status == 401 ? tf__fail(TF_ERR_INVALID_KEY, NULL) : tf__fail(TF_ERR_SERVER_REJECTED, "HTTP %d for %s", r->status, rel);
    tf__cause_now(rc, stage, code[0] ? "refused" : "http", r->status, NULL, code);
    return rc;
  }
  return tf__fail(TF_ERR_DOWNLOAD_FAILED, "HTTP %d for %s", r->status, rel);
}

/* The pause before the `nth` resume (0 for the first). 1 when the download was cancelled, at once. */
void (*tf__download_on_pause)(long long ms);
static int pause_before_resume(const TfDownload *o, int nth) {
  long long ms = (long long)tf__download_pause_ms << nth;
  if (tf__download_on_pause) {
    tf__download_on_pause(ms);
    ms = 0;
  }
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
  run->counted = 0;
  run->touched = 0;
  if (file_ok(to, size, sha)) {
    set_counted(run, size);
    return 0;
  }
  uint8_t want[32];
  const int have_want = !tf_hex_decode(sha, want, 32);
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
  /* 4.0.1 (28.4): the hosts this file asks, P first; each failure a resume is for moves to the next, cycling; the
   * first host's failure is what a failover reports */
  TfEpOrder ord;
  tf__ep_order(run->ep, TF_EP_MODELS, run->first[0] ? run->first : NULL, &ord);
  int k = 0, answered = 0, first_status = 0, installed = 0;
  char first_reason[20] = "";
  const char *host = ord.host[0];
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
    if (from == 0) etag_write(part, NULL, NULL); /* no bytes, so nothing to validate */
    set_counted(run, from); /* what an earlier request already fetched */
    /* resumed, as the telemetry counts it: this call went on from a partial file an earlier call left. A resume
     * inside the call is not counted (it would make every download that rode out one break a resumed one) */
    if (from > 0 && from < size && resumes == 0 && run->sh->o->resumed) {
      tf_mutex_lock(&run->sh->lock);
      *run->sh->o->resumed = 1;
      tf_mutex_unlock(&run->sh->lock);
    }
    Xfer x;
    memset(&x, 0, sizeof x);
    x.run = run;
    host = ord.host[k % ord.n];
    x.host = host;
    /* 24.2: this file's requests are of the models role; more than one host asked from the second host on */
    x.fallback = (k + 1 < ord.n ? k + 1 : ord.n) > 1;
    x.part = part;
    x.whole = whole;
    x.from = from;
    x.size = size;
    /* 4.0.1: the hash of the partial file's bytes, read once; the rest goes into it as it is written */
    tf_sha256_init(&x.sha);
    if (from > 0) x.hashed = hash_prefix(part, from, &x.sha) ? -1 : from;
    if (from < size) {
      char tag[80] = "", tag_host[TF_EP_HOST] = "";
      if (from > 0) etag_read(part, tag, tag_host);
      if (strcmp(tag_host, host)) tag[0] = 0; /* the validator goes only to the host that gave it */
      TfHttpResponse r;
      int again = 0; /* 1: a failed attempt of the kind a resume is for */
      int st = 0;
      run->touched = 1; /* this call asked for bytes of the file: what comes of it is this call's */
      rc = get(run, host, rel, &x, tag[0] ? tag : NULL, &r, &again);
      if (rc) st = tf__http_failure(NULL, NULL) == TF__FAIL_ANSWER ? r.status : 0;
      if (!rc) {
        st = r.status;
        if (st == 200 && from > 0 && file_size(whole) >= 0) {
          /* the whole file came for a range, beside the partial file: all of it, and it takes the partial
           * file's place with its own validator; less, and the partial file stays as it was */
          if (file_size(whole) == size) {
            remove(part);
            if (rename(whole, part)) rc = on_storage(tf__fail(TF_ERR_STORAGE_FAILED, "cannot put %s in the place of its partial file", rel));
            else etag_write(part, x.etag, host);
          } else {
            again = 1;
            tf__cause("download", "network", 0, NULL, NULL); /* a connection that ended */
            rc = tf__fail(TF_ERR_DOWNLOAD_FAILED, "the answer for %s ended early", rel);
          }
        } else if (st == 200 || st == 206) {
          /* an answer that ended before the file's last byte, or brought none, without an error of the
           * transport, is a connection that ended */
          if (file_size(part) < size) {
            again = 1;
            tf__cause("download", "network", 0, NULL, NULL);
            rc = tf__fail(TF_ERR_DOWNLOAD_FAILED, "the answer for %s ended early", rel);
          }
        } else if (st >= 200 && st < 300) {
          /* a 204 or any other 2xx is not an answer to a file request */
          again = 1;
          tf__cause("download", "invalid_answer", st, NULL, NULL);
          rc = tf__fail(TF_ERR_DOWNLOAD_FAILED, "HTTP %d is not an answer to the request for %s", st, rel);
        } else {
          /* 4.0.1 (28.3, 28.4): an answer that is the host unreachable (a 5xx, a redirect, a 4xx that is not the
           * API's) is resumed at the next host; a refusal and a 429 are asked once and on no other host. (4.0.0
           * resumed after a 5xx only, on its one host, and never after network_blocked.) */
          const int cls = tf__ep_class(NULL, 0, &r);
          again = cls == TF_EP_UNREACHABLE;
          answered = !again;
          /* no answer takes the bytes already here, but a 416: the server has no such range, so they are past
           * its file */
          if (st == 416) {
            remove(part);
            etag_write(part, NULL, NULL);
          }
          rc = http_error(run, rel, &r);
        }
        tf_http_response_free(&r);
      }
      if (rc) {
        remove(whole); /* part of a whole file that did not all come */
        if (again && k == 0) { /* the first host's failure (28.7) */
          snprintf(first_reason, sizeof first_reason, "%s", *tf__cause_reason() ? tf__cause_reason() : "network");
          first_status = st;
        }
        /* the failure after the last resume, and every failure a resume is not for, is the call's error */
        if (!again || resumes == TF_DOWNLOAD_RESUMES || pause_before_resume(run->o, resumes)) break;
        resumes++;
        k++; /* 28.4: the resume goes to the next host */
        continue;
      }
    }
    /* every byte of the file went into the hash as it was written (4.0.1): its size and digest decide, with no second
     * read; else (a test's transport that writes the file itself, a prefix that could not be read) the file is read */
    int good;
    if (x.hashed == size && have_want && file_size(part) == size) {
      uint8_t got[32];
      tf_sha256_final(&x.sha, got);
      good = !memcmp(got, want, 32);
    } else {
      good = file_ok(part, size, sha);
      tf_mutex_lock(&run->sh->lock);
      tf__download_rereads++;
      tf_mutex_unlock(&run->sh->lock);
    }
    if (good) {
      remove(to);
      rc = rename(part, to) ? on_storage(tf__fail(TF_ERR_STORAGE_FAILED, "cannot move %s into place", rel)) : 0;
      etag_write(part, NULL, NULL);
      set_counted(run, size);
      installed = !rc;
      break;
    }
    remove(part); /* the full size and not the file: it goes with its validator, and the file is fetched once more */
    etag_write(part, NULL, NULL);
    tf__cause("download", "hash", 0, NULL, NULL);
    rc = tf__fail(TF_ERR_CHECKSUM_MISMATCH, "%s", rel);
    if (++mismatches == 2) break;
  }
  /* 28.3 item 5, 28.4: the host the file was installed from, or refused or slowed down on, is P (the next file starts
   * there); when every host asked failed, P stays. A file another host gave after the first was unreachable is the
   * silent failover report (28.7) */
  if (installed || answered) {
    tf__ep_settle(run->ep, TF_EP_MODELS, host, ord.probe);
    if (k > 0 && first_reason[0]) {
      /* 28.7: lastFailover however the file ended, back on its first host too; the silent report only when another
       * host answered */
      tf__ep_failed_over(run->ep, TF_EP_MODELS, ord.host[0], host, first_reason, first_status);
      if (strcmp(host, ord.host[0])) {
        tf__ep_note(TF_EP_MODELS, 1);
        tf__rep_silent("download", "failover", first_status, NULL, first_reason);
      }
    }
  } else if (first_reason[0]) {
    /* 28.7: every file whose first host was unreachable, a role of one host and an offline device included */
    tf__ep_failed_over(run->ep, TF_EP_MODELS, ord.host[0], NULL, first_reason, first_status);
  }
  if (rc) tf__cause_retries(resumes + (mismatches > 1 ? 1 : 0)); /* 24.2 retries: the resumes of 16.2 */
  free(part);
  free(whole);
  return rc;
}

int tf__download_file_test(TfEndpoints *ep, const char *path, const char *rel, long long size, const char *sha_hex, const char *to) {
  tf__clear_error();
  TfDownload o = tf__download_defaults();
  o.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  Shared sh;
  memset(&sh, 0, sizeof sh);
  tf_mutex_init(&sh.lock);
  tf_cond_init(&sh.cv);
  sh.o = &o;
  sh.total = size;
  Run run;
  memset(&run, 0, sizeof run);
  run.o = &o;
  run.sh = &sh;
  run.ep = ep;
  snprintf(run.path, sizeof run.path, "%s", path);
  const int rc = fetch_raw(&run, rel, size, sha_hex, to);
  tf_cond_destroy(&sh.cv);
  tf_mutex_destroy(&sh.lock);
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

/* One entry fetched and checked on a lane (4.0.1: an encrypted entry's CDN form is only downloaded here; the Core
 * installs it on the caller's thread, after the lease, install_entry) */
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
  if (!strcmp(enc, "qle1")) {
    const TfJson *d = tf_json_get(f, "decoded");
    const char *dp = tf_json_str(tf_json_get(d, "path"));
    char rel[1100];
    snprintf(rel, sizeof rel, "cdn/%s.qle", dp);
    char *cdn = tf_path_join(o->dest, rel);
    if (!cdn) return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
    mkparent(cdn);
    const int rc = fetch_raw(run, p, size, sha, cdn);
    free(cdn);
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
  else if (file_ok(dfull, dsize, dsha)) {
    run->counted = 0;
    set_counted(run, size);
  }
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
        tf__cause("download", "hash", 0, NULL, NULL);
        rc = tf__fail(TF_ERR_CHECKSUM_MISMATCH, "%s", dp);
      } else {
        remove(dfull);
        rc = rename(tmp, dfull) ? on_storage(tf__fail(TF_ERR_STORAGE_FAILED, "cannot move %s into place", dp)) : 0;
        remove(gz);
      }
      free(tmp);
    }
  }
  free(dfull);
  free(gz);
  return rc;
}

/* 1 when the entry's file is already in place and verified (a qle1 entry: its device form, which only the Core
 * writes, is there; the Core checks its tags at open) */
static int entry_done(const TfDownload *o, const TfJson *f) {
  const char *enc = tf_json_str(tf_json_get(f, "encoding"));
  if (enc && !strcmp(enc, "qle1")) {
    char rel[1100];
    snprintf(rel, sizeof rel, "dev/%s.qld", tf_json_str(tf_json_get(tf_json_get(f, "decoded"), "path")));
    char *dev = tf_path_join(o->dest, rel);
    const int here = dev && tf_file_exists(dev);
    free(dev);
    return here;
  }
  const TfJson *loc = enc && !strcmp(enc, "gzip") ? tf_json_get(f, "decoded") : f;
  char *full = tf_path_join(o->dest, tf_json_str(tf_json_get(loc, "path")));
  const int ok = full && file_ok(full, (long long)tf_json_get(loc, "size")->num, tf_json_str(tf_json_get(loc, "sha256")));
  free(full);
  return ok;
}

/* ------------------------------------------------------------------ 4.0.1: the lanes */
#define TF_MAX_LANES 8
enum { JOB_WAIT = 0, JOB_RUN = 1, JOB_DONE = 2, JOB_FAIL = 3 };
typedef struct {
  const TfJson *f;   /* the manifest entry */
  long long size;    /* as stored */
  int state;         /* JOB_WAIT, JOB_RUN, JOB_DONE (fetched and checked: the CDN form of an encrypted entry), JOB_FAIL */
  int touched;       /* this call wrote bytes of it */
  int rc;            /* its failure, and the error its lane had */
  TfErrState err;
} Job;
typedef struct {
  Shared sh;
  TfDownload lo;     /* the lanes' options: the caller's, with the call's stop as the cancel flag */
  Job *jobs;         /* the files to fetch, largest first */
  int n;
  long long done0;   /* the bytes of the files that were in place before any lane ran */
  char base[512];    /* where the lanes read the files: a folder (a lease's base, or a path) */
  TfEndpoints *ep;   /* 4.0.1: the models hosts every lane asks */
  int failed;        /* a job failed: no lane takes another, and the first failure is the call's */
  int first_fail;
  int live;          /* lanes running */
  TfThread th[TF_MAX_LANES];
  int nth;
} Pool;

static void lane_main(void *arg) {
  Pool *pl = (Pool *)arg;
  Run run;
  memset(&run, 0, sizeof run);
  run.o = &pl->lo;
  run.sh = &pl->sh;
  run.ep = pl->ep;
  run.conn = tf__http_conn_open();
  tf_mutex_lock(&pl->sh.lock);
  snprintf(run.base, sizeof run.base, "%s", pl->base);
  tf_mutex_unlock(&pl->sh.lock);
  folder_of(&run, run.base); /* 4.0.1 (28.4): its path, through the models hosts (the base's own host first) */
  for (;;) {
    Job *j = NULL;
    tf_mutex_lock(&pl->sh.lock);
    if (!pl->failed && !pl->sh.stop)
      for (int i = 0; i < pl->n && !j; ++i)
        if (pl->jobs[i].state == JOB_WAIT) {
          j = &pl->jobs[i];
          j->state = JOB_RUN;
        }
    tf_mutex_unlock(&pl->sh.lock);
    if (!j) break;
    tf__clear_error();
    const int rc = fetch_entry(&run, j->f);
    int stop_others = 0;
    tf_mutex_lock(&pl->sh.lock);
    j->touched |= run.touched;
    if (rc) {
      j->state = JOB_FAIL;
      j->rc = rc;
      tf__err_save(&j->err);
      if (!pl->failed) {
        pl->failed = 1;
        pl->first_fail = (int)(j - pl->jobs);
        stop_others = 1;
      }
    } else {
      j->state = JOB_DONE;
    }
    tf_cond_signal(&pl->sh.cv);
    tf_mutex_unlock(&pl->sh.lock);
    /* the first failure ends the call: the other lanes' requests in flight end too, their partial files kept */
    if (stop_others) tf__http_cancel(&pl->sh.stop);
  }
  tf__http_conn_close(run.conn);
  tf_mutex_lock(&pl->sh.lock);
  pl->live--;
  tf_cond_signal(&pl->sh.cv);
  tf_mutex_unlock(&pl->sh.lock);
}

/* the lanes start on `base` (at most tf__download_lanes, one per job at most); the count started, 0 when no thread
 * could start (the caller then runs one lane itself) */
static int pool_start(Pool *pl, const char *base) {
  pl->sh.stop = 0;
  pl->failed = 0;
  pl->first_fail = -1;
  snprintf(pl->base, sizeof pl->base, "%s", base ? base : "");
  int want = tf__download_lanes < 1 ? 1 : tf__download_lanes > TF_MAX_LANES ? TF_MAX_LANES : tf__download_lanes;
  if (want > pl->n) want = pl->n;
  tf_mutex_lock(&pl->sh.lock);
  for (pl->nth = 0; pl->nth < want; ++pl->nth) {
    if (tf_thread_start(&pl->th[pl->nth], lane_main, pl)) break;
    pl->live++;
  }
  tf_mutex_unlock(&pl->sh.lock);
  return pl->nth;
}
static void pool_join(Pool *pl) {
  for (int i = 0; i < pl->nth; ++i) tf_thread_join(pl->th[i]);
  pl->nth = 0;
}

/* everything one entry's fetch may have left in dest: the file, its partial file, validator and whole file beside it
 * (an encrypted entry: its CDN form; a gzip entry: the stored file and the decoded one) */
static void remove_with_parts(const char *full) {
  if (!full) return;
  static const char *const sfx[] = {"", ".part", ".part.etag", ".part.new", ".inflate"};
  for (size_t i = 0; i < sizeof sfx / sizeof sfx[0]; ++i) {
    const size_t n = strlen(full) + strlen(sfx[i]) + 1;
    char *p = (char *)malloc(n);
    if (!p) continue;
    snprintf(p, n, "%s%s", full, sfx[i]);
    remove(p);
    free(p);
  }
}
static void remove_entry(const TfDownload *o, const TfJson *f) {
  const char *p = tf_json_str(tf_json_get(f, "path")), *enc = tf_json_str(tf_json_get(f, "encoding"));
  const char *dp = tf_json_str(tf_json_get(tf_json_get(f, "decoded"), "path"));
  if (enc && !strcmp(enc, "qle1") && dp) {
    char rel[1100];
    snprintf(rel, sizeof rel, "cdn/%s.qle", dp);
    char *cdn = tf_path_join(o->dest, rel);
    remove_with_parts(cdn);
    free(cdn);
    return;
  }
  char *full = tf_path_join(o->dest, p);
  remove_with_parts(full);
  free(full);
  if (enc && !strcmp(enc, "gzip") && dp) {
    char *d = tf_path_join(o->dest, dp);
    remove_with_parts(d);
    free(d);
  }
}
/* the lanes stopped, and every file this call fetched removed (a lease that did not come, or named another folder):
 * the jobs wait again from nothing */
static void pool_drop(Pool *pl, const TfDownload *o) {
  tf__http_cancel(&pl->sh.stop);
  pool_join(pl);
  for (int i = 0; i < pl->n; ++i) {
    if (pl->jobs[i].touched) remove_entry(o, pl->jobs[i].f);
    pl->jobs[i].state = JOB_WAIT;
    pl->jobs[i].touched = 0;
    pl->jobs[i].rc = 0;
  }
  tf_mutex_lock(&pl->sh.lock);
  pl->sh.done = pl->done0;
  pl->failed = 0;
  pl->first_fail = -1;
  pl->live = 0;
  pl->sh.stop = 0;
  tf_mutex_unlock(&pl->sh.lock);
}
/* the progress on the caller's thread: the bytes of the files asked for that are here */
static void report_progress(Pool *pl, long long *last) {
  const TfDownload *o = pl->sh.o;
  if (!o->progress) return;
  tf_mutex_lock(&pl->sh.lock);
  const long long d = pl->sh.done, t = pl->sh.total;
  tf_mutex_unlock(&pl->sh.lock);
  if (d == *last) return;
  *last = d;
  o->progress(o->user, d > t ? t : d < 0 ? 0 : d, t);
}
/* Waits until job j is fetched (0), the lanes failed (1: the first failure is the call's), or they all ended without
 * it (2: the call was cancelled). The progress is reported while waiting, and a cancel of the caller (its Client
 * going) ends the lanes. */
static int wait_job(Pool *pl, Job *j, long long *last) {
  const TfDownload *o = pl->sh.o;
  for (;;) {
    tf_mutex_lock(&pl->sh.lock);
    if (j->state != JOB_DONE && !pl->failed && pl->live > 0) tf_cond_wait_ms(&pl->sh.cv, &pl->sh.lock, 100);
    const int st = j->state, failed = pl->failed, live = pl->live;
    tf_mutex_unlock(&pl->sh.lock);
    report_progress(pl, last);
    if (st == JOB_DONE) return 0;
    if (failed) return 1;
    if (live <= 0) return 2;
    if (o->cancel && *o->cancel && !pl->sh.stop) tf__http_cancel(&pl->sh.stop);
  }
}
/* the Core installs an encrypted entry's CDN form (on the caller's thread, after the lease); 0 for a plain entry */
static int install_entry(const TfDownload *o, const TfJson *f, const char *mbytes, size_t mn) {
  const char *enc = tf_json_str(tf_json_get(f, "encoding"));
  if (!enc || strcmp(enc, "qle1") || !o->install) return 0;
  const char *dp = tf_json_str(tf_json_get(tf_json_get(f, "decoded"), "path"));
  const int rc = o->install(o->hook_user, dp, mbytes, mn);
  /* a container the Core refused is fetched again next time; one that waits for a lease stays */
  if (!rc || rc == TF_ERR_CHECKSUM_MISMATCH || rc == TF_ERR_MODEL_INVALID) {
    char rel[1100];
    snprintf(rel, sizeof rel, "cdn/%s.qle", dp);
    char *cdn = tf_path_join(o->dest, rel);
    if (cdn) remove(cdn);
    free(cdn);
  }
  return rc;
}

int tf__download_model(const TfDownload *opt) {
  tf__clear_error();
  TfDownload o = opt ? *opt : tf__download_defaults();
  if (!o.repo) o.repo = "Quran-Lab/tajweedformer-v4";
  if (!o.folder) o.folder = "sdk/v4.0";
  if (!o.revision || !o.dest || !o.key) return tf__fail(TF_ERR_INVALID_ARGUMENT, "a key, a build and a destination folder are required");
  if (tf_mkdirs(o.dest)) return on_storage(tf__fail(TF_ERR_STORAGE_FAILED, "cannot create %s", o.dest));
  Pool pl;
  memset(&pl, 0, sizeof pl);
  tf_mutex_init(&pl.sh.lock);
  tf_cond_init(&pl.sh.cv);
  pl.sh.o = &o;
  pl.lo = o;
  pl.lo.cancel = &pl.sh.stop;
  pl.lo.progress = NULL;
  pl.first_fail = -1;
  Run run;
  memset(&run, 0, sizeof run);
  run.o = &o;
  run.sh = &pl.sh;
  /* 4.0.1 (28.4): the Client's models hosts (the downloader's own tests: a set of their own, cdn_url their one host)
   * and the build's folder: the held lease's for the build, else the one qlab-api names for it */
  TfEndpoints *own_ep = NULL;
  run.ep = pl.ep = o.client ? tf__client_endpoints(o.client) : (own_ep = tf__ep_new(NULL, o.cdn_url));
  char mfolder[600] = "";
  if (o.base_hint && folder_of(&run, o.base_hint)) snprintf(mfolder, sizeof mfolder, "%s", o.base_hint);
  else if (!tf__model_folder(mfolder, sizeof mfolder, tf__ep_prefix(run.ep), o.repo, o.revision, o.folder)) folder_of(&run, mfolder);
  else mfolder[0] = 0;
  if (!run.ep || !mfolder[0]) {
    tf__ep_free(own_ep);
    tf_cond_destroy(&pl.sh.cv);
    tf_mutex_destroy(&pl.sh.lock);
    return tf__fail(run.ep ? TF_ERR_INVALID_ARGUMENT : TF_ERR_ENGINE_FAILED, run.ep ? "the model folder of %s is too long" : "out of memory", o.revision);
  }
  /* 3.1.3: a kept connection for the manifest (each lane keeps its own for the files) */
  run.conn = tf__http_conn_open();
  TfHttpResponse r;
  /* The manifest is asked once (of the models hosts in their order): there is no partial file to go on from,
   * nothing is lost when the call ends here, and a start with a model on disk is not held up when no model host
   * answers. */
  Xfer mx;
  memset(&mx, 0, sizeof mx);
  mx.run = &run;
  int net = 0;
  int rc = get(&run, NULL, "manifest.json", &mx, NULL, &r, &net);
  if (!rc && r.status != 200) {
    rc = http_error(&run, "manifest.json", &r);
    tf_http_response_free(&r);
  }
  tf__http_conn_close(run.conn);
  run.conn = NULL;
  if (rc) {
    tf__ep_free(own_ep);
    tf_cond_destroy(&pl.sh.cv);
    tf_mutex_destroy(&pl.sh.lock);
    return rc;
  }
  /* the same acceptance checks, in the same order, as an open from disk (tf__manifest_accept) */
  TfJson *m = NULL;
  rc = tf__manifest_accept(r.body, r.body_n, o.check_build ? o.revision : NULL, o.want_version,
                           o.trusted_key_hex ? o.trusted_key_hex : TF_QURANLAB_KEY_HEX, 0, &m, NULL, NULL, NULL);
  if (rc) /* 24.3 manifest: its signature, its form, a build this SDK cannot run, not the build the config named */
    tf__cause_now(rc, "manifest", rc == TF_ERR_SIGNATURE_INVALID ? "signature" : rc == TF_ERR_MODEL_INCOMPATIBLE ? "incompatible" : rc == TF_ERR_CHECKSUM_MISMATCH ? "hash" : "parse",
                  200, NULL, NULL);
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
        (enc && (!strcmp(enc, "gzip") || !strcmp(enc, "qle1")) && (!dp || !tf_json_is_int(tf_json_get(d, "size")) || !tf_json_str(tf_json_get(d, "sha256")))))
      rc = tf__fail(TF_ERR_MODEL_INVALID, "manifest.json lists a path that is not inside the folder");
  }
  /* 4.0.1: the build's key id is the one the signed config names beside it (18.4.5), checked before any file is asked
   * for (the lease hook checks it too) */
  if (!rc && o.want_key_id) {
    const char *kid = tf_json_str(tf_json_get(tf_json_get(m, "encryption"), "key_id"));
    if (!kid || strcmp(kid, o.want_key_id))
      rc = tf__fail(TF_ERR_MODEL_INVALID, "the manifest names key id %s, the signed config named %s", kid ? kid : "none", o.want_key_id);
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
      pl.lo.contexts = fallback;
    }
  }
  /* what is asked for, and how many bytes of it (as stored); the files not in place yet are the jobs */
  char *want = !rc ? (char *)calloc((size_t)files->n + 1, 1) : NULL;
  int *jix = !rc ? (int *)malloc(((size_t)files->n + 1) * sizeof(int)) : NULL;
  pl.jobs = !rc ? (Job *)calloc((size_t)files->n + 1, sizeof(Job)) : NULL;
  if (!rc && (!want || !jix || !pl.jobs)) rc = tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  long long missing = 0, room = 0;
  for (int i = 0; !rc && i < files->n; ++i) {
    const TfJson *f = &files->kids[i];
    const int w = format == 3 ? in_profile3(f, o.contexts, o.only_role, o.want_recited) : in_profile2(tf_json_str(tf_json_get(f, "path")), o.contexts, o.only_role);
    if (w < 0) {
      rc = tf__fail(TF_ERR_MODEL_INVALID, "manifest.json: files[%d].platforms is neither \"*\" nor a list of platform names", i);
      break;
    }
    want[i] = (char)w;
    if (!w) continue;
    const long long size = (long long)tf_json_get(f, "size")->num;
    pl.sh.total += size;
    if (entry_done(&o, f)) {
      pl.done0 += size;
      continue;
    }
    const TfJson *d = tf_json_get(f, "decoded");
    const long long dsize = d ? (long long)tf_json_get(d, "size")->num : 0;
    missing += size;
    if (dsize > room) room = dsize; /* a gzip file and its decoded copy (a CDN form and its device form) are on disk together for a moment */
    pl.jobs[pl.n].f = f;
    pl.jobs[pl.n].size = size;
    pl.n++;
  }
  pl.sh.done = pl.done0;
  if (!rc && o.only_role && pl.sh.total == 0) rc = tf__fail(TF_ERR_MODEL_MISSING, "this build has no %s data", o.only_role);
  if (!rc && missing > 0) {
    /* SPEC 13.4: not enough space is its own error, before any byte is fetched */
    const long long avail = tf__free_bytes(o.dest);
    if (avail >= 0 && avail < missing + room) {
      rc = tf__fail_storage_full(missing + room, avail);
      tf__cause_now(rc, "download", "quota", 0, NULL, NULL);
    }
  }
  /* 4.0.1: the lanes take the largest file first (the weights start at once, the small files fill the other lanes): a
   * stable insertion sort, the manifest's order among equal sizes; jix maps a manifest entry to its job */
  if (!rc) {
    for (int a = 1; a < pl.n; ++a) {
      const Job t = pl.jobs[a];
      int b = a - 1;
      while (b >= 0 && pl.jobs[b].size < t.size) {
        pl.jobs[b + 1] = pl.jobs[b];
        b--;
      }
      pl.jobs[b + 1] = t;
    }
    for (int i = 0; i < files->n; ++i) jix[i] = -1;
    for (int k = 0; k < pl.n; ++k)
      for (int i = 0; i < files->n; ++i)
        if (&files->kids[i] == pl.jobs[k].f) jix[i] = k;
  }
  /* 4.0.1: with a lease hook, the lanes start before it from the build's folder the manifest came from (through the
   * models hosts) while the hook makes the install key, attests and asks the lease on this thread */
  char prebase[600] = "";
  int started = 0, inline_lane = 0;
  if (!rc && pl.n > 0 && o.after_manifest && tf__download_early) snprintf(prebase, sizeof prebase, "%s", mfolder);
  if (!rc && pl.n > 0 && (!o.after_manifest || prebase[0])) {
    started = pool_start(&pl, o.after_manifest ? prebase : mfolder);
    if (!started && !o.after_manifest) inline_lane = 1; /* no thread: one lane on this thread */
  }
  /* SDK 4.0: the lease before any container is installed (the Core needs it), with the manifest just accepted */
  if (!rc && o.after_manifest) {
    rc = o.after_manifest(o.hook_user, r.body, r.body_n, m, run.base, sizeof run.base);
    const size_t bl = strlen(run.base);
    /* 4.0.1 (28.4): the files from the lease answer's folder, through the models hosts (its host first when it is
     * not one of them); a base that names no folder downloads nothing */
    if (bl && (run.base[bl - 1] != '/' || !folder_of(&run, run.base))) run.base[0] = 0;
    /* SDK 4.0: a file only from the lease answer's model folder (a build already here needs none) */
    if (!rc && pl.n > 0 && !run.base[0]) {
      rc = tf__fail(TF_ERR_SERVER_REJECTED, "invalid_response: the lease answer names no model folder on Quran Lab's hosts; nothing was downloaded");
      tf__cause_now(rc, "lease", "invalid_answer", 200, NULL, "invalid_response"); /* a 200 that is not what was asked for */
    }
    if (rc) {
      /* no lease: nothing the lanes fetched stays */
      if (started) pool_drop(&pl, &o);
      started = 0;
    } else if (started && !same_folder(run.base, prebase)) {
      /* the lease answer names another folder: what came from the host goes, and the files come from that folder */
      pool_drop(&pl, &o);
      started = 0;
    }
    if (!rc && pl.n > 0 && !started) {
      started = pool_start(&pl, run.base);
      if (!started) inline_lane = 1;
    }
  }
  if (!rc && inline_lane) {
    snprintf(pl.base, sizeof pl.base, "%s", o.after_manifest ? run.base : mfolder);
    pl.live = 1;
    lane_main(&pl);
  }
  long long last = -1;
  if (!rc) report_progress(&pl, &last);
  /* the installs, on this thread, each file once its lane has it: the build's layout first (the Core keys the install
   * mark of the weights with it, 18.7.4), then the manifest's order */
  int lane_failed = 0;
  for (int pass = 0; pass < 2 && !rc; ++pass)
    for (int i = 0; !rc && i < files->n; ++i) {
      const char *role = tf_json_str(tf_json_get(&files->kids[i], "role"));
      const int layout = role && !strcmp(role, "layout");
      if (!want[i] || layout != (pass == 0) || jix[i] < 0) continue;
      const int w = wait_job(&pl, &pl.jobs[jix[i]], &last);
      if (w == 1) {
        lane_failed = 1;
        rc = TF_ERR_DOWNLOAD_FAILED;
      } else if (w == 2) {
        rc = tf__fail(TF_ERR_DOWNLOAD_FAILED, "the download was cancelled");
      } else {
        rc = install_entry(&o, &files->kids[i], r.body, r.body_n);
      }
    }
  if (rc && started) tf__http_cancel(&pl.sh.stop);
  pool_join(&pl);
  if (lane_failed) {
    /* the first file that failed is the call's error, as its lane had it */
    const Job *j = &pl.jobs[pl.first_fail >= 0 ? pl.first_fail : 0];
    tf__err_restore(&j->err);
    rc = j->rc ? j->rc : tf__fail(TF_ERR_DOWNLOAD_FAILED, "a file could not be downloaded");
  }
  free(want);
  free(jix);
  free(pl.jobs);
  if (!rc) {
    char *mp = tf_path_join(o.dest, "manifest.json");
    if (!mp || tf_write_file(mp, r.body, r.body_n)) rc = on_storage(tf__fail(TF_ERR_STORAGE_FAILED, "cannot write manifest.json in %s", o.dest));
    free(mp);
    if (!rc && o.progress) o.progress(o.user, pl.sh.total, pl.sh.total);
  }
  tf_json_free(m);
  tf_http_response_free(&r);
  tf__ep_free(own_ep);
  tf_cond_destroy(&pl.sh.cv);
  tf_mutex_destroy(&pl.sh.lock);
  return rc;
}
