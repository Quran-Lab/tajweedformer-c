/* The public Client (include/tajweedformer.h): one per process, the app's link to Quran Lab. It sits in front of
 * the platform client (tf_client.c: token, register, signed config, telemetry, issues), the model files
 * (tf_store.c) and sharing (tf_sharing.c, tf_share.c).
 *
 * tf_client_create never waits for the network: the launch sequence (register once a day, the signed config, the
 * telemetry earlier runs left, the uploads that are due) runs on a thread of the client, which then keeps the
 * 15-minute telemetry timer. tf_client_model is where the app waits: the config (a conditional request), the
 * download on a first run, the verification and the engine. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"
#include "tf_thread.h"

typedef struct {
  TfMutex lock;
  TfThread launch;
  int launch_on;
  TfModel *model;      /* the Model tf_client_model gave out, while it is open */
  int orphan;          /* a Model outlived this Client (tf_client_destroy with it open): its files are not removed here */
  int refs;
  char *storage_json, *diag_json, *items_json;
  char d_version[24], d_build[41], d_context[32], d_reason[48], d_latest[24];
} Api;

static TfClient *g_client; /* the process's one Client */
static volatile char g_client_lock;
static void spin_lock(void) {
  while (__atomic_test_and_set(&g_client_lock, __ATOMIC_ACQUIRE)) {
  }
}
static void spin_unlock(void) { __atomic_clear(&g_client_lock, __ATOMIC_RELEASE); }

static Api *api(TfClient *c) { return c ? (Api *)*tf__client_api_slot(c) : NULL; }

/* The Api's lock, with a count of how often this thread holds it: a refusal that arrives inside tf_client_model
 * or tf_client_prefetch (which hold it over their requests) removes the models without taking it again. */
static _Thread_local int t_api_held;
static void api_lock(Api *a) {
  tf_mutex_lock(&a->lock);
  t_api_held++;
}
static void api_unlock(Api *a) {
  t_api_held--;
  tf_mutex_unlock(&a->lock);
}

/* SPEC 15.3: the server refused the key or the app, so the models leave this device. Now,
 * when no Model is open; else the removal is owed: tf_model_close comes here again once the Model is closed,
 * and the next start removes what is left before anything else. A Model that is open keeps running. */
void tf__client_purge_models(TfClient *c) {
  if (!c || (!*tf__client_gate(c) && !tf__client_purge_owed(c))) return;
  Api *a = api(c);
  if (!a) {
    tf__store_purge(c);
    return;
  }
  const int held = t_api_held > 0;
  if (!held) api_lock(a);
  if (!a->model && !a->orphan) tf__store_purge(c);
  if (!held) api_unlock(a);
}

/* the catalogue code of a failure, remembered on the client for the diagnosis */
static int fail(TfClient *c, int rc) {
  if (rc < 0) {
    rc = tf__raise(rc);
    tf__client_note_error(c, rc);
  }
  return rc;
}

void tf__client_cancel_calls(TfClient *c, int on);
void tf__model_detach(TfModel *m);

static void launch_thread(void *arg) { tf__client_launch((TfClient *)arg); }

/* where the SDK keeps its state when the app names no folder: per user and per app */
static char *default_storage(const char *app_id) {
  char base[1024] = "";
#ifdef _WIN32
  const char *la = getenv("LOCALAPPDATA");
  if (la && *la) snprintf(base, sizeof base, "%s", la);
#else
  const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
  if (xdg && *xdg) snprintf(base, sizeof base, "%s", xdg);
  else if (home && *home) snprintf(base, sizeof base, "%s/.local/share", home);
#endif
  if (!base[0]) return NULL;
  /* the app id as a folder name: anything outside letters, digits, '.', '_' and '-' becomes '_' */
  char safe[200];
  size_t n = 0;
  for (const char *p = app_id; *p && n + 1 < sizeof safe; ++p)
    safe[n++] = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-' ? *p : '_';
  safe[n] = 0;
  char *a = tf_path_join(base, safe), *b = a ? tf_path_join(a, "tajweedformer") : NULL;
  free(a);
  return b;
}

/* NULL in every app: Quran Lab's key. tf__client_create_with_key sets it for one call (tests). */
static _Thread_local const char *t_create_key;

TfClient *tf_client_create(const TfClientOptions *options) {
  tf__clear_error();
  if (tf__runtime_check()) return NULL;
  if (!options || !options->key || !*options->key) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "TfClientOptions.key is required (the desktop key from sdk.quranlab.ai)");
    return NULL;
  }
  if (!options->app_id || !*options->app_id) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "TfClientOptions.app_id is required (the app's id as registered for the key)");
    return NULL;
  }
  if (options->audience && strcmp(options->audience, "general") && strcmp(options->audience, "children")) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "audience %s is not general or children", options->audience);
    return NULL;
  }
  if (options->analytics != TF_ANALYTICS_ON && options->analytics != TF_ANALYTICS_OFF) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "analytics is not TF_ANALYTICS_ON or TF_ANALYTICS_OFF");
    return NULL;
  }
  /* one Client per process (and so one upload queue on one storage folder) */
  spin_lock();
  if (g_client) {
    spin_unlock();
    tf__fail(TF_ERR_CLIENT_EXISTS, NULL);
    return NULL;
  }
  g_client = (TfClient *)(size_t)1; /* the place is taken while this one is made */
  spin_unlock();
  char *own = options->storage && *options->storage ? NULL : default_storage(options->app_id);
  TfClientConfig cfg = tf__client_default_config();
  cfg.key = options->key;
  cfg.app_id = options->app_id;
  cfg.app_version = options->app_version;
  cfg.storage_dir = own ? own : options->storage;
  cfg.telemetry = options->analytics == TF_ANALYTICS_OFF ? 0 : 1;
  cfg.children = options->audience && !strcmp(options->audience, "children");
  cfg.language = options->language;
  cfg.on_consent = options->on_consent;
  cfg.user = options->user;
  cfg.debug_build = options->debug;
  cfg.on_log = options->on_log;
  cfg.log_user = options->user;
  cfg.trusted_key_hex = t_create_key;
  TfClient *c = NULL;
  Api *a = (Api *)calloc(1, sizeof *a);
  if (!cfg.storage_dir) tf__fail(TF_ERR_STORAGE_FAILED, "no storage folder was given and this user has no data folder");
  else if (!a) tf__fail(TF_ERR_ENGINE_FAILED, "out of memory");
  else c = tf__client_open(&cfg);
  free(own);
  if (!c) {
    free(a);
    tf__error_as(TF_ERR_STORAGE_FAILED);
    spin_lock();
    g_client = NULL;
    spin_unlock();
    return NULL;
  }
  tf_mutex_init(&a->lock);
  *tf__client_api_slot(c) = a;
  spin_lock();
  g_client = c;
  spin_unlock();
  if (!tf__client_no_timer && !tf_thread_start(&a->launch, launch_thread, c)) a->launch_on = 1;
  tf__clear_error();
  return c;
}

void tf_client_destroy(TfClient *c) {
  if (!c) return;
  Api *a = api(c);
  if (a) {
    /* the launch thread may be inside a request: it is abandoned, not waited for */
    tf__client_cancel_calls(c, 1);
    if (a->launch_on) tf_thread_join(a->launch);
    api_lock(a);
    if (a->model) {
      tf__model_detach(a->model); /* a Model that outlives its Client keeps running, alone */
      a->orphan = 1;
    }
    a->model = NULL;
    api_unlock(a);
  }
  tf__client_close(c); /* stops the timer, sends what was counted, frees */
  if (a) {
    free(a->storage_json);
    free(a->diag_json);
    free(a->items_json);
    tf_mutex_destroy(&a->lock);
    free(a);
  }
  spin_lock();
  if (g_client == c) g_client = NULL;
  spin_unlock();
}

/* tf_model_close of a Model the Client gave out: 1 while another holder remains (the Model stays open) */
int tf__client_model_release(TfClient *c, TfModel *m) {
  Api *a = api(c);
  if (!a) return 0;
  api_lock(a);
  int keep = 0;
  if (a->model == m) {
    if (--a->refs > 0) keep = 1;
    else a->model = NULL;
  }
  api_unlock(a);
  return keep;
}

/* tests only: the key tf_client_create gives the client for the signed config and the manifest */
TfClient *tf__client_create_with_key(const TfClientOptions *options, const char *trusted_key_hex) {
  t_create_key = trusted_key_hex;
  TfClient *c = tf_client_create(options);
  t_create_key = NULL;
  return c;
}

TfModel *tf_client_model(TfClient *c, const TfModelOptions *options) {
  Api *a = api(c);
  if (!a) {
    tf__raise(tf__null_handle("client"));
    return NULL;
  }
  tf__clear_error();
  api_lock(a);
  if (a->model) {
    /* the Model is open already: the same one, held once more */
    a->refs++;
    TfModel *m = a->model;
    api_unlock(a);
    return m;
  }
  TfFetch f;
  memset(&f, 0, sizeof f);
  f.version = options ? options->version : NULL;
  char dir[1024];
  TfModelOpen open;
  memset(&open, 0, sizeof open);
  TfModel *m = NULL;
  if (tf__store_fetch(c, &f, dir, sizeof dir, &open) == 0) {
    if (options) {
      open.threads = options->threads;
      open.pin = options->pin;
    }
    m = tf__model_open(dir, &open);
  }
  if (m) {
    a->model = m;
    a->refs = 1;
    tf__store_prune(c, tf_model_build(m)); /* builds an update replaced */
  } else {
    fail(c, -1);
  }
  api_unlock(a);
  return m;
}

int tf_client_prefetch(TfClient *c, const TfPrefetchOptions *options) {
  tf__clear_error();
  Api *a = api(c);
  if (!a) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  TfFetch f;
  memset(&f, 0, sizeof f);
  if (options) {
    f.version = options->version;
    f.unmetered_only = options->unmetered_only;
    f.on_progress = options->on_progress;
    f.user = options->user;
  }
  char dir[1024];
  api_lock(a);
  const int rc = tf__store_fetch(c, &f, dir, sizeof dir, NULL);
  api_unlock(a);
  return fail(c, rc);
}

const char *tf_client_storage_json(TfClient *c) {
  tf__clear_error();
  Api *a = api(c);
  if (!a) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
    return NULL;
  }
  api_lock(a);
  free(a->storage_json);
  a->storage_json = tf__store_json(c, NULL, NULL);
  api_unlock(a);
  return a->storage_json;
}

int tf_client_remove_models(TfClient *c) {
  tf__clear_error();
  Api *a = api(c);
  if (!a) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  api_lock(a);
  const int rc = a->model ? tf__fail(TF_ERR_STORAGE_FAILED, "a Model of these files is open: close it first") : tf__store_remove(c);
  api_unlock(a);
  return fail(c, rc);
}

int tf_client_report_checks(TfClient *c, const char *rule, int checked, int flagged) {
  tf__clear_error();
  if (!api(c)) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  /* refused input is a programming error and is never counted (SPEC 6.3) */
  if (!tf__rule_name_ok(rule))
    return fail(c, tf__fail(TF_ERR_INVALID_ARGUMENT, "rule %s is not a lower-case letter followed by at most 23 lower-case letters, digits or underscores",
                            rule ? rule : "(none)"));
  if (flagged < 0 || flagged > checked || checked > 100000)
    return fail(c, tf__fail(TF_ERR_INVALID_ARGUMENT, "checked %d and flagged %d are not 0 <= flagged <= checked <= 100000", checked, flagged));
  tf__client_count_checks(c, rule, checked, flagged); /* a 33rd rule of the day is dropped */
  return 0;
}

int tf_client_background(TfClient *c) {
  tf__clear_error();
  if (!api(c)) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  tf__client_background(c); /* sends the telemetry counted so far; closes the recording that waits for a take */
  tf__clear_error();        /* a send that failed stays queued: not the app's error */
  return 0;
}

int tf_client_foreground(TfClient *c) {
  tf__clear_error();
  if (!api(c)) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  /* portal changes apply within a minute of use: the config is asked again (a conditional request) */
  tf__client_refresh_config(c);
  tf__clear_error();
  return 0;
}

/* ------------------------------------------------------------------ diagnose */

int tf__model_verify(TfModel *m, const char *context);
int tf__store_current(TfClient *c, char version[24], char build[41]);
int tf__client_answered(const TfClient *c);

int tf_client_diagnose(TfClient *c, TfDiagnosis *out) {
  tf__clear_error();
  Api *a = api(c);
  if (!a || !out) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  memset(out, 0, sizeof *out);
  const int last = tf__last_raised(); /* the last error a public call raised in this process */
  /* the key: one request when this client has not heard from the server yet */
  if (strcmp(tf__client_config_state(c), "fresh") && !tf__client_stopped(c)) tf__client_refresh_config(c);
  tf__clear_error();
  api_lock(a);
  out->sdk_version = TF_SDK_VERSION;
  tf__client_check_latest(c);
  if (tf__client_latest(c)) {
    snprintf(a->d_latest, sizeof a->d_latest, "%s", tf__client_latest(c));
    out->sdk_latest = a->d_latest;
  }
  /* the key: refused with the server's reason, accepted once the server answered a request of this client, else
   * not known yet */
  out->key_accepted = -1;
  if (tf__client_stopped(c) && *tf__client_stop_code(c)) {
    const char *code = tf__client_stop_code(c);
    out->key_accepted = 0;
    snprintf(a->d_reason, sizeof a->d_reason, "%s",
             !strcmp(code, "invalid_key") || !strcmp(code, "unauthorized") ? "invalid_key" : !strcmp(code, "sdk_too_old") ? "sdk_too_old" : "server_rejected");
    out->key_reason = a->d_reason;
  } else if (tf__client_answered(c)) {
    out->key_accepted = 1;
  }
  out->config = tf__client_config_state(c);
  /* the model: the open one, else the build on disk, verified now (signature, every file of the context in force) */
  a->d_version[0] = a->d_build[0] = a->d_context[0] = 0;
  if (a->model) {
    snprintf(a->d_version, sizeof a->d_version, "%s", tf_model_version(a->model));
    snprintf(a->d_build, sizeof a->d_build, "%s", tf_model_build(a->model));
    tf__model_context_in_force(a->model, a->d_context, sizeof a->d_context);
    out->model_verified = 1;
  } else if (tf__store_current(c, a->d_version, a->d_build)) {
    char *dir = tf_path_join(tf__client_dir(c), "models"), *bd = dir ? tf_path_join(dir, a->d_build) : NULL;
    TfModelOpen o;
    memset(&o, 0, sizeof o);
    o.no_engine = 1;
    o.client = c; /* for the config's inference.context; nothing is fetched */
    o.trusted_key_hex = tf__client_model_key(c);
    o.want_version = a->d_version;
    o.want_build = a->d_build;
    TfModel *m = bd ? tf__model_open(bd, &o) : NULL;
    if (m) {
      tf__model_context_in_force(m, a->d_context, sizeof a->d_context);
      out->model_verified = tf__model_verify(m, a->d_context) == 0;
      tf_model_close(m);
    }
    tf__clear_error();
    free(bd);
    free(dir);
  }
  out->model_version = a->d_version[0] ? a->d_version : NULL;
  out->model_build = a->d_build[0] ? a->d_build : NULL;
  out->context = a->d_context[0] ? a->d_context : NULL;
  out->backend.kind = "cpu";
  out->backend.name = "cpu";
  out->backend.stage = "cpu";
  out->backend.reason = NULL;
  out->microphone = "none";
  char *storage = tf__store_json(c, &out->storage_models, &out->storage_sharing);
  out->last_error = last ? tf_error_name(last) : NULL;
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{\"sdk\":{\"version\":\"" TF_SDK_VERSION "\",\"latest\":");
  if (out->sdk_latest) tf_buf_json_str(&b, out->sdk_latest);
  else tf_buf_puts(&b, "null");
  tf_buf_printf(&b, "},\"key\":{\"accepted\":%s,\"reason\":", out->key_accepted == 1 ? "true" : out->key_accepted == 0 ? "false" : "null");
  if (out->key_reason) tf_buf_json_str(&b, out->key_reason);
  else tf_buf_puts(&b, "null");
  tf_buf_printf(&b, "},\"config\":\"%s\",\"model\":", out->config);
  if (out->model_version) {
    tf_buf_puts(&b, "{\"version\":");
    tf_buf_json_str(&b, out->model_version);
    tf_buf_puts(&b, ",\"build\":");
    tf_buf_json_str(&b, out->model_build);
    tf_buf_printf(&b, ",\"verified\":%s}", out->model_verified ? "true" : "false");
  } else {
    tf_buf_puts(&b, "null");
  }
  tf_buf_puts(&b, ",\"context\":");
  if (out->context) tf_buf_json_str(&b, out->context);
  else tf_buf_puts(&b, "null");
  /* a Backend only with a model on the device */
  tf_buf_puts(&b, out->model_version ? ",\"backend\":{\"kind\":\"cpu\",\"name\":\"cpu\",\"stage\":\"cpu\",\"reason\":null}" : ",\"backend\":null");
  tf_buf_puts(&b, ",\"microphone\":\"none\",\"storage\":");
  tf_buf_puts(&b, storage ? storage : "{\"models\":[],\"sharingQueueBytes\":0}");
  tf_buf_puts(&b, ",\"lastError\":");
  if (out->last_error) tf_buf_json_str(&b, out->last_error);
  else tf_buf_puts(&b, "null");
  /* chunks come by callback here, so none waits and none is dropped; the kind of connection is not known */
  out->dropped_chunks = 0;
  out->connection = tf__connection_name(tf__connection());
  tf_buf_printf(&b, ",\"droppedChunks\":%d,\"connection\":\"%s\"}", out->dropped_chunks, out->connection);
  free(storage);
  free(a->diag_json);
  a->diag_json = tf_buf_take(&b);
  out->json = a->diag_json;
  api_unlock(a);
  return 0;
}

const char *tf_client_diagnose_json(TfClient *c) {
  TfDiagnosis d;
  return tf_client_diagnose(c, &d) == 0 ? d.json : NULL;
}

/* ------------------------------------------------------------------ the sharing queue as JSON */

const char *tf_sharing_items_json(TfClient *c) {
  tf__clear_error();
  Api *a = api(c);
  if (!a) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
    return NULL;
  }
  char *items = tf__sharing_outbox_json(c);
  api_lock(a);
  free(a->items_json);
  a->items_json = items;
  api_unlock(a);
  if (!items) tf__error_as(TF_ERR_STORAGE_FAILED);
  return items;
}
