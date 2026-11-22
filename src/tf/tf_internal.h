/* Internal declarations of the tajweedformer library (not installed). The public API is include/tajweedformer.h
 * and nothing else; the library's tests reach what is below by including this header and linking the static
 * library. */
#ifndef TF_INTERNAL_H
#define TF_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "tajweedformer.h"
#include "qlcore.h"

typedef struct TfPm TfPm;
typedef struct TfIkey TfIkey;
typedef struct TfPlat TfPlat;

/* ---- SDK 4.0: the closed Core's types as this library names them ----
 * The engine is inside the Core (quranlab-core, a prebuilt library); its heads come out as qlc_heads, the layout the
 * 3.x engine's ZfTajweedHeads had. The vocabulary is TajweedFormer's tj3: 38 tokens, blank 0. */
#define TF_VOCAB 38
#define TF_BLANK 0
typedef qlc_heads TfRawHeads;
/* per 40 ms frame of log_probs, the best token and the runner-up (tf_rank.c) */
typedef struct {
  int token, runner_up;
  float log_prob, runner_up_log_prob, margin;
} TfTop2;
int tf__top2(const float *lp, int frames, int vocab, TfTop2 *out);
int tf__topk_rows(const float *values, int frames, int cols, int k, int *ids, float *values_out, float *margins);

/* ---- errors (thread-local) ---- */
/* Failures that stay inside the library (a network call of the client's background work, a refused config): never a
 * TfError of the app. tf_last_error_code maps one that escapes to the nearest catalogue code. */
enum {
  TF__ERR_NETWORK = -101,       /* no answer: no connection, a timeout */
  TF__ERR_HTTP = -102,          /* the server answered with an error status */
  TF__ERR_CONFIG = -103,        /* a signed config failed verification (signature, key id, expiry) */
  TF__ERR_NOT_AVAILABLE = -104, /* not possible now (network calls stopped for this launch, sharing off) */
  TF__ERR_IO = -105,            /* a file of the SDK's own state */
  TF__ERR_NO_MEMORY = -106
};
/* Sets this thread's error and returns `code`. For a catalogue code the message is the catalogue's, with its
 * {detail} or {file} placeholder filled from the formatted text; for an internal code the text itself. */
int tf__fail(int code, const char *fmt, ...);
int tf__fail_rejected(const char *server_code, const char *app); /* server_rejected with that refusal's message and fix */
int tf__reason_known(const char *code);                          /* the catalogue names this refusal */
int tf__fail_sdk_too_old(const char *version);                       /* {version}, {sdk} */
int tf__fail_storage_full(long long needed, long long available);    /* {needed}, {available} */
void tf__clear_error(void);
int tf__error_code(void);        /* the code as set (catalogue or internal) */
const char *tf__error_text(void);/* the message as set */
/* An internal code left on this thread becomes `code` (the catalogue's message with the internal text as its
 * detail); a catalogue code stays. Returns the code in force. */
int tf__error_as(int code);
/* tf__raise without a report: the same failure handed to the app again (a failed take's heads asked for) */
int tf__raise_again(int rc);
/* 4.0.1: a thread's error (its code, message, fix and the cause it carries), taken on the thread that failed and put
 * on another (a download lane's failure becomes the call's, on the caller's thread) */
typedef struct {
  int code;
  char msg[640], fix[640], raw[512];
  unsigned char cause[192];
} TfErrState;
void tf__err_save(TfErrState *s);
void tf__err_restore(const TfErrState *s);
void tf__cause_save(void *out, size_t n);       /* tf_report.c: the thread's cause into n bytes (n >= its size) */
void tf__cause_restore(const void *in, size_t n);

/* ---- error reports (tf_report.c; contract sdk_4.0.0 ERRORS_40.md, section 24) ---- */
enum { TF_REP_NONE = 0, TF_REP_PARTIAL = 1, TF_REP_COMPLETE = 2 }; /* what of the build the call needs is on the device */
#define TF_REP_DAILY 16         /* signatures an install reports a day */
#define TF_REP_QUEUE 32         /* reports queued */
#define TF_REP_REPORTED 64      /* signatures remembered as reported (first_report) */
#define TF_REP_FIRST_AGAIN 90   /* days after which a signature is a first report again */
#define TF_REP_KEEP 7           /* days a report is kept */
#define TF_REP_COUNT_MAX 100000
#define TF_REP_DAYSIGS 192      /* the days' signature list: 16 a day over the days kept, with room */
/* 24.4: the code of the TfError the app gets ("none": the call goes on) for a cause; tools/errors40.py code_for */
const char *tf__rep_code_for(const char *stage, const char *reason, int status, int on_device, const char *error_code);
int tf__rep_reported(const char *reason, const char *code);      /* 24.1: whether it makes a report at all */
const char *tf__rep_failure_reason(const char *reason);          /* 24.8: the telemetry failure_reasons name */
void tf__rep_signature(const char *stage, const char *reason, const char *code, int status, char *out, size_t n);
void tf__rep_signature_hex(const char *stage, const char *reason, const char *code, int status, char hex[33]);
const char *tf__rep_storage_free(long long bytes);
/* The cause of a failure, on this thread: recorded where the failure is seen, taken by the next catalogue code set
 * on the thread that it can lead to (tf__fail), reported by tf__raise; tf__clear_error forgets it. */
void tf__cause(const char *stage, const char *reason, int status, const char *error_name, const char *error_code);
void tf__cause_force(void);       /* the next catalogue code takes the cause whatever 24.4 gives the pair */
/* the code already set on the thread has this cause (known only after the code was set; nothing else changes) */
void tf__cause_now(int code, const char *stage, const char *reason, int status, const char *error_name, const char *error_code);
void tf__cause_retries(int n);    /* the attempts inside the failing call beyond the first */
void tf__cause_bind(int code);
void tf__cause_clear(void);
const char *tf__cause_reason(void); /* the reason of the cause in force, "" for none */
const char *tf__rep_answer_reason(int status, const char *body, size_t n, char code[33]);
/* a 403 lease_integrity_failed (18.4.10; ERRORS_40.md 24.3, contract 69326fb): the report's error_code from the
 * answer's why and check (why:check, or why alone; 0 when why is not a token), and the developer's detail */
int tf__integrity_code(const char *why, const char *check, char out[33]);
void tf__integrity_detail(const char *why, const char *check, char *out, size_t n);
void tf__cause_answer(const char *stage, int status, const char *body, size_t n); /* an answer that failed */
void tf__cause_transport(const char *stage, int status);                       /* no answer the request took */
void tf__cause_silent(void); /* the cause, as a silent report, unless an app call of this thread reports it */
/* >0 while the app's own call (a model, a prefetch, the reference data, a deletion) runs on this thread: its
 * failures reach the app and are reported there, never as silent ones */
extern _Thread_local int tf__app_call;
void tf__rep_raised(int code, int report); /* report 0: the same failure handed over again (its cause, no report) */
/* 4.0.2 (24.13 item 3): while set, every file the call in flight needs is on the device, so a needs_connection it
 * makes needs the lease, not the model; returns what was set before (put it back after) */
int tf__cause_files_here(int here);
unsigned tf__error_serial(void); /* tf_util.c: changes whenever an error is set on the thread */
void tf__rep_silent(const char *stage, const char *reason, int status, const char *error_name, const char *error_code);
/* the queue machine (tools/errors40.py Reporter): one install's reports, the days' signatures, the reported list */
typedef struct {
  char day[11], stage[16], reason[33], code[33], sig[33];
  int status, count, first_report, first_launch, retries, answer;
  char name[49], ecode[33], ram[8], free_[8], revision[24], build[41];
  char role[8];  /* 4.0.1 (28.7): host_role, "" for none */
  int fallback;  /* 4.0.1: 0 none, 1 false, 2 true */
} TfRepItem;
typedef struct {
  TfRepItem q[TF_REP_QUEUE + 1];
  int nq;
  struct {
    char day[11], sig[33];
    int sent;
  } ds[TF_REP_DAYSIGS];
  int nds;
  struct {
    char sig[33], day[11];
  } rep[TF_REP_REPORTED];
  int nrep;
  int off;
} TfRepState;
typedef struct {
  const char *stage, *reason, *code;
  int status, first_launch, retries;
  const char *name, *ecode, *ram, *free_, *revision, *build;
  const char *role; /* 4.0.1: host_role (NULL: none) */
  int fallback;     /* 4.0.1: 0 none, 1 false, 2 true */
} TfRepOcc;
typedef int (*TfRepPost)(void *user, const TfRepItem *it); /* the answer's status; 0 none; -1 stop (tests) */
int tf__rep_occur(TfRepState *r, const char *day, const TfRepOcc *o);
void tf__rep_off(TfRepState *r, int off);
int tf__rep_send_queue(TfRepState *r, const char *today, TfRepPost post, void *user, TfRepItem *made, int max_made);
void tf__rep_from_json(TfRepState *r, const char *text, size_t n);
char *tf__rep_to_json(const TfRepState *r);
/* the embedded catalogue file (sdk_3.0.0/errors.json, byte for byte) and its recorded SHA-256 */
const unsigned char *tf__errors_json(size_t *n);
const char *tf__errors_sha256(void);

/* a model file's URL under a download base (tf_download.c) */
int tf__model_folder(char *out, size_t n, const char *prefix, const char *repo, const char *revision, const char *folder);

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
/* bytes of every file under a folder (0 when it does not exist) */
long long tf__dir_bytes(const char *path);
/* removes a folder and everything in it; 0 when it is gone */
int tf__remove_tree(const char *path);
/* bytes free for this user on the volume that holds `path` (-1 when unknown) */
long long tf__free_bytes(const char *path);
/* names of the entries of a folder (not "." and ".."), each at most 255 bytes; returns how many were written */
int tf__list_dir(const char *path, char (*names)[256], int max);
void tf__sleep_ms(int ms);

/* ---- HTTP ---- */
typedef struct {
  int status;
  char *body;
  size_t body_n;
  char signature[160];  /* x-qlab-signature */
  char key_id[40];      /* x-qlab-key-id */
  char etag[80];
  long long content_length;
  long long range_start, range_total; /* the answer's Content-Range (bytes S-E/T): S and T; -1 without one */
  char protocol[12];    /* what the answer came on: "h3", "h2" or "http/1.1" (HTTP/1.1 or older); "" from a test's
                           transport */
  int retry_after;      /* the answer's Retry-After in seconds (the delay form), 0 without one */
} TfHttpResponse;
/* A connection kept for several requests to one host, one after another on one thread (the model download):
 * one WinHTTP session and connection, or one curl handle. HTTP/2 is asked for on it (WinHTTP: HTTP/3 too, where
 * the system has it), and the system's HTTP/1.1 is what is left where it is refused. */
typedef struct TfHttpConn TfHttpConn;
TfHttpConn *tf__http_conn_open(void);    /* NULL: out of memory; a request without one opens its own connection */
extern int tf__http_opened;              /* tests: the WinHTTP sessions, or curl handles, this process has opened */
void tf__http_conn_close(TfHttpConn *k); /* NULL is fine */
typedef struct {
  const char *method;   /* GET, POST, PUT */
  const char *url;
  const char *const *headers; /* "name: value", NULL-terminated */
  const void *body;
  size_t body_n;
  const char *upload_file; /* PUT the file's bytes instead of body */
  const char *download_file; /* GET into this file instead of body: a 206 to a Range is appended, a 200 is the
                                whole file and replaces what the file held; any other status goes to body */
  long long range_from; /* > 0: Range: bytes=N- */
  const char *download_whole; /* optional: where a 200 to a Range request is written (the whole file), so that
                           the bytes in download_file are not touched before all of the file is here */
  long long download_size; /* > 0: the file's size. The body of a 200 may be that long, the body of a 206 that
                           minus range_from: a body that runs past it is cut off there and the request fails */
  const char *if_range; /* with range_from: If-Range, the validator (ETag) of the bytes already here */
  int timeout_s;        /* the limit of the whole request (and of each of its steps); not used with stall_ms */
  int stall_ms;         /* > 0: the request ends when no byte of the answer arrives for this long, and has no
                           limit on its whole time (resolving, connecting and sending keep 30 s each) */
  int head_ms;          /* > 0: the answer's status and headers must arrive within this long of the request's start
                           (resolving, connecting, TLS, a proxy's CONNECT, sending, the server's wait); then the
                           request fails as no answer. Apart from the stall limit and the whole time, which go on
                           after the headers as before. 4.0.1: TF_EP_HEAD_MS on every request to Quran Lab's hosts */
  TfHttpConn *conn;     /* optional: the kept connection this request goes on */
  volatile int *cancel; /* optional: tf__http_cancel(cancel) from another thread aborts the request */
  int (*accept)(void *user, const TfHttpResponse *r); /* optional: asked when the status and the headers are
                           in and before any file is opened. 0: this is not the answer that was asked for;
                           nothing of it is written and the request fails */
  void (*on_head)(void *user, const TfHttpResponse *r); /* optional: the status and the headers are in; for a
                           download the file is open (emptied by a 200) and no byte of the body is written yet */
  void (*on_bytes)(void *user, long long n); /* optional: called as bytes of a download reach its file */
  void (*on_data)(void *user, const void *p, size_t n); /* optional (4.0.1): the same bytes, as they were written
                           to the file, in order (the download is hashed as it comes); a test's transport may
                           not call it */
  void *bytes_user;     /* the user pointer of on_head, on_bytes and on_data */
} TfHttpRequest;
/* Sets *flag and aborts every request in flight that carries it (WinHTTP: its handle is closed; libcurl: the
 * progress callback stops the transfer). */
void tf__http_cancel(volatile int *flag);
/* 0: got a response (any status). On a failure the status and the headers that had arrived stay in resp.
 * No request follows a redirect (SPEC 15.4): a 3xx answer is the response, and neither a header nor the body
 * of the request goes to the place it names. */
int tf_http(const TfHttpRequest *req, TfHttpResponse *resp);
void tf_http_response_free(TfHttpResponse *r);
/* why the thread's last request failed without an answer it took: the kind, and the transport's error name and code
 * (NULL when none; a token, never a message) */
enum { TF__FAIL_NETWORK = 0, TF__FAIL_TIMEOUT = 1, TF__FAIL_CANCELLED = 2, TF__FAIL_ANSWER = 3 };
void tf__http_note(int kind, const char *name, long code);
int tf__http_failure(const char **name, const char **code);
/* Tests: the system's transport (WinHTTP, libcurl) whatever transport is set, for a test's transport that
 * answers some requests itself and passes the others on. */
int tf__http_native(const TfHttpRequest *req, TfHttpResponse *resp);
/* Tests: replace the transport. */
typedef int (*TfHttpTransport)(const TfHttpRequest *req, TfHttpResponse *resp, void *user);
void tf_http_set_transport(TfHttpTransport t, void *user);
/* Test programs only (an app never sets it): "host:port" of an HTTP proxy every request goes through (https by
 * CONNECT), so a test can block hosts for its own process; NULL: the system's proxy settings, as always */
extern const char *tf__http_proxy;
/* the thread's record of its last request (why it failed, the answer it got), saved and put back */
typedef struct {
  int kind, status, api_error;
  char name[48], code[16];
} TfHttpState;
void tf__http_state_save(TfHttpState *s);
void tf__http_state_restore(const TfHttpState *s);
/* the thread's error (code and texts), saved and put back (tf__error_restore frees it when asked) */
typedef struct TfErrSave TfErrSave;
TfErrSave *tf__error_save(void);
void tf__error_restore(TfErrSave *s, int free_it);

/* ---- endpoint sets: more than one host per role, and failover (ENDPOINTS_40.md, section 28; tf_endpoints.c) ---- */
enum { TF_EP_API = 0, TF_EP_MODELS = 1 };
enum { TF_EP_TAKEN = 0, TF_EP_REFUSED = 1, TF_EP_SLOW = 2, TF_EP_UNREACHABLE = 3 }; /* 28.3 item 3 */
#define TF_EP_MAX 8            /* hosts of a role: a config's list has at most 8 (28.2) */
#define TF_EP_HOST 254         /* a host name and its NUL (253 characters at most, 28.1) */
#define TF_EP_PROBE_S 1800     /* 28.3 item 1: 30 minutes after P became preferred, the first host is asked first */
#define TF_EP_CONNECT_MS 5000  /* 28.3 item 6: a host's time to connect (TCP; libcurl: TCP, TLS and a proxy) */
#define TF_EP_HEAD_MS 10000    /* 28.3 item 6: a host's time to the answer's headers, from the request's start: WinHTTP
                                  cannot bound TLS or a proxy's CONNECT alone, so a host that accepts and then says
                                  nothing has 10 s, then the next host is asked */
typedef struct TfEndpoints TfEndpoints;
/* 28.1: a host of Quran Lab's allow-list (n bytes of h; ASCII labels, one of the three domains or under one) */
int tf__ep_host_allowed(const char *h, size_t n);
/* 28.4: a lease's base split into its host (lowercase) and its path (starting and ending with '/'); 0 when it is no
 * base of 28.4 */
int tf__ep_base_parts(const char *base, char host[TF_EP_HOST], char *path, size_t cap);
/* 28.2: a role's hosts from a config's `endpoints` (NULL: none) while that config is in force; the count */
int tf__ep_role_list(int role, const TfJson *endpoints, int in_force, char out[TF_EP_MAX][TF_EP_HOST]);
/* The Client's endpoint set. api_url, cdn_url: a test build's overrides, each its role's one host (NULL: the built-in
 * lists); cdn_url's path is the models folder's prefix ("/models" by default). */
TfEndpoints *tf__ep_new(const char *api_url, const char *cdn_url);
void tf__ep_free(TfEndpoints *e);
/* the config in force (its JSON text, NULL for none): the lists of 28.2; a list that changes starts at its first host */
void tf__ep_set_config(TfEndpoints *e, const char *config_json, int in_force);
void tf__ep_set_lists(TfEndpoints *e, int role, char hosts[][TF_EP_HOST], int n); /* tests: a list in force */
const char *tf__ep_prefix(const TfEndpoints *e);
typedef struct {
  int n, probe;
  char host[TF_EP_MAX + 1][TF_EP_HOST];
} TfEpOrder;
/* 28.3 item 1: the hosts a request starting now asks, in order; `first` (may be NULL): a lease base's host, asked
 * before them when it is not one of the role's (28.4) */
void tf__ep_order(TfEndpoints *e, int role, const char *first, TfEpOrder *o);
/* where a host is reached: https://<host>, a test's override, or a test's stand-in (tf__ep_test_origin) */
void tf__ep_origin(const TfEndpoints *e, int role, const char *host, char *out, size_t n);
/* 28.3 item 5: the host an answer was taken, refused or slowed down on becomes P */
void tf__ep_settle(TfEndpoints *e, int role, const char *host, int probe);
/* 28.7 lastFailover: a request found its first host unreachable (to: NULL when no host answered) */
void tf__ep_failed_over(TfEndpoints *e, int role, const char *from, const char *to, const char *reason, int status);
void tf__ep_preferred(TfEndpoints *e, int role, char out[TF_EP_HOST]); /* the preferred host P of a role (28.3) */
char *tf__ep_diag_json(TfEndpoints *e); /* the diagnosis's `endpoints` object; caller frees */
/* one request across a role's hosts */
typedef struct {
  int role;
  const char *path;       /* what follows the origin: the path and the query */
  const char *first;      /* a lease base's host that is not one of the role's, asked first (28.4); NULL */
  const char *stage;      /* the 24.3 stage of the request, for the silent failover report (NULL: none) */
  int not_modified_ok;    /* a 304 is taken (a config request with If-None-Match) */
  /* a 2xx: 1 when it is the answer the route asks for (taken), 0 when not (unreachable: the next host). NULL: a body
   * that is empty or JSON */
  int (*taken)(void *user, const TfHttpResponse *r);
  void *user;
  double deadline;        /* tf__now_s after which no host is asked (18.4.7.1's 10 s); 0 for none */
  TfClient *hosts_client; /* 34.6: each model request counted in this Client's per-host telemetry (NULL: none) */
} TfEpReq;
typedef struct {
  int cls;                /* TF_EP_TAKEN, TF_EP_REFUSED, TF_EP_SLOW, TF_EP_UNREACHABLE */
  int asked, fallback;    /* the hosts tried (one a hold skipped too); 4.0.2: more than one really asked (24.2) */
  char by[TF_EP_HOST];    /* the host the request ended on ("" when every host was unreachable) */
  char first_reason[20];  /* the first host's 24.3 reason when it was unreachable ("" when not) */
  int first_status;       /* its status, 0 for none */
} TfEpOut;
int tf__ep_class(const TfEpReq *er, int rc, const TfHttpResponse *r);
const char *tf__ep_reason(int rc, const TfHttpResponse *r); /* an unreachable answer's 24.3 reason */
/* 28.3: q (its url is set here for each host) asked of the hosts in order until one answer is not unreachable. The
 * request ends as that answer (rc, r), or, when every host was unreachable, as the FIRST host's, with the largest
 * Retry-After any host gave. out may be NULL. */
int tf__ep_http(TfEndpoints *e, const TfEpReq *er, TfHttpRequest *q, TfHttpResponse *r, TfEpOut *out);
/* 4.0.2 (STEERING_40.md 34.6): the day's model requests per models host (at most 8 hosts a day), kept with the
 * telemetry state from the first start and sent in `hosts` while tf__telemetry_hosts is set (1: the server's
 * /v1/events takes `hosts` from 2026-11-21; 0 keeps them on the device). file_done: 1 this request brought the file's last byte; 2 only that (no
 * request of its own). ttfb_ms < 0: no status came. tf__dl_now: the clock of these timings (tests: tf__dl_clock). */
#define TF_HOSTS_MAX 8
extern int tf__telemetry_hosts;
extern double (*tf__dl_clock)(void);
double tf__dl_now(void);
void tf__client_count_host(TfClient *c, const char *host, int unreachable, double ttfb_ms, long long bytes, double body_s, int file_done);
const char *tf__hosts_problem(const TfJson *hosts); /* NULL when `hosts` passes the server's check */
/* 4.0.2 (ENDPOINTS_40.md 28.11): every route honours Retry-After. A host's 429 or 503 with a Retry-After (1 to 21600 s)
 * holds that route off that host until it passes (tf__ep_http and the file requests then answer for it, as that
 * answer with its error: a 429 ends the request, a 503 sends it to the next host). The route: tf__ep_route. The holds
 * and the optional routes' rows are kept across restarts in the file tf__ep_holds_file names (<storage>/holds.json). */
#define TF_EP_GATES 32
#define TF_EP_ROUTE 256 /* a route's name: a model file's path, or an API route */
void tf__ep_route(int role, const char *path, char out[TF_EP_ROUTE]);
void tf__ep_holds_file(TfEndpoints *e, const char *path); /* reads what an earlier process kept, then keeps it there */
void tf__ep_gate_note(TfEndpoints *e, int role, const char *path, const char *host, const TfHttpResponse *r);
long long tf__ep_gate(TfEndpoints *e, int role, const char *path, const char *host, int *slow, char err[33]); /* s left */
void tf__ep_gated(TfHttpResponse *r, int status, long long left, const char *err); /* the answer a hold stands for */
/* 28.11 item 4: optional traffic after a 429 (tf_endpoints.c); kind: TF_SLOW_*, -1 for another route */
enum { TF_SLOW_EVENTS = 0, TF_SLOW_ISSUES = 1, TF_SLOW_UPLOADS = 2 };
int tf__ep_optional(const char *path);
long long tf__ep_slow_429(TfEndpoints *e, int kind, int retry_after); /* a 429: the row grows; returns the wait */
void tf__ep_slow_ok(TfEndpoints *e, int kind);                        /* an accepted answer ends the row */
long long tf__ep_slow_left(TfEndpoints *e, int kind);                 /* seconds the route still waits, 0 none */
/* the thread's last request to Quran Lab's hosts: its role and whether more than one host was asked (24.2's host_role
 * and fallback); noted by tf__ep_http and the model download, read by the error reports */
void tf__ep_note(int role, int fallback);
int tf__ep_noted(int *role, int *fallback);
void tf__ep_note_clear(void);
const char *tf__ep_role_name(int role);
extern double tf__ep_now_override;                    /* tests: 28.3's clock in seconds (0: the monotonic clock) */
extern const char *(*tf__ep_test_origin)(const char *host); /* tests: a host's stand-in ("http://127.0.0.1:N"), NULL: none */
TfEndpoints *tf__client_endpoints(TfClient *c);       /* the Client's set, its lists brought to the config in force */
void tf__clean27(const char *v, char out[33]); /* APP_VERSION_40.md 27.1: app_version and app_build cleaned ("" for none) */
int _tf_strnicmp(const char *a, const char *b, size_t n);
/* The kind of connection the machine has, for the diagnose report only. Neither system gives this library a
 * dependable answer it can use without a new dependency (NetworkManager's D-Bus API on Linux; the Network List
 * Manager's COM object on Windows, which an app may have initialised differently), so the answer is always unknown.
 * 4.0.2: nothing reads it to decide whether or when a request goes out. */
enum { TF__CONNECTION_UNMETERED = 0, TF__CONNECTION_METERED = 1, TF__CONNECTION_NONE = 2, TF__CONNECTION_UNKNOWN = 3 };
extern int tf__connection_override; /* tests: one of the four instead of the platform's answer; -1: the platform's */
int tf__connection(void);
const char *tf__connection_name(int kind);
/* What the SDK runs on: the same words as the README's "Before you start" (Systems). The one place that says it. */
#define TF_RUNTIME_MINIMUM "Windows and Linux on x86-64"
/* 0, or TF_ERR_RUNTIME_UNSUPPORTED when this build is for a CPU the SDK does not support. On x86-64 every CPU
 * runs it: the int8 kernels fall back to their scalar tier and the engine's float side is plain C. */
int tf__runtime_check(void);

/* ---- heads ---- */
enum {
  TF_HEAD_LOG_PROBS = 0,  /* [n40, 38] final CTC, log-softmax */
  TF_HEAD_INTER1 = 1,     /* [n40, 38] stack-1 InterCTC */
  TF_HEAD_INTER4 = 2,     /* [n40, 38] stack-4 InterCTC (diagnostic) */
  TF_HEAD_ID = 3,         /* [n20, 38] identity CTC */
  TF_HEAD_TIMING = 4,     /* [n20, 38] H-time chain; 0 = pause */
  TF_HEAD_GEM = 5,        /* [n20, 2] single, held */
  TF_HEAD_TAFKHEEM = 6,   /* [n20, 3] thin, heavy, low_heavy */
  TF_HEAD_GHUNNA = 7,     /* [n20, 5] none, plain, geminate, ikhfa, idgham */
  TF_HEAD_QALQALAH = 8,   /* [n20, 3] none, sughra, kubra */
  TF_HEAD_FACE = 9,       /* [n20, 4] none, imala, tasheel, sakt */
  TF_HEAD_BOUNDARY = 10,  /* [n20] logit: a unit starts here */
  TF_HEAD_CONF = 11,      /* [n20] logit: the frame's label is truthful */
  TF_HEAD_COUNT = 12
};
#define TF_HEAD_BIT(h) (1u << (h))
/* The default selection (SPEC section 2): six heads. */
#define TF_HEADS_DEFAULT (TF_HEAD_BIT(TF_HEAD_LOG_PROBS) | TF_HEAD_BIT(TF_HEAD_INTER1) | TF_HEAD_BIT(TF_HEAD_TIMING) | \
                          TF_HEAD_BIT(TF_HEAD_GEM) | TF_HEAD_BIT(TF_HEAD_QALQALAH) | TF_HEAD_BIT(TF_HEAD_CONF))
#define TF_HEADS_ALL 0xFFFu
typedef struct {
  const char *name;     /* "log_probs", "inter1_log_probs", ..., "conf" (the graph's output names) */
  int width;            /* values per frame */
  int rate_ms;          /* 40 or 20 */
  const char *kind;     /* "log_softmax", "softmax" or "logit" */
  int n_classes;        /* names below, 0 for the token heads (use tokens.txt) and the logits */
  const char *const *classes;
} TfHeadInfo;
const TfHeadInfo *tf_head_info(int head);
int tf_head_from_name(const char *name); /* -1 when unknown */
/* "all", NULL (the default six) or names separated by commas; 0 and an error for an unknown name */
unsigned tf__heads_mask(const char *list);
/* One head's array inside the engine's heads (NULL when absent), and its frame count. */
const float *tf__heads_array(const TfRawHeads *h, int head, int *frames);
/* keep only the heads in `mask` */
void tf__heads_select(TfRawHeads *h, unsigned mask);

/* The Heads object: a view of the engine's heads (a chunk, or heads from anywhere in the tests) or, for a take
 * that has ended, copies it owns. */
struct TfHeads {
  TfRawHeads z;
  unsigned mask;          /* the heads it holds */
  const TfModel *m;       /* token names; may be NULL */
  float *own[TF_HEAD_COUNT]; /* copies (a finished take's), else NULL */
  const char *names[TF_HEAD_COUNT];
  int n_names;
  int top_k;              /* the decoded layer's depth: 0, or 2 to 38 */
  int is_final;
  TfDecoded *decoded;     /* built on demand, owned */
  int decoded_borrowed;   /* `decoded` belongs to the session */
  TfToken *greedy[TF_HEAD_COUNT]; /* built on demand, owned */
  int n_greedy[TF_HEAD_COUNT];
};
void tf__heads_view(TfHeads *out, const TfRawHeads *z, unsigned mask, const TfModel *m);
int tf__heads_own(TfHeads *out, const TfRawHeads *z, unsigned mask, const TfModel *m); /* copies; 0 ok */
void tf__heads_release(TfHeads *h); /* frees what it owns (not the struct) */

/* ---- decoded layer and top-K ---- */
/* The decoded layer of heads from anywhere (log_probs required; conf optional). k 0 or 2: no ranking beyond the
 * runner-up; 3 to 38: each frame's k best tokens too. top2 may be NULL (the engine's kept top-2 of a take, else
 * computed). Free with tf__decoded_free. */
TfDecoded *tf__decode(const TfModel *m, const TfRawHeads *heads, const TfTop2 *top2, int k);
void tf__decoded_free(TfDecoded *d);
void tf__decoded_mark(TfDecoded *d, int settled, int is_final);
/* greedy tokens of any ctc head: [frames, 38] log-probs at frame_ms; conf (20 ms logits, n20 of them) only for a
 * 40 ms head. Returns the count and the array (caller frees), or -1. */
int tf__greedy(const TfModel *m, const float *log_probs, int frames, int frame_ms, const float *conf, int n20,
               const TfTop2 *top2, TfFrame *frames_out, TfToken **out);

/* Per frame the k best entries of a token head or a class head: highest value first, equal values by the lower id
 * (a stable argsort). margins[j] = values[j] - the value ranked j + 1, as a double from the float values (also
 * when that one is beyond k), 0 for the last rank of the row. boundary and conf have no ranking. Arrays are
 * [frames, k], row major. */
typedef struct {
  int head;                 /* TF_HEAD_* */
  int frames, k, cols;
  const int *ids;
  const float *values;      /* log-probs or probabilities */
  const double *margins;
} TfTopK;
TfTopK *tf__topk(const TfRawHeads *heads, int head, int k);            /* heads from anywhere */
TfTopK *tf__session_topk(const TfSession *s, int head, int k);             /* a take so far (the engine's kept heads) */
#define TF_TAKE_MAX_S 1200 /* the most audio one take holds, in seconds at the input rate: then ended(limit) */
extern int tf__take_max_s; /* TF_TAKE_MAX_S; tests set a few seconds */
int tf__session_kept_peak(const TfSession *s); /* the most chunks a finish ever held back for delivery (tests) */
void tf__topk_free(TfTopK *t);
const char *tf__topk_json(TfTopK *t);

/* ---- the model ---- */
/* How the library opens a folder; the public TfModelOptions maps onto it. Zero is the default of every field. */
typedef struct {
  const char *trusted_key_hex; /* NULL: Quran Lab's key */
  int allow_unsigned;          /* tests: a manifest without a signature opens */
  int skip_file_checks;        /* tests: the engine files' hashes are not checked at open */
  int no_engine;               /* the engine is opened at the first Session, not now (folders without engine files) */
  const char *context;         /* the context to warm: NULL = the config's, then the manifest's default */
  int sample_rate;             /* the rate to warm at: 0 = 16000 */
  int threads, batch;
  int pin;                     /* 0 the engine's default, 1 pinned, -1 not pinned */
  TfClient *client;            /* the Client the Model belongs to (telemetry, sharing, lazy downloads), or NULL */
  const char *want_version;    /* the version the signed config named (checked against the signed one), or NULL */
  const char *want_build;      /* the build the signed config named (the manifest's SHA-256 must start with it) */
  const char *repo;            /* where its files come from (lazy contexts, the reference data) */
  const char *folder_path;     /* "sdk/v4.0" */
} TfModelOpen;
TfModel *tf__model_open(const char *folder, const TfModelOpen *o);
/* Tests, the local proof and the conformance tools only (SPEC 15.1): a model folder on disk, verified like a
 * downloaded one. Not in the public header and not exported from the shared library since 3.1.0 (it was
 * tf_model_open). trusted_key_hex: another manifest key for a folder signed with a test key, or NULL. */
TfModel *tf__model_open_folder(const char *folder, const TfModelOptions *options, const char *trusted_key_hex);
/* Tests only: tf_client_create with another Ed25519 key (64 hex) for the signed config and the manifest. */
TfClient *tf__client_create_with_key(const TfClientOptions *options, const char *trusted_key_hex);
TfClient *tf__client_create2_with_key(const TfClientOptions *options, const TfClientOptionsExt *ext, const char *trusted_key_hex);
const char *tf__model_dir(const TfModel *m);
TfClient *tf__model_client(const TfModel *m);
const TfJson *tf__model_manifest(const TfModel *m);
const char *tf__model_default_context(const TfModel *m);
int tf__model_has_context(const TfModel *m, const char *context);
/* The engine for a context and an input rate, with its stream: opened on first use, reopened when either differs
 * (never while a Session is open). NULL and an error otherwise. */
qlc_stream *tf__model_stream(TfModel *m, const char *context, int sample_rate);
qlc_model *tf__model_engine(TfModel *m);
/* the one open Session of the Model: set returns -1 when another is open */
int tf__model_set_session(TfModel *m, TfSession *s);
void tf__model_clear_session(TfModel *m, TfSession *s);
void tf__model_session_gone(TfModel *m);
/* 4.0.3: a call that uses the Model's engine (a take's start, a reference read) claims it: the folder's open model is
 * not given to another Model of the folder while a claim or a take of this one is open. Another Model's take is
 * waited for up to tf__open_wait_s (seconds), then TF_ERR_SESSION_OPEN. */
void tf__model_engine_claim(TfModel *m);
void tf__model_engine_unclaim(TfModel *m);
extern double tf__open_wait_s;
/* 4.0.3: the calling thread is an app's UI thread (Windows: IsGUIThread), where that wait is not made: session_open at
 * once. tf__ui_thread_hook: tests only, answers in its place */
int tf__ui_thread(void);
extern int (*tf__ui_thread_hook)(void);
double tf__model_load_ms(TfModel *m); /* how long the last engine open took; 0 once read */
/* a manifest entry as it is kept on disk (the decoded file of a gzip entry), by its local path; NULL when not listed */
const TfJson *tf__model_listed(const TfModel *m, const char *path);
/* the first file with this role (and context, when not NULL): its local path, or NULL */
const char *tf__model_role_path(const TfModel *m, const char *role, const char *context);
const char *tf__model_manifest_bytes(const TfModel *m, size_t *n);
TfPm *tf__model_pm(const TfModel *m);
void tf__pm_retain(TfPm *p);
void tf__pm_release(TfPm *p);
/* the recited index of the build (17.5), handed to the Core when the build has one and it is installed */
void tf__recited_index(TfClient *c, TfPm *p, const char *build_dir, const char *manifest, size_t n);

/* a clip's samples as floats (16-bit PCM in the folder, verified before it is read); the caller frees. Returns the
 * count, or a negative TfError. */
int tf__model_clip_samples(const TfModel *m, const char *name, float **out, int *rate);
/* The reference data of a Model (tf_reference.c): the file's place in the folder and, once opened, the table. */
struct TfReference {
  TfModel *m;    /* SDK 4.0: the expected units are read by the Core (qlc_reference_expected, R3) */
  int **ids, **word, *n, *n_words; /* per ayah of the Quran (6236, in order), made at its first read, kept while the
                                      Model is open: what tf_reference_units gave stays valid until the Model closes */
};
const TfJson *tf__model_role_entry(TfModel *m, const char *role);
int tf__model_entry_installed(TfModel *m, const TfJson *f);
qlc_model *tf__model_engine_for_reference(TfModel *m);
int tf__model_engine_release(TfModel *m);
void tf__reference_release(TfReference *r);

/* ---- the signed model manifest (tf_model.c; the downloader verifies with the same code) ---- */
extern const char *const TF_QURANLAB_KEY_HEX;
/* The text the signature covers; format 3 with with_tolerance carries the golden_tolerance lines. Caller frees. */
char *tf_manifest_signed_text(const TfJson *m); /* with the golden_tolerance lines */
char *tf_manifest_signed_text_ex(const TfJson *m, int with_tolerance);
int tf_manifest_has_tolerance(const TfJson *m);
/* 1 when the signature verifies with key_hex: with the golden_tolerance lines, or (field present) without them,
 * when *tolerance_signed (may be NULL) is 0 and the tolerance counts as absent. 0 otherwise. */
int tf_manifest_verify_signature_ex(const TfJson *m, const char *key_hex, int *tolerance_signed);
int tf_manifest_verify_signature(const TfJson *m, const char *key_hex);
/* 1 when dotted version v is min or newer */
int tf__version_at_least(const char *v, const char *min);
int tf__version_cmp(const char *a, const char *b); /* x.y.z by parts: -1, 0, 1; -2 when either is not a version */
char *tf__manifest_text(const TfJson *m, int with_tolerance, int with_raw);
int tf__manifest_verify(const TfJson *m, const char *key_hex, int *tolerance_signed, int *raw_signed);
int tf__golden_raw_build_ok(const TfJson *build_entry);
/* The acceptance checks of a manifest's bytes, in the order the shared vectors pin; 0 and the parsed manifest in
 * *out (the caller frees; out may be NULL), or the TfError of the first check that fails. */
int tf__manifest_accept(const char *bytes, size_t n, const char *want_build, const char *want_version, const char *key_hex, int allow_unsigned,
                        TfJson **out, char sha_hex[65], int *tolerance_signed, int *raw_signed);
/* 1 when s is a model version: x.y.z, digits only, no leading zeros, at most 4, 4 and 6 digits */
int tf__is_model_version(const char *s);

/* the take so far, as the Core keeps it (the stream's heads); 0 or -1 */
int tf__session_take_heads(const TfSession *s, TfRawHeads *out);
qlc_stream *tf__session_stream(const TfSession *s);

/* ---- the session ---- */
/* What one take counted, for the telemetry (tf_client.c; SPEC section 6). */
typedef struct {
  const TfModel *model;       /* the take's model (its version and build), or NULL */
  const char *version, *build;/* without a model: the cohort's key as text (tests), or NULL */
  const char *context;
  const TfDecoded *decoded;   /* optional: token margins and confidences */
  double audio_s, compute_s;  /* audio length and engine wall time of the take (6.1: minutes, latency, rtf) */
  int chunks;
  double model_load_ms;       /* > 0 when the take loaded the engine */
  int cold_start;             /* the first take of this launch */
  int aborted;                /* the take was cancelled or failed: it adds nothing to the 6.2 counters */
  int fallback;               /* 0 none, else a TF_FALLBACK_* reason */
  int feedback_shown;         /* the take ended with something for the learner to see (drives the sharing ask) */
  /* 6.2, for a take that ended with its Heads */
  long long audio_ms;         /* floor(1000 * input samples / input rate) */
  long long compute_us;       /* compute time of the take's chunks in whole microseconds */
  int speech_frames, silence_frames; /* 40 ms frames of log_probs */
  int end;                           /* how the take ended (SHARE_ASK_40.md 20.2): 0 normal, 1 interrupted, 2 an error, 3 cancelled */
  long long hour_ms[24];      /* 4.0 (17.8): ms of speech by the learner's local hour at each chunk */
  int tokens[TF_HEAD_COUNT];  /* greedy tokens per ctc head selected (0 for the others) */
} TfSessionStats;
enum { TF_FALLBACK_NONE = 0, TF_FALLBACK_NON_FINITE, TF_FALLBACK_DEADLINE, TF_FALLBACK_MODEL_LOAD, TF_FALLBACK_MEMORY,
       TF_FALLBACK_THERMAL, TF_FALLBACK_OTHER };

/* speech and silence from the greedy ids of log_probs and timing_log_probs (tf_speech.c) */
typedef struct {
  int window;    /* W: inactive frames in a row that end a speech span */
  int end_on;    /* end_on_silence_ms is set: the take ends at speechEnded */
  int in_speech; /* the state: 0 silence (the start of every take), 1 speech */
  int last;      /* the last active frame, -1 before the first */
  int frames;    /* frames fed so far */
  int active;    /* the frame fed last is active */
  int ended;     /* the take ended on silence */
} TfSpeech;
typedef struct {
  int type;      /* TF_EVENT_SPEECH_STARTED or TF_EVENT_SPEECH_ENDED */
  int at_ms, silence_ms;
} TfSpeechEvent;
void tf__speech_init(TfSpeech *s, int end_on_silence_ms);
/* 1 when a 40 ms frame is active: its log_probs id, or one of its two timing_log_probs ids, is non-blank */
int tf__speech_active(int id, int timing_a, int timing_b);
/* feeds one frame (active or not); returns 1 and fills *ev when the frame raises an event, else 0 */
int tf__speech_frame(TfSpeech *s, int active, TfSpeechEvent *ev);
/* the speech frames of a whole take: ids has T greedy ids of log_probs, timing 2 T of timing_log_probs */
int tf__speech_frames(const int *ids, const int *timing, int T);
int tf__token_runs(const int *ids, int T);    /* greedy tokens: runs of equal non-blank ids */
/* Which context a Session runs on (SPEC section 3): the app's choice (an unknown one is TF_ERR_INVALID_ARGUMENT),
 * else the config's inference.context when it is a string and one of the model's contexts, else the manifest's
 * default. config_json: the signed config's text, or NULL. Copies the context into out; *source (may be NULL) is
 * "app", "config" or "manifest". */
int tf__resolve_context(const char *app, const char *config_json, const char *const *contexts, int n_contexts, const char *default_context,
                        char *out, size_t out_n, const char **source);
/* the context the next Session of this Model uses when the app names none */
const char *tf__model_context_in_force(TfModel *m, char *out, size_t n);
/* A public call failed: its catalogue code becomes the process's last error (tf_client_diagnose). Returns rc. */
int tf__raise(int rc);
int tf__null_handle(const char *what);
int tf__last_raised(void);
qlc_stream *tf__session_engine(const TfSession *s);
int tf__ayah_count(int surah); /* ayahs of a surah (1 to 114), 0 otherwise */
const char *tf__event_name(int type);
const char *tf__ended_reason_name(int reason);
int tf__end_on_silence_ok(int ms);
/* the app's judgement of an ayah or a range (17.9), as kept: correct, or incorrect with mistakes and rules */
typedef struct {
  int set, correct, mistakes, n_rules;
  char rules[8][25];
} TfOutcomeX;
typedef struct {
  int ayah;
  TfOutcomeX o;
} TfPerAyah;
/* a take's recited report (SPEC 6.3, with its outcomes 17.9): what was reported last, and whether it has been
 * counted. 4.0: the report is the fallback, used only for a take the detector counted nothing for (17.1) */
typedef struct {
  int surah, ayah_from, ayah_to;
  int counted;
  TfOutcomeX whole;
  TfPerAyah *per; /* owned */
  int n_per;
} TfRecited;
int tf__recited_ok(int surah, int ayah_from, int ayah_to);
int tf__recited_report(TfRecited *r, TfClient *c, int state, int surah, int ayah_from, int ayah_to);
void tf__recited_end(TfRecited *r, TfClient *c, int counts);
char *tf__client_day_body(TfClient *c); /* today's counts as the delta that would be sent; caller frees */

/* ---- SDK 4.0: the install key store (tf_ikey.c), the Core's platform table (tf_platform.c), the lease (tf_lease.c) ---- */
typedef struct TfIkey TfIkey;
enum { TF_IKEY_TPM = 1, TF_IKEY_KSP = 2, TF_IKEY_FILE = 3 };
TfIkey *tf__ikey_open(const char *storage_dir, int create); /* NULL when the store cannot be used */
void tf__ikey_close(TfIkey *k);
int tf__ikey_kind(const TfIkey *k);
const char *tf__ikey_kind_name(const TfIkey *k); /* "tpm", "software", "file" */
int tf__ikey_hw(const TfIkey *k);                /* 1 for a TPM key (install_hw) */
const uint8_t *tf__ikey_spki(const TfIkey *k, int which, size_t *n); /* 0 agreement, 1 signing, 2 pending agreement */
const char *tf__ikey_cert(const TfIkey *k);
long long tf__ikey_cert_exp(const TfIkey *k);
const char *tf__ikey_key_id(const TfIkey *k);   /* the certificate's API key id */
int tf__ikey_agree(TfIkey *k, int pending, const uint8_t *eph, size_t n, uint8_t z[32]);
int tf__ikey_sign(TfIkey *k, int pending, const uint8_t *msg, size_t n, uint8_t *sig, size_t *sig_len);
int tf__ikey_rotate(TfIkey *k);
int tf__ikey_commit(TfIkey *k, const char *cert);
void tf__ikey_discard(TfIkey *k);
int tf__ikey_set_cert(TfIkey *k, const char *cert);
void tf__ikey_drop_cert(TfIkey *k);
void tf__ikey_destroy_keys(TfIkey *k); /* tests */
int tf__ikey_exportable(TfIkey *k);    /* tests */
void tf__spki_p256(const uint8_t xy[64], uint8_t spki[91]);
int tf__machine_key(uint8_t out[32]);
extern const uint8_t *tf__ikey_test_pkcs8;
extern size_t tf__ikey_test_pkcs8_n;
extern int tf__ikey_force_kind;

typedef struct TfPlat TfPlat;
TfPlat *tf__plat_new(const char *storage_dir, TfIkey *ik);
const qlc_platform *tf__plat_table(TfPlat *p, int pending);
void tf__plat_set_build(TfPlat *p, const char *build_dir);
char *tf__plat_path(TfPlat *p, int form, const char *path); /* caller frees; NULL for a path outside the folder */
uint8_t *tf__plat_state(TfPlat *p, size_t *n);
void tf__plat_free(TfPlat *p);
void tf__plat_counter_reset(TfPlat *p);
extern long long tf__plat_test_wall_ms, tf__plat_test_boot_ms;
extern const char *tf__plat_test_boot_id;
extern int tf__plat_test_no_counter;

typedef struct TfPm TfPm;
TfPm *tf__pm(TfClient *c); /* the Client's Core host, made at first use; NULL and an error when the key store fails */
qlc_core *tf__pm_core(TfPm *p);
TfPm *tf__pm_existing(TfClient *c); /* the Core host when the install has keys; NULL when not */
int tf__pm_lease_expired(int qlc_status, int detail);
TfIkey *tf__pm_ikey(TfPm *p);
TfPlat *tf__pm_plat(TfPm *p);
int tf__pm_base(TfPm *p, const char *build, char *out, size_t cap); /* the lease answer's base for build: 1 when known */
int tf__pm_error(int qlc_status, int detail); /* a Core status as the catalogue's error */
int tf__pm_attest(TfClient *c, TfPm *p);
int tf__pm_lease(TfClient *c, TfPm *p, const char *manifest, size_t mn, const TfJson *m, const char *build);
/* 18.4.7.1: 1 while the Core's expired lease is in its grace (until: exp + 86400 while a lease is held, else 0) */
int tf__pm_grace(TfPm *p, long long *until);
long long tf__pm_next_try(TfPm *p); /* tests: the background refresh's next attempt (unix s), 0 for at the next tick */
/* 4.0.2: the Client is being destroyed: a lease attempt in flight gets up to max_s (TF_CLOSE_LEASE_S) to end */
#define TF_CLOSE_LEASE_S 3.0
void tf__pm_closing(TfClient *c, double max_s);
/* tests (tf40_tests lifetime): counted frees and makes, an engine-less ended Session, the Client's Model as given out,
 * lease.json written as from a lease issued at iat */
extern int tf__model_frees, tf__pm_made;
TfSession *tf__session_test_ended(TfModel *m);
void tf__client_test_model(TfClient *c, TfModel *m, int holders);
void tf__pm_test_rec_save(TfPm *p, const char *build, long long iat);
/* 4.0.2: a Model's or Session's call that reaches the Client holds it (taken through the Model's link, NULL once the
 * Model is no longer the Client's); tf_client_destroy waits for the last hold to go */
void tf__client_hold(TfClient *c);
void tf__client_unhold(TfClient *c);
void tf__client_wait_unheld(TfClient *c);
TfClient *tf__model_client_hold(TfModel *m);
void tf__model_link_lock(TfModel *m);
void tf__model_link_unlock(TfModel *m);
void tf__model_detach(TfModel *m, int holders);
/* 4.0.2: a Model's memory stays while a Session of it lives; the stream a Session reads is its own only while its
 * generation is the one the Session began with */
int tf__model_closed(TfModel *m);
int tf__model_session_add(TfModel *m);
void tf__model_session_freed(TfModel *m);
unsigned tf__model_stream_gen(TfModel *m);
void tf__model_stream_lock(void);
void tf__model_stream_unlock(void);
qlc_stream *tf__model_stream_of(TfModel *m, unsigned gen);
int tf__pm_fails(TfPm *p);           /* tests: 18.4.7.3's failed attempts in a row */
long long tf__pm_lease_step(void);   /* tests: 18.4.7.3's wait after a failure (drawn afresh) */
void tf__pm_tick(TfClient *c, const char *build_dir, const char *build);
int tf__pm_install(TfPm *p, const char *build_dir, const char *manifest, size_t mn, const char *decoded_path);
qlc_model *tf__pm_open(TfPm *p, const char *build_dir, const char *manifest, size_t mn, const char *context, int threads, int rate, int pin);
void tf__pm_close(TfPm *p, qlc_model *m);
/* 4.0.3: the folder's one open model (tf_lease.c): its holder, under the host's lock (tf__pm_lock) */
TfModel *tf__pm_holder(TfPm *p);
void tf__pm_set_holder(TfPm *p, TfModel *m);
void tf__pm_wait(TfPm *p, long ms);              /* under the host's lock, taken once */
void tf__pm_notify(TfPm *p);
void tf__pm_close_held(TfPm *p, qlc_model *m);   /* under the host's lock */
int tf__pm_status(TfPm *p, qlc_lease_status *s);
void tf__pm_lock(TfPm *p);
void tf__pm_unlock(TfPm *p);
void tf__pm_client_gone(TfClient *c);
extern const char *tf__lease_test_base;
extern int tf__pm_open_fail_status; /* tests: the model open fails with this Core status (0: never) */
void **tf__client_pm_slot(TfClient *c);
int tf__client_call_lease(TfClient *c, const char *body, int (*taken)(void *user, const TfHttpResponse *r), void *user, TfHttpResponse *r);
int tf__client_call_taken(TfClient *c, const char *method, const char *path, const char *body, int auth,
                          int (*taken)(void *user, const TfHttpResponse *r), void *user, TfHttpResponse *r);
/* 18.4.7.1: the answer the last request of this thread got (status, 0 for none; the API's refusal: a JSON error
 * string) and this thread's request count; tf__http_answer_none records a request that never started */
unsigned tf__http_answer(int *status, int *api_error);
void tf__http_answer_none(void);
/* a deadline (tf__now_s) for this thread's API requests, 0 for none: what a model open waits for a lease attempt */
void tf__client_deadline(double at);
/* tests: how long a model open past the lease's exp waits for an attempt (18.4.7.1: 10 s), and the minute in which
 * one attempt's outcome serves the next open of this process (60 s) */
extern double tf__lease_wait_s, tf__lease_reuse_s;
void tf__client_set_token(TfClient *c, const char *token, long long expires_in);
void tf__client_refuse_api(TfClient *c, const char *code);
void tf__store_tick(TfClient *c);
void tf_b64url(const uint8_t *in, size_t n, char *out);

/* ---- the recited range (17.1, 17.6) and the app's report with its outcome (17.9) ---- */
/* what a take's measured or reported range adds to the day (tf_client.c) */
typedef struct {
  int source;             /* 1 measured, 2 the app's report, 0 nothing */
  int recovered;          /* finalised from the journal after a restart */
  int late;               /* the app's report of a measured take after it ended: its outcomes only */
  int context;            /* R5: the session context decided the measured range (17.8 recited_takes.context) */
  int ties;               /* R5: equally good ranges besides the one counted (a refrain tie; 0: none) */
  int surah, first, last; /* the range counted (never sent: only its counts are) */
  uint64_t take_id;       /* local only: a take is counted once */
  const char *day;        /* the take's UTC day (YYYY-MM-DD), NULL for today */
  const TfRecited *app;   /* 17.9: the app's report with its outcomes, paired with the counted ayahs; may be NULL */
} TfRecitedCount;
void tf__client_count_lease(TfClient *c, int outcome);
void tf__client_session_close(TfClient *c);
extern int tf__learner_test_kill;
/* the install's days (tf_learner.c: the Core's journal, qlc_days_*) */
#define TF_LEARNER_DAYS 40
typedef struct {
  char first[11];
  char active[TF_LEARNER_DAYS][11], local[TF_LEARNER_DAYS][11], sent[10][11];
  int n_active, n_local, n_sent;
  char streak_day[10][11];
  int streak_n[10], n_streaks;
  char week[6][11];
  long long week_judged[6], week_correct[6];
  int n_weeks;
  char trend_week[11];
  int session_open;
  long long session_ms;
} TfLearner;
void tf__learner_load(TfClient *c, TfLearner *j);          /* the days from the Core's journal, the open session */
int tf__learner_save_session(TfClient *c, const TfLearner *j); /* session.json only: the days are the Core's */
void tf__learner_mark_active(TfClient *c, TfLearner *j, const char *utc, const char *local);
int tf__learner_facts(const TfLearner *j, const char *day, int learner[5], char gap[8], char streak[8]);
void tf__learner_mark_sent(TfClient *c, TfLearner *j, const char *day);
void tf__learner_judged(TfClient *c, TfLearner *j, const char *day, int judged, int correct);
void tf__learner_trend_sent(TfClient *c, TfLearner *j, const char *week);
int tf__learner_trend(const TfLearner *j, const char *day, char out[16], char last_week[11]);
int tf__learner_tenure(const TfLearner *j, const char *day);
int tf__juz_of(int surah, int ayah);
long long tf__day_num(const char *day);
void tf__day_str(long long n, char out[11]);
void tf__day_monday(const char *day, char out[11]);
void tf__local_now(char day[11], int *hour);
extern long long tf__learner_test_tz_s;
/* the recited range through the Core (tf_recited.c) */
unsigned tf__recited_flags(TfClient *c);
int tf__recited_begin(TfClient *c, TfPm *p, uint64_t *id);
void tf__recited_feed(TfPm *p, const qlc_stream *s);
void tf__recited_report_pending(TfClient *c, uint64_t id, const TfRecited *r);
int tf__recited_finish(TfClient *c, TfPm *p, const qlc_stream *s, uint64_t id, const TfRecited *app, qlc_recited_result *out);
void tf__recited_cancel(TfClient *c, TfPm *p, uint64_t id);
/* 17.9's arguments, checked (an error and nothing kept when one is wrong) into a report */
int tf__recited_outcome(TfRecited *r, int surah, int ayah_from, int ayah_to, const TfRecitedOutcome *whole, const TfRecitedAyah *per, int n);
void tf__recited_clear(TfRecited *r);
void tf__client_count_take_range(TfClient *c, const TfRecitedCount *r);
int tf__client_take_counted(TfClient *c, uint64_t take_id); /* 1 when a take id was counted on one of the last 8 days */
int tf__client_metric_on(const TfClient *c, const char *group); /* flags.metrics.<group> is true */

/* ---- the client ---- */
/* How the library configures a client; the public TfClientOptions maps onto it. */
typedef struct {
  const char *key;            /* the developer key (desktop key) */
  const char *app_id;
  const char *platform;       /* "windows" or "linux"; NULL = this system */
  const char *app_name, *app_version;
  const char *api_url;        /* default https://api.quranlab.ai */
  const char *cdn_url;        /* default https://cdn.quranlab.ai/models */
  const char *storage_dir;    /* where the SDK keeps its state (config cache, counters, outbox, consent, models) */
  const char *trusted_key_hex;/* config and manifest signing key; NULL = Quran Lab's */
  const char *language;       /* copy deck language, BCP 47; NULL = "en" */
  void (*on_consent)(void *user, const TfConsentRequest *request);
  void *user;
  int debug_build;            /* 1 in the app's debug builds; 0: the client logs nothing */
  void (*on_log)(void *user, int level, const char *message);
  void *log_user;
  const char *app_build;      /* 4.0.1 (APP_VERSION_40.md 27.1): the app's build number; NULL: not known */
} TfClientConfig;
enum { TF_LOG_DEBUG = 0, TF_LOG_INFO = 1, TF_LOG_WARNING = 2, TF_LOG_ERROR = 3 };
TfClientConfig tf__client_default_config(void); /* anonymous telemetry is always on: only flags.analytics stops it */
/* The client itself, without the one-per-process rule and without its background thread (the tests drive it). */
TfClient *tf__client_open(const TfClientConfig *o);
void tf__client_close(TfClient *c);
/* Launch sequence: token, register (once per UTC day), config, telemetry left from earlier runs, due uploads. Each
 * step independent. */
int tf__client_launch(TfClient *c);
int tf__client_register(TfClient *c);
/* GET /v1/config, verified (Ed25519 over the exact body, key id, expiry); kept on disk. */
int tf__client_refresh_config(TfClient *c);
const char *tf__client_config_json(TfClient *c); /* the last verified config in force, or NULL */
/* One call per finished take: counts it for the day's telemetry and for the sharing ask. */
void tf__client_session_finished(TfClient *c, const TfSessionStats *st);
/* App-reported (SPEC 6.3): the ayahs ayah_from..ayah_to of a surah were recited. Validated by the caller. */
void tf__client_count_recited(TfClient *c, int surah, int ayah_from, int ayah_to);
/* Seals what was counted (include_today: also today's counts; 0: only earlier days) and sends every sealed delta,
 * oldest first. Returns the deltas accepted. */
int tf__client_flush(TfClient *c, int include_today);
int tf__client_background(TfClient *c);
int tf__client_report_issue(TfClient *c, const char *kind, const char *code, const char *message);
/* error reports of the process (tf_report.c): the Client attached at open and detached at close, the signed config in
 * force (flags.issues), the model in use or being fetched, the app in the foreground, a send point, the 30 s send */
void tf__rep_attach(TfClient *c, int first_process);
void tf__rep_detach(TfClient *c);
void tf__rep_config(TfClient *c);
void tf__rep_model(const char *version, const char *build);
void tf__rep_foreground(int fg);
int tf__rep_send(TfClient *c);
long long tf__rep_due(void);
char *tf__rep_body(TfClient *c, const TfRepItem *it);
char *tf__rep_state_json(void);
const char *tf__client_ram_tier(void);
void tf__client_rep_wake(TfClient *c);
int tf__client_os_major(void);
const char *tf__client_dir(const TfClient *c);
const TfClientConfig *tf__client_options(const TfClient *c);
void tf__client_log(TfClient *c, int level, const char *message);
/* the config's inference.context when it names a context (any string; the Model checks it), else NULL; the text
 * is copied into out */
const char *tf__client_inference_context(TfClient *c, char *out, size_t n);
/* the catalogue code of the last failure a public call of this client raised (0 when none) */
void tf__client_note_error(TfClient *c, int code);

const char *tf__client_api(const TfClient *c);
const char *tf__client_app(const TfClient *c);
const char *tf__client_platform(const TfClient *c);
const char *tf__client_cdn(const TfClient *c);
/* The flag tf__client_cancel_calls sets: a model download of this Client carries it. */
volatile int *tf__client_cancel_flag(TfClient *c);
/* The protocol the last model request of this Client ran on ("h3", "h2", "http/1.1"), for the diagnosis; "" before
 * the first. In memory only: never written, never sent. */
void tf__client_set_download_protocol(TfClient *c, const char *protocol);
void tf__client_download_protocol(TfClient *c, char out[12]);
const char *tf__client_key(const TfClient *c);
const char *tf__client_trusted_hex(const TfClient *c);
/* the key a model's manifest must be signed with: the client's trusted key (tests may name another) */
const char *tf__client_model_key(const TfClient *c);
/* the day's telemetry: one download attempt of a model build ended (bytes fetched now, whether it succeeded,
 * whether a file was resumed) */
void tf__client_count_download(TfClient *c, const char *version, const char *build, long long bytes, int ok, int resumed);
extern const char *tf__model_key_override; /* tests: the manifest's key when it is not the config's; NULL otherwise */
void **tf__client_api_slot(TfClient *c);
int tf__client_last_error(const TfClient *c);
const char *tf__client_stop_code(const TfClient *c);   /* the server's code that stopped this launch's calls, or "" */
const char *tf__client_stop_detail(const TfClient *c); /* sdk_too_old: the version the answer names, or "" */
int tf__client_stopped(const TfClient *c);
/* SPEC 15.3: the refusals of the key or the app that stop a model on the device (vectors/model_gate.json,
 * stop_reasons). 1 when `code` is one of them. */
int tf__stop_reason(const char *code);
int tf__key_reason(const char *code);                                /* key_revoked, key_unknown: the error is invalid_key */
int tf__body_stop_reason(const char *body, size_t n, char out[48]);  /* the stop reason an answer's body carries */
int tf__fail_gate(const TfClient *c);                                /* the remembered refusal as the app's error */
/* The refusal that stops the model, remembered on the device until a later answer accepts the key; "" for none. */
const char *tf__client_gate(const TfClient *c);
/* The CDN refused a model request with a stop reason: remembered like the API's refusal. */
void tf__client_refuse(TfClient *c, const char *code);
/* The API has just accepted the key: a refusal that came from a model request is asked again. */
void tf__client_gate_retry(TfClient *c);
/* SPEC 15.3: a refusal that stops a model also removes the models from the device: every
 * build folder, the install records, partial downloads and current.json; never the remembered refusal, the
 * device key, the sharing state or the outbox. tf__client_purge_models is the one way in for a running Client:
 * it removes them now, or leaves the removal owed while a Model is open (done when the Model is closed, and at
 * the next start). tf__store_purge does the removal; a removal that fails stays owed and is no error. */
void tf__client_purge_models(TfClient *c);
void tf__store_purge(TfClient *c);
int tf__client_purge_owed(const TfClient *c);
void tf__client_purge_set(TfClient *c, int owed);
extern int tf__store_purge_fail_test; /* tests: 1 the removal fails as a storage error would, and removes nothing */
/* SPEC 15.4: the app token for a model request, in out. renew: the CDN did not take the last one (401
 * invalid_token), so a new one is asked for. 0, or the client's error (no token: the request is not made). */
int tf__client_model_token(TfClient *c, int renew, char *out, size_t n);
/* SPEC 15.2: the device key of the install record (tf_devicekey.c). 0 with the key; 1 when there is none (and
 * create is 0); -1 when the key store failed, which is never treated as "no key". */
int tf__install_key(const char *dir, int create, uint8_t key[32]);
extern int tf__install_key_test; /* tests: 0 the real store; 1 "there is no key" for a read; -1 the store fails */
const char *tf__client_latest(const TfClient *c);      /* a newer SDK release the config names, or NULL */
void tf__client_set_pin(TfClient *c, const char *version);
int tf__client_pin_status(TfClient *c, char *out, size_t n);
const char *tf__client_config_state(TfClient *c);      /* "fresh", "stale" or "none" */
void tf__client_timer_start(TfClient *c);
void tf__client_check_latest(TfClient *c);
int tf__client_count_checks(TfClient *c, const char *rule, int checked, int flagged);
int tf__rule_name_ok(const char *name);
int tf__client_call(TfClient *c, const char *method, const char *path, const char *body, int auth, const char *extra,
                    TfHttpResponse *r);
int tf__client_flag(const TfClient *c, const char *name, int dflt);
int tf__client_flag_int(const TfClient *c, const char *name, int lo, int hi, int dflt);
int tf__client_telemetry_on(const TfClient *c);
void tf__telemetry_add(TfClient *c, const char *field, long long n);
extern int tf__client_no_timer; /* tests: no background threads */
char *tf__share_session_json_test(int rate, int flac); /* tests: a session JSON (tf_share.c) */
extern uint32_t tf__share_serial; /* tests: the Ogg serial of the next recordings (0: random) */

/* ---- when SDK 4.0 asks a learner to share (tf_share_ask.c; SHARE_ASK_40.md section 20) ---- */
enum { TF_ASK_UNKNOWN = 0, TF_ASK_GRANTED = 1, TF_ASK_DECLINED = 2, TF_ASK_FINAL = 3 };            /* consent */
enum { TF_ASK_NONE = 0 }; /* an ask's kind: none, or the public TF_ASK_SHEET and TF_ASK_CARD */
enum { TF_ASK_EV_LAUNCH = 1, TF_ASK_EV_TAKE_START, TF_ASK_EV_TAKE_END, TF_ASK_EV_ASK_NOW, TF_ASK_EV_SHOWN, TF_ASK_EV_ANSWER,
       TF_ASK_EV_WITHDRAWN, TF_ASK_EV_SETTING, TF_ASK_EV_UNKNOWN };
enum { TF_ASK_END_NORMAL = 0, TF_ASK_END_OTHER = 1 };
enum { TF_ASK_ANSWER_OTHER = 0, TF_ASK_YES, TF_ASK_NOT_NOW, TF_ASK_OPEN };
typedef struct {
  int first_min_sessions, first_min_hours, reask_days[2], reask_sessions[2], good_session_min_s, max_asks;
} TfAskConfig;
typedef struct {
  int kind, number, shown; /* kind 0: no ask in hand */
} TfAskHand;
typedef struct {           /* this app session's part: in memory only */
  long long takes, recited_ms;
  int asked, take_open;
  TfAskHand ask;
} TfAskApp;
typedef struct {
  int consent;
  long long asks, sessions, last_ask_session;
  long long installed_at, last_ask_at, last_ask_day; /* unix ms, unix ms, the calendar day */
  int has_installed_at, has_last_ask_at, has_last_ask_day;
  TfAskApp app;
} TfAskState;
typedef struct {
  int type, end, offered, on, answer;
  long long speech_ms;
} TfAskEvent;
void tf__ask_config(const TfJson *raw, TfAskConfig *c);
long long tf__ask_local_day(long long ms, int tz_min);
int tf__ask_decide(TfAskState *s, const TfAskEvent *ev, const TfJson *cfg_raw, long long now_ms, int tz_min, const char **why);
const char *tf__ask_consent_name(int c);
int tf__ask_consent_of(const char *name);
void tf__ask_read_field(TfAskState *s, const char *key, const TfJson *v);
void tf__ask_state_json(const TfAskState *s, TfBuf *b);
void tf__ask_event_of(const TfJson *e, TfAskEvent *ev);
/* tf_sharing.c: the events at their hooks (20.10) */
int tf__sharing_event(TfClient *c, const TfAskEvent *ev, const char **why);
void tf__sharing_settle_check(TfClient *c);
void tf__sharing_take_start(TfClient *c);
void tf__sharing_background(TfClient *c);
void tf__sharing_foreground(TfClient *c);
void tf__sharing_session(TfClient *c, int end, long long speech_frames); /* end: 0 normal, 1 interrupted, 2 an error, 3 cancelled */
int tf__sharing_answer_ask(TfClient *c, int yes);
int tf__sharing_open(TfClient *c);
int tf__sharing_shown(TfClient *c);
extern long long tf__ask_now_ms; /* tests: the wall clock of the ask (0: the system's) */
extern int tf__ask_tz_min;       /* tests: its UTC offset in minutes (-100000: the device's) */
extern const char *const *tf__sharing_test_card; /* tests: card strings (line, open, not now) in place of the deck's; {NULL}: none */
extern long long tf__now_override; /* tests: a fixed clock (unix seconds), 0 for the system's */

/* ---- the model downloader (tf_download.c) ---- */
/* tests: the pause before a resume (16.2) is given to this instead of being slept (NULL: slept) */
extern void (*tf__download_on_pause)(long long ms);
/* tests (tf40_tests endpoints): one file of a build's folder `path` fetched through 16.2's resumes and 28.4's hosts on
 * the set `ep`, to `to` (its .part beside it); key and size and SHA-256 as a manifest names them */
int tf__download_file_test(TfEndpoints *ep, const char *path, const char *rel, long long size, const char *sha_hex, const char *to);
typedef struct {
  const char *cdn_url;        /* tests without a Client: the models role's one host and the folder's prefix; NULL: the
                                 built-in hosts and /models (4.0.1: a Client's downloads use the Client's hosts) */
  const char *base_hint;      /* 4.0.1: the base of the lease the install holds for this build (the manifest's folder) */
  const char *key;            /* developer key (qlk_v1_...) */
  TfClient *client;           /* the Client whose app token every request carries beside the key (SPEC 15.4); NULL
                                 in the downloader's own tests: the key alone */
  const char *repo;           /* "Quran-Lab/tajweedformer-v4" */
  const char *revision;       /* the build, from the signed config's models entry */
  const char *folder;         /* "sdk/v4.0" */
  const char *dest;           /* local folder to fill (resumable; .part files kept between runs) */
  const char *contexts;       /* comma list; NULL = every context */
  const char *trusted_key_hex;
  const char *only_role;      /* NULL: the CPU set and the sample clips; else only the files of this role ("reference") */
  const char *want_version;   /* the version the signed config named: the signed manifest must carry it; NULL: any */
  int check_build;            /* 1: the manifest's SHA-256 must start with `revision` (a 40 hex build id) */
  void (*progress)(void *user, long long done, long long total); /* bytes of the files asked for, as stored */
  long long *fetched;         /* out, may be NULL: += the bytes that came over the network in this call */
  int *resumed;               /* out, may be NULL: set to 1 when a file went on, with a Range request, from a partial
                                 file that an earlier call left (a resume inside this call does not set it) */
  void *user;
  volatile int *cancel;       /* may be NULL: tf__http_cancel(cancel) from another thread ends the request in flight
                                 and the pause before a resume, and the download returns TF_ERR_DOWNLOAD_FAILED */
  /* SDK 4.0: after the manifest is accepted and before any file (the lease); 0 or an error that ends the download.
   * It writes the model folder the lease answer named (base, ending in '/'): the files are read from its path on the
   * models hosts (4.0.1, its host first when it is not one of them), and with none no file is downloaded. Without
   * this hook (the downloader's own tests) the files come from the manifest's folder. */
  int (*after_manifest)(void *user, const char *manifest, size_t n, const TfJson *m, char *base, size_t base_cap);
  /* SDK 4.0: a qle1 container is downloaded and verified: the Core installs it (CDN form to device form) */
  int (*install)(void *user, const char *decoded_path, const char *manifest, size_t n);
  int want_recited;           /* the recited index (role recited, 17.5) is fetched too */
  void *hook_user;            /* after_manifest's and install's */
  const char *want_key_id;    /* 4.0.1, may be NULL: the build's key id the signed config names (18.4.5); a manifest
                                 that names another is refused before any file is asked for */
} TfDownload;
TfDownload tf__download_defaults(void);
/* A model request ends when no byte of its answer arrives for this long (30 s). It is not a limit on the whole
 * transfer: a slow link that keeps delivering is never cut. Tests set a shorter one. */
extern int tf__download_stall_ms;
/* The pause before the first resume of a file inside one call (1 s); it doubles before each further resume
 * (1, 2, 4, 8, 16 s). Tests set a shorter one. */
extern int tf__download_pause_ms;
/* 4.0.1: at most this many files of one call download at once, each on its own connection (4). Tests may set 1. */
extern int tf__download_lanes;
/* 4.0.1: 1 (the default): with a lease hook, the files are fetched from the build's folder on the model host while
 * the lease is asked, and used when the lease answer names that folder; 0: only after the lease, from its folder. */
extern int tf__download_early;
/* tests: how many files were read again after their last byte (only a transport that gives no on_data makes one) */
extern int tf__download_rereads;

/* ---- 4.0.1, Windows: the engine's threads are never power-throttled (tf_qos.c; no-ops elsewhere) ---- */
typedef struct {
  unsigned long ids[512];
  int n, full, taken;
} TfThreadSnap;
typedef struct {
  int set;
  unsigned long control, state;
} TfQosSave;
extern int tf__qos_off;                          /* measurements and tests: 1 is the 4.0.0 behaviour */
void tf__qos_snapshot(TfThreadSnap *s);          /* this process's threads now */
int tf__qos_mark_new(const TfThreadSnap *before); /* every thread since `before` gets high QoS; how many */
void tf__qos_enter(TfQosSave *q);                /* the calling thread gets high QoS until tf__qos_leave */
void tf__qos_leave(const TfQosSave *q);          /* puts back what the calling thread had */
int tf__qos_thread_high(void);                   /* tests: 1 high QoS, 0 not, -1 unknown */
/* Fetches and verifies the manifest, then every file of the profile (never the expected table, CoreML or NPU
 * files), resuming partial files with HTTP Range and checking each SHA-256. 0, or a TfError. */
int tf__download_model(const TfDownload *o);

/* ---- model files of a client (tf_store.c) ---- */
/* Downloads (resumable) and verifies the build Quran Lab's config names for this app; out_dir receives the
 * folder. progress may be NULL. 0, or a catalogue code. */
typedef struct {
  const char *version;        /* a pin, or NULL */
  void (*on_progress)(void *user, long long done, long long total);
  void *user;
  const char *context;        /* the context whose files are needed: NULL = the one in force */
} TfFetch;
int tf__store_fetch(TfClient *c, const TfFetch *f, char *out_dir, size_t n, TfModelOpen *open);
/* one more context, or the reference data, of a model that came from the client */
int tf__store_fetch_role(TfClient *c, const TfModel *m, const char *context, const char *role);
void tf__store_prune(TfClient *c, const char *keep_build);
char *tf__store_json(TfClient *c, long long *models, long long *sharing); /* caller frees */
int tf__store_remove(TfClient *c);
int tf__store_current(TfClient *c, char version[24], char build[41]);

/* ---- sharing (tf_sharing.c, tf_share.c) ---- */
typedef struct TfShareSession TfShareSession;
#define TF_SHARING_BITRATE_DEFAULT 192000 /* bit/s: the ladder's top and start */
#define TF_SHARING_JOIN_MS_DEFAULT 3000   /* the join window without flags.share_join_ms */
#define TF_SHARING_JOIN_MS_MAX 60000      /* the longest window a config may set (SDK 4.0, section 23; 3.x: 10000) */
#define TF_SHARING_RECORDING_MAX_S 1200   /* the most audio one recording holds: it closes there, also inside a take */
#define TF_SHARING_RING_MS 1000           /* the feed's ring: the last second fed while no take runs, in memory only */
#define TF_SHARING_TAIL_MS 500            /* fed audio recorded after a take ends, at most */
#define TF_SHARING_TAIL_WAIT_MS 1500      /* how long after a take's end its tail can still take fed audio */
#define TF_SHARING_HOLD_MS 30000          /* a joining take is held in memory when the recording has this much room or less */
#define TF_SHARING_FEED_STALE_MS 500      /* a feed call later than its own audio plus this after the audio before it: the feed paused */
#define TF_SHARING_GAP_PREV_MAX_MS 3600000 /* gap_ms_prev is left out above an hour */
/* tests: where the samples of the last feed call went, the ring's samples and allocated bytes now, and the
 * pre-roll the last take started with */
void tf__sharing_deleted(TfClient *c); /* what a confirmed "Delete what I shared" does on the device */
void tf__share_feed_stats(TfClient *c, long long *to_tail, long long *to_ring, long long *dropped, long long *ring_samples,
                          long long *ring_bytes, long long *pre_roll);
const char *tf__sharing_deck_json(const char *language); /* the bundled deck (consent sheet, settings, thank-you) */
int tf__client_sharing_answer(TfClient *c, int share);
int tf__client_sharing_set(TfClient *c, int on);
int tf__client_sharing_state(TfClient *c);              /* 1 on (granted, to any version), 0 off or not granted */
const char *tf__client_sharing_status_json(TfClient *c);
int tf__client_sharing_delete(TfClient *c);
int tf__client_sharing_ask_now(TfClient *c);
const char *tf__sharing_consent_version(TfClient *c);
const char *tf__sharing_consent_text(TfClient *c);
const char *tf__sharing_consent_sha256(TfClient *c);
const char *tf__client_sharing_copy_json(TfClient *c);
int tf__client_sharing_recovery_code(TfClient *c, char out[33]);
const char *tf__client_sharing_summary_json(TfClient *c);
/* A take begins: it joins the client's open recording or starts a new one. NULL when sharing is off. */
TfShareSession *tf__sharing_session_begin(TfClient *c, int input_sample_rate, const char *input_route);
/* the same for a take of a Model: its version and build go into the session JSON (either may be NULL) */
TfShareSession *tf__share_begin(TfClient *c, int input_sample_rate, const char *input_route, const char *version, const char *build);
int tf__sharing_session_push(TfShareSession *s, const float *samples, int n); /* mono, [-1, 1] */
/* The end of the take (completed 1), or a cancelled one (0). Frees the session; returns the take's milliseconds
 * that went into a recording. */
int tf__sharing_session_finish(TfShareSession *s, int completed);
int tf__sharing_recording_close(TfClient *c);
int tf__sharing_set_bitrate(TfClient *c, int bits_per_second);
int tf__sharing_bitrate(TfClient *c);
char *tf__sharing_outbox_json(TfClient *c);   /* caller frees */
int tf__sharing_outbox_pending(TfClient *c);
int tf__sharing_outbox_resume(TfClient *c);
int tf__sharing_outbox_wait(TfClient *c, int timeout_ms);
long long tf__share_bytes(TfClient *c);       /* bytes of the sharing queue on disk */

double tf__now_s(void);
const char *tf__platform_name(void);

#endif
