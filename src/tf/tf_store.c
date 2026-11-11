/* The model files of a Client: which build Quran Lab's signed config names for this app, where it lives on disk,
 * fetching it (resumable), and what is on disk when there is no network.
 *
 *   <storage>/models/<build>/        one folder per build (the build id: 40 hex), as the downloader fills it
 *   <storage>/models/current.json    the build last fetched completely: {"build","version","repo","folder","id"}
 *
 * SPEC 2a: the server picks the build for this SDK (the config's "recommended": repo, folder, revision = the
 * build, version); the SDK downloads by the build and reports the version. A pin is a version sent with the config
 * request. SPEC 13.4: a first run without a network is TF_ERR_NEEDS_CONNECTION, not a timeout; with a model on
 * disk the SDK starts offline on it. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

typedef struct {
  char build[41], version[24], repo[128], folder[64], id[40], base[256];
} Pick;

static char *models_dir(TfClient *c) { return tf_path_join(tf__client_dir(c), "models"); }

static int is_build(const char *s) {
  if (!s || strlen(s) != 40) return 0;
  for (const char *p = s; *p; ++p)
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0;
  return 1;
}
/* a repo or folder as it goes into a URL: letters, digits, '.', '_', '-', '/' ; no "..", no leading '/' */
static int url_part(const char *s, size_t max) {
  if (!s || !*s || strlen(s) >= max || s[0] == '/' || strstr(s, "..")) return 0;
  for (const char *p = s; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("._-/", *p))) return 0;
  return 1;
}

/* the config's choice: "recommended": {"model": repo, "model_id", "folder", "revision": build, "version"}, and the
 * download base ("base", else the client's). 0, or -1 when the config in force names no usable build. */
static int pick_from_config(TfClient *c, Pick *p) {
  memset(p, 0, sizeof *p);
  const char *cfg = tf__client_config_json(c);
  TfJson *j = cfg ? tf_json_parse(cfg, strlen(cfg), NULL) : NULL;
  const TfJson *r = tf_json_get(j, "recommended");
  const char *repo = tf_json_str(tf_json_get(r, "model")), *folder = tf_json_str(tf_json_get(r, "folder"));
  const char *build = tf_json_str(tf_json_get(r, "revision")), *version = tf_json_str(tf_json_get(r, "version"));
  const char *id = tf_json_str(tf_json_get(r, "model_id")), *base = tf_json_str(tf_json_get(j, "base"));
  int ok = r && is_build(build) && tf__is_model_version(version) && url_part(repo, sizeof p->repo) && url_part(folder, sizeof p->folder);
  if (ok) {
    snprintf(p->build, sizeof p->build, "%s", build);
    snprintf(p->version, sizeof p->version, "%s", version);
    snprintf(p->repo, sizeof p->repo, "%s", repo);
    snprintf(p->folder, sizeof p->folder, "%s", folder);
    snprintf(p->id, sizeof p->id, "%s", id ? id : "");
    if (base && !strncmp(base, "https://", 8) && strlen(base) < sizeof p->base) snprintf(p->base, sizeof p->base, "%s", base);
  }
  tf_json_free(j);
  return ok ? 0 : -1;
}

/* the build last fetched completely (current.json), when its folder still holds a manifest */
static int pick_from_disk(TfClient *c, Pick *p) {
  memset(p, 0, sizeof *p);
  char *dir = models_dir(c), *cur = dir ? tf_path_join(dir, "current.json") : NULL, *text = NULL;
  size_t n = 0;
  TfJson *j = cur && !tf_read_file(cur, &text, &n) ? tf_json_parse(text, n, NULL) : NULL;
  free(text);
  free(cur);
  const char *build = tf_json_str(tf_json_get(j, "build")), *version = tf_json_str(tf_json_get(j, "version"));
  const char *repo = tf_json_str(tf_json_get(j, "repo")), *folder = tf_json_str(tf_json_get(j, "folder"));
  int ok = is_build(build) && tf__is_model_version(version) && url_part(repo, sizeof p->repo) && url_part(folder, sizeof p->folder);
  if (ok) {
    snprintf(p->build, sizeof p->build, "%s", build);
    snprintf(p->version, sizeof p->version, "%s", version);
    snprintf(p->repo, sizeof p->repo, "%s", repo);
    snprintf(p->folder, sizeof p->folder, "%s", folder);
    snprintf(p->id, sizeof p->id, "%s", tf_json_str(tf_json_get(j, "id")) ? tf_json_str(tf_json_get(j, "id")) : "");
    char *bd = tf_path_join(dir, p->build), *mf = bd ? tf_path_join(bd, "manifest.json") : NULL;
    ok = mf && tf_file_exists(mf);
    free(mf);
    free(bd);
  }
  tf_json_free(j);
  free(dir);
  return ok ? 0 : -1;
}

static void write_current(TfClient *c, const Pick *p) {
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"build\":");
  tf_buf_json_str(&b, p->build);
  tf_buf_puts(&b, ",\"version\":");
  tf_buf_json_str(&b, p->version);
  tf_buf_puts(&b, ",\"repo\":");
  tf_buf_json_str(&b, p->repo);
  tf_buf_puts(&b, ",\"folder\":");
  tf_buf_json_str(&b, p->folder);
  tf_buf_puts(&b, ",\"id\":");
  tf_buf_json_str(&b, p->id);
  tf_buf_puts(&b, "}");
  char *dir = models_dir(c), *cur = dir ? tf_path_join(dir, "current.json") : NULL;
  if (cur && b.p) tf_write_file(cur, b.p, b.n);
  free(cur);
  free(dir);
  tf_buf_free(&b);
}

/* why the server would not talk to this app, as a catalogue error (0 when it did not refuse) */
static int stop_error(TfClient *c) {
  const char *code = tf__client_stop_code(c);
  if (!tf__client_stopped(c) || !*code) return 0;
  if (!strcmp(code, "invalid_key") || !strcmp(code, "unauthorized")) return tf__fail(TF_ERR_INVALID_KEY, NULL);
  if (!strcmp(code, "sdk_too_old")) return tf__fail_sdk_too_old(tf__client_stop_detail(c));
  return tf__fail_rejected(code, tf__client_app(c));
}

/* the folder of a build, and the open options that go with it */
static void fill_open(TfClient *c, const Pick *p, char *out_dir, size_t n, TfModelOpen *open, char keep[4][256]) {
  char *dir = models_dir(c), *bd = dir ? tf_path_join(dir, p->build) : NULL;
  snprintf(out_dir, n, "%s", bd ? bd : "");
  free(bd);
  free(dir);
  if (!open) return;
  snprintf(keep[0], 256, "%s", p->version);
  snprintf(keep[1], 256, "%s", p->build);
  snprintf(keep[2], 256, "%s", p->repo);
  snprintf(keep[3], 256, "%s", p->folder);
  open->client = c;
  open->trusted_key_hex = tf__client_model_key(c);
  open->want_version = keep[0];
  open->want_build = keep[1];
  open->repo = keep[2];
  open->folder_path = keep[3];
}

static _Thread_local char g_keep[4][256]; /* the strings a TfModelOpen filled here points at, until the next call */

int tf__store_fetch(TfClient *c, const TfFetch *f, char *out_dir, size_t n, TfModelOpen *open) {
  tf__clear_error();
  TfFetch none;
  memset(&none, 0, sizeof none);
  if (!f) f = &none;
  if (f->version && !tf__is_model_version(f->version)) return tf__fail(TF_ERR_INVALID_ARGUMENT, "%s is not a model version (x.y.z)", f->version);
  /* the signed config, asked with the pin: a conditional request when this client holds one for that pin */
  tf__client_set_pin(c, f->version);
  const int net = tf__client_refresh_config(c);
  const int offline = net == TF__ERR_NETWORK;
  Pick p, disk;
  const int have_cfg = pick_from_config(c, &p) == 0;
  const int have_disk = pick_from_disk(c, &disk) == 0;
  if (!have_cfg) {
    /* no config in force (no network, or the server refused): what is on disk still runs */
    const int refused = stop_error(c);
    if (have_disk) {
      tf__clear_error();
      fill_open(c, &disk, out_dir, n, open, g_keep);
      return 0;
    }
    if (refused) return refused;
    if (offline) return tf__fail(TF_ERR_NEEDS_CONNECTION, NULL);
    /* a config is in force and recommends nothing: Quran Lab has no build for this SDK version yet (the config
     * is asked again at every call and every minute of use; nothing is retried in a loop here) */
    if (tf__client_config_json(c)) return tf__fail(TF_ERR_MODEL_UNAVAILABLE, NULL);
    return tf__fail(TF_ERR_DOWNLOAD_FAILED, "the signed config could not be fetched");
  }
  tf__clear_error();
  fill_open(c, &p, out_dir, n, open, g_keep);
  char *mf = tf_path_join(out_dir, "manifest.json");
  const int on_disk = mf && tf_file_exists(mf);
  free(mf);
  /* here = fetched completely once (current.json is written after a whole download): a folder with a manifest
   * and half the weights is not a model */
  const int complete = on_disk && have_disk && !strcmp(disk.build, p.build);
  /* without a network: the build the config names when it is here, else the build that is; a first run is
   * TF_ERR_NEEDS_CONNECTION, not a timeout (SPEC 13.4) */
  if (offline) {
    if (complete) return 0;
    if (have_disk) {
      fill_open(c, &disk, out_dir, n, open, g_keep);
      return 0;
    }
    return tf__fail(TF_ERR_NEEDS_CONNECTION, NULL);
  }
  /* an unmetered-only prefetch downloads on an unmetered connection and on one of unknown kind, and waits on a
   * metered one and without one (what is on disk stays usable). This library's answer is always unknown. */
  const int kind = tf__connection();
  if (f->unmetered_only && (kind == TF__CONNECTION_METERED || kind == TF__CONNECTION_NONE)) {
    if (complete) return 0;
    return tf__fail(TF_ERR_DOWNLOAD_FAILED, "this prefetch waits for an unmetered connection (the connection is %s)", tf__connection_name(kind));
  }
  /* which context's files: the caller's, else the config's inference.context, else c16_l64 (the default of every
   * build so far; a build with another default gets it on first use, as any other context) */
  char cfg_ctx[32] = "";
  const char *ctx = f->context ? f->context : tf__client_inference_context(c, cfg_ctx, sizeof cfg_ctx);
  TfDownload d = tf__download_defaults();
  d.cdn_url = p.base[0] ? p.base : tf__client_cdn(c);
  d.key = tf__client_key(c);
  d.repo = p.repo;
  d.revision = p.build;
  d.folder = p.folder;
  d.dest = out_dir;
  d.contexts = ctx ? ctx : "c16_l64";
  d.trusted_key_hex = tf__client_model_key(c);
  d.want_version = p.version;
  d.check_build = 1;
  d.progress = f->on_progress;
  d.user = f->user;
  /* the folder says which version it is being filled with, for the storage report while the manifest is not here */
  if (!on_disk && !tf_mkdirs(out_dir)) {
    char *mk = tf_path_join(out_dir, "version");
    if (mk) tf_write_file(mk, p.version, strlen(p.version));
    free(mk);
  }
  long long fetched = 0;
  int resumed = 0;
  d.fetched = &fetched;
  d.resumed = &resumed;
  const int rc = tf__download_model(&d);
  /* the day's telemetry: one attempt for this build (none when nothing was fetched and nothing failed) */
  if (rc || fetched > 0) tf__client_count_download(c, p.version, p.build, fetched, rc == 0, resumed);
  if (!rc && open) open->skip_file_checks = 1; /* every engine file of the context was hashed just now */
  if (rc == TF_ERR_DOWNLOAD_FAILED && open && (complete || have_disk)) {
    /* the CDN did not answer and a model is here: it runs (the folder is verified again at open); a prefetch
     * has nothing to open and reports the failure */
    tf__clear_error();
    if (!complete) fill_open(c, &disk, out_dir, n, open, g_keep);
    return 0;
  }
  if (rc) return rc;
  write_current(c, &p);
  return 0;
}

/* removes every build folder but `keep` (a newer build replaced them); call it only while no Model is open */
void tf__store_prune(TfClient *c, const char *keep) {
  char *dir = models_dir(c);
  char (*names)[256] = (char (*)[256])malloc(64 * 256);
  const int n = dir && names ? tf__list_dir(dir, names, 64) : 0;
  for (int i = 0; i < n; ++i) {
    if (!is_build(names[i]) || (keep && !strcmp(names[i], keep))) continue;
    char *bd = tf_path_join(dir, names[i]);
    if (bd) tf__remove_tree(bd);
    free(bd);
  }
  free(names);
  free(dir);
}

int tf__store_fetch_role(TfClient *c, const TfModel *m, const char *context, const char *role) {
  if (!c || !m) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  /* the Model came from this store: its folder is models/<build> and current.json (or the config) names its repo */
  Pick p;
  if (pick_from_disk(c, &p) || strcmp(p.build, tf_model_build(m))) {
    if (pick_from_config(c, &p) || strcmp(p.build, tf_model_build(m)))
      return tf__fail(role ? TF_ERR_REFERENCE_MISSING : TF_ERR_MODEL_MISSING, "the files of %s are not on this device",
                      context ? context : role ? role : "the model");
  }
  TfDownload d = tf__download_defaults();
  d.cdn_url = p.base[0] ? p.base : tf__client_cdn(c);
  d.key = tf__client_key(c);
  d.repo = p.repo;
  d.revision = p.build;
  d.folder = p.folder;
  d.dest = tf__model_dir(m);
  d.contexts = context;
  d.only_role = role;
  d.trusted_key_hex = tf__client_model_key(c);
  d.want_version = p.version;
  d.check_build = 1;
  return tf__download_model(&d);
}

typedef struct {
  char version[24], build[41];
  long long bytes;
} Entry;
static int cmp_entry(const void *a, const void *b) {
  const Entry *x = (const Entry *)a, *y = (const Entry *)b;
  const int v = tf__version_cmp(x->version, y->version);
  return v ? v : strcmp(x->build, y->build);
}
/* What the SDK holds on disk (the vectors README section 11): {"models":[{"version","build","bytes"}],
 * "sharingQueueBytes":n}: one entry per model build on disk with the bytes of its files, ordered by version then
 * build; the bytes of the sharing queue. The version of a build is its manifest's, or while it is still being
 * fetched the one the config named (the folder's marker). */
char *tf__store_json(TfClient *c, long long *models_out, long long *sharing_out) {
  char *dir = models_dir(c);
  char (*names)[256] = (char (*)[256])malloc(64 * 256);
  const int n = dir && names ? tf__list_dir(dir, names, 64) : 0;
  Entry *v = (Entry *)calloc(65, sizeof(Entry));
  int nv = 0;
  long long models = 0;
  for (int i = 0; v && i < n; ++i) {
    if (!is_build(names[i])) continue;
    char *bd = tf_path_join(dir, names[i]), *mf = bd ? tf_path_join(bd, "manifest.json") : NULL, *mk = bd ? tf_path_join(bd, "version") : NULL;
    char *text = NULL, *mark = NULL;
    size_t tn = 0, mn = 0;
    TfJson *j = mf && !tf_read_file(mf, &text, &tn) ? tf_json_parse(text, tn, NULL) : NULL;
    const char *ver = tf_json_str(tf_json_get(j, "version"));
    if (!ver && mk && !tf_read_file(mk, &mark, &mn)) ver = mark;
    if (tf__is_model_version(ver)) {
      snprintf(v[nv].version, sizeof v[nv].version, "%s", ver);
      snprintf(v[nv].build, sizeof v[nv].build, "%s", names[i]);
      v[nv].bytes = tf__dir_bytes(bd);
      models += v[nv].bytes;
      nv++;
    }
    tf_json_free(j);
    free(text);
    free(mark);
    free(mk);
    free(mf);
    free(bd);
  }
  if (v) qsort(v, (size_t)nv, sizeof *v, cmp_entry);
  free(names);
  free(dir);
  const long long sharing = tf__share_bytes(c);
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"models\":[");
  for (int k = 0; k < nv; ++k)
    tf_buf_printf(&b, "%s{\"version\":\"%s\",\"build\":\"%s\",\"bytes\":%lld}", k ? "," : "", v[k].version, v[k].build, v[k].bytes);
  tf_buf_printf(&b, "],\"sharingQueueBytes\":%lld}", sharing);
  free(v);
  if (models_out) *models_out = models;
  if (sharing_out) *sharing_out = sharing;
  return tf_buf_take(&b);
}

/* removes the model files: every build folder and current.json. Never the consent state or the sharing queue. */
int tf__store_remove(TfClient *c) {
  char *dir = models_dir(c);
  if (!dir) return tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  const int rc = tf__remove_tree(dir);
  free(dir);
  return rc ? tf__fail(TF_ERR_STORAGE_FAILED, "the model files cannot be removed (close the Model first)") : 0;
}

/* the build on disk, for the diagnosis: version and build copied out, 1 when there is one */
int tf__store_current(TfClient *c, char version[24], char build[41]) {
  Pick p;
  if (pick_from_disk(c, &p)) return 0;
  snprintf(version, 24, "%s", p.version);
  snprintf(build, 41, "%s", p.build);
  return 1;
}
