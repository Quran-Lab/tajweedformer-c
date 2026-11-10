# Changelog

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
