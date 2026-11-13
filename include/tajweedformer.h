/* tajweedformer.h: the TajweedFormer SDK for Windows and Linux (C API, SDK 3.1.2).
 *
 * A pure inference engine: audio in, the model's own output out. Five things, with the same names and the same
 * order of calls as the Android, Apple and web SDKs:
 *
 *   Client   the app's link to Quran Lab (the key, the signed config, the model files, telemetry, sharing);
 *            one per process
 *   Model    one verified model build on this device (version, contexts, tokens, calibration and reference
 *            data, sample clips); it holds the engine
 *   Session  one take: audio in, chunks and events out, the take's Heads at the end
 *   Heads    raw model output for a chunk or a whole take: every selected head, greedy tokens, the decoded layer
 *   Backend  what runs the model (here always the CPU engine)
 *
 *   static void on_chunk(void *user, const TfChunk *chunk) { puts(chunk->decoded->text); }
 *
 *   TfClient *client = tf_client_create(&(TfClientOptions){.key = "qlk_v1_...", .app_id = "com.example.app"});
 *   TfModel *model = tf_client_model(client, NULL);                    downloaded once, verified, opened, warm
 *   TfSession *session = tf_session_create(model, &(TfSessionOptions){.on_chunk = on_chunk});
 *   while (more audio) tf_session_push(session, samples, n);           mono float samples
 *   tf_session_finish(session);                                        the end of the take
 *   const TfHeads *heads = tf_session_heads(session);                  the take's raw output
 *   tf_session_destroy(session); tf_model_close(model); tf_client_destroy(client);
 *
 * The SDK never compares a recitation with an expected ayah and never says "mistake". It gives the heads, the
 * decoded layer, the events (speech started and ended), and two files as read-only data for a judge of your own:
 * the calibration (tf_model_calibration_json) and the expected units of each ayah (tf_model_reference).
 *
 * Options: every options struct may be NULL or zeroed; zero is the default of every field.
 *
 * Errors: a function returns NULL or a negative TfError. tf_last_error_code() is the calling thread's last failure,
 * tf_last_error() its message and tf_last_error_fix() what to do about it; tf_error_name(), tf_error_fix() and
 * tf_error_docs() give the catalogue entry of any TfError (the same codes, texts and links as the other SDKs).
 * A call on a NULL object fails and leaves an earlier error in place, so after a chain of calls the first failure
 * is the one reported.
 *
 * Strings and arrays returned as `const` belong to the object they came from and stay valid until the next call on
 * that object, or until it is closed or destroyed.
 *
 * Threads: a Client may be used from any thread. A Model and its Session belong to one thread at a time. A Model
 * has one open Session at a time, and a process runs one take at a time (the engine's rule): a server runs one
 * worker process per take in flight (the `tajweedformer serve` and `bench` commands). */
#ifndef TAJWEEDFORMER_H
#define TAJWEEDFORMER_H

#include <stddef.h>

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

#define TF_SDK_VERSION "3.1.2"

TF_API const char *tf_version(void); /* "3.1.2" */

/* ============================================================================ errors */

/* The 26 errors of the catalogue shared by the four SDKs (docs: https://sdk.quranlab.ai/docs/errors). */
typedef enum {
  TF_OK = 0,
  TF_ERR_INVALID_ARGUMENT = -1,      /* a value the API does not accept */
  TF_ERR_INVALID_KEY = -2,           /* the server does not know the key, or it is revoked */
  TF_ERR_SDK_TOO_OLD = -3,           /* this key needs a newer SDK */
  TF_ERR_SERVER_REJECTED = -4,       /* the server refused this app */
  TF_ERR_CLIENT_EXISTS = -5,         /* a second Client while the first is not destroyed */
  TF_ERR_MODEL_UNAVAILABLE = -6,     /* Quran Lab has no model build for this SDK version yet */
  TF_ERR_NEEDS_CONNECTION = -7,      /* the model is not downloaded yet: a connection is needed once */
  TF_ERR_NETWORK_NOT_PERMITTED = -8, /* never raised by this SDK (Windows and Linux have no such permission) */
  TF_ERR_DOWNLOAD_FAILED = -9,       /* a download stopped; it resumes at the next call */
  TF_ERR_STORAGE_FULL = -10,          /* not enough space for the model */
  TF_ERR_STORAGE_FAILED = -11,       /* the SDK's folder cannot be read or written */
  TF_ERR_MODEL_MISSING = -12,        /* the folder has no model, or a listed file is gone */
  TF_ERR_MODEL_INVALID = -13,        /* the manifest or a model file is malformed */
  TF_ERR_SIGNATURE_INVALID = -14,    /* the manifest's signature does not verify */
  TF_ERR_CHECKSUM_MISMATCH = -15,    /* a file's size or SHA-256 differs from the manifest */
  TF_ERR_MODEL_INCOMPATIBLE = -16,   /* the build needs a newer SDK, or is not one this SDK runs */
  TF_ERR_RUNTIME_UNSUPPORTED = -17,  /* this build cannot run here: the SDK supports Windows and Linux on x86-64 */
  TF_ERR_REFERENCE_MISSING = -18,    /* tf_reference_units before tf_reference_fetch */
  TF_ERR_SESSION_OPEN = -19,         /* the Model already has an open Session */
  TF_ERR_SESSION_CLOSED = -20,       /* a call on a Session that has ended */
  TF_ERR_CANCELLED = -21,            /* the take was cancelled */
  TF_ERR_ENGINE_FAILED = -22,        /* the engine reported an error */
  TF_ERR_MICROPHONE_DENIED = -23,    /* never raised by this SDK (it has no microphone) */
  TF_ERR_MICROPHONE_UNAVAILABLE = -24, /* never raised by this SDK */
  TF_ERR_INTERRUPTED = -25,          /* never raised by this SDK */
  TF_ERR_SHARING_UNAVAILABLE = -26   /* a sharing call while Quran Lab's config does not allow sharing for this app */
} TfError;

TF_API int tf_last_error_code(void);          /* this thread's last failure (a TfError), 0 when none */
TF_API const char *tf_last_error(void);       /* its message, or NULL */
TF_API const char *tf_last_error_fix(void);   /* what to do about it: one sentence, or NULL */
TF_API const char *tf_error_name(int error);  /* the stable code: "invalid_argument", ..., "ok" for 0 */
TF_API const char *tf_error_fix(int error);   /* the catalogue's fix for that code */
TF_API const char *tf_error_docs(int error);  /* https://sdk.quranlab.ai/docs/errors#<code> */

/* ============================================================================ Backend */

/* What ran or will run the model. Here always {"cpu", "cpu", "cpu", NULL}: the exact CPU engine, no accelerator.
 * The strings are static. */
typedef struct {
  const char *kind;    /* "cpu" or "accelerator" */
  const char *name;    /* "cpu" */
  const char *stage;   /* "cpu": no accelerator here */
  const char *reason;  /* NULL */
} TfBackend;

/* ============================================================================ output */

typedef struct TfHeads TfHeads;

/* One raw head of a chunk or a take. */
typedef struct {
  const char *name;                 /* "log_probs", "inter1_log_probs", "inter4_log_probs", "id_log_probs",
                                       "timing_log_probs", "gem", "attr_tafkheem", "attr_ghunna", "attr_qalqalah",
                                       "attr_face", "boundary", "conf" */
  const char *kind;                 /* "ctc" (log-probs over the tokens) or "attribute" */
  int frame_ms;                     /* 40 or 20 */
  int frames;
  int classes;                      /* values per frame: 38 for a ctc head; 2 to 5 for the class heads (softmax
                                       probabilities); 1 for boundary and conf (a logit) */
  const float *values;              /* [frames, classes], row major */
  const char *const *class_names;   /* [classes] for the class heads ("single", "held", ...), else NULL */
} TfHead;

/* One 40 ms frame of log_probs. */
typedef struct {
  int top1;                 /* largest log-prob, lowest id on ties */
  float top1_log_prob;
  int runner_up;            /* largest of the others, lowest id on ties */
  float runner_up_log_prob;
  double margin;            /* top1_log_prob - runner_up_log_prob */
} TfFrame;

/* One greedy token: a run of frames with the same best token, blank removed. */
typedef struct {
  int id;                   /* token id (the vocabulary) */
  const char *name;         /* its name, NULL when decoded without a Model */
  int first_ms, peak_ms, last_ms; /* where its first, peak and last frame begin */
  int first_frame, last_frame;    /* frames of the run, inclusive */
  int peak_frame;           /* the run's frame with the largest top-1 log-prob */
  int start_ms, end_ms;     /* the run: frame_ms * first_frame, frame_ms * (last_frame + 1) */
  float log_prob;           /* at the peak */
  int runner_up;            /* at the peak */
  float runner_up_log_prob;
  double margin;
  double confidence;        /* mean sigmoid(conf) over the token's frames; NAN without the conf head */
} TfToken;

/* The tokens between two word bars. */
typedef struct {
  int first_token, n_tokens;
  int start_ms, end_ms;
  double confidence;        /* min over its tokens (NAN without conf) */
  double margin;            /* min over its tokens */
} TfWord;

/* The decoded layer: what the model heard, ready to show. Plain data; it belongs to the chunk or the Heads it
 * came with. */
typedef struct {
  const char *text;         /* the token names, separated by spaces */
  int n_tokens;
  const TfToken *tokens;
  int n_words;
  const TfWord *words;
  int n_frames;
  const TfFrame *frames;
  int top_k;                /* 0, or the depth asked for with TfSessionOptions.top_k */
  const int *ranked_ids;            /* [n_frames, top_k]: each frame's best tokens, best first (NULL when top_k 0) */
  const float *ranked_log_probs;    /* [n_frames, top_k] */
  const double *ranked_margins;     /* [n_frames, top_k]: to the next rank; 0 for the last rank of all 38 */
  int settled;              /* the first `settled` tokens will not change in this take; the rest still can */
  int is_final;             /* 1 on the last one of the take */
} TfDecoded;

/* {"tokens":[{"id","unit","frame0","frame1","startMs","endMs","peakFrame","logProb","runnerUp","runnerUpUnit",
 *  "runnerUpLogProb","margin","confidence"}],"words":[...],"transcript","settled","isFinal"}, and with with_frames
 * "frames":[{"top1","top1LogProb","runnerUp","runnerUpLogProb","margin","ranked":[...]}] first. Valid until the
 * next call on the same decoded layer. */
TF_API const char *tf_decoded_json(const TfDecoded *decoded, int with_frames);

TF_API int tf_heads_count(const TfHeads *h);                  /* the heads it holds */
TF_API const char *tf_heads_name(const TfHeads *h, int i);
/* One raw head by name. TF_ERR_INVALID_ARGUMENT for a name the Heads do not hold. */
TF_API int tf_heads_get(const TfHeads *h, const char *name, TfHead *out);
/* Greedy tokens of a ctc head (NULL: "log_probs"): returns the count and sets *tokens. confidence is NAN except
 * for log_probs with conf selected. */
TF_API int tf_heads_greedy(const TfHeads *h, const char *name, const TfToken **tokens);
/* The decoded layer of exactly these Heads (for the take's Heads: the final one). */
TF_API const TfDecoded *tf_heads_decoded(const TfHeads *h);

/* A chunk, valid during the on_chunk call. */
typedef struct {
  int index;                /* chunk number in the take, from 0 */
  int start_ms, end_ms;     /* on the take's clock */
  int first_frame;          /* its first 40 ms frame in the take */
  TfBackend backend;        /* what ran it */
  int speech;               /* 1 when the chunk holds speech frames */
  const TfHeads *heads;     /* the Heads of this chunk only */
  const TfDecoded *decoded; /* the decoded layer of the take so far */
} TfChunk;

/* What happened to the take. */
enum {
  TF_EVENT_SPEECH_STARTED = 1,  /* the first speech frame after silence: at_ms */
  TF_EVENT_SPEECH_ENDED = 2,    /* silence after speech: at_ms (where the speech ended), silence_ms */
  TF_EVENT_INTERRUPTED = 3,     /* never raised by this SDK (no microphone) */
  TF_EVENT_RESUMED = 4,         /* never raised by this SDK */
  TF_EVENT_BACKEND_CHANGED = 5, /* never raised by this SDK (no accelerator) */
  TF_EVENT_ENDED = 6            /* the take ended without tf_session_finish: reason */
};
enum {
  TF_ENDED_SILENCE = 1,     /* end_on_silence_ms */
  TF_ENDED_BACKGROUND = 2,  /* never raised by this SDK */
  TF_ENDED_HIDDEN = 3,      /* never raised by this SDK */
  TF_ENDED_INTERRUPTED = 4, /* never raised by this SDK */
  TF_ENDED_SOURCE_END = 5,  /* the clip of tf_session_listen ran out */
  TF_ENDED_LIMIT = 6,       /* the take reached 20 minutes of audio, the most a take holds */
  TF_ENDED_ERROR = 7        /* error holds the TfError */
};
typedef struct {
  int type;                 /* TF_EVENT_* */
  const char *name;         /* "speechStarted", "speechEnded", "ended": the same names in every SDK */
  int at_ms;                /* speechStarted, speechEnded */
  int silence_ms;           /* speechEnded: the silence that ended it */
  int reason;               /* ended: TF_ENDED_* */
  const char *reason_name;  /* ended: "silence", "sourceEnd", "limit", "error" */
  int error;                /* ended with TF_ENDED_ERROR: the TfError */
} TfEvent;

/* ============================================================================ Client */

typedef struct TfClient TfClient;
typedef struct TfModel TfModel;

/* Anonymous telemetry (sums and histograms per day; no audio, text or identifier). On by default; Quran Lab's
 * signed config can stop it too. */
typedef enum { TF_ANALYTICS_ON = 0, TF_ANALYTICS_OFF = 1 } TfAnalytics;

/* The SDK asks the learner whether to share recitations (section Sharing). This library has no UI: the app shows
 * its own sheet with these strings and reports the tap with tf_sharing_answer. */
typedef struct {
  int reask;                   /* 0: the first ask, 1 or 2: a gentle re-ask */
  int guardian;                /* the audience is children: address the parent or guardian */
  const char *copy_json;       /* the copy deck for the client's language, with the app's own wording applied */
  const char *consent_version; /* the version a "share" answer grants */
} TfConsentRequest;

typedef struct {
  const char *key;             /* the desktop key from sdk.quranlab.ai (qlk_v1_...); required */
  const char *app_id;          /* the app's id as registered for the key (Windows: package family name or
                                  AppUserModelID; Linux: a reverse-DNS id); required */
  const char *app_version;     /* NULL: "0" */
  const char *storage;         /* a folder the app can write: the SDK's state and the model files; NULL:
                                  %LOCALAPPDATA%/<app_id>/tajweedformer, or $XDG_DATA_HOME (~/.local/share) on Linux */
  TfAnalytics analytics;
  const char *audience;        /* NULL or "general"; "children": consent is addressed to a guardian */
  const char *language;        /* the copy deck's language, BCP 47; NULL: "en" */
  void (*on_consent)(void *user, const TfConsentRequest *request); /* the SDK asks to show the sharing sheet */
  int debug;                   /* 1 in the app's debug builds: the client logs for the developer (a newer SDK,
                                  a pin the server could not honour); 0: it logs nothing */
  void (*on_log)(void *user, int level, const char *message); /* where that log goes; NULL: stderr. level 0 debug,
                                  1 info, 2 warning, 3 error */
  void *user;                  /* passed to on_consent and on_log */
} TfClientOptions;

/* One Client per process: a second one while the first is not destroyed is TF_ERR_CLIENT_EXISTS. Never waits for
 * the network: registration, the signed config and the telemetry go in the background. */
TF_API TfClient *tf_client_create(const TfClientOptions *options);
TF_API void tf_client_destroy(TfClient *c);

typedef struct {
  const char *version;         /* pin a model version ("4.0.0"); NULL: the one Quran Lab releases to this app.
                                  tf_client_model and tf_client_prefetch only */
  int threads;                 /* engine threads; 0: the fast cores */
  int pin;                     /* 0: the engine's default (desktops and servers: threads not pinned to cores);
                                  1: pinned; -1: not pinned. Timing only, never a bit */
} TfModelOptions;

/* The Model for this app: downloads on first use (resumable), verifies every file, opens and warms the engine, so
 * call it early. Without a network and without a model on disk: TF_ERR_NEEDS_CONNECTION. While Quran Lab has no
 * build for this SDK version: TF_ERR_MODEL_UNAVAILABLE. When Quran Lab refuses the key or the app (a revoked key,
 * a paused or blocked app): TF_ERR_INVALID_KEY or TF_ERR_SERVER_REJECTED, and the SDK removes the downloaded
 * model from the device; once the key is accepted again the model is downloaded again. A Model that is open
 * then keeps running until tf_model_close, which removes its files. */
TF_API TfModel *tf_client_model(TfClient *c, const TfModelOptions *options);

typedef struct {
  int unmetered_only;          /* 1: wait for an unmetered connection. This library cannot tell what kind of
                                  connection the machine has (diagnose reports "unknown"), so here the download
                                  goes ahead: an app that knows it is on a metered connection does not call
                                  prefetch */
  void (*on_progress)(void *user, long long bytes_done, long long bytes_total);
  void *user;
  const char *version;         /* as TfModelOptions.version */
} TfPrefetchOptions;

/* Downloads and verifies the model ahead of use. Safe at every start: a no-op when done; it resumes where an
 * earlier call stopped. Returns 0 when the model is on disk. */
TF_API int tf_client_prefetch(TfClient *c, const TfPrefetchOptions *options);
/* What the SDK holds on disk: {"models":[{"version","build","bytes"}],"sharingQueueBytes":n}, one entry per model
 * build, ordered by version then build. */
TF_API const char *tf_client_storage_json(TfClient *c);
/* Removes the model files (never the consent state or the sharing queue). Close the Model first. */
TF_API int tf_client_remove_models(TfClient *c);

/* One structured report, safe to paste into an issue: no audio, text or identifiers. */
typedef struct {
  const char *sdk_version;     /* "3.1.2" */
  const char *sdk_latest;      /* the SDK release the signed config in force names, or NULL when it names none */
  int key_accepted;            /* 1 accepted, 0 refused, -1 not known yet (no answer from the server) */
  const char *key_reason;      /* why not: the server's code, or NULL */
  const char *config;          /* "fresh" (a verified config that has not expired), "stale" (expired) or "none" */
  const char *model_version;   /* NULL without a model on disk */
  const char *model_build;
  int model_verified;
  const char *context;         /* the context in force, or NULL */
  TfBackend backend;           /* kind NULL without a model on disk */
  const char *microphone;      /* "none": this SDK has no microphone */
  long long storage_models;    /* bytes */
  long long storage_sharing;
  const char *last_error;      /* the code of the last error this process raised, or NULL */
  int dropped_chunks;          /* chunks dropped because the app did not read them: always 0 here (chunks come
                                  by callback, none waits) */
  const char *connection;      /* "unmetered", "metered", "none" or "unknown": always "unknown" here */
  const char *json;            /* all of it as JSON */
} TfDiagnosis;
TF_API int tf_client_diagnose(TfClient *c, TfDiagnosis *out);
TF_API const char *tf_client_diagnose_json(TfClient *c);

/* App-reported counter: the results of the app's own judge. rule is lower-case letters, digits and underscores
 * (at most 24, a letter first; use the calibration's class ids where they apply), 0 <= flagged <= checked <=
 * 100000. Counts only; nothing is sent when the app never calls it. */
TF_API int tf_client_report_checks(TfClient *c, const char *rule, int checked, int flagged);

/* The app left the foreground (a window minimized, about to sleep) or returned to it. Background sends the
 * telemetry counted so far and closes the shared recording that waits for another take. */
TF_API int tf_client_background(TfClient *c);
TF_API int tf_client_foreground(TfClient *c);

/* ============================================================================ Model */

/* A Model comes from tf_client_model and from nowhere else: from Quran Lab's server, through the Client. Since
 * 3.1.0 no call opens a model folder the app supplies (the call that did is gone), and the Client opens only a
 * build it downloaded itself: a model folder put into its storage by anything else is deleted and downloaded. */
TF_API void tf_model_close(TfModel *m);

TF_API const char *tf_model_id(const TfModel *m);       /* "v4.0" */
TF_API const char *tf_model_version(const TfModel *m);  /* "4.0.0" */
TF_API const char *tf_model_build(const TfModel *m);    /* the build id (40 hex); for diagnostics only */
/* {"id","version","build","family","contexts":[...],"default_context","chunk_ms":{"<context>":ms},"heads":[{"name",
 *  "kind","frame_ms","classes","class_names"}],"default_heads":[...],"tokens":[...],"blank","word_bar",
 *  "calibration":{"format","sha256"}|null,"reference":bool,"sample_clips":[{"name","kind","duration_ms"}],
 *  "signed","key_id","sdk_version"} */
TF_API const char *tf_model_info_json(TfModel *m);
/* Audio per chunk in a context (160, 320 or 480 ms); context NULL: the default one. 0 for an unknown context. */
TF_API int tf_model_chunk_ms(const TfModel *m, const char *context);

/* The vocabulary. */
typedef struct {
  int count;                   /* 38 */
  int blank;                   /* 0 */
  int word_bar;                /* the id of "|", -1 when the vocabulary has none */
  const char *const *names;    /* [count] */
} TfTokens;
TF_API const TfTokens *tf_model_tokens(const TfModel *m);
TF_API int tf_model_token_count(const TfModel *m);
TF_API const char *tf_model_token_name(const TfModel *m, int id);   /* NULL out of range */
TF_API int tf_model_token_id(const TfModel *m, const char *name);    /* -1 when unknown */

/* calibration.json as shipped: read-only data for a judge of your own, never applied by the SDK. */
typedef struct {
  const char *json;            /* the file's bytes, NUL-terminated */
  size_t size;
  const char *format;          /* "quranlab.tajweed-calibration/1" */
  const char *sha256;          /* of the file, as the signed manifest lists it */
} TfCalibration;
/* NULL when the folder has no calibration (not an error). */
TF_API const char *tf_model_calibration_json(const TfModel *m);
TF_API int tf_model_calibration(const TfModel *m, TfCalibration *out); /* 1 when present, 0 when not */
/* A number of the calibration by its path, for example "contexts.c16_l64.sites.vowel_sub.thresholds.balanced" or
 * "contexts.c16_l64.fusion.a.vowel_sub". 1 and *out set, or 0 when the path is not a number. */
TF_API int tf_model_calibration_number(const TfModel *m, const char *path, double *out);

/* The expected units of each ayah: read-only data, downloaded only when the app asks. The SDK never aligns them
 * and never compares them with a take. */
typedef struct TfReference TfReference;
typedef struct {
  int n;                       /* units of the ayah */
  const int *ids;              /* [n] token ids */
  const int *word;             /* [n] the 0-based word of each unit */
  int n_words;
} TfUnits;
TF_API TfReference *tf_model_reference(TfModel *m);            /* NULL when the build has none (not an error) */
TF_API int tf_reference_is_downloaded(const TfReference *r);
TF_API int tf_reference_fetch(TfReference *r);                  /* download and verify, once; 0 when on disk */
/* TF_ERR_REFERENCE_MISSING before the fetch; TF_ERR_INVALID_ARGUMENT for a surah or ayah that does not exist. */
TF_API int tf_reference_units(TfReference *r, int surah, int ayah, TfUnits *out);

/* The self-test clips of the folder: real recitations to run without a microphone. */
typedef struct {
  const char *name;            /* "Husary_001001" */
  const char *kind;            /* "clean" or "planted" */
  int duration_ms;
} TfClip;
TF_API int tf_model_sample_clip_count(const TfModel *m);
TF_API int tf_model_sample_clip(const TfModel *m, int i, TfClip *out);

TF_API int tf_model_backend(const TfModel *m, TfBackend *out); /* what the next Session will use */

/* ============================================================================ Session */

typedef struct TfSession TfSession;

enum { TF_PACE_REAL_TIME = 0, TF_PACE_FAST = 1 };

typedef struct {
  const char *context;         /* NULL: the config's inference.context, then the manifest's default */
  int sample_rate;             /* of pushed audio: 0 or 16000, or 8000 to 48000 (resampled by the engine) */
  const char *heads;           /* head names separated by commas, or "all"; NULL: "log_probs,inter1_log_probs,
                                  timing_log_probs,gem,attr_qalqalah,conf". A head not asked for is not computed
                                  (log_probs always is) */
  const char *backend;         /* NULL, "auto" or "cpu": the CPU engine either way */
  int top_k;                   /* 0, or 1 to 38: the decoded layer ranks that many tokens per frame */
  int end_on_silence_ms;       /* 0: off. Else 500 to 60000: the take finishes after that much silence following
                                  speech (a held sound is speech: the timing head hears it) */
  int pace;                    /* tf_session_listen only: TF_PACE_REAL_TIME (default) or TF_PACE_FAST */
  void (*on_chunk)(void *user, const TfChunk *chunk);   /* each chunk as it is ready, on the pushing thread */
  void (*on_event)(void *user, const TfEvent *event);   /* what happened to the take, on the pushing thread */
  void *user;
  const char *input_route;     /* where the audio comes from, for a shared recording: "built_in", "wired", "usb",
                                  "bluetooth", "car", "continuity", "other"; NULL: "unknown" */
} TfSessionOptions;

/* A Session the app pushes audio into: one take. TF_ERR_SESSION_OPEN while the Model has an open Session. */
TF_API TfSession *tf_session_create(TfModel *m, const TfSessionOptions *options);
/* The same path with no microphone: runs a sample clip of the Model through a Session on the calling thread (at
 * the clip's pace, or as fast as possible) and returns it ended: ended(sourceEnd), or ended(silence) earlier with
 * end_on_silence_ms. Read tf_session_heads, then tf_session_destroy. */
TF_API TfSession *tf_session_listen(TfModel *m, const TfClip *clip, const TfSessionOptions *options);
/* Audio in: mono float samples in [-1, 1] at sample_rate. Returns the chunks run. TF_ERR_SESSION_CLOSED once the
 * take has ended (by itself, or by finish or cancel). A take holds at most 20 minutes of audio: at that point it
 * ends by itself as a finish would (the last chunk final, the Heads kept) with the event ended(limit), and the
 * rest of the pushed buffer is not taken. */
TF_API int tf_session_push(TfSession *s, const float *samples, int n);
/* The end of the take: the last chunks arrive (the last one's decoded layer has is_final), then the take's Heads
 * are ready. Returns the chunks run. On a take that already ended by itself: 0. */
TF_API int tf_session_finish(TfSession *s);
TF_API const TfHeads *tf_session_heads(TfSession *s);   /* the take's Heads once it has ended; NULL before */
TF_API int tf_session_cancel(TfSession *s);             /* end the take, keep nothing */
TF_API int tf_session_backend(const TfSession *s, TfBackend *out);
/* App-reported counter: where the learner recited in this take (surah 1 to 114, 1 <= ayah_from <= ayah_to <= the
 * surah's ayahs). At most one report per take: a second replaces the first. Counts only. */
TF_API int tf_session_report_recited(TfSession *s, int surah, int ayah_from, int ayah_to);
TF_API void tf_session_destroy(TfSession *s);            /* an open take is cancelled */

/* ============================================================================ Sharing */

/* Recitation sharing. Quran Lab's signed config decides whether learners in this app are asked; the learner
 * decides. The sheet is shown at most three times per install; a yes is never asked again. With a yes, every take
 * of a Session whose Model came from tf_client_model is shared in full in the background (one Ogg Opus recording
 * for takes in a row, an on-disk queue that survives restarts). Nothing is recorded or sent before a yes. */
typedef struct {
  int is_available;            /* Quran Lab's config allows sharing for this app */
  int is_on;                   /* the learner said yes (and sharing is available) */
  int asks;                    /* sheets shown so far */
  int pending_ask;             /* an ask waits for an answer (without on_consent: show the sheet now) */
  int guardian;                /* the grant was given by a guardian */
  int shared;                  /* recordings uploaded from this install */
  const char *consent_version; /* what a new grant would record */
  const char *consent_text;    /* with the app's own wording: the canonical text to send to Quran Lab; else NULL */
  const char *consent_sha256;  /* its SHA-256; NULL without the app's own wording */
} TfSharing;
TF_API int tf_sharing_status(TfClient *c, TfSharing *out);
TF_API int tf_sharing_is_available(TfClient *c);
TF_API int tf_sharing_is_on(TfClient *c);
/* The settings switch. Off stops uploads at once and deletes the unsent queue. */
TF_API int tf_sharing_set_on(TfClient *c, int on);
/* Ask now, if an ask is still allowed (the app chooses the moment only). TF_ERR_SHARING_UNAVAILABLE when the
 * config does not offer sharing to this app. */
TF_API int tf_sharing_ask(TfClient *c);
/* The learner's answer to a sheet, only from the learner's own tap: 1 share, 0 not now. */
TF_API int tf_sharing_answer(TfClient *c, int share);
/* The client's copy deck with the app's own wording applied: what the app's sheet and settings show. */
TF_API const char *tf_sharing_copy_json(TfClient *c);
/* Audio the app already captures outside takes (before the first, between two, after the last), mono float at
 * sample_rate: the last second before a take and the half second after it go into the shared recording, and
 * what lies between two takes in a row. Dropped at once without a yes. The model never sees it. */
TF_API int tf_sharing_feed(TfClient *c, const float *samples, int n, int sample_rate);
/* The queue: [{"id","created","ended","completed","segments","uploaded","takes","error"}], oldest first. */
TF_API const char *tf_sharing_items_json(TfClient *c);
/* The learner's summary: {"you":{"sessions","minutes"},"app":...|null,"all":...}, or NULL. */
TF_API const char *tf_sharing_summary_json(TfClient *c);
/* "Delete what I shared": deleted from Quran Lab's servers at once, with the unsent queue. The queue stops before
 * the server is asked. When the server took the deletion (0), sharing is off afterwards, as after
 * tf_sharing_set_on(client, 0): the learner turns it on again with the switch and is not asked again. When it did
 * not (an error code), nothing has changed and the queue carries on. The off is written to disk before the server
 * is asked: when the process ends while the server is asked and the server never took the deletion, the learner
 * finds sharing off at the next launch and the unsent queue deleted; the switch turns sharing on again and they
 * are not asked again (they had said yes before: only a learner who shared can delete). */
TF_API int tf_sharing_delete(TfClient *c);
/* The code for deletion when the device is lost (26 characters in groups of 4), for the learner only; "" before
 * the first shared recording. */
TF_API int tf_sharing_recovery_code(TfClient *c, char out[33]);
/* The app's own wording: replaces deck strings by key ("sheet.what", ...). An unknown key, an empty text, text
 * that is not UTF-8 or a key given twice is TF_ERR_INVALID_ARGUMENT and nothing changes; n = 0 clears. Call it
 * right after tf_client_create at every start, and send TfSharing.consent_text to Quran Lab for review. */
TF_API int tf_sharing_set_wording(TfClient *c, const char *const *keys, const char *const *texts, int n);

#ifdef __cplusplus
}
#endif
#endif /* TAJWEEDFORMER_H */
