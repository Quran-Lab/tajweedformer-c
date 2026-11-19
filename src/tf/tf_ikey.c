/* The install key store (PROTECTED_MODEL.md 18.4.2): an agreement key that receives leases and a signing key that
 * proves the install holds it. What crosses into the Core is one lease's ECDH secret and signatures, never a
 * private key; nothing here exports a private key, and no public call reaches this file.
 *
 *   Windows with a TPM   CNG, Microsoft Platform Crypto Provider: an ECDH_P256 key and an ECDSA_P256 key, persisted in
 *                        the provider under this user, never exportable (a TPM key cannot be). install_hw 1.
 *   Windows without      CNG, Microsoft Software Key Storage Provider, export policy 0 (not exportable): the private
 *                        keys rest in the user's DPAPI-protected key files. install_hw 0, signal 16 (software key
 *                        store). Chosen when the Platform Crypto Provider cannot be opened or cannot make a P-256 key.
 *   Linux                one P-256 key for both (18.4.2: "same"), in <storage>/install/key.bin, mode 0600, its scalar
 *                        sealed with AES-256-GCM under a key derived from the machine id and the user id
 *                        (tf__machine_key), so a folder copied to another machine or user does not open. install_hw 0,
 *                        signal 16.
 *
 * <storage>/install/identity.json says which store holds the keys, their names, their public keys (SubjectPublicKey-
 * Info DER), and the install certificate the server gave for them (18.4.3) with its key id and expiry.
 *
 * A NEW agreement key at every attest (the task's rule; the spec's Android rule applied to desktop too): tf__ikey_rotate
 * makes a pending key, the attest goes out signed over its hash, and tf__ikey_commit makes it current and deletes the
 * old one; a failed attest discards the pending key and keeps the old one (18.4.10, install_sig_invalid). The signing
 * key stays (on Linux the one key is replaced as a whole).
 *
 * The raw ECDH secret: CNG's BCRYPT_KDF_RAW_SECRET answers the x coordinate in little-endian order; it is reversed
 * here (the lease vectors' ECDH case only opens with the big-endian x, tests/tf40). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>
#ifndef BCRYPT_KDF_RAW_SECRET
#define BCRYPT_KDF_RAW_SECRET L"TRUNCATE"
#endif
#else
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "tf_internal.h"

const uint8_t *tf__ikey_test_pkcs8; /* tests: a software key from this PKCS#8 (P-256), held in memory, never rotated */
size_t tf__ikey_test_pkcs8_n;
int tf__ikey_force_kind; /* tests: 0 the platform's choice; TF_IKEY_KSP skips the TPM */

/* the SubjectPublicKeyInfo DER of a P-256 point: a fixed 26-byte prefix, then 04 || X || Y */
static const uint8_t kSpkiPrefix[26] = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
                                        0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00};
void tf__spki_p256(const uint8_t xy[64], uint8_t spki[91]) {
  memcpy(spki, kSpkiPrefix, 26);
  spki[26] = 4;
  memcpy(spki + 27, xy, 64);
}

/* one key: in a CNG provider (a handle and its name) or in memory (the scalar and the point) */
typedef struct {
  int set;
#ifdef _WIN32
  NCRYPT_KEY_HANDLE h;
#endif
  char name[96];
  uint8_t d[32];  /* FILE kind only */
  uint8_t xy[64]; /* the public point */
} Key;

struct TfIkey {
  int kind;
  char dir[1024];     /* <storage>/install */
  char scope[17];     /* names this install's keys in the provider: 16 hex of SHA-256(storage folder) */
  int gen;            /* the agreement key's generation (its name) */
  Key agree, sign, pending;
  uint8_t spki_agree[91], spki_sign[91], spki_pending[91];
  char *cert;         /* the install certificate (18.4.3), or NULL */
  long long cert_exp;
  char key_id[64];    /* the certificate's `key` line: the API key id a signed refusal names */
  char att[16];
  int test;           /* the key came from tf__ikey_test_pkcs8 */
#ifdef _WIN32
  NCRYPT_PROV_HANDLE prov;
#endif
};

int tf__ikey_kind(const TfIkey *k) { return k ? k->kind : 0; }
int tf__ikey_hw(const TfIkey *k) { return k && k->kind == TF_IKEY_TPM; }
const char *tf__ikey_kind_name(const TfIkey *k) {
  return !k ? "none" : k->kind == TF_IKEY_TPM ? "tpm" : k->kind == TF_IKEY_KSP ? "software" : "file";
}
const uint8_t *tf__ikey_spki(const TfIkey *k, int which, size_t *n) {
  if (n) *n = 91;
  if (!k) return NULL;
  /* the signing key: a file key store's pending key while an attest is under way, since it signs that attest
   * (tf__ikey_sign) and is the signing key after the commit (Stage B, production, 11-18: the old key was named here, so
   * the server could not verify install_sig and refused every Linux install's first attest, 401 install_sig_invalid) */
  if (which == 1) return k->kind == TF_IKEY_FILE && k->pending.set ? k->spki_pending : k->spki_sign;
  return which == 2 ? (k->pending.set ? k->spki_pending : NULL) : k->spki_agree;
}
const char *tf__ikey_cert(const TfIkey *k) { return k ? k->cert : NULL; }
long long tf__ikey_cert_exp(const TfIkey *k) { return k ? k->cert_exp : 0; }
const char *tf__ikey_key_id(const TfIkey *k) { return k && k->key_id[0] ? k->key_id : NULL; }

/* ------------------------------------------------------------------ PKCS#8 (tests) */
/* the scalar and the point of a PKCS#8 P-256 key as protected40.py and OpenSSL write it: ECPrivateKey's
 * `02 01 01 04 20 <d>` and the public key's `03 42 00 04 <X> <Y>`; 0 or -1 */
static int pkcs8_p256(const uint8_t *p, size_t n, uint8_t d[32], uint8_t xy[64]) {
  int got_d = 0, got_q = 0;
  for (size_t i = 0; i + 5 + 32 <= n && !got_d; ++i)
    if (p[i] == 2 && p[i + 1] == 1 && p[i + 2] == 1 && p[i + 3] == 4 && p[i + 4] == 32) {
      memcpy(d, p + i + 5, 32);
      got_d = 1;
    }
  for (size_t i = 0; i + 4 + 64 <= n && !got_q; ++i)
    if (p[i] == 3 && p[i + 1] == 0x42 && p[i + 2] == 0 && p[i + 3] == 4) {
      memcpy(xy, p + i + 4, 64);
      got_q = 1;
    }
  return got_d && got_q ? 0 : -1;
}

/* ------------------------------------------------------------------ software P-256 (FILE kind) */
#ifdef _WIN32
static BCRYPT_ALG_HANDLE soft_alg(int ecdh) {
  static BCRYPT_ALG_HANDLE h[2];
  if (!h[ecdh] && BCryptOpenAlgorithmProvider(&h[ecdh], ecdh ? BCRYPT_ECDH_P256_ALGORITHM : BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0)) h[ecdh] = NULL;
  return h[ecdh];
}
static BCRYPT_KEY_HANDLE soft_import(const Key *k, int ecdh) {
  uint8_t blob[8 + 96];
  BCRYPT_ECCKEY_BLOB *hd = (BCRYPT_ECCKEY_BLOB *)blob;
  hd->dwMagic = ecdh ? BCRYPT_ECDH_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PRIVATE_P256_MAGIC;
  hd->cbKey = 32;
  memcpy(blob + 8, k->xy, 64);
  memcpy(blob + 72, k->d, 32);
  BCRYPT_KEY_HANDLE h = NULL;
  if (!soft_alg(ecdh) || BCryptImportKeyPair(soft_alg(ecdh), NULL, BCRYPT_ECCPRIVATE_BLOB, &h, blob, sizeof blob, 0)) h = NULL;
  SecureZeroMemory(blob, sizeof blob);
  return h;
}
static int soft_generate(Key *k) {
  BCRYPT_KEY_HANDLE h = NULL;
  uint8_t blob[8 + 96];
  ULONG got = 0;
  if (!soft_alg(1) || BCryptGenerateKeyPair(soft_alg(1), &h, 256, 0) || BCryptFinalizeKeyPair(h, 0) ||
      BCryptExportKey(h, NULL, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof blob, &got, 0) || got != sizeof blob) {
    if (h) BCryptDestroyKey(h);
    return -1;
  }
  BCryptDestroyKey(h);
  memcpy(k->xy, blob + 8, 64);
  memcpy(k->d, blob + 72, 32);
  SecureZeroMemory(blob, sizeof blob);
  k->set = 1;
  return 0;
}
static int soft_agree(const Key *k, const uint8_t *eph, size_t n, uint8_t z[32]) {
  if (n != 65 || eph[0] != 4) return -1;
  BCRYPT_KEY_HANDLE me = soft_import(k, 1), peer = NULL;
  uint8_t pub[8 + 64];
  BCRYPT_ECCKEY_BLOB *hd = (BCRYPT_ECCKEY_BLOB *)pub;
  hd->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
  hd->cbKey = 32;
  memcpy(pub + 8, eph + 1, 64);
  BCRYPT_SECRET_HANDLE s = NULL;
  uint8_t le[32];
  ULONG got = 0;
  int rc = -1;
  if (me && !BCryptImportKeyPair(soft_alg(1), NULL, BCRYPT_ECCPUBLIC_BLOB, &peer, pub, sizeof pub, 0) && !BCryptSecretAgreement(me, peer, &s, 0) &&
      !BCryptDeriveKey(s, BCRYPT_KDF_RAW_SECRET, NULL, le, sizeof le, &got, 0) && got == 32) {
    for (int i = 0; i < 32; ++i) z[i] = le[31 - i];
    rc = 0;
  }
  if (s) BCryptDestroySecret(s);
  if (peer) BCryptDestroyKey(peer);
  if (me) BCryptDestroyKey(me);
  SecureZeroMemory(le, sizeof le);
  return rc;
}
static int soft_sign(const Key *k, const uint8_t *msg, size_t n, uint8_t sig[64]) {
  BCRYPT_KEY_HANDLE h = soft_import(k, 0);
  uint8_t dg[32];
  tf_sha256(msg, n, dg);
  ULONG got = 0;
  const int rc = h && !BCryptSignHash(h, NULL, dg, 32, sig, 64, &got, 0) && got == 64 ? 0 : -1;
  if (h) BCryptDestroyKey(h);
  return rc;
}
#else
static EVP_PKEY *soft_pkey(const Key *k, int with_private) {
  uint8_t pub[65];
  pub[0] = 4;
  memcpy(pub + 1, k->xy, 64);
  OSSL_PARAM_BLD *b = OSSL_PARAM_BLD_new();
  BIGNUM *d = with_private ? BN_bin2bn(k->d, 32, NULL) : NULL;
  OSSL_PARAM_BLD_push_utf8_string(b, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
  OSSL_PARAM_BLD_push_octet_string(b, OSSL_PKEY_PARAM_PUB_KEY, pub, 65);
  if (d) OSSL_PARAM_BLD_push_BN(b, OSSL_PKEY_PARAM_PRIV_KEY, d);
  OSSL_PARAM *ps = OSSL_PARAM_BLD_to_param(b);
  EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
  EVP_PKEY *key = NULL;
  if (c && EVP_PKEY_fromdata_init(c) > 0) EVP_PKEY_fromdata(c, &key, with_private ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, ps);
  EVP_PKEY_CTX_free(c);
  OSSL_PARAM_free(ps);
  OSSL_PARAM_BLD_free(b);
  BN_clear_free(d);
  return key;
}
static int soft_generate(Key *k) {
  EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  if (!key) return -1;
  size_t n = 0;
  uint8_t pt[65];
  BIGNUM *d = NULL;
  const int ok = EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, pt, sizeof pt, &n) && n == 65 &&
                 EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &d) && BN_bn2binpad(d, k->d, 32) == 32;
  if (ok) memcpy(k->xy, pt + 1, 64);
  BN_clear_free(d);
  EVP_PKEY_free(key);
  k->set = ok;
  return ok ? 0 : -1;
}
static int soft_agree(const Key *k, const uint8_t *eph, size_t n, uint8_t z[32]) {
  if (n != 65 || eph[0] != 4) return -1;
  Key peer;
  memset(&peer, 0, sizeof peer);
  memcpy(peer.xy, eph + 1, 64);
  EVP_PKEY *me = soft_pkey(k, 1), *pk = soft_pkey(&peer, 0);
  EVP_PKEY_CTX *c = me ? EVP_PKEY_CTX_new(me, NULL) : NULL;
  size_t zn = 32;
  const int ok = c && pk && EVP_PKEY_derive_init(c) > 0 && EVP_PKEY_derive_set_peer(c, pk) > 0 && EVP_PKEY_derive(c, z, &zn) > 0 && zn == 32;
  EVP_PKEY_CTX_free(c);
  EVP_PKEY_free(pk);
  EVP_PKEY_free(me);
  return ok ? 0 : -1;
}
static int soft_sign(const Key *k, const uint8_t *msg, size_t n, uint8_t sig[64]) {
  EVP_PKEY *key = soft_pkey(k, 1);
  EVP_MD_CTX *c = EVP_MD_CTX_new();
  uint8_t der[80];
  size_t dn = sizeof der;
  int ok = key && c && EVP_DigestSignInit(c, NULL, EVP_sha256(), NULL, key) > 0 && EVP_DigestSign(c, der, &dn, msg, n) > 0;
  if (ok) {
    const uint8_t *p = der;
    ECDSA_SIG *s = d2i_ECDSA_SIG(NULL, &p, (long)dn);
    ok = s && BN_bn2binpad(ECDSA_SIG_get0_r(s), sig, 32) == 32 && BN_bn2binpad(ECDSA_SIG_get0_s(s), sig + 32, 32) == 32;
    ECDSA_SIG_free(s);
  }
  EVP_MD_CTX_free(c);
  EVP_PKEY_free(key);
  return ok ? 0 : -1;
}
#endif

/* ------------------------------------------------------------------ CNG providers (Windows) */
#ifdef _WIN32
static void wname(const char *a, wchar_t *w, size_t n) {
  size_t i = 0;
  for (; a[i] && i + 1 < n; ++i) w[i] = (wchar_t)(unsigned char)a[i];
  w[i] = 0;
}
static int ncrypt_public(NCRYPT_KEY_HANDLE h, uint8_t xy[64]) {
  uint8_t blob[8 + 64];
  DWORD got = 0;
  if (NCryptExportKey(h, 0, BCRYPT_ECCPUBLIC_BLOB, NULL, blob, sizeof blob, &got, 0) || got != sizeof blob) return -1;
  memcpy(xy, blob + 8, 64);
  return 0;
}
/* a new persisted key in the provider under `name` (replacing one of that name), not exportable */
static int ncrypt_create(TfIkey *k, Key *out, const char *name, int ecdh) {
  wchar_t w[96];
  wname(name, w, 96);
  NCRYPT_KEY_HANDLE h = 0;
  if (NCryptCreatePersistedKey(k->prov, &h, ecdh ? NCRYPT_ECDH_P256_ALGORITHM : NCRYPT_ECDSA_P256_ALGORITHM, w, 0, NCRYPT_OVERWRITE_KEY_FLAG))
    return -1;
  if (k->kind == TF_IKEY_KSP) {
    DWORD pol = 0; /* no export of any kind (the KSP's default too; set so it never depends on the default) */
    NCryptSetProperty(h, NCRYPT_EXPORT_POLICY_PROPERTY, (PBYTE)&pol, sizeof pol, NCRYPT_PERSIST_FLAG);
  }
  if (NCryptFinalizeKey(h, 0) || ncrypt_public(h, out->xy)) {
    NCryptDeleteKey(h, 0);
    return -1;
  }
  out->h = h;
  snprintf(out->name, sizeof out->name, "%s", name);
  out->set = 1;
  return 0;
}
static int ncrypt_open_key(TfIkey *k, Key *out, const char *name) {
  wchar_t w[96];
  wname(name, w, 96);
  NCRYPT_KEY_HANDLE h = 0;
  if (NCryptOpenKey(k->prov, &h, w, 0, 0) || ncrypt_public(h, out->xy)) {
    if (h) NCryptFreeObject(h);
    return -1;
  }
  out->h = h;
  snprintf(out->name, sizeof out->name, "%s", name);
  out->set = 1;
  return 0;
}
static void ncrypt_delete(Key *x) {
  if (x->set && x->h) NCryptDeleteKey(x->h, 0); /* frees the handle too */
  x->h = 0;
  x->set = 0;
}
static int ncrypt_agree(NCRYPT_PROV_HANDLE prov, NCRYPT_KEY_HANDLE me, const uint8_t *eph, size_t n, uint8_t z[32]) {
  if (n != 65 || eph[0] != 4) return -1;
  uint8_t pub[8 + 64];
  BCRYPT_ECCKEY_BLOB *hd = (BCRYPT_ECCKEY_BLOB *)pub;
  hd->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
  hd->cbKey = 32;
  memcpy(pub + 8, eph + 1, 64);
  NCRYPT_KEY_HANDLE peer = 0;
  NCRYPT_SECRET_HANDLE s = 0;
  uint8_t le[32];
  DWORD got = 0;
  int rc = -1;
  if (!NCryptImportKey(prov, 0, BCRYPT_ECCPUBLIC_BLOB, NULL, &peer, pub, sizeof pub, 0) && !NCryptSecretAgreement(me, peer, &s, 0) &&
      !NCryptDeriveKey(s, BCRYPT_KDF_RAW_SECRET, NULL, le, sizeof le, &got, 0) && got == 32) {
    for (int i = 0; i < 32; ++i) z[i] = le[31 - i];
    rc = 0;
  }
  if (s) NCryptFreeObject(s);
  if (peer) NCryptFreeObject(peer);
  SecureZeroMemory(le, sizeof le);
  return rc;
}
static int ncrypt_sign(NCRYPT_KEY_HANDLE h, const uint8_t *msg, size_t n, uint8_t sig[64]) {
  uint8_t dg[32];
  tf_sha256(msg, n, dg);
  DWORD got = 0;
  return !NCryptSignHash(h, NULL, dg, 32, sig, 64, &got, 0) && got == 64 ? 0 : -1;
}
#endif

/* ------------------------------------------------------------------ the machine key (Linux key file, state seal) */
/* 32 bytes from the machine id and the user id: SHA-256("quranlab-install-file 1\n" machine-id "\n" uid). A machine
 * without a machine id (some containers) has none: -1, and the key file is then written in the clear with mode 0600
 * (the README says so). Windows does not use it (CNG and DPAPI hold the keys). */
int tf__machine_key(uint8_t out[32]) {
#ifdef _WIN32
  (void)out;
  return -1;
#else
  char *id = NULL;
  size_t idn = 0;
  if (tf_read_file("/etc/machine-id", &id, &idn) && tf_read_file("/var/lib/dbus/machine-id", &id, &idn)) return -1;
  while (idn && (id[idn - 1] == '\n' || id[idn - 1] == '\r' || id[idn - 1] == ' ')) idn--;
  if (!idn) {
    free(id);
    return -1;
  }
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "quranlab-install-file 1\n%.*s\n%lu", (int)idn, id, (unsigned long)getuid());
  const int ok = b.p != NULL;
  if (ok) tf_sha256(b.p, b.n, out);
  tf_buf_free(&b);
  free(id);
  return ok ? 0 : -1;
#endif
}

/* ------------------------------------------------------------------ identity.json */
static char *ipath(const TfIkey *k, const char *name) { return tf_path_join(k->dir, name); }
static void b64u(const uint8_t *in, size_t n, char *out) { tf_b64url(in, n, out); }

/* the certificate's lines: `key`, `exp`, `att` (18.4.3); 0 when it reads as one */
static int cert_parse(TfIkey *k, const char *cert) {
  const char *dot = cert ? strchr(cert, '.') : NULL;
  if (!dot || dot == cert || (size_t)(dot - cert) > 2000) return -1;
  char head[2048];
  snprintf(head, sizeof head, "%.*s", (int)(dot - cert), cert);
  uint8_t text[1600];
  const int n = tf_base64_decode(head, text, sizeof text - 1);
  if (n <= 0) return -1;
  text[n] = 0;
  if (strncmp((const char *)text, "quranlab-install-cert 1\n", 24)) return -1;
  char key_id[64] = "", att[16] = "";
  long long exp = 0;
  for (const char *s = (const char *)text; *s;) {
    const char *e = strchr(s, '\n');
    const size_t len = e ? (size_t)(e - s) : strlen(s);
    if (len > 4 && !strncmp(s, "key ", 4) && len - 4 < sizeof key_id) snprintf(key_id, sizeof key_id, "%.*s", (int)(len - 4), s + 4);
    if (len > 4 && !strncmp(s, "exp ", 4)) exp = atoll(s + 4);
    if (len > 4 && !strncmp(s, "att ", 4) && len - 4 < sizeof att) snprintf(att, sizeof att, "%.*s", (int)(len - 4), s + 4);
    if (!e) break;
    s = e + 1;
  }
  if (!key_id[0] || exp <= 0) return -1;
  snprintf(k->key_id, sizeof k->key_id, "%s", key_id);
  snprintf(k->att, sizeof k->att, "%s", att);
  k->cert_exp = exp;
  return 0;
}

static int save(TfIkey *k) {
  if (k->test && k->kind != TF_IKEY_FILE) return 0;
  char sa[130], ss[130];
  b64u(k->spki_agree, 91, sa);
  b64u(k->spki_sign, 91, ss);
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "{\"v\":1,\"kind\":\"%s\",\"gen\":%d,\"agree\":", tf__ikey_kind_name(k), k->gen);
  tf_buf_json_str(&b, k->agree.name);
  tf_buf_puts(&b, ",\"sign\":");
  tf_buf_json_str(&b, k->sign.name);
  tf_buf_printf(&b, ",\"agree_spki\":\"%s\",\"sign_spki\":\"%s\",\"cert\":", sa, ss);
  if (k->cert) tf_buf_json_str(&b, k->cert);
  else tf_buf_puts(&b, "null");
  tf_buf_puts(&b, "}");
  char *p = ipath(k, "identity.json");
  const int rc = p && b.p && !tf_mkdirs(k->dir) ? tf_write_file(p, b.p, b.n) : -1;
  free(p);
  tf_buf_free(&b);
  return rc;
}

/* Linux: the key file. version 1 (u8), sealed 1 or 0 (u8), then sealed: nonce (12) || AES-256-GCM(d) (32) || tag
 * (16) under the machine key, AAD "quranlab-install-file 1"; not sealed: d (32). The point is made from d again. */
#ifndef _WIN32
static int file_save(TfIkey *k) {
  if (k->test) return 0;
  uint8_t mk[32], out[2 + 12 + 32 + 16], fin[16];
  size_t n = 0;
  out[0] = 1;
  if (!tf__machine_key(mk)) {
    out[1] = 1;
    if (tf_random_bytes(out + 2, 12)) return -1;
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int l = 0, ok = c && EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, mk, out + 2) > 0 &&
                   EVP_EncryptUpdate(c, NULL, &l, (const uint8_t *)"quranlab-install-file 1", 23) > 0 &&
                   EVP_EncryptUpdate(c, out + 14, &l, k->agree.d, 32) > 0 && EVP_EncryptFinal_ex(c, fin, &l) > 0 &&
                   EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, out + 46) > 0;
    EVP_CIPHER_CTX_free(c);
    memset(mk, 0, sizeof mk);
    if (!ok) return -1;
    n = sizeof out;
  } else {
    out[1] = 0;
    memcpy(out + 2, k->agree.d, 32);
    n = 34;
  }
  char *p = ipath(k, "key.bin");
  int rc = p && !tf_mkdirs(k->dir) ? tf_write_file(p, out, n) : -1;
  if (!rc) chmod(p, 0600);
  free(p);
  memset(out, 0, sizeof out);
  return rc;
}
static int file_load(TfIkey *k) {
  char *p = ipath(k, "key.bin"), *raw = NULL;
  size_t n = 0;
  const int have = p && !tf_read_file(p, &raw, &n);
  free(p);
  if (!have) return -1;
  const uint8_t *b = (const uint8_t *)raw;
  int ok = 0;
  if (n == 34 && b[0] == 1 && b[1] == 0) {
    memcpy(k->agree.d, b + 2, 32);
    ok = 1;
  } else if (n == 62 && b[0] == 1 && b[1] == 1) {
    uint8_t mk[32], fin[16];
    if (!tf__machine_key(mk)) {
      EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
      int l = 0;
      ok = c && EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, mk, b + 2) > 0 &&
           EVP_DecryptUpdate(c, NULL, &l, (const uint8_t *)"quranlab-install-file 1", 23) > 0 &&
           EVP_DecryptUpdate(c, k->agree.d, &l, b + 14, 32) > 0 && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, (void *)(b + 46)) > 0 &&
           EVP_DecryptFinal_ex(c, fin, &l) > 0;
      EVP_CIPHER_CTX_free(c);
      memset(mk, 0, sizeof mk);
    }
  }
  memset(raw, 0, n);
  free(raw);
  if (!ok) return -1;
  /* the point from the scalar */
  BIGNUM *d = BN_bin2bn(k->agree.d, 32, NULL);
  EC_GROUP *g = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
  EC_POINT *q = g ? EC_POINT_new(g) : NULL;
  uint8_t pt[65];
  ok = d && q && EC_POINT_mul(g, q, d, NULL, NULL, NULL) && EC_POINT_point2oct(g, q, POINT_CONVERSION_UNCOMPRESSED, pt, 65, NULL) == 65;
  if (ok) memcpy(k->agree.xy, pt + 1, 64);
  EC_POINT_free(q);
  EC_GROUP_free(g);
  BN_clear_free(d);
  k->agree.set = ok;
  return ok ? 0 : -1;
}
#endif

static void spkis(TfIkey *k) {
  tf__spki_p256(k->agree.xy, k->spki_agree);
  tf__spki_p256(k->sign.xy, k->spki_sign);
}

/* a key store with no keys yet: make them */
static int make_keys(TfIkey *k) {
  k->gen = 1;
  if (k->kind == TF_IKEY_FILE) {
    if (soft_generate(&k->agree)) return -1;
    k->sign = k->agree;
#ifndef _WIN32
    if (file_save(k)) return -1;
#endif
  } else {
#ifdef _WIN32
    char an[96], sn[96];
    snprintf(an, sizeof an, "QuranLab-%s-a%d", k->scope, k->gen);
    snprintf(sn, sizeof sn, "QuranLab-%s-s", k->scope);
    if (ncrypt_create(k, &k->agree, an, 1)) return -1;
    if (ncrypt_create(k, &k->sign, sn, 0)) {
      ncrypt_delete(&k->agree);
      return -1;
    }
#endif
  }
  spkis(k);
  return save(k);
}

TfIkey *tf__ikey_open(const char *storage_dir, int create) {
  TfIkey *k = (TfIkey *)calloc(1, sizeof *k);
  if (!k) return NULL;
  char *d = tf_path_join(storage_dir, "install");
  if (!d || strlen(d) >= sizeof k->dir) {
    free(d);
    free(k);
    return NULL;
  }
  snprintf(k->dir, sizeof k->dir, "%s", d);
  free(d);
  uint8_t h[32];
  tf_sha256(storage_dir, strlen(storage_dir), h);
  tf_hex_encode(h, 8, k->scope);
  /* what identity.json says (a file that cannot be read: no identity) */
  char *p = ipath(k, "identity.json"), *text = NULL;
  size_t n = 0;
  TfJson *j = p && !tf_read_file(p, &text, &n) ? tf_json_parse(text, n, NULL) : NULL;
  free(p);
  free(text);
  const char *kind = tf_json_str(tf_json_get(j, "kind"));
  const char *cert = tf_json_str(tf_json_get(j, "cert"));
  if (tf__ikey_test_pkcs8) {
    k->kind = TF_IKEY_FILE;
    k->test = 1;
    if (pkcs8_p256(tf__ikey_test_pkcs8, tf__ikey_test_pkcs8_n, k->agree.d, k->agree.xy)) {
      tf_json_free(j);
      free(k);
      return NULL;
    }
    k->agree.set = 1;
    k->sign = k->agree;
    k->gen = 1;
    spkis(k);
    /* a certificate is kept only for the test key it was given to */
    char sa[130];
    b64u(k->spki_agree, 91, sa);
    const char *was = tf_json_str(tf_json_get(j, "agree_spki"));
    if (cert && was && !strcmp(was, sa) && !cert_parse(k, cert)) {
      k->cert = (char *)malloc(strlen(cert) + 1);
      if (k->cert) strcpy(k->cert, cert);
    }
    tf_json_free(j);
    return k;
  }
#ifdef _WIN32
  /* the store identity.json names, with the keys it names; else (none yet, or its keys are gone from the provider:
   * another user, a cleared TPM) new keys, in the TPM when it makes a P-256 key, else in the software KSP */
  const int want = kind && !strcmp(kind, "tpm") ? TF_IKEY_TPM : kind && !strcmp(kind, "software") ? TF_IKEY_KSP : 0;
  const char *an = tf_json_str(tf_json_get(j, "agree")), *sn = tf_json_str(tf_json_get(j, "sign"));
  if (want && an && sn && (!tf__ikey_force_kind || tf__ikey_force_kind == want)) {
    k->kind = want;
    if (!NCryptOpenStorageProvider(&k->prov, want == TF_IKEY_TPM ? MS_PLATFORM_CRYPTO_PROVIDER : MS_KEY_STORAGE_PROVIDER, 0)) {
      const TfJson *g = tf_json_get(j, "gen");
      k->gen = g && g->type == TJ_NUM ? (int)g->num : 1;
      if (ncrypt_open_key(k, &k->agree, an) || ncrypt_open_key(k, &k->sign, sn)) {
        if (k->agree.set) NCryptFreeObject(k->agree.h);
        memset(&k->agree, 0, sizeof k->agree);
        memset(&k->sign, 0, sizeof k->sign);
      } else {
        spkis(k);
      }
    }
    if (!k->agree.set && k->prov) {
      NCryptFreeObject(k->prov);
      k->prov = 0;
    }
  }
  for (int kd = tf__ikey_force_kind ? tf__ikey_force_kind : TF_IKEY_TPM; create && !k->agree.set && kd <= TF_IKEY_KSP; ++kd) {
    k->kind = kd;
    if (NCryptOpenStorageProvider(&k->prov, kd == TF_IKEY_TPM ? MS_PLATFORM_CRYPTO_PROVIDER : MS_KEY_STORAGE_PROVIDER, 0)) {
      k->prov = 0;
      continue;
    }
    if (make_keys(k)) {
      NCryptFreeObject(k->prov);
      k->prov = 0;
      memset(&k->agree, 0, sizeof k->agree);
      memset(&k->sign, 0, sizeof k->sign);
    }
  }
#else
  (void)kind;
  k->kind = TF_IKEY_FILE;
  if (!file_load(k)) {
    k->sign = k->agree;
    const TfJson *g = tf_json_get(j, "gen");
    k->gen = g && g->type == TJ_NUM ? (int)g->num : 1;
    spkis(k);
  } else if (create && make_keys(k)) {
    memset(&k->agree, 0, sizeof k->agree);
  }
#endif
  if (!k->agree.set) {
    tf_json_free(j);
    tf__ikey_close(k);
    return NULL;
  }
  /* the certificate, when it was given for these keys */
  char sa[130];
  b64u(k->spki_agree, 91, sa);
  const char *was = tf_json_str(tf_json_get(j, "agree_spki"));
  if (cert && was && !strcmp(was, sa) && !cert_parse(k, cert)) {
    k->cert = (char *)malloc(strlen(cert) + 1);
    if (k->cert) strcpy(k->cert, cert);
  }
  tf_json_free(j);
  return k;
}

void tf__ikey_close(TfIkey *k) {
  if (!k) return;
#ifdef _WIN32
  if (k->kind != TF_IKEY_FILE) {
    if (k->agree.set && k->agree.h) NCryptFreeObject(k->agree.h);
    if (k->sign.set && k->sign.h) NCryptFreeObject(k->sign.h);
    if (k->pending.set && k->pending.h) NCryptDeleteKey(k->pending.h, 0);
  }
  if (k->prov) NCryptFreeObject(k->prov);
#endif
  free(k->cert);
  memset(k, 0, sizeof *k);
  free(k);
}

/* the agreement key in use (or, while an attest is out, the pending one) */
static Key *agreement(TfIkey *k, int pending) { return pending && k->pending.set ? &k->pending : &k->agree; }

int tf__ikey_agree(TfIkey *k, int pending, const uint8_t *eph, size_t n, uint8_t z[32]) {
  if (!k) return -1;
  Key *a = agreement(k, pending);
  if (k->kind == TF_IKEY_FILE) return soft_agree(a, eph, n, z);
#ifdef _WIN32
  return ncrypt_agree(k->prov, a->h, eph, n, z);
#else
  return -1;
#endif
}

/* ECDSA P-256 SHA-256 by the signing key: 64 bytes r || s (the server takes it, 18.4.4); on Linux, while a new key
 * is pending, the new key signs (it is the signing key too) */
int tf__ikey_sign(TfIkey *k, int pending, const uint8_t *msg, size_t n, uint8_t *sig, size_t *sig_len) {
  if (!k || !sig || !sig_len || *sig_len < 64) return -1;
  int rc;
  if (k->kind == TF_IKEY_FILE) rc = soft_sign(pending && k->pending.set ? &k->pending : &k->sign, msg, n, sig);
#ifdef _WIN32
  else rc = ncrypt_sign(k->sign.h, msg, n, sig);
#else
  else rc = -1;
#endif
  if (!rc) *sig_len = 64;
  return rc;
}

/* ------------------------------------------------------------------ the new agreement key of an attest */
int tf__ikey_rotate(TfIkey *k) {
  if (!k) return -1;
  if (k->pending.set) return 0;
  if (k->test) {
    k->pending = k->agree; /* tests: the vectors' key stays (the test lease is wrapped for it) */
  } else if (k->kind == TF_IKEY_FILE) {
    if (soft_generate(&k->pending)) return -1;
  } else {
#ifdef _WIN32
    char an[96];
    snprintf(an, sizeof an, "QuranLab-%s-a%d", k->scope, k->gen + 1);
    if (ncrypt_create(k, &k->pending, an, 1)) return -1;
#endif
  }
  tf__spki_p256(k->pending.xy, k->spki_pending);
  return 0;
}

/* the attest took the pending key: it is current now, with the certificate given for it; the old one is deleted */
int tf__ikey_commit(TfIkey *k, const char *cert) {
  if (!k || !k->pending.set) return -1;
  if (k->kind != TF_IKEY_FILE || !k->test) {
#ifdef _WIN32
    if (k->kind != TF_IKEY_FILE) ncrypt_delete(&k->agree);
#endif
    k->agree = k->pending;
    if (k->kind == TF_IKEY_FILE) k->sign = k->pending;
    k->gen++;
  }
  memset(&k->pending, 0, sizeof k->pending);
  spkis(k);
  free(k->cert);
  k->cert = NULL;
  k->cert_exp = 0;
  k->key_id[0] = 0;
  if (cert && !cert_parse(k, cert)) {
    k->cert = (char *)malloc(strlen(cert) + 1);
    if (k->cert) strcpy(k->cert, cert);
  }
  int rc = 0;
#ifndef _WIN32
  rc = file_save(k);
#endif
  return save(k) || rc ? -1 : 0;
}

void tf__ikey_discard(TfIkey *k) {
  if (!k || !k->pending.set) return;
#ifdef _WIN32
  if (k->kind != TF_IKEY_FILE) ncrypt_delete(&k->pending);
#endif
  memset(&k->pending, 0, sizeof k->pending);
}

/* a lease answer carries the certificate again (18.4.3; renewed only by fresh evidence): kept when it is for these
 * keys' install hash */
int tf__ikey_set_cert(TfIkey *k, const char *cert) {
  if (!k || !cert) return -1;
  TfIkey probe = *k;
  if (cert_parse(&probe, cert)) return -1;
  char *copy = (char *)malloc(strlen(cert) + 1);
  if (!copy) return -1;
  strcpy(copy, cert);
  free(k->cert);
  k->cert = copy;
  k->cert_exp = probe.cert_exp;
  snprintf(k->key_id, sizeof k->key_id, "%s", probe.key_id);
  snprintf(k->att, sizeof k->att, "%s", probe.att);
  return save(k);
}
/* the certificate is no longer good (install_cert_invalid): the next lease attests again */
void tf__ikey_drop_cert(TfIkey *k) {
  if (!k) return;
  free(k->cert);
  k->cert = NULL;
  k->cert_exp = 0;
  save(k);
}

/* tests: the install's keys are removed from the store (a later open makes new ones) */
void tf__ikey_destroy_keys(TfIkey *k) {
  if (!k) return;
#ifdef _WIN32
  if (k->kind != TF_IKEY_FILE) {
    ncrypt_delete(&k->agree);
    ncrypt_delete(&k->sign);
  }
#endif
  char *p = ipath(k, "identity.json");
  if (p) remove(p);
  free(p);
  p = ipath(k, "key.bin");
  if (p) remove(p);
  free(p);
}

/* tests: 1 when a private key of this store can be exported in any form (it never should) */
int tf__ikey_exportable(TfIkey *k) {
  if (!k) return 0;
#ifdef _WIN32
  if (k->kind != TF_IKEY_FILE) {
    uint8_t blob[512];
    DWORD got = 0;
    const int a = !NCryptExportKey(k->agree.h, 0, BCRYPT_ECCPRIVATE_BLOB, NULL, blob, sizeof blob, &got, 0);
    const int b = !NCryptExportKey(k->sign.h, 0, BCRYPT_ECCPRIVATE_BLOB, NULL, blob, sizeof blob, &got, 0);
    const int c = !NCryptExportKey(k->agree.h, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, blob, sizeof blob, &got, 0);
    SecureZeroMemory(blob, sizeof blob);
    return a || b || c;
  }
#endif
  return 1; /* the file kind is a software key by definition */
}
