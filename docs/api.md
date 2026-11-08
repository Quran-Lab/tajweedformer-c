# The C API: `tajweedformer.h` (SDK 2.0.0)

One library for Windows and Linux apps and for servers: `tajweedformer.dll` / `libtajweedformer.so` (or the static
library), with `include/tajweedformer.h` and the engine's `zf_tajweed.h`. It runs the V4 TajweedFormer model only
(SDK 2.0.0 hosts V4; v3.2 stays on the 1.x SDKs). The concepts and numbers are the same as in the Android, Apple
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
engine format 2), family `tajweedformer`, `min_sdk_version` at most 2.0.0, an Ed25519 signature by Quran Lab's key (key id
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
TfClientOptions co = tf_client_default_options();   /* telemetry on (anonymous daily aggregates) */
co.key = "qlk_v1_...";                               /* a desktop key from the portal */
co.app_id = "com.example.Hifz";                      /* Linux reverse-DNS id, or the Windows package family name */
co.app_version = "1.2.0";
co.storage_dir = "/home/me/.local/share/hifz/quranlab";
TfClient *c = tf_client_create(&co);
tf_client_launch(c);                                  /* token, register, verified config, due uploads */
tf_client_apply_config(c, m);                         /* the portal's tajweed block into the model's settings */
/* after each session */
TfSessionStats st = {sizeof st}; st.model = m; st.decoded = d; st.result = r; st.audio_s = 6.3; st.compute_s = 1.1;
st.chunks = 20; st.feedback_shown = 1;
tf_client_session_finished(c, &st);
```

The config is verified before use: the key id must be pinned, the Ed25519 signature must cover the exact body
bytes, `v` must be 1 and `expires_at` in the future; otherwise the last good config stays. Refresh with
`tf_client_refresh_config` (ETag, 304) at launch and every 60 s while in use. Telemetry is the v2 body of
PLATFORM_CLIENT.md (no audio, no text, no identifiers; sums and histograms per UTC day); the signed config's
`flags.analytics: false` stops it and drops the queue; `co.telemetry = ZF_ANALYTICS_OFF` is the developer's opt-out.
Issues carry a kind and a code, never free text, one per signature per day.

## Recitation sharing

One flag: `co.contributions = 1` (or `zf_platform_set_contributions(1)` before `tf_client_create`), plus
`co.children = 1` for a children's app. The SDK decides when to ask (after a finished session with feedback, never
on first launch; "Not now" re-asks at most twice, at least 7 sessions apart), and calls `co.on_consent` with the
copy deck for `co.language` (`tf_sharing_copy_json`: en, ar, id, ur, tr, ms, fr, bn); the app shows the sheet and
answers with `tf_client_sharing_answer`. Settings: `tf_client_sharing_set`, `tf_client_sharing_status_json`,
`tf_client_sharing_delete`.

"Delete what I shared" needs no app credential and cannot be done by the app developer: at the first shared
session the SDK makes a contributor id, a P-256 device key (Windows: the private key protected with DPAPI for the
current user; Linux: a 0600 file in the storage directory) and a recovery code. `tf_client_sharing_delete` asks
for a challenge, signs `quranlab-contribution-delete 1` with the device key and posts it, with no key or token;
`tf_client_sharing_recovery_code` shows the code (for portal.quranlab.ai/delete when the device is lost);
`tf_client_sharing_summary_json` gives the learner's shared minutes. No call exports the key.

### Sharing whole sessions (PLATFORM_CLIENT.md section 10)

With the learner's consent on, every session the SDK listens to is shared in full, in the background:

```c
TfStreamOptions so = tf_stream_default_options();
so.client = c;                 /* each take of this stream is shared while consent is on; nothing otherwise */
so.input_route = "usb";        /* built_in, wired, usb, bluetooth, car, continuity, other, unknown */
TfStream *s = tf_stream_create(m, &so);
/* push, finish as usual: the take's audio is resampled to 48 kHz, encoded (libopus 1.6.1: AUDIO, fullband,
 * VBR, complexity 10, 20 ms, TF_SHARING_BITRATE_DEFAULT 192000 bit/s) and muxed as one Ogg Opus stream */
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
aborts the PUT in flight and deletes the queue at once. Errors follow the spec's table: `contributor_revoked` /
`contributor_key_mismatch` make a new contributor, key and recovery code; `segment_cap` ends the session's
upload; `volume_limit` / `capped_limit` wait until the next UTC day; 5xx, 429 and network errors back off.
`tf_sharing_set_bitrate` changes the bitrate for later sessions (one constant, `TF_SHARING_BITRATE_DEFAULT`,
pending Quran Lab's decision); `tf_sharing_outbox_pending` counts what is queued.

Trust: desktop uploads at the lower `capped` tier (an unattested token; the server caps per key and per
contributor and marks the sessions attested=false). `flags.upload_trust[windows|linux]` absent or `capped` =
upload; `off` or `attested` = nothing is uploaded (`tf_client_sharing_status_json` says `mayUpload`).

## Downloads (the native profile)

```c
TfDownloadOptions d = tf_download_default_options();   /* cdn.quranlab.ai, Quran-Lab/tajweedformer-v4, sdk/v4.0 */
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
