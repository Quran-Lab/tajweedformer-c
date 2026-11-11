/* The model downloader: the folder's signed manifest, then the CPU set (the tokens, the calibration, the engine
 * files of the contexts asked for and the sample clips; never the expected table of 2.x builds, CoreML or NPU
 * files, and the reference data only when it is asked for by its role), each file resumed with HTTP Range from
 * its .part and checked against its size and SHA-256, then moved into place. Layout 3 (manifest format 3): the
 * profile comes from the tags, gzip files are checked as stored, decoded and checked again, and only the decoded
 * file is kept. The manifest is written last, so a folder with a manifest.json is complete for what was asked.
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
} Run;

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

static void on_bytes(void *user, long long n);
/* bytes that came over the network in this call (the telemetry's downloads entry counts them) */
static void net_bytes(void *user, long long n) {
  const Run *run = (const Run *)user;
  if (run->o->fetched) *run->o->fetched += n;
  on_bytes(user, n);
}
static void on_bytes(void *user, long long n) {
  Run *run = (Run *)user;
  run->done += n;
  if (run->o->progress) run->o->progress(run->o->user, run->done > run->total ? run->total : run->done, run->total);
}

static int get(Run *run, const char *rel, const char *to_file, long long from, TfHttpResponse *r) {
  const TfDownload *o = run->o;
  char url[4096], key[512];
  if (tf__model_url(url, sizeof url, o->cdn_url ? o->cdn_url : "https://cdn.quranlab.ai/models", o->repo, o->revision, o->folder, rel))
    return tf__fail(TF_ERR_INVALID_ARGUMENT, "the model URL of %s is too long", rel);
  snprintf(key, sizeof key, "x-qlab-key: %s", o->key ? o->key : "");
  const char *hdr[] = {key, NULL};
  TfHttpRequest q;
  memset(&q, 0, sizeof q);
  q.method = "GET";
  q.url = url;
  q.headers = hdr;
  q.download_file = to_file;
  q.range_from = from;
  q.timeout_s = 3600;
  if (to_file) {
    q.on_bytes = net_bytes;
    q.bytes_user = run;
  }
  if (tf_http(&q, r)) {
    /* no answer: the .part stays and the next call resumes */
    if (tf__error_code() == TF__ERR_IO) return tf__error_as(TF_ERR_STORAGE_FAILED);
    return tf__error_as(TF_ERR_DOWNLOAD_FAILED);
  }
  return 0;
}

/* an error status of the CDN as a catalogue error: the key's answers by their code, anything else a stopped
 * download */
static int http_error(const char *rel, const TfHttpResponse *r) {
  const char *b = r->body ? r->body : "";
  if (r->status == 401 && strstr(b, "invalid_key")) return tf__fail(TF_ERR_INVALID_KEY, NULL);
  if (r->status == 403 && strstr(b, "sdk_too_old")) return tf__fail_sdk_too_old(NULL);
  if (r->status == 401 || r->status == 403) return tf__fail(TF_ERR_SERVER_REJECTED, "HTTP %d for %s", r->status, rel);
  return tf__fail(TF_ERR_DOWNLOAD_FAILED, "HTTP %d for %s", r->status, rel);
}

/* download `rel` (stored bytes: size, sha) to `to` through `to`.part, resuming; 0 when `to` is verified */
static int fetch_raw(Run *run, const char *rel, long long size, const char *sha, const char *to) {
  if (file_ok(to, size, sha)) {
    on_bytes(run, size);
    return 0;
  }
  char *part = (char *)malloc(strlen(to) + sizeof(".part"));
  if (!part) return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  snprintf(part, strlen(to) + sizeof(".part"), "%s.part", to);
  int rc = 0;
  const long long before = run->done;
  for (int attempt = 0; attempt < 2; ++attempt) {
    long long from = file_size(part);
    if (from < 0 || from > size) {
      remove(part);
      from = 0;
    }
    run->done = before;
    on_bytes(run, from); /* what an earlier call already fetched */
    if (from > 0 && from < size && run->o->resumed) *run->o->resumed = 1;
    if (from < size) {
      TfHttpResponse r;
      if ((rc = get(run, rel, part, from, &r)) != 0) break; /* the .part stays for the next run */
      const int st = r.status;
      if (st == 200 && from > 0 && file_size(part) != size) {
        tf_http_response_free(&r);
        remove(part); /* the server ignored the range: start again */
        continue;
      }
      if (st != 200 && st != 206) {
        remove(part);
        rc = http_error(rel, &r);
        tf_http_response_free(&r);
        break;
      }
      tf_http_response_free(&r);
    }
    if (file_ok(part, size, sha)) {
      remove(to);
      rc = rename(part, to) ? tf__fail(TF_ERR_STORAGE_FAILED, "cannot move %s into place", rel) : 0;
      run->done = before + size;
      break;
    }
    remove(part); /* a mismatch deletes the file and retries once */
    rc = tf__fail(TF_ERR_CHECKSUM_MISMATCH, "%s", rel);
  }
  free(part);
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
  else if (file_ok(dfull, dsize, dsha)) on_bytes(run, size);
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
  TfHttpResponse r;
  int rc = get(&run, "manifest.json", NULL, 0, &r);
  if (rc) return rc;
  if (r.status != 200) {
    rc = http_error("manifest.json", &r);
    tf_http_response_free(&r);
    return rc;
  }
  /* the same acceptance checks, in the same order, as tf_model_open (tf__manifest_accept) */
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
  return rc;
}
