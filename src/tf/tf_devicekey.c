/* "Delete what I shared" without app credentials (PLATFORM_CLIENT.md 9.1, the 2.0.0 design): a per-contributor
 * P-256 device key, the recovery code, the signed deletion message, and the deletion and summary calls.
 *
 * Key storage: Windows, the private key blob protected with DPAPI (CryptProtectData, current user) in the
 * client's storage directory; Linux, a PEM file with mode 0600 in the storage directory (libsecret is not used
 * in this build). There is no call that exports the private key or deletes another contributor. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#else
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <sys/stat.h>
#endif

#include "tf_internal.h"

/* ------------------------------------------------------------------ recovery code */
static const char kCrock[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* 16 bytes -> 26 Crockford base32 characters, most significant bit first, in groups of 4 joined by "-" */
void tf_recovery_code(const uint8_t b[16], char out[33]) {
  char raw[27];
  for (int i = 0; i < 26; ++i) {
    int v = 0;
    for (int k = 0; k < 5; ++k) {
      const int bit = i * 5 + k;
      v = v << 1 | (bit < 128 ? (b[bit / 8] >> (7 - bit % 8)) & 1 : 0);
    }
    raw[i] = kCrock[v];
  }
  int o = 0;
  for (int i = 0; i < 26; ++i) {
    if (i && i % 4 == 0) out[o++] = '-';
    out[o++] = raw[i];
  }
  out[o] = 0;
}
/* upper case, spaces and "-" removed, O as 0, I and L as 1; 0 and the 26 characters, or -1 when not a code */
int tf_recovery_normalise(const char *typed, char out[27]) {
  int n = 0;
  for (const char *p = typed; *p; ++p) {
    char c = *p;
    if (c == ' ' || c == '-') continue;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    if (!strchr(kCrock, c) || n >= 26) return -1;
    out[n++] = c;
  }
  out[n] = 0;
  return n == 26 ? 0 : -1;
}
void tf_recovery_hash(const char *normalised, char out[65]) {
  uint8_t h[32];
  tf_sha256(normalised, strlen(normalised), h);
  tf_hex_encode(h, 32, out);
}
/* the exact bytes the device key signs */
char *tf_deletion_message(const char *app, const char *contributor, const char *nonce, long long ts) {
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "quranlab-contribution-delete 1\napp %s\ncontributor %s\nnonce %s\ntimestamp %lld\naction delete-all\n", app, contributor,
                nonce, ts);
  return tf_buf_take(&b);
}
static void b64url(const uint8_t *in, size_t n, char *out) {
  static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    out[o++] = a[v >> 18 & 63];
    out[o++] = a[v >> 12 & 63];
    if (i + 1 < n) out[o++] = a[v >> 6 & 63];
    if (i + 2 < n) out[o++] = a[v & 63];
  }
  out[o] = 0;
}
void tf_b64url(const uint8_t *in, size_t n, char *out) { b64url(in, n, out); }

/* ------------------------------------------------------------------ P-256 */
/* d (32 bytes, may be NULL to generate), the public point (65 bytes 04||X||Y), r||s signatures (64 bytes) */
#ifdef _WIN32
static BCRYPT_ALG_HANDLE alg(void) {
  static BCRYPT_ALG_HANDLE h;
  if (!h && BCryptOpenAlgorithmProvider(&h, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) != 0) h = NULL;
  return h;
}
/* private blob: BCRYPT_ECCKEY_BLOB + X + Y + d */
int tf_p256_generate(uint8_t priv[96], uint8_t pub[65]) {
  BCRYPT_KEY_HANDLE k = NULL;
  if (!alg() || BCryptGenerateKeyPair(alg(), &k, 256, 0) || BCryptFinalizeKeyPair(k, 0)) return -1;
  uint8_t blob[8 + 96];
  ULONG got = 0;
  const NTSTATUS st = BCryptExportKey(k, NULL, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof blob, &got, 0);
  BCryptDestroyKey(k);
  if (st || got != sizeof blob) return -1;
  memcpy(priv, blob + 8, 96);
  pub[0] = 4;
  memcpy(pub + 1, blob + 8, 64);
  return 0;
}
int tf_p256_sign(const uint8_t priv[96], const void *msg, size_t n, uint8_t sig[64]) {
  uint8_t blob[8 + 96];
  BCRYPT_ECCKEY_BLOB *h = (BCRYPT_ECCKEY_BLOB *)blob;
  h->dwMagic = BCRYPT_ECDSA_PRIVATE_P256_MAGIC;
  h->cbKey = 32;
  memcpy(blob + 8, priv, 96);
  BCRYPT_KEY_HANDLE k = NULL;
  if (!alg() || BCryptImportKeyPair(alg(), NULL, BCRYPT_ECCPRIVATE_BLOB, &k, blob, sizeof blob, 0)) return -1;
  uint8_t dg[32];
  tf_sha256(msg, n, dg);
  ULONG got = 0;
  const NTSTATUS st = BCryptSignHash(k, NULL, dg, 32, sig, 64, &got, 0);
  BCryptDestroyKey(k);
  SecureZeroMemory(blob, sizeof blob);
  return st || got != 64 ? -1 : 0;
}
int tf_p256_verify(const uint8_t pub[65], const void *msg, size_t n, const uint8_t sig[64]) {
  uint8_t blob[8 + 64];
  BCRYPT_ECCKEY_BLOB *h = (BCRYPT_ECCKEY_BLOB *)blob;
  h->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
  h->cbKey = 32;
  memcpy(blob + 8, pub + 1, 64);
  BCRYPT_KEY_HANDLE k = NULL;
  if (pub[0] != 4 || !alg() || BCryptImportKeyPair(alg(), NULL, BCRYPT_ECCPUBLIC_BLOB, &k, blob, sizeof blob, 0)) return 0;
  uint8_t dg[32];
  tf_sha256(msg, n, dg);
  const NTSTATUS st = BCryptVerifySignature(k, NULL, dg, 32, (PUCHAR)sig, 64, 0);
  BCryptDestroyKey(k);
  return st == 0;
}
static int protect(const uint8_t *in, size_t n, uint8_t **out, size_t *on, int unprotect) {
  DATA_BLOB a = {(DWORD)n, (BYTE *)in}, b = {0, NULL};
  const BOOL ok = unprotect ? CryptUnprotectData(&a, NULL, NULL, NULL, NULL, 0, &b) : CryptProtectData(&a, L"QuranLab contributor key", NULL, NULL, NULL, 0, &b);
  if (!ok) return -1;
  *out = (uint8_t *)malloc(b.cbData);
  if (*out) memcpy(*out, b.pbData, b.cbData);
  *on = b.cbData;
  LocalFree(b.pbData);
  return *out ? 0 : -1;
}
#else
/* priv[96] = X || Y || d, as the Windows blob, so the stored format is one shape */
static EVP_PKEY *key_from(const uint8_t priv[96], int with_private) {
  uint8_t pub[65];
  pub[0] = 4;
  memcpy(pub + 1, priv, 64);
  OSSL_PARAM_BLD *b = OSSL_PARAM_BLD_new();
  BIGNUM *d = with_private ? BN_bin2bn(priv + 64, 32, NULL) : NULL;
  OSSL_PARAM_BLD_push_utf8_string(b, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
  OSSL_PARAM_BLD_push_octet_string(b, OSSL_PKEY_PARAM_PUB_KEY, pub, 65);
  if (d) OSSL_PARAM_BLD_push_BN(b, OSSL_PKEY_PARAM_PRIV_KEY, d);
  OSSL_PARAM *ps = OSSL_PARAM_BLD_to_param(b);
  EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
  EVP_PKEY *k = NULL;
  if (c && EVP_PKEY_fromdata_init(c) > 0) EVP_PKEY_fromdata(c, &k, with_private ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, ps);
  EVP_PKEY_CTX_free(c);
  OSSL_PARAM_free(ps);
  OSSL_PARAM_BLD_free(b);
  BN_clear_free(d);
  return k;
}
int tf_p256_generate(uint8_t priv[96], uint8_t pub[65]) {
  EVP_PKEY *k = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  if (!k) return -1;
  size_t n = 0;
  uint8_t pt[65];
  BIGNUM *d = NULL;
  int ok = EVP_PKEY_get_octet_string_param(k, OSSL_PKEY_PARAM_PUB_KEY, pt, sizeof pt, &n) && n == 65 &&
           EVP_PKEY_get_bn_param(k, OSSL_PKEY_PARAM_PRIV_KEY, &d) && BN_bn2binpad(d, priv + 64, 32) == 32;
  if (ok) {
    memcpy(priv, pt + 1, 64);
    memcpy(pub, pt, 65);
  }
  BN_clear_free(d);
  EVP_PKEY_free(k);
  return ok ? 0 : -1;
}
int tf_p256_sign(const uint8_t priv[96], const void *msg, size_t n, uint8_t sig[64]) {
  EVP_PKEY *k = key_from(priv, 1);
  EVP_MD_CTX *c = EVP_MD_CTX_new();
  uint8_t der[80];
  size_t dn = sizeof der;
  int ok = k && c && EVP_DigestSignInit(c, NULL, EVP_sha256(), NULL, k) > 0 && EVP_DigestSign(c, der, &dn, msg, n) > 0;
  if (ok) {
    const uint8_t *p = der;
    ECDSA_SIG *s = d2i_ECDSA_SIG(NULL, &p, (long)dn);
    ok = s && BN_bn2binpad(ECDSA_SIG_get0_r(s), sig, 32) == 32 && BN_bn2binpad(ECDSA_SIG_get0_s(s), sig + 32, 32) == 32;
    ECDSA_SIG_free(s);
  }
  EVP_MD_CTX_free(c);
  EVP_PKEY_free(k);
  return ok ? 0 : -1;
}
int tf_p256_verify(const uint8_t pub[65], const void *msg, size_t n, const uint8_t sig[64]) {
  uint8_t tmp[96] = {0};
  if (pub[0] != 4) return 0;
  memcpy(tmp, pub + 1, 64);
  EVP_PKEY *k = key_from(tmp, 0);
  ECDSA_SIG *s = ECDSA_SIG_new();
  BIGNUM *r = BN_bin2bn(sig, 32, NULL), *ss = BN_bin2bn(sig + 32, 32, NULL);
  ECDSA_SIG_set0(s, r, ss);
  uint8_t *der = NULL;
  const int dn = i2d_ECDSA_SIG(s, &der);
  EVP_MD_CTX *c = EVP_MD_CTX_new();
  const int ok = k && c && dn > 0 && EVP_DigestVerifyInit(c, NULL, EVP_sha256(), NULL, k) > 0 && EVP_DigestVerify(c, der, (size_t)dn, msg, n) == 1;
  OPENSSL_free(der);
  ECDSA_SIG_free(s);
  EVP_MD_CTX_free(c);
  EVP_PKEY_free(k);
  return ok;
}
#endif

/* ------------------------------------------------------------------ the contributor: id, key, recovery code */
typedef struct {
  char id[33];
  uint8_t priv[96];
  uint8_t pub[65];
  char recovery[33];
} Contributor;

static char *cpath(const char *dir) { return tf_path_join(dir, "contributor.bin"); }

/* stored: 32 hex id, 96 key bytes, 32 recovery characters; protected (Windows DPAPI) or 0600 (Linux) */
static int contributor_save(const char *dir, const Contributor *c) {
  uint8_t plain[33 + 96 + 33];
  memcpy(plain, c->id, 33);
  memcpy(plain + 33, c->priv, 96);
  memcpy(plain + 129, c->recovery, 33);
  char *p = cpath(dir);
  int rc = -1;
#ifdef _WIN32
  uint8_t *enc = NULL;
  size_t en = 0;
  if (p && !protect(plain, sizeof plain, &enc, &en, 0)) rc = tf_write_file(p, enc, en);
  free(enc);
  SecureZeroMemory(plain, sizeof plain);
#else
  if (p) {
    rc = tf_write_file(p, plain, sizeof plain);
    if (!rc) chmod(p, 0600);
  }
  memset(plain, 0, sizeof plain);
#endif
  free(p);
  return rc;
}
static int contributor_load(const char *dir, Contributor *c) {
  char *p = cpath(dir), *raw = NULL;
  size_t n = 0;
  int rc = -1;
  if (p && !tf_read_file(p, &raw, &n)) {
#ifdef _WIN32
    uint8_t *plain = NULL;
    size_t pn = 0;
    if (!protect((const uint8_t *)raw, n, &plain, &pn, 1) && pn == 162) {
      memcpy(c->id, plain, 33);
      memcpy(c->priv, plain + 33, 96);
      memcpy(c->recovery, plain + 129, 33);
      rc = 0;
    }
    if (plain) SecureZeroMemory(plain, pn);
    free(plain);
#else
    if (n == 162) {
      memcpy(c->id, raw, 33);
      memcpy(c->priv, raw + 33, 96);
      memcpy(c->recovery, raw + 129, 33);
      rc = 0;
    }
#endif
  }
  if (!rc) {
    c->pub[0] = 4;
    memcpy(c->pub + 1, c->priv, 64);
  }
  free(raw);
  free(p);
  return rc;
}
/* the contributor of this install, made at the first shared session */
int tf__contributor(const char *dir, int create, char id[33], char contributor_key[90], char recovery_hash[65]) {
  Contributor c;
  memset(&c, 0, sizeof c);
  if (contributor_load(dir, &c)) {
    if (!create) return -1;
    uint8_t r[32];
    if (tf_random_bytes(r, sizeof r) || tf_p256_generate(c.priv, c.pub)) return -1;
    tf_hex_encode(r, 16, c.id);
    tf_recovery_code(r + 16, c.recovery);
    if (contributor_save(dir, &c)) return -1;
  }
  if (id) memcpy(id, c.id, 33);
  if (contributor_key) b64url(c.pub, 65, contributor_key);
  if (recovery_hash) {
    char norm[27];
    tf_recovery_normalise(c.recovery, norm);
    tf_recovery_hash(norm, recovery_hash);
  }
  memset(&c, 0, sizeof c);
  return 0;
}
/* "Show my recovery code": for the learner's eyes only (the app shows it behind its own confirmation) */
int tf__contributor_recovery(const char *dir, char out[33]) {
  Contributor c;
  if (contributor_load(dir, &c)) return -1;
  memcpy(out, c.recovery, 33);
  memset(&c, 0, sizeof c);
  return 0;
}
int tf__contributor_sign(const char *dir, const char *app, const char *nonce, long long ts, char sig_b64url[90]) {
  Contributor c;
  if (contributor_load(dir, &c)) return -1;
  char *m = tf_deletion_message(app, c.id, nonce, ts);
  uint8_t sig[64];
  const int rc = m && !tf_p256_sign(c.priv, m, strlen(m), sig) ? 0 : -1;
  if (!rc) b64url(sig, 64, sig_b64url);
  free(m);
  memset(&c, 0, sizeof c);
  return rc;
}
void tf__contributor_forget(const char *dir) {
  char *p = cpath(dir);
  if (p) remove(p);
  free(p);
}
