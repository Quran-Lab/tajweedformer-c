# The C API: `tajweedformer.h` (SDK 3.1.3)

One library for Windows and Linux apps and for servers: `tajweedformer.dll` / `libtajweedformer.so` (or the static
library) and one header, `include/tajweedformer.h`. It includes nothing but `<stddef.h>` and names no engine type.
SDK 3.0.0 is a pure inference engine: audio in, the model's own output out. It never compares a recitation with an
expected ayah and never says "mistake"; an app that judges builds its judge on the output (see
[migrating-to-3.md](migrating-to-3.md)).

Five things, with the same names and the same order of calls as the Android, Apple and web SDKs:

| | C type | What it is |
| - | - | - |
| Client | `TfClient` | the app's link to Quran Lab: the key, the signed config, the model files, telemetry, sharing. One per process |
| Model | `TfModel` | one verified model build on this device: version, contexts, tokens, calibration and reference data, sample clips |
| Session | `TfSession` | one take: audio in, chunks and events out, the take's Heads at the end |
| Heads | `TfHeads` | raw model output of a chunk or a whole take: every selected head, greedy tokens, the decoded layer |
| Backend | `TfBackend` | what runs the model: here always the CPU engine |

Every options struct (`TfClientOptions`, `TfModelOptions`, `TfPrefetchOptions`, `TfSessionOptions`) may be NULL or
zeroed: zero is the default of every field. Strings and arrays returned as `const` belong to the object they came
from and stay valid until the next call on it, or until it is closed or destroyed. `tf_version()` is `"3.1.3"`.

## Errors

A function returns NULL or a negative `TfError`. The 26 codes, their messages, fixes and links are the catalogue
shared by the four SDKs (<https://sdk.quranlab.ai/docs/errors>).

```c
TfModel *model = tf_client_model(client, NULL);
if (!model) {
  fprintf(stderr, "%s: %s\n  %s\n  %s\n", tf_error_name(tf_last_error_code()), tf_last_error(), tf_last_error_fix(),
          tf_error_docs(tf_last_error_code()));
  return 1;
}
```

| Call | Gives |
| - | - |
| `tf_last_error_code()` | the calling thread's last failure, a `TfError` (0 when none) |
| `tf_last_error()` | its message |
| `tf_last_error_fix()` | what to do about it, one sentence |
| `tf_error_name(error)` | the stable code of any `TfError`: `"invalid_argument"`, `"needs_connection"`, ... |
| `tf_error_fix(error)` | the catalogue's fix for that code |
| `tf_error_docs(error)` | `https://sdk.quranlab.ai/docs/errors#<code>` |

A call on a NULL object fails and leaves an earlier error in place, so after a chain of calls (the quick start
checks nothing until the end) the first failure is the one reported.

The codes: `invalid_argument`, `invalid_key`, `sdk_too_old`, `server_rejected`, `client_exists`,
`model_unavailable`, `needs_connection`, `download_failed`, `storage_full`, `storage_failed`, `model_missing`,
`model_invalid`, `signature_invalid`, `checksum_mismatch`, `model_incompatible`, `runtime_unsupported` (a build for a
CPU the SDK does not support; on x86-64 every CPU runs it), `reference_missing`,
`session_open`, `session_closed`, `cancelled`, `engine_failed`, `sharing_unavailable`, and four this SDK never
raises: `microphone_denied`, `microphone_unavailable`, `interrupted` (it has no microphone) and
`network_not_permitted` (Windows and Linux have no such permission).

When the server refuses the app, `server_rejected` carries the message and the fix of that refusal, with the
server's code in brackets (an account without access yet, a key that belongs to another app, a blocked app and
the others the catalogue lists under `reasons`).

## Client

```c
TfClient *client = tf_client_create(&(TfClientOptions){.key = "qlk_v1_...", .app_id = "com.example.app",
                                                       .app_version = "1.4", .debug = 1});
```

`tf_client_create` never waits for the network: registration, the signed config and the telemetry go in the
background. One Client per process: a second one while the first is not destroyed is `client_exists`.
`tf_client_destroy` sends what is counted and closes the shared recording.

`TfClientOptions`: `key` and `app_id` (required), `app_version`, `storage` (a folder the app can write; default
`%LOCALAPPDATA%/<app_id>/tajweedformer` on Windows, `$XDG_DATA_HOME` or `~/.local/share` on Linux), `analytics`
(`TfAnalytics`: `TF_ANALYTICS_ON` by default, `TF_ANALYTICS_OFF`), `audience` (`"general"` or `"children"`),
`language` (the copy deck's language), `on_consent` (the SDK asks the app to show the sharing sheet, with a
`TfConsentRequest`), `debug` and `on_log` (in the app's debug builds the client logs a newer SDK release and a
version pin the server could not honour), `user`.

| Call | What it does |
| - | - |
| `tf_client_model(client, options)` | the Model for this app: downloads on first use (resumed by itself after a break, see below), verifies every file, opens and warms the engine. The same Model while it is open. `needs_connection` without a network and without a model on disk; `model_unavailable` while Quran Lab has no build for this SDK version |
| `tf_client_prefetch(client, options)` | downloads and verifies ahead of use. Safe at every start: a no-op when done, and it resumes where an earlier call stopped. `TfPrefetchOptions`: `unmetered_only` (this library cannot tell what kind of connection the machine has, so the download goes ahead), `on_progress(user, bytes_done, bytes_total)`, `version` |
| `tf_client_storage_json(client)` | `{"models":[{"version","build","bytes"}],"sharingQueueBytes":n}` |
| `tf_client_remove_models(client)` | removes the model files, never the consent state or the sharing queue. Close the Model first |
| `tf_client_diagnose(client, &d)`, `tf_client_diagnose_json(client)` | one report, safe to paste into an issue (below) |
| `tf_client_report_checks(client, rule, checked, flagged)` | app-reported counter: the results of the app's own judge (below) |
| `tf_client_background(client)`, `tf_client_foreground(client)` | the app left the foreground or returned: background sends the telemetry counted so far and closes the shared recording that waits for another take |

`TfModelOptions`: `version` pins a model version (`"4.0.0"`; NULL: the one Quran Lab releases to this app),
`threads` (0: the fast cores), `pin` (thread pinning: 0 the engine's default, 1 pinned, -1 not; timing only,
never a bit).

What is downloaded: the CPU files of the context in force and the sample clips, gzip on the wire, each file checked
against the signed manifest; another context's files come on first use, the reference data only when the app
asks. The files live under `<storage>/models/<build>/`.

How a download goes: all of it over one kept connection, HTTP/2 where the system has it (WinHTTP on Windows,
with HTTP/3 too on Windows 11; the system's libcurl on Linux), else HTTP/1.1. A request ends when no byte
arrives for 30 seconds (on Windows up to 34) and has no limit on its whole time, so a slow link is never cut.
A file whose transfer breaks (no answer, a connection that ends, a stall, a server error) is resumed inside
the call from its partial file, with the server's ETag as the validator (`If-Range`), up to five times after
pauses of 1, 2, 4, 8 and 16 seconds; then the call fails with `download_failed`, the partial file stays, and
the next call goes on from it. Only the answer that was asked for is taken (a 200 of the file's length, or a
206 whose `Content-Range` starts where the request did and names the file's size); any other answer that
says success is a failed attempt and takes no byte from the partial file. A refusal (a revoked key, a
blocked app, a limit, a network that is not served)
is asked once. `tf_client_destroy` from another thread ends a download in flight: the downloading call returns
`download_failed`. No request of the SDK follows a redirect: one answered to a model request ends the download
with `download_failed`, and one answered to another call is that call's failure.

### Diagnose

`TfDiagnosis` has the SDK version and the newest release the config names, whether the key was accepted (and the
server's reason when not), the config's state (`fresh`, `stale`, `none`), the model on disk (version, build,
verified), the context in force, the backend, the bytes held (models, sharing queue) and the code of the last
error. `json` is all of it with the same eleven keys in every SDK: `sdk`, `key`, `config`, `model`, `context`,
`backend`, `microphone` (always `"none"` here), `storage`, `lastError`, `droppedChunks` (always 0 here: chunks come
by callback, none waits) and `connection` (always `"unknown"` here); and, in `json` only (the struct has no
field for it), a twelfth, `downloadProtocol`: the protocol the last request for a model file ran on, `"h3"`,
`"h2"` or `"http/1.1"`, null while this Client has made none in this process. It is kept in memory on the
device and is never sent. No audio, text or identifiers.

## Model

```c
TfModel *model = tf_client_model(client, NULL);
printf("%s %s, %d ms chunks\n", tf_model_id(model), tf_model_version(model), tf_model_chunk_ms(model, NULL));
```

A Model comes from `tf_client_model` and from nowhere else: from Quran Lab's server, through the Client (the
Ed25519 signature of the manifest by Quran Lab's key, then the size and SHA-256 of every file it reads; a file the
manifest does not list is never opened). Since 3.1.0 no call opens a model folder the app supplies, and the Client
opens only a build it downloaded itself: a folder put into its storage by anything else is deleted and
downloaded. When Quran Lab refuses the key or the app (a revoked key, a paused or blocked app), the SDK stops
running the model and removes the downloaded model from the device: every build in the Client's storage folder
with its download record, and nothing else in that folder. `tf_client_model` and `tf_client_prefetch` then fail
with `invalid_key`, or with `server_rejected` and that refusal's message and fix, until a later answer accepts
the key; once the key is accepted again the model is downloaded again. A Model that is already open keeps
running until it is closed, and its files are removed when it is closed, or at the next start. `tf_model_close` releases it.

| Call | Gives |
| - | - |
| `tf_model_id`, `tf_model_version`, `tf_model_build` | `"v4.0"`, `"4.0.0"`, the build id (40 hex, for diagnostics) |
| `tf_model_info_json` | id, version, build, contexts with their chunk length, heads with kinds and class names, default heads, tokens, calibration format and hash, whether it has reference data, sample clips |
| `tf_model_chunk_ms(model, context)` | audio per chunk: 160, 320 or 480 ms; NULL: the default context |
| `tf_model_tokens` | `TfTokens`: `count` (38), `blank` (0), `word_bar`, `names` |
| `tf_model_token_count`, `tf_model_token_name`, `tf_model_token_id` | the vocabulary by id and by name |
| `tf_model_backend(model, &backend)` | what the next Session will use: `{"cpu", "cpu", "cpu", NULL}` |

### Calibration: read-only data

`calibration.json` as shipped, never applied by the SDK: the numbers Quran Lab measured for a judge built on the
heads. `tf_model_calibration_json(model)` is the file's text (NULL when the build has none, which is not an
error); `tf_model_calibration(model, &c)` fills a `TfCalibration` (`json`, `size`, `format`
`quranlab.tajweed-calibration/1`, `sha256` as the signed manifest lists it);
`tf_model_calibration_number(model, path, &x)` reads one number by its path:

```c
double threshold;
if (tf_model_calibration_number(model, "contexts.c16_l64.sites.vowel_sub.thresholds.balanced", &threshold)) ...
```

What the file holds: `classes` (`vowel_sub`, `vowel_del`, `vowel_ins`, `short_to_long`, `long_to_short`,
`cons_sub`, `cons_del`, `insertion`), `presets` (`strict`, `balanced`, `sensitive`, `very_sensitive`, each with
its target false-alarm rate), and per context: `fusion` (the weight `a` per class for `llr = a * llr(log_probs) +
(1 - a) * llr(inter1_log_probs)`, the `inter1_prep` rule and the `clip` of LLRs), `sites.<class>.thresholds.<preset>`
(a threshold in nats on the fused LLR, with a `status`: `validated`, `experimental` or `off`), `madd` (cuts on the
held span of a long vowel from `timing_log_probs`), `qalqalah` and `gemination` (a `tau` on `attr_qalqalah` and
`gem`), and `word`. [migrating-to-3.md](migrating-to-3.md) says how 2.x used each.

### Reference data: read-only

The expected units of each ayah (the same token ids as the model's vocabulary), for an app that aligns a take with
an ayah itself. The SDK never aligns them and never compares them with a take.

```c
TfReference *reference = tf_model_reference(model);       /* NULL when the build has none: not an error */
if (reference && tf_reference_fetch(reference) == 0) {    /* downloaded and verified once; 0 when on disk */
  TfUnits units;
  if (tf_reference_units(reference, 1, 2, &units) == 0)    /* surah 1, ayah 2 */
    for (int i = 0; i < units.n; ++i) printf("%s%s", tf_model_token_name(model, units.ids[i]), i + 1 < units.n ? " " : "\n");
}
```

`tf_reference_is_downloaded` says whether the file is here; `tf_reference_units` before the fetch is
`reference_missing`, and a surah or ayah that does not exist is `invalid_argument`. `TfUnits`: `n`, `ids`, `word`
(the 0-based word of each unit), `n_words`.

### Sample clips

Real recitations that ship in the model folder, to run without a microphone: `tf_model_sample_clip_count(model)`,
`tf_model_sample_clip(model, i, &clip)` fills a `TfClip` (`name`, `kind` `"clean"` or `"planted"`,
`duration_ms`), and `tf_session_listen` runs one.

## Session

```c
static void on_chunk(void *user, const TfChunk *chunk) { puts(chunk->decoded->text); }

TfSession *session = tf_session_create(model, &(TfSessionOptions){.on_chunk = on_chunk, .end_on_silence_ms = 1500});
while (more_audio()) if (tf_session_push(session, samples, n) < 0) break;   /* mono float samples in [-1, 1] */
tf_session_finish(session);                                               /* the end of the take */
const TfHeads *heads = tf_session_heads(session);                         /* the take's raw output */
tf_session_destroy(session);
```

A Session is one take. A Model has one open Session at a time (`session_open` for a second), and a process runs
one take at a time (the engine's rule): a server runs one worker process per take in flight.

`TfSessionOptions`:

| Field | Meaning |
| - | - |
| `context` | NULL: the signed config's `inference.context` when the build has it, else the manifest's default. A context the build does not have is `invalid_argument` |
| `sample_rate` | of pushed audio: 0 or 16000, or 8000 to 48000 (resampled by the engine) |
| `heads` | head names separated by commas, or `"all"`; NULL: `log_probs,inter1_log_probs,timing_log_probs,gem,attr_qalqalah,conf`. A head not asked for is not computed (`log_probs` always is) |
| `backend` | NULL, `"auto"` or `"cpu"`: the CPU engine either way |
| `top_k` | 0, or 1 to 38: the decoded layer ranks that many tokens per frame (each with its log-prob and its margin to the next rank) |
| `end_on_silence_ms` | 0: off. Else 500 to 60000: the take ends by itself after that much silence following speech |
| `pace` | `tf_session_listen` only: `TF_PACE_REAL_TIME` (the clip's own pace) or `TF_PACE_FAST` |
| `on_chunk`, `on_event`, `user` | each chunk and each event, on the pushing thread |
| `input_route` | where the audio comes from, for a shared recording: `built_in`, `wired`, `usb`, `bluetooth`, `car`, `continuity`, `other` |

| Call | What it does |
| - | - |
| `tf_session_create(model, options)` | a take the app pushes audio into |
| `tf_session_listen(model, &clip, options)` | the same path with no microphone: runs a sample clip through a Session on the calling thread and returns it ended |
| `tf_session_push(session, samples, n)` | audio in; returns the chunks run. `session_closed` once the take has ended |
| `tf_session_finish(session)` | the end of the take: the last chunks arrive (the last one's decoded layer has `is_final`), then the Heads are ready. 0 on a take that already ended by itself |
| `tf_session_heads(session)` | the take's `TfHeads` once it has ended; NULL before |
| `tf_session_cancel(session)` | ends the take and keeps nothing; later calls are `cancelled` |
| `tf_session_backend(session, &backend)` | what runs it |
| `tf_session_report_recited(session, surah, ayah_from, ayah_to)` | app-reported counter (below) |
| `tf_session_destroy(session)` | an open take is cancelled |

### Chunks

`on_chunk` gets a `TfChunk`, valid during the call: `index`, `start_ms`, `end_ms`, `first_frame`, `backend`,
`speech` (1 when the chunk holds speech frames), `heads` (the `TfHeads` of this chunk only) and `decoded` (the
decoded layer of the take so far).

### Events

`on_event` gets a `TfEvent`: `type`, `name` (the same names in every SDK), and the fields of that type.

| Event | Fields |
| - | - |
| `TF_EVENT_SPEECH_STARTED` (`speechStarted`) | `at_ms`: the first speech frame after silence |
| `TF_EVENT_SPEECH_ENDED` (`speechEnded`) | `at_ms` where the speech ended, `silence_ms` the silence that ended it (1520 ms, the 1500 ms window in whole frames, unless `end_on_silence_ms` sets another) |
| `TF_EVENT_ENDED` (`ended`) | the take ended without `tf_session_finish`: `reason` `TF_ENDED_SILENCE` (`end_on_silence_ms`), `TF_ENDED_SOURCE_END` (the clip ran out), `TF_ENDED_LIMIT` (the take reached 20 minutes of audio, the most a take holds) or `TF_ENDED_ERROR` (`error` holds the `TfError`); `reason_name` |

Speech is what the model heard: a 40 ms frame is speech when the best token on it is not blank in `log_probs`
or in `timing_log_probs`, which keeps hearing a held sound (a long madd) while `log_probs` shows blank. The SDK
computes the timing head for every take for this, and hands it to the app only when it is selected.
`speechEnded` comes when the window (1500 ms, or `end_on_silence_ms`) has passed without a speech frame.
In a noisy room a take may not end on silence, so keep a way to stop (the app calls `tf_session_finish`);
the 20-minute limit ends it in any case. Values of `end_on_silence_ms` under 1000 can end a take in a breath
pause between two words; the quick start uses 1500; the minimum is 500.
Events about a microphone (`TF_EVENT_INTERRUPTED`, `TF_EVENT_RESUMED`) and about an accelerator
(`TF_EVENT_BACKEND_CHANGED`) exist for the shared names and are never raised here. With `end_on_silence_ms` the
chunk that reaches the silence is whole and final, nothing is flushed after it, and audio pushed later is not
decoded.

## Heads

```c
const TfHeads *heads = tf_session_heads(session);
TfHead gem;
if (tf_heads_get(heads, "gem", &gem) == 0)                 /* [frames, classes] floats, row major */
  printf("%d frames of %d ms, %d classes, P(held) at frame 0: %f\n", gem.frames, gem.frame_ms, gem.classes, gem.values[1]);
const TfToken *tokens;
const int n = tf_heads_greedy(heads, NULL, &tokens);       /* greedy tokens of log_probs */
```

| Call | Gives |
| - | - |
| `tf_heads_count(heads)`, `tf_heads_name(heads, i)` | the heads it holds |
| `tf_heads_get(heads, name, &head)` | one raw `TfHead`: `name`, `kind` (`"ctc"` or `"attribute"`), `frame_ms` (40 or 20), `frames`, `classes`, `values`, `class_names` |
| `tf_heads_greedy(heads, name, &tokens)` | greedy tokens of a ctc head (NULL: `log_probs`) |
| `tf_heads_decoded(heads)` | the decoded layer of exactly these Heads |

The twelve heads: `log_probs`, `inter1_log_probs`, `inter4_log_probs`, `id_log_probs` (40 ms frames, log-probs
over the 38 tokens), `timing_log_probs` (20 ms frames), `gem`, `attr_tafkheem`, `attr_ghunna`, `attr_qalqalah`,
`attr_face` (class probabilities per frame), `boundary` and `conf` (one logit per frame).

### The decoded layer

`TfDecoded` is plain data: `text` (the token names separated by spaces), `tokens` (`TfToken`: `id`, `name`,
`first_ms`, `peak_ms`, `last_ms`, frames, `log_prob`, `runner_up`, `runner_up_log_prob`, `margin`, `confidence`),
`words` (`TfWord`: the tokens between two word bars, with times, the lowest confidence and margin), `frames`
(`TfFrame`: `top1`, `top1_log_prob`, `runner_up`, `runner_up_log_prob`, `margin`), and with `top_k` the ranked
ids, log-probs and margins per frame. `confidence` is the mean sigmoid of `conf` over the token's frames, NAN when
`conf` was not selected.

`settled` says how many tokens will not change in this take: a token is settled once its run of frames has ended;
the last token is still tentative while its run reaches the last frame. `is_final` is 1 on the last decoded layer
of the take, where every token is settled. `tf_decoded_json(decoded, with_frames)` is the same as JSON, with the
keys of the other SDKs.

## App-reported counters

The SDK does not know what the learner recited or what an app's judge found. Two calls feed the anonymous
telemetry with counts only; nothing is sent when the app never calls them:

```c
tf_session_report_recited(session, 1, 2, 2);                 /* this take was surah 1, ayah 2 to 2 */
tf_client_report_checks(client, "vowel_sub", 31, 1);         /* the app's judge: 31 sites checked, 1 flagged */
```

`tf_session_report_recited`: surah 1 to 114, `1 <= ayah_from <= ayah_to <=` the surah's ayahs, at most one report
per take (a second replaces the first), during the take or after it ended. `tf_client_report_checks`: `rule` is
lower-case letters, digits and underscores (at most 24, a letter first; the calibration's class ids where they
apply), `0 <= flagged <= checked <= 100000`, at most 32 rules.

## Sharing

Recitation sharing. Quran Lab's signed config decides whether learners in this app are asked; the learner
decides. With a yes, every take of a Session whose Model came from `tf_client_model` is shared in full in the
background: one Ogg Opus recording for takes in a row, an on-disk queue that survives restarts. Nothing is
recorded or sent before a yes. The sheet is shown at most three times per install; a yes is never asked again.

This library has no UI. The SDK asks at a good moment through `TfClientOptions.on_consent` (a
`TfConsentRequest`: `reask`, `guardian`, `copy_json`, `consent_version`), the app shows its own sheet with those
strings and reports the learner's own tap.

| Call | What it does |
| - | - |
| `tf_sharing_status(client, &s)` | `TfSharing`: `is_available`, `is_on`, `asks`, `pending_ask`, `guardian`, `shared`, `consent_version`, `consent_text`, `consent_sha256` |
| `tf_sharing_is_available(client)`, `tf_sharing_is_on(client)` | the two answers alone |
| `tf_sharing_set_on(client, on)` | the settings switch. Off stops uploads at once and deletes the unsent queue: the request in flight is aborted and the SDK hands nothing more to the system after the call has returned (what the system had already buffered may still go out, which can be the segment being sent), and it returns within 3 s even when a request hangs |
| `tf_sharing_ask(client)` | ask now, if an ask is still allowed. `sharing_unavailable` when the config does not offer sharing to this app |
| `tf_sharing_answer(client, share)` | the learner's answer to a sheet: 1 share, 0 not now |
| `tf_sharing_copy_json(client)` | the copy deck in the client's language, with the app's own wording applied |
| `tf_sharing_feed(client, samples, n, sample_rate)` | audio the app already captures outside takes: the last second before a take and the half second after it go into the shared recording. Dropped at once without a yes. The model never sees it |
| `tf_sharing_items_json(client)` | the queue: `[{"id","created","ended","completed","segments","uploaded","takes","error"}]` |
| `tf_sharing_summary_json(client)` | the learner's summary: `{"you":{"sessions","minutes"},"app":...,"all":...}` |
| `tf_sharing_delete(client)` | "Delete what I shared": deleted from Quran Lab's servers at once, with the unsent queue. The queue stops before the server is asked. When the server took the deletion (0), sharing is OFF afterwards, as after `tf_sharing_set_on(client, 0)`: the learner turns it on again with the switch and is not asked again. When it did not (an error code), nothing has changed and the queue carries on. The off is written to disk before the server is asked: when the process ends while the server is asked and the server never took the deletion, the learner finds sharing off at the next launch and the unsent queue deleted; the switch turns it on again and they are not asked again. 0 and no change when nothing was ever shared from this install |
| `tf_sharing_recovery_code(client, out)` | the code for deletion when the device is lost, for the learner only |
| `tf_sharing_set_wording(client, keys, texts, n)` | the app's own wording for deck strings. Call it right after `tf_client_create` at every start and send `TfSharing.consent_text` to Quran Lab for review |

How the SDK talks to Quran Lab, what it sends and when: [platform-client.md](platform-client.md).

## Command line and server tier

```sh
# every command: --key qlk_v1_... --app-id com.example.app --storage DIR   (a Client, as in an app)
tajweedformer info
tajweedformer decode   clip.wav [--context c24_l72] [--heads all] [--top-k 5] [--frames] [--values] [--end-on-silence 1500]
tajweedformer serve                                  # JSON lines in: {"audio": "<path>"}; one result line each
tajweedformer download [--version 4.0.0]
tajweedformer diagnose
```

A decode line holds the audio's length, the compute time, the chunks, each head (`--values` adds its numbers), the
greedy tokens of every ctc head and the decoded layer (`--frames` adds the frames). Audio is WAV (PCM 16-bit or
float 32-bit, the first channel, 8 to 48 kHz) or raw float32 at 16 kHz. `serve` handles one take at a time; a
server scales with processes, each with its own storage folder. The tool uses only the public header and gets
its model through a Client, as an app does (since 3.1.0 it opens no folder, and `bench` is gone with that).

## Tests

`ctest` runs what needs no model data (the error catalogue, the shared 3.0 vectors, the public Client against a
stub server, sharing, telemetry, and `test/api_scan.py`, which keeps this page and the header in step);
`test/run_tests.sh` adds the model: conformance through Sessions (35 clips, all twelve heads, bit for bit), the
decoded layer, and the Model, Session, listen and reference tests on a 3.0 model folder. Every test has a time
limit and `tf_tests` caps its own memory at 2 GB.
