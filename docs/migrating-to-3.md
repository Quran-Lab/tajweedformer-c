# Migrating a 2.x app to SDK 3.0.0 (C)

SDK 3.0.0 is a pure inference engine: audio in, the model's own output out. The tajweed layer of 2.x (verdicts per
site, madd counts, per-word scores, presets and settings) is gone from the SDK. The model, its heads and its bits
are the same; what 2.x computed from them an app now computes itself, with the same inputs: the heads, the
vocabulary, the calibration and the expected units of the ayah, all of which the SDK gives as data.

An app that only showed what the model heard needs the renames below and nothing else. An app that used verdicts
has two choices: build its own judge (the second half of this page), or stay on 2.x for now (the last section).

## The calls

| 2.x | 3.0.0 |
| - | - |
| `tf_client_default_options()`, `tf_model_default_options()`, `tf_stream_default_options()` | gone: a NULL or zeroed options struct is the default |
| `tf_client_create`, then `tf_client_launch`, reading the config for the revision, `tf_download_model`, `tf_model_open` | `tf_client_create`, then `tf_client_model(client, NULL)`: downloads, verifies, opens and warms. `tf_client_prefetch` to download ahead. Since 3.1.0 this is the only way to a Model: `tf_model_open` is gone |
| `TfClientOptions.storage_dir`, `.telemetry`, `.debug_build` | `.storage`, `.analytics` (`TF_ANALYTICS_ON`, `TF_ANALYTICS_OFF`), `.debug` |
| `TfModelOptions.context`, `.sample_rate` | on the Session: `TfSessionOptions.context`, `.sample_rate` |
| `TfStream`, `tf_stream_create`, `tf_stream_push_audio`, `tf_stream_finish`, `tf_stream_destroy` | `TfSession`, `tf_session_create`, `tf_session_push`, `tf_session_finish`, `tf_session_destroy` |
| `tf_stream_reset` for the next take | a Session is one take: destroy it and create the next (one line) |
| `tf_stream_decode(stream)`, `tf_decoded_tokens`, `tf_decoded_free` | `chunk->decoded` in `on_chunk` (the take so far) and `tf_heads_decoded(tf_session_heads(session))` (the whole take): plain structs, nothing to free |
| `tf_stream_heads`, head masks (`TF_HEAD_GEM`, ...) | `tf_session_heads`, `tf_heads_get(heads, "gem", &head)`; `TfSessionOptions.heads` is a list of names or `"all"` |
| `tf_stream_topk`, `TfTopK` | `TfSessionOptions.top_k`, then `decoded->ranked_ids`, `ranked_log_probs`, `ranked_margins` |
| `tf_stream_score`, `tf_score`, `TfResult`, `TfWordScore`, `tf_settings_set_app_json`, `tf_settings_set_remote_json`, `tf_settings_json` | gone: see "Your own judge" |
| `tf_client_config_json`, `tf_client_apply_config`, `tf_client_session_finished`, `tf_client_flush`, `tf_client_report_issue` | gone from the public API: the client does them itself |
| `tf_client_sharing_set`, `tf_client_sharing_answer`, `tf_client_sharing_delete`, `tf_sharing_recording_close`, `tf_sharing_outbox_json` | `tf_sharing_set_on`, `tf_sharing_answer`, `tf_sharing_delete`, `tf_client_background`, `tf_sharing_items_json` |
| `tf_sharing_session_begin`, `tf_sharing_session_push`, `tf_sharing_session_finish` | gone: a take of a Session whose Model came from `tf_client_model` is shared by the SDK when the learner said yes |
| error codes `TF_ERR_SETTINGS`, `TF_ERR_CALIBRATION`, `TF_ERR_NETWORK`, ... | the 24 codes of the shared catalogue (`TfError`), each with a message, a fix and a docs link |
| `tajweedformer decode --ayah S:A`, `--settings`, `tajweedformer portal` | gone; `tajweedformer diagnose` is new |

New in 3.0.0 and worth using: `end_on_silence_ms` (a take ends by itself after the learner stops), events
(`speechStarted`, `speechEnded`, `ended`), `settled` and `is_final` on the decoded layer, `tf_session_listen` on a
sample clip (the whole path without a microphone), `tf_client_diagnose_json`, model versions
(`tf_model_version`, and `TfModelOptions.version` to pin one), and one Client per process.

The signed config's tajweed block no longer reaches the SDK. Its Response time setting does: it picks the context
(`inference.context`) a Session uses when the app names none.

## Your own judge

What 2.x did for a known ayah, and where each input is now.

1. **The expected units of the ayah.** In 2.x the SDK read them from the model folder. In 3.0.0 they are the
   reference data, read-only: `tf_model_reference(model)`, `tf_reference_fetch(reference)` once, then
   `tf_reference_units(reference, surah, ayah, &units)` gives the token ids and the word of each unit. The ids are
   the model's vocabulary (`tf_model_tokens`). The table is built with quran-g2p, Quran Lab's public tool; nothing
   has to be run by the app.

2. **Alignment and per-site evidence.** For each site of the expected sequence, the 2.x scorer compared two
   probabilities on the take's heads: the full-sequence CTC forward of the expected sequence, and the same with
   the best single-unit alternative at that site (a substitution, a deletion or an insertion, by class). The
   evidence is the log-likelihood ratio `log P(alternative) - log P(expected)`, computed on `log_probs` and on
   `inter1_log_probs` (prepared as the calibration's `contexts.<context>.fusion.inter1_prep` says: the word bar
   merged into blank), fused as `a * llr(log_probs) + (1 - a) * llr(inter1_log_probs)` with the class's weight
   `fusion.a.<class>`, and clipped to `fusion.clip`. A site is flagged when the fused value is above
   `contexts.<context>.sites.<class>.thresholds.<preset>`. Classes whose `status` is `experimental` or `off` were
   not flagged by default.

3. **Madd, qalqalah, gemination, words.** Madd counts come from `timing_log_probs` (20 ms frames): the held span
   of a long vowel against the median span of the take's short vowels, cut by `madd.cuts` (and not counted with
   fewer than `madd.min_short_vowels` short vowels). Qalqalah: the mean of `attr_qalqalah` classes 1 and 2 over
   the 12 frames after the letter's end, against `qalqalah.tau`. Gemination: the mean of `gem` class 1 over the
   letter's span, against `gemination.tau`. The per-word score uses `word` (its window, alternatives and `tau` per
   word preset).

4. **Presets and overrides.** `strict`, `balanced`, `sensitive` and `very_sensitive` are columns of the
   calibration (`presets` gives each one's target false-alarm rate). Which one applies, per-class overrides and
   how learners choose are now the app's own settings.

Read the numbers with `tf_model_calibration_number(model, "contexts.c16_l64.sites.vowel_sub.thresholds.balanced",
&x)` or parse `tf_model_calibration_json(model)`. Use the context the take ran in. The calibration belongs to one
vocabulary: its `tokens_sha256` is the model's.

Every head is on the take's Heads: `tf_heads_get(tf_session_heads(session), "inter1_log_probs", &head)`. Select
the heads the judge needs in `TfSessionOptions.heads` (the default selection has `log_probs`, `inter1_log_probs`,
`timing_log_probs`, `gem`, `attr_qalqalah` and `conf`).

## Feeding analytics

In 2.x the SDK counted ayahs and verdicts because it produced them. In 3.0.0 the app reports them, in two lines
after a take; counts only, and nothing is sent when the app never calls them:

```c
tf_session_report_recited(session, 1, 2, 2);            /* surah 1, ayah 2 to 2 */
tf_client_report_checks(client, "vowel_sub", 31, 1);    /* 31 sites checked by the app's judge, 1 flagged */
```

## Staying on 2.x

SDK 2.0.2 keeps working until Quran Lab retires SDK 2.x, which happens when 3.0 is released or shortly after;
there is no long overlap, so plan the move to 3.0 now.

A 3.0.0 SDK does not open a 2.x build (`model_incompatible`) and a 2.x SDK does not open a 3.0 build.
