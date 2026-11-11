/* Reference data (SPEC 13.7): the expected units of each ayah, as DATA. An optional file of the build (manifest
 * role `reference`), signed and verified, never part of the CPU set, downloaded only when the app asks. The
 * accessor is a lookup: surah and ayah in, the unit ids and their words out. The SDK never aligns them, never
 * compares them with a take and never reads them itself; the engine runs without the file. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

/* 0 when the file is in the folder with the size and SHA-256 the manifest lists */
static int on_disk(const TfReference *r) {
  const TfJson *e = tf__model_listed(r->m, r->path);
  char *full = e ? tf_path_join(tf__model_dir(r->m), r->path) : NULL;
  uint8_t h[32], want[32];
  long long size = 0;
  const int ok = full && !tf_sha256_file(full, h, &size) && !tf_hex_decode(tf_json_str(tf_json_get(e, "sha256")), want, 32) &&
                 size == (long long)tf_json_get(e, "size")->num && !memcmp(h, want, 32);
  free(full);
  return ok ? 0 : -1;
}

int tf_reference_is_downloaded(const TfReference *r) { return r && (r->table || on_disk(r) == 0); }

int tf_reference_fetch(TfReference *r) {
  tf__clear_error();
  if (!r) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no reference data");
  if (tf_reference_is_downloaded(r)) return 0;
  TfClient *c = tf__model_client(r->m);
  if (!c) return tf__fail(TF_ERR_REFERENCE_MISSING, NULL); /* a folder opened by hand: nothing to download from */
  const int rc = tf__store_fetch_role(c, r->m, NULL, "reference");
  if (rc || on_disk(r)) {
    tf__clear_error();
    return tf__fail(TF_ERR_REFERENCE_MISSING, NULL);
  }
  return 0;
}

void tf__reference_release(TfReference *r) {
  if (r->table) zf_tajweed_reference_close((ZfTajweedReference *)r->table);
  r->table = NULL;
  free(r->ids);
  free(r->word);
  r->ids = r->word = NULL;
  r->cap = 0;
}

int tf_reference_units(TfReference *r, int surah, int ayah, TfUnits *out) {
  tf__clear_error();
  if (!r || !out) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no reference data");
  memset(out, 0, sizeof *out);
  if (!r->table) {
    if (on_disk(r)) return tf__fail(TF_ERR_REFERENCE_MISSING, NULL);
    char *full = tf_path_join(tf__model_dir(r->m), r->path);
    const char *err = NULL;
    r->table = full ? zf_tajweed_reference_open(full, &err) : NULL;
    free(full);
    if (!r->table) return tf__fail(TF_ERR_MODEL_INVALID, "the reference data cannot be read (%s)", err ? err : "?");
  }
  const int ayahs = zf_tajweed_reference_ayah_count((ZfTajweedReference *)r->table, surah);
  if (surah < 1 || surah > 114 || ayahs <= 0) return tf__fail(TF_ERR_INVALID_ARGUMENT, "surah %d is not 1 to 114", surah);
  if (ayah < 1 || ayah > ayahs) return tf__fail(TF_ERR_INVALID_ARGUMENT, "ayah %d is not inside surah %d (1 to %d)", ayah, surah, ayahs);
  const int *ids = NULL, *word = NULL;
  int n_words = 0;
  const int n = zf_tajweed_reference_units((ZfTajweedReference *)r->table, surah, ayah, &ids, &word, &n_words);
  if (n < 0) return tf__fail(TF_ERR_INVALID_ARGUMENT, "the reference data has no ayah %d:%d", surah, ayah);
  out->n = n;
  out->ids = ids;
  out->word = word;
  out->n_words = n_words;
  return 0;
}
