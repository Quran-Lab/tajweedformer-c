/* The native download profile (PLATFORM_CLIENT.md section 4, P2.2, contract LAYOUT.md): the folder's signed
 * manifest, then the full CPU set (common files and every context's engine files; never CoreML or NPU files),
 * each file resumed with HTTP Range from its .part and checked against its size and SHA-256, then moved into
 * place. Layout 3 (manifest format 3): the profile comes from the tags, gzip files are checked as stored, decoded
 * and checked again, and only the decoded file is kept. The manifest is written last, so a folder with a
 * manifest.json is complete. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

extern const char *const TF_QURANLAB_KEY_HEX;
int tf_manifest_verify_signature(const TfJson *m, const char *key_hex);
int tf_gunzip_file(const char *in_path, const char *out_path);

TfDownloadOptions tf_download_default_options(void) {
  TfDownloadOptions o;
  memset(&o, 0, sizeof o);
  o.struct_size = sizeof o;
  o.cdn_url = "https://cdn.quranlab.ai/models";
  o.repo = "Quran-Lab/tajweedformer-v4";
  o.folder = "sdk/v4.0";
  return o;
}

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
/* layout 1/2 (manifest format 2): top-level files and engine context folders */
static int in_profile2(const char *path, const char *contexts) {
  if (!strncmp(path, "coreml/", 7) || !strncmp(path, "npu/", 4)) return 0;
  const char *slash = strchr(path, '/');
  if (!slash) return 1;
  if (strchr(slash + 1, '/')) return 0;
  return ctx_wanted(path, (size_t)(slash - path), contexts);
}
/* layout 3: platform native or *, soc *, need required, context * or one asked for */
/* 1 when a format 3 entry belongs to the native profile, 0 when not, -1 when its platforms field is malformed.
 * platforms is "*" (every platform; what make_manifest3.py writes and the signed text carries) or a list of
 * platform names (a list may also hold "*"). Before 11-08 only the list was accepted, so a published folder's
 * shared files ("*") were skipped silently and the model failed to open; anything else is now an error. */
static int in_profile3(const TfJson *f, const char *contexts) {
  const char *need = tf_json_str(tf_json_get(f, "need")), *cx = tf_json_str(tf_json_get(f, "context"));
  const char *soc = tf_json_str(tf_json_get(f, "soc"));
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
  if (!plat || !need || strcmp(need, "required") || (soc && strcmp(soc, "*"))) return 0;
  return !cx || !strcmp(cx, "*") || ctx_wanted(cx, strlen(cx), contexts);
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

static int get(const TfDownloadOptions *o, const char *rel, const char *to_file, long long from, TfHttpResponse *r) {
  char url[4096], key[512];
  if (tf__model_url(url, sizeof url, o->cdn_url ? o->cdn_url : "https://cdn.quranlab.ai/models", o->repo, o->revision, o->folder, rel))
    return tf__fail(TF_ERR_ARGUMENT, "download: the model URL is too long");
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
  return tf_http(&q, r);
}

/* download `rel` (stored bytes: size, sha) to `to` through `to`.part, resuming; 0 when `to` is verified */
static int fetch_raw(const TfDownloadOptions *o, const char *rel, long long size, const char *sha, const char *to) {
  if (file_ok(to, size, sha)) return 0;
  char *part = (char *)malloc(strlen(to) + sizeof(".part"));
  if (!part) return tf__fail(TF_ERR_NO_MEMORY, "out of memory");
  snprintf(part, strlen(to) + sizeof(".part"), "%s.part", to);
  int rc = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    long long from = file_size(part);
    if (from < 0 || from > size) {
      remove(part);
      from = 0;
    }
    if (from < size) {
      TfHttpResponse r;
      if (get(o, rel, part, from, &r)) {
        rc = tf_last_error_code();
        break; /* the .part stays for the next run */
      }
      const int st = r.status;
      tf_http_response_free(&r);
      if (st == 200 && from > 0 && file_size(part) != size) {
        remove(part); /* the server ignored the range: start again */
        continue;
      }
      if (st != 200 && st != 206) {
        remove(part);
        rc = tf__fail(TF_ERR_HTTP, "%s: HTTP %d", rel, st);
        break;
      }
    }
    if (file_ok(part, size, sha)) {
      remove(to);
      rc = rename(part, to) ? tf__fail(TF_ERR_IO, "cannot move %s into place", rel) : 0;
      break;
    }
    remove(part); /* a mismatch deletes the file and retries once */
    rc = tf__fail(TF_ERR_CHECKSUM, "%s does not match its size and SHA-256 in the manifest", rel);
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

static int fetch_entry(const TfDownloadOptions *o, const TfJson *f) {
  const char *p = tf_json_str(tf_json_get(f, "path"));
  const long long size = (long long)tf_json_get(f, "size")->num;
  const char *sha = tf_json_str(tf_json_get(f, "sha256"));
  const char *enc = tf_json_str(tf_json_get(f, "encoding"));
  if (!enc || !strcmp(enc, "identity")) {
    char *full = tf_path_join(o->dest, p);
    if (!full) return tf__fail(TF_ERR_NO_MEMORY, "out of memory");
    mkparent(full);
    const int rc = fetch_raw(o, p, size, sha, full);
    free(full);
    if (!rc && o->progress) o->progress(o->user, p, size, size);
    return rc;
  }
  if (strcmp(enc, "gzip")) return tf__fail(TF_ERR_MANIFEST, "%s: unknown encoding %s", p, enc);
  const TfJson *d = tf_json_get(f, "decoded");
  const char *dp = tf_json_str(tf_json_get(d, "path"));
  const long long dsize = (long long)tf_json_get(d, "size")->num;
  const char *dsha = tf_json_str(tf_json_get(d, "sha256"));
  char *dfull = tf_path_join(o->dest, dp), *gz = tf_path_join(o->dest, p);
  int rc = 0;
  if (!dfull || !gz) rc = tf__fail(TF_ERR_NO_MEMORY, "out of memory");
  else if (!file_ok(dfull, dsize, dsha)) {
    mkparent(gz);
    mkparent(dfull);
    rc = fetch_raw(o, p, size, sha, gz);
    if (!rc) {
      /* sizeof counts the suffix and its NUL; "+ 8" was one byte short (heap corruption for about half of all
       * destination path lengths once gzip files were really fetched, 11-08) */
      char *tmp = (char *)malloc(strlen(dfull) + sizeof(".inflate"));
      if (!tmp) {
        free(dfull);
        free(gz);
        return tf__fail(TF_ERR_NO_MEMORY, "out of memory");
      }
      snprintf(tmp, strlen(dfull) + sizeof(".inflate"), "%s.inflate", dfull);
      if (tf_gunzip_file(gz, tmp) || !file_ok(tmp, dsize, dsha)) {
        remove(tmp);
        remove(gz); /* any mismatch deletes both */
        rc = tf__fail(TF_ERR_CHECKSUM, "%s does not decode to its decoded size and SHA-256", p);
      } else {
        remove(dfull);
        rc = rename(tmp, dfull) ? tf__fail(TF_ERR_IO, "cannot move %s into place", dp) : 0;
        remove(gz);
      }
      free(tmp);
    }
  }
  if (!rc && o->progress) o->progress(o->user, dp, dsize, dsize);
  free(dfull);
  free(gz);
  return rc;
}

int tf_download_model(const TfDownloadOptions *opt) {
  tf__clear_error();
  TfDownloadOptions o = tf_download_default_options();
  if (opt) memcpy(&o, opt, opt->struct_size && opt->struct_size < sizeof o ? opt->struct_size : sizeof o);
  if (!o.revision || !o.dest || !o.key) return tf__fail(TF_ERR_ARGUMENT, "key, revision and dest are required");
  if (tf_mkdirs(o.dest)) return tf__fail(TF_ERR_IO, "cannot create %s", o.dest);
  TfHttpResponse r;
  if (get(&o, "manifest.json", NULL, 0, &r)) return tf_last_error_code();
  if (r.status != 200) {
    const int st = r.status;
    tf_http_response_free(&r);
    return tf__fail(TF_ERR_HTTP, "manifest.json: HTTP %d", st);
  }
  const char *err = NULL;
  TfJson *m = tf_json_parse(r.body, r.body_n, &err);
  int rc = 0, format = 0;
  if (!m) rc = tf__fail(TF_ERR_MANIFEST, "manifest.json is not JSON: %s", err);
  else if (!tf_manifest_verify_signature(m, o.trusted_key_hex ? o.trusted_key_hex : TF_QURANLAB_KEY_HEX))
    rc = tf__fail(TF_ERR_SIGNATURE, "the model manifest's signature does not verify");
  else {
    const TfJson *min = tf_json_get(m, "min_sdk_version"), *fm = tf_json_get(m, "format");
    format = fm && fm->type == TJ_NUM ? (int)fm->num : 0;
    int a = 0, b = 0, c = 0;
    if (!tf_json_str(min) || sscanf(min->str, "%d.%d.%d", &a, &b, &c) < 1 || a > 2 || (a == 2 && (b > 0 || c > 0)))
      rc = tf__fail(TF_ERR_SDK_TOO_OLD, "the model needs a newer SDK");
    else if (format != 2 && format != 3)
      rc = tf__fail(TF_ERR_UNSUPPORTED_MODEL, "manifest format %d is not supported", format);
  }
  const TfJson *files = rc ? NULL : tf_json_get(m, "files");
  for (int i = 0; files && i < files->n; ++i) {
    const TfJson *f = &files->kids[i];
    const char *p = tf_json_str(tf_json_get(f, "path"));
    const TfJson *d = tf_json_get(f, "decoded");
    const char *dp = d ? tf_json_str(tf_json_get(d, "path")) : NULL;
    if (!p || strstr(p, "..") || p[0] == '/' || strchr(p, '\\') || (dp && (strstr(dp, "..") || dp[0] == '/' || strchr(dp, '\\'))) ||
        !tf_json_is_int(tf_json_get(f, "size"))) {
      rc = tf__fail(TF_ERR_MANIFEST, "a listed path is not safe");
      break;
    }
  }
  for (int i = 0; !rc && files && i < files->n; ++i) {
    const TfJson *f = &files->kids[i];
    const int want = format == 3 ? in_profile3(f, o.contexts) : in_profile2(tf_json_str(tf_json_get(f, "path")), o.contexts);
    if (want < 0) {
      rc = tf__fail(TF_ERR_MANIFEST, "files[%d].platforms is neither \"*\" nor a list of platform names", i);
      break;
    }
    if (!want) continue;
    rc = fetch_entry(&o, f);
  }
  if (!rc) {
    char *mp = tf_path_join(o.dest, "manifest.json");
    if (!mp || tf_write_file(mp, r.body, r.body_n)) rc = tf__fail(TF_ERR_IO, "cannot write manifest.json");
    free(mp);
  }
  tf_json_free(m);
  tf_http_response_free(&r);
  return rc;
}
