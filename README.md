<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/assets/quranlab-mark-dark.svg">
    <source media="(prefers-color-scheme: light)" srcset=".github/assets/quranlab-mark-light.svg">
    <img alt="Quran Lab" src=".github/assets/quranlab-mark-light.svg" width="51" height="48">
  </picture>
</p>

<h1 align="center"><picture><source media="(prefers-color-scheme: dark)" srcset="docs/banner-dark.svg"><img alt="TajweedFormer for Windows and Linux" src="docs/banner-light.svg" width="100%"></picture></h1>

<p align="center">Give your Windows or Linux app, or your server, an ear for tajweed. It follows the learner word by word and shows the one to fix: offline, on your own machines, and free.</p>

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

**Hears every word**<br>Streaming recognition of Quranic recitation while the learner recites. Results arrive as often as every 160 ms (320 ms by default, 480 ms for longer context).

</td>
<td width="33%" valign="top">

**Points to the word to fix**<br>Wrong letters and vowels, madd held too short or too long, and a score for every word. Qalqalah checks are experimental.

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

**Nothing to host**<br>The SDK downloads the model (about 59 MB) from our CDN in six regions, verifies it and keeps it up to date.

</td>
<td valign="top">

**Tuned in the portal**<br>Four presets and three word levels. Change them at any time; your app picks them up without a new build.

</td>
</tr>
</table>

### Strict or gentle: you choose

**Strict** almost never flags a correct recitation. **Balanced** is the default. **Sensitive** catches more, and **Very sensitive** catches the most, for a teacher who reviews every flag first. The word level follows your learners: **strict** for huffaz and advanced students, **general** for most apps, **beginner** for children and classes.

## Why TajweedFormer

- **Made for the Quran.** Your app names the ayah; TajweedFormer checks the recitation against it, sound by sound, and gives every word its own verdict.
- **Feedback in the moment.** Results stream while the learner recites, so your app can respond on the word itself, not after the take.
- **One engine on every platform.** The same engine runs the [Windows and Linux](https://github.com/Quran-Lab/tajweedformer-c), [iOS and macOS](https://github.com/Quran-Lab/tajweedformer-apple), [Android](https://github.com/Quran-Lab/tajweedformer-android) and [web](https://github.com/Quran-Lab/tajweedformer-web) SDKs, all set from one portal.
- **Free, and it stays free.** Quran Lab is a non-profit waqf: the SDKs and models are never sold. The licence, NPL-1.2, asks three things: no paywall on model features, no results presented as rulings, and show the notice.

## Built for AI developers

Copy the setup prompt from [portal.quranlab.ai](https://portal.quranlab.ai) and paste it into your coding assistant. That one prompt sets everything up: the library, your key, and the settings for the learners your app is for. [AGENTS.md](AGENTS.md), beside this README, gives the assistant the complete API, so the code it writes uses names that exist. Prefer to do it by hand? Get a key in the portal and start below.

## Quick start

```bash
git clone --recurse-submodules https://github.com/Quran-Lab/tajweedformer-c
cd tajweedformer-c
cmake --preset windows-clang && cmake --build --preset windows-clang    # Windows (LLVM clang + Ninja)
cmake --preset linux && cmake --build --preset linux                    # Linux (gcc or clang, libcurl)

tajweedformer info   models/sdk/v4.0
tajweedformer decode models/sdk/v4.0 take.wav --ayah 1:2 --heads all > take.json
```

```c
#include "tajweedformer.h"
TfModel *m = tf_model_open("models/sdk/v4.0", NULL);       /* verified: signature and every file's hash */
TfStream *s = tf_stream_create(m, NULL);
tf_stream_push_audio(s, pcm, n);                             /* 16 kHz mono float; other rates via the options */
tf_stream_finish(s);
TfDecoded *d = tf_stream_decode(s);                          /* what was heard, with runner-ups and confidence */
TfResult *r = tf_stream_score(s, 1, 2);                      /* tajweed verdicts for 1:2, per site and per word */
puts(tf_result_json(r));
```

Everything is in [docs/api.md](docs/api.md): settings, head selection, the decoded and tajweed layers, the portal
client, recitation sharing, downloads and the server tier. Prebuilt binaries (Windows x64 DLL, Linux x64 shared
library, the CLI) are the CI workflow's artifacts.

## More

The full reference (every layer of the API, backends, what a device downloads, platform settings, recording sharing, building and testing) is in [AGENTS.md](AGENTS.md), written for coding assistants and just as readable for you. Guides: [portal.quranlab.ai/docs](https://portal.quranlab.ai/docs).

## Licence

Quran Lab is a waqf: the SDKs and the models are free to use and are never sold.

The source code in this repository and the TajweedFormer model weights are released under the
Quran-Lab No-Profit License 1.2 ([NPL-1.2](LICENSE)): use, copy, modify and build on them freely,
but never charge for them, for access to them, or for any feature they power. The weights are not
stored in this repository; Quran Lab hosts them itself and serves them to apps by
`cdn.quranlab.ai/models` under the same licence. Request access at
[portal.quranlab.ai/access](https://portal.quranlab.ai/access).

Third-party components keep their own licences, listed in their folders under `third_party/`.
