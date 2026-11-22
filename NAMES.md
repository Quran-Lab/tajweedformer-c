# NAMES: the public surface of the C SDK 4.0.0

Every public type and function of [`include/tajweedformer.h`](include/tajweedformer.h), the one public header. The
library exports exactly these functions (`src/tf/tajweedformer.def`, `src/tf/tajweedformer.map`), and
`test/api_scan.py` checks that the header, the two export lists, this file's source (the header) and
[docs/api.md](docs/api.md) agree. The names are the naming table's (quranlab-contract `sdk_3.0.0/NAMING.md`), the
same concepts as the Android, Apple and web SDKs: Client, Model, Session, Heads, Backend. 4.0 adds
`TfRecitedOutcome`, `TfRecitedAyah` and `tf_session_report_recited_outcome`, and for the ask of section 20
(`SHARE_ASK_40.md`) `TfAskKind` (the sheet or the card), `tf_sharing_shown` and `tf_sharing_open`. 4.0.1 adds `TfClientOptionsExt` and
`tf_client_create2` (`APP_VERSION_40.md` 27.4), and 4.0.2 `TfErrorCause` and `tf_last_error_cause`. Nothing public reads, writes or names a
model file, a key or a path of one.

## Types (30)

| Type | Kind |
| --- | --- |
| `TfError` | enum (the catalogue's 26 errors) |
| `TfErrorCause` | struct |
| `TfBackend` | struct |
| `TfHeads` | opaque handle |
| `TfHead` | struct |
| `TfFrame` | struct |
| `TfToken` | struct |
| `TfWord` | struct |
| `TfDecoded` | struct |
| `TfChunk` | struct |
| `TfEvent` | struct |
| `TfClient` | opaque handle |
| `TfModel` | opaque handle |
| `TfAskKind` | enum |
| `TfConsentRequest` | struct |
| `TfClientOptions` | struct |
| `TfClientOptionsExt` | struct |
| `TfModelOptions` | struct |
| `TfPrefetchOptions` | struct |
| `TfDiagnosis` | struct |
| `TfTokens` | struct |
| `TfCalibration` | struct |
| `TfReference` | opaque handle |
| `TfUnits` | struct |
| `TfClip` | struct |
| `TfSession` | opaque handle |
| `TfSessionOptions` | struct |
| `TfRecitedOutcome` | struct |
| `TfRecitedAyah` | struct |
| `TfSharing` | struct |

## Functions (71)

### Errors

```c
const char *tf_version(void);
int tf_last_error_code(void);
const char *tf_last_error(void);
const char *tf_last_error_fix(void);
int tf_last_error_cause(TfErrorCause *out);
const char *tf_error_name(int error);
const char *tf_error_fix(int error);
const char *tf_error_docs(int error);
```

### Client

```c
TfClient *tf_client_create(const TfClientOptions *options);
TfClient *tf_client_create2(const TfClientOptions *options, const TfClientOptionsExt *ext);
void tf_client_destroy(TfClient *c);
TfModel *tf_client_model(TfClient *c, const TfModelOptions *options);
int tf_client_prefetch(TfClient *c, const TfPrefetchOptions *options);
const char *tf_client_storage_json(TfClient *c);
int tf_client_remove_models(TfClient *c);
int tf_client_diagnose(TfClient *c, TfDiagnosis *out);
const char *tf_client_diagnose_json(TfClient *c);
int tf_client_report_checks(TfClient *c, const char *rule, int checked, int flagged);
int tf_client_background(TfClient *c);
int tf_client_foreground(TfClient *c);
```

### Model

```c
void tf_model_close(TfModel *m);
const char *tf_model_id(const TfModel *m);
const char *tf_model_version(const TfModel *m);
const char *tf_model_build(const TfModel *m);
const char *tf_model_info_json(TfModel *m);
int tf_model_chunk_ms(const TfModel *m, const char *context);
const TfTokens *tf_model_tokens(const TfModel *m);
int tf_model_token_count(const TfModel *m);
const char *tf_model_token_name(const TfModel *m, int id);
int tf_model_token_id(const TfModel *m, const char *name);
const char *tf_model_calibration_json(const TfModel *m);
int tf_model_calibration(const TfModel *m, TfCalibration *out);
int tf_model_calibration_number(const TfModel *m, const char *path, double *out);
TfReference *tf_model_reference(TfModel *m);
int tf_model_sample_clip_count(const TfModel *m);
int tf_model_sample_clip(const TfModel *m, int i, TfClip *out);
int tf_model_backend(const TfModel *m, TfBackend *out);
```

### Reference data

```c
int tf_reference_is_downloaded(const TfReference *r);
int tf_reference_fetch(TfReference *r);
int tf_reference_units(TfReference *r, int surah, int ayah, TfUnits *out);
```

### Session

```c
TfSession *tf_session_create(TfModel *m, const TfSessionOptions *options);
TfSession *tf_session_listen(TfModel *m, const TfClip *clip, const TfSessionOptions *options);
int tf_session_push(TfSession *s, const float *samples, int n);
int tf_session_finish(TfSession *s);
const TfHeads *tf_session_heads(TfSession *s);
int tf_session_cancel(TfSession *s);
int tf_session_backend(const TfSession *s, TfBackend *out);
int tf_session_report_recited(TfSession *s, int surah, int ayah_from, int ayah_to);
int tf_session_report_recited_outcome(TfSession *s, int surah, int ayah_from, int ayah_to, const TfRecitedOutcome *outcome, const TfRecitedAyah *per_ayah, int n_per_ayah);
void tf_session_destroy(TfSession *s);
```

### Heads and the decoded layer

```c
const char *tf_decoded_json(const TfDecoded *decoded, int with_frames);
int tf_heads_count(const TfHeads *h);
const char *tf_heads_name(const TfHeads *h, int i);
int tf_heads_get(const TfHeads *h, const char *name, TfHead *out);
int tf_heads_greedy(const TfHeads *h, const char *name, const TfToken **tokens);
const TfDecoded *tf_heads_decoded(const TfHeads *h);
```

### Sharing

```c
int tf_sharing_status(TfClient *c, TfSharing *out);
int tf_sharing_is_available(TfClient *c);
int tf_sharing_is_on(TfClient *c);
int tf_sharing_set_on(TfClient *c, int on);
int tf_sharing_ask(TfClient *c);
int tf_sharing_shown(TfClient *c);
int tf_sharing_open(TfClient *c);
int tf_sharing_answer(TfClient *c, int share);
const char *tf_sharing_copy_json(TfClient *c);
int tf_sharing_feed(TfClient *c, const float *samples, int n, int sample_rate);
const char *tf_sharing_items_json(TfClient *c);
const char *tf_sharing_summary_json(TfClient *c);
int tf_sharing_delete(TfClient *c);
int tf_sharing_recovery_code(TfClient *c, char out[33]);
int tf_sharing_set_wording(TfClient *c, const char *const *keys, const char *const *texts, int n);
```
