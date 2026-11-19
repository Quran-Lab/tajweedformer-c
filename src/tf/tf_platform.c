/* The table of platform services the closed Core runs on (qlcore.h qlc_platform; PROTECTED_MODEL.md 18.5.2). The
 * Core does no file I/O and no networking of its own: it names a file by its form and its decoded path, and this
 * file decides where it lives. Nothing here reads a plaintext model file: the CDN form and the device form are
 * containers, and only the Core opens them.
 *
 *   QLC_FORM_CDN     <storage>/models/<build>/cdn/<decoded path>.qle   the downloaded container, until installed
 *   QLC_FORM_DEVICE  <storage>/models/<build>/dev/<decoded path>.qld   what the Core writes at install
 *   QLC_FORM_PLAIN   <storage>/models/<build>/<decoded path>           calibration.json, selftest/ (public data)
 *   QLC_FORM_CORE    <storage>/core/<name>                              the Core's own small files (the recited
 *                                                                       journal), written whole, then committed
 *   the state        <storage>/core/state.bin                           save_state; sealed by the Core, and its
 *                                                                       32-byte secret sealed again here
 *
 * A write goes to "<file>.part" in order and commit renames it into place, so a kill leaves the last whole file.
 * remove(NULL) removes every device-form file and partial write of every build (a verified stop, 18.4.10).
 *
 * Clocks (18.4.7): wall_ms is the system time; boot_ms is the time since this boot, sleep included (Windows
 * GetTickCount64, Linux CLOCK_BOOTTIME; 18.4.7 names QueryUnbiasedInterruptTime for Windows, which stops while the
 * machine sleeps, so a lease would outlive its days on a laptop that sleeps: not used), and the boot's id (Windows:
 * the kernel's BootId, a boot count; Linux: /proc/sys/kernel/random/boot_id).
 *
 * The state's seal (state_seal / state_open, optional in qlcore.h): Windows DPAPI for this user; Linux AES-256-GCM
 * under the machine key (tf__machine_key), and none on a machine without a machine id (a container: the Core seals
 * its state under its release's key). The counter (counter_get / counter_raise): Windows a value in the user's
 * registry beside, not inside, the storage folder, so a folder put back from a copy is seen; Linux has none (the
 * optional callbacks are left out, and qlcore.h says what that costs). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>
#else
#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "tf_internal.h"
#include "tf_thread.h"

long long tf__plat_test_wall_ms = -1; /* tests: a fixed wall clock (ms); -1 the system's */
long long tf__plat_test_boot_ms = -1; /* tests: a fixed boot clock (ms); -1 the system's */
const char *tf__plat_test_boot_id;    /* tests: the boot's id; NULL the system's */
int tf__plat_test_no_counter;         /* tests: leave the counter out */

struct TfPlat {
  qlc_platform t;
  char *storage, *core_dir;
  char build_dir[1024]; /* <storage>/models/<build> of the build in use, "" before one is set */
  TfIkey *ik;
  int pending;          /* agree and sign use the pending agreement key (an attest is out) */
  char counter_name[40];
  TfMutex lock;
};

static TfPlat *P(void *u) { return (TfPlat *)u; }

/* where a (form, decoded path) lives; NULL for a path that is not inside the folder */
char *tf__plat_path(TfPlat *p, int form, const char *path) {
  if (!path || !*path || path[0] == '/' || strchr(path, '\\') || strstr(path, "..") || strchr(path, ':')) return NULL;
  char rel[1200];
  if (form == QLC_FORM_CORE) {
    snprintf(rel, sizeof rel, "%s", path);
    return tf_path_join(p->core_dir, rel);
  }
  if (!p->build_dir[0]) return NULL;
  if (form == QLC_FORM_CDN) snprintf(rel, sizeof rel, "cdn/%s.qle", path);
  else if (form == QLC_FORM_DEVICE) snprintf(rel, sizeof rel, "dev/%s.qld", path);
  else if (form == QLC_FORM_PLAIN) snprintf(rel, sizeof rel, "%s", path);
  else return NULL;
  return tf_path_join(p->build_dir, rel);
}
void tf__plat_set_build(TfPlat *p, const char *build_dir) {
  if (!p) return;
  tf_mutex_lock(&p->lock);
  snprintf(p->build_dir, sizeof p->build_dir, "%s", build_dir ? build_dir : "");
  tf_mutex_unlock(&p->lock);
}
void tf__plat_use_pending(TfPlat *p, int on) {
  if (p) p->pending = on;
}

static void mkparent(const char *full) {
  char *dir = (char *)malloc(strlen(full) + 1);
  if (!dir) return;
  strcpy(dir, full);
  char *sl = strrchr(dir, '/');
  char *bs = strrchr(dir, '\\');
  if (bs > sl) sl = bs;
  if (sl) {
    *sl = 0;
    tf_mkdirs(dir);
  }
  free(dir);
}

/* ------------------------------------------------------------------ storage */
static int64_t cb_size(void *u, int form, const char *path) {
  char *f = tf__plat_path(P(u), form, path);
  if (!f) return -1;
  FILE *h = fopen(f, "rb");
  free(f);
  if (!h) return -1;
#ifdef _WIN32
  _fseeki64(h, 0, SEEK_END);
  const int64_t n = _ftelli64(h);
#else
  fseeko(h, 0, SEEK_END);
  const int64_t n = (int64_t)ftello(h);
#endif
  fclose(h);
  return n;
}
static int cb_read(void *u, int form, const char *path, uint64_t off, void *buf, size_t len) {
  char *f = tf__plat_path(P(u), form, path);
  if (!f) return -1;
  FILE *h = fopen(f, "rb");
  free(f);
  if (!h) return -1;
#ifdef _WIN32
  int ok = _fseeki64(h, (long long)off, SEEK_SET) == 0;
#else
  int ok = fseeko(h, (off_t)off, SEEK_SET) == 0;
#endif
  ok = ok && fread(buf, 1, len, h) == len;
  fclose(h);
  return ok ? 0 : -1;
}
static char *part_of(const char *f) {
  char *p = (char *)malloc(strlen(f) + 6);
  if (p) snprintf(p, strlen(f) + 6, "%s.part", f);
  return p;
}
static int cb_write(void *u, int form, const char *path, uint64_t off, const void *buf, size_t len) {
  if (form != QLC_FORM_DEVICE && form != QLC_FORM_CORE) return -1;
  char *f = tf__plat_path(P(u), form, path), *pp = f ? part_of(f) : NULL;
  free(f);
  if (!pp) return -1;
  mkparent(pp);
  /* the writes of one file come in order: the first (offset 0) starts it again */
  FILE *h = fopen(pp, off == 0 ? "wb" : "r+b");
  int ok = h != NULL;
#ifdef _WIN32
  ok = ok && _fseeki64(h, (long long)off, SEEK_SET) == 0;
#else
  ok = ok && fseeko(h, (off_t)off, SEEK_SET) == 0;
#endif
  ok = ok && fwrite(buf, 1, len, h) == len;
  if (h && fclose(h)) ok = 0;
  free(pp);
  return ok ? 0 : -1;
}
static int cb_commit(void *u, int form, const char *path) {
  char *f = tf__plat_path(P(u), form, path), *pp = f ? part_of(f) : NULL;
  int rc = -1;
  if (f && pp) {
#ifdef _WIN32
    wchar_t wa[1100], wb[1100];
    rc = MultiByteToWideChar(CP_UTF8, 0, pp, -1, wa, 1100) && MultiByteToWideChar(CP_UTF8, 0, f, -1, wb, 1100) &&
                 MoveFileExW(wa, wb, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
             ? 0
             : -1;
#else
    rc = rename(pp, f) ? -1 : 0;
#endif
  }
  free(pp);
  free(f);
  return rc;
}
/* every device-form tree of every build */
static int remove_all_device(TfPlat *p) {
  char *models = tf_path_join(p->storage, "models");
  char (*names)[256] = (char (*)[256])malloc(64 * 256);
  int rc = 0;
  const int n = models && names ? tf__list_dir(models, names, 64) : 0;
  for (int i = 0; i < n; ++i) {
    char *bd = tf_path_join(models, names[i]), *dev = bd ? tf_path_join(bd, "dev") : NULL;
    if (dev && tf__remove_tree(dev)) rc = -1;
    free(dev);
    free(bd);
  }
  free(names);
  free(models);
  return rc;
}
static int cb_remove(void *u, int form, const char *path) {
  if (!path) return form == QLC_FORM_DEVICE ? remove_all_device(P(u)) : -1;
  char *f = tf__plat_path(P(u), form, path), *pp = f ? part_of(f) : NULL;
  if (pp) remove(pp);
  const int rc = f && (remove(f) == 0 || !tf_file_exists(f)) ? 0 : -1;
  free(pp);
  free(f);
  return rc;
}

/* A read-only mapping of a stored file (qlcore.h map/unmap): the Core decrypts a device form's records from it
 * straight into its own memory, and maps the recited index. A real mapping of the file (MapViewOfFile, mmap), never a
 * copy: the Core rests its pages once read. An empty file or one that does not open: -1 (the Core reads instead). */
#ifdef _WIN32
static int cb_map(void *u, int form, const char *path, const void **data, size_t *len) {
  char *f = tf__plat_path(P(u), form, path);
  wchar_t w[1100];
  const int ok = f && MultiByteToWideChar(CP_UTF8, 0, f, -1, w, 1100) > 0;
  free(f);
  if (!ok) return -1;
  HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  LARGE_INTEGER sz;
  HANDLE m = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && (unsigned long long)sz.QuadPart <= (size_t)-1
                 ? CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL)
                 : NULL;
  CloseHandle(h);
  if (!m) return -1;
  const void *v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(m); /* the view keeps the mapping */
  if (!v) return -1;
  *data = v;
  *len = (size_t)sz.QuadPart;
  return 0;
}
static void cb_unmap(void *u, const void *data, size_t len) {
  (void)u;
  (void)len;
  if (data) UnmapViewOfFile(data);
}
#else
static int cb_map(void *u, int form, const char *path, const void **data, size_t *len) {
  char *f = tf__plat_path(P(u), form, path);
  const int fd = f ? open(f, O_RDONLY | O_CLOEXEC) : -1;
  free(f);
  if (fd < 0) return -1;
  struct stat st;
  void *v = fstat(fd, &st) == 0 && st.st_size > 0 ? mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
  close(fd); /* the mapping keeps the file */
  if (v == MAP_FAILED) return -1;
  *data = v;
  *len = (size_t)st.st_size;
  return 0;
}
static void cb_unmap(void *u, const void *data, size_t len) {
  (void)u;
  if (data) munmap((void *)data, len);
}
#endif

/* ------------------------------------------------------------------ clocks and randomness */
static int64_t cb_wall_ms(void *u) {
  (void)u;
  if (tf__plat_test_wall_ms >= 0) return tf__plat_test_wall_ms;
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  const unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (int64_t)(t / 10000ULL) - 11644473600000LL;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}
static int64_t cb_boot_ms(void *u, uint8_t boot_id[32]) {
  (void)u;
  char id[128] = "";
  int64_t ms;
#ifdef _WIN32
  ms = (int64_t)GetTickCount64();
  DWORD v = 0, sz = sizeof v;
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters", "BootId",
                   RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
    snprintf(id, sizeof id, "bootid %lu", (unsigned long)v);
  else /* no BootId: the boot's start on the wall clock, in 10-minute steps (a step crossed reads as a reboot) */
    snprintf(id, sizeof id, "boottime %lld", (long long)((cb_wall_ms(u) - ms) / 600000));
#else
  struct timespec ts;
  clock_gettime(CLOCK_BOOTTIME, &ts);
  ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  char *b = NULL;
  size_t n = 0;
  if (!tf_read_file("/proc/sys/kernel/random/boot_id", &b, &n)) {
    snprintf(id, sizeof id, "%.*s", (int)(n < 100 ? n : 100), b);
    free(b);
  }
#endif
  if (tf__plat_test_boot_ms >= 0) ms = tf__plat_test_boot_ms;
  if (tf__plat_test_boot_id) snprintf(id, sizeof id, "%s", tf__plat_test_boot_id);
  char text[160];
  snprintf(text, sizeof text, "quranlab-boot 1\n%s\n", id);
  tf_sha256(text, strlen(text), boot_id);
  return ms;
}
static int cb_random(void *u, void *buf, size_t len) {
  (void)u;
  return tf_random_bytes((uint8_t *)buf, len) ? -1 : 0;
}

/* ------------------------------------------------------------------ the key store */
static int cb_agree(void *u, const uint8_t *eph, size_t n, uint8_t z[32]) { return tf__ikey_agree(P(u)->ik, P(u)->pending, eph, n, z); }
static int cb_sign(void *u, const uint8_t *msg, size_t n, uint8_t *sig, size_t *sig_len) {
  return tf__ikey_sign(P(u)->ik, P(u)->pending, msg, n, sig, sig_len);
}

/* ------------------------------------------------------------------ the state */
static void cb_save_state(void *u, const uint8_t *blob, size_t n) {
  char *f = tf_path_join(P(u)->core_dir, "state.bin");
  if (f && !tf_mkdirs(P(u)->core_dir)) tf_write_file(f, blob, n);
  free(f);
}
#ifdef _WIN32
static int dpapi(const uint8_t *in, size_t n, uint8_t *out, size_t *out_n, int open) {
  static const char ent[] = "quranlab-core-state 1";
  DATA_BLOB a = {(DWORD)n, (BYTE *)in}, b = {0, NULL}, e = {(DWORD)(sizeof ent - 1), (BYTE *)ent};
  const BOOL ok = open ? CryptUnprotectData(&a, NULL, &e, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &b)
                       : CryptProtectData(&a, L"Quran Lab Core state", &e, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &b);
  if (!ok) return -1;
  int rc = -1;
  if (b.cbData <= *out_n) {
    memcpy(out, b.pbData, b.cbData);
    *out_n = b.cbData;
    rc = 0;
  }
  SecureZeroMemory(b.pbData, b.cbData);
  LocalFree(b.pbData);
  return rc;
}
static int cb_state_seal(void *u, const uint8_t *in, size_t n, uint8_t *out, size_t *out_n) {
  (void)u;
  return dpapi(in, n, out, out_n, 0);
}
static int cb_state_open(void *u, const uint8_t *in, size_t n, uint8_t *out, size_t *out_n) {
  (void)u;
  return dpapi(in, n, out, out_n, 1);
}
/* the counter: HKCU\Software\Quran Lab\TajweedFormer, a QWORD per storage folder */
static int cb_counter_get(void *u, uint64_t *v) {
  unsigned long long x = 0;
  DWORD sz = sizeof x;
  const LSTATUS st = RegGetValueA(HKEY_CURRENT_USER, "Software\\Quran Lab\\TajweedFormer", P(u)->counter_name, RRF_RT_REG_QWORD, NULL, &x, &sz);
  if (st == ERROR_FILE_NOT_FOUND) x = 0;
  else if (st != ERROR_SUCCESS) return -1;
  *v = x;
  return 0;
}
static int cb_counter_raise(void *u, uint64_t v) {
  uint64_t now = 0;
  if (cb_counter_get(u, &now)) return -1;
  if (v <= now) return 0;
  HKEY k;
  if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Quran Lab\\TajweedFormer", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return -1;
  const unsigned long long x = v;
  const LSTATUS st = RegSetValueExA(k, P(u)->counter_name, 0, REG_QWORD, (const BYTE *)&x, sizeof x);
  RegCloseKey(k);
  return st == ERROR_SUCCESS ? 0 : -1;
}
#else
/* nonce (12) || AES-256-GCM(in) || tag (16) under the machine key, AAD "quranlab-core-state 1" */
static int cb_state_seal(void *u, const uint8_t *in, size_t n, uint8_t *out, size_t *out_n) {
  (void)u;
  uint8_t mk[32], fin[16];
  if (n > 512 || *out_n < n + 28 || tf__machine_key(mk) || tf_random_bytes(out, 12)) return -1;
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  int l = 0;
  const int ok = c && EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, mk, out) > 0 &&
                 EVP_EncryptUpdate(c, NULL, &l, (const uint8_t *)"quranlab-core-state 1", 21) > 0 && EVP_EncryptUpdate(c, out + 12, &l, in, (int)n) > 0 &&
                 EVP_EncryptFinal_ex(c, fin, &l) > 0 && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, out + 12 + n) > 0;
  EVP_CIPHER_CTX_free(c);
  memset(mk, 0, sizeof mk);
  if (ok) *out_n = n + 28;
  return ok ? 0 : -1;
}
static int cb_state_open(void *u, const uint8_t *in, size_t n, uint8_t *out, size_t *out_n) {
  (void)u;
  uint8_t mk[32], fin[16];
  if (n < 28 || *out_n < n - 28 || tf__machine_key(mk)) return -1;
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  int l = 0;
  const int ok = c && EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, mk, in) > 0 &&
                 EVP_DecryptUpdate(c, NULL, &l, (const uint8_t *)"quranlab-core-state 1", 21) > 0 &&
                 EVP_DecryptUpdate(c, out, &l, in + 12, (int)(n - 28)) > 0 && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, (void *)(in + n - 16)) > 0 &&
                 EVP_DecryptFinal_ex(c, fin, &l) > 0;
  EVP_CIPHER_CTX_free(c);
  memset(mk, 0, sizeof mk);
  if (ok) *out_n = n - 28;
  return ok ? 0 : -1;
}
#endif

TfPlat *tf__plat_new(const char *storage_dir, TfIkey *ik) {
  TfPlat *p = (TfPlat *)calloc(1, sizeof *p);
  if (!p) return NULL;
  p->storage = tf_path_join(storage_dir, "");
  p->core_dir = tf_path_join(storage_dir, "core");
  if (!p->storage || !p->core_dir) {
    tf__plat_free(p);
    return NULL;
  }
  tf_mutex_init(&p->lock);
  p->ik = ik;
  uint8_t h[32];
  tf_sha256(storage_dir, strlen(storage_dir), h);
  char hx[33];
  tf_hex_encode(h, 16, hx);
  snprintf(p->counter_name, sizeof p->counter_name, "c%s", hx);
  qlc_platform *t = &p->t;
  t->struct_size = sizeof *t;
  t->user = p;
  t->size = cb_size;
  t->read = cb_read;
  t->write = cb_write;
  t->commit = cb_commit;
  t->remove = cb_remove;
  t->wall_ms = cb_wall_ms;
  t->boot_ms = cb_boot_ms;
  t->random = cb_random;
  t->agree = cb_agree;
  t->sign = cb_sign;
  t->save_state = cb_save_state;
  t->signals = tf__ikey_hw(ik) ? 0 : 16; /* 18.6.4 bit 4: a software-only key store */
#ifdef _WIN32
  t->state_seal = cb_state_seal;
  t->state_open = cb_state_open;
#else
  {
    /* only with a machine key: a machine without a machine id (a container) leaves the seal to the Core, which then
     * seals its state under its release's key (qlcore.h: it opens in that release only); a seal that always failed
     * kept no state at all, so no lease outlived the process */
    uint8_t mk[32];
    if (!tf__machine_key(mk)) {
      t->state_seal = cb_state_seal;
      t->state_open = cb_state_open;
    }
    memset(mk, 0, sizeof mk);
  }
#endif
  t->map = cb_map;
  t->unmap = cb_unmap;
#ifdef _WIN32
  if (!tf__plat_test_no_counter) {
    t->counter_get = cb_counter_get;
    t->counter_raise = cb_counter_raise;
  }
#endif
  return p;
}
/* the table for the agreement key in use, or for the pending one while an attest is out */
const qlc_platform *tf__plat_table(TfPlat *p, int pending) {
  size_t n = 0;
  p->t.install_key_spki = tf__ikey_spki(p->ik, pending ? 2 : 0, &n);
  p->t.install_key_spki_len = n;
  p->pending = pending;
  return &p->t;
}
/* the state the Core saved last (NULL when none): the caller frees */
uint8_t *tf__plat_state(TfPlat *p, size_t *n) {
  char *f = tf_path_join(p->core_dir, "state.bin"), *b = NULL;
  *n = 0;
  if (f && tf_read_file(f, &b, n)) b = NULL;
  free(f);
  return (uint8_t *)b;
}
void tf__plat_free(TfPlat *p) {
  if (!p) return;
  if (p->storage) tf_mutex_destroy(&p->lock);
  free(p->storage);
  free(p->core_dir);
  free(p);
}
/* tests: forget the counter of this storage folder; the keys go too when nothing else is in them */
#ifdef _WIN32
static void drop_key_if_empty(const char *path) {
  HKEY k;
  DWORD subkeys = 1, values = 1;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
  const LSTATUS st = RegQueryInfoKeyA(k, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
  RegCloseKey(k);
  if (st == ERROR_SUCCESS && !subkeys && !values) RegDeleteKeyA(HKEY_CURRENT_USER, path);
}
#endif
void tf__plat_counter_reset(TfPlat *p) {
#ifdef _WIN32
  HKEY k;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Quran Lab\\TajweedFormer", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
    RegDeleteValueA(k, p->counter_name);
    RegCloseKey(k);
  }
  drop_key_if_empty("Software\\Quran Lab\\TajweedFormer");
  drop_key_if_empty("Software\\Quran Lab");
#else
  (void)p;
#endif
}
