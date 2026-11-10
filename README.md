<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/assets/quranlab-mark-dark.svg">
    <source media="(prefers-color-scheme: light)" srcset=".github/assets/quranlab-mark-light.svg">
    <img alt="Quran Lab" src=".github/assets/quranlab-mark-light.svg" width="51" height="48">
  </picture>
</p>

<h1 align="center"><picture><source media="(prefers-color-scheme: dark)" srcset="docs/banner-dark.svg"><img alt="TajweedFormer for Windows and Linux" src="docs/banner-light.svg" width="100%"></picture></h1>

<p align="center">TajweedFormer V4 for Windows and Linux apps and servers: recitation audio in, the model's output out. On your own machines, offline, and free.</p>

<p align="center">
  <img alt="platform: Linux, Windows" src="https://img.shields.io/badge/platform-Linux%20%7C%20Windows-1c1b19?style=flat-square">
  <img alt="bit-exact: 35 of 35 parity clips" src="https://img.shields.io/badge/bit--exact-35%2F35%20parity%20clips-2a3b8f?style=flat-square">
  <img alt="status: preview" src="https://img.shields.io/badge/status-preview-2a3b8f?style=flat-square">
  <img alt="model licence: NPL-1.2" src="https://img.shields.io/badge/model%20licence-NPL--1.2-1c1b19?style=flat-square">
</p>

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

**Desktops and servers**<br>A C API, a CLI and a server runner for Windows and Linux on x86-64, with int8 kernels for AVX2, AVX-VNNI and AVX-512 VNNI.

</td>
</tr>
<tr>
<td valign="top">

**Private by default**<br>Offline, on the machine it runs on. A recording leaves it only when the learner agrees to share it.

</td>
<td valign="top">

**Nothing to host**<br>The SDK downloads the model (56.5 MB for the default context) from our CDN in six regions and verifies it.

</td>
<td valign="top">

**Tajweed scoring in SDK 2.x**<br>For a known ayah, SDK 2.x also scores the take: letters and vowels, madd held too short or too long, a score for every word. Qalqalah checks are experimental.

</td>
</tr>
</table>

### Tajweed scoring in SDK 2.x

Scoring has four presets. **Strict** almost never flags a correct recitation. **Balanced** is the default. **Sensitive** catches more, and **Very sensitive** catches the most, for a teacher who reviews every flag first. The word level follows your learners: **strict** for huffaz and advanced students, **general** for most apps, **beginner** for children and classes. You set them in the portal and your app picks them up without a new build. Scoring is part of SDK 2.x; SDK 3.0 is inference only, so build new judging on the model's output.

## Why TajweedFormer

- **Made for the Quran.** Trained on Quranic recitation, with its own sound units and heads for what tajweed is about.
- **Feedback in the moment.** Output streams while the learner recites, so your app can respond on the word itself, not after the take.
- **One engine on every platform.** The same engine runs the [Windows and Linux](https://github.com/Quran-Lab/tajweedformer-c), [Android](https://github.com/Quran-Lab/tajweedformer-android), [iOS and macOS](https://github.com/Quran-Lab/tajweedformer-apple) and [web](https://github.com/Quran-Lab/tajweedformer-web) SDKs, all set from one portal.
- **Free, and it stays free.** Quran Lab is a non-profit waqf: the SDKs and models are never sold. The licence, NPL-1.2, asks three things: no paywall on model features, no results presented as rulings, and show the notice.

## Built for AI developers

Copy the setup prompt from [portal.quranlab.ai](https://portal.quranlab.ai) and paste it into your coding assistant. That one prompt sets everything up: the library, your key, and the settings for the learners your app is for. [AGENTS.md](AGENTS.md), beside this README, gives the assistant the complete API, so the code it writes uses names that exist. Prefer to do it by hand? Get a key in the portal and start below.

## Install

There are no prebuilt binaries: CMake builds the release tag inside your own project. `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(app C CXX)

include(FetchContent)
FetchContent_Declare(tajweedformer
  GIT_REPOSITORY https://github.com/Quran-Lab/tajweedformer-c.git
  GIT_TAG        2.0.3)
FetchContent_MakeAvailable(tajweedformer)

add_executable(quickstart quickstart.c)
target_link_libraries(quickstart PRIVATE tajweedformer)
if(WIN32)   # the library beside the program
  add_custom_command(TARGET quickstart POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:quickstart> $<TARGET_FILE_DIR:quickstart> COMMAND_EXPAND_LISTS)
endif()
```

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release                                                         # Linux
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++     # Windows
cmake --build build
```

To build the repository by itself (the library, the `tajweedformer` command-line tool and the tests), clone it with `git clone --recurse-submodules --branch 2.0.3 https://github.com/Quran-Lab/tajweedformer-c`, then `cmake --preset linux` or `cmake --preset windows-clang`, and `cmake --build --preset` with the same name.

## Before you start

| | |
| - | - |
| **Key** | A desktop key from [portal.quranlab.ai](https://portal.quranlab.ai), for your app's id (a reverse-DNS id on Linux, the package family name on Windows). |
| **Systems** | Windows and Linux on x86-64. |
| **Compilers** | Linux: gcc or clang. Windows: LLVM clang on the Microsoft runtime, or MinGW-w64 GCC. Microsoft's `cl.exe` cannot build the engine. CMake 3.24 or later, and Ninja. |
| **Libraries** | Linux: the development files of libcurl and OpenSSL 3 (`libcurl4-openssl-dev libssl-dev` on Debian and Ubuntu). Windows: none. The build downloads libopus 1.6.1 once and checks its hash. |
| **Microphone** | The library takes samples; capturing them is your program's part. The quick start pipes the system's recorder in. |
| **Download** | The model is 56.5 MB for the default context (73.5 MB on disk), downloaded once into a folder you choose; every context together is 58.9 MB. |
| **Streams** | One live stream per process at a time. A server runs one worker process per stream. |

## Quick start

From your key to what the model hears from the microphone. Save this as `quickstart.c` beside the
`CMakeLists.txt` of Install (it is [`examples/quickstart.c`](examples/quickstart.c), which every build of this
repository compiles):

```c
#include <stdio.h>
#include <string.h>
#include "tajweedformer.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

typedef struct {
  TfModel *model;
  TfStream *stream;
} Take;

static void on_chunk(void *user, const TfChunk *chunk) {   /* every 320 ms of audio: what the model heard so far */
  const Take *take = user;
  TfDecoded *heard = tf_stream_decode(take->stream);
  const TfToken *t;
  const int n = tf_decoded_tokens(heard, &t);
  printf("%6d ms:", chunk->end_ms);
  for (int i = 0; i < n; ++i) printf(" %s", tf_model_token_name(take->model, t[i].id));
  printf("\n");
  fflush(stdout);
  tf_decoded_free(heard);
}

int main(int argc, char **argv) {
  if (argc < 2) return fprintf(stderr, "usage: quickstart qlk_v1_... < mono float samples at 16 kHz\n"), 2;

  /* 1. the key: Quran Lab's signed config names the model revision this app gets */
  TfClientOptions co = tf_client_default_options();
  co.key = argv[1];
  co.app_id = "com.example.quickstart";      /* your app's id, as registered for the key */
  co.app_version = "1.0";
  co.storage_dir = "quranlab";               /* the SDK's own state */
  TfClient *client = tf_client_create(&co);
  if (!client || tf_client_launch(client)) return fprintf(stderr, "portal: %s\n", tf_last_error()), 1;
  const char *config = tf_client_config_json(client);   /* verified; "recommended":{...,"revision":"..."} */
  const char *at = config ? strstr(config, "\"recommended\"") : NULL;
  char revision[80] = "";
  if (at && (at = strstr(at, "\"revision\"")) && (at = strchr(at + 10, '"'))) {
    const size_t len = strcspn(at + 1, "\"");
    if (len < sizeof revision) memcpy(revision, at + 1, len);
  }
  if (!revision[0]) return fprintf(stderr, "the config recommends no model for this key\n"), 1;

  /* 2. the model: downloaded once (56.5 MB), resumed if interrupted, every file checked */
  TfDownloadOptions dl = tf_download_default_options();
  dl.key = argv[1];
  dl.revision = revision;
  dl.dest = "model";
  dl.contexts = "c16_l64";                   /* the default context: 320 ms chunks */
  if (tf_download_model(&dl)) return fprintf(stderr, "download: %s\n", tf_last_error()), 1;
  TfModel *model = tf_model_open("model", NULL);
  if (!model) return fprintf(stderr, "model: %s\n", tf_last_error()), 1;

  /* 3. audio in, model output out */
  Take take = {model, NULL};
  TfStreamOptions so = tf_stream_default_options();
  so.on_chunk = on_chunk;
  so.user = &take;
  take.stream = tf_stream_create(model, &so);
  if (!take.stream) return fprintf(stderr, "stream: %s\n", tf_last_error()), 1;
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
#endif
  float samples[1600];                       /* 100 ms at 16 kHz */
  size_t n;
  while ((n = fread(samples, sizeof *samples, 1600, stdin)) > 0) tf_stream_push_audio(take.stream, samples, (int)n);
  tf_stream_finish(take.stream);             /* the end of the take: the last chunks arrive */

  tf_stream_destroy(take.stream);
  tf_model_close(model);
  tf_client_destroy(client);
  return 0;
}
```

Build it with the three commands under Install, then pipe the microphone in as raw mono float samples at 16 kHz. Capture is the system's recorder, because it is a platform API this library does not wrap:

```sh
arecord -q -f FLOAT_LE -r 16000 -c 1 -t raw | ./build/quickstart qlk_v1_...                                                       # Linux
ffmpeg -loglevel error -f dshow -i audio="<your microphone>" -ac 1 -ar 16000 -f f32le - | build\quickstart.exe qlk_v1_...        # Windows
```

(`ffmpeg -list_devices true -f dshow -i dummy` lists the Windows microphones.) The first run downloads the model
into `model/`; later runs only check it. Each line printed is what the model heard so far; `tf_stream_decode` also
gives every token's confidence, runner-up and times, and the chunk carries every head of the network you selected.

In SDK 2.x a finished take of a known ayah can also be scored: `tf_stream_score(stream, surah, ayah)` gives verdicts
per site and per word.

Everything is in [docs/api.md](docs/api.md): settings, head selection, the decoded and tajweed layers, the portal
client, recitation sharing, downloads and the server tier. There are no prebuilt binaries: the library is built
from source.

## More

The full reference (every layer of the API, backends, what a device downloads, platform settings, recording sharing, building and testing) is in [AGENTS.md](AGENTS.md), written for coding assistants and just as readable for you. Guides: [portal.quranlab.ai/docs](https://portal.quranlab.ai/docs).

## Licence

Quran Lab is a waqf: the SDKs and the models are free to use and are never sold.

The source code in this repository and the TajweedFormer model weights are released under the
Quran-Lab No-Profit License 1.2 ([NPL-1.2](LICENSE)): use, copy, modify and build on them freely,
but never charge for them, for access to them, or for any feature they power. The weights are not
stored in this repository; Quran Lab hosts them itself and serves them to apps by
`cdn.quranlab.ai/models` under the same licence. Get access at
[portal.quranlab.ai](https://portal.quranlab.ai).

Third-party components keep their own licences, listed in their folders under `third_party/`.
