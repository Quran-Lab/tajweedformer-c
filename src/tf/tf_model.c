/* The Model: one verified model build on this device. The signed manifest (format 3 with its `encryption` line,
 * PROTECTED_MODEL.md 18.2.5), its version and build, the tokens, the calibration as read-only data, the sample clips,
 * and the engine, opened for one context and input rate at a time.
 *
 * SDK 4.0: the engine, the formats and the keys are inside the closed Core. The Model hands the Core the signed
 * manifest and the context; the Core reads the device forms it installed, checks the lease, and keeps the plaintext
 * in its own memory. Nothing in this file opens a model file other than the public ones (the manifest, the
 * calibration, the sample clips). The vocabulary is encrypted with the weights, so the names of the tokens come from
 * the table below, chosen by the signed manifest's tokens_sha256 (a build with another vocabulary needs a newer SDK).
 * The expected units of each ayah (the reference data, role `reference`, encrypted) are read by the Core
 * (qlc_reference_expected, R3) from the open engine; tf_reference.c asks it. Nothing here scores. */
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

#define MAX_CONTEXTS 8
#define MAX_CLIPS 16

typedef struct {
  char name[48], kind[16], file[96];
  int samples, sample_rate;
} Clip;

struct TfModel {
  char *dir;
  TfJson *manifest;
  char sha[65];            /* SHA-256 of manifest.json as it is on disk */
  char build[41];          /* its first 40 hex digits */
  char version[24];
  char id[40];
  int format;              /* manifest format: 2 (a folder per context) or 3 (shared weights) */
  int is_signed;
  char key_id[17];
  char *tokens[TF_VOCAB];
  const char *names[TF_VOCAB];
  TfTokens vocab;
  char *calib;             /* calibration.json as shipped, NUL-terminated; NULL when the folder has none */
  size_t calib_n;
  char calib_sha[65];
  char calib_format[64];
  TfJson *calib_json;      /* parsed on the first tf_model_calibration_number */
  char contexts[MAX_CONTEXTS][32];
  int n_contexts;
  char default_context[32];
  char warm_context[32];   /* the context in force when the Model was opened (the config's, or the default) */
  Clip clips[MAX_CLIPS];
  int n_clips;
  int threads, batch, pin, skip_checks;
  TfClient *client;
  char *repo, *folder_path;
  /* the engine (in the Core): one context and one input rate at a time */
  TfPm *pm;                /* the Client's Core host (a reference: it outlives the Client while this Model is open) */
  char *mbytes;            /* manifest.json's bytes, as the Core takes them */
  size_t mn;
  qlc_model *zm;
  qlc_stream *zs;
  char eng_context[32];
  int eng_rate;
  double load_ms;
  TfSession *open;         /* the one open Session */
  TfReference ref;
  int has_ref;
  char *info_json;
};

const char *tf__model_dir(const TfModel *m) { return m->dir; }
TfClient *tf__model_client(const TfModel *m) { return m ? m->client : NULL; }
const TfJson *tf__model_manifest(const TfModel *m) { return m->manifest; }
const char *tf__model_default_context(const TfModel *m) { return m->default_context; }
qlc_model *tf__model_engine(TfModel *m) { return m ? m->zm : NULL; }
/* Which context a Session runs on (SPEC section 3; the vectors' inference_context.json): the app's choice, else
 * the config's inference.context when `inference` is an object, its `context` a string and one of the model's
 * contexts (anything else there is ignored; 3.0 never reads tajweed.context), else the manifest's default. Names
 * are compared exactly. */
int tf__resolve_context(const char *app, const char *config_json, const char *const *contexts, int n_contexts, const char *default_context,
                        char *out, size_t out_n, const char **source) {
  const char *pick = NULL, *src = "manifest";
  TfJson *j = NULL;
  if (app) {
    for (int i = 0; i < n_contexts; ++i)
      if (!strcmp(contexts[i], app)) pick = contexts[i];
    if (!pick) return tf__fail(TF_ERR_INVALID_ARGUMENT, "context %s is not one this model has", app);
    src = "app";
  } else if (config_json) {
    j = tf_json_parse(config_json, strlen(config_json), NULL);
    const TfJson *inf = tf_json_get(j, "inference");
    const char *c = inf && inf->type == TJ_OBJ ? tf_json_str(tf_json_get(inf, "context")) : NULL;
    for (int i = 0; c && i < n_contexts; ++i)
      if (!strcmp(contexts[i], c)) pick = contexts[i];
    if (pick) src = "config";
  }
  if (!pick) pick = default_context;
  snprintf(out, out_n, "%s", pick);
  if (source) *source = src;
  tf_json_free(j);
  return 0;
}
const char *tf__model_context_in_force(TfModel *m, char *out, size_t n) {
  const char *list[MAX_CONTEXTS];
  for (int i = 0; i < m->n_contexts; ++i) list[i] = m->contexts[i];
  tf__resolve_context(NULL, m->client ? tf__client_config_json(m->client) : NULL, list, m->n_contexts, m->default_context, out, n, NULL);
  return out;
}
int tf__model_has_context(const TfModel *m, const char *context) {
  for (int i = 0; context && i < m->n_contexts; ++i)
    if (!strcmp(m->contexts[i], context)) return 1;
  return 0;
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

/* cmp for tolerance entries: the build key's UTF-8 bytes (strcmp compares as unsigned char) */
static int cmp_key(const void *a, const void *b) {
  return strcmp((*(const TfJson *const *)a)->key, (*(const TfJson *const *)b)->key);
}

/* a build key of a tolerance: 1 to 64 printable ASCII characters (0x21 to 0x7E, so no space) */
static int build_key_ok(const char *k) {
  if (!k || !*k || strlen(k) > 64) return 0;
  for (const unsigned char *p = (const unsigned char *)k; *p; ++p)
    if (*p < 0x21 || *p > 0x7E) return 0;
  return 1;
}
static int is_count(const TfJson *v) { /* a number with an integer value 0 to 1,000,000 */
  return v && v->type == TJ_NUM && v->num >= 0 && v->num <= 1000000 && v->num == floor(v->num);
}
/* a tolerance number: lo to hi, with at most 6 digits after the dot (written with 6 decimals and parsed back, it
 * is the same double) */
static int is_decimal6(const TfJson *v, double lo, double hi) {
  if (!v || v->type != TJ_NUM || !(v->num >= lo && v->num <= hi)) return 0;
  char t[64];
  snprintf(t, sizeof t, "%.6f", v->num);
  return strtod(t, NULL) == v->num;
}
/* <num> of a golden_raw line: the value with 6 digits after the dot, trailing zeros removed but one digit kept */
static void put_num6(TfBuf *b, double v) {
  char t[64];
  snprintf(t, sizeof t, "%.6f", v == 0 ? 0.0 : v);
  size_t n = strlen(t);
  while (n > 2 && t[n - 1] == '0' && t[n - 2] != '.') t[--n] = 0;
  tf_buf_puts(b, t);
}

/* 1 when the manifest has a golden_tolerance field (not null): only then is the text without its lines a fallback */
int tf_manifest_has_tolerance(const TfJson *m) {
  const TfJson *gt = tf_json_get(m, "golden_tolerance");
  return gt && gt->type != TJ_NULL;
}
static int has_raw(const TfJson *m) {
  const TfJson *gt = tf_json_get(m, "golden_raw_tolerance");
  return gt && gt->type != TJ_NULL;
}
/* a well-formed golden_tolerance entry (2.x): its three counts are integers 0 to 1,000,000 */
static int tolerance_ok(const TfJson *e) {
  return e && e->type == TJ_OBJ && build_key_ok(e->key) && is_count(tf_json_get(e, "token_diffs")) && is_count(tf_json_get(e, "site_flag_diffs")) &&
         is_count(tf_json_get(e, "word_flag_diffs"));
}
/* A well-formed build of golden_raw_tolerance (the vectors README 4.2): an object (it may be empty) whose keys are
 * `attr` holding exactly {"p_mean"} (0 to 1) or a ctc head name (^[a-z][a-z0-9_]{0,31}$) holding exactly
 * {"token_edits", "lp_mean", "lp_p99"} (edits 0 to 1,000,000, distances 0 to 30); at most 6 decimals. One bad head
 * makes the whole build malformed: no signed line, no tolerance. */
int tf__golden_raw_build_ok(const TfJson *e) {
  if (!e || e->type != TJ_OBJ || !build_key_ok(e->key)) return 0;
  for (int i = 0; i < e->n; ++i) {
    const TfJson *h = &e->kids[i];
    if (h->type != TJ_OBJ) return 0;
    if (!strcmp(h->key, "attr")) {
      if (h->n != 1 || !is_decimal6(tf_json_get(h, "p_mean"), 0, 1)) return 0;
      continue;
    }
    if (!(h->key[0] >= 'a' && h->key[0] <= 'z') || strlen(h->key) > 32) return 0;
    for (const char *p = h->key + 1; *p; ++p)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    if (h->n != 3 || !is_count(tf_json_get(h, "token_edits")) || !is_decimal6(tf_json_get(h, "lp_mean"), 0, 30) || !is_decimal6(tf_json_get(h, "lp_p99"), 0, 30))
      return 0;
    for (int k = 0; k < i; ++k)
      if (!strcmp(e->kids[k].key, h->key)) return 0;
  }
  return 1;
}

/* the header lines: each field in order; `version x.y.z` right after `revision`, only when the manifest has the
 * field (SPEC 2a), so a manifest without it keeps the 2.0.x text byte for byte */
static void header_lines(TfBuf *b, const TfJson *m, const char *const *fields, size_t n, TfBuf *num) {
  for (size_t i = 0; i < n; ++i) {
    tf_buf_printf(b, "%s %s\n", fields[i], field_str(m, fields[i], num));
    if (!strcmp(fields[i], "revision") && tf_json_get(m, "version")) tf_buf_printf(b, "version %s\n", field_str(m, "version", num));
  }
}

/* the entries of a tolerance object that pass `ok`, sorted by their key's bytes; caller frees */
static const TfJson **sorted_entries(const TfJson *obj, int (*ok)(const TfJson *), int *count) {
  *count = 0;
  if (!obj || obj->type != TJ_OBJ || obj->n <= 0) return NULL;
  const TfJson **keys = (const TfJson **)malloc(sizeof(TfJson *) * (size_t)obj->n);
  if (!keys) return NULL;
  for (int i = 0; i < obj->n; ++i)
    if (obj->kids[i].key && ok(&obj->kids[i])) keys[(*count)++] = &obj->kids[i];
  qsort(keys, (size_t)*count, sizeof *keys, cmp_key);
  return keys;
}

/* The exact text the signature covers (the vectors README 4.1; make_manifest.py signed_text for format 2,
 * make_manifest3.py signed_text for format 3). Format 3: after the header lines and before the first file line,
 * with `with_tolerance` one line per well-formed golden_tolerance build
 *   golden_tolerance <build> <token_diffs> <site_flag_diffs> <word_flag_diffs>
 * and with `with_raw` the lines of every well-formed golden_raw_tolerance build, heads sorted by their bytes
 *   golden_raw <build> <head> token_edits=<int> lp_mean=<num> lp_p99=<num>
 *   golden_raw <build> attr p_mean=<num>
 * both sorted by the build key's bytes; none when the field is absent. */
char *tf__manifest_text(const TfJson *m, int with_tolerance, int with_raw) {
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
    header_lines(&b, m, f2, sizeof f2 / sizeof f2[0], &num);
  } else if (format == 3) {
    static const char *const f3[] = {"model_id", "family", "revision", "min_sdk_version", "engine_format", "coreml_format",
                                     "tokens", "tokens_sha256", "default_context"};
    tf_buf_puts(&b, "quranlab-model-manifest 3\n");
    header_lines(&b, m, f3, sizeof f3 / sizeof f3[0], &num);
    tf_buf_puts(&b, "contexts ");
    const TfJson *cx = tf_json_get(m, "contexts");
    for (int i = 0; cx && cx->type == TJ_ARR && i < cx->n; ++i) tf_buf_printf(&b, "%s%s", i ? "," : "", tf_json_str(&cx->kids[i]) ? cx->kids[i].str : "");
    tf_buf_puts(&b, "\n");
    /* 18.2.5: an encrypted build signs one more header line, right after contexts */
    const TfJson *ec = tf_json_get(m, "encryption");
    if (ec && ec->type == TJ_OBJ) {
      TfBuf rn;
      tf_buf_init(&rn);
      tf_buf_printf(&b, "encryption %s %s ", field_str(ec, "scheme", &rn), field_str(ec, "key_id", &rn));
      tf_buf_printf(&b, "%s\n", field_str(ec, "root", &rn));
      tf_buf_free(&rn);
    }
    if (with_tolerance) {
      int nk = 0;
      const TfJson **keys = sorted_entries(tf_json_get(m, "golden_tolerance"), tolerance_ok, &nk);
      for (int i = 0; i < nk; ++i)
        tf_buf_printf(&b, "golden_tolerance %s %.0f %.0f %.0f\n", keys[i]->key, tf_json_get(keys[i], "token_diffs")->num,
                      tf_json_get(keys[i], "site_flag_diffs")->num, tf_json_get(keys[i], "word_flag_diffs")->num);
      free(keys);
    }
    if (with_raw) {
      int nk = 0;
      const TfJson **keys = sorted_entries(tf_json_get(m, "golden_raw_tolerance"), tf__golden_raw_build_ok, &nk);
      for (int i = 0; i < nk; ++i) {
        const TfJson *e = keys[i];
        const TfJson **heads = (const TfJson **)malloc(sizeof(TfJson *) * (size_t)(e->n + 1));
        if (!heads) continue;
        for (int k = 0; k < e->n; ++k) heads[k] = &e->kids[k];
        qsort(heads, (size_t)e->n, sizeof *heads, cmp_key);
        for (int k = 0; k < e->n; ++k) {
          const TfJson *h = heads[k];
          tf_buf_printf(&b, "golden_raw %s %s ", e->key, h->key);
          if (!strcmp(h->key, "attr")) {
            tf_buf_puts(&b, "p_mean=");
            put_num6(&b, tf_json_get(h, "p_mean")->num);
          } else {
            tf_buf_printf(&b, "token_edits=%.0f lp_mean=", tf_json_get(h, "token_edits")->num);
            put_num6(&b, tf_json_get(h, "lp_mean")->num);
            tf_buf_puts(&b, " lp_p99=");
            put_num6(&b, tf_json_get(h, "lp_p99")->num);
          }
          tf_buf_puts(&b, "\n");
        }
        free(heads);
      }
      free(keys);
    }
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
char *tf_manifest_signed_text(const TfJson *m) { return tf__manifest_text(m, 1, 1); }
char *tf_manifest_signed_text_ex(const TfJson *m, int with_tolerance) { return tf__manifest_text(m, with_tolerance, with_tolerance); }

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

/* A version (SPEC 2a; the same rule for a model version and for min_sdk_version): x.y.z, digits only, no leading
 * zeros, no suffix, at most 4, 4 and 6 digits. Anything else is not a version: refused, never ordered. */
static int parse_version3(const char *s, long part[3]) {
  static const int max_digits[3] = {4, 4, 6};
  if (!s) return 0;
  for (int k = 0; k < 3; ++k) {
    int digits = 0;
    const char *start = s;
    long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'), ++digits;
    if (digits < 1 || digits > max_digits[k] || (digits > 1 && *start == '0')) return 0;
    part[k] = v;
    if (k < 2) {
      if (*s != '.') return 0;
      ++s;
    }
  }
  return *s == 0;
}
int tf__is_model_version(const char *s) {
  long p[3];
  return parse_version3(s, p);
}
/* -1, 0 or 1 by the parts as integers; -2 when either is not a version */
int tf__version_cmp(const char *a, const char *b) {
  long x[3], y[3];
  if (!parse_version3(a, x) || !parse_version3(b, y)) return -2;
  for (int i = 0; i < 3; ++i)
    if (x[i] != y[i]) return x[i] > y[i] ? 1 : -1;
  return 0;
}
/* 1 when dotted version v is min or newer (lenient: "2", "2.0"; the signed config's sdk fields) */
int tf__version_at_least(const char *v, const char *min) {
  int a[3] = {0}, b[3] = {0};
  if (!v || !min || sscanf(v, "%d.%d.%d", &a[0], &a[1], &a[2]) < 1 || sscanf(min, "%d.%d.%d", &b[0], &b[1], &b[2]) < 1) return 0;
  for (int i = 0; i < 3; ++i)
    if (a[i] != b[i]) return a[i] > b[i];
  return 1;
}

static int verify_text(const char *text, const uint8_t sig[64], const uint8_t pk[32]) {
  return text && tf_ed25519_verify(sig, (const uint8_t *)text, strlen(text), pk);
}

/* Verify the signature with `key_hex` (64 hex). 1 ok, 0 bad. Up to four texts are tried, in this order; the first
 * on which the signature verifies decides (the vectors README 4.3):
 *   1. with the golden_tolerance lines and the golden_raw lines;
 *   2. with the golden_tolerance lines only: golden_raw_tolerance counts as absent (*raw_signed 0);
 *   3. with the golden_raw lines only: the 2.x tolerance is unsigned (*tolerance_signed 0);
 *   4. without both.
 * A text equal to one already tried is skipped. So a tampered tolerance value is refused (the signature covered
 * its line, and the text without the lines is not the signed text), while a publisher that signed without the
 * lines gets a manifest that loads with no tolerance. This SDK runs the CPU engine only and never reads a
 * tolerance; the flags are for the tests. */
int tf__manifest_verify(const TfJson *m, const char *key_hex, int *tolerance_signed, int *raw_signed) {
  const char *sig64 = tf_json_str(tf_json_get(m, "signature"));
  uint8_t sig[66], pk[32];
  if (tolerance_signed) *tolerance_signed = 0;
  if (raw_signed) *raw_signed = 0;
  if (!sig64 || tf_base64_decode(sig64, sig, sizeof sig) != 64) return 0;
  if (tf_hex_decode(key_hex, pk, 32)) return 0;
  static const int with[4][2] = {{1, 1}, {1, 0}, {0, 1}, {0, 0}};
  char *tried[4] = {NULL, NULL, NULL, NULL};
  int ok = 0;
  for (int i = 0; i < 4 && !ok; ++i) {
    tried[i] = tf__manifest_text(m, with[i][0], with[i][1]);
    if (!tried[i]) break;
    int seen = 0;
    for (int k = 0; k < i; ++k) seen |= !strcmp(tried[k], tried[i]);
    if (seen) continue;
    if (verify_text(tried[i], sig, pk)) {
      ok = 1;
      if (tolerance_signed) *tolerance_signed = with[i][0] && tf_manifest_has_tolerance(m);
      if (raw_signed) *raw_signed = with[i][1] && has_raw(m) && tf_json_get(m, "golden_raw_tolerance")->type == TJ_OBJ;
    }
  }
  for (int i = 0; i < 4; ++i) free(tried[i]);
  return ok;
}
int tf_manifest_verify_signature_ex(const TfJson *m, const char *key_hex, int *tolerance_signed) {
  return tf__manifest_verify(m, key_hex, tolerance_signed, NULL);
}
int tf_manifest_verify_signature(const TfJson *m, const char *key_hex) { return tf__manifest_verify(m, key_hex, NULL, NULL); }

/* the manifest's shape (as 2.0.2), with `version` (when present) and `min_sdk_version` being versions */
static int check_shape(const TfJson *m) {
  if (!m || m->type != TJ_OBJ) return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json is not an object");
  const TfJson *fmt = tf_json_get(m, "format");
  if (!fmt || fmt->type != TJ_NUM || (fmt->num != 2 && fmt->num != 3))
    return tf__fail(TF_ERR_MODEL_INVALID, "manifest format %g is not one this SDK reads (2 or 3)", fmt && fmt->type == TJ_NUM ? fmt->num : -1.0);
  const int f3 = fmt->num == 3;
  static const char *const strs2[] = {"model_id", "family", "revision", "min_sdk_version", "tokens_sha256", "coreml", "sidecar", "keeps", "tokens"};
  static const char *const strs3[] = {"model_id", "family", "revision", "min_sdk_version", "tokens_sha256", "tokens", "default_context"};
  const char *const *strs = f3 ? strs3 : strs2;
  const size_t nstrs = f3 ? sizeof strs3 / sizeof strs3[0] : sizeof strs2 / sizeof strs2[0];
  for (size_t i = 0; i < nstrs; ++i)
    if (!tf_json_str(tf_json_get(m, strs[i])) || !*tf_json_get(m, strs[i])->str) return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json has no %s", strs[i]);
  if (!tf_json_is_int(tf_json_get(m, "engine_format")) || !tf_json_is_int(tf_json_get(m, "coreml_format")))
    return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json: engine_format and coreml_format are not integers");
  if (!tf__is_model_version(tf_json_str(tf_json_get(m, "min_sdk_version")))) return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json: min_sdk_version is not x.y.z");
  if (tf_json_get(m, "version") && !tf__is_model_version(tf_json_str(tf_json_get(m, "version"))))
    return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json: version is not x.y.z");
  const TfJson *files = tf_json_get(m, "files");
  if (!files || files->type != TJ_ARR || !files->n) return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json lists no files");
  for (int i = 0; i < files->n; ++i) {
    const TfJson *f = &files->kids[i];
    const char *p = tf_json_str(tf_json_get(f, "path"));
    uint8_t h[32];
    if (!safe_path(p)) return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json lists a path that is not inside the folder");
    const char *enc = tf_json_str(tf_json_get(f, "encoding"));
    if (enc && strcmp(enc, "identity")) {
      const TfJson *d = tf_json_get(f, "decoded");
      uint8_t dh[32];
      if ((strcmp(enc, "gzip") && strcmp(enc, "qle1")) || !d || !safe_path(tf_json_str(tf_json_get(d, "path"))) || !tf_json_is_int(tf_json_get(d, "size")) ||
          tf_hex_decode(tf_json_str(tf_json_get(d, "sha256")), dh, 32))
        return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json: the encoding of %s", p);
    }
    if (!tf_json_is_int(tf_json_get(f, "size")) || tf_json_get(f, "size")->num < 0) return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json: the size of %s", p);
    if (tf_hex_decode(tf_json_str(tf_json_get(f, "sha256")), h, 32)) return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json: the sha256 of %s", p);
    for (int k = 0; k < i; ++k)
      if (!strcmp(p, tf_json_str(tf_json_get(&files->kids[k], "path")))) return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json lists %s twice", p);
  }
  return 0;
}

/* The acceptance checks of a manifest, in order; the first that fails decides the code (the vectors README 4.4):
 *   1. a config named the build: the SHA-256 of the bytes starts with it, else checksum_mismatch;
 *   2. the bytes are a JSON object of the manifest's shape, else model_invalid;
 *   3. the signature verifies with the trusted key (unsigned: the same), else signature_invalid;
 *   4. the manifest has `version`, else model_incompatible (a 2.x build);
 *   5. min_sdk_version is not above this SDK's version (and it is a TajweedFormer build of the engine format that
 *      goes with the manifest format), else model_incompatible;
 *   6. a config named the version: the manifest's is that one, else model_invalid.
 * A folder opened without a config skips 1 and 6. Returns 0 and the parsed manifest in *out (the caller frees),
 * or the TfError. sha_hex receives the SHA-256 of the bytes. */
int tf__manifest_accept(const char *bytes, size_t n, const char *want_build, const char *want_version, const char *key_hex, int allow_unsigned,
                        TfJson **out, char sha_hex[65], int *tolerance_signed, int *raw_signed) {
  uint8_t h[32];
  char sha[65];
  tf_sha256(bytes, n, h);
  tf_hex_encode(h, 32, sha);
  if (sha_hex) memcpy(sha_hex, sha, 65);
  if (out) *out = NULL;
  if (tolerance_signed) *tolerance_signed = 0;
  if (raw_signed) *raw_signed = 0;
  if (want_build && (strlen(want_build) != 40 || strncmp(sha, want_build, 40))) return tf__fail(TF_ERR_CHECKSUM_MISMATCH, "manifest.json (not the build the signed config named)");
  const char *err = NULL;
  TfJson *m = tf_json_parse(bytes, n, &err);
  if (!m) return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json is not JSON (%s)", err ? err : "?");
  int rc = check_shape(m);
  if (!rc) {
    if (tf_json_get(m, "signature")) {
      if (!tf__manifest_verify(m, key_hex, tolerance_signed, raw_signed)) rc = tf__fail(TF_ERR_SIGNATURE_INVALID, NULL);
    } else if (!allow_unsigned) {
      rc = tf__fail(TF_ERR_SIGNATURE_INVALID, NULL);
    }
  }
  if (!rc && !tf_json_get(m, "version")) rc = tf__fail(TF_ERR_MODEL_INCOMPATIBLE, "the manifest names no model version (a build for SDK 2)");
  /* SDK 4.0 runs encrypted builds only (18.2.5): {"scheme":"qle1","key_id":<32 hex>,"root":1..9} */
  if (!rc) {
    const TfJson *ec = tf_json_get(m, "encryption");
    const char *sc = tf_json_str(tf_json_get(ec, "scheme")), *kid = tf_json_str(tf_json_get(ec, "key_id"));
    const TfJson *root = tf_json_get(ec, "root");
    uint8_t k16[16];
    if (!ec || ec->type != TJ_OBJ || !sc || strcmp(sc, "qle1") || !kid || strlen(kid) != 32 || tf_hex_decode(kid, k16, 16) ||
        !tf_json_is_int(root) || root->num < 1 || root->num > 9)
      rc = tf__fail(TF_ERR_MODEL_INCOMPATIBLE, "the build is not encrypted: SDK 4 runs encrypted builds only");
  }
  if (!rc && tf__version_cmp(tf_json_str(tf_json_get(m, "min_sdk_version")), TF_SDK_VERSION) > 0)
    rc = tf__fail(TF_ERR_MODEL_INCOMPATIBLE, "%s needs SDK %s or later (this is %s)", tf_json_str(tf_json_get(m, "model_id")),
                  tf_json_str(tf_json_get(m, "min_sdk_version")), TF_SDK_VERSION);
  if (!rc && strcmp(tf_json_str(tf_json_get(m, "family")), "tajweedformer"))
    rc = tf__fail(TF_ERR_MODEL_INCOMPATIBLE, "family %s is not a TajweedFormer build", tf_json_str(tf_json_get(m, "family")));
  if (!rc && tf_json_get(m, "engine_format")->num != (tf_json_get(m, "format")->num == 3 ? 3 : 2))
    rc = tf__fail(TF_ERR_MODEL_INCOMPATIBLE, "engine format %g does not go with manifest format %g", tf_json_get(m, "engine_format")->num,
                  tf_json_get(m, "format")->num);
  if (!rc && want_version && strcmp(want_version, tf_json_str(tf_json_get(m, "version"))))
    rc = tf__fail(TF_ERR_MODEL_INVALID, "the manifest is version %s, the signed config named %s", tf_json_str(tf_json_get(m, "version")), want_version);
  if (rc || !out) tf_json_free(m);
  else *out = m;
  return rc;
}

/* a file as the manifest names it on this device: the decoded entry of a gzip or qle1 entry (layout 3), else the
 * entry itself. A qle1 file is never on disk as plaintext: its decoded path names it to the Core. */
static const TfJson *local_of(const TfJson *f) {
  const char *enc = tf_json_str(tf_json_get(f, "encoding"));
  return enc && (!strcmp(enc, "gzip") || !strcmp(enc, "qle1")) ? tf_json_get(f, "decoded") : f;
}
static int is_qle1(const TfJson *f) {
  const char *enc = tf_json_str(tf_json_get(f, "encoding"));
  return enc && !strcmp(enc, "qle1");
}
static const TfJson *listed(const TfJson *m, const char *path) {
  const TfJson *files = tf_json_get(m, "files");
  for (int i = 0; i < files->n; ++i)
    if (!strcmp(tf_json_str(tf_json_get(local_of(&files->kids[i]), "path")), path)) return local_of(&files->kids[i]);
  return NULL;
}
const TfJson *tf__model_listed(const TfModel *m, const char *path) { return listed(m->manifest, path); }
const char *tf__model_manifest_bytes(const TfModel *m, size_t *n) {
  if (n) *n = m ? m->mn : 0;
  return m ? m->mbytes : NULL;
}
TfPm *tf__model_pm(const TfModel *m) { return m ? m->pm : NULL; }

const char *tf__model_role_path(const TfModel *m, const char *role, const char *context) {
  const TfJson *files = tf_json_get(m->manifest, "files");
  for (int i = 0; i < files->n; ++i) {
    const TfJson *f = &files->kids[i];
    const char *r = tf_json_str(tf_json_get(f, "role")), *cx = tf_json_str(tf_json_get(f, "context"));
    if (!r || strcmp(r, role)) continue;
    if (context && (!cx || strcmp(cx, context))) continue;
    return tf_json_str(tf_json_get(local_of(f), "path"));
  }
  return NULL;
}

/* 0 when the file is on disk as the manifest lists it; TF_ERR_MODEL_MISSING when it is not there;
 * TF_ERR_CHECKSUM_MISMATCH when it differs */
static int verify_file(const char *dir, const TfJson *f) {
  f = local_of(f);
  const char *p = tf_json_str(tf_json_get(f, "path"));
  char *full = tf_path_join(dir, p);
  if (!full) return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  uint8_t h[32], want[32];
  long long size = 0;
  if (tf_sha256_file(full, h, &size)) {
    free(full);
    return tf__fail(TF_ERR_MODEL_MISSING, "%s is listed in the manifest and is not in the folder", p);
  }
  free(full);
  tf_hex_decode(tf_json_str(tf_json_get(f, "sha256")), want, 32);
  if (size != (long long)tf_json_get(f, "size")->num || memcmp(h, want, 32)) return tf__fail(TF_ERR_CHECKSUM_MISMATCH, "%s", p);
  return 0;
}

/* ------------------------------------------------------------------ open */

/* The vocabularies this SDK knows, by the SHA-256 of their tokens.txt (the signed manifest's tokens_sha256): the
 * names in id order. TajweedFormer V4's tj3 vocabulary (tokens.txt of every V4 build, 224 bytes): blank, the word
 * bar, the 29 consonant letters as UTF-8, the six vowels, and <foreign>. */
static const struct {
  const char *sha256;
  const char *names[TF_VOCAB];
} kVocabs[] = {
    {"66fe6eb81b15506270f9e3c6efb0724cda757cd54326f26ca4bcd6be4f3b2aa4",
     {"<blk>", "|", "\xd8\xa1", "\xd8\xa8", "\xd8\xaa", "\xd8\xab", "\xd8\xac", "\xd8\xad", "\xd8\xae", "\xd8\xaf",
      "\xd8\xb0", "\xd8\xb1", "\xd8\xb2", "\xd8\xb3", "\xd8\xb4", "\xd8\xb5", "\xd8\xb6", "\xd8\xb7", "\xd8\xb8",
      "\xd8\xb9", "\xd8\xba", "\xd9\x81", "\xd9\x82", "\xd9\x83", "\xd9\x84", "\xd9\x85", "\xd9\x86", "\xd9\x87",
      "\xd9\x88", "\xd9\x8a", "\xda\xba", "a", "i", "u", "aa", "ii", "uu", "<foreign>"}},
};

static int vocab_for(TfModel *m, const char *sha) {
  for (size_t v = 0; sha && v < sizeof kVocabs / sizeof kVocabs[0]; ++v) {
    if (_tf_strnicmp(sha, kVocabs[v].sha256, 65)) continue;
    for (int i = 0; i < TF_VOCAB; ++i) {
      m->tokens[i] = (char *)malloc(strlen(kVocabs[v].names[i]) + 1);
      if (!m->tokens[i]) return -1;
      strcpy(m->tokens[i], kVocabs[v].names[i]);
    }
    return 0;
  }
  return -1;
}

/* the contexts and the default one: the manifest's header (format 3); for a format 2 folder (one engine folder per
 * context, no header fields) the folders that list an engine graph, and c16_l64 or the first of them */
static int read_contexts(TfModel *m) {
  const TfJson *cx = tf_json_get(m->manifest, "contexts");
  if (m->format == 3) {
    for (int i = 0; cx && cx->type == TJ_ARR && i < cx->n && m->n_contexts < MAX_CONTEXTS; ++i)
      if (tf_json_str(&cx->kids[i]) && strlen(cx->kids[i].str) < sizeof m->contexts[0]) snprintf(m->contexts[m->n_contexts++], sizeof m->contexts[0], "%s", cx->kids[i].str);
    snprintf(m->default_context, sizeof m->default_context, "%s", tf_json_str(tf_json_get(m->manifest, "default_context")));
  } else {
    const TfJson *files = tf_json_get(m->manifest, "files");
    for (int i = 0; i < files->n && m->n_contexts < MAX_CONTEXTS; ++i) {
      const char *p = tf_json_str(tf_json_get(&files->kids[i], "path"));
      const char *sl = strchr(p, '/');
      if (!sl || strcmp(sl + 1, "model_engine2.onnx.zfg") || (size_t)(sl - p) >= sizeof m->contexts[0]) continue;
      snprintf(m->contexts[m->n_contexts++], sizeof m->contexts[0], "%.*s", (int)(sl - p), p);
    }
    const char *dc = tf_json_str(tf_json_get(m->manifest, "default_context"));
    snprintf(m->default_context, sizeof m->default_context, "%s", dc ? dc : tf__model_has_context(m, "c16_l64") || !m->n_contexts ? "c16_l64" : m->contexts[0]);
    if (!m->n_contexts) snprintf(m->contexts[m->n_contexts++], sizeof m->contexts[0], "%s", m->default_context);
  }
  if (!m->n_contexts || !tf__model_has_context(m, m->default_context)) return tf__fail(TF_ERR_MODEL_INVALID, "manifest.json: the default context is not one of its contexts");
  return 0;
}

/* the self-test clips (selftest/selftest.json, format quranlab.selftest/1): listed, verified, and only then read */
static void read_clips(TfModel *m) {
  const TfJson *e = listed(m->manifest, "selftest/selftest.json");
  if (!e || verify_file(m->dir, e)) {
    tf__clear_error();
    return;
  }
  char *full = tf_path_join(m->dir, "selftest/selftest.json"), *text = NULL;
  size_t n = 0;
  TfJson *j = full && !tf_read_file(full, &text, &n) ? tf_json_parse(text, n, NULL) : NULL;
  free(full);
  free(text);
  const TfJson *clips = tf_json_get(j, "clips");
  for (int i = 0; clips && clips->type == TJ_ARR && i < clips->n && m->n_clips < MAX_CLIPS; ++i) {
    const TfJson *c = &clips->kids[i];
    const char *id = tf_json_str(tf_json_get(c, "id")), *kind = tf_json_str(tf_json_get(c, "kind")), *file = tf_json_str(tf_json_get(c, "file"));
    const TfJson *ns = tf_json_get(c, "samples"), *sr = tf_json_get(c, "sample_rate");
    Clip *x = &m->clips[m->n_clips];
    if (!id || !file || !tf_json_is_int(ns) || !tf_json_is_int(sr) || sr->num < 8000 || sr->num > 48000 || strlen(id) >= sizeof x->name ||
        strchr(file, '/') || strchr(file, '\\') || snprintf(x->file, sizeof x->file, "selftest/%s", file) >= (int)sizeof x->file ||
        !listed(m->manifest, x->file))
      continue;
    snprintf(x->name, sizeof x->name, "%s", id);
    snprintf(x->kind, sizeof x->kind, "%s", kind ? kind : "clean");
    x->samples = (int)ns->num;
    x->sample_rate = (int)sr->num;
    m->n_clips++;
  }
  tf_json_free(j);
}

static void engine_close(TfModel *m);

int tf__client_model_release(TfClient *c, TfModel *m);
void tf__session_detach(TfSession *s);

void tf_model_close(TfModel *m) {
  if (!m) return;
  /* a Model the Client gave out more than once stays open until its last holder closes it */
  if (m->client && tf__client_model_release(m->client, m)) return;
  TfClient *const client = m->client;
  if (client) tf__client_session_close(client); /* 17.8: the Model's close ends the analytics session */
  engine_close(m);
  tf__reference_release(&m->ref);
  if (m->pm) tf__pm_release(m->pm);
  free(m->mbytes);
  tf_json_free(m->manifest);
  tf_json_free(m->calib_json);
  for (int i = 0; i < TF_VOCAB; ++i) free(m->tokens[i]);
  free(m->calib);
  free(m->dir);
  free(m->repo);
  free(m->folder_path);
  free(m->info_json);
  free(m);
  /* SPEC 15.3: a refusal arrived while this Model was open: its files go now */
  if (client) tf__client_purge_models(client);
}

static char *dup_s(const char *s) {
  if (!s) return NULL;
  char *p = (char *)malloc(strlen(s) + 1);
  if (p) strcpy(p, s);
  return p;
}

TfModel *tf__model_open(const char *folder, const TfModelOpen *opt) {
  tf__clear_error();
  TfModelOpen o;
  memset(&o, 0, sizeof o);
  if (opt) o = *opt;
  if (!folder || !*folder) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "no model folder was given");
    return NULL;
  }
  TfModel *m = (TfModel *)calloc(1, sizeof *m);
  if (!m) {
    tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
    return NULL;
  }
  m->dir = dup_s(folder);
  m->threads = o.threads;
  m->batch = o.batch;
  m->pin = o.pin;
  m->skip_checks = o.skip_file_checks;
  m->client = o.client;
  m->repo = dup_s(o.repo);
  m->folder_path = dup_s(o.folder_path);
  m->ref.m = m;
  const char *key = o.trusted_key_hex ? o.trusted_key_hex : TF_QURANLAB_KEY_HEX;
  {
    uint8_t pk[32], h[32];
    if (tf_hex_decode(key, pk, 32)) {
      tf__fail(TF_ERR_INVALID_ARGUMENT, "trusted_key_hex is not 64 hex digits");
      goto fail;
    }
    tf_sha256(pk, 32, h);
    tf_hex_encode(h, 8, m->key_id);
  }
  /* the manifest: its bytes name the build; its signed text names the version */
  {
    char *full = tf_path_join(folder, "manifest.json"), *text = NULL;
    size_t n = 0;
    if (!full || tf_read_file(full, &text, &n)) {
      free(full);
      tf__fail(TF_ERR_MODEL_MISSING, "%s has no manifest.json", folder);
      goto fail;
    }
    free(full);
    const int rc = tf__manifest_accept(text, n, o.want_build, o.want_version, key, o.allow_unsigned, &m->manifest, m->sha, NULL, NULL);
    if (rc) {
      free(text);
      goto fail;
    }
    m->mbytes = text; /* the Core verifies the same bytes again with its own pinned keys */
    m->mn = n;
    m->is_signed = tf_json_get(m->manifest, "signature") != NULL;
    snprintf(m->build, sizeof m->build, "%.40s", m->sha);
  }
  m->format = (int)tf_json_get(m->manifest, "format")->num;
  snprintf(m->version, sizeof m->version, "%s", tf_json_str(tf_json_get(m->manifest, "version")));
  snprintf(m->id, sizeof m->id, "%s", tf_json_str(tf_json_get(m->manifest, "model_id")));
  if (read_contexts(m)) goto fail;
  /* the tokens: the build's vocabulary, by its signed hash (its file is encrypted and read by the Core only) */
  {
    if (vocab_for(m, tf_json_str(tf_json_get(m->manifest, "tokens_sha256")))) {
      tf__fail(TF_ERR_MODEL_INCOMPATIBLE, "the build's vocabulary is not one this SDK knows: a newer SDK is needed");
      goto fail;
    }
    m->vocab.count = TF_VOCAB;
    m->vocab.blank = TF_BLANK;
    m->vocab.word_bar = -1;
    for (int i = 0; i < TF_VOCAB; ++i) {
      m->names[i] = m->tokens[i];
      if (!strcmp(m->tokens[i], "|")) m->vocab.word_bar = i;
    }
    m->vocab.names = m->names;
  }
  /* the Core: the Client's host, held while this Model lives */
  m->pm = o.client ? tf__pm(o.client) : NULL;
  if (!m->pm) {
    if (!tf__error_code()) tf__fail(TF_ERR_STORAGE_FAILED, "no install key store");
    goto fail;
  }
  tf__pm_retain(m->pm);
  /* the calibration: data. Read when the manifest lists it and it is in the folder (then it must be the listed
   * bytes); a folder without it opens and the accessor gives NULL (SPEC section 4). */
  {
    const char *cp = tf__model_role_path(m, "calibration", NULL);
    if (!cp && listed(m->manifest, "calibration.json")) cp = "calibration.json";
    const TfJson *ce = cp ? listed(m->manifest, cp) : NULL;
    char *full = ce ? tf_path_join(folder, cp) : NULL;
    if (full && tf_file_exists(full)) {
      if (verify_file(folder, ce) || tf_read_file(full, &m->calib, &m->calib_n)) {
        free(full);
        if (!tf__error_code()) tf__fail(TF_ERR_STORAGE_FAILED, "cannot read %s", cp);
        goto fail;
      }
      snprintf(m->calib_sha, sizeof m->calib_sha, "%s", tf_json_str(tf_json_get(ce, "sha256")));
      for (char *q = m->calib_sha; *q; ++q)
        if (*q >= 'A' && *q <= 'F') *q = (char)(*q - 'A' + 'a');
      /* its "format" string, for the accessor (the file is handed over whatever it says) */
      TfJson *cj = tf_json_parse(m->calib, m->calib_n, NULL);
      const char *f = tf_json_str(tf_json_get(cj, "format"));
      snprintf(m->calib_format, sizeof m->calib_format, "%s", f ? f : "");
      m->calib_json = cj;
    }
    free(full);
  }
  read_clips(m);
  /* the reference data: an encrypted entry of role `reference` (downloaded only when the app asks, read by the Core) */
  m->ref.m = m;
  m->has_ref = tf__model_role_entry(m, "reference") != NULL;
  /* the context to warm: the caller's, else the one the next Session uses with no choice by the app */
  if (o.context) {
    if (!tf__model_has_context(m, o.context)) {
      tf__fail(TF_ERR_INVALID_ARGUMENT, "context %s is not one this model has", o.context);
      goto fail;
    }
    snprintf(m->warm_context, sizeof m->warm_context, "%s", o.context);
  } else {
    tf__model_context_in_force(m, m->warm_context, sizeof m->warm_context);
  }
  if (!o.no_engine && !tf__model_stream(m, m->warm_context, o.sample_rate > 0 ? o.sample_rate : 16000)) goto fail;
  return m;
fail:
  tf_model_close(m);
  return NULL;
}

TfModel *tf__model_open_folder(const char *folder, const TfModelOptions *options, const char *trusted_key_hex) {
  TfModelOpen o;
  memset(&o, 0, sizeof o);
  if (tf__runtime_check()) {
    tf__raise(-1);
    return NULL;
  }
  if (options) {
    if (options->version) {
      tf__fail(TF_ERR_INVALID_ARGUMENT, "a version pin goes to tf_client_model; a folder holds one build");
      return NULL;
    }
    o.threads = options->threads;
    o.pin = options->pin;
  }
  o.trusted_key_hex = trusted_key_hex;
  return tf__model_open(folder, &o);
}

/* ------------------------------------------------------------------ the engine */

/* The engine allows one live stream per process (qlcore.h: "one stream may be live per process"). A Model keeps its
 * stream between takes so the next Session starts at once; when another Model needs a stream, the idle one gives
 * its own up. */
static TfModel *g_stream_owner;
static volatile char g_stream_lock;
static void owner_lock(void) {
  while (__atomic_test_and_set(&g_stream_lock, __ATOMIC_ACQUIRE)) {
  }
}
static void owner_unlock(void) { __atomic_clear(&g_stream_lock, __ATOMIC_RELEASE); }

static void stream_close(TfModel *m) {
  owner_lock();
  if (m->zs) qlc_stream_destroy(m->zs);
  m->zs = NULL;
  if (g_stream_owner == m) g_stream_owner = NULL;
  owner_unlock();
}
static void engine_close(TfModel *m) {
  stream_close(m);
  if (m->zm) tf__pm_close(m->pm, m->zm);
  m->zm = NULL;
  m->eng_context[0] = 0;
  m->eng_rate = 0;
}

/* the context's files: every encrypted entry of the context (and the shared ones) installed as a device form. A
 * file that is not here yet is fetched and installed when the Model came from a Client (another context's files are
 * downloaded on first use; the Core installs only while none of its models is open). */
static int engine_files(TfModel *m, const char *context) {
  const TfJson *files = tf_json_get(m->manifest, "files");
  for (int pass = 0; pass < 2; ++pass) {
    int missing = 0;
    for (int i = 0; i < files->n; ++i) {
      const TfJson *f = &files->kids[i];
      const char *cx = tf_json_str(tf_json_get(f, "context")), *need = tf_json_str(tf_json_get(f, "need"));
      if (!is_qle1(f) || !need || strcmp(need, "required") || (cx && strcmp(cx, "*") && strcmp(cx, context))) continue;
      const char *soc = tf_json_str(tf_json_get(f, "soc"));
      if (soc && strcmp(soc, "*")) continue;
      char *dev = tf__plat_path(tf__pm_plat(m->pm), QLC_FORM_DEVICE, tf_json_str(tf_json_get(local_of(f), "path")));
      /* the device form's place belongs to the build in use */
      tf__plat_set_build(tf__pm_plat(m->pm), m->dir);
      free(dev);
      dev = tf__plat_path(tf__pm_plat(m->pm), QLC_FORM_DEVICE, tf_json_str(tf_json_get(local_of(f), "path")));
      const int here = dev && tf_file_exists(dev);
      free(dev);
      if (!here) missing = 1;
    }
    if (!missing) return 0;
    if (pass || !m->client) return tf__fail(TF_ERR_MODEL_MISSING, "the files of %s are not on this device", context);
    const int rc = tf__store_fetch_role(m->client, m, context, NULL);
    if (rc) return rc;
  }
  return 0;
}

qlc_stream *tf__model_stream(TfModel *m, const char *context, int rate) {
  if (!m || !context) return NULL;
  if (rate <= 0) rate = 16000;
  if (rate != 16000 && (rate < 8000 || rate > 48000)) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "sample rate %d is not 16000 or one from 8000 to 48000", rate);
    return NULL;
  }
  if (!tf__model_has_context(m, context)) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "context %s is not one this model has", context);
    return NULL;
  }
  if (m->open) {
    tf__fail(TF_ERR_SESSION_OPEN, NULL);
    return NULL;
  }
  /* 4.0.1: the threads the Core makes for the engine (at its open and the stream's) are never power-throttled */
  TfThreadSnap before;
  const int making = !m->zm || strcmp(m->eng_context, context) || m->eng_rate != rate || !m->zs;
  if (making) tf__qos_snapshot(&before);
  if (!m->zm || strcmp(m->eng_context, context) || m->eng_rate != rate) {
    engine_close(m);
    if (engine_files(m, context)) return NULL;
    /* 18.4.7.1: past the lease's exp the open makes a lease attempt first (with no network too: unreachable), and
     * opens in the lease's grace when the Core allows it; a valid lease returns at once */
    if (m->client && tf__pm_lease(m->client, m->pm, m->mbytes, m->mn, m->manifest, m->build)) return NULL;
    const double t0 = tf__now_s();
    m->zm = tf__pm_open(m->pm, m->dir, m->mbytes, m->mn, context, m->threads, rate, m->pin);
    if (!m->zm) return NULL;
    snprintf(m->eng_context, sizeof m->eng_context, "%s", context);
    m->eng_rate = rate;
    m->load_ms = (tf__now_s() - t0) * 1000.0;
    /* the recited index (17.1) goes with the model: read by the Core from its device form when it is installed */
    if (m->client) tf__recited_index(m->client, m->pm, m->dir, m->mbytes, m->mn);
  }
  if (!m->zs) {
    owner_lock();
    TfModel *other = g_stream_owner;
    if (other && other != m) {
      if (other->open) {
        owner_unlock();
        tf__fail(TF_ERR_SESSION_OPEN, NULL);
        return NULL;
      }
      qlc_stream_destroy(other->zs);
      other->zs = NULL;
      g_stream_owner = NULL;
    }
    int st = 0;
    m->zs = qlc_stream_create(m->zm, &st);
    if (m->zs) g_stream_owner = m;
    owner_unlock();
    if (!m->zs) {
      tf__pm_error(st, 0);
      return NULL;
    }
  }
  if (making) tf__qos_mark_new(&before);
  return m->zs;
}

/* the Client is going away: the Model (and its open take) carry on without telemetry, sharing or downloads */
void tf__model_detach(TfModel *m) {
  if (m->open) tf__session_detach(m->open);
  m->client = NULL;
}
/* the context's files, now, without fetching anything: 0 when every one is installed (the Core checks their tags
 * when it opens them) */
int tf__model_verify(TfModel *m, const char *context) {
  TfClient *c = m->client;
  const int skip = m->skip_checks;
  m->client = NULL;
  m->skip_checks = 0;
  const int rc = engine_files(m, context);
  m->client = c;
  m->skip_checks = skip;
  return rc;
}

int tf__model_set_session(TfModel *m, TfSession *s) {
  if (m->open && m->open != s) return -1;
  m->open = s;
  return 0;
}
void tf__model_clear_session(TfModel *m, TfSession *s) {
  if (m && m->open == s) m->open = NULL;
}
/* A take's Session is destroyed. With pinning asked for, the engine pins the calling thread while its stream lives:
 * the stream is given back now (no other take being open), so the caller's thread has its own affinity between
 * takes, and the next Session makes a new stream. Without pinning the stream stays for the next take. */
void tf__model_session_gone(TfModel *m) {
  if (m && m->pin > 0 && !m->open) stream_close(m);
}
double tf__model_load_ms(TfModel *m) {
  const double v = m->load_ms;
  m->load_ms = 0;
  return v;
}

/* ------------------------------------------------------------------ accessors */

const char *tf_model_id(const TfModel *m) { return m ? m->id : NULL; }
const char *tf_model_version(const TfModel *m) { return m ? m->version : NULL; }
const char *tf_model_build(const TfModel *m) { return m ? m->build : NULL; }

/* a context's chunk: its name says how many 20 ms frames a chunk holds ("c16_l64": 16, so 320 ms) */
int tf_model_chunk_ms(const TfModel *m, const char *context) {
  if (!m) return 0;
  if (!context) context = tf__model_default_context(m);
  int frames = 0;
  if (!tf__model_has_context(m, context) || sscanf(context, "c%d_", &frames) != 1 || frames < 1 || frames > 500) return 0;
  return 20 * frames;
}

const TfTokens *tf_model_tokens(const TfModel *m) { return m ? &m->vocab : NULL; }
int tf_model_token_count(const TfModel *m) { return m ? TF_VOCAB : 0; }
const char *tf_model_token_name(const TfModel *m, int id) { return m && id >= 0 && id < TF_VOCAB ? m->tokens[id] : NULL; }
int tf_model_token_id(const TfModel *m, const char *name) {
  for (int i = 0; m && name && i < TF_VOCAB; ++i)
    if (!strcmp(m->tokens[i], name)) return i;
  return -1;
}

const char *tf_model_calibration_json(const TfModel *m) { return m ? m->calib : NULL; }
int tf_model_calibration(const TfModel *m, TfCalibration *out) {
  if (!m || !out) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no model");
  memset(out, 0, sizeof *out);
  if (!m->calib) return 0;
  out->json = m->calib;
  out->size = m->calib_n;
  out->format = m->calib_format;
  out->sha256 = m->calib_sha;
  return 1;
}
int tf_model_calibration_number(const TfModel *m, const char *path, double *out) {
  if (!m || !path || !m->calib_json) return 0;
  const TfJson *j = m->calib_json;
  for (const char *s = path; j;) {
    const char *e = strchr(s, '.');
    char key[96];
    const size_t n = e ? (size_t)(e - s) : strlen(s);
    if (!n || n >= sizeof key) return 0;
    memcpy(key, s, n);
    key[n] = 0;
    j = tf_json_get(j, key);
    if (!e) break;
    s = e + 1;
  }
  if (!j || j->type != TJ_NUM) return 0;
  if (out) *out = j->num;
  return 1;
}

int tf_model_sample_clip_count(const TfModel *m) { return m ? m->n_clips : 0; }
int tf_model_sample_clip(const TfModel *m, int i, TfClip *out) {
  if (!m || !out || i < 0 || i >= m->n_clips) return tf__fail(TF_ERR_INVALID_ARGUMENT, "the model has no sample clip %d", i);
  out->name = m->clips[i].name;
  out->kind = m->clips[i].kind;
  out->duration_ms = (int)(1000LL * m->clips[i].samples / m->clips[i].sample_rate);
  return 0;
}
/* a clip's samples (16-bit PCM in the folder, verified against the manifest before it is read) as floats; the
 * caller frees. Returns the count, or a negative TfError. */
int tf__model_clip_samples(const TfModel *m, const char *name, float **out, int *rate) {
  const Clip *c = NULL;
  for (int i = 0; m && name && i < m->n_clips; ++i)
    if (!strcmp(m->clips[i].name, name)) c = &m->clips[i];
  if (!c) return tf__fail(TF_ERR_INVALID_ARGUMENT, "%s is not a sample clip of this model", name ? name : "(no clip)");
  const TfJson *e = listed(m->manifest, c->file);
  int rc = e ? verify_file(m->dir, e) : tf__fail(TF_ERR_MODEL_INVALID, "manifest.json does not list %s", c->file);
  if (rc) return rc;
  char *full = tf_path_join(m->dir, c->file), *raw = NULL;
  size_t n = 0;
  if (!full || tf_read_file(full, &raw, &n)) {
    free(full);
    return tf__fail(TF_ERR_STORAGE_FAILED, "cannot read %s", c->file);
  }
  free(full);
  const int count = (int)(n / 2);
  float *x = (float *)malloc(((size_t)count + 1) * sizeof(float));
  if (!x) {
    free(raw);
    return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  }
  const unsigned char *p = (const unsigned char *)raw;
  for (int i = 0; i < count; ++i) x[i] = (float)(int16_t)(p[2 * i] | p[2 * i + 1] << 8) / 32768.0f;
  free(raw);
  *out = x;
  *rate = c->sample_rate;
  return count;
}

int tf_model_backend(const TfModel *m, TfBackend *out) {
  if (!m || !out) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no model");
  out->kind = "cpu";
  out->name = "cpu";
  out->stage = "cpu";
  out->reason = NULL;
  return 0;
}

/* ------------------------------------------------------------------ reference data */

TfReference *tf_model_reference(TfModel *m) { return m && m->has_ref ? &m->ref : NULL; }

/* the manifest's encrypted entry of a role (the first), or NULL */
const TfJson *tf__model_role_entry(TfModel *m, const char *role) {
  const TfJson *files = m ? tf_json_get(m->manifest, "files") : NULL;
  for (int i = 0; files && i < files->n; ++i) {
    const char *r = tf_json_str(tf_json_get(&files->kids[i], "role"));
    if (r && !strcmp(r, role) && is_qle1(&files->kids[i])) return &files->kids[i];
  }
  return NULL;
}
/* 1 when the entry's device form (what the Core installed) is in the build's folder */
int tf__model_entry_installed(TfModel *m, const TfJson *f) {
  if (!m || !f || !m->pm) return 0;
  tf__plat_set_build(tf__pm_plat(m->pm), m->dir);
  char *dev = tf__plat_path(tf__pm_plat(m->pm), QLC_FORM_DEVICE, tf_json_str(tf_json_get(local_of(f), "path")));
  const int here = dev && tf_file_exists(dev);
  free(dev);
  return here;
}
/* The engine, open for a read of the reference data: the one in use, else the Model's context at 16 kHz. NULL and the
 * error when it cannot be opened (the Core needs a valid lease, as for a take). */
qlc_model *tf__model_engine_for_reference(TfModel *m) {
  if (!m) return NULL;
  if (m->zm) return m->zm;
  if (!m->pm) {
    tf__fail(TF_ERR_REFERENCE_MISSING, NULL);
    return NULL;
  }
  const char *ctx = m->warm_context[0] ? m->warm_context : m->default_context;
  if (engine_files(m, ctx)) return NULL;
  if (m->client && tf__pm_lease(m->client, m->pm, m->mbytes, m->mn, m->manifest, m->build)) return NULL;
  m->zm = tf__pm_open(m->pm, m->dir, m->mbytes, m->mn, ctx, m->threads, 16000, m->pin);
  if (!m->zm) return NULL;
  snprintf(m->eng_context, sizeof m->eng_context, "%s", ctx);
  m->eng_rate = 16000;
  if (m->client) tf__recited_index(m->client, m->pm, m->dir, m->mbytes, m->mn);
  return m->zm;
}
/* The engine closed, so the Core can install a file of this build that the open model would read (the reference
 * data; qlc_install refuses it beside an open model of the same build). The next Session or reference read opens it
 * again, and only then does the Core see the new file. TF_ERR_SESSION_OPEN while a take runs. */
int tf__model_engine_release(TfModel *m) {
  if (!m) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no model");
  if (m->open) return tf__fail(TF_ERR_SESSION_OPEN, NULL);
  engine_close(m);
  return 0;
}

/* ------------------------------------------------------------------ info */

const char *tf_model_info_json(TfModel *m) {
  if (!m) return NULL;
  if (m->info_json) return m->info_json;
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"id\":");
  tf_buf_json_str(&b, m->id);
  tf_buf_puts(&b, ",\"version\":");
  tf_buf_json_str(&b, m->version);
  tf_buf_puts(&b, ",\"build\":");
  tf_buf_json_str(&b, m->build);
  tf_buf_puts(&b, ",\"family\":");
  tf_buf_json_str(&b, tf_json_str(tf_json_get(m->manifest, "family")));
  tf_buf_puts(&b, ",\"contexts\":[");
  for (int i = 0; i < m->n_contexts; ++i) {
    if (i) tf_buf_puts(&b, ",");
    tf_buf_json_str(&b, m->contexts[i]);
  }
  tf_buf_puts(&b, "],\"default_context\":");
  tf_buf_json_str(&b, m->default_context);
  tf_buf_puts(&b, ",\"chunk_ms\":{");
  for (int i = 0; i < m->n_contexts; ++i) {
    if (i) tf_buf_puts(&b, ",");
    tf_buf_json_str(&b, m->contexts[i]);
    tf_buf_printf(&b, ":%d", tf_model_chunk_ms(m, m->contexts[i]));
  }
  tf_buf_puts(&b, "},\"heads\":[");
  for (int h = 0; h < TF_HEAD_COUNT; ++h) {
    const TfHeadInfo *hi = tf_head_info(h);
    if (h) tf_buf_puts(&b, ",");
    tf_buf_puts(&b, "{\"name\":");
    tf_buf_json_str(&b, hi->name);
    tf_buf_printf(&b, ",\"kind\":\"%s\",\"frame_ms\":%d,\"classes\":%d,\"class_names\":[", !strcmp(hi->kind, "log_softmax") ? "ctc" : "attribute",
                  hi->rate_ms, hi->width);
    for (int k = 0; k < hi->n_classes; ++k) {
      if (k) tf_buf_puts(&b, ",");
      tf_buf_json_str(&b, hi->classes[k]);
    }
    tf_buf_puts(&b, "]}");
  }
  tf_buf_puts(&b, "],\"default_heads\":[");
  for (int h = 0, first = 1; h < TF_HEAD_COUNT; ++h)
    if (TF_HEADS_DEFAULT & TF_HEAD_BIT(h)) {
      if (!first) tf_buf_puts(&b, ",");
      tf_buf_json_str(&b, tf_head_info(h)->name);
      first = 0;
    }
  tf_buf_puts(&b, "],\"tokens\":[");
  for (int i = 0; i < TF_VOCAB; ++i) {
    if (i) tf_buf_puts(&b, ",");
    tf_buf_json_str(&b, m->tokens[i]);
  }
  tf_buf_printf(&b, "],\"blank\":%d,\"word_bar\":%d,\"calibration\":", m->vocab.blank, m->vocab.word_bar);
  if (m->calib) {
    tf_buf_puts(&b, "{\"format\":");
    tf_buf_json_str(&b, m->calib_format);
    tf_buf_puts(&b, ",\"sha256\":");
    tf_buf_json_str(&b, m->calib_sha);
    tf_buf_puts(&b, "}");
  } else {
    tf_buf_puts(&b, "null");
  }
  tf_buf_printf(&b, ",\"reference\":%s,\"sample_clips\":[", m->has_ref ? "true" : "false");
  for (int i = 0; i < m->n_clips; ++i) {
    if (i) tf_buf_puts(&b, ",");
    tf_buf_puts(&b, "{\"name\":");
    tf_buf_json_str(&b, m->clips[i].name);
    tf_buf_puts(&b, ",\"kind\":");
    tf_buf_json_str(&b, m->clips[i].kind);
    tf_buf_printf(&b, ",\"duration_ms\":%d}", (int)(1000LL * m->clips[i].samples / m->clips[i].sample_rate));
  }
  tf_buf_printf(&b, "],\"signed\":%s,\"key_id\":", m->is_signed ? "true" : "false");
  if (m->is_signed) tf_buf_json_str(&b, m->key_id);
  else tf_buf_puts(&b, "null");
  tf_buf_puts(&b, ",\"sdk_version\":\"" TF_SDK_VERSION "\"}");
  m->info_json = tf_buf_take(&b);
  return m->info_json;
}
