/* tajweedformer.h: the TajweedFormer SDK for Windows and Linux (C API, SDK 2.0.2).
 *
 * The exact engine (bit for bit the reference arithmetic) runs the V4 TajweedFormer model from a
 * verified model folder (sdk/v4.0), and this library gives every layer of what it computes:
 *
 *   raw      every head of the network per chunk (log_probs, inter1/inter4, id, timing, gem, the four
 *            attribute heads, boundary, conf), selectable per stream
 *   decoded  per 40 ms frame the top-1 and runner-up token with their log-probs and margin; the greedy
 *            transcript with per-token peak log-prob, runner-up, margin and confidence
 *   tajweed  for a known surah:ayah: per-site verdicts with per-head and fused LLRs, madd counts,
 *            qalqalah and gemination evidence, per-word scores and flags, the settings in force
 *
 * Settings resolve app code > the portal's tajweed block (signed /v1/config) > calibration.json.
 * The same concepts, names and numbers as the Android, Apple and Web SDKs.
 *
 *   TfModelOptions o = tf_model_default_options();
 *   TfModel *m = tf_model_open("models/sdk/v4.0", &o);          verified: manifest, signature, hashes
 *   tf_settings_set_app_json(m, "{\"preset\":\"sensitive\"}");   optional
 *   TfStream *s = tf_stream_create(m, NULL);
 *   tf_stream_push_audio(s, pcm, n);  ...  tf_stream_finish(s);
 *   TfDecoded *d = tf_stream_decode(s);                          what was heard
 *   TfResult *r = tf_stream_score(s, 1, 2);                      tajweed verdicts for 1:2
 *   puts(tf_result_json(r));
 *   tf_result_free(r); tf_decoded_free(d); tf_stream_destroy(s); tf_model_close(m);
 *
 * Threads: a TfModel may be shared for scoring and decoding. Settings calls and streams belong to one
 * thread at a time. The engine allows ONE live stream per process (its operators keep process-wide
 * state); a second tf_stream_create fails with TF_ERR_BUSY until the first is destroyed. The server
 * tier runs one stream per worker process (the `tajweedformer serve`/`bench` commands).
 *
 * Errors: functions return NULL or a negative TF_ERR_* value; tf_last_error() gives the message of the
 * calling thread's last failure. Strings returned as `const char *` belong to the object they came from
 * and stay valid until that object changes or is freed; `char *` results are freed with tf_free. */
#ifndef TAJWEEDFORMER_H
#define TAJWEEDFORMER_H

#include <stddef.h>

#include "zf_tajweed.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(TF_BUILD_DLL)
#define TF_API __declspec(dllexport)
#elif defined(_WIN32) && defined(TF_USE_DLL)
#define TF_API __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#define TF_API __attribute__((visibility("default")))
#else
#define TF_API
#endif

#define TF_SDK_VERSION "2.0.2"
#define TF_API_VERSION 1

typedef enum {
  TF_OK = 0,
  TF_ERR_ARGUMENT = -1,
  TF_ERR_IO = -2,
  TF_ERR_MANIFEST = -3,          /* manifest.json missing or malformed */
  TF_ERR_SIGNATURE = -4,         /* the manifest or config signature does not verify */
  TF_ERR_SIGNATURE_REQUIRED = -5,/* unsigned, and a signature is required */
  TF_ERR_CHECKSUM = -6,          /* a file's size or SHA-256 differs from the manifest */
  TF_ERR_NOT_LISTED = -7,        /* a file the folder needs is not listed */
  TF_ERR_SDK_TOO_OLD = -8,       /* min_sdk_version above this SDK's version */
  TF_ERR_UNSUPPORTED_MODEL = -9, /* not a V4 TajweedFormer folder (format, family, engine_format) */
  TF_ERR_CALIBRATION = -10,
  TF_ERR_ENGINE = -11,
  TF_ERR_SETTINGS = -12,         /* an app setting is invalid (app code is strict) */
  TF_ERR_BUSY = -13,             /* another stream is live in this process */
  TF_ERR_NETWORK = -14,
  TF_ERR_HTTP = -15,             /* the server answered with an error status */
  TF_ERR_NO_MEMORY = -16,
  TF_ERR_NOT_AVAILABLE = -17,    /* not possible here (for example uploads without attestation) */
  TF_ERR_CONFIG = -18            /* a remote config failed verification (signature, key id, expiry) */
} TfError;

TF_API const char *tf_version(void); /* "2.0.2" */
TF_API int tf_api_version(void);
TF_API const char *tf_error_name(int code); /* "signature_invalid", ... */
TF_API const char *tf_last_error(void);     /* this thread's last failure, or NULL */
TF_API int tf_last_error_code(void);
TF_API void tf_free(void *p);

/* ============================================================================ heads (raw layer) */

typedef enum {
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
} TfHead;

#define TF_HEAD_BIT(h) (1u << (h))
/* What the scorer reads; a stream always keeps these. */
#define TF_HEADS_SCORER (TF_HEAD_BIT(TF_HEAD_LOG_PROBS) | TF_HEAD_BIT(TF_HEAD_INTER1) | \
                         TF_HEAD_BIT(TF_HEAD_TIMING) | TF_HEAD_BIT(TF_HEAD_GEM) | TF_HEAD_BIT(TF_HEAD_QALQALAH))
/* The default selection: the scorer's heads and conf (the decoded layer's confidence). */
#define TF_HEADS_DEFAULT (TF_HEADS_SCORER | TF_HEAD_BIT(TF_HEAD_CONF))
#define TF_HEADS_ALL 0xFFFu

typedef struct {
  const char *name;     /* "log_probs", "inter1_log_probs", ..., "conf" (the graph's output names) */
  int width;            /* values per frame */
  int rate_ms;          /* 40 or 20 */
  const char *kind;     /* "log_softmax", "softmax" or "logit" */
  int n_classes;        /* names below, 0 for the token heads (use tokens.txt) and the logits */
  const char *const *classes;
} TfHeadInfo;
TF_API const TfHeadInfo *tf_head_info(int head);
TF_API int tf_head_from_name(const char *name); /* -1 when unknown */
/* One head's array inside a ZfTajweedHeads (NULL when absent), and its frame count. */
TF_API const float *tf_heads_get(const ZfTajweedHeads *h, int head, int *frames);

/* ============================================================================ the model */

typedef struct TfModel TfModel;

typedef struct {
  size_t struct_size;          /* sizeof(TfModelOptions) */
  const char *context;         /* NULL: the portal's (when set before open via remote_json) or calibration's default */
  int threads;                 /* engine threads, 0 = the fast cores */
  int sample_rate;             /* rate of pushed audio: 0/16000, or 8000..48000 through the engine's resampler */
  int batch;                   /* chunks run together for stored audio (same bits); 0/1 for live audio */
  int load_engine;             /* 1 (default): streams; 0: scoring heads from elsewhere only */
  int require_signature;       /* 1 (default): the manifest must be signed by a trusted key */
  int verify_files;            /* 1 (default): check every listed file's size and SHA-256 at open */
  const char *trusted_key_hex; /* raw Ed25519 public key (64 hex); NULL = Quran Lab's key */
  int debug_build;             /* 1 lets app code turn on checks whose calibration status is "off" */
  const char *remote_json;     /* optional: the portal's /v1/config (verified) or its tajweed block */
  /* 1: the engine does not pin its threads to cores (many streams in many processes: the server tier; the
   * `tajweedformer serve`, `bench` and `worker` commands set it). 0 (default): the engine's pinning, best for one
   * stream. ZfTajweedOpenOptions.no_pin of the engine (API 3). -1 (default): the engine's platform default
   * (phones pin, Windows, Linux and macOS do not). Bits are the same either way. */
  int no_pin;
} TfModelOptions;

TF_API TfModelOptions tf_model_default_options(void);
/* Opens a V4 folder: manifest format 2, family tajweedformer, engine_format 2, min_sdk_version <= TF_SDK_VERSION,
 * signature (by default required), every listed file's size and SHA-256; never opens an unlisted file. */
TF_API TfModel *tf_model_open(const char *folder, const TfModelOptions *options);
TF_API void tf_model_close(TfModel *m);
/* {"model_id","revision","family","signed","key_id","contexts":[...],"context","default_context",
 *  "chunk_ms","vocab","blank","tokens":[...],"presets":[{"name","label","site_fa"}],"word_presets":[...],
 *  "heads":[{"name","width","rate_ms","kind","classes"}],"sdk_version"} */
TF_API const char *tf_model_info_json(TfModel *m);
TF_API const char *tf_model_context(const TfModel *m);
TF_API int tf_model_chunk_ms(const TfModel *m);
TF_API const char *tf_model_token_name(const TfModel *m, int id); /* "<blk>", "|", ... */
/* The engine's own handles, for anything this header does not wrap. */
TF_API ZfTajweedModel *tf_model_engine(TfModel *m);
TF_API ZfTajweedConfig *tf_model_engine_config(TfModel *m);

/* ============================================================================ settings */

/* App code (strict: an invalid value is TF_ERR_SETTINGS and nothing changes). Replaces the whole app layer;
 * NULL clears it. {"preset": "balanced", "overrides": {"cons_sub": {"preset": "sensitive"},
 * "vowel_sub": {"threshold": 1.25}}, "checks": {"qalqalah": true}, "word": {"preset": "general"} or
 * {"word": {"threshold": 4.5}}}. ("context" is chosen at open.) */
TF_API int tf_settings_set_app_json(TfModel *m, const char *json);
/* The portal's layer: a whole /v1/config document or its "tajweed" block, as received. Lenient: invalid fields
 * are dropped (listed in the effective settings' "dropped"), never the block. Returns the number dropped, or
 * TF_ERR_ARGUMENT for text that is not JSON (the layer is then cleared). Use tf_client_* for the verified fetch. */
TF_API int tf_settings_set_remote_json(TfModel *m, const char *json, size_t len);
/* "int8" (the exact engine's heads) or "fp32" (heads from elsewhere); NULL = the default for this model. */
TF_API int tf_settings_set_backend(TfModel *m, const char *backend);
/* The effective settings with each value's source: the engine's resolution plus "word". */
TF_API const char *tf_settings_json(TfModel *m);

/* ============================================================================ streams */

typedef struct TfStream TfStream;
typedef struct TfClient TfClient;

typedef struct {
  int index;                   /* chunk number in the take, from 0 */
  int start_ms, end_ms;        /* the chunk's 40 ms frames on the take's clock */
  int frame0;                  /* first 40 ms frame of the chunk in the take */
  const ZfTajweedHeads *heads; /* this chunk's selected heads (others NULL); valid during the call */
} TfChunk;
typedef void (*TfChunkCallback)(void *user, const TfChunk *chunk);

typedef struct {
  size_t struct_size;
  unsigned heads;              /* TF_HEAD_BIT mask COMPUTED, kept and handed over (the scorer's heads are always
                                  added); 0 = TF_HEADS_DEFAULT; TF_HEADS_ALL = all. A head not asked for is not
                                  computed at all (the engine's lazy heads, API 3) and comes back NULL */
  TfChunkCallback on_chunk;    /* optional, called on the pushing thread after every chunk */
  void *user;
  /* Recitation sharing (optional): with a platform client whose learner agreed to share, every take this stream
   * listens to is shared in full in the background (tf_sharing_session_*); nothing happens otherwise. */
  TfClient *client;
  const char *input_route;     /* built_in, wired, usb, bluetooth, car, continuity, other, unknown (default) */
} TfStreamOptions;
TF_API TfStreamOptions tf_stream_default_options(void);

TF_API TfStream *tf_stream_create(TfModel *m, const TfStreamOptions *options);
TF_API void tf_stream_destroy(TfStream *s);
TF_API int tf_stream_push_audio(TfStream *s, const float *samples, int n); /* chunks run, or < 0 */
TF_API int tf_stream_finish(TfStream *s);                                    /* the end of the take */
TF_API void tf_stream_reset(TfStream *s);                                    /* a new take */
/* The take's selected heads so far (plus the scorer's), valid until the next push, finish or reset. */
TF_API int tf_stream_heads(const TfStream *s, ZfTajweedHeads *out);
/* The engine's greedy transcript ids (equal to tf_decoded's token ids). Returns the count. */
TF_API int tf_stream_greedy(const TfStream *s, int *ids, int max);

/* ============================================================================ decoded layer */

typedef struct {
  int top1;                 /* largest log-prob, lowest id on ties */
  float top1_log_prob;
  int runner_up;            /* largest of the other 37, lowest id on ties */
  float runner_up_log_prob;
  double margin;            /* top1_log_prob - runner_up_log_prob */
} TfFrame;

typedef struct {
  int id;                   /* token id (tokens.txt) */
  int frame0, frame1;       /* 40 ms frames of the run, inclusive */
  int start_ms, end_ms;     /* 40 * frame0, 40 * (frame1 + 1) */
  int peak_frame;           /* the run's frame with the largest top-1 log-prob */
  float log_prob;           /* at the peak */
  int runner_up;            /* at the peak */
  float runner_up_log_prob;
  double margin;
  double confidence;        /* mean sigmoid(conf) over the token's 20 ms frames; NAN without conf */
} TfToken;

typedef struct {
  int first_token, n_tokens;
  int start_ms, end_ms;
  double confidence;        /* min over its tokens (NAN without conf) */
  double margin;            /* min over its tokens */
} TfSegment;                /* tokens between "|" (V4 rarely emits "|": use the tajweed layer's words) */

typedef struct TfDecoded TfDecoded;
TF_API TfDecoded *tf_decode(const TfModel *m, const ZfTajweedHeads *heads); /* m may be NULL (no unit names) */
TF_API TfDecoded *tf_stream_decode(const TfStream *s);
TF_API void tf_decoded_free(TfDecoded *d);
TF_API int tf_decoded_frames(const TfDecoded *d, const TfFrame **frames);
TF_API int tf_decoded_tokens(const TfDecoded *d, const TfToken **tokens);
TF_API int tf_decoded_segments(const TfDecoded *d, const TfSegment **segments);
/* {"frames":[...],"tokens":[{"id","unit",...}],"words":[...],"transcript"} (frames only with with_frames; each
 * frame carries "ranked": its k best tokens, see tf_decode_k) */
TF_API const char *tf_decoded_json(TfDecoded *d, int with_frames);
/* The decoded layer to depth k (2 to 38; tf_decode is k = 2): each frame also ranks its k best tokens of
 * log_probs (tf_decoded_ranked). */
TF_API TfDecoded *tf_decode_k(const TfModel *m, const ZfTajweedHeads *heads, int k);
TF_API TfDecoded *tf_stream_decode_k(const TfStream *s, int k);
/* The per-frame ranking of the decoded layer: [frames, k] ids, log-probs and margins. Returns k. */
TF_API int tf_decoded_ranked(const TfDecoded *d, const int **ids, const float **log_probs, const double **margins);

/* ============================================================================ top-K (any ranked head) */

/* Per frame the k best entries of a token head (log_probs, inter1, inter4, id, timing: 38 tokens) or a class
 * head (gem 2, attr_tafkheem 3, attr_ghunna 5, attr_qalqalah 3, attr_face 4: tf_head_info(head)->classes):
 * highest value first, equal values by the lower id (a stable argsort). margins[j] = values[j] - the value
 * ranked j + 1, as a double from the float values (also when that one is beyond k), 0 for the last rank of
 * the row. boundary and conf have no ranking. Arrays are [frames, k], row major. */
typedef struct {
  int head;                 /* TF_HEAD_* */
  int frames, k, cols;
  const int *ids;
  const float *values;      /* log-probs or probabilities */
  const double *margins;
} TfTopK;
/* For heads from anywhere (zf_tajweed_topk). NULL on error (an absent head, k out of 1..cols). */
TF_API TfTopK *tf_topk(const ZfTajweedHeads *heads, int head, int k);
/* For a stream's take so far (zf_tajweed_stream_topk): the head must be computed by the stream (its selection). */
TF_API TfTopK *tf_stream_topk(const TfStream *s, int head, int k);
TF_API void tf_topk_free(TfTopK *t);
/* {"head","k","cols","classes":[...]?,"ids":[[...]],"values":[[...]],"margins":[[...]]} (valid until freed) */
TF_API const char *tf_topk_json(TfTopK *t);

/* ============================================================================ tajweed layer */

typedef struct {
  int word;                 /* word index on the scored path (the engine's site.word) */
  int has_score;
  double score;             /* the largest clipped fused LLR over every single edit inside the word (engine API 3;
                               with an API 2 engine: over the word's site alternatives) */
  int evidence_site;        /* index into tf_result_engine's site array, -1 when no site */
  int has_threshold;        /* calibration.json carries a word block */
  double threshold;
  int flagged;              /* score >= threshold; -1 unknown (no word calibration or the check off), 0, 1 */
  int start_ms, end_ms;     /* the word's sites' frames on the best path; -1 unknown */
  int n_flagged_sites, n_flagged_madd, n_flagged_qalqalah, n_flagged_gemination;
  /* the best edit (engine API 3): its class, kind (ZF_TJ_EDIT_*), the unit edited, the unit put in (-1 for a
   * deletion) and the per-head LLRs; -1 / NAN when the word has no score */
  int evidence_class, evidence_edit, evidence_expected, evidence_alt;
  double evidence_llr_final, evidence_llr_inter1;
} TfWordScore;

typedef struct TfResult TfResult;
/* Score a take of surah:ayah (the whole ayah). backend NULL: the settings' backend. */
TF_API TfResult *tf_score(TfModel *m, const ZfTajweedHeads *heads, int surah, int ayah, const char *backend);
TF_API TfResult *tf_stream_score(TfStream *s, int surah, int ayah);
TF_API void tf_result_free(TfResult *r);
TF_API const ZfTajweedResult *tf_result_engine(const TfResult *r); /* sites, madd, qalqalah, gemination */
TF_API int tf_result_words(const TfResult *r, const TfWordScore **words);
/* {"surah","ayah","context","backend","settings","sites":[...],"madd":[...],"qalqalah":[...],"gemination":[...],
 *  "words":[...],"summary":{...},"raw":<the engine's JSON>} */
TF_API const char *tf_result_json(TfResult *r);

/* ============================================================================ model downloads */

typedef void (*TfProgressCallback)(void *user, const char *path, long long done, long long total);

typedef struct {
  size_t struct_size;
  const char *cdn_url;        /* the model base: default https://cdn.quranlab.ai/models ({base}/{repo}/{revision}/{folder}/...);
                                 https://cdn.quranlab.ai still works (its /hf/ path), and the signed config's base fits here */
  const char *key;            /* developer key (qlk_v1_...) */
  const char *repo;           /* "Quran-Lab/tajweedformer-v4" */
  const char *revision;       /* commit, from the signed config's models entry */
  const char *folder;         /* "sdk/v4.0" */
  const char *dest;           /* local folder to fill (resumable; .part files kept between runs) */
  const char *contexts;       /* comma list; NULL = every context (the native profile: the full CPU set) */
  const char *trusted_key_hex;
  TfProgressCallback progress;
  void *user;
} TfDownloadOptions;
TF_API TfDownloadOptions tf_download_default_options(void);
/* Fetches and verifies the manifest, then every file of the profile (CPU engine files and common files;
 * never CoreML or NPU files), resuming partial files with HTTP Range and checking each SHA-256. */
TF_API int tf_download_model(const TfDownloadOptions *o);

/* ============================================================================ portal (platform client) */

/* Consent: Quran Lab's signed config decides whether sharing is offered to the app (flags.contributions); the
 * learner decides on the sheet. The SDK asks at the right moment (after a finished session with feedback, never on
 * first launch) and shows the sheet at most three times per install, ever: "not now" may lead to a re-ask >= 7
 * sessions later until the third showing; a "share" answer or turning sharing off ends asking for good, across copy
 * decks, wording and SDK upgrades (a grant keeps the version it was given to). This library has no UI of its own
 * (so there is nothing to theme): the app shows its own sheet with the SDK's strings (copy_json below, or
 * tf_client_sharing_copy_json, with the app's own wording applied) and reports the learner's tap through
 * tf_client_sharing_answer, now or later. Every ask is counted (the status' asks, telemetry share_asked) and the
 * re-ask rules are the SDK's. Nothing is recorded or sent before a "share" answer. */
typedef struct {
  int reask;                /* 0: the first ask, 1 or 2: a gentle re-ask */
  int guardian;             /* the app's audience is children: address the parent or guardian */
  const char *copy_json;    /* the copy deck for the client's language, with the app's own wording applied */
  /* 2.0.1 (read only by apps built against 2.0.1 or later): */
  int update;               /* deprecated: always 0 since 2.0.1 (a grant persists across decks and wording) */
  const char *consent_version; /* the version a "share" answer grants (the deck's, or custom-<24 hex>) */
} TfConsentRequest;
typedef void (*TfConsentCallback)(void *user, const TfConsentRequest *req);

/* The client's log (2.0.1): messages for the developer, never for the learner, never sent anywhere. */
typedef enum { TF_LOG_DEBUG = 0, TF_LOG_INFO = 1, TF_LOG_WARNING = 2, TF_LOG_ERROR = 3 } TfLogLevel;
typedef void (*TfLogCallback)(void *user, int level, const char *message);

typedef struct {
  size_t struct_size;
  const char *key;            /* the developer key from portal.quranlab.ai (desktop key) */
  const char *app_id;         /* Windows package family name / AppUserModelID, or Linux reverse-DNS id */
  const char *platform;       /* "windows" or "linux"; NULL = this system */
  const char *app_name, *app_version;
  const char *api_url;        /* default https://api.quranlab.ai */
  const char *storage_dir;    /* where the SDK keeps its state (config cache, counters, outbox, consent) */
  const char *trusted_key_hex;/* config signing key; NULL = Quran Lab's */
  int telemetry;              /* 1 (default): anonymous telemetry (deltas of the day's sums); flags.analytics can stop it */
  int contributions;          /* ignored since 2.0.1: Quran Lab's signed config decides per app whether sharing is
                               offered (flags.contributions); the learner still decides on the sheet */
  int children;               /* the app's audience is children (consent addressed to a guardian) */
  const char *language;       /* copy deck language, BCP 47; NULL = "en" */
  TfConsentCallback on_consent;
  void *user;
  /* 2.0.1 (apps built against 2.0.0 leave these 0 through struct_size): */
  int debug_build;            /* 1 in the app's debug builds (as TfModelOptions.debug_build), for example
                                 #ifndef NDEBUG co.debug_build = 1; #endif. The library is always built as a release,
                                 so the app says which build it is. 0: the client logs nothing */
  TfLogCallback on_log;       /* where the log goes in debug builds; NULL: stderr */
  void *log_user;
} TfClientOptions;

TF_API TfClientOptions tf_client_default_options(void);
TF_API TfClient *tf_client_create(const TfClientOptions *o);
TF_API void tf_client_destroy(TfClient *c);
/* Launch sequence: token, register (once per UTC day), config, telemetry left from earlier runs, due uploads. Each
 * step independent; never blocks recognition. Opening a client counts one launch in the telemetry. */
TF_API int tf_client_launch(TfClient *c);
/* POST /v1/register at most once per UTC day per install (a day stamp in the storage directory, written after a
 * 200); a second call the same day returns 0 without a request. */
TF_API int tf_client_register(TfClient *c);
/* GET /v1/config, verified (Ed25519 over the exact body, key id, expiry); kept on disk. */
TF_API int tf_client_refresh_config(TfClient *c);
TF_API const char *tf_client_config_json(TfClient *c); /* the last verified config, or NULL */
/* The verified config may carry "sdk_latest": {"version","severity","url"} for the app's platform (2.0.1). When that
 * version is newer than this SDK's (dotted numbers; anything else is not newer), a debug build (debug_build 1) logs
 * once per client, at TF_LOG_WARNING: "Quran Lab SDK X is available (severity), you have Y: URL". Release builds
 * stay silent; a missing or malformed field is ignored. Unknown top-level fields of the config are ignored. */
/* Puts the verified config's tajweed block into the model's portal layer. */
TF_API int tf_client_apply_config(TfClient *c, TfModel *m);
/* Telemetry (anonymous, docs/platform-client.md): one call per finished session. The SDK reads the numbers it needs
 * from the session's model, decoded layer and result (no audio, no text, no ids leave the device: only sums and
 * histograms per UTC day). It sends deltas: what was counted since the previous delta of the same UTC day, sealed
 * with a random report_id and kept on disk until the server takes it (a retry sends the same bytes). Send points:
 * every 15 minutes while a client is open (its own timer once tf_client_launch ran, or the next
 * tf_client_session_finished), tf_client_background, tf_client_destroy, and tf_client_launch (leftovers). */
typedef struct {
  size_t struct_size;
  const TfModel *model;       /* the session's model (revision, context, preset, word level, checks) */
  const TfDecoded *decoded;   /* optional: token margins and confidences */
  const TfResult *result;     /* optional: classes, madd, words, flag rate, coverage (surah) */
  double audio_s, compute_s;  /* audio length and engine wall time of the session */
  int chunks;
  double model_load_ms;       /* > 0 when the session loaded the model */
  int cold_start;             /* the first session of this launch */
  int aborted;                /* the session never finished */
  int fallback;               /* 0 none, else a TF_FALLBACK_* reason */
  int feedback_shown;         /* the learner saw the model's feedback (drives the sharing ask) */
} TfSessionStats;
enum { TF_FALLBACK_NONE = 0, TF_FALLBACK_NON_FINITE, TF_FALLBACK_DEADLINE, TF_FALLBACK_MODEL_LOAD, TF_FALLBACK_MEMORY,
       TF_FALLBACK_THERMAL, TF_FALLBACK_OTHER };
/* The developer opt-out in code (never a prompt): TfClientOptions.telemetry = ZF_ANALYTICS_OFF. */
#define ZF_ANALYTICS_OFF 0
TF_API void tf_client_session_finished(TfClient *c, const TfSessionStats *st);
/* Seals what was counted (include_today: also today's counts; 0: only earlier days) and sends every sealed delta,
 * oldest first. Returns the deltas accepted. */
TF_API int tf_client_flush(TfClient *c, int include_today);
/* The app goes to the background or idle (a window minimized, the app about to sleep): seals and sends a delta now.
 * Returns the deltas accepted, or < 0. */
TF_API int tf_client_background(TfClient *c);
TF_API int tf_client_report_issue(TfClient *c, const char *kind, const char *code, const char *message);
/* Sharing. */
TF_API const char *tf_sharing_copy_json(const char *language); /* the bundled deck (consent sheet, settings, thank-you) */
/* The learner's answer to a sheet (the SDK's ask through on_consent or a pending ask, shown in the app's own UI):
 * 1 share, 0 not now. Only from the learner's own tap. Counted as share_yes / share_no. */
TF_API int tf_client_sharing_answer(TfClient *c, int share);
TF_API int tf_client_sharing_set(TfClient *c, int on);        /* the settings switch (not a sheet answer); off stops uploads at once */
TF_API int tf_client_sharing_state(TfClient *c);              /* 1 on (granted, to any version), 0 off or not granted */
/* {"consent","enabled","asks","sessions","pendingAsk","thanked","guardian","sharedSessions","uploadTrust","mayUpload",
 *  "consentVersion" (agreed to, what shared sessions carry),"consentTextSha256","effectiveConsentVersion" (what a new
 *  grant would record),"updateNeeded" (deprecated: always false since 2.0.1)} */
TF_API const char *tf_client_sharing_status_json(TfClient *c);
TF_API int tf_client_sharing_delete(TfClient *c);             /* "Delete what I shared" (device-key deletion) */
/* The app chooses the moment (timing only): no ask once asking ended (a yes, sharing off, three sheets shown). */
TF_API int tf_client_sharing_ask_now(TfClient *c);
/* The app's own wording (2.0.1): replaces deck strings by key ("sheet.what", "impact.minutes.other", ...: the deck's
 * keys, impact.minutes by plural category). An unknown key, an empty or blank text, text that is not UTF-8 or a key
 * given twice is TF_ERR_ARGUMENT and nothing changes; n = 0 (or keys NULL) clears. With overrides the learner agrees
 * to that text: the consent version becomes custom-<the first 24 hex digits of its SHA-256> (the canonical text of
 * docs/platform-client.md) and every session shared under a grant to it carries consent_text_sha256. A learner
 * who agreed earlier keeps the version recorded then and is not asked again; new grants record the new version.
 * Set it right after tf_client_create, before tf_client_launch, at every start. Disclosure lines must stay honest (the doc). */
TF_API int tf_sharing_set_wording(TfClient *c, const char *const *keys, const char *const *texts, int n);
/* Read only, for the developer to send to Quran Lab: the effective consent version, the canonical text and its
 * SHA-256 (64 hex). The text and hash are NULL without overrides. Valid until the next tf_sharing_set_wording. */
TF_API const char *tf_sharing_consent_version(TfClient *c);
TF_API const char *tf_sharing_consent_text(TfClient *c);
TF_API const char *tf_sharing_consent_sha256(TfClient *c);
/* The client's copy deck (its language) with the app's own wording applied and the effective consent_version: what
 * the app's own sheet shows. Without overrides, the bundled deck itself. */
TF_API const char *tf_client_sharing_copy_json(TfClient *c);
/* "Show my recovery code" (26 Crockford characters in groups of 4): for the learner only, behind the app's own
 * confirmation; used at portal.quranlab.ai/delete when the device is lost. TF_ERR_NOT_AVAILABLE before the first
 * shared session. */
TF_API int tf_client_sharing_recovery_code(TfClient *c, char out[33]);
/* GET /v1/contributions/summary: {"you":{"sessions","minutes"},"app":...|null,"all":...}, or NULL. */
TF_API const char *tf_client_sharing_summary_json(TfClient *c);
/* The shared-session upload (PLATFORM_CLIENT.md section 10; the 2.0.0 design): with consent on, each session the SDK
 * listens to is encoded as one Ogg Opus stream (libopus, 48 kHz mono, 20 ms frames, complexity 10, VBR), cut into
 * segments of 10, 20, 40, then 60 s, and uploaded in the background (presigned PUTs to R2, a thread of the client)
 * from an on-disk outbox in storage_dir/outbox that survives a crash or restart (tf_client_launch resumes it).
 * Desktop uploads at the capped trust tier: flags.upload_trust[windows|linux] absent or "capped" = upload; "off"
 * or "attested" = none. Turning sharing off (tf_client_sharing_set 0) aborts the PUT in flight and deletes the
 * unsent sessions at once. Streams created with TfStreamOptions.client do all of this by themselves; the calls
 * below are for apps that capture audio outside a TfStream. */
/* Bitrate (2.0.1): an adaptive ladder of 192000, 96000, 64000 and 32000 bit/s, starting at the top for each client.
 * An upload's network failure (no connection, a timeout, HTTP 408 or 5xx) steps down; a closed segment while 120 s
 * or more of closed audio waits steps down; three segment uploads in a row while at most 30 s waits step up. The
 * rung in force when a segment opens is that segment's bitrate (tf_sharing_outbox_json "segmentBitrates"). */
#define TF_SHARING_BITRATE_DEFAULT 192000 /* bit/s: the ladder's top and start */
typedef struct TfShareSession TfShareSession;
/* NULL (TF_ERR_NOT_AVAILABLE) when sharing is off: no consent, the option, flags.contributions or upload_trust. */
TF_API TfShareSession *tf_sharing_session_begin(TfClient *c, int input_sample_rate, const char *input_route);
TF_API int tf_sharing_session_push(TfShareSession *s, const float *samples, int n); /* mono, [-1, 1] */
/* The end of the session (completed 1), or a cancelled one (0: kept only from 2 s). Frees the session; returns
 * the milliseconds shared (0: nothing). */
TF_API int tf_sharing_session_finish(TfShareSession *s, int completed);
/* No effect since 2.0.1 (the ladder decides, the same in every SDK); kept for compatibility: 0, or TF_ERR_ARGUMENT
 * outside 6000 to 510000. */
TF_API int tf_sharing_set_bitrate(TfClient *c, int bits_per_second);
TF_API int tf_sharing_bitrate(TfClient *c); /* the bitrate the next segment opens with (bit/s) */
/* The outbox's sessions, oldest first: [{"id","created","ended","completed","segments" (closed),"uploaded",
 * "segmentBitrates":[bit/s per closed segment, in order],"error"}]. Free with tf_free; NULL on error. */
TF_API char *tf_sharing_outbox_json(TfClient *c);
TF_API int tf_sharing_outbox_pending(TfClient *c);                     /* sessions not uploaded yet */
TF_API int tf_sharing_outbox_resume(TfClient *c);                      /* start or wake the uploader */
/* Wakes the uploader and waits (at most timeout_ms) for a full pass; returns the sessions still pending. */
TF_API int tf_sharing_outbox_wait(TfClient *c, int timeout_ms);

/* PLATFORM_CLIENT.md's C name for the old sharing option. No effect since 2.0.1: the signed config decides. */
TF_API void zf_platform_set_contributions(int on);

#ifdef __cplusplus
}
#endif
#endif /* TAJWEEDFORMER_H */
