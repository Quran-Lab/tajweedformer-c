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
#if defined(_MSC_VER) && !defined(__clang__)
#define TF_TLS __declspec(thread)
#else
#define TF_TLS _Thread_local
#endif
static TF_TLS char g_msg[512];
static TF_TLS int g_code;

int tf__fail(int code, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_msg, sizeof g_msg, fmt, ap);
  va_end(ap);
  g_code = code;
  return code;
}
void tf__clear_error(void) {
  g_msg[0] = 0;
  g_code = 0;
}
const char *tf_last_error(void) { return g_msg[0] ? g_msg : NULL; }
int tf_last_error_code(void) { return g_code; }

const char *tf_error_name(int code) {
  switch (code) {
    case TF_OK: return "ok";
    case TF_ERR_ARGUMENT: return "invalid_argument";
    case TF_ERR_IO: return "io";
    case TF_ERR_MANIFEST: return "manifest_invalid";
    case TF_ERR_SIGNATURE: return "signature_invalid";
    case TF_ERR_SIGNATURE_REQUIRED: return "signature_required";
    case TF_ERR_CHECKSUM: return "checksum_mismatch";
    case TF_ERR_NOT_LISTED: return "not_listed";
    case TF_ERR_SDK_TOO_OLD: return "sdk_too_old";
    case TF_ERR_UNSUPPORTED_MODEL: return "unsupported_model";
    case TF_ERR_CALIBRATION: return "calibration_invalid";
    case TF_ERR_ENGINE: return "engine";
    case TF_ERR_SETTINGS: return "invalid_settings";
    case TF_ERR_BUSY: return "stream_busy";
    case TF_ERR_NETWORK: return "network";
    case TF_ERR_HTTP: return "http";
    case TF_ERR_NO_MEMORY: return "no_memory";
    case TF_ERR_NOT_AVAILABLE: return "not_available";
    case TF_ERR_CONFIG: return "config_invalid";
    default: return "unknown";
  }
}

const char *tf_version(void) { return TF_SDK_VERSION; }
int tf_api_version(void) { return TF_API_VERSION; }

void tf_free(void *p) { free(p); }

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
