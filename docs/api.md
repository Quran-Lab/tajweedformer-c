# The C API: `tajweedformer.h` (SDK 2.0.2)

One library for Windows and Linux apps and for servers: `tajweedformer.dll` / `libtajweedformer.so` (or the static
library), with `include/tajweedformer.h` and the engine's `zf_tajweed.h`. It runs the V4 TajweedFormer model only
(SDK 2.0 hosts V4; v3.2 stays on the 1.x SDKs). The concepts and numbers are the same as in the Android, Apple
and Web SDKs: raw heads, the decoded layer, the tajweed layer, settings with the same precedence.

## Open a model folder

```c
#include "tajweedformer.h"

TfModelOptions o = tf_model_default_options();   /* signature required, every file hashed */
o.context = NULL;                                 /* the portal's, else calibration.json's default (c16_l64) */
o.sample_rate = 48000;                            /* push audio at its own rate (8 to 48 kHz) */
TfModel *m = tf_model_open("models/sdk/v4.0", &o);
if (!m) { fprintf(stderr, "%s: %s\n", tf_error_name(tf_last_error_code()), tf_last_error()); return 1; }
puts(tf_model_info_json(m));    /* model id, revision, contexts, chunk length, tokens, presets, heads */
```

`tf_model_open` refuses anything that is not a verified V4 folder: manifest format 3 (layout 3: one shared
`layers.zfw` and `frontend.zfw`, a `frames.zfw` per context; engine format 3) or format 2 (a folder per context;
engine format 2), family `tajweedformer`, `min_sdk_version` at most 2.0.2, an Ed25519 signature by Quran Lab's key (key id
`660a07197c4808da`), and every file it reads matching its size and SHA-256. A file the manifest does not list is
never opened. Errors: `signature_invalid`, `signature_required`, `checksum_mismatch`, `not_listed`,
`sdk_too_old`, `unsupported_model`, `calibration_invalid`.

## Settings: app code > portal > calibration

```c
tf_settings_set_app_json(m, "{\"preset\":\"sensitive\","
                            "\"overrides\":{\"cons_sub\":{\"threshold\":1.5}},"
                            "\"checks\":{\"qalqalah\":true},"
                            "\"word\":{\"preset\":\"general\"}}");   /* strict: TF_ERR_SETTINGS on any invalid value */
tf_settings_set_remote_json(m, config_text, len);                     /* lenient: invalid fields dropped */
puts(tf_settings_json(m));   /* every class and check with its threshold, status and source; "word"; "dropped"; "refused" */
```

Presets `strict`, `balanced`, `sensitive`, `very_sensitive`; per-class overrides by preset or raw fused-LLR
threshold (-20 to 20); checks `letters vowels length madd gemination qalqalah lazim`; the word level (`preset`
from calibration.json's `word` block, or a raw `threshold`). A check whose calibration status is `off` is
turned on only by app code in a debug build (`TfModelOptions.debug_build`), never by the portal. Until
calibration.json carries the `word` block, word scores are reported and word flags are `null`.

## Stream audio and choose the heads

```c
static void on_chunk(void *user, const TfChunk *c) {
  int frames;
  const float *conf = tf_heads_get(c->heads, TF_HEAD_CONF, &frames);   /* [frames] logits, 20 ms */
  /* c->index, c->start_ms, c->end_ms; heads not selected are NULL */
}

TfStreamOptions so = tf_stream_default_options();
so.heads = TF_HEADS_ALL;               /* or TF_HEADS_DEFAULT | TF_HEAD_BIT(TF_HEAD_GHUNNA); 0 = the default */
so.on_chunk = on_chunk;
TfStream *s = tf_stream_create(m, &so);
tf_stream_push_audio(s, pcm, n);       /* mono float in [-1, 1] at o.sample_rate; any piece size */
tf_stream_finish(s);                   /* the end of the take */
```

| Head (`tf_head_info`) | Width | Rate | Classes |
|---|---|---|---|
| `log_probs`, `inter1_log_probs`, `inter4_log_probs` | 38 | 40 ms | tokens.txt |
| `id_log_probs`, `timing_log_probs` | 38 | 20 ms | tokens.txt (timing 0 = pause) |
| `gem` | 2 | 20 ms | single, held |
| `attr_tafkheem` | 3 | 20 ms | thin, heavy, low_heavy |
| `attr_ghunna` | 5 | 20 ms | none, plain, geminate, ikhfa, idgham |
| `attr_qalqalah` | 3 | 20 ms | none, sughra, kubra |
| `attr_face` | 4 | 20 ms | none, imala, tasheel, sakt |
| `boundary`, `conf` | 1 | 20 ms | logits (apply a sigmoid) |

The default selection is what the scorer reads (`log_probs`, `inter1_log_probs`, `timing_log_probs`, `gem`,
`attr_qalqalah`) plus `conf`. The stream always keeps the scorer's heads, so scoring works whatever is selected.
A head that is not selected is not computed at all (the engine's lazy heads, zf_tajweed_stream_set_heads,
API 3) and comes back NULL; every computed head is the same bits whatever else is selected. One live stream per process (the engine's rule): `tf_stream_create` answers `TF_ERR_BUSY` while another
lives.

## The decoded layer

```c
TfDecoded *d = tf_stream_decode(s);           /* or tf_decode(m, &heads) for heads from anywhere */
const TfToken *t; int n = tf_decoded_tokens(d, &t);
for (int i = 0; i < n; ++i)
  printf("%s %d-%d ms logp %.2f runner-up %s margin %.2f conf %.2f\n", tf_model_token_name(m, t[i].id),
         t[i].start_ms, t[i].end_ms, t[i].log_prob, tf_model_token_name(m, t[i].runner_up), t[i].margin, t[i].confidence);
puts(tf_decoded_json(d, 1));                  /* with every frame's top-1 and runner-up */
tf_decoded_free(d);
```

Per 40 ms frame: the top-1 and runner-up token with their log-probs and the margin. Tokens: the greedy CTC runs
with the peak log-prob, the runner-up and margin at the peak, and `confidence` = the mean sigmoid of the `conf` head
over the token's 20 ms frames. The token ids equal the engine's own greedy transcript (`tf_stream_greedy`). Times
are the model's frame clock from the start of the take. V4 rarely emits the word boundary `|`, so for words use the
tajweed layer (it aligns the known ayah).

The decoded layer goes deeper with `tf_decode_k(m, &heads, k)` / `tf_stream_decode_k(s, k)` (k 2 to 38): every
frame also carries its k best tokens (`tf_decoded_ranked`, and "ranked" in each JSON frame).

## Top-K for any ranked head

```c
TfTopK *t = tf_stream_topk(s, TF_HEAD_GHUNNA, 5);  /* or tf_topk(&heads, head, k) for heads from anywhere */
for (int f = 0; f < t->frames; ++f)
  printf("%s %.3f (margin %.3f)\n", tf_head_info(TF_HEAD_GHUNNA)->classes[t->ids[f * t->k]], t->values[f * t->k],
         t->margins[f * t->k]);
tf_topk_free(t);
```

Per frame the k best entries of a token head (`log_probs`, `inter1_log_probs`, `inter4_log_probs`, `id_log_probs`,
`timing_log_probs`: k 1 to 38) or a class head (`gem` 2, `attr_tafkheem` 3, `attr_ghunna` 5, `attr_qalqalah` 3,
`attr_face` 4 classes: names in `tf_head_info(head)->classes`), ranked highest first with equal values by the
lower id (the engine's `zf_tajweed_topk` / `zf_tajweed_stream_topk`). `margins[j]` is the value minus the next
rank's, as a double from the float values (also past k), and 0 for the last rank of the row. `boundary` and
`conf` are single logits and have no ranking. A stream ranks only heads it computes (`TfStreamOptions.heads`).

## The tajweed layer

```c
TfResult *r = tf_stream_score(s, 1, 2);        /* or tf_score(m, &heads, 1, 2, "fp32") for heads from elsewhere */
const TfWordScore *w; int nw = tf_result_words(r, &w);
for (int i = 0; i < nw; ++i)
  if (w[i].flagged == 1) printf("word %d score %.2f > %.2f (%d-%d ms)\n", w[i].word, w[i].score, w[i].threshold, w[i].start_ms, w[i].end_ms);
const ZfTajweedSite *sites; int ns = zf_tajweed_result_sites(tf_result_engine(r), &sites);
puts(tf_result_json(r));                        /* sites, madd, qalqalah, gemination, words, summary, settings, raw */
tf_result_free(r);
```

- Sites: one per scored (window, class): expected unit, best alternative, LLR per head (`final`, `inter1`), the
  fused LLR, the threshold in force, flagged, the expected unit's frames on the best path.
- Madd: count (2, 4, 6), ratio, display count, allowed range, duration, flagged. Qalqalah and gemination: score,
  threshold, frames, flagged.
- Words: `score` = the largest fused LLR of any single-unit alternative inside the word (all classes), its evidence
  site, `threshold` and `flagged` from the word level, the word's time span, and how many of its items were flagged.
- `raw`: the engine's own JSON, every number it computed.

## Portal (portal.quranlab.ai)

```c
TfClientOptions co = tf_client_default_options();   /* telemetry on (anonymous deltas of the day's sums) */
co.key = "qlk_v1_...";                               /* a desktop key from the portal */
co.app_id = "com.example.Hifz";                      /* Linux reverse-DNS id, or the Windows package family name */
co.app_version = "1.2.0";
co.storage_dir = "/home/me/.local/share/hifz/quranlab";
TfClient *c = tf_client_create(&co);                  /* opening a client counts one launch */
tf_client_launch(c);                                  /* token, register (once a day), verified config, telemetry, uploads */
tf_client_apply_config(c, m);                         /* the portal's tajweed block into the model's settings */
/* after each session */
TfSessionStats st = {sizeof st}; st.model = m; st.decoded = d; st.result = r; st.audio_s = 6.3; st.compute_s = 1.1;
st.chunks = 20; st.feedback_shown = 1;
tf_client_session_finished(c, &st);
/* the window is minimized or the app goes idle */
tf_client_background(c);
/* at exit */
tf_client_destroy(c);                                 /* also a send point */
```

The config is verified before use: the key id must be pinned, the Ed25519 signature must cover the exact body
bytes, `v` must be 1 and `expires_at` in the future; otherwise the last good config stays. Refresh with
`tf_client_refresh_config` (ETag, 304) at launch and every 60 s while in use. Issues carry a kind and a code, never
free text, one per signature per day.

The config may name a newer SDK for the app's platform (`"sdk_latest": {"version", "severity", "url"}`, 2.0.1).
In a debug build the client then logs once, at `TF_LOG_WARNING`: `Quran Lab SDK 2.0.2 is available (recommended),
you have 2.0.1: https://...`. A release build stays silent, and an equal or older version or a malformed field is
ignored. The library is always built as a release, so the app says which build it is:

```c
#ifndef NDEBUG
co.debug_build = 1;            /* the client's developer log on */
#endif
co.on_log = my_log;            /* void my_log(void *user, int level, const char *message); NULL: stderr */
co.log_user = my_state;
```

Unknown top-level fields of the signed config are ignored, and the signature covers the exact body bytes, so a
field the server adds later neither breaks verification nor changes the settings.

### Telemetry (2.0.1: deltas)

The body is telemetry v2 of [platform-client.md](platform-client.md): no audio, no text, no identifiers; sums and
histograms per UTC day. Since 2.0.1 the SDK sends deltas: what was counted since the previous delta of the same
UTC day, never across midnight, each with a fresh random `report_id` (32 lower-case hex). A delta is sealed on disk
(`storage_dir/report-<day>-...json`) before it is sent and a retry sends the same bytes, so the server counts it
once. 202 deletes it; 400 and 413 drop it (a 400 also queues a `bad_request:events` issue); 401 renews the token;
429, 5xx and network errors keep it for later (1 minute, then 10 minutes on the timer; the next launch). Unsent
deltas older than 7 days are deleted.

Send points, with no work for the app beyond the calls it already makes:

- every 15 minutes while a client is open: the client's own timer (a thread started by `tf_client_launch`), or the
  next `tf_client_session_finished` after 15 minutes;
- `tf_client_background(c)`: the app goes to the background or idle;
- `tf_client_destroy(c)`;
- `tf_client_launch(c)`: this launch and whatever earlier runs left.

`tf_client_flush(c, include_today)` seals and sends on demand. The 2.0.1 fields, each left out when zero:
`launches` (each `tf_client_create`), `ayahs`, `surah_ayahs` (per surah of each scored result), `words` (words
scored), `rule_checks` / `rule_flags` (per site class, `madd`, `qalqalah`, `gemination` and `word`: the verdicts
whose check was on, and how many of those were flagged), `share_asked`, `share_yes`, `share_no`. The signed config's
`flags.analytics: false` stops telemetry and drops the queue; `co.telemetry = ZF_ANALYTICS_OFF` is the developer's
opt-out in code.

`POST /v1/register` goes out at most once per UTC day per install (a day stamp in `client.json`, written after a
200; a failed attempt is tried again, and `new_install` survives it): at `tf_client_launch`, and from the timer once
the day changed.

## Recitation sharing

Whether sharing is offered is Quran Lab's decision per app, in the signed config (`flags.contributions`); since
2.0.1 `co.contributions` and `zf_platform_set_contributions` are ignored. Set `co.children = 1` for a children's
app. Where sharing is offered, the SDK decides when to ask (after a finished session with feedback, never on first
launch) and shows the sheet at most three times per install, ever: "Not now" (or a sheet left unanswered) re-asks
at most twice, at least 7 sessions apart; the count carries over from 2.0.0's `sharing.json`. A "share" answer
ends asking for good: the grant keeps the version in force when the learner agreed, across copy decks, wording
changes and upgrades from 2.0.0 (a 2.0.0 grant keeps `share-2026-11-07`), and every shared session carries that
version. Turning sharing off ends asking too; after that only `tf_client_sharing_set` turns sharing on or off, and
`tf_client_sharing_ask_now` asks nothing. Offered is never consent: nothing is recorded or sent before the learner
answers "share".

This library has no UI, so there is no theme to set: the app's own sheet is the C SDK's sheet. The SDK asks through
`co.on_consent` (or, without a callback, `"pendingAsk":true` in `tf_client_sharing_status_json`), with the copy deck
for `co.language` (en, ar, id, ur, tr, ms, fr, bn; deck `2026-11-09.1`, consent `share-2026-11-09`) and the app's
own wording applied (`TfConsentRequest.copy_json`, or `tf_client_sharing_copy_json(c)`); `req->update` is always 0
(kept for source compatibility, like the status' `updateNeeded`, always false). The app shows its sheet and reports
the learner's tap with `tf_client_sharing_answer(c, 1 or 0)`. Every ask is counted (`asks`, telemetry `share_asked`),
answers count `share_yes` / `share_no`, and the re-ask rules are the SDK's. Settings: `tf_client_sharing_set` (the
switch; not counted as a sheet answer), `tf_client_sharing_status_json`, `tf_client_sharing_delete`.

### Your own wording

```c
const char *keys[]  = {"sheet.what", "impact.minutes.other"};
const char *texts[] = {"The audio of your recitations in this app, and the app's version. Nothing else.",
                       "You have shared {minutes} minutes. Thank you."};
tf_sharing_set_wording(c, keys, texts, 2);   /* right after tf_client_create, at every start */
tf_sharing_consent_version(c);                /* "custom-" + 24 hex */
tf_sharing_consent_text(c);                   /* the canonical text, for Quran Lab's review */
tf_sharing_consent_sha256(c);                 /* its SHA-256, 64 hex */
```

Any deck string can be replaced by key (`impact.minutes` by plural category). An unknown key, an empty or blank
text, text that is not UTF-8 or a key given twice is `TF_ERR_ARGUMENT` and nothing changes; `n = 0` clears. The
learner then agrees to that text: the consent version becomes `custom-` plus the first 24 hex digits of the
SHA-256 of the canonical text ([platform-client.md](platform-client.md)), and every session shared under a grant to
it carries `consent_text_sha256` next to `consent_version`. Without own wording the field is absent. Wording set or
changed after a learner agreed changes nothing for that learner (no ask, the version and hash recorded at the
grant); learners who agree afterwards record the new version. Withdrawal and deletion work whatever version the
learner agreed to. Disclosure lines must stay honest (what is sent, to whom, why, how to stop and delete): Quran
Lab can switch sharing off for an app whose wording is not.

### Deletion

"Delete what I shared" needs no app credential and cannot be done by the app developer: at the first shared
session the SDK makes a contributor id, a P-256 device key (Windows: the private key protected with DPAPI for the
current user; Linux: a 0600 file in the storage directory) and a recovery code. `tf_client_sharing_delete` asks
for a challenge, signs `quranlab-contribution-delete 1` with the device key and posts it, with no key or token;
`tf_client_sharing_recovery_code` shows the code (for portal.quranlab.ai/delete when the device is lost);
`tf_client_sharing_summary_json` gives the learner's shared minutes. No call exports the key.

### Sharing whole sessions

With the learner's consent on, every session the SDK listens to is shared in full, in the background:

```c
TfStreamOptions so = tf_stream_default_options();
so.client = c;                 /* each take of this stream is shared while consent is on; nothing otherwise */
so.input_route = "usb";        /* built_in, wired, usb, bluetooth, car, continuity, other, unknown */
TfStream *s = tf_stream_create(m, &so);
/* push, finish as usual: the take's audio is resampled to 48 kHz, encoded (libopus 1.6.1: AUDIO, fullband,
 * VBR, complexity 10, 20 ms frames, the bitrate ladder below) and muxed as one Ogg Opus stream */
```

Apps that capture audio outside a `TfStream` use `tf_sharing_session_begin(c, rate, route)`,
`tf_sharing_session_push` and `tf_sharing_session_finish(s, completed)` (a cancelled session is kept only from
2 s). The stream is cut into segments of 10, 20, 40, then 60 s at page boundaries; each closed segment goes up at
once while the session continues: `POST /v1/uploads` (the session JSON with `contributor_key` and
`recovery_hash`), presigned URLs in batches of 20 (the next batch when 5 are left; a 403 asks again from that
segment), each segment PUT straight to R2 with `content-type: audio/ogg` and nothing else. The uploader is a
thread of the client; the queue is an on-disk outbox (`storage_dir/outbox/<session>/`) that survives a crash or a
restart: `tf_client_launch` (or `tf_sharing_outbox_resume`) resumes it, and a session whose process died is
closed at its last whole page. Unsent sessions older than 7 days are deleted. `tf_client_sharing_set(c, 0)`
aborts the PUT in flight and deletes the queue at once. Errors follow the platform client doc: `contributor_revoked`
/ `contributor_key_mismatch` make a new contributor, key and recovery code; `segment_cap` ends the session's
upload; `volume_limit` / `capped_limit` wait until the next UTC day; 5xx, 429 and network errors back off.
`tf_sharing_outbox_pending` counts what is queued; `tf_sharing_outbox_json` lists it.

Bitrate on bad networks (2.0.1): Opus starts at 192 kbit/s and follows a ladder of 192, 96, 64 and 32 kbit/s, one per
client, in memory. An upload's network failure (no connection, a timeout, HTTP 408 or 5xx on `POST /v1/uploads`,
the URL request or a segment PUT) steps down; a segment that closes while 120 s or more of closed audio waits
steps down; three segment PUTs in a row that succeed while at most 30 s waits step up. The rung in force when a
segment opens is that segment's bitrate (set before its first packet: one stream, no new headers), recorded in its
marker on disk (`seg-NNNNN.json`, kept through crash recovery) and listed in `tf_sharing_outbox_json` as
`segmentBitrates`. `tf_sharing_bitrate(c)` gives the rung the next segment opens with. `tf_sharing_set_bitrate` has
no effect since 2.0.1. The session JSON does not change with the bitrate.

Trust: desktop uploads at the lower `capped` tier (an unattested token; the server caps per key and per
contributor and marks the sessions attested=false). `flags.upload_trust[windows|linux]` absent or `capped` =
upload; `off` or `attested` = nothing is uploaded (`tf_client_sharing_status_json` says `mayUpload`).

## Downloads (the native profile)

```c
TfDownloadOptions d = tf_download_default_options();   /* cdn.quranlab.ai/models, Quran-Lab/tajweedformer-v4, sdk/v4.0 */
d.key = "qlk_v1_..."; d.revision = "<commit from the config>"; d.dest = "models/sdk/v4.0";
tf_download_model(&d);    /* signed manifest, then every CPU file, resumed with Range, SHA-256 checked */
```

## Command line and server tier

```
tajweedformer info   models/sdk/v4.0
tajweedformer decode models/sdk/v4.0 take.wav --ayah 1:2 --heads all --frames > take.json
tajweedformer serve  models/sdk/v4.0 < jobs.jsonl            {"audio": "a.wav", "ayah": "1:2"} per line
tajweedformer bench  models/sdk/v4.0 list.txt --workers 8 --threads 1
```

The server tier scales with worker processes (one stream each). Core pinning follows the engine's platform
default (engine d0bb2b3, `ZF_NO_PIN_DEFAULT`): Windows, Linux and macOS do not pin, which is what many streams on
one machine want; `--pin` or `--no-pin` sets it explicitly for `serve`, `bench` and the workers (the measurement
is in the CHANGELOG). In code, `TfModelOptions.no_pin` defaults to -1 (the engine's default); 0 or 1 overrides it.
`--key HEX` trusts another manifest key (tests); `--unsigned` accepts an unsigned folder.
