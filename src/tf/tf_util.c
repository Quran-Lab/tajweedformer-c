/* Errors, text buffers, a small JSON DOM, files and time for the tajweedformer library. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "tf_internal.h"

/* ------------------------------------------------------------------ errors */
/* One catalogue for the four SDKs (the contract's sdk_3.0.0/errors.json, embedded by tools/gen_errors.py): a
 * TfError is the entry's index (entry i is -(i + 1)); the message is the entry's with its placeholders filled, the
 * fix likewise. */
#include "tf_error_catalogue.h"
#if defined(_MSC_VER) && !defined(__clang__)
#define TF_TLS __declspec(thread)
#else
#define TF_TLS _Thread_local
#endif
static TF_TLS char g_msg[640];
static TF_TLS char g_fix[640];
static TF_TLS char g_raw[512]; /* the text given to tf__fail, as given */
static TF_TLS int g_code;

static const TfErrorEntry *entry(int code) { return code <= -1 && code >= -TF_ERRORS_COUNT ? &kTfErrors[-code - 1] : NULL; }

/* tmpl with each {name} replaced: names[i] -> values[i]; a placeholder without a value becomes "" */
static void fill(char *out, size_t n, const char *tmpl, const char *const *names, const char *const *values, int count) {
  size_t at = 0;
  for (const char *p = tmpl; *p && at + 1 < n;) {
    const char *close = *p == '{' ? strchr(p, '}') : NULL;
    if (close) {
      const char *v = "";
      for (int i = 0; i < count; ++i)
        if (strlen(names[i]) == (size_t)(close - p - 1) && !strncmp(names[i], p + 1, (size_t)(close - p - 1))) v = values[i];
      const size_t k = strlen(v) < n - 1 - at ? strlen(v) : n - 1 - at;
      memcpy(out + at, v, k);
      at += k;
      p = close + 1;
    } else {
      out[at++] = *p++;
    }
  }
  out[at] = 0;
}

static int set_error(int code, const char *const *names, const char *const *values, int count) {
  const TfErrorEntry *e = entry(code);
  g_code = code;
  if (e) {
    fill(g_msg, sizeof g_msg, e->message, names, values, count);
    fill(g_fix, sizeof g_fix, e->fix, names, values, count);
  } else {
    snprintf(g_msg, sizeof g_msg, "%s", g_raw);
    g_fix[0] = 0;
  }
  return code;
}

int tf__fail(int code, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_raw, sizeof g_raw, fmt ? fmt : "", ap);
  va_end(ap);
  /* a detail ends without a full stop: the catalogue's sentence brings its own */
  size_t n = strlen(g_raw);
  while (n && (g_raw[n - 1] == '.' || g_raw[n - 1] == '\n' || g_raw[n - 1] == ' ')) g_raw[--n] = 0;
  static const char *const names[] = {"detail", "file", "sdk", "minimum"};
  const char *values[] = {g_raw, g_raw, TF_SDK_VERSION, TF_RUNTIME_MINIMUM};
  return set_error(code, names, values, 4);
}
int tf__runtime_check(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
  return 0;
#else
  return tf__fail(TF_ERR_RUNTIME_UNSUPPORTED, "this build of the library is not for x86-64");
#endif
}
/* The server refused this app with `code` (a 403 of the error table). When the catalogue has that refusal, the
 * error's message and fix are the refusal's own, so a developer reads what to do; any other code gives the
 * entry's own sentence with the code in it. {origin} is the app's identifier (a page's origin on the web). */
/* 1 when the catalogue has a refusal of this code under server_rejected (its `reasons`) */
int tf__reason_known(const char *code) {
  for (int i = 0; code && i < TF_REASONS_COUNT; ++i)
    if (!strcmp(kTfReasons[i].code, code)) return 1;
  return 0;
}
int tf__fail_rejected(const char *code, const char *app) {
  char given[64]; /* a copy: tf__fail below formats into g_raw, so g_raw cannot also be its argument */
  snprintf(given, sizeof given, "%s", code && *code ? code : "refused");
  snprintf(g_raw, sizeof g_raw, "%s", given);
  for (int i = 0; i < TF_REASONS_COUNT; ++i) {
    if (strcmp(kTfReasons[i].code, given)) continue;
    static const char *const names[] = {"origin"};
    const char *values[] = {app && *app ? app : "this app"};
    g_code = TF_ERR_SERVER_REJECTED;
    fill(g_msg, sizeof g_msg, kTfReasons[i].message, names, values, 1);
    fill(g_fix, sizeof g_fix, kTfReasons[i].fix, names, values, 1);
    return g_code;
  }
  return tf__fail(TF_ERR_SERVER_REJECTED, "%s", given);
}
int tf__fail_sdk_too_old(const char *version) {
  snprintf(g_raw, sizeof g_raw, "%s", version && *version ? version : "a newer version");
  static const char *const names[] = {"version", "sdk"};
  const char *values[] = {g_raw, TF_SDK_VERSION};
  return set_error(TF_ERR_SDK_TOO_OLD, names, values, 2);
}
int tf__fail_storage_full(long long needed, long long available) {
  char a[32], b[32];
  snprintf(a, sizeof a, "%lld", needed);
  snprintf(b, sizeof b, "%lld", available < 0 ? 0 : available);
  snprintf(g_raw, sizeof g_raw, "%s bytes needed, %s free", a, b);
  static const char *const names[] = {"needed", "available"};
  const char *values[] = {a, b};
  return set_error(TF_ERR_STORAGE_FULL, names, values, 2);
}
void tf__clear_error(void) {
  g_msg[0] = g_fix[0] = g_raw[0] = 0;
  g_code = 0;
}
int tf__error_code(void) { return g_code; }
const char *tf__error_text(void) { return g_msg; }
int tf__error_as(int code) {
  if (!g_code || entry(g_code)) return g_code;
  char keep[sizeof g_raw];
  snprintf(keep, sizeof keep, "%s", g_raw);
  return tf__fail(code, "%s", keep);
}
/* An internal code never reaches the app: one that was left on the thread reads as the nearest catalogue code. */
static int public_code(int code) {
  switch (code) {
    case TF__ERR_NETWORK: return TF_ERR_DOWNLOAD_FAILED;
    case TF__ERR_HTTP: return TF_ERR_SERVER_REJECTED;
    case TF__ERR_CONFIG: return TF_ERR_SERVER_REJECTED;
    case TF__ERR_NOT_AVAILABLE: return TF_ERR_SHARING_UNAVAILABLE;
    case TF__ERR_IO: return TF_ERR_STORAGE_FAILED;
    case TF__ERR_NO_MEMORY: return TF_ERR_ENGINE_FAILED;
    default: return entry(code) ? code : TF_ERR_ENGINE_FAILED;
  }
}
int tf_last_error_code(void) {
  if (g_code && !entry(g_code)) tf__error_as(public_code(g_code));
  return g_code;
}
const char *tf_last_error(void) {
  tf_last_error_code();
  return g_msg[0] ? g_msg : NULL;
}
const char *tf_last_error_fix(void) {
  tf_last_error_code();
  return g_fix[0] ? g_fix : NULL;
}
const char *tf_error_name(int code) {
  if (code == TF_OK) return "ok";
  const TfErrorEntry *e = entry(code);
  return e ? e->code : NULL;
}
const char *tf_error_fix(int code) {
  const TfErrorEntry *e = entry(code);
  return e ? e->fix : NULL;
}
const char *tf_error_docs(int code) {
  /* docs_base + code, written once per code (the same bytes whichever thread writes them) */
  static char urls[TF_ERRORS_COUNT][96];
  const TfErrorEntry *e = entry(code);
  if (!e) return NULL;
  char *u = urls[-code - 1];
  if (!u[0]) {
    char tmp[96];
    snprintf(tmp, sizeof tmp, "%s%s", TF_ERRORS_DOCS_BASE, e->code);
    memcpy(u + 1, tmp + 1, sizeof tmp - 1);
    u[0] = tmp[0];
  }
  return u;
}
/* A public call was given no object (a NULL Client, Model, Session or Heads). When an earlier call of the chain
 * failed, its error stays: the first failure is the one tf_last_error reports (the quick start checks only its
 * last call). Otherwise it is an invalid argument. */
int tf__null_handle(const char *what) {
  if (g_code) return tf_last_error_code();
  return tf__fail(TF_ERR_INVALID_ARGUMENT, "no %s", what);
}
/* the last error a public call raised in this process (any thread), for tf_client_diagnose */
static volatile int g_last_raised;
int tf__raise(int rc) {
  if (rc < 0) {
    const int code = tf_last_error_code();
    if (code) __atomic_store_n(&g_last_raised, code, __ATOMIC_SEQ_CST);
    return code ? code : rc;
  }
  return rc;
}
int tf__last_raised(void) { return __atomic_load_n(&g_last_raised, __ATOMIC_SEQ_CST); }

const unsigned char *tf__errors_json(size_t *n) {
  if (n) *n = TF_ERRORS_JSON_SIZE;
  return kTfErrorsJson;
}
const char *tf__errors_sha256(void) { return TF_ERRORS_JSON_SHA256; }

const char *tf_version(void) { return TF_SDK_VERSION; }

/* ------------------------------------------------------------------ buffers */
void tf_buf_init(TfBuf *b) { memset(b, 0, sizeof *b); }
void tf_buf_free(TfBuf *b) {
  free(b->p);
  memset(b, 0, sizeof *b);
}
void tf_buf_put(TfBuf *b, const char *s, size_t n) {
  if (b->oom) return;
  if (b->n + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + n + 1) cap *= 2;
    char *p = (char *)realloc(b->p, cap);
    if (!p) {
      b->oom = 1;
      return;
    }
    b->p = p;
    b->cap = cap;
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}
void tf_buf_puts(TfBuf *b, const char *s) { tf_buf_put(b, s, strlen(s)); }
void tf_buf_printf(TfBuf *b, const char *fmt, ...) {
  char tmp[1024];
  va_list ap;
  va_start(ap, fmt);
  int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (k < 0) return;
  if ((size_t)k < sizeof tmp) {
    tf_buf_put(b, tmp, (size_t)k);
    return;
  }
  char *big = (char *)malloc((size_t)k + 1);
  if (!big) {
    b->oom = 1;
    return;
  }
  va_start(ap, fmt);
  vsnprintf(big, (size_t)k + 1, fmt, ap);
  va_end(ap);
  tf_buf_put(b, big, (size_t)k);
  free(big);
}
void tf_buf_json_str(TfBuf *b, const char *s) {
  if (!s) {
    tf_buf_puts(b, "null");
    return;
  }
  tf_buf_put(b, "\"", 1);
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    const unsigned char c = *p;
    if (c == '"' || c == '\\') {
      char e[2] = {'\\', (char)c};
      tf_buf_put(b, e, 2);
    } else if (c == '\n') tf_buf_puts(b, "\\n");
    else if (c == '\r') tf_buf_puts(b, "\\r");
    else if (c == '\t') tf_buf_puts(b, "\\t");
    else if (c < 0x20) tf_buf_printf(b, "\\u%04x", c);
    else tf_buf_put(b, (const char *)p, 1);
  }
  tf_buf_put(b, "\"", 1);
}
void tf_buf_num(TfBuf *b, double v) {
  if (v != v || v == HUGE_VAL || v == -HUGE_VAL) {
    tf_buf_puts(b, "null");
    return;
  }
  if (v == floor(v) && fabs(v) < 1e15) {
    tf_buf_printf(b, "%.0f", v);
    return;
  }
  char t[40];
  for (int prec = 15; prec <= 17; ++prec) {
    snprintf(t, sizeof t, "%.*g", prec, v);
    if (strtod(t, NULL) == v) break;
  }
  tf_buf_puts(b, t);
}
char *tf_buf_take(TfBuf *b) {
  if (b->oom) {
    tf_buf_free(b);
    return NULL;
  }
  if (!b->p) tf_buf_put(b, "", 0);
  char *p = b->p;
  b->p = NULL;
  b->n = b->cap = 0;
  return p;
}

/* ------------------------------------------------------------------ JSON */
typedef struct {
  const char *s, *e;
  const char *err;
  int depth;
} Parser;

static void ws(Parser *p) {
  while (p->s < p->e && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')) p->s++;
}
static int put_utf8(TfBuf *b, unsigned cp) {
  char o[4];
  int n;
  if (cp < 0x80) o[0] = (char)cp, n = 1;
  else if (cp < 0x800) o[0] = (char)(0xc0 | cp >> 6), o[1] = (char)(0x80 | (cp & 63)), n = 2;
  else if (cp < 0x10000) o[0] = (char)(0xe0 | cp >> 12), o[1] = (char)(0x80 | ((cp >> 6) & 63)), o[2] = (char)(0x80 | (cp & 63)), n = 3;
  else o[0] = (char)(0xf0 | cp >> 18), o[1] = (char)(0x80 | ((cp >> 12) & 63)), o[2] = (char)(0x80 | ((cp >> 6) & 63)), o[3] = (char)(0x80 | (cp & 63)), n = 4;
  tf_buf_put(b, o, (size_t)n);
  return 0;
}
static int hex4(const char *s, unsigned *v) {
  *v = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = s[i];
    *v <<= 4;
    if (c >= '0' && c <= '9') *v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') *v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') *v |= (unsigned)(c - 'A' + 10);
    else return -1;
  }
  return 0;
}
static char *parse_string(Parser *p) {
  if (p->s >= p->e || *p->s != '"') {
    p->err = "expected a string";
    return NULL;
  }
  p->s++;
  TfBuf b;
  tf_buf_init(&b);
  while (p->s < p->e && *p->s != '"') {
    unsigned char c = (unsigned char)*p->s++;
    if (c < 0x20) {
      p->err = "control character in a string";
      tf_buf_free(&b);
      return NULL;
    }
    if (c != '\\') {
      tf_buf_put(&b, (const char *)&c, 1);
      continue;
    }
    if (p->s >= p->e) break;
    c = (unsigned char)*p->s++;
    switch (c) {
      case '"': tf_buf_put(&b, "\"", 1); break;
      case '\\': tf_buf_put(&b, "\\", 1); break;
      case '/': tf_buf_put(&b, "/", 1); break;
      case 'b': tf_buf_put(&b, "\b", 1); break;
      case 'f': tf_buf_put(&b, "\f", 1); break;
      case 'n': tf_buf_put(&b, "\n", 1); break;
      case 'r': tf_buf_put(&b, "\r", 1); break;
      case 't': tf_buf_put(&b, "\t", 1); break;
      case 'u': {
        unsigned v;
        if (p->e - p->s < 4 || hex4(p->s, &v)) {
          p->err = "bad \\u escape";
          tf_buf_free(&b);
          return NULL;
        }
        p->s += 4;
        if (v >= 0xd800 && v < 0xdc00 && p->e - p->s >= 6 && p->s[0] == '\\' && p->s[1] == 'u') {
          unsigned lo;
          if (!hex4(p->s + 2, &lo) && lo >= 0xdc00 && lo < 0xe000) {
            p->s += 6;
            v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00);
          }
        }
        put_utf8(&b, v);
        break;
      }
      default:
        p->err = "bad escape";
        tf_buf_free(&b);
        return NULL;
    }
  }
  if (p->s >= p->e) {
    p->err = "unterminated string";
    tf_buf_free(&b);
    return NULL;
  }
  p->s++;
  char *out = tf_buf_take(&b);
  if (!out) p->err = "out of memory";
  return out;
}
static int parse_value(Parser *p, TfJson *v);
static int push_kid(TfJson *v, TfJson *kid, int *cap) {
  if (v->n == *cap) {
    *cap = *cap ? *cap * 2 : 4;
    TfJson *k = (TfJson *)realloc(v->kids, (size_t)*cap * sizeof(TfJson));
    if (!k) return -1;
    v->kids = k;
  }
  v->kids[v->n++] = *kid;
  return 0;
}
static int parse_value(Parser *p, TfJson *v) {
  memset(v, 0, sizeof *v);
  ws(p);
  if (p->s >= p->e) {
    p->err = "unexpected end";
    return -1;
  }
  if (++p->depth > 64) {
    p->err = "nested too deep";
    return -1;
  }
  const char c = *p->s;
  int rc = 0;
  if (c == '{' || c == '[') {
    const int obj = c == '{';
    v->type = obj ? TJ_OBJ : TJ_ARR;
    p->s++;
    int cap = 0;
    ws(p);
    if (p->s < p->e && *p->s == (obj ? '}' : ']')) {
      p->s++;
    } else {
      for (;;) {
        TfJson kid;
        char *key = NULL;
        ws(p);
        if (obj) {
          key = parse_string(p);
          if (!key) return -1;
          ws(p);
          if (p->s >= p->e || *p->s != ':') {
            free(key);
            p->err = "expected ':'";
            return -1;
          }
          p->s++;
        }
        if (parse_value(p, &kid)) {
          free(key);
          return -1;
        }
        kid.key = key;
        if (push_kid(v, &kid, &cap)) {
          p->err = "out of memory";
          return -1;
        }
        ws(p);
        if (p->s < p->e && *p->s == ',') {
          p->s++;
          continue;
        }
        if (p->s < p->e && *p->s == (obj ? '}' : ']')) {
          p->s++;
          break;
        }
        p->err = obj ? "expected ',' or '}'" : "expected ',' or ']'";
        return -1;
      }
    }
  } else if (c == '"') {
    v->type = TJ_STR;
    v->str = parse_string(p);
    if (!v->str) rc = -1;
  } else if (c == 't' && p->e - p->s >= 4 && !memcmp(p->s, "true", 4)) {
    v->type = TJ_BOOL, v->boolean = 1, p->s += 4;
  } else if (c == 'f' && p->e - p->s >= 5 && !memcmp(p->s, "false", 5)) {
    v->type = TJ_BOOL, p->s += 5;
  } else if (c == 'n' && p->e - p->s >= 4 && !memcmp(p->s, "null", 4)) {
    v->type = TJ_NULL, p->s += 4;
  } else if (c == '-' || (c >= '0' && c <= '9')) {
    char tmp[64];
    size_t k = 0;
    const char *q = p->s;
    while (q < p->e && k < sizeof tmp - 1 && strchr("+-0123456789.eE", *q)) tmp[k++] = *q++;
    tmp[k] = 0;
    char *end;
    v->type = TJ_NUM;
    v->num = strtod(tmp, &end);
    if (end != tmp + k || k == 0) {
      p->err = "bad number";
      rc = -1;
    }
    p->s = q;
  } else {
    p->err = "unexpected character";
    rc = -1;
  }
  p->depth--;
  return rc;
}
void tf_json_free_inner(TfJson *j) {
  if (!j) return;
  for (int i = 0; i < j->n; ++i) tf_json_free_inner(&j->kids[i]);
  free(j->kids);
  free(j->str);
  free(j->key);
}
void tf_json_free(TfJson *j) {
  if (!j) return;
  tf_json_free_inner(j);
  free(j);
}
TfJson *tf_json_parse(const char *text, size_t len, const char **error) {
  Parser p = {text, text + len, NULL, 0};
  TfJson *v = (TfJson *)calloc(1, sizeof *v);
  if (!v) {
    if (error) *error = "out of memory";
    return NULL;
  }
  if (parse_value(&p, v) == 0) {
    ws(&p);
    if (p.s == p.e) return v;
    p.err = "trailing characters";
  }
  if (error) *error = p.err ? p.err : "invalid JSON";
  tf_json_free(v);
  return NULL;
}
const TfJson *tf_json_get(const TfJson *o, const char *key) {
  if (!o || o->type != TJ_OBJ) return NULL;
  const TfJson *found = NULL;
  for (int i = 0; i < o->n; ++i)
    if (o->kids[i].key && !strcmp(o->kids[i].key, key)) found = &o->kids[i]; /* last wins, as JSON.parse */
  return found;
}
const char *tf_json_str(const TfJson *j) { return j && j->type == TJ_STR ? j->str : NULL; }
int tf_json_is_int(const TfJson *j) { return j && j->type == TJ_NUM && j->num == floor(j->num) && fabs(j->num) < 9e15; }
void tf_json_write(TfBuf *b, const TfJson *j) {
  if (!j) {
    tf_buf_puts(b, "null");
    return;
  }
  switch (j->type) {
    case TJ_NULL: tf_buf_puts(b, "null"); break;
    case TJ_BOOL: tf_buf_puts(b, j->boolean ? "true" : "false"); break;
    case TJ_NUM: tf_buf_num(b, j->num); break;
    case TJ_STR: tf_buf_json_str(b, j->str); break;
    case TJ_ARR:
    case TJ_OBJ:
      tf_buf_puts(b, j->type == TJ_OBJ ? "{" : "[");
      for (int i = 0; i < j->n; ++i) {
        if (i) tf_buf_puts(b, ",");
        if (j->type == TJ_OBJ) {
          tf_buf_json_str(b, j->kids[i].key);
          tf_buf_puts(b, ":");
        }
        tf_json_write(b, &j->kids[i]);
      }
      tf_buf_puts(b, j->type == TJ_OBJ ? "}" : "]");
      break;
  }
}

/* ------------------------------------------------------------------ files */
int tf_read_file(const char *path, char **data, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END)) {
    fclose(f);
    return -1;
  }
  const long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    fclose(f);
    return -1;
  }
  char *p = (char *)malloc((size_t)sz + 1);
  if (!p) {
    fclose(f);
    return -1;
  }
  const size_t got = fread(p, 1, (size_t)sz, f);
  fclose(f);
  if (got != (size_t)sz) {
    free(p);
    return -1;
  }
  p[sz] = 0;
  *data = p;
  if (n) *n = (size_t)sz;
  return 0;
}
/* atomic replace: a temp name unique to this process and write (two writers of the same file never share one),
 * then a replace retried a few times (5, 10, 15, 20, 25 ms): on Windows another handle on the target (a reader,
 * an antivirus scan) makes MoveFileEx fail with a sharing violation for a moment */
static volatile long g_tmp_seq;
int tf_write_file(const char *path, const void *data, size_t n) {
  const size_t L = strlen(path);
  char *tmp = (char *)malloc(L + 48);
  if (!tmp) return -1;
#ifdef _WIN32
  const long seq = InterlockedIncrement(&g_tmp_seq);
  snprintf(tmp, L + 48, "%s.%lu.%ld.tmp", path, (unsigned long)GetCurrentProcessId(), seq);
#else
  const long seq = __atomic_add_fetch(&g_tmp_seq, 1, __ATOMIC_SEQ_CST);
  snprintf(tmp, L + 48, "%s.%ld.%ld.tmp", path, (long)getpid(), seq);
#endif
  FILE *f = fopen(tmp, "wb");
  if (!f) {
    free(tmp);
    return -1;
  }
  const int ok = fwrite(data, 1, n, f) == n;
  const int closed = fclose(f) == 0;
  if (!ok || !closed) {
    remove(tmp);
    free(tmp);
    return -1;
  }
  int rc = -1;
  for (int attempt = 0; attempt < 6 && rc; ++attempt) {
#ifdef _WIN32
    if (attempt) Sleep((DWORD)(5 * attempt));
    rc = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    if (attempt) usleep((useconds_t)(5000 * attempt));
    rc = rename(tmp, path);
#endif
  }
  if (rc) remove(tmp);
  free(tmp);
  return rc;
}
int tf_file_exists(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}
static int mkdir1(const char *p) {
#ifdef _WIN32
  if (_mkdir(p) == 0 || errno == EEXIST) return 0;
#else
  if (mkdir(p, 0700) == 0 || errno == EEXIST) return 0;
#endif
  return -1;
}
int tf_mkdirs(const char *path) {
  char *p = (char *)malloc(strlen(path) + 1);
  if (!p) return -1;
  strcpy(p, path);
  for (char *q = p + 1; *q; ++q) {
    if (*q == '/' || *q == '\\') {
      const char c = *q;
      *q = 0;
      if (!(q - p == 2 && p[1] == ':')) mkdir1(p);
      *q = c;
    }
  }
  const int rc = mkdir1(p);
  free(p);
  return rc;
}
char *tf_path_join(const char *a, const char *b) {
  const size_t la = strlen(a), lb = strlen(b);
  char *p = (char *)malloc(la + lb + 2);
  if (!p) return NULL;
  memcpy(p, a, la);
  size_t at = la;
  if (la && a[la - 1] != '/' && a[la - 1] != '\\') p[at++] = '/';
  memcpy(p + at, b, lb + 1);
  return p;
}

double tf__now_s(void) {
#ifdef _WIN32
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)f.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}
const char *tf__platform_name(void) {
#ifdef _WIN32
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

/* ------------------------------------------------------------------ folders */
#ifndef _WIN32
#include <dirent.h>
#include <sys/statvfs.h>
#endif

int tf__list_dir(const char *path, char (*names)[256], int max) {
  int n = 0;
#ifdef _WIN32
  char *pat = tf_path_join(path, "*");
  WIN32_FIND_DATAA fd;
  HANDLE h = pat ? FindFirstFileA(pat, &fd) : INVALID_HANDLE_VALUE;
  free(pat);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..") || strlen(fd.cFileName) > 255) continue;
    if (n < max) snprintf(names[n++], 256, "%s", fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR *d = opendir(path);
  struct dirent *e;
  while (d && (e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) > 255) continue;
    if (n < max) snprintf(names[n++], 256, "%s", e->d_name);
  }
  if (d) closedir(d);
#endif
  return n;
}

static int is_dir(const char *path) {
#ifdef _WIN32
  const DWORD a = GetFileAttributesA(path);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}
static long long file_bytes(const char *path) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 0;
  return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
#else
  struct stat st;
  return stat(path, &st) == 0 ? (long long)st.st_size : 0;
#endif
}

/* walks a tree (at most 4096 entries a folder): sums the files' bytes and, with `rm`, removes everything */
static long long walk(const char *path, int rm, int depth) {
  if (!is_dir(path)) {
    const long long n = file_bytes(path);
    if (rm) remove(path);
    return n;
  }
  long long total = 0;
  if (depth < 16) {
    char (*names)[256] = (char (*)[256])malloc(4096 * 256);
    const int n = names ? tf__list_dir(path, names, 4096) : 0;
    for (int i = 0; i < n; ++i) {
      char *sub = tf_path_join(path, names[i]);
      if (sub) total += walk(sub, rm, depth + 1);
      free(sub);
    }
    free(names);
  }
  if (rm) {
#ifdef _WIN32
    RemoveDirectoryA(path);
#else
    rmdir(path);
#endif
  }
  return total;
}
long long tf__dir_bytes(const char *path) { return path && (is_dir(path) || tf_file_exists(path)) ? walk(path, 0, 0) : 0; }
int tf__remove_tree(const char *path) {
  if (!path || !*path) return -1;
  if (is_dir(path) || tf_file_exists(path)) walk(path, 1, 0);
  return is_dir(path) || tf_file_exists(path) ? -1 : 0;
}
long long tf__free_bytes(const char *path) {
#ifdef _WIN32
  ULARGE_INTEGER avail;
  return GetDiskFreeSpaceExA(path, &avail, NULL, NULL) ? (long long)avail.QuadPart : -1;
#else
  struct statvfs v;
  return statvfs(path, &v) == 0 ? (long long)v.f_bavail * (long long)v.f_frsize : -1;
#endif
}
void tf__sleep_ms(int ms) {
  if (ms <= 0) return;
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  usleep((useconds_t)ms * 1000);
#endif
}
