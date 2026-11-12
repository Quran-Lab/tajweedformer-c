# Changelog

## 3.1.0

A model comes from Quran Lab's server, through the Client, and from nowhere else. The same change in the Android,
Apple, web and C SDKs, and one sentence more on the sharing sheet.

- **Removed: opening a model folder the app supplies.** `tf_model_open` is gone, and the header has 66 functions.
  A Model comes from `tf_client_model` (and `tf_client_prefetch` to download ahead): the key, the signed config,
  the SDK's own download and its verification. Why: the model is Quran Lab's to deliver, verify, update and
  withdraw, and an app that carries its own copy can be told of none of that. An app that called
  `tf_model_open` does not compile; it creates a Client and calls `tf_client_model(client, NULL)`.
- **Removed: `trusted_key_hex`** in `TfClientOptions` and `TfModelOptions` (it was for tests). Remove the field
  from a designated initializer that names it.
- **The Client opens only a build it downloaded itself.** It keeps a record of each download, bound to this
  machine and user (Windows: a key protected with DPAPI; Linux: a key derived from the machine id and the user
  id, or a key file in the storage folder where the machine has no id). A model folder in the Client's storage
  without that record, put there by an installer or copied from another machine, is deleted and the model is
  downloaded. **An installation that updates from an earlier SDK downloads the model once more**; without a
  connection at that first start `tf_client_model` fails with `needs_connection`. After that a model runs
  without a network, as before.
- **Changed: a Client that Quran Lab has refused does not run the model, and removes it.** When Quran Lab
  refuses the key or the app (a revoked key, a paused or blocked app), the SDK stops running the model and
  removes the downloaded model from the device. Once the key is accepted again the model is downloaded again.
  The refusals that do this: a key that was revoked or that Quran Lab never issued; `app_not_allowed`,
  `origin_not_allowed`, `key_type_mismatch`; `app_blocked`, which is an app blocked by Quran Lab or paused by
  its developer; `developer_not_approved`. `tf_client_model` and `tf_client_prefetch` fail with that refusal
  (`invalid_key`, or `server_rejected` with the refusal's message and fix), at the next start without a network
  too, until a later answer accepts the key. What is removed: every model build in the Client's storage folder,
  with the record of its download and any partial download; nothing else in that folder (not the sharing state,
  not a recording that is not sent yet). A Model that is already open keeps running until it is closed, and
  its files are removed when it is closed, or at the next start. A removal that cannot be done is tried again
  at the next start and is never an error of its own. Before, a model on disk kept running. A missing
  connection, a timeout, a server error, a network Quran Lab does not serve and any answer that names no such
  reason never stop a model and remove nothing. `sdk_too_old` does what it did.
- **Changed: model files are requested with the installation's token.** Every request for the model (its
  manifest, each file, each resumed range) carries the app token the SDK already uses for its other calls, next
  to the key. Two refusals of such a request have their own message and fix under `server_rejected`:
  `model_attestation_required` and `model_not_allowed`; a daily limit is `download_failed` and is not retried in
  a loop.
- **Changed: the command-line tool gets its model through a Client**, as an app does: every command takes
  `--key`, `--app-id` and `--storage`, and none takes a folder. `bench` is gone (it ran several processes on one
  model folder; a server runs one process per take, each with its own storage folder). The example program's
  `--folder` mode is gone.
- **Changed: the sharing sheet and "What is shared" now say that no human ever listens to your recordings: only
  software learns from them.** Copy deck `2026-11-12.1` (consent `share-2026-11-12`) in eight languages; the
  sentence is in `sheet.who` and `what.detail`, and an app that draws its own sheet from the deck shows it with
  no change. A learner who already agreed is not asked again, and their recordings keep the version they agreed
  to.
- **Changed: the error catalogue** (the shared `errors.json`): the fix sentence of `model_missing`, the two
  reasons above, and the words of `app_blocked`.
- Tests: the shared vector `model_gate.json` (32 answers of the API and of the model host: which stop a model
  and remove it, remembered across a start and cleared by an accepting answer, and the error the app gets); a
  refusal with a real download on the device leaves no model file and no record, and the sharing state, an
  unsent recording, the device key and the remembered refusal stay; a model put back by hand is removed at the
  next start without a network; after the key is accepted again every file is downloaded again; a Model open at
  the refusal runs a take and its files go when it is closed; a removal that fails stays owed; a storage folder
  filled in advance is not opened and is replaced by a download; a record made with another key and a record of
  another build are not valid; a download, then a start with no request for a file; the two ways the key store
  can fail.

## 3.0.1

One subject, the same in the Android, Apple, web and C SDKs: what "sharing off" and "delete what I shared" must
guarantee. The rules, held by one shared test of 17 situations, each run 20 times: from the moment the call returns
the SDK hands nothing more to the system and has ended the request in flight, whatever it was (what the system had
already buffered may still go out, see below); the call returns within 3 seconds even when a request hangs; nothing
unsent stays on disk, also when the request that was in flight comes back later; turning sharing on again works
right away; and a recording made under an earlier consent is never uploaded, whatever happened in between (the
web SDK holds the first four rules in its 3.0.1 and this last one from its 3.0.2). The API is unchanged (the
same 67 functions); one behaviour changes, see the third point.
In production the sharing sheet has been on for iOS and macOS only, so no app built on this SDK had sharing on.

- **Sharing off ends the request in flight, whichever it is.** In 3.0.0 only a segment PUT could be aborted. The
  start of an upload, a batch of upload URLs and a take list could not, so `tf_sharing_set_on(client, 0)` and
  `tf_sharing_delete` waited their whole 3 seconds for such a request and returned after 3.0 to 3.4 s (the rule
  is within 3 s), and a batch of URLs that arrived after the call was followed by one more request (a PUT of a
  segment that was already deleted: no audio in it, but a request). While such a request hung, `tf_sharing_is_on`
  and every call that reads the signed config waited for it too. Now every request of the uploader carries its
  abort flag; the uploader asks before every request and before every write whether sharing went off since its
  pass began; a request that comes back late sends nothing more and writes nothing back. The call returns in
  about 20 ms, and in 2 s at most with a transport that cannot abort a request (the uploader's request then
  finishes by itself and does nothing after it).
- **"Delete what I shared" stops the queue before it asks the server** and holds it until the answer. In 3.0.0 it
  asked first, so a recording could still be uploaded under the contributor being deleted while the server was
  deleting.
- **Behaviour change: a deletion the server took turns sharing off**, as it always did on Android and Apple.
  After `tf_sharing_delete` returned 0, `tf_sharing_is_on` is 0 and the learner is not asked again; they turn
  sharing on again with the switch (`tf_sharing_set_on(client, 1)`), and what they share after that goes under a
  new contributor id. A deletion the server did not take (an error code) changes nothing: sharing stays as it
  was and the queue carries on with the recording that was in flight. An app that shows the state of the switch
  reads `tf_sharing_is_on` after the deletion. With nothing ever shared from the install the call still returns
  0 and changes nothing.
- **A recording made under an earlier consent is never uploaded.** In 3.0.0, and in every version with sharing
  before it, a recording that a stop left on the device was uploaded when sharing came on again: when the process
  ended between the off and the deletion of the queue, or when a recording could not be deleted (a file in use).
  Nothing deleted it at the next launch either. In a test with nine such recordings all nine were uploaded after
  a restart (389,931 bytes of audio; there is no limit, it is whatever the stop left). Now the settings file
  carries a count of the learner's changes of mind (sharing turned off, a deletion the server took, sharing turned
  on), written in the same write as the consent, and every recording carries the count it was begun under (a file
  `stamp` in its folder, never uploaded). A recording with another count than the one in force is never uploaded
  and is deleted before every upload pass and at launch; at a launch with sharing off the whole unsent queue is
  deleted. This holds for any number of recordings and when the process ends at any point after the call began. A
  queue of 3.0.0 uploads as before while sharing stays on.
- **`tf_sharing_delete` writes the off to disk before it asks the server**, and writes the earlier state back when
  the server does not take the deletion (nothing changes then, as before). A process that ends while the server
  is asked, or between its answer and the device's deletes, starts with sharing off and deletes what is left: the
  learner asked for a deletion, so nothing is sent until they say otherwise.
- **What can still leave after the call.** The request is aborted by closing its handle (WinHTTP) or by stopping
  the transfer (libcurl), and the SDK hands over nothing more. Bytes the system had already taken into its send
  buffer for the connection may still go out, and that can be the whole segment that was being sent: measured on
  a phone with another SDK of this family (Android 11), the system's buffer held 2,220,032 bytes when the request
  was ended, which is more than one segment; the buffers of Windows and Linux are not measured. Nothing after
  that segment is sent, and a PUT that is cut short does not complete. In the test, against a stub in place of
  the network, 0 bytes arrived after the call in every run, before and after this fix (3.0.0 already aborted a
  PUT in flight); that is the test's stub, not a property of a device.
- **The settings file is written once more after the stop.** A recording that finished uploading at the moment
  of the off call writes the same file (its count) and could, in a narrow window, have written it back with the
  consent it had read before; sharing would then have been on again at the next launch. Not seen in a test.
- The test: `tf_tests sharing_off <work dir> [runs]` (ctest `tf_sharing_off`): the 17 shared situations (12 during
  an upload, 5 where a stop leaves nine recordings behind and the process may end) and two rows of this SDK's own
  (a request that the transport cannot abort). Runs passed of 20:

  | situation | 3.0.0 | 3.0.1 |
  | - | - | - |
  | put held, off | 20 | 20 |
  | put held, delete | 0 (the server was asked first; sharing stayed on) | 20 |
  | start held, off | 0 (the call took 3.03 s) | 20 |
  | start held, delete | 0 (3.03 s; asked first; sharing stayed on) | 20 |
  | urls held, off | 0 (3.04 s; one request after the call) | 20 |
  | urls held, delete | 0 (3.04 s; one request after the call; asked first; sharing stayed on) | 20 |
  | takes held, off | 0 (3.03 s) | 20 |
  | takes held, delete | 0 (3.39 s; asked first; sharing stayed on) | 20 |
  | put held, delete refused | 0 (the server was asked while the PUT was still going) | 20 |
  | start held, delete refused | 0 (the server was asked while the start was still going) | 20 |
  | put held, off, on again at once | 20 | 20 |
  | start held, off, on again at once | 0 (3.04 s) | 20 |
  | nine left by off, on again | 0 (all nine uploaded) | 20 |
  | nine left by off, the process ends, sharing off | 0 (not deleted at the next launch) | 20 |
  | nine left by off, the process ends, on again | 0 (all nine uploaded) | 20 |
  | nine left by an accepted delete, the process ends, on again | 0 (sharing was still on; all nine uploaded) | 20 |
  | delete, the process ends while the server is asked | 0 (sharing was still on; all nine uploaded) | 20 |
  | own: the start cannot be aborted, off | 0 (3.03 s) | 20 (2.02 s) |
  | own: the start cannot be aborted, delete | 0 (3.03 s; sharing stayed on) | 20 (2.02 s) |
- The vectors hold for every SDK of the 3.0 line: `manifest_30.json`'s patch case names 3.0.100, the recorder
  vector turns sharing on again after its deletion, and the tests no longer require the files to be built with
  this SDK's exact version.

## 3.0.0

A breaking release. The SDK is now a pure inference engine: recitation audio in, the model's own output out. The
tajweed layer of 2.x (verdicts, madd counts, word scores, presets and settings) is no longer in the SDK; an app
that used it builds its own judge on the output, with the calibration and the expected units of each ayah as
read-only data. Every call has a new name. The guide: [docs/migrating-to-3.md](docs/migrating-to-3.md). The
guide also says what staying on 2.x means.

- **Five names, the same in the Android, Apple, web and C SDKs:** Client, Model, Session, Heads, Backend
  (`TfClient`, `TfModel`, `TfSession`, `TfHeads`, `TfBackend`). `tf_client_create`, `tf_client_model` (downloads on
  first use, verifies, opens and warms), `tf_session_create`, `tf_session_push`, `tf_session_finish`,
  `tf_session_heads`. A Session is one take; `TfStream` and the `tf_stream_*` calls are gone.
- **One header, no engine type.** `include/tajweedformer.h` includes only `<stddef.h>`; the shared library exports
  exactly its 67 functions. Options structs may be NULL or zeroed (the `tf_*_default_options()` calls are gone).
- **One Client per process** (`client_exists` for a second). It never waits for the network at creation.
  `tf_client_prefetch` downloads ahead with progress in bytes and resumes a cut download (its `unmetered_only`
  option cannot be honoured here: the library cannot tell the kind of connection, so it downloads); `tf_client_storage_json` and `tf_client_remove_models` manage what is on disk.
- **Model versions.** A model is addressed by version (`4.0.0`): `tf_model_version`, `tf_model_build`, and
  `TfModelOptions.version` to pin one. SDK 3.0.0 opens 3.0 builds only (`model_incompatible` otherwise).
- **Heads and the decoded layer.** `tf_heads_get` by name, `tf_heads_greedy`, `tf_heads_decoded`. `TfDecoded` is
  a plain struct (nothing to free) with the same numbers as 2.x, plus `settled` (the tokens that will not change
  any more in this take) and `is_final`. `TfSessionOptions.top_k` (1 to 38) ranks the best K tokens per frame,
  each with its log-prob and its margin to the next rank.
- **Events and the end of a take.** `on_event` reports `speechStarted`, `speechEnded` and `ended`, from what the
  model heard; `chunk->speech` says whether a chunk holds speech. A frame is speech when the token head or the
  timing head hears something, so a held sound (a long madd) does not end a take. With `end_on_silence_ms` (500
  to 60000) a take ends by itself after that much silence following speech. A take holds at most 20 minutes of
  audio and then ends by itself with `ended(limit)`.
- **Without a microphone.** `tf_session_listen` runs a sample clip that ships in the model folder through the same
  path as pushed audio (`tf_model_sample_clip`), at the clip's pace or as fast as possible.
- **Calibration and reference data, read-only.** `tf_model_calibration_json`, `tf_model_calibration`,
  `tf_model_calibration_number`; `tf_model_reference`, `tf_reference_fetch`, `tf_reference_units` (the expected
  units of every ayah, downloaded only when the app asks). The SDK applies neither.
- **Errors.** `TfError` has the 26 codes of the catalogue shared by the four SDKs; `tf_last_error`,
  `tf_last_error_fix` and `tf_error_docs` give the message, what to do and the link
  (`https://sdk.quranlab.ai/docs/errors#<code>`). A call on a NULL object keeps the first error.
- **A refusal says what to do.** When Quran Lab's server refuses the app, `server_rejected` carries the message
  and the fix of that refusal, with the server's code in brackets: an account without access yet, a key of the
  wrong type, a blocked app, and a key that belongs to another app. A key works for the first app id that used
  it; another id reads "This key belongs to another app" with the fix (a key for this app, or this id allowed on
  the key's page). `examples/quickstart.c` registers `com.example.quickstart`: give it a key of its own or put
  your app's id there.
- **Diagnose.** `tf_client_diagnose` and `tf_client_diagnose_json`: one report (SDK, key, config, model, context,
  backend, storage, last error), safe to paste into an issue. `tajweedformer diagnose` prints it.
- **Telemetry.** The SDK counts what it sees (takes, seconds of audio, speech and silence, tokens per head, per
  model version and build). What only the app knows it reports as counts: `tf_session_report_recited` and
  `tf_client_report_checks`. Still anonymous, still off with `TF_ANALYTICS_OFF`.
- **Sharing.** The calls are `tf_sharing_*` (`tf_sharing_status`, `tf_sharing_set_on`, `tf_sharing_ask`,
  `tf_sharing_answer`, `tf_sharing_delete`, ...). A take of a Session whose Model came from `tf_client_model` is
  shared by the SDK when the learner said yes; the `tf_sharing_session_*` calls are gone. Takes in a row share one
  recording: when a take ends, the recording stays open for a few seconds (3 s, or the signed config's
  `flags.share_join_ms`), and a take that starts in that time at the same sample rate continues the same Ogg
  stream; a recording of several takes then sends where each take starts. `tf_sharing_feed` takes audio the app
  already captures outside takes: the last second before a take and the half second after it go into the
  recording, so it does not begin or end inside a word (kept in memory only, dropped at once without a yes). A
  recording holds at most 20 minutes; a longer take goes on in a new recording. What a learner agreed to does
  not change and nobody is asked again.
- **Sharing off during an upload.** Turning sharing off while a recording was being uploaded could leave that recording on the device: the
  queue was deleted while the uploader still held the file open (Windows cannot delete an open file) and wrote
  the recording's state back on its way out. It was never uploaded while sharing stayed off, and was deleted
  after 7 days, but turning sharing on again within those days would have sent it. The SDK now stops the
  upload, waits for the uploader to let go (at most 3 s) and then deletes. In the library since 2.0.0; found
  by the Windows CI run of this release.
- **The example app.** `examples/quickstart.c` is one console program: the model loading with download progress,
  a sample clip without a microphone, reciting from standard input, the tokens with tentative ones marked, and
  the diagnose report. `examples/CMakeLists.txt` builds it as an app's own project.
- **Command line.** `info`, `decode`, `serve`, `bench`, `download` and the new `diagnose`. `--ayah`, `--settings`
  and the `portal` command are gone.
- **The developer site** is [sdk.quranlab.ai](https://sdk.quranlab.ai).
- **Tests.** Every test has a time limit and the test program caps its own memory at 2 GB. The ranking behind
  `top_k` is held to shared vectors: 26 rows in `ctest` (ties, a flat row, a margin a float cannot hold) and, where
  the model is, the 35 conformance clips at two contexts through a Session, ids, log-probs and margins exact. The
  README's "Verify it yourself" says what the public CI covers and what only the local proof runs.

## 2.0.3

Takes in a row share one recording. An app often stops and starts its takes while the learner is still reciting,
so shared recordings began or ended in the middle of speech. A take (`tf_sharing_session_begin` to `tf_sharing_session_finish`, or one take of a `TfStream` made with a client), your code and what a learner is asked do not
change.

- The join window. When a take ends while the learner shares, the shared recording (its Ogg stream, its item in the
  outbox, its upload) stays open for a few seconds: `flags.share_join_ms` of Quran Lab's signed config, a whole number
  from 0 to 10000 ms; 3000 when the flag is absent; 0 never joins, which is 2.0.2's behaviour. A take that starts
  within the window at the same input sample rate, with the same contributor, consent and model, continues the same
  stream: no new headers, the encoder and the resampler keep their state, segments are cut by the same plan counted
  across the takes, the bitrate ladder applies as before. The pause between the takes gets no audio. The bytes are
  those of one continuous encode of the same audio.
- Otherwise the recording closes as a session ended in 2.0.2 and the take starts a new one. It also closes at once
  when the window passes (a timer thread of the client; the wall clock decides), at `tf_client_background`, at the
  new `tf_sharing_recording_close(c)`, at `tf_client_destroy`, when sharing is turned off (the queue is deleted,
  the open recording with it), and at 20 minutes of audio or 256 takes. `completed` is the last take's; the 2 s
  minimum of a cancelled session counts the whole recording.
- The 20 minute cap never cuts a take. With 30 s or less of room, a joining take is held in memory until it ends: it
  joins when it fits, else the recording closes without it and the take starts a new one with all its audio.
- Take boundaries. A recording of two or more takes keeps where each take starts in the stream and the pause before
  it (rounded to 100 ms), and sends them once after its last segment: `POST /v1/uploads/{id}/takes`. The recording is
  uploaded when its segments are, whatever becomes of that list: a server without the route (404) or a deleted
  upload (410) drops it, a refused list (400) drops it with an issue report, 401 renews the token, and 429, 5xx or
  no answer keep it for a retry with the same body, also after a restart. A recording of one take sends nothing.
- A process that died between takes or inside one: the recording is closed at its last whole page, as before, and its take list keeps the takes that
  start inside what was recovered.
- API: `tf_sharing_recording_close(c)` (no take follows: close the recording now; with a take running, when it
  ends); `tf_sharing_session_finish` returns the take's milliseconds that went into a recording;
  `tf_sharing_outbox_json` adds `takes`; `TF_SHARING_JOIN_MS_DEFAULT`, `TF_SHARING_JOIN_MS_MAX` and
  `TF_SHARING_RECORDING_MAX_S`. The calls are in `docs/api.md`.
- Fixed: a session recovered after a crash reported a duration of 0; it now has its real duration.
- Usable as a CMake subproject: an app can take the repository by tag with `FetchContent` (or `add_subdirectory`)
  and link `tajweedformer`. Nothing is built unless the app links it, the engine's release configuration is the
  default there, and the tests and checks stay with builds of the repository itself. Before, the project only
  configured as the top-level one. The README's install is now that `CMakeLists.txt`, and CI builds it.
- Unchanged: consent (a yes is never asked again, at most three asks; the ask after a finished take leaves the
  recording open), one upload at a time and in order, the bitrate ladder, stopping and deleting. Telemetry counts a
  session per take.
- `docs/platform-client.md`: the 2.0.3 text.

## 2.0.2

- Fixed: model manifests with a `golden_tolerance` field did not verify. `tf_model_open` and the model download built the signed text without the
  `golden_tolerance` lines that the signature covers, so the current V4 build (4.0.0, the build with the CoreML
  programs and deltas) was refused as unsigned; manifests without the field were not affected. The signed text now
  has one `golden_tolerance <build> <tokens> <sites> <words>` line per build, sorted by the build key's bytes, after
  the header lines, exactly as Quran Lab signs it. A manifest whose signature does not cover a tolerance still
  verifies without it, and the tolerance is then ignored.
- Fixed: `tf_download_model` refused any build whose `min_sdk_version` is above 2.0.0 (the current V4 build needs
  2.0.1); it now accepts every build up to this SDK's version, as `tf_model_open` does.
- Tests on the two real signed V4 manifests (both builds): each verifies with the pinned key, and one changed byte
  in a header value, a tolerance value or a file line is refused. The same cross-check runs in every
  TajweedFormer SDK.
- `docs/platform-client.md`: the 2.0.2 text (the accelerator failover rules of the Android and Apple SDKs; nothing
  changes for this SDK, which has no accelerator).

## 2.0.1

- Recitation sharing is offered where Quran Lab decides, per app, in the signed config (the consent ask
  comes only then); `TfClientOptions.contributions` and `zf_platform_set_contributions` are ignored, and
  without a config nothing is offered (2.0.0 treated a missing config as on). Nothing is recorded or sent
  before the learner's answer.
- Asking: the sheet is shown at most three times per install, ever (the count carries over from 2.0.0);
  "Not now" may lead to a later ask within those three, a yes ends asking forever, and so does turning
  sharing off after a yes (the app's settings can still turn it on). A yes persists across copy decks, app
  wording and the upgrade from 2.0.0 (no sheet, no pause), and each session carries the version the learner
  agreed to. Stopping and deleting work whatever the learner agreed to.
- Your own consent UI and wording: the ask callback, `tf_client_sharing_copy_json` (the localized strings)
  and `tf_client_sharing_answer`; `tf_sharing_set_wording` replaces deck strings and records consent as
  `custom-<24 hex>`, with the wording's SHA-256 sent with each session for Quran Lab to review
  (`tf_sharing_consent_version`, `tf_sharing_consent_text`, `tf_sharing_consent_sha256`).
- Copy deck 2026-11-09.1 (consent `share-2026-11-09`) in eight languages: no longer says the recited ayah is
  sent (it is not), and deletion from Quran Lab's servers is immediate.
- Adaptive Opus bitrate on bad networks: 192, 96, 64 or 32 kbit/s, lowered when uploads fail or back up and
  raised again when they succeed, changed between segments of the same Ogg stream (`tf_sharing_bitrate`,
  `segmentBitrates` in `tf_sharing_outbox_json`). `tf_sharing_set_bitrate` no longer has an effect.
- Telemetry: deltas during the day (every 15 minutes while a client is open, `tf_client_background`, client
  close), each with a random `report_id`; new aggregate counters (launches, ayahs, ayahs per surah, words,
  tajweed rule checks and flags, sharing sheet outcomes). Register once per UTC day per install.
- A logging callback and a debug switch for the client (`TfClientOptions.on_log`, `log_user`,
  `debug_build`; appended, so 2.0.0 apps stay silent). In a debug build the client logs once when Quran Lab's
  signed config names a newer SDK: "Quran Lab SDK X is available (severity), you have Y: URL".
- `docs/platform-client.md`: what the SDK sends to Quran Lab, when, and what an app can change.

## 2.0.0 (first public release)

- TajweedFormer V4 on Windows and Linux: `tajweedformer.h`, a C library (DLL or shared object, and a
  static library) over the exact engine: verified model folders (signed manifest, every file hashed,
  layout 3), streams with every head, the decoded layer (top-1 and runner-up per frame, tokens with
  confidence) and the tajweed layer (per-site and per-word verdicts with their evidence).
- The Quran Lab platform client (register, signed configuration with the portal's tajweed block,
  anonymous telemetry, issues), resumable model downloads, and opt-in recitation sharing.
- The `tajweedformer` command-line tool: info, decode, serve, bench, download.
- x86 int8 kernels picked at run time (AVX2, AVX-VNNI, AVX-512 VNNI), each equal to the engine's own
  kernel; the V4 conformance (35 clips, every output of every chunk, every context) passes through
  the library's own API.
