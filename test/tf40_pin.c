/* The production pin (integration, not the unit tests' test release): the Core release this SDK ships with, loaded by
 * its path and checked against what core/CORE_RELEASE and core/qlcore.h say.
 *
 *   tf40_pin <the release's qlcore.dll or qlcore.so> <core/CORE_RELEASE> <core/qlcore.h> <target>
 *
 * - qlc_version() is "<release> <target>" of CORE_RELEASE (the release that answers to production's root);
 * - every function core/qlcore.h declares (QLC_API) is exported by the binary, the grace's two among them, so the
 *   header vendored here is that binary's interface;
 * - the calls that take a Core refuse a missing one (QLC_E_ARG) rather than crash: qlc_create without a platform,
 *   qlc_lease_attempt, qlc_lease_grace.
 * The file's SHA-256 is checked by ctest beside it (tf40_production_sha256). Nothing here needs a lease, a key or the
 * network: a production Core opens nothing a test signs, and production leases are the end-to-end run's. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int fails, checks;
#define CHECK(c, ...)                                                                                                                      \
  do {                                                                                                                                     \
    checks++;                                                                                                                              \
    if (!(c)) {                                                                                                                            \
      fails++;                                                                                                                             \
      printf("FAIL ");                                                                                                                     \
      printf(__VA_ARGS__);                                                                                                                 \
      printf("\n");                                                                                                                        \
    }                                                                                                                                      \
  } while (0)

static char *slurp(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = n >= 0 ? (char *)malloc((size_t)n + 1) : NULL;
  if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
    free(b);
    b = NULL;
  }
  if (b) b[n] = 0;
  fclose(f);
  return b;
}

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: tf40_pin <core binary> <core/CORE_RELEASE> <core/qlcore.h> <target>\n");
    return 2;
  }
  char *rel = slurp(argv[2]), *hdr = slurp(argv[3]);
  if (!rel || !hdr) {
    fprintf(stderr, "cannot read %s or %s\n", argv[2], argv[3]);
    return 2;
  }
  char release[64] = "";
  if (sscanf(rel, "release %63s", release) != 1) {
    fprintf(stderr, "%s: no release line\n", argv[2]);
    return 2;
  }
#ifdef _WIN32
  HMODULE h = LoadLibraryA(argv[1]);
#define SYM(name) ((void *)GetProcAddress(h, name))
#else
  void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
#define SYM(name) dlsym(h, name)
#endif
  CHECK(h != NULL, "the production Core loads (%s)", argv[1]);
  if (!h) return 1;
  const char *(*version)(void) = (const char *(*)(void))SYM("qlc_version");
  char want[128];
  snprintf(want, sizeof want, "%s %s", release, argv[4]);
  CHECK(version && !strcmp(version(), want), "qlc_version is \"%s\" (got \"%s\")", want, version ? version() : "no symbol");
  /* every QLC_API function of the header */
  int names = 0, found = 0;
  for (const char *p = strstr(hdr, "QLC_API "); p; p = strstr(p + 8, "QLC_API ")) {
    /* the declared name: the identifier right before the first '(' of the declaration */
    const char *e = strchr(p, '(');
    if (!e) continue;
    const char *q = e;
    while (q > p && (q[-1] == '_' || (q[-1] >= 'a' && q[-1] <= 'z') || (q[-1] >= '0' && q[-1] <= '9'))) q--;
    if (strncmp(q, "qlc_", 4) || e - q >= 64) continue;
    char name[64];
    memcpy(name, q, (size_t)(e - q));
    name[e - q] = 0;
    names++;
    const int ok = SYM(name) != NULL;
    found += ok;
    CHECK(ok, "the binary exports %s, which core/qlcore.h declares", name);
  }
  CHECK(names >= 40 && SYM("qlc_lease_attempt") && SYM("qlc_lease_grace"), "the header's %d functions, the grace's two among them", names);
  /* the calls refuse a missing Core */
  void *(*create)(const void *, const uint8_t *, size_t, int *) = (void *(*)(const void *, const uint8_t *, size_t, int *))SYM("qlc_create");
  int (*attempt)(void *, int) = (int (*)(void *, int))SYM("qlc_lease_attempt");
  int (*grace)(void *, int64_t *) = (int (*)(void *, int64_t *))SYM("qlc_lease_grace");
  int st = 0;
  void *core = create ? create(NULL, NULL, 0, &st) : NULL;
  CHECK(create && !core && st == -1, "qlc_create without a platform is QLC_E_ARG (%d)", st);
  CHECK(attempt && attempt(NULL, 1) == -1, "qlc_lease_attempt without a Core is QLC_E_ARG");
  CHECK(grace && grace(NULL, NULL) == -1, "qlc_lease_grace without a Core is QLC_E_ARG");
  printf("production pin %s %s: %d of %d functions exported, %s (%d checks)\n", release, argv[4], found, names, fails ? "FAIL" : "ok", checks);
  free(rel);
  free(hdr);
  return fails ? 1 : 0;
}
