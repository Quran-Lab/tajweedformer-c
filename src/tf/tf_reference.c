/* Reference data (SPEC 13.7): the expected units of each ayah, as DATA. An optional file of the build (manifest role
 * `reference`), downloaded only when the app asks. The accessor is a lookup: surah and ayah in, the unit ids and
 * their words out. The SDK never aligns them, never compares them with a take and never reads them itself.
 *
 * SDK 4.0: the file is encrypted like the weights. tf_reference_fetch downloads its CDN form and the Core installs it.
 * The Core installs beside an open model, except a file that model reads now or later (qlcore.h qlc_install, spec fix
 * 16): the reference data of the open model's own build is one, so the Model's engine is closed for the fetch (refused
 * while a take runs) and opened again at the next read, which is when the Core sees the file. tf_reference_units asks the Core (qlc_reference_expected, R3), which reads the table at
 * its first ask with the lease (as SDK 3.x read it on first use) from the open engine; that first ask can need a
 * connection (no valid lease) or find the file refused (a changed byte), and a later ask tries again. The units and
 * the word of each unit are 3.x's (a word bar belongs to the word before it), kept per ayah until the Model closes. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

#define QURAN_AYAHS 6236

static const TfJson *entry(const TfReference *r) { return r ? tf__model_role_entry(r->m, "reference") : NULL; }

int tf_reference_is_downloaded(const TfReference *r) { return r && tf__model_entry_installed(r->m, entry(r)); }

int tf_reference_fetch(TfReference *r) {
  tf__clear_error();
  if (!r) return tf__raise(tf__fail(TF_ERR_INVALID_ARGUMENT, "no reference data"));
  if (tf_reference_is_downloaded(r)) return 0;
  TfClient *c = tf__model_client_hold(r->m);
  if (!c) return tf__raise(tf__fail(TF_ERR_REFERENCE_MISSING, NULL)); /* a folder opened by hand: nothing to download from */
  int rc = tf__model_engine_release(r->m);
  if (rc) {
    tf__client_unhold(c);
    return tf__raise(rc);
  }
  tf__app_call++; /* its failure reaches the app (24.1) */
  rc = tf__store_fetch_role(c, r->m, NULL, "reference");
  tf__app_call--;
  tf__client_unhold(c);
  if (rc || !tf_reference_is_downloaded(r)) {
    /* the app is told the reference data is missing; the report names why (the download's cause, when one failed) */
    if (rc) tf__cause_force();
    else tf__clear_error();
    return tf__raise(tf__fail(TF_ERR_REFERENCE_MISSING, NULL));
  }
  return 0;
}

void tf__reference_release(TfReference *r) {
  for (int i = 0; r->ids && i < QURAN_AYAHS; ++i) {
    free(r->ids[i]);
    free(r->word[i]);
  }
  free(r->ids);
  free(r->word);
  free(r->n);
  free(r->n_words);
  r->ids = r->word = NULL;
  r->n = r->n_words = NULL;
}

/* the Core's answer for one ayah into the cache: 0, or an error */
static int read_ayah_claimed(TfReference *r, int surah, int ayah, int at);
static int read_ayah(TfReference *r, int surah, int ayah, int at) {
  /* 4.0.3: the engine stays this Model's while it is read (no other Model of the folder takes the Core's open model) */
  tf__model_engine_claim(r->m);
  const int rc = read_ayah_claimed(r, surah, ayah, at);
  tf__model_engine_unclaim(r->m);
  return rc;
}
static int read_ayah_claimed(TfReference *r, int surah, int ayah, int at) {
  qlc_model *zm = tf__model_engine_for_reference(r->m);
  if (!zm) return tf__error_code() ? tf__error_code() : tf__fail(TF_ERR_REFERENCE_MISSING, NULL);
  TfPm *pm = tf__model_pm(r->m);
  int cap = 2048, n = 0, nw = 0;
  int *u = NULL, *starts = NULL;
  for (int pass = 0; pass < 2; ++pass) {
    free(u);
    free(starts);
    u = (int *)malloc(sizeof(int) * (size_t)cap);
    starts = (int *)malloc(sizeof(int) * (size_t)cap);
    if (!u || !starts) {
      free(u);
      free(starts);
      return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
    }
    /* the first read decrypts the model's own device form: the platform's folder is this build's for the call (a
     * build installed beside the open model may have set another) */
    tf__pm_lock(pm);
    tf__plat_set_build(tf__pm_plat(pm), tf__model_dir(r->m));
    n = qlc_reference_expected(zm, surah, ayah, u, cap, starts, cap, &nw);
    tf__pm_unlock(pm);
    if (n < 0 || (n <= cap && nw <= cap)) break;
    cap = (n > nw ? n : nw) + 1; /* more units than the first guess: once more with room for all */
  }
  if (n < 0) {
    free(u);
    free(starts);
    if (n == QLC_E_ARG) return tf__fail(TF_ERR_INVALID_ARGUMENT, "the reference data has no ayah %d:%d", surah, ayah);
    if (n == QLC_E_FILE && !tf_reference_is_downloaded(r)) return tf__fail(TF_ERR_REFERENCE_MISSING, NULL);
    /* the first read needs a valid lease (QLC_E_NEEDS_CONNECTION, QLC_E_LEASE) and the key store (QLC_E_KEYSTORE);
     * a refused file is QLC_E_FILE here, not at the open. The next ask tries again. */
    tf__pm_lock(pm);
    const int detail = qlc_detail(tf__pm_core(pm));
    tf__pm_unlock(pm);
    const int here = tf__cause_files_here(1); /* 24.13 item 3: the reference data is here; the lease is what is missing */
    const int rc = tf__pm_error(n, detail);
    tf__cause_files_here(here);
    return rc;
  }
  int *word = (int *)malloc(sizeof(int) * (size_t)(n ? n : 1));
  if (!word) {
    free(u);
    free(starts);
    return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  }
  /* the word of each unit from the words' first units (a word bar belongs to the word before it, as 3.x gave it) */
  for (int k = 0, w = 0; k < n; ++k) {
    while (w + 1 < nw && starts[w + 1] <= k) ++w;
    word[k] = w;
  }
  free(starts);
  r->ids[at] = u;
  r->word[at] = word;
  r->n[at] = n;
  r->n_words[at] = nw;
  return 0;
}

int tf_reference_units(TfReference *r, int surah, int ayah, TfUnits *out) {
  tf__clear_error();
  if (!r || !out) return tf__raise(tf__fail(TF_ERR_INVALID_ARGUMENT, "no reference data"));
  memset(out, 0, sizeof *out);
  const int ayahs = tf__ayah_count(surah);
  if (!ayahs) return tf__raise(tf__fail(TF_ERR_INVALID_ARGUMENT, "surah %d is not 1 to 114", surah));
  if (ayah < 1 || ayah > ayahs) return tf__raise(tf__fail(TF_ERR_INVALID_ARGUMENT, "ayah %d is not inside surah %d (1 to %d)", ayah, surah, ayahs));
  if (!tf_reference_is_downloaded(r)) return tf__raise(tf__fail(TF_ERR_REFERENCE_MISSING, NULL));
  if (!r->ids) {
    r->ids = (int **)calloc(QURAN_AYAHS, sizeof *r->ids);
    r->word = (int **)calloc(QURAN_AYAHS, sizeof *r->word);
    r->n = (int *)calloc(QURAN_AYAHS, sizeof *r->n);
    r->n_words = (int *)calloc(QURAN_AYAHS, sizeof *r->n_words);
    if (!r->ids || !r->word || !r->n || !r->n_words) {
      tf__reference_release(r);
      return tf__raise(tf__fail(TF_ERR_ENGINE_FAILED, "out of memory"));
    }
  }
  int at = ayah - 1;
  for (int s = 1; s < surah; ++s) at += tf__ayah_count(s);
  if (!r->ids[at]) {
    const int rc = read_ayah(r, surah, ayah, at);
    if (rc) return tf__raise(rc);
  }
  out->n = r->n[at];
  out->ids = r->ids[at];
  out->word = r->word[at];
  out->n_words = r->n_words[at];
  return 0;
}
