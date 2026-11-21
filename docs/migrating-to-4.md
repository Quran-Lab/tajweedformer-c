# Migrating from 3.x to 4.0 (C, Windows and Linux)

4.0 keeps the 3.x API (Client, Model, Session, Heads, Backend) with two options fewer (analytics and audience, below).
What changes is underneath: the engine, the model's formats and its keys move into the Quran Lab Core, a closed binary that ships
beside this library, and the model opens only inside the Core, with a key for this install. Most apps change their
build and what they ship, and delete the analytics and audience lines if they had them.

**When to move.** SDK 3.x stops receiving the V4 model on 15 January 2027, 00:00 UTC. New model versions, from V4.1
on, are published only encrypted, for SDK 4.0: an app on 3.x never gets them.

| 3.x | 4.0 |
| --- | --- |
| The library was built from source with the engine (`FetchContent` of this repository; clang on Windows). | The library links the Core, which Quran Lab publishes with each release: `qlcore.dll` for Windows x64, `qlcore.so` for Linux x86-64. Configure with `-DQLCORE_DIR=<the Core's folder>`. On Windows the library builds with MinGW-w64 GCC, as the Core does; an app built with MSVC can still link the DLL (only C types cross it). |
| You shipped `tajweedformer.dll` or `libtajweedformer.so`. | Ship the Core too: `tajweedformer.dll`, `qlcore.dll` and `libwinpthread-1.dll` beside the program on Windows; `libtajweedformer.so` and `qlcore.so` in one folder on Linux (the library finds the Core there). Ship the Core's file as published: a changed Core gets no lease. |
| `TfClientOptions.analytics = TF_ANALYTICS_OFF` turned the anonymous analytics off. | Removed, with `TfAnalytics`, `TF_ANALYTICS_ON` and `TF_ANALYTICS_OFF`, so code that sets it no longer compiles: delete the line. Anonymous analytics are always on, because they are daily counts and durations only, never audio, text, names or device identifiers. Only Quran Lab can switch them off, from its signed config. |
| `TfClientOptions.audience = "children"`, and the `guardian` members of `TfConsentRequest` and `TfSharing`. | Removed, so code that sets or reads them no longer compiles: delete those lines. Every learner is asked the same way, whatever the app. Complying with children's privacy law is your app's and its app store's. A learner who said yes on a 3.x guardian sheet keeps that yes and is not asked again. |
| The storage folder held the model as plain files. | The model is encrypted for this install. Only the calibration and the sample clips are plain. No call reads, writes or names a model file, and nothing in the storage folder opens outside this install. The first start after the upgrade downloads the model once more, so it needs a connection once. |
| `tf_client_model` worked offline once the model was downloaded. | The first run needs a connection; after that it works offline, and the SDK keeps itself up to date in the background when the machine is online. See "The protected model". |
| `tf_session_report_recited` was the only count of recited ayahs. | The SDK counts the ayahs of each take itself. Your report is used only for a take it could not place, and a take is never counted twice, so keep calling it if you already do. `tf_session_report_recited_outcome` adds your app's own judgement: correct, or incorrect with the number of mistakes and the rules. |
| `tf_reference_fetch` and `tf_reference_units` read a plain table. | The table is encrypted with the model and read by the Core. `tf_reference_fetch` is refused with `TF_ERR_SESSION_OPEN` while a take runs, and the first `tf_reference_units` needs the model's key on this install. |
| The int8 kernel headers (`zf_gemm_vnni.h`, `zf_gemm_cuda.h`) and `ZF_NATIVE_INT8`. | Gone: the engine is in the Core. 4.0 runs the CPU engine, never an accelerator, and the CUDA tier is gone. |
| `tf_client_diagnose_json` | The same, plus `"protected"`: which key store holds the install's keys, the state of the install's model key, and the Core's release. |

## Install and requirements

- Windows x64 and Linux x86-64, as before.
- The Core for your target, from the same SDK release as this library: `qlcore-<version>-windows-x64.zip` or
  `qlcore-<version>-linux-x64.tar.gz` in `https://cdn.quranlab.ai/sdk/native/<version>/`, with `SHA256SUMS` beside
  them. `core/qlcore.h` is its interface, `core/CORE_VERSION` names the Core the library was made for, and
  `core/CORE_RELEASE` gives the SHA-256 of each of its binaries.
- Windows: MinGW-w64 GCC, CMake and Ninja. Linux: GCC or clang, libcurl, OpenSSL 3, CMake and Ninja.
- The README's "Install" has the two configure lines.

## The protected model

- **Encrypted end to end.** The model downloads encrypted, the Core installs it encrypted again for this install, and
  it is decrypted only in the Core's memory when it opens. No plaintext model file is ever written.
- **The install's keys.** The first `tf_client_model` makes this install's keys: in the TPM through Windows CNG where
  the machine has one, else in CNG's software key store (not exportable, protected for this Windows account); on Linux
  in a key file of the storage folder, sealed to this machine and account. It then gets an install certificate from Quran
  Lab. The first call takes a little longer for this.
- **Online and offline.** The first run needs a connection: the model downloads and this install gets the key that
  opens it. After that it works offline, and while the machine is online the SDK keeps itself up to date in the
  background; an open never waits for that. When the model needs Quran Lab to continue and Quran Lab gives no answer,
  `tf_client_model` fails with `TF_ERR_NEEDS_CONNECTION`; when Quran Lab answers with a server error, with
  `TF_ERR_DOWNLOAD_FAILED`. An answer that the service is briefly unavailable (503 with a `Retry-After`) is never a
  refusal. `tf_client_diagnose_json` names the state of the install's model key (`lease`).
- **A Model already open keeps running** until it is closed.
- **The clock is the Core's own.**
- **Refusals.** When Quran Lab stops a key or an app, its answer on the model-key route is signed. Only when the Core
  verifies it are the model's key and files removed, as in 3.x. Any other refusal there removes nothing and is
  `TF_ERR_SERVER_REJECTED`.
- **The honest limit.** A model that runs on a device can be taken by someone who controls that device and has the
  skill and the time. The goal of 4.0 is that a key, an AI agent, a copied folder or a script gets nothing usable, and
  that taking the model needs an expert, weeks, and a new effort for every release.

## Error codes

The catalogue is the same 26 codes, so a `switch` over them compiles as before. What changes is which code a failure
gets: an answer from a server is never `TF_ERR_NEEDS_CONNECTION`.

| What happened | Code |
| --- | --- |
| A request the call needs got no answer at all (offline, DNS, a refused connection) and nothing of the model is here yet | `TF_ERR_NEEDS_CONNECTION` |
| The model needs Quran Lab to continue, and Quran Lab gave no answer | `TF_ERR_NEEDS_CONNECTION` |
| Quran Lab answered with a failure: a server error, a 429, a redirect, a lease answer without a lease | `TF_ERR_DOWNLOAD_FAILED`; the message names what answered. Call again later |
| The connection went while part of the model was already downloaded | `TF_ERR_DOWNLOAD_FAILED`; the download resumes at the next call |
| Quran Lab refused the key or the app | `TF_ERR_INVALID_KEY`, or `TF_ERR_SERVER_REJECTED` with the reason, as in 3.x; `TF_ERR_SDK_TOO_OLD` when the key needs a newer SDK |

`tf_error_name` gives the code as text (`"needs_connection"`), and `tf_last_error_fix` what to do about it.

## Error reports

New in 4.0. Every error a call hands to your app, except the codes that only say the API was used wrongly
(`invalid_argument`, `client_exists`, `session_open`, `session_closed`, `cancelled`), and every failure the SDK goes
on from without telling your app (for example a background update that failed), is sent to Quran Lab as one small
report: where it failed, why, the HTTP status, the error's name and code, how often that day, the memory tier and the
free-space bucket. Never a message, a path, a URL or an identifier. One report per kind of failure per day, with its
count; at most 16 a day and 32 waiting, kept 7 days. Your app's page at sdk.quranlab.ai lists them under Problems, with
what to do. No option turns them off; Quran Lab can, from its signed config. A failed download is also counted in the
day's analytics, with its reason.

## Sharing

- **Every learner is asked; the choice is theirs.** The SDK asks every learner, under the same rules in every app,
  and each learner decides: "Not now", or a yes they can take back, and they can delete what they shared at any time.
  There is no option to turn the ask off and no app is exempt. Quran Lab's signed config keeps an incident switch
  (`flags.contributions`) that stops all asking and sharing; it is not a decision about your app.
- **Asked later and less often.** The first ask, the full sheet, comes never in the install's first 24 hours (or the
  first 24 hours after the upgrade) nor before the 3rd completed session, only at the end of a good session. After
  "Not now" the next ask needs 14 days and 7 sessions, the one after it 30 days and 10. Three asks at most, ever.
- **The re-ask is a card.** `TfConsentRequest.kind` is `TF_ASK_SHEET` or `TF_ASK_CARD`. Draw a card small and out of
  the way, with `card_line` and two equal buttons: `card_open` (call `tf_sharing_open`: the SDK hands you the full
  sheet for the same ask) and `card_not_now` (`tf_sharing_answer(client, 0)`). A card has no yes:
  `tf_sharing_answer(client, 1)` on a card is `TF_ERR_INVALID_ARGUMENT`.
- **Call `tf_sharing_shown`** as your screen for an ask appears: an ask counts only then. `tf_sharing_ask` returns 1
  when it handed you an ask and 0 when none is due; `TfSharing.pending_kind` says which kind is pending.
- **Takes in a row.** A shared recording now stays open for up to 60 seconds after a take (3 seconds unless Quran
  Lab's config sets it), so a learner who pauses between turns gives one recording. Nothing to change.
- **Lossless audio.** When Quran Lab's config turns it on, a take captured at a rate other than 48 kHz is shared as
  lossless FLAC at its own rate. No device model is sent from Windows or Linux.

## What stays the same

The five names, every call of a take, the Heads and the decoded layer, events, one Client per process, the default
storage folder, `tf_client_remove_models`, the calibration, the copy decks and the learner's earlier answer to the
sharing ask, with the consent version they agreed to. Quran Lab encourages, but does not require, one line where your
app shows feedback built on the model, for example: "Automatic tajweed feedback can be wrong. It does not replace a
qualified teacher."
