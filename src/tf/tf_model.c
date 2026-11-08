/* The model: a verified V4 folder (manifest format 2, signature, every needed file's size and SHA-256),
 * the engine opened on it, and the settings layers (app > portal > calibration) including the word level. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

/* Quran Lab's manifest and config signing key (raw Ed25519, hex) and its key id (first 8 bytes of its
 * SHA-256), the public half of qlab-cdn's and qlab-api's MANIFEST_SIGNING_KEY, as in every SDK. */
const char *const TF_QURANLAB_KEY_HEX = "fc02fbd906608ebc206a371d8e61c1062ea555778b44b1ac834061043aee4a33";

static const char *const kPresets[] = {"strict", "balanced", "sensitive", "very_sensitive"};
static const char *const kClasses[] = {"vowel_sub", "vowel_del",  "vowel_ins", "short_to_long",
                                       "long_to_short", "cons_sub", "cons_del", "insertion"};
static const char *const kFeatures[] = {"madd", "qalqalah", "gemination"};
#if ZF_TAJWEED_API_VERSION >= 3
static const char *const kChecks[] = {"letters", "vowels", "length", "madd", "gemination", "qalqalah", "lazim", "word"};
#else
static const char *const kChecks[] = {"letters", "vowels", "length", "madd", "gemination", "qalqalah", "lazim"};
#endif
static const char *const kEngineFiles[] = {"model_engine2.onnx", "model_engine2.onnx.zfg",
                                           "model_engine2.onnx.zfg.bin", "frontend.zfw", "layers.zfw"};

static int in_list(const char *s, const char *const *list, int n) {
  if (!s) return 0;
  for (int i = 0; i < n; ++i)
    if (!strcmp(s, list[i])) return 1;
  return 0;
}
#define IN(s, list) in_list((s), (list), (int)(sizeof(list) / sizeof((list)[0])))

typedef struct {
  int set;        /* 0 none, 1 preset, 2 threshold */
  char preset[32];
  double threshold;
} WordSetting;

struct TfModel {
  char *dir;
  TfJson *manifest;
  TfJson *calib;
  char *tokens[ZF_TAJWEED_VOCAB];
  ZfTajweedModel *zm;
  ZfTajweedConfig *cfg;
  int load_engine;
  int is_signed;
  char key_id[17];
  char context[32];
  char *info_json;
  char *settings_json;
  char *app_json;       /* the app layer as given (kept for re-application) */
  WordSetting app_word, remote_word;
  int remote_word_dropped;
  int debug;
  int stream_live;
  int format; /* manifest format: 2 (a folder per context) or 3 (shared weights, layout 3) */
  char backend[8]; /* the app's backend, "" = the default */
  int sample_rate; /* the rate streams take audio at */
};

const char *tf__model_dir(const TfModel *m) { return m->dir; }
const TfJson *tf__model_calibration(const TfModel *m) { return m->calib; }
const char *tf__model_context(const TfModel *m) { return m->context; }
int tf__model_stream_live(TfModel *m, int delta) {
  if (delta > 0 && m->stream_live) return -1;
  m->stream_live += delta;
  return 0;
}

TfModelOptions tf_model_default_options(void) {
  TfModelOptions o;
  memset(&o, 0, sizeof o);
  o.struct_size = sizeof o;
  o.load_engine = 1;
  o.require_signature = 1;
  o.verify_files = 1;
  o.no_pin = -1; /* the engine's platform default (pinned on phones, not on desktop and servers) */
  return o;
}

/* ------------------------------------------------------------------ manifest */

static int cmp_path(const void *a, const void *b) {
  const TfJson *x = *(const TfJson *const *)a, *y = *(const TfJson *const *)b;
  return strcmp(tf_json_str(tf_json_get(x, "path")), tf_json_str(tf_json_get(y, "path"))); /* UTF-8 byte order */
}

static const char *field_str(const TfJson *m, const char *k, TfBuf *num) {
  const TfJson *v = tf_json_get(m, k);
  if (!v) return "undefined";
  if (v->type == TJ_STR) return v->str;
  if (v->type == TJ_NUM) {
    num->n = 0;
    tf_buf_num(num, v->num);
    return num->p;
  }
  return "?";
}

/* The exact text the signature covers (make_manifest.py signed_text, format 2; make_manifest3.py, format 3). */
char *tf_manifest_signed_text(const TfJson *m) {
  const TfJson *files = tf_json_get(m, "files");
  const TfJson *fmt = tf_json_get(m, "format");
  if (!files || files->type != TJ_ARR || !fmt || fmt->type != TJ_NUM) return NULL;
  const int format = (int)fmt->num;
  TfBuf b, num;
  tf_buf_init(&b);
  tf_buf_init(&num);
  if (format == 2) {
    static const char *const f2[] = {"model_id", "family", "revision", "min_sdk_version", "engine_format", "coreml_format",
                                     "tokens_sha256", "coreml", "sidecar", "keeps", "tokens"};
    tf_buf_puts(&b, "quranlab-model-manifest 2\n");
    for (size_t i = 0; i < sizeof f2 / sizeof f2[0]; ++i) tf_buf_printf(&b, "%s %s\n", f2[i], field_str(m, f2[i], &num));
  } else if (format == 3) {
    static const char *const f3[] = {"model_id", "family", "revision", "min_sdk_version", "engine_format", "coreml_format",
                                     "tokens", "tokens_sha256", "default_context"};
    tf_buf_puts(&b, "quranlab-model-manifest 3\n");
    for (size_t i = 0; i < sizeof f3 / sizeof f3[0]; ++i) tf_buf_printf(&b, "%s %s\n", f3[i], field_str(m, f3[i], &num));
    tf_buf_puts(&b, "contexts ");
    const TfJson *cx = tf_json_get(m, "contexts");
    for (int i = 0; cx && cx->type == TJ_ARR && i < cx->n; ++i) tf_buf_printf(&b, "%s%s", i ? "," : "", tf_json_str(&cx->kids[i]) ? cx->kids[i].str : "");
    tf_buf_puts(&b, "\n");
  } else {
    tf_buf_free(&num);
    tf_buf_free(&b);
    return NULL;
  }
  const TfJson **sorted = (const TfJson **)malloc(sizeof(TfJson *) * (size_t)(files->n + 1));
  if (!sorted) {
    tf_buf_free(&num);
    tf_buf_free(&b);
    return NULL;
  }
  for (int i = 0; i < files->n; ++i) sorted[i] = &files->kids[i];
  qsort(sorted, (size_t)files->n, sizeof *sorted, cmp_path);
  for (int i = 0; i < files->n; ++i) {
    const TfJson *f = sorted[i];
    char sha[65];
    snprintf(sha, sizeof sha, "%s", tf_json_str(tf_json_get(f, "sha256")));
    for (char *q = sha; *q; ++q)
      if (*q >= 'A' && *q <= 'F') *q = (char)(*q - 'A' + 'a');
    tf_buf_printf(&b, "file %s %s %s", sha, field_str(f, "size", &num), tf_json_str(tf_json_get(f, "path")));
    if (format == 3) {
      static const char *const order[] = {"android", "apple", "web", "native"};
      const TfJson *pl = tf_json_get(f, "platforms");
      tf_buf_puts(&b, " ");
      if (!pl || pl->type != TJ_ARR) tf_buf_puts(&b, "*");
      else {
        int first = 1, star = 0;
        for (int k = 0; k < pl->n; ++k)
          if (tf_json_str(&pl->kids[k]) && !strcmp(pl->kids[k].str, "*")) star = 1;
        if (star) tf_buf_puts(&b, "*");
        else
          for (int o = 0; o < 4; ++o)
            for (int k = 0; k < pl->n; ++k)
              if (tf_json_str(&pl->kids[k]) && !strcmp(pl->kids[k].str, order[o])) {
                tf_buf_printf(&b, "%s%s", first ? "" : ",", order[o]);
                first = 0;
              }
      }
      tf_buf_printf(&b, " %s %s %s %s", field_str(f, "soc", &num), field_str(f, "context", &num), field_str(f, "role", &num),
                    field_str(f, "need", &num));
      const char *encd = tf_json_str(tf_json_get(f, "encoding"));
      tf_buf_printf(&b, " %s", encd ? encd : "identity");
      const TfJson *dec = tf_json_get(f, "decoded");
      if (encd && strcmp(encd, "identity") && dec) {
        char dsha[65];
        snprintf(dsha, sizeof dsha, "%s", tf_json_str(tf_json_get(dec, "sha256")) ? tf_json_str(tf_json_get(dec, "sha256")) : "");
        for (char *q = dsha; *q; ++q)
          if (*q >= 'A' && *q <= 'F') *q = (char)(*q - 'A' + 'a');
        tf_buf_printf(&b, " %s %s %s", dsha, field_str(dec, "size", &num), tf_json_str(tf_json_get(dec, "path")));
      } else {
        tf_buf_puts(&b, " - - -");
      }
    }
    tf_buf_puts(&b, "\n");
  }
  free(sorted);
  tf_buf_free(&num);
  return tf_buf_take(&b);
}

static int safe_path(const char *p) {
  if (!p || !*p || p[0] == '/' || strchr(p, '\\') || (p[0] && p[1] == ':')) return 0;
  const char *s = p;
  while (*s) {
    const char *e = strchr(s, '/');
    const size_t n = e ? (size_t)(e - s) : strlen(s);
    if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
    if (!e) break;
    s = e + 1;
  }
  return 1;
}

static int version_at_least(const char *v, const char *min) {
  int a[3] = {0}, b[3] = {0};
  if (sscanf(v, "%d.%d.%d", &a[0], &a[1], &a[2]) < 1 || sscanf(min, "%d.%d.%d", &b[0], &b[1], &b[2]) < 1) return 0;
  for (int i = 0; i < 3; ++i)
    if (a[i] != b[i]) return a[i] > b[i];
  return 1;
}

/* Verify the signature with `key_hex` (64 hex). 1 ok, 0 bad. */
int tf_manifest_verify_signature(const TfJson *m, const char *key_hex) {
  const char *sig64 = tf_json_str(tf_json_get(m, "signature"));
  uint8_t sig[66], pk[32];
  if (!sig64 || tf_base64_decode(sig64, sig, sizeof sig) != 64) return 0;
  if (tf_hex_decode(key_hex, pk, 32)) return 0;
  char *text = tf_manifest_signed_text(m);
  if (!text) return 0;
  const int ok = tf_ed25519_verify(sig, (const uint8_t *)text, strlen(text), pk);
  free(text);
  return ok;
}

/* Check the manifest's shape and compatibility. Returns 0 or a TF_ERR_*. */
static int check_manifest(const TfJson *m, int require_signature, const char *key_hex, int *is_signed) {
  if (!m || m->type != TJ_OBJ) return tf__fail(TF_ERR_MANIFEST, "manifest.json: not an object");
  const TfJson *fmt = tf_json_get(m, "format");
  if (!fmt || fmt->type != TJ_NUM || (fmt->num != 2 && fmt->num != 3))
    return tf__fail(TF_ERR_UNSUPPORTED_MODEL, "manifest format %g is not supported by this SDK (2 or 3)", fmt && fmt->type == TJ_NUM ? fmt->num : -1.0);
  const int f3 = fmt->num == 3;
  static const char *const strs2[] = {"model_id", "family", "revision", "min_sdk_version", "tokens_sha256", "coreml", "sidecar", "keeps", "tokens"};
  static const char *const strs3[] = {"model_id", "family", "revision", "min_sdk_version", "tokens_sha256", "tokens", "default_context"};
  const char *const *strs = f3 ? strs3 : strs2;
  const size_t nstrs = f3 ? sizeof strs3 / sizeof strs3[0] : sizeof strs2 / sizeof strs2[0];
  for (size_t i = 0; i < nstrs; ++i)
    if (!tf_json_str(tf_json_get(m, strs[i])) || !*tf_json_get(m, strs[i])->str) return tf__fail(TF_ERR_MANIFEST, "manifest.json: %s missing", strs[i]);
  if (!tf_json_is_int(tf_json_get(m, "engine_format")) || !tf_json_is_int(tf_json_get(m, "coreml_format")))
    return tf__fail(TF_ERR_MANIFEST, "manifest.json: engine_format / coreml_format are not integers");
  const TfJson *files = tf_json_get(m, "files");
  if (!files || files->type != TJ_ARR || !files->n) return tf__fail(TF_ERR_MANIFEST, "manifest.json: files missing");
  for (int i = 0; i < files->n; ++i) {
    const TfJson *f = &files->kids[i];
    const char *p = tf_json_str(tf_json_get(f, "path"));
    uint8_t h[32];
    if (!safe_path(p)) return tf__fail(TF_ERR_MANIFEST, "manifest.json: bad path");
    const char *enc = tf_json_str(tf_json_get(f, "encoding"));
    if (enc && strcmp(enc, "identity")) {
      const TfJson *d = tf_json_get(f, "decoded");
      uint8_t dh[32];
      if (strcmp(enc, "gzip") || !d || !safe_path(tf_json_str(tf_json_get(d, "path"))) || !tf_json_is_int(tf_json_get(d, "size")) ||
          tf_hex_decode(tf_json_str(tf_json_get(d, "sha256")), dh, 32))
        return tf__fail(TF_ERR_MANIFEST, "manifest.json: the encoding of %s", p);
    }
    if (!tf_json_is_int(tf_json_get(f, "size")) || tf_json_get(f, "size")->num < 0) return tf__fail(TF_ERR_MANIFEST, "manifest.json: size of %s", p);
    if (tf_hex_decode(tf_json_str(tf_json_get(f, "sha256")), h, 32)) return tf__fail(TF_ERR_MANIFEST, "manifest.json: sha256 of %s", p);
    for (int k = 0; k < i; ++k)
      if (!strcmp(p, tf_json_str(tf_json_get(&files->kids[k], "path")))) return tf__fail(TF_ERR_MANIFEST, "manifest.json: %s is listed twice", p);
  }
  const TfJson *sig = tf_json_get(m, "signature");
  *is_signed = 0;
  if (sig) {
    if (!tf_manifest_verify_signature(m, key_hex))
      return tf__fail(TF_ERR_SIGNATURE, "the manifest signature does not verify with the trusted key");
    *is_signed = 1;
  } else if (require_signature) {
    return tf__fail(TF_ERR_SIGNATURE_REQUIRED, "the manifest is not signed and a signature is required");
  }
  if (strcmp(tf_json_str(tf_json_get(m, "family")), "tajweedformer"))
    return tf__fail(TF_ERR_UNSUPPORTED_MODEL, "family %s: SDK 2.0.0 runs TajweedFormer V4 only", tf_json_str(tf_json_get(m, "family")));
  if (!version_at_least(TF_SDK_VERSION, tf_json_str(tf_json_get(m, "min_sdk_version"))))
    return tf__fail(TF_ERR_SDK_TOO_OLD, "%s needs SDK %s or newer (this is %s)", tf_json_str(tf_json_get(m, "model_id")),
                    tf_json_str(tf_json_get(m, "min_sdk_version")), TF_SDK_VERSION);
  if (tf_json_get(m, "engine_format")->num != (f3 ? 3 : 2))
    return tf__fail(TF_ERR_UNSUPPORTED_MODEL, "engine format %g is not supported with manifest format %d", tf_json_get(m, "engine_format")->num, f3 ? 3 : 2);
  return 0;
}

/* a file as it is kept on disk: the decoded file of a gzip entry (layout 3), else the entry itself */
static const TfJson *local_of(const TfJson *f) {
  const char *enc = tf_json_str(tf_json_get(f, "encoding"));
  return enc && !strcmp(enc, "gzip") ? tf_json_get(f, "decoded") : f;
}
static const TfJson *listed(const TfJson *m, const char *path) {
  const TfJson *files = tf_json_get(m, "files");
  for (int i = 0; i < files->n; ++i)
    if (!strcmp(tf_json_str(tf_json_get(local_of(&files->kids[i]), "path")), path)) return local_of(&files->kids[i]);
  return NULL;
}
static int verify_file(const char *dir, const TfJson *f) {
  f = local_of(f);
  const char *p = tf_json_str(tf_json_get(f, "path"));
  char *full = tf_path_join(dir, p);
  if (!full) return tf__fail(TF_ERR_NO_MEMORY, "out of memory");
  uint8_t h[32], want[32];
  long long size = 0;
  if (tf_sha256_file(full, h, &size)) {
    free(full);
    return tf__fail(TF_ERR_IO, "%s is listed but missing", p);
  }
  free(full);
  tf_hex_decode(tf_json_str(tf_json_get(f, "sha256")), want, 32);
  if (size != (long long)tf_json_get(f, "size")->num || memcmp(h, want, 32))
    return tf__fail(TF_ERR_CHECKSUM, "%s does not match its size and SHA-256 in the manifest", p);
  return 0;
}

/* ------------------------------------------------------------------ open */

static int parse_tokens(TfModel *m, const char *text) {
  int n = 0;
  const char *s = text;
  while (*s) {
    const char *e = strchr(s, '\n');
    size_t len = e ? (size_t)(e - s) : strlen(s);
    while (len && (s[len - 1] == '\r' || s[len - 1] == ' ')) len--;
    if (len) {
      size_t sp = len;
      while (sp && s[sp - 1] != ' ') sp--;
      int id = n;
      size_t symlen = len;
      if (sp && sp < len) {
        id = atoi(s + sp);
        symlen = sp - 1;
      }
      if (id < 0 || id >= ZF_TAJWEED_VOCAB) return -1;
      free(m->tokens[id]);
      m->tokens[id] = (char *)malloc(symlen + 1);
      if (!m->tokens[id]) return -1;
      memcpy(m->tokens[id], s, symlen);
      m->tokens[id][symlen] = 0;
      n++;
    }
    if (!e) break;
    s = e + 1;
  }
  for (int i = 0; i < ZF_TAJWEED_VOCAB; ++i)
    if (!m->tokens[i]) return -1;
  return n == ZF_TAJWEED_VOCAB ? 0 : -1;
}

static int load_json_file(const char *dir, const char *name, TfJson **out) {
  char *full = tf_path_join(dir, name), *text = NULL;
  size_t n = 0;
  if (!full || tf_read_file(full, &text, &n)) {
    free(full);
    return tf__fail(TF_ERR_IO, "cannot read %s", name);
  }
  free(full);
  const char *err = NULL;
  *out = tf_json_parse(text, n, &err);
  free(text);
  if (!*out) return tf__fail(TF_ERR_MANIFEST, "%s is not JSON: %s", name, err ? err : "?");
  return 0;
}

void tf_model_close(TfModel *m) {
  if (!m) return;
  if (m->cfg) zf_tajweed_config_destroy(m->cfg);
  if (m->zm) zf_tajweed_model_close(m->zm);
  tf_json_free(m->manifest);
  tf_json_free(m->calib);
  for (int i = 0; i < ZF_TAJWEED_VOCAB; ++i) free(m->tokens[i]);
  free(m->dir);
  free(m->info_json);
  free(m->settings_json);
  free(m->app_json);
  free(m);
}

static int word_parse(const TfJson *w, const TfJson *word_cal, WordSetting *out, int strict);

TfModel *tf_model_open(const char *folder, const TfModelOptions *opt) {
  tf__clear_error();
  TfModelOptions o = tf_model_default_options();
  if (opt) memcpy(&o, opt, opt->struct_size && opt->struct_size < sizeof o ? opt->struct_size : sizeof o);
  if (!folder) {
    tf__fail(TF_ERR_ARGUMENT, "no folder");
    return NULL;
  }
  TfModel *m = (TfModel *)calloc(1, sizeof *m);
  if (!m) {
    tf__fail(TF_ERR_NO_MEMORY, "out of memory");
    return NULL;
  }
  m->dir = (char *)malloc(strlen(folder) + 1);
  strcpy(m->dir, folder);
  m->debug = o.debug_build;
  m->load_engine = o.load_engine;
  const char *key = o.trusted_key_hex ? o.trusted_key_hex : TF_QURANLAB_KEY_HEX;
  {
    uint8_t pk[32], h[32];
    if (tf_hex_decode(key, pk, 32)) {
      tf__fail(TF_ERR_ARGUMENT, "trusted_key_hex is not 64 hex digits");
      goto fail;
    }
    tf_sha256(pk, 32, h);
    tf_hex_encode(h, 8, m->key_id);
  }
  if (load_json_file(folder, "manifest.json", &m->manifest)) goto fail;
  if (check_manifest(m->manifest, o.require_signature, key, &m->is_signed)) goto fail;
  /* role files and the V4 files must be listed */
  {
    const char *need[] = {tf_json_str(tf_json_get(m->manifest, "tokens")), "calibration.json", "expected_tj3.zfx"};
    m->format = (int)tf_json_get(m->manifest, "format")->num;
    for (int i = 0; i < 3; ++i)
      if (!listed(m->manifest, need[i])) {
        tf__fail(TF_ERR_NOT_LISTED, "%s is required by a TajweedFormer folder and is not listed", need[i]);
        goto fail;
      }
    const TfJson *tok = listed(m->manifest, need[0]);
    if (strcmp(tf_json_str(tf_json_get(tok, "sha256")), tf_json_str(tf_json_get(m->manifest, "tokens_sha256")))) {
      tf__fail(TF_ERR_MANIFEST, "tokens_sha256 does not match %s", need[0]);
      goto fail;
    }
  }
  /* the common files, verified before they are read */
  {
    const TfJson *files = tf_json_get(m->manifest, "files");
    for (int i = 0; i < files->n; ++i) {
      const char *p = tf_json_str(tf_json_get(local_of(&files->kids[i]), "path"));
      if (strchr(p, '/')) continue;
      if (m->format == 3 && (!strcmp(p, "layers.zfw") || !strcmp(p, "frontend.zfw"))) continue; /* engine files, below */
      if (verify_file(folder, &files->kids[i])) goto fail;
    }
  }
  if (load_json_file(folder, "calibration.json", &m->calib)) goto fail;
  {
    const char *f = tf_json_str(tf_json_get(m->calib, "format"));
    if (!f || strcmp(f, "quranlab.tajweed-calibration/1") || !tf_json_get(m->calib, "contexts") ||
        tf_json_get(m->calib, "contexts")->type != TJ_OBJ) {
      tf__fail(TF_ERR_CALIBRATION, "calibration.json is not quranlab.tajweed-calibration/1");
      goto fail;
    }
    const char *mid = tf_json_str(tf_json_get(m->calib, "model_id"));
    if (!mid || strcmp(mid, tf_json_str(tf_json_get(m->manifest, "model_id")))) {
      tf__fail(TF_ERR_CALIBRATION, "calibration.json is for another model");
      goto fail;
    }
  }
  {
    char *full = tf_path_join(folder, tf_json_str(tf_json_get(m->manifest, "tokens"))), *text = NULL;
    if (!full || tf_read_file(full, &text, NULL) || parse_tokens(m, text)) {
      free(full);
      free(text);
      tf__fail(TF_ERR_MANIFEST, "tokens.txt does not have the 38 TajweedFormer tokens");
      goto fail;
    }
    free(full);
    free(text);
  }
  /* the context: options > the portal's block (when given) > calibration's default */
  {
    const char *ctx = o.context;
    TfJson *remote = NULL;
    if (!ctx && o.remote_json) {
      remote = tf_json_parse(o.remote_json, strlen(o.remote_json), NULL);
      const TfJson *blk = remote ? (tf_json_get(remote, "tajweed") ? tf_json_get(remote, "tajweed") : remote) : NULL;
      const char *rc = tf_json_str(tf_json_get(blk, "context"));
      if (rc && tf_json_get(tf_json_get(m->calib, "contexts"), rc)) ctx = rc;
    }
    if (!ctx) ctx = tf_json_str(tf_json_get(m->calib, "default_context"));
    if (!ctx || !tf_json_get(tf_json_get(m->calib, "contexts"), ctx)) {
      tf_json_free(remote);
      tf__fail(TF_ERR_ARGUMENT, "context %s is not one this model ships", ctx ? ctx : "(none)");
      goto fail;
    }
    snprintf(m->context, sizeof m->context, "%s", ctx);
    tf_json_free(remote);
  }
  /* the context's engine files: listed and verified; nothing else under the folder is opened */
  if (o.load_engine) {
    /* layout 3: the shared weights and the context's graph, graph data and geometry overlay (the ONNX file is the
     * ONNX Runtime tier's, never opened by the engine) */
    static const char *const k3[] = {"../layers.zfw", "../frontend.zfw", "model_engine2.onnx.zfg", "model_engine2.onnx.zfg.bin", "frames.zfw"};
    const char *const *names = m->format == 3 ? k3 : kEngineFiles;
    const size_t count = m->format == 3 ? sizeof k3 / sizeof k3[0] : sizeof kEngineFiles / sizeof kEngineFiles[0];
    for (size_t i = 0; i < count; ++i) {
      char p[96];
      if (!strncmp(names[i], "../", 3)) snprintf(p, sizeof p, "%s", names[i] + 3);
      else snprintf(p, sizeof p, "%s/%s", m->context, names[i]);
      const TfJson *f = listed(m->manifest, p);
      if (!f) {
        tf__fail(TF_ERR_NOT_LISTED, "%s is not listed in the manifest", p);
        goto fail;
      }
      if (o.verify_files && verify_file(folder, f)) goto fail;
    }
  }
  {
    ZfTajweedOpenOptions zo = zf_tajweed_open_default_options();
    zo.context = m->context;
    zo.load_engine = o.load_engine;
    zo.threads = o.threads;
    zo.sample_rate = o.sample_rate;
    m->sample_rate = o.sample_rate > 0 ? o.sample_rate : 16000;
    zo.batch = o.batch;
    /* ZfTajweedOpenOptions.no_pin (API 3): -1 keeps the engine's platform default (ZF_NO_PIN_DEFAULT: Windows,
     * Linux and macOS do not pin), 0 or 1 overrides it */
    if (o.struct_size >= offsetof(TfModelOptions, no_pin) + sizeof o.no_pin && o.no_pin >= 0) zo.no_pin = o.no_pin;
    const char *err = NULL;
    m->zm = zf_tajweed_model_open(folder, &zo, &err);
    if (!m->zm) {
      tf__fail(TF_ERR_ENGINE, "engine: %s", err ? err : "cannot open the model");
      goto fail;
    }
    m->cfg = zf_tajweed_config_create(m->zm);
    if (!m->cfg) {
      tf__fail(TF_ERR_ENGINE, "engine: cannot create a config");
      goto fail;
    }
    zf_tajweed_config_set_debug_build(m->cfg, o.debug_build);
  }
  if (o.remote_json && tf_settings_set_remote_json(m, o.remote_json, strlen(o.remote_json)) < 0) tf__clear_error();
  return m;
fail:
  tf_model_close(m);
  return NULL;
}

const char *tf_model_context(const TfModel *m) { return m ? m->context : NULL; }
int tf_model_chunk_ms(const TfModel *m) { return m && m->zm ? zf_tajweed_model_chunk_ms(m->zm) : 0; }
const char *tf_model_token_name(const TfModel *m, int id) {
  return m && id >= 0 && id < ZF_TAJWEED_VOCAB ? m->tokens[id] : NULL;
}
ZfTajweedModel *tf_model_engine(TfModel *m) { return m ? m->zm : NULL; }
ZfTajweedConfig *tf_model_engine_config(TfModel *m) { return m ? m->cfg : NULL; }
int tf__model_has_engine(const TfModel *m) { return m->load_engine; }

/* ------------------------------------------------------------------ word level */

/* calibration's word block: contexts[ctx].word, else the root's word */
static const TfJson *word_calibration(const TfModel *m) {
  const TfJson *c = tf_json_get(tf_json_get(tf_json_get(m->calib, "contexts"), m->context), "word");
  if (!c) c = tf_json_get(m->calib, "word");
  if (!c || c->type != TJ_OBJ) return NULL;
  const char *st = tf_json_str(tf_json_get(c, "status"));
  if (st && !strcmp(st, "off")) return NULL;
  return c;
}
/* a word preset's threshold: presets.<name>.tau (the reference's block), or {threshold}, or a number, or thresholds.<name> */
static int word_preset_threshold(const TfJson *wc, const char *name, double *t) {
  const TfJson *p = tf_json_get(tf_json_get(wc, "presets"), name);
  if (p && p->type == TJ_OBJ) p = tf_json_get(p, "tau") ? tf_json_get(p, "tau") : tf_json_get(p, "threshold");
  if (!p) p = tf_json_get(tf_json_get(wc, "thresholds"), name);
  if (!p || p->type != TJ_NUM) return 0;
  *t = p->num;
  return 1;
}
/* parse {"preset": name} or {"threshold": x}. strict: errors are failures (app); else returns -1 (dropped). */
static int word_parse(const TfJson *w, const TfJson *wc, WordSetting *out, int strict) {
  memset(out, 0, sizeof *out);
  if (!w) return 0;
  if (w->type != TJ_OBJ) return strict ? tf__fail(TF_ERR_SETTINGS, "word must be an object") : -1;
  const TfJson *p = tf_json_get(w, "preset"), *t = tf_json_get(w, "threshold");
  for (int i = 0; i < w->n; ++i)
    if (strcmp(w->kids[i].key, "preset") && strcmp(w->kids[i].key, "threshold"))
      return strict ? tf__fail(TF_ERR_SETTINGS, "word.%s is not a word option", w->kids[i].key) : -1;
  if (!!p == !!t) return strict ? tf__fail(TF_ERR_SETTINGS, "word needs exactly one of preset or threshold") : -1;
  if (p) {
    double tmp;
    if (p->type != TJ_STR || (wc && !word_preset_threshold(wc, p->str, &tmp)) || strlen(p->str) >= sizeof out->preset)
      return strict ? tf__fail(TF_ERR_SETTINGS, "word.preset is not a word preset of this model") : -1;
    out->set = 1;
    snprintf(out->preset, sizeof out->preset, "%s", p->str);
    return 0;
  }
  /* any finite number: it is clamped to the calibration's custom_range and floor when resolved (the engine's rule) */
  if (t->type != TJ_NUM || !(t->num == t->num) || t->num > 1e300 || t->num < -1e300)
    return strict ? tf__fail(TF_ERR_SETTINGS, "word.threshold must be a finite number") : -1;
  out->set = 2;
  out->threshold = t->num;
  return 0;
}

void tf__model_word_effective(const TfModel *m, int *enabled, double *threshold, char *preset, size_t preset_n,
                              char *source, size_t source_n) {
#if ZF_TAJWEED_API_VERSION >= 3
  {
    ZfTajweedWordSettings w;
    memset(&w, 0, sizeof w);
    w.struct_size = sizeof w;
    static const char *const src[] = {"calibration", "remote", "app"};
    if (zf_tajweed_config_word(m->cfg, &w) == 0) {
      *enabled = w.available && w.enabled;
      *threshold = w.available ? w.threshold : NAN;
      snprintf(preset, preset_n, "%s", w.available ? zf_tajweed_word_preset_name(w.preset) : "");
      snprintf(source, source_n, "%s", w.preset_source >= 0 && w.preset_source < 3 ? src[w.preset_source] : "calibration");
      return;
    }
  }
#endif
  const TfJson *wc = word_calibration(m);
  *enabled = 0;
  *threshold = NAN;
  snprintf(preset, preset_n, "%s", "");
  snprintf(source, source_n, "%s", "calibration");
  if (!wc) return;
  const WordSetting *s = m->app_word.set ? &m->app_word : m->remote_word.set ? &m->remote_word : NULL;
  if (s) snprintf(source, source_n, "%s", s == &m->app_word ? "app" : "remote");
  if (s && s->set == 2) {
    *enabled = 1;
    *threshold = s->threshold;
    snprintf(preset, preset_n, "%s", "custom");
    return;
  }
  const char *name = s ? s->preset : tf_json_str(tf_json_get(wc, "default_preset"));
  double t;
  if (name && word_preset_threshold(wc, name, &t)) {
    *enabled = 1;
    *threshold = t;
    snprintf(preset, preset_n, "%s", name);
  }
}

/* ------------------------------------------------------------------ settings */

static int app_check(const TfModel *m, const TfJson *a, WordSetting *word) {
  if (a->type != TJ_OBJ) return tf__fail(TF_ERR_SETTINGS, "app settings must be a JSON object");
  memset(word, 0, sizeof *word);
  for (int i = 0; i < a->n; ++i) {
    const TfJson *v = &a->kids[i];
    const char *k = v->key;
    if (!strcmp(k, "preset")) {
      if (!IN(tf_json_str(v), kPresets)) return tf__fail(TF_ERR_SETTINGS, "preset must be strict, balanced, sensitive or very_sensitive");
    } else if (!strcmp(k, "context")) {
      if (!tf_json_str(v) || !tf_json_get(tf_json_get(m->calib, "contexts"), v->str))
        return tf__fail(TF_ERR_SETTINGS, "context is not one this model ships");
    } else if (!strcmp(k, "overrides")) {
      if (v->type != TJ_OBJ) return tf__fail(TF_ERR_SETTINGS, "overrides must be an object");
      for (int j = 0; j < v->n; ++j) {
        const TfJson *ov = &v->kids[j];
        const int feature = IN(ov->key, kFeatures);
        if (!feature && !IN(ov->key, kClasses)) return tf__fail(TF_ERR_SETTINGS, "overrides.%s is not a class", ov->key);
        if (ov->type != TJ_OBJ) return tf__fail(TF_ERR_SETTINGS, "overrides.%s must be an object", ov->key);
        const TfJson *p = tf_json_get(ov, "preset"), *t = tf_json_get(ov, "threshold");
        if (ov->n != 1 || (!p && !t)) return tf__fail(TF_ERR_SETTINGS, "overrides.%s needs exactly one of preset or threshold", ov->key);
        if (p && !IN(tf_json_str(p), kPresets)) return tf__fail(TF_ERR_SETTINGS, "overrides.%s.preset is not a preset", ov->key);
        if (t && (feature || t->type != TJ_NUM || !(t->num >= -20 && t->num <= 20)))
          return tf__fail(TF_ERR_SETTINGS, "overrides.%s.threshold must be a number from -20 to 20 (a class only)", ov->key);
      }
    } else if (!strcmp(k, "checks")) {
      if (v->type != TJ_OBJ) return tf__fail(TF_ERR_SETTINGS, "checks must be an object");
      for (int j = 0; j < v->n; ++j)
        if (!IN(v->kids[j].key, kChecks) || v->kids[j].type != TJ_BOOL)
          return tf__fail(TF_ERR_SETTINGS, "checks.%s is not a check with a boolean", v->kids[j].key);
    } else if (!strcmp(k, "word")) {
      if (word_parse(v, word_calibration(m), word, 1)) return TF_ERR_SETTINGS;
    } else {
      return tf__fail(TF_ERR_SETTINGS, "%s is not a tajweed setting", k);
    }
  }
  return 0;
}

static int app_apply(TfModel *m, const TfJson *a) {
  ZfTajweedConfig *c = m->cfg;
  zf_tajweed_config_clear_app(c);
  if (!a) return 0;
  int bad = 0;
  const TfJson *v;
  if ((v = tf_json_get(a, "context"))) bad |= zf_tajweed_config_set_context(c, v->str);
  if ((v = tf_json_get(a, "preset"))) bad |= zf_tajweed_config_set_preset(c, v->str);
  if ((v = tf_json_get(a, "overrides")))
    for (int j = 0; j < v->n; ++j) {
      const TfJson *ov = &v->kids[j];
      const TfJson *p = tf_json_get(ov, "preset"), *t = tf_json_get(ov, "threshold");
      bad |= p ? zf_tajweed_config_set_class_preset(c, ov->key, p->str) : zf_tajweed_config_set_class_threshold(c, ov->key, t->num);
    }
  if ((v = tf_json_get(a, "checks")))
    for (int j = 0; j < v->n; ++j) bad |= zf_tajweed_config_set_check(c, v->kids[j].key, v->kids[j].boolean);
#if ZF_TAJWEED_API_VERSION >= 3
  /* API 3: the engine scores words itself; the app's word setting goes to it too */
  if ((v = tf_json_get(a, "word"))) {
    const TfJson *p = tf_json_get(v, "preset"), *t = tf_json_get(v, "threshold");
    if (p) bad |= zf_tajweed_config_set_word_preset(c, p->str);
    else if (t) bad |= zf_tajweed_config_set_word_threshold(c, t->num);
  }
#endif
  return bad ? tf__fail(TF_ERR_SETTINGS, "the engine refused an app setting") : 0;
}

int tf_settings_set_app_json(TfModel *m, const char *json) {
  tf__clear_error();
  if (!m) return tf__fail(TF_ERR_ARGUMENT, "no model");
  if (!json) {
    zf_tajweed_config_clear_app(m->cfg);
    memset(&m->app_word, 0, sizeof m->app_word);
    free(m->app_json);
    m->app_json = NULL;
    return 0;
  }
  const char *err = NULL;
  TfJson *a = tf_json_parse(json, strlen(json), &err);
  if (!a) return tf__fail(TF_ERR_SETTINGS, "app settings are not JSON: %s", err);
  WordSetting w;
  int rc = app_check(m, a, &w);
  if (rc == 0) {
    rc = app_apply(m, a);
    if (rc) {
      app_apply(m, NULL);
      memset(&m->app_word, 0, sizeof m->app_word);
    } else {
      m->app_word = w;
      free(m->app_json);
      m->app_json = (char *)malloc(strlen(json) + 1);
      if (m->app_json) strcpy(m->app_json, json);
    }
  }
  tf_json_free(a);
  return rc;
}

int tf_settings_set_remote_json(TfModel *m, const char *json, size_t len) {
  tf__clear_error();
  if (!m) return tf__fail(TF_ERR_ARGUMENT, "no model");
  memset(&m->remote_word, 0, sizeof m->remote_word);
  m->remote_word_dropped = 0;
  if (json && ZF_TAJWEED_API_VERSION < 3) {
    TfJson *r = tf_json_parse(json, len, NULL);
    if (r) {
      const TfJson *blk = tf_json_get(r, "tajweed");
      if (!blk && (tf_json_get(r, "v") || tf_json_get(r, "preset"))) blk = r;
      const TfJson *v = tf_json_get(blk, "v");
      if (blk && v && v->type == TJ_NUM && v->num == 1 && tf_json_get(blk, "word"))
        m->remote_word_dropped = word_parse(tf_json_get(blk, "word"), word_calibration(m), &m->remote_word, 0) ? 1 : 0;
      tf_json_free(r);
    }
  }
  const int dropped = zf_tajweed_config_set_remote(m->cfg, json, json ? len : 0);
  if (dropped < 0) return tf__fail(TF_ERR_ARGUMENT, "the remote settings are not JSON; the portal layer was cleared");
  return dropped;
}

int tf_settings_set_backend(TfModel *m, const char *backend) {
  if (!m) return tf__fail(TF_ERR_ARGUMENT, "no model");
  if (zf_tajweed_config_set_backend(m->cfg, backend)) return tf__fail(TF_ERR_SETTINGS, "backend must be int8 or fp32");
  snprintf(m->backend, sizeof m->backend, "%s", backend ? backend : "");
  return 0;
}
const char *tf__model_backend(const TfModel *m) { return m->backend[0] ? m->backend : NULL; }

/* engine JSON + ,"word":{...} before its closing brace */
char *tf__settings_json_alloc(TfModel *m) {
  const char *e = zf_tajweed_config_json(m->cfg);
  if (!e) return NULL;
#if ZF_TAJWEED_API_VERSION >= 3
  {
    /* the engine resolves the word level itself and writes "word" */
    char *copy = (char *)malloc(strlen(e) + 1);
    if (copy) strcpy(copy, e);
    return copy;
  }
#endif
  size_t n = strlen(e);
  while (n && e[n - 1] != '}') n--;
  if (!n) return NULL;
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_put(&b, e, n - 1);
  int en;
  double th;
  char preset[32], source[16];
  tf__model_word_effective(m, &en, &th, preset, sizeof preset, source, sizeof source);
  const TfJson *wc = word_calibration(m);
  tf_buf_puts(&b, ",\"word\":{\"available\":");
  tf_buf_puts(&b, wc ? "true" : "false");
  tf_buf_puts(&b, ",\"enabled\":");
  tf_buf_puts(&b, en ? "true" : "false");
  tf_buf_puts(&b, ",\"preset\":");
  if (*preset) tf_buf_json_str(&b, preset);
  else tf_buf_puts(&b, "null");
  tf_buf_puts(&b, ",\"threshold\":");
  tf_buf_num(&b, th);
  tf_buf_puts(&b, ",\"source\":");
  tf_buf_json_str(&b, source);
  tf_buf_printf(&b, ",\"dropped\":%s}", m->remote_word_dropped ? "[\"word\"]" : "[]");
  tf_buf_puts(&b, "}");
  return tf_buf_take(&b);
}

const char *tf_settings_json(TfModel *m) {
  if (!m) return NULL;
  free(m->settings_json);
  m->settings_json = tf__settings_json_alloc(m);
  return m->settings_json;
}

/* ------------------------------------------------------------------ info */

const char *tf_model_info_json(TfModel *m) {
  if (!m) return NULL;
  if (m->info_json) return m->info_json;
  TfBuf b;
  tf_buf_init(&b);
  const TfJson *man = m->manifest;
  tf_buf_puts(&b, "{\"model_id\":");
  tf_buf_json_str(&b, tf_json_str(tf_json_get(man, "model_id")));
  tf_buf_puts(&b, ",\"revision\":");
  tf_buf_json_str(&b, tf_json_str(tf_json_get(man, "revision")));
  tf_buf_puts(&b, ",\"family\":");
  tf_buf_json_str(&b, tf_json_str(tf_json_get(man, "family")));
  tf_buf_printf(&b, ",\"signed\":%s,\"key_id\":", m->is_signed ? "true" : "false");
  if (m->is_signed) tf_buf_json_str(&b, m->key_id);
  else tf_buf_puts(&b, "null");
  tf_buf_puts(&b, ",\"contexts\":[");
  const TfJson *cx = tf_json_get(m->calib, "contexts");
  for (int i = 0; i < cx->n; ++i) {
    if (i) tf_buf_puts(&b, ",");
    tf_buf_json_str(&b, cx->kids[i].key);
  }
  tf_buf_puts(&b, "],\"context\":");
  tf_buf_json_str(&b, m->context);
  tf_buf_puts(&b, ",\"default_context\":");
  tf_buf_json_str(&b, tf_json_str(tf_json_get(m->calib, "default_context")));
  tf_buf_printf(&b, ",\"chunk_ms\":%d,\"vocab\":%d,\"blank\":%d,\"engine\":%s,\"tokens\":[", tf_model_chunk_ms(m), ZF_TAJWEED_VOCAB,
                ZF_TAJWEED_BLANK, m->load_engine ? "\"exact-cpu\"" : "\"scorer\"");
  for (int i = 0; i < ZF_TAJWEED_VOCAB; ++i) {
    if (i) tf_buf_puts(&b, ",");
    tf_buf_json_str(&b, m->tokens[i]);
  }
  tf_buf_puts(&b, "],\"presets\":[");
  const TfJson *pr = tf_json_get(m->calib, "presets");
  for (int i = 0; i < 4; ++i) {
    const TfJson *p = tf_json_get(pr, kPresets[i]);
    if (i) tf_buf_puts(&b, ",");
    tf_buf_puts(&b, "{\"name\":");
    tf_buf_json_str(&b, kPresets[i]);
    tf_buf_puts(&b, ",\"label\":");
    tf_buf_json_str(&b, tf_json_str(tf_json_get(p, "label")) ? tf_json_str(tf_json_get(p, "label")) : kPresets[i]);
    tf_buf_puts(&b, ",\"site_fa\":");
    const TfJson *fa = tf_json_get(p, "site_fa");
    if (fa && fa->type == TJ_NUM) tf_buf_num(&b, fa->num);
    else tf_buf_puts(&b, "null");
    tf_buf_puts(&b, "}");
  }
  tf_buf_puts(&b, "],\"word_presets\":");
  const TfJson *wc = word_calibration(m);
  const TfJson *wp = wc ? tf_json_get(wc, "presets") : NULL;
  if (wp && wp->type == TJ_OBJ) {
    tf_buf_puts(&b, "[");
    for (int i = 0; i < wp->n; ++i) {
      double t = NAN;
      word_preset_threshold(wc, wp->kids[i].key, &t);
      if (i) tf_buf_puts(&b, ",");
      tf_buf_puts(&b, "{\"name\":");
      tf_buf_json_str(&b, wp->kids[i].key);
      tf_buf_puts(&b, ",\"threshold\":");
      tf_buf_num(&b, t);
      tf_buf_puts(&b, "}");
    }
    tf_buf_puts(&b, "]");
  } else {
    tf_buf_puts(&b, "[]");
  }
  tf_buf_puts(&b, ",\"heads\":[");
  for (int h = 0; h < TF_HEAD_COUNT; ++h) {
    const TfHeadInfo *hi = tf_head_info(h);
    if (h) tf_buf_puts(&b, ",");
    tf_buf_puts(&b, "{\"name\":");
    tf_buf_json_str(&b, hi->name);
    tf_buf_printf(&b, ",\"width\":%d,\"rate_ms\":%d,\"kind\":", hi->width, hi->rate_ms);
    tf_buf_json_str(&b, hi->kind);
    tf_buf_puts(&b, ",\"classes\":[");
    for (int k = 0; k < hi->n_classes; ++k) {
      if (k) tf_buf_puts(&b, ",");
      tf_buf_json_str(&b, hi->classes[k]);
    }
    tf_buf_puts(&b, "]}");
  }
  tf_buf_puts(&b, "],\"sdk_version\":\"" TF_SDK_VERSION "\"}");
  m->info_json = tf_buf_take(&b);
  return m->info_json;
}

int tf__model_sample_rate(const TfModel *m) { return m ? m->sample_rate : 0; }
