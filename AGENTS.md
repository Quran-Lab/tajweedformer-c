# TajweedFormer for Windows and Linux: reference for coding assistants

Everything an assistant needs to integrate this SDK correctly: use only the names, calls and settings shown here or in the installed version's sources. People: the short version is in [README.md](README.md).

## What you get

<table>
<tr>
<td width="33%" valign="top">

**Hears the recitation**<br>Streaming recognition of Quranic recitation: audio in, and what the model heard comes out as often as every 160 ms (320 ms by default, 480 ms for longer context).

</td>
<td width="33%" valign="top">

**Every output of the model**<br>The sounds it heard with confidence and timing, and every head of the network per chunk (lengthening, gemination, ghunna, qalqalah and more): yours to build on.

</td>
<td width="33%" valign="top">

**Desktops and servers**<br>A C library and a CLI for Windows and Linux on x86-64, over the Quran Lab Core with its int8 kernels for AVX2, AVX-VNNI and AVX-512 VNNI.

</td>
</tr>
<tr>
<td valign="top">

**Private by default**<br>Offline, on the machine it runs on. A recording leaves it only when the learner agrees to share it.

</td>
<td valign="top">

**Nothing to host**<br>The SDK downloads the model (56.2 MB for the default context) from our CDN in six regions and verifies it.

</td>
<td valign="top">

**Data for your own judge**<br>The SDK scores nothing. The model's thresholds come with it as read-only data, and the expected units of each ayah where the served build carries them, so your app can judge a recitation its own way.

</td>
</tr>
</table>

## Why TajweedFormer

- **Made for the Quran.** Trained on Quranic recitation, with its own sound units and heads for what tajweed is about.
- **Feedback in the moment.** Output streams while the learner recites, so your app can respond on the word itself, not after the take.
- **One engine on every platform.** The same engine runs the [Windows and Linux](https://github.com/Quran-Lab/tajweedformer-c), [Android](https://github.com/Quran-Lab/tajweedformer-android), [iOS and macOS](https://github.com/Quran-Lab/tajweedformer-apple) and [web](https://github.com/Quran-Lab/tajweedformer-web) SDKs, all set from one portal.
- **Free, and it stays free.** Quran Lab is a non-profit waqf: the SDKs and models are never sold. The licence, NPL-1.2, and Quran Lab's access terms ask two things: no paywall on model features, and no output presented as an authoritative religious ruling. A line telling learners that automatic feedback can be wrong is encouraged, not required.

## Built for AI developers

Copy the setup prompt from [sdk.quranlab.ai](https://sdk.quranlab.ai) and paste it into your coding assistant. That one prompt sets everything up: the library, your key, and the first calls. [AGENTS.md](AGENTS.md), beside this README, gives the assistant the complete API, so the code it writes uses names that exist. Prefer to do it by hand? Get a key in the portal and start below.

## At a glance

- **What it is:** the TajweedFormer SDK for Windows and Linux: `tajweedformer.dll` / `libtajweedformer.so` with one
  C header, [`include/tajweedformer.h`](include/tajweedformer.h), over **the Core** (`qlcore.dll` / `qlcore.so`),
  Quran Lab's closed binary that holds the engine, the model formats, the keys and the recited-ayah detector. This
  repository holds the wrappers: the API, networking, the install key store, telemetry and sharing.
- **What it gives:** a pure inference engine. Audio in, the model's own output out: every head of the network, greedy
  tokens, the decoded layer, events. The calibration comes as read-only data. The SDK never says "mistake".
- **Five names, the same in every SDK:** Client, Model, Session, Heads, Backend.
- **Model:** TajweedFormer V4 as an encrypted build. SDK 4 runs encrypted builds only, and every model version from
  V4.1 on is published only encrypted, for SDK 4.0 and later.
- **Coming from 3.x:** [docs/migrating-to-4.md](docs/migrating-to-4.md) says what changes. SDK 3.x stops receiving the
  V4 model on 15 January 2027, 00:00 UTC.
- **Status:** 4.0.1, not released yet. 4.0.0 was built and tested on Windows x86-64 (MinGW-w64 GCC 13): the 35
  conformance clips through the whole 4.0 path (download, install, the model key, the Core) give outputs byte-identical to the 3.1.3 C SDK's; the
  contract's 4.0 vectors (lease, clock, container, attest proof) and a stand-in server's flow pass. Linux x86-64:
  built and tested on every change (GCC, the full `ctest` against the Core's test release for `linux-x64`); the
  conformance clips run on Windows.

## The TajweedFormer SDKs

One model and one engine, the same bits on every platform. From 4.0 the engine runs inside the Quran Lab Core, a
closed library that each SDK ships as a binary: it opens the encrypted model under a lease for each install and never
hands out its weights or keys. The SDKs around it are open source.

| Repository | What it is | Platforms | Install |
| - | - | - | - |
| [tajweedformer-android](https://github.com/Quran-Lab/tajweedformer-android) | Kotlin SDK (AAR), the exact CPU engine on every phone | Android 8.0+ on arm64-v8a and armeabi-v7a | Maven Central `ai.quranlab:tajweedformer:4.0.0` |
| [tajweedformer-apple](https://github.com/Quran-Lab/tajweedformer-apple) | Swift SDK: Neural Engine (CoreML) with the exact CPU engine as fallback | iOS 15+, macOS 12+, Apple silicon | SwiftPM, `from: "4.0.1"`, product `TajweedFormer` |
| [tajweedformer-web](https://github.com/Quran-Lab/tajweedformer-web) | Web SDK: a hosted loader, and the engine in Quran Lab's frame on your page | Browsers, and Node for desktop apps | `<script src="https://web.quranlab.ai/4/loader.js">`, or `npm install @quran-lab/tajweedformer` |
| [tajweedformer-c](https://github.com/Quran-Lab/tajweedformer-c) | C library and CLI | Windows, Linux (x86-64) | CMake `FetchContent`, tag 4.0.1 |
| [tajweedformer-flutter](https://github.com/Quran-Lab/tajweedformer-flutter) | Flutter plugin over the Android and Apple SDKs | Android and iOS | pub.dev, `flutter pub add quranlab_tajweedformer` |

The model is Quran-Lab/tajweedformer-v4 (`sdk/v4.0`), encrypted end to end, hosted by Quran Lab and served by
`cdn.quranlab.ai/models` to app keys from [sdk.quranlab.ai](https://sdk.quranlab.ai).
[sdk.quranlab.ai](https://sdk.quranlab.ai) is where you create an app, get its developer key, choose which model
revision your learners get, and see the problems your app's learners hit.

## Install

The library builds from this repository's tag inside your project, over the Quran Lab Core, which is not built from
source: download the Core's binary for your target from the release's folder,
`https://cdn.quranlab.ai/sdk/native/4.0.1/` (`qlcore-4.0.1-windows-x64.zip` with `qlcore.dll` and
`libwinpthread-1.dll`, or `qlcore-4.0.1-linux-x64.tar.gz` with `qlcore.so`; `SHA256SUMS` beside them), and unpack
it into a folder of your project, here `qlcore/`. `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(app C)

include(FetchContent)
set(QLCORE_DIR "${CMAKE_SOURCE_DIR}/qlcore" CACHE PATH "the Core's binary for this target")
FetchContent_Declare(tajweedformer
  GIT_REPOSITORY https://github.com/Quran-Lab/tajweedformer-c.git
  GIT_TAG        4.0.1)
FetchContent_MakeAvailable(tajweedformer)

add_executable(quickstart quickstart.c)
target_link_libraries(quickstart PRIVATE tajweedformer)
if(WIN32)   # the library, the Core and its thread library beside the program
  add_custom_command(TARGET quickstart POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:quickstart> "${QLCORE_DIR}/libwinpthread-1.dll" $<TARGET_FILE_DIR:quickstart> COMMAND_EXPAND_LISTS)
endif()
```

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release                                # Linux
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc        # Windows, MinGW-w64
cmake --build build
```

Ship `tajweedformer.dll`, `qlcore.dll` and `libwinpthread-1.dll` beside your program (Windows), or
`libtajweedformer.so` and `qlcore.so` in one folder (Linux). Use the Core's file exactly as it was published: the Core
checks its own code, and a changed Core gets no lease.

## Before you start

| | |
| - | - |
| **Key** | A desktop key from [sdk.quranlab.ai](https://sdk.quranlab.ai), for your app's id (a reverse-DNS id on Linux, the package family name on Windows). |
| **Systems** | Windows and Linux on x86-64. |
| **The Core** | Its binary for your target, from the release's folder (see Install): `qlcore.dll` with `libwinpthread-1.dll` on Windows, `qlcore.so` on Linux. CMake reads its folder from `QLCORE_DIR`. |
| **Compilers** | Linux: gcc or clang. Windows: MinGW-w64 GCC; an app built with MSVC links the DLL as well (only C types cross it). CMake 3.24 or later, and Ninja. |
| **Libraries** | Linux: the development files of libcurl and OpenSSL 3 (`libcurl4-openssl-dev libssl-dev` on Debian and Ubuntu). Windows: none. The build downloads libopus 1.6.1 once and checks its hash. |
| **Microphone** | The library takes samples; capturing them is your program's part. The quick start pipes the system's recorder in. |
| **Download** | The model is 56.2 MB for the default context (69.1 MB on disk), encrypted, downloaded once by `tf_client_model` into the SDK's folder; every context together is 58.5 MB. The first run needs a connection. |
| **Offline** | The first run needs a connection. After that the model works offline, and the SDK keeps itself up to date in the background when the machine is online. When it needs Quran Lab to continue, `tf_client_model` answers `TF_ERR_NEEDS_CONNECTION`. A model that is open keeps running. |
| **Analytics** | Anonymous counts and durations (never audio, text, names or device identifiers) and reports of the failures your learners hit are always sent; you find them at sdk.quranlab.ai under your app, Problems included. |
| **Takes** | One Client per process, and one take at a time. A server runs one worker process per take. |

## Quick start

From your key to what the model hears. The code is the marked region of
[`examples/quickstart.c`](examples/quickstart.c), which every build of this repository compiles (the file around it
has the includes and `main`):

```c
static void on_chunk(void *user, const TfChunk *chunk) { (void)user, puts(chunk->decoded->text); }

static int recite(const char *key) {
  TfClient *client = tf_client_create(&(TfClientOptions){.key = key, .app_id = "com.example.quickstart"});
  TfModel *model = tf_client_model(client, NULL);
  TfSession *session = tf_session_create(model, &(TfSessionOptions){.on_chunk = on_chunk, .end_on_silence_ms = 1500});
  float samples[1600];
  for (size_t n; (n = fread(samples, sizeof *samples, 1600, stdin)) > 0 && tf_session_push(session, samples, (int)n) >= 0;) {}
  return tf_session_finish(session);
}
```

Put your app's id where the example says `com.example.quickstart`: a key works for the first app id that used
it. `tf_client_model` downloads the model once, verifies every file, installs it encrypted for
this install, gets the key that opens it on this install, opens it and warms the engine. Each chunk (320 ms of audio) prints what the model heard so far; the take ends by itself after
1.5 s of silence following speech (`end_on_silence_ms`). In a noisy room a take may not end on silence, so keep
a way to stop (here the end of the input; an app calls `tf_session_finish`); the 20-minute limit ends it in any
case. Values under 1000 can end a take in a breath pause between two words. A call on a NULL object fails and keeps the first error, so one check at the
end is enough: `tf_last_error()`, `tf_last_error_fix()` and `tf_error_docs()` say what went wrong and what to do.

This library has no microphone: pipe the system's recorder in as raw mono float samples at 16 kHz.

```sh
arecord -q -f FLOAT_LE -r 16000 -c 1 -t raw | ./build/quickstart qlk_v1_...                                                       # Linux
ffmpeg -loglevel error -f dshow -i audio="<your microphone>" -ac 1 -ar 16000 -f f32le - | build\quickstart.exe qlk_v1_...        # Windows
```

(`ffmpeg -list_devices true -f dshow -i dummy` lists the Windows microphones.)

The example app is that same file, one console program to read in one sitting. It shows, in this order: the
model loading with the download's progress (`tf_client_prefetch`), a sample clip that ships with the model
played through `tf_session_listen` (output with no microphone), reciting from standard input take after take,
the tokens heard so far with the tentative ones in brackets (`settled`), and the diagnose report.
`quickstart qlk_v1_... --quick` runs only the ten lines above. `examples/CMakeLists.txt` builds it as its own
project.

Everything is in [docs/api.md](docs/api.md); an app coming from 2.x reads
[docs/migrating-to-3.md](docs/migrating-to-3.md). Coming from 3.x: `TfClientOptions.analytics` (`TfAnalytics`) is
gone, not deprecated: anonymous analytics are always on, because they are anonymous daily counts and durations only,
never audio, text, names or device identifiers.

## Evaluate the model on your own data

`tajweedformer-eval` runs the model over your own recordings, on your own machine, and writes one JSON line per file:
the transcript (units, text and word timings), a summary of each tajweed head, the processing time and, with
references, the unit and word error rates. It uses the public C API only, with your app's desktop key: the model is
downloaded once and runs inside the Core as in any app. **The raw weights are not needed to evaluate the model**, and
the tool never writes model files or plaintext. The first run needs a connection; after that it works offline, and it
keeps itself up to date in the background when the machine is online.

It ships beside the library (`tajweedformer-eval.exe` with `tajweedformer.dll`, `qlcore.dll` and
`libwinpthread-1.dll` on Windows; `tajweedformer-eval` with `libtajweedformer.so` and `qlcore.so` on Linux), and every
build of this repository makes it: the CMake lines under Install build it beside the library. It runs where the C SDK
runs: Windows x64 and Linux x86-64.

Windows (PowerShell):

```powershell
$env:QURANLAB_KEY = "qlk_v1_..."    # your app's desktop key from sdk.quranlab.ai
.\tajweedformer-eval.exe --app-id com.example.app D:\recordings --refs D:\recordings\refs.csv --out results.jsonl
Get-Content results.jsonl -Tail 1    # the totals
```

Linux:

```sh
export QURANLAB_KEY=qlk_v1_...      # your app's desktop key from sdk.quranlab.ai
./tajweedformer-eval --app-id com.example.app ~/recordings --refs ~/recordings/refs.csv --out results.jsonl
tail -n 1 results.jsonl              # the totals
```

Write the results with `--out`: Windows PowerShell 5.1's `>` rewrites a program's output as UTF-16.

`refs.csv` (UTF-8) has one row per file. A reference is an ayah range (`S:A`, `S:A-B` or `S:A-S2:B`), whose
expected units come from the model's own reference data (fetched once, as the model is), or the expected units
themselves, written as the transcript's text writes them: unit names separated by spaces, `|` between words.

```
file,reference
Husary_001001.wav,1:1
fatiha.flac,1:1-7
takes/baqarah_end.wav,2:285-286
```

The same as JSONL: `{"file": "Husary_001001.wav", "reference": "1:1"}` per line. A file finds its reference by its
path in the folder, else by its file name, else by its name without the extension.

| Flag | |
| - | - |
| `<input>` | a folder (with its subfolders: every `.wav`, `.flac`, `.pcm`, `.f32` and `.raw`), a list file (`.txt` or `.lst`: one path per line, relative to the list) or one audio file |
| `--key KEY` | the app's desktop key; default: the `QURANLAB_KEY` environment variable |
| `--app-id ID` | the app's id as registered for the key (required) |
| `--refs FILE` | references, CSV or JSONL as above |
| `--context C` | the inference context; default: the one in force for the app |
| `--threads N` | engine threads; default 0: the fast cores |
| `--heads LIST` | the heads to compute, comma-separated, or `all`; default: the SDK's default heads |
| `--out FILE` | where the JSON lines go; default: standard output |
| `--storage DIR` | the SDK's folder; default: the app's own, as for any app |
| `--version X.Y.Z` | pin a model version |

Audio: WAV (PCM 8, 16, 24 or 32-bit, float 32 or 64-bit), FLAC (up to 32-bit) and raw float32 at 16 kHz, any number
of channels (averaged to one), at 8 to 48 kHz (the SDK resamples); convert anything else first
(`ffmpeg -i in.mp3 out.flac`). A take holds 20 minutes: a longer file is evaluated on its first 20 minutes and its
line says `"limit": true`. One take runs at a time per process (the engine's rule); for more at once, run one process
per file list, each with its own `--storage`.

One line per file, here from the test run (`tf40_tests eval`: a clip of 112:1 as a 48 kHz stereo WAV), shortened
where it says `...`. The output is plain ASCII: the units' letters are `\u` escapes, which every JSON reader decodes.

```json
{"file":"Saood_112001.wav","duration_s":2.011,"sample_rate":48000,"channels":2,"limit":false,"processing_s":0.771,"rtf":0.3832,"transcript":{"text":"\u0642 u \u0644 \u0647 u \u0648 a ...","units":[{"id":22,"unit":"\u0642","start_ms":80,"end_ms":160,"confidence":0.9981},{"id":33,"unit":"u","start_ms":160,"end_ms":200,"confidence":0.9970},...],"words":[{"text":"\u0642 u \u0644 \u0647 u \u0648 a ...","start_ms":80,"end_ms":1840,"confidence":0.9706}]},"heads":{"inter1_log_probs":{"frame_ms":40,"frames":72,"units":16},"timing_log_probs":{"frame_ms":20,"frames":144,"units":15},"gem":{"frame_ms":20,"frames":144,"classes":{"single":{"frames":138,"mean":0.960402},"held":{"frames":6,"mean":0.039598}}},"attr_qalqalah":{"frame_ms":20,"frames":144,"classes":{"none":{"frames":108,"mean":0.753246},"sughra":{"frames":0,"mean":0.002035},"kubra":{"frames":36,"mean":0.244719}}},"conf":{"frame_ms":20,"frames":144,"mean":0.997019,"frames_over_half":144}},"reference":{"source":"112:1","text":"\u0642 u \u0644 | \u0647 u \u0648 a | ...","units":16,"words":4},"errors":{"units":{"ref":16,"hyp":16,"sub":0,"del":0,"ins":0,"rate":0.000000},"words":{"ref":4,"wrong":0,"rate":0.000000}}}
```

The last line holds the totals:

```json
{"total":{"files":6,"evaluated":6,"failed":0,"with_reference":6,"audio_s":27.507,"processing_s":7.804,"rtf":0.2837,"units":{"ref":138,"hyp":143,"sub":3,"del":0,"ins":5,"rate":0.057971},"words":{"ref":25,"wrong":2,"rate":0.080000},"sdk":"4.0.1","model":"4.1.0","context":"c16_l64","threads":2}}
```

`transcript` is what the public API gives: the decoded text, each unit with its id, time and confidence, and the
words between word bars with their times (V4 emits no word bars, so its `words` is one span for the take). `heads`
sums up every head computed besides `log_probs` (whose output is the transcript): a ctc head by its greedy unit
count, a class head (`gem`, `attr_*`) by the frames where each class is the most likely one and its mean
probability, `boundary` and `conf` by the mean of their sigmoid and the frames over one half. A file that cannot be
read or run has a line with `"error"` and `"code"` and the tool exits with 1; 2 means nothing ran (the arguments, the
references or no model).

The error rates. The units are the model's own (the consonant letters, the vowels and `<foreign>`; the word bar is
not a unit), so the unit error rate is a phoneme error rate.

- **Unit error rate (UER)** = (substitutions + deletions + insertions) / reference units: the Levenshtein distance
  between the reference's units and the transcript's, over the reference's length. `errors.units` has `ref`, `hyp`,
  `sub`, `del`, `ins` and `rate`. Of the alignments with the fewest edits, the split is the one that prefers a match
  or substitution, then a deletion, then an insertion.
- **Word error rate (WER)** = wrong words / reference words, the words being the reference's (an ayah's words in the
  reference data, or the `|` of a units reference). On the same alignment, a word is wrong when one of its units is
  substituted or deleted, or an inserted unit follows one of its units (an insertion before the first unit counts
  against the first word). `errors.words` has `ref`, `wrong` and `rate`. The transcript's own word bars are not used.
- The total line sums the counts over the files with a reference and divides once (a corpus rate, not a mean of the
  files' rates). The code is `ev_align` and `ev_score` in [`cli/tajweedformer_eval.c`](cli/tajweedformer_eval.c).

What leaves the machine: none of the audio (sharing needs a learner's yes, which this tool never gives); the
anonymous daily counts and durations every app of the SDK sends.

## The protected model

What happens the first time `tf_client_model` runs, and what stays on the device:

1. **The install's keys.** On Windows with a TPM, an ECDH P-256 key (to receive the model's key) and an ECDSA P-256 key (to
   prove it holds them) are made in the TPM through CNG's Microsoft Platform Crypto Provider, for this user, never
   exportable. Without a TPM they are made in CNG's Microsoft Software Key Storage Provider, not exportable, their
   private halves protected by DPAPI for this user; the SDK reports that as a software key store. On Linux one
   P-256 key does both, in a file of the storage folder (mode 0600), sealed under a key from the machine id and the
   user id, so a folder copied to another machine or user does not open (a machine without a machine id keeps the
   file unsealed, mode 0600 only).
2. **The install certificate.** `POST /v1/attest` carries both public keys and `install_sig`, the signing key's
   signature (made through the Core) over the agreement key's hash. Desktop installs are not attested: the
   certificate says `att none`, and a TPM changes only its `hw` field. **A new agreement key is made at every
   attest**; when an attest fails, the new key is deleted and the old one kept.
3. **The model key.** A challenge, the Core's proof, and `POST /v1/model-key`: the server answers the key to the
   model for this install's key and this Core release. While the machine is online the SDK keeps it up to date in
   the background (after an answer's `Retry-After` where there is one; a `503 lease_unavailable` is never a
   refusal), and an open never waits for that. The clock is the Core's own. An answer may carry a data piece (signed
   data, never code); the SDK hands it to the Core after the answer is accepted, and the Core uses it from the next
   request on.
4. **The files.** The signed config names the build and its key id (no download address); the signed manifest must
   name that key id before any file is asked for. The build's containers (encrypted, as the CDN serves them) come
   from the build's folder on the model host while the model key is asked for (4.0.1), up to four at once, and are
   used only when its answer names that same folder (https, on Quran Lab's hosts); an answer that names another is
   where they come from instead, and a key that does not come, or an answer without a folder, leaves none of them. Each
   is checked against the signed manifest as it arrives (its size and SHA-256) and handed to the
   Core, which writes the device form (encrypted again, for this install) and checks the plaintext's hash inside.
   The downloaded form is deleted. **No plaintext model file is ever written**; the only plain files are the
   calibration and the sample clips (public data).
5. **Opening.** The Core reads the device forms into its own memory with the model key. After the first run the
   model opens offline. When it needs Quran Lab to continue and Quran Lab cannot be reached, `tf_client_model`
   answers `needs_connection` (no answer) or `download_failed` (a server's error); an open that asks waits a few
   seconds at most, and not at all offline. A Model already open keeps running until it is closed.

**Refusals.** Every refusal of the model-key route goes to the Core. When the server refuses the key or the app
there, the refusal counts only when the Core verifies its signature (Quran Lab's key, this install, this request,
and recent): then the model's key and files are removed, as in 3.x. An unsigned refusal there is a failed request
and changes nothing. A refusal while a new data piece is in force sends the Core back to the last piece an answer
was accepted with, and a request refused for the Core's proof under a new piece is asked once more under it, so a
bad data piece never costs a learner the model. On the other routes 3.x's rules stand.

**The honest limit.** A model that runs on a device can be taken by someone who controls that device and has the
skill and the time. The goal is that a key, an AI agent, a copied folder or a script gets nothing usable: the
folder holds ciphertext bound to this install, and the key that opens it never exists outside the Core.

## What the SDK gives

- **Chunks.** While audio comes in, `on_chunk` gets each chunk as it is ready: its Heads, whether it holds
  speech, and the decoded layer of the take so far.
- **Heads.** The raw output of the network, per chunk and for the whole take (`tf_session_heads`): the final CTC
  log-probs, two intermediate CTC heads, the identity and timing heads, and the attribute heads (gemination,
  tafkheem, ghunna, qalqalah, face, boundary, confidence). `TfSessionOptions.heads` selects them; a head not asked
  for is not computed.
- **The decoded layer.** What the model heard, ready to show: tokens with their times, confidence, runner-up and
  margin, words, per-frame top-1 and runner-up, and with `top_k` the best K tokens of every frame. `settled`
  says how many tokens will not change any more in this take; `is_final` marks the last one.
- **Events.** `speechStarted`, `speechEnded` and `ended`, from what the model heard. A held sound (a long madd)
  is speech: the timing head keeps hearing it while the token head shows blank. With `end_on_silence_ms` (500
  to 60000) a take ends by itself when the learner stops. In a noisy room a take may not end on silence, so
  keep a way to stop (the app finishes the Session with `tf_session_finish`); the 20-minute limit ends it in
  any case. Values under 1000 can end a take in a breath pause between two words; the quick start uses 1500;
  the minimum is 500.
- **One report for support.** `tf_client_diagnose_json`: SDK, key, config, model, context, backend, storage, the
  last error and the protocol of the last model download, with nothing private in it.


## Calibration and reference data

- **The calibration** (`tf_model_calibration_json`, `tf_model_calibration_number`): the thresholds Quran Lab
  measured on the heads, read-only, never applied by the SDK (as in 3.x).
- **The reference data** (`tf_model_reference`): the table of expected units is encrypted with the model and read by
  the Core. `tf_reference_fetch` downloads it when the app asks and the Core installs it (with the Model's engine
  closed for the moment: it is refused while a take runs); `tf_reference_units` asks the Core, which reads the table at
  the first ask (that ask needs the model's key on this install, as a take does). The units and words are 3.x's: the conformance run
  holds every ayah of the Quran to the plaintext table (`test/reference_fnv.py`).

## Running in an app

- **One Client per process, created early.** `tf_client_create` never waits for the network; call
  `tf_client_prefetch` or `tf_client_model` at start.
- **Storage.** `TfClientOptions.storage` (default: `%LOCALAPPDATA%/<app_id>/tajweedformer`, or
  `$XDG_DATA_HOME/<app_id>/tajweedformer`) holds the containers, the Core's state, the install's identity and the
  SDK's own state. Nothing in it opens outside this install. `tf_client_remove_models` removes the model files.
- **A Session is one take**; a process runs one take at a time (`tajweedformer serve` runs one process per take).
- **The diagnose report** (`tf_client_diagnose_json`) adds `"protected"` (the contract's section 25): which key store
  holds the keys, the state of this install's model key (`lease`, `leaseExpires`, `leaseClass`, `refreshAfter`,
  `graceUntil`), and the Core's release. From 4.0.1 it also names the servers in use (`"endpoints"`).
- **Backup servers (4.0.1).** If Quran Lab's main servers can't be reached, the SDK uses backup servers automatically.

## Recitation sharing

How the SDK talks to Quran Lab, what it sends and when, is in [docs/platform-client.md](docs/platform-client.md) (the
same page in every TajweedFormer SDK); the C calls are in [docs/api.md](docs/api.md).

- **Every learner is asked, and the choice is theirs.** The SDK asks every learner, under the same rules in every
  app (below); the learner can say "Not now", turn sharing off, or delete what they shared at any time. No app is
  exempt and no option turns the ask off. Being asked is never consent: nothing is recorded or sent before the
  learner answers "share". Quran Lab's signed config (`flags.contributions`) is an incident switch: until the SDK
  holds a signed config, and while the switch is off, nothing is asked or shared.
- **The ask is the app's to draw.** This library has no UI: the SDK hands an ask over through the `on_consent`
  callback (or `pending_ask` and `pending_kind` in `tf_sharing_status`), with the copy deck in the learner's
  language (deck `2026-11-19.1`, consent `share-2026-11-09`, eight languages); the app draws it, calls
  `tf_sharing_shown` as it appears (the ask counts then), and reports the learner's tap with `tf_sharing_answer`.
- **When (SDK 4.0, contract section 20).** Never within 24 hours of the install (or of the upgrade to 4.0) or before
  the 3rd completed session, and only at the end of a good session: a take that ended normally, a minute of speech
  in this app session, and 3 s with no new take after it. After "Not now" the next ask needs both 14 days and 7
  sessions, the one after it 30 days and 10; one ask per app session and per day. `tf_sharing_ask` lets the app
  choose where in its flow an ask appears, never whether: it asks nothing (0) when none is due. Quran Lab's signed
  config can only make asks rarer.
- **The first ask is the full sheet; a re-ask is a card.** `kind` says which. Draw the card small and out of the
  way: at the bottom of the window, no dim, no focus taken, no timeout; the card's line and two equal buttons,
  "See how it helps" (`tf_sharing_open`: the SDK hands over the full sheet for the same ask, where the learner
  says yes or not now) and "Not now" (`tf_sharing_answer(client, 0)`). A card has no yes. Take it away when a
  Session starts or the app goes to the background: the SDK counts that as "Not now". Until the copy decks carry the
  card's three strings, the SDK hands a re-ask over as the re-ask sheet instead.
- **At most three asks, and a yes is final.** At most three asks per install, ever, a card included once it was
  on screen. A learner who answered "share" is never asked again: the grant keeps the version they agreed to across
  copy decks, wording changes and SDK upgrades, and every shared recording carries that version. After that only
  the app's settings switch (`tf_sharing_set_on`) turns sharing on or off.
- **What is shared.** Every take of a Session whose Model came from `tf_client_model`, in full, from an on-disk
  queue that survives restarts (`tf_sharing_items_json`): Ogg Opus at 48 kHz; or, for audio captured at any other
  rate once Quran Lab's signed config sets `flags.share_flac`, lossless Ogg FLAC at the capture rate, 16-bit, with no
  resampling. No device model
  is sent from Windows or Linux.
- **Own wording.** `tf_sharing_set_wording` replaces any deck string. A learner who agrees then agrees to that
  text: the consent version becomes `custom-` plus 24 hex digits of its SHA-256, and every recording shared under
  it carries `consent_text_sha256` so Quran Lab can review the wording (`TfSharing.consent_text` and
  `consent_sha256` give it to the developer).
- **Bitrate on bad networks.** Opus starts at 192 kbit/s and steps down to 96, 64 and 32 kbit/s when uploads fail
  for network reasons or the queue backs up, and back up when uploads succeed; each segment keeps one bitrate. A
  lossless recording has no bitrate to step.
- **Takes in a row are one recording.** When a take ends, the shared recording stays open for a few seconds (3 s,
  or what the signed config's `flags.share_join_ms` sets, up to 60 s): a take that starts in that time with the same sample
  rate continues the same Ogg stream. The recording closes when no take follows in time, at
  `tf_client_background`, when sharing is turned off, after 20 minutes, or when the next take has another sample
  rate. For a recording of several takes the SDK then sends where each take starts and how long the pause before
  it was.
- **Recordings that do not cut speech.** An app that already captures audio outside takes hands it to
  `tf_sharing_feed`: the last second before a take and the half second after it go into the recording. The SDK
  keeps that second in memory only, drops the feed at once without a yes, and the model never sees it. An app
  that captures only during takes gets no pre-roll and no tail.
- **Stop and delete** at any time (`tf_sharing_set_on(client, 0)`, `tf_sharing_delete`), whatever wording the
  learner agreed to. Both stop an upload that is in flight: the request is aborted and the SDK hands nothing more
  to the system once the call has returned (what the system had already buffered may still go out, which can be
  the segment being sent), the call returns within 3 seconds, and nothing unsent stays on disk. A deletion the server
  took also turns sharing off (the learner turns it on again with the switch and is not asked again); one the
  server did not take changes nothing.

## Telemetry

Anonymous by construction, as in 3.x (sums per UTC day; no audio, text or identifier), and always on in SDK 4.0:
no app can turn them off (they are anonymous daily counts and durations only, never audio, text, names or device
identifiers). Quran Lab's signed config (`flags.analytics: false`) is the only off switch.

**The ayat recited are counted by the SDK itself** (SDK 4.0, contract draft 17.1): after each take the Core's
detector finds which ayat of which surah the take recited, from the take's own units and the build's index, and the
day's totals `ayahs` and `surah_ayahs` grow by them. The take's units are journaled by the Core while it runs (every
10 seconds and at its end), so a take killed half way is still counted, once, on the day it began. Where the
detector could not tell, the app's `tf_session_report_recited` (or `_outcome`) for that take is used and sent as
`app_ayahs` and `app_surah_ayahs`; a take is never counted twice. No ayah number reaches the app, a log or the
server. `flags.recited: false` turns the detector off; `flags.recited_twins: false` its cross-surah rule.

**More automatic analytics (17.8, 17.9), the same as the Apple SDK's.** Measured ayahs per juz (`juz_ayahs`) and
those recited again in the same session (`repeat_ayahs`); seconds of speech by the learner's local hour
(`hour_speech_s`: only the hour's bucket leaves the device); finished takes and closed sessions by their seconds of
audio (`take_s`, `session_s`); whether the install was active and came back (`learner` with `active`, `new`, `d1`,
`d7`, `d30`; `learner_gap`; `streak` of local days), once per active day; the lease's requests and the takes by how
they were counted (`lease`, `recited_takes`); and the app's own judgement (`tf_session_report_recited_outcome`:
`judged` with first attempt and repeat in the session, `judged_surah_ayahs`, `correct_surah_ayahs`, `judged_tenure`
by how long the install has been in use, and once a week the install's `trend` against its four weeks before, all
listed in `reported`). An analytics session runs from the first take until the Model or the Client is closed; one a
process left open when it ended is closed at the next start. Every group is on by default; the signed config turns
one off with `flags.metrics.<group>: false` (`juz`, `hours`, `lengths`, `learner`, `repeats`, `health`, `judged`),
and then nothing of it is sent. The install's own days (its first and active days, the streak, six weeks of
judgements) are kept in the Core's journal with its recited counters (`qlc_days_*`) and never sent; the open
analytics session is `session.json` of the storage folder. A take the session context decided counts in
`recited_takes.context`, and a take counted through a refrain tie counts toward a juz only when every copy lies in
it, never as a repeat, and its judgement not toward first attempt or repeat.

**Error reports**. Every error a call hands to the app, except the five that
only say the API was used wrongly (`invalid_argument`, `client_exists`, `session_open`, `session_closed`,
`cancelled`), and every failure the SDK goes on from without telling the app (a launch's config, register or token
request answered with a failure, a config that does not verify, the background update of the model key, a telemetry delta
refused or kept 7 days, a shared recording refused for good, an installed build whose files were removed) is sent to
Quran Lab as one small report: where the cause was (`config`, `lease`, `manifest`, `download`, `storage`,
`model_open`, `session` and the rest), why (`http`, `timeout`, `hash`, `memory`, ...), the HTTP status, the transport's
or the Core's error name and code, how often it happened that day, the memory tier, the free-space bucket and the ABI.
Never a message, a path, a URL or an identifier. One report per UTC day and kind of failure, with its count; at most
16 a day and 32 waiting, kept 7 days (`errors.json` in the storage folder); sent with the telemetry and 30 s after a
new one. The signed config's `flags.issues: false` turns them off. The code the app gets is always the specific one:
`needs_connection` only when no request got an answer and nothing of the build is here; any answer of a server is
`download_failed` with what answered in its message. A failed download is also counted in the day's telemetry, with
its reason (`failure_reasons`).

## Build from source, tests and CI

CMake builds the library, the CLI, the eval tool and the tests over the Core's binary for your target (Install):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DQLCORE_DIR=<the Core's folder>
cmake --build build
ctest --test-dir build --output-on-failure
```

- **Windows x64:** MinGW-w64 GCC 13 (Strawberry Perl's `c\bin` has it), CMake and Ninja; the Core is built with the
  same toolchain.
- **Linux x86-64:** GCC or clang, libcurl, OpenSSL 3, CMake and Ninja, and the Core's `qlcore.so` for linux-x64.
  A library for every distribution from Debian 10 and Ubuntu 20.04 on (their OpenSSL is 1.1) is built on Debian 10
  with OpenSSL 3's libcrypto linked in statically (`-DOPENSSL_ROOT_DIR=<an OpenSSL 3 built with -fPIC, no-shared>
  -DOPENSSL_USE_STATIC_LIBS=TRUE`): it needs glibc 2.25 and the system's `libcurl.so.4`, and keeps OpenSSL to itself.
  Tested so, in containers: Debian 10 and Ubuntu 20.04, 22.04 and 24.04, ctest in full on each.

What `ctest` runs: `tf40_ikey` (the key store: TPM and software KSP on Windows, the key file on Linux; agreement,
signing, not exportable, a new agreement key per attest), `tf40_recited` and `tf40_outcome` (the counting rules of
17.1 and 17.9), `tf40_learner` (every group of 17.8 and 17.9 on the wire, a session a kill left open, tenure, the
weekly trend), `tf40_eval_units` (tajweedformer-eval without a model: the edit distance and its split, the reference's
words, ayah ranges, CSV and JSONL references, JSON escapes, every WAV form it reads, FLAC from this SDK's encoder and
from ffmpeg's: LPC, stereo decorrelation, 24-bit, wasted bits), `tf40_switches` (a signed config with every group off, and without), `tf40_json_leak` (the JSON parser frees a half-parsed child: quranlab-core's sdk-patches/0001),
`tf40_errors` (section 24's `errors_40.json`, a copy in `test/data/sdk_4.0.0`: every `codes`, `queue`, `telemetry` and
`signatures` case), `tf40_errors_flow` (with `TF_CONTRACT`: each stage this SDK reports, from its failure against the
stand-in server, the code the app gets and the report it sends), the
3.x library tests that need no model (client, sharing, consent, bitrate, telemetry, sdk_latest, errors, crypto), and
with `TF_CONTRACT` (a quranlab-contract checkout; its test keys only, never a server's): `tf40_lease`, `tf40_clock`,
`tf40_container`, `tf40_attest` (the contract's vectors through this library and the Core), `tf40_flac` (section
19's `vectors/sharing40`: `flac16k` byte for byte, every `flac_cases.json` case by SHA-256 and in full, `pcm16.json`,
the start bodies of `upload_start.json`), `tf40_share_ask` (section 20's `share_ask.json`: every config case, and
every step of its 40 cases, action, reason and the whole new state), `tf40_share_moments` (the ask through the SDK on
a fake clock: the settle, the card, its open, a take that ends it, the re-ask sheet in a card's place) and
`tf40_flow` (the
whole flow against a stand-in server: config, attest with `install_sig`, challenge, the lease request's closed schema
and signature, download beside the lease and from the lease answer's model folder (and nothing kept when the answer
names none, or one off Quran Lab's hosts), install, signed and unsigned stops,
`install_cert_invalid`, `challenge_invalid`, the route off, a config without the build's key id and one with another,
and the data pieces of the contract's `piece.json`: each one in force from the next request, a proof refused once
under a new piece and asked again under the last good one, a piece with a bad signature changing nothing),
`tf40_lanes` (the downloader's lanes: at most four files at once, each hashed as it comes and never read again,
resume, a partial file of other bytes, a cut connection, a mismatch, a stall, the files fetched while the lease hook
runs and none left when it fails), `tf40_qos` (Windows: the engine's threads and the pushing thread are not
power-throttled), and
`tf40_lease_clock` (a device clock moved by hand against a server's clock, through the Core's own clock). The Core used by the tests is the contract's test release
`4.0.0-c1`.

**Conformance.** `test/make_flow40.py --testbuild <folder>` takes an encrypted test build (made by quranlab-core's
`tools/make_test_build.py` from a local V4 build, with the release tool's `protected.py encrypt` and the contract's
test root), and `tf40_tests conformance <fixture> <clips.txt> <out.jsonl> <work>` runs the 35 clips through the
whole 4.0 path; the engine's `bench/v4_conformance_check.py` compares the result with
`conformance/v4_c16_l64_audio_expected.jsonl`. The same run fetches the build's reference data and reads every ayah
through the Core; its `ref_fnv` must equal `python test/reference_fnv.py <plaintext expected_tj3.zfx>`. On the same
fixture, `tf40_tests piece_order <fixture> <vectors> <work>` replays `piece_order.json`, and
`tf40_tests eval <fixture> <clips.txt> <work>` runs `tajweedformer-eval` on six files made from five of the 35 clips
(raw, float WAV in a subfolder, FLAC, 48 kHz stereo WAV, `.f32`, and one held against another ayah) with CSV
references: every line's fields, the transcripts equal to a Session's, the error counts equal to a plain edit
distance of the reference data's units, the totals; then again with no network at all and JSONL references, one of
them units text; after both, no 32 bytes of any of the build's plaintext model files anywhere in its work folder. Never
put a test build inside a repository.

**Checks.** Every change is built and tested on Linux x86-64 with the full `ctest`, and Windows x64 is built and
tested the same way for each release. The conformance run needs the model and stays a release-time check on a machine
that has it. The Core's own checks cover its binaries' symbols and strings, and the release build strips the SDK's
library.

## Layout

```
include/  tajweedformer.h     the public C API (tf_ names only)
core/     qlcore.h            the Core's interface, copied unchanged; CORE_VERSION names its commit
src/tf/   tf_api.c            the public Client: one per process, model, prefetch, storage, diagnose
          tf_model.c, tf_store.c, tf_download.c   the signed manifest, the build store, the encrypted download
          tf_ikey.c           the install key store (CNG TPM or software KSP; the Linux key file)
          tf_platform.c       the platform table the Core runs on (storage forms, clocks, the state's seal)
          tf_lease.c          the attest, the lease, refusals, the data piece, refresh: the Core host of a Client
          tf_reference.c      the reference data through the Core (qlc_reference_expected)
          tf_learner.c        the learner's days (the Core's journal) and the rules of section D
          tf_recited.c        the recited range on the Core's detector and journal; the app's fallback
          tf_session.c, tf_speech.c, tf_heads.c, tf_decode.c, tf_topk.c, tf_rank.c   a take, events, Heads, the
                              decoded layer and its ranking
          tf_client.c, tf_sharing.c, tf_http.c   registration, the signed config, telemetry, consent, HTTP
          tf_report.c         the error reports: causes, the queue, sending
          tf_share.c, tf_share_audio.c, tf_ogg.c   the shared recording
          tf_crypto.c, tf_util.c, tf_copy_deck.c, tf_error_catalogue.h   SHA-2, Ed25519, JSON, errors, copy decks
cmake/    opus.cmake          libopus 1.6.1 (pinned tarball, checked)
cli/      tajweedformer.c     info, decode, serve, download, diagnose (each through a Client)
          tajweedformer_eval.c   tajweedformer-eval: the model on your own recordings, the public API only
examples/ quickstart.c        the example app; its marked region is the README's quick start
test/     tf40_tests.c        SDK 4.0's tests; make_flow40.py makes the stand-in server's data;
                              tf40_eval_tests.inc tests tajweedformer-eval (data/eval: make_eval_flac.py);
                              tf40_errors_tests.inc the error reports (data/sdk_4.0.0: the contract's errors_40.json)
          tf_tests.c, *.inc   the 3.x library tests that need no model
tools/    build.py            build, test and package one target
docs/     api.md, migrating-to-3.md, migrating-to-4.md, platform-client.md
NAMES.md                      every public type and function
```

## Problems and wishes

Something does not work, something is missing, or you want to tell us how it went:
[open an issue](https://github.com/Quran-Lab/tajweedformer-c/issues/new/choose). There is a form for a bug, one for a
wish and one for a word of feedback. Issues are public, so leave out keys and anything that came from a learner.
A security problem does not go into an issue: use "Report a vulnerability" on the
[Security tab](https://github.com/Quran-Lab/tajweedformer-c/security). A coding assistant can file an issue for you; it
shows you the text first.

## Licence

Quran Lab is a waqf: the SDKs and the models are free to use and are never sold.

The source code in this repository and the TajweedFormer model weights are released under the
Quran-Lab No-Profit License 1.2 ([NPL-1.2](LICENSE)): use, copy, modify and build on them freely,
but never charge for them, for access to them, or for any feature they power. The weights are not
stored in this repository; Quran Lab hosts them itself and serves them to apps by
`cdn.quranlab.ai/models` under the same licence. Get access at
[sdk.quranlab.ai](https://sdk.quranlab.ai).

Third-party components keep their own licences, listed in their folders under `third_party/`.
