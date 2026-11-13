/* Internal declarations of the tajweedformer library (not installed). The public API is include/tajweedformer.h
 * and nothing else; the library's tests reach what is below by including this header and linking the static
 * library. */
#ifndef TF_INTERNAL_H
#define TF_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "tajweedformer.h"
#include "zf_tajweed.h"

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
/* the embedded catalogue file (sdk_3.0.0/errors.json, byte for byte) and its recorded SHA-256 */
const unsigned char *tf__errors_json(size_t *n);
const char *tf__errors_sha256(void);

/* a model file's URL under a download base (tf_download.c) */
int tf__model_url(char *out, size_t n, const char *base, const char *repo, const char *revision, const char *folder, const char *rel);

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
  TfHttpConn *conn;     /* optional: the kept connection this request goes on */
  volatile int *cancel; /* optional: tf__http_cancel(cancel) from another thread aborts the request */
  int (*accept)(void *user, const TfHttpResponse *r); /* optional: asked when the status and the headers are
                           in and before any file is opened. 0: this is not the answer that was asked for;
                           nothing of it is written and the request fails */
  void (*on_head)(void *user, const TfHttpResponse *r); /* optional: the status and the headers are in; for a
                           download the file is open (emptied by a 200) and no byte of the body is written yet */
  void (*on_bytes)(void *user, long long n); /* optional: called as bytes of a download reach its file */
  void *bytes_user;     /* the user pointer of on_head and on_bytes */
} TfHttpRequest;
/* Sets *flag and aborts every request in flight that carries it (WinHTTP: its handle is closed; libcurl: the
 * progress callback stops the transfer). */
void tf__http_cancel(volatile int *flag);
/* 0: got a response (any status). On a failure the status and the headers that had arrived stay in resp.
 * No request follows a redirect (SPEC 15.4): a 3xx answer is the response, and neither a header nor the body
 * of the request goes to the place it names. */
int tf_http(const TfHttpRequest *req, TfHttpResponse *resp);
void tf_http_response_free(TfHttpResponse *r);
/* Tests: the system's transport (WinHTTP, libcurl) whatever transport is set, for a test's transport that
 * answers some requests itself and passes the others on. */
int tf__http_native(const TfHttpRequest *req, TfHttpResponse *resp);
/* Tests: replace the transport. */
typedef int (*TfHttpTransport)(const TfHttpRequest *req, TfHttpResponse *resp, void *user);
void tf_http_set_transport(TfHttpTransport t, void *user);
int _tf_strnicmp(const char *a, const char *b, size_t n);
/* 1 when the system says the connection in use is metered (Windows); 0 when not or when it cannot tell */
/* The kind of connection the machine has. Neither system gives this library a dependable answer it can use
 * without a new dependency (NetworkManager's D-Bus API on Linux; the Network List Manager's COM object on
 * Windows, which an app may have initialised differently), so the answer is always unknown, the diagnose report
 * says so, and an unmetered-only prefetch downloads (it waits on metered and none, which only a test sets). */
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
const float *tf__heads_array(const ZfTajweedHeads *h, int head, int *frames);
/* keep only the heads in `mask` */
void tf__heads_select(ZfTajweedHeads *h, unsigned mask);

/* The Heads object: a view of the engine's heads (a chunk, or heads from anywhere in the tests) or, for a take
 * that has ended, copies it owns. */
struct TfHeads {
  ZfTajweedHeads z;
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
void tf__heads_view(TfHeads *out, const ZfTajweedHeads *z, unsigned mask, const TfModel *m);
int tf__heads_own(TfHeads *out, const ZfTajweedHeads *z, unsigned mask, const TfModel *m); /* copies; 0 ok */
void tf__heads_release(TfHeads *h); /* frees what it owns (not the struct) */

/* ---- decoded layer and top-K ---- */
/* The decoded layer of heads from anywhere (log_probs required; conf optional). k 0 or 2: no ranking beyond the
 * runner-up; 3 to 38: each frame's k best tokens too. top2 may be NULL (the engine's kept top-2 of a take, else
 * computed). Free with tf__decoded_free. */
TfDecoded *tf__decode(const TfModel *m, const ZfTajweedHeads *heads, const ZfTajweedTop2 *top2, int k);
void tf__decoded_free(TfDecoded *d);
void tf__decoded_mark(TfDecoded *d, int settled, int is_final);
/* greedy tokens of any ctc head: [frames, 38] log-probs at frame_ms; conf (20 ms logits, n20 of them) only for a
 * 40 ms head. Returns the count and the array (caller frees), or -1. */
int tf__greedy(const TfModel *m, const float *log_probs, int frames, int frame_ms, const float *conf, int n20,
               const ZfTajweedTop2 *top2, TfFrame *frames_out, TfToken **out);

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
TfTopK *tf__topk(const ZfTajweedHeads *heads, int head, int k);            /* heads from anywhere */
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
const char *tf__model_dir(const TfModel *m);
TfClient *tf__model_client(const TfModel *m);
const TfJson *tf__model_manifest(const TfModel *m);
const char *tf__model_default_context(const TfModel *m);
int tf__model_has_context(const TfModel *m, const char *context);
/* The engine for a context and an input rate, with its stream: opened on first use, reopened when either differs
 * (never while a Session is open). NULL and an error otherwise. */
ZfTajweedStream *tf__model_stream(TfModel *m, const char *context, int sample_rate);
ZfTajweedModel *tf__model_engine(TfModel *m);
/* the one open Session of the Model: set returns -1 when another is open */
int tf__model_set_session(TfModel *m, TfSession *s);
void tf__model_clear_session(TfModel *m, TfSession *s);
void tf__model_session_gone(TfModel *m);
double tf__model_load_ms(TfModel *m); /* how long the last engine open took; 0 once read */
/* a manifest entry as it is kept on disk (the decoded file of a gzip entry), by its local path; NULL when not listed */
const TfJson *tf__model_listed(const TfModel *m, const char *path);
/* the first file with this role (and context, when not NULL): its local path, or NULL */
const char *tf__model_role_path(const TfModel *m, const char *role, const char *context);

/* a clip's samples as floats (16-bit PCM in the folder, verified before it is read); the caller frees. Returns the
 * count, or a negative TfError. */
int tf__model_clip_samples(const TfModel *m, const char *name, float **out, int *rate);
/* The reference data of a Model (tf_reference.c): the file's place in the folder and, once opened, the table. */
struct TfReference {
  TfModel *m;
  char path[96];      /* the file's local path in the folder */
  void *table;        /* the engine's table once opened */
  int *ids, *word;    /* the last lookup's arrays */
  int cap;
};
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
ZfTajweedStream *tf__session_engine(const TfSession *s);
int tf__ayah_count(int surah); /* ayahs of a surah (1 to 114), 0 otherwise */
const char *tf__event_name(int type);
const char *tf__ended_reason_name(int reason);
int tf__end_on_silence_ok(int ms);
/* a take's recited report (SPEC 6.3): what was reported last, and whether it has been counted */
typedef struct {
  int surah, ayah_from, ayah_to;
  int counted;
} TfRecited;
int tf__recited_ok(int surah, int ayah_from, int ayah_to);
int tf__recited_report(TfRecited *r, TfClient *c, int state, int surah, int ayah_from, int ayah_to);
void tf__recited_end(TfRecited *r, TfClient *c, int counts);
char *tf__client_day_body(TfClient *c); /* today's counts as the delta that would be sent; caller frees */

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
  int telemetry;              /* 1: anonymous telemetry (deltas of the day's sums); flags.analytics can stop it */
  int children;               /* the app's audience is children (consent addressed to a guardian) */
  const char *language;       /* copy deck language, BCP 47; NULL = "en" */
  void (*on_consent)(void *user, const TfConsentRequest *request);
  void *user;
  int debug_build;            /* 1 in the app's debug builds; 0: the client logs nothing */
  void (*on_log)(void *user, int level, const char *message);
  void *log_user;
} TfClientConfig;
enum { TF_LOG_DEBUG = 0, TF_LOG_INFO = 1, TF_LOG_WARNING = 2, TF_LOG_ERROR = 3 };
TfClientConfig tf__client_default_config(void); /* telemetry 1 */
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
extern long long tf__now_override; /* tests: a fixed clock (unix seconds), 0 for the system's */

/* ---- the model downloader (tf_download.c) ---- */
typedef struct {
  const char *cdn_url;        /* the model base: default https://cdn.quranlab.ai/models ({base}/{repo}/{build}/{folder}/...) */
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
} TfDownload;
TfDownload tf__download_defaults(void);
/* A model request ends when no byte of its answer arrives for this long (30 s). It is not a limit on the whole
 * transfer: a slow link that keeps delivering is never cut. Tests set a shorter one. */
extern int tf__download_stall_ms;
/* The pause before the first resume of a file inside one call (1 s); it doubles before each further resume
 * (1, 2, 4, 8, 16 s). Tests set a shorter one. */
extern int tf__download_pause_ms;
/* Fetches and verifies the manifest, then every file of the profile (never the expected table, CoreML or NPU
 * files), resuming partial files with HTTP Range and checking each SHA-256. 0, or a TfError. */
int tf__download_model(const TfDownload *o);

/* ---- model files of a client (tf_store.c) ---- */
/* Downloads (resumable) and verifies the build Quran Lab's config names for this app; out_dir receives the
 * folder. progress may be NULL. 0, or a catalogue code. */
typedef struct {
  const char *version;        /* a pin, or NULL */
  int unmetered_only;
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
#define TF_SHARING_JOIN_MS_MAX 10000      /* the longest window a config may set */
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
