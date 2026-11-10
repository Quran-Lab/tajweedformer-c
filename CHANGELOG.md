# Changelog

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
