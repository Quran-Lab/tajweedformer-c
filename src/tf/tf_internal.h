/* Internal declarations of the tajweedformer library (not installed). */
#ifndef TF_INTERNAL_H
#define TF_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "tajweedformer.h"

/* ---- errors (thread-local) ---- */
int tf__fail(int code, const char *fmt, ...);
/* a model file's URL under a download base (tf_download.c) */
int tf__model_url(char *out, size_t n, const char *base, const char *repo, const char *revision, const char *folder, const char *rel);
void tf__clear_error(void);

/* ---- growable text buffer ---- */
typedef struct {
  char *p;
  size_t n, cap;
  int oom;
} TfBuf;
void tf_buf_init(TfBuf *b);
void tf_buf_free(TfBuf *b);
void tf_buf_put(TfBuf *b, const char *s, size_t n);
void tf_buf_puts(TfBuf *b, const char *s);
void tf_buf_printf(TfBuf *b, const char *fmt, ...);
void tf_buf_json_str(TfBuf *b, const char *s); /* a JSON string literal, quoted and escaped */
void tf_buf_num(TfBuf *b, double v);           /* shortest round-trip repr; null for NaN/inf */
char *tf_buf_take(TfBuf *b);                   /* NUL-terminated, caller frees; NULL on OOM */

/* ---- JSON DOM ---- */
typedef enum { TJ_NULL, TJ_BOOL, TJ_NUM, TJ_STR, TJ_ARR, TJ_OBJ } TfJsonType;
typedef struct TfJson {
  TfJsonType type;
  double num;
  int boolean;
  char *str;          /* TJ_STR: value (UTF-8, NUL-terminated); object member: key in `key` */
  char *key;
  struct TfJson *kids; /* TJ_ARR / TJ_OBJ children */
  int n;
} TfJson;
TfJson *tf_json_parse(const char *text, size_t len, const char **error);
void tf_json_free(TfJson *j);
const TfJson *tf_json_get(const TfJson *obj, const char *key); /* NULL when absent or not an object */
const char *tf_json_str(const TfJson *j);                     /* NULL unless a string */
int tf_json_is_int(const TfJson *j);
void tf_json_write(TfBuf *b, const TfJson *j); /* compact re-serialisation */

/* ---- crypto ---- */
void tf_sha256(const void *data, size_t n, uint8_t out[32]);
typedef struct {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  size_t fill;
} TfSha256;
void tf_sha256_init(TfSha256 *c);
void tf_sha256_update(TfSha256 *c, const void *data, size_t n);
void tf_sha256_final(TfSha256 *c, uint8_t out[32]);
void tf_sha512(const void *data, size_t n, uint8_t out[64]);
/* 1 when sig (64 bytes) is a valid Ed25519 signature of msg by pk (32 bytes), else 0. */
int tf_ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t pk[32]);
int tf_hex_decode(const char *hex, uint8_t *out, size_t n); /* exactly 2n hex digits; 0 ok */
void tf_hex_encode(const uint8_t *in, size_t n, char *out); /* 2n chars + NUL */
/* base64 (standard or url-safe, padding optional); returns bytes written or -1 */
int tf_base64_decode(const char *s, uint8_t *out, size_t cap);
int tf_sha256_file(const char *path, uint8_t out[32], long long *size);
int tf_random_bytes(uint8_t *out, size_t n);

/* ---- files ---- */
int tf_read_file(const char *path, char **data, size_t *n);
int tf_write_file(const char *path, const void *data, size_t n); /* atomic: tmp + rename */
int tf_mkdirs(const char *path);
char *tf_path_join(const char *a, const char *b);
int tf_file_exists(const char *path);

/* ---- HTTP ---- */
typedef struct {
  int status;
  char *body;
  size_t body_n;
  char signature[160];  /* x-qlab-signature */
  char key_id[40];      /* x-qlab-key-id */
  char etag[80];
  long long content_length;
} TfHttpResponse;
typedef struct {
  const char *method;   /* GET, POST, PUT */
  const char *url;
  const char *const *headers; /* "name: value", NULL-terminated */
  const void *body;
  size_t body_n;
  const char *upload_file; /* PUT the file's bytes instead of body */
  const char *download_file; /* GET into this file (appending from `range_from`) instead of body */
  long long range_from; /* > 0: Range: bytes=N- */
  int timeout_s;
  volatile int *cancel; /* optional: tf__http_cancel(cancel) from another thread aborts the request */
} TfHttpRequest;
/* Sets *flag and aborts every request in flight that carries it (WinHTTP: its handle is closed; libcurl: the
 * progress callback stops the transfer). */
void tf__http_cancel(volatile int *flag);
int tf_http(const TfHttpRequest *req, TfHttpResponse *resp); /* 0: got a response (any status) */
void tf_http_response_free(TfHttpResponse *r);
/* Tests: replace the transport. */
typedef int (*TfHttpTransport)(const TfHttpRequest *req, TfHttpResponse *resp, void *user);
void tf_http_set_transport(TfHttpTransport t, void *user);
int _tf_strnicmp(const char *a, const char *b, size_t n);

/* ---- model internals other units need ---- */
struct TfModel;
const char *tf__model_dir(const struct TfModel *m);
const TfJson *tf__model_calibration(const struct TfModel *m);
const char *tf__model_context(const struct TfModel *m);
void tf__model_word_effective(const struct TfModel *m, int *enabled, double *threshold, char *preset, size_t preset_n,
                              char *source, size_t source_n);

double tf__now_s(void);
const char *tf__platform_name(void);

#endif
