/* tajweedformer: the command-line tool and server tier of the TajweedFormer SDK (Windows, Linux).
 *
 *   tajweedformer info     <folder>                             the model's info JSON
 *   tajweedformer decode   <folder> <audio> [--context C] [--heads all|name,name] [--top-k K] [--frames] [--values]
 *                          [--end-on-silence MS] [--threads N] [--pin | --no-pin]
 *   tajweedformer serve    <folder> [options as decode]         JSON lines in: {"audio": "<path>"}; one result line each
 *   tajweedformer bench    <folder> <list.txt> [--workers N] [--threads N] [--pin | --no-pin]   N worker processes
 *   tajweedformer download --key KEY --app-id ID --storage DIR [--version X.Y.Z]   the model for this app, verified
 *   tajweedformer diagnose --key KEY --app-id ID --storage DIR                     the client's report as JSON
 *   (every command that opens a folder: --trusted-key HEX for a folder signed with a test key)
 *
 * A decode line: {"audio","audio_s","compute_s","chunks","heads":[{"name","kind","frame_ms","frames","classes",
 * "values"?}],"greedy":{"<ctc head>":[ids]},"decoded":{...}}. --values adds each head's numbers; --frames the
 * decoded layer's per-frame top-1 and runner-up (and the top K with --top-k).
 *
 * Audio: WAV (PCM 16-bit or float 32-bit, the first channel, any rate 8 to 48 kHz) or raw float32 16 kHz
 * (.pcm/.f32/.raw). One take at a time per process (the engine's rule): the server tier scales with worker
 * processes. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "tajweedformer.h"

static double now_s(void) {
#ifdef _WIN32
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)f.QuadPart;
#else
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
#endif
}

typedef struct {
  const char *folder, *key, *context, *heads;
  int frames, values, top_k, threads, workers, end_ms;
  int pin; /* 0: the engine's default; 1: --pin; -1: --no-pin */
} Opts;

static const char *arg(int argc, char **argv, const char *name) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!strcmp(argv[i], name)) return argv[i + 1];
  return NULL;
}
static int flag(int argc, char **argv, const char *name) {
  for (int i = 1; i < argc; ++i)
    if (!strcmp(argv[i], name)) return 1;
  return 0;
}
static int num(int argc, char **argv, const char *name, int dflt) { return arg(argc, argv, name) ? atoi(arg(argc, argv, name)) : dflt; }
static void parse(Opts *o, int argc, char **argv) {
  memset(o, 0, sizeof *o);
  o->key = arg(argc, argv, "--trusted-key");
  o->context = arg(argc, argv, "--context");
  o->heads = arg(argc, argv, "--heads");
  o->frames = flag(argc, argv, "--frames");
  o->values = flag(argc, argv, "--values");
  o->top_k = num(argc, argv, "--top-k", 0);
  o->threads = num(argc, argv, "--threads", 0);
  o->workers = num(argc, argv, "--workers", 1);
  o->end_ms = num(argc, argv, "--end-on-silence", 0);
  o->pin = flag(argc, argv, "--pin") ? 1 : flag(argc, argv, "--no-pin") ? -1 : 0;
}

static int fail(const char *what) {
  fprintf(stderr, "tajweedformer: %s: %s (%s)\n  %s\n  %s\n", what, tf_last_error(), tf_error_name(tf_last_error_code()), tf_last_error_fix(),
          tf_error_docs(tf_last_error_code()));
  return 1;
}

static TfModel *open_model(const Opts *o) {
  TfModelOptions mo = {0};
  mo.trusted_key_hex = o->key;
  mo.threads = o->threads;
  mo.pin = o->pin;
  TfModel *m = tf_model_open(o->folder, &mo);
  if (!m) exit(fail(o->folder));
  return m;
}

/* ------------------------------------------------------------------ audio */
static float *read_audio(const char *path, int *n, int *rate) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  const long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *d = (unsigned char *)malloc((size_t)sz + 1);
  if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz) {
    fclose(f);
    free(d);
    return NULL;
  }
  fclose(f);
  float *out = NULL;
  if (sz > 44 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) {
    int fmt = 0, ch = 0, bits = 0;
    long at = 12;
    while (at + 8 <= sz) {
      const uint32_t len = d[at + 4] | d[at + 5] << 8 | d[at + 6] << 16 | (uint32_t)d[at + 7] << 24;
      if (!memcmp(d + at, "fmt ", 4)) {
        fmt = d[at + 8] | d[at + 9] << 8;
        ch = d[at + 10] | d[at + 11] << 8;
        *rate = (int)(d[at + 12] | d[at + 13] << 8 | d[at + 14] << 16 | (uint32_t)d[at + 15] << 24);
        bits = d[at + 22] | d[at + 23] << 8;
      } else if (!memcmp(d + at, "data", 4)) {
        const long bytes = (long)len > sz - at - 8 ? sz - at - 8 : (long)len;
        const unsigned char *p = d + at + 8;
        if (ch < 1 || bits < 8) break;
        const int bps = bits / 8;
        *n = (int)(bytes / (bps * ch));
        out = (float *)malloc(sizeof(float) * (size_t)(*n + 1));
        for (int i = 0; out && i < *n; ++i) {
          const unsigned char *s = p + (size_t)i * bps * ch; /* channel 0 */
          if (fmt == 3 && bits == 32) memcpy(&out[i], s, 4);
          else if (bits == 16) out[i] = (int16_t)(s[0] | s[1] << 8) / 32768.0f;
          else out[i] = 0;
        }
        break;
      }
      at += 8 + len + (len & 1);
    }
  } else {
    *rate = 16000;
    *n = (int)(sz / 4);
    out = (float *)malloc((size_t)sz + 4);
    if (out) memcpy(out, d, (size_t)sz);
  }
  free(d);
  return out;
}

static void json_path(FILE *out, const char *s) {
  for (const char *p = s; *p; ++p) fprintf(out, *p == '\\' || *p == '"' ? "\\%c" : "%c", *p);
}

/* one take through a Session, every layer printed as one JSON line */
static int run_take(TfModel *m, const Opts *o, const char *audio, FILE *out, double *audio_s, double *wall_s) {
  int n = 0, rate = 16000;
  float *pcm = read_audio(audio, &n, &rate);
  if (!pcm) {
    fprintf(out, "{\"audio\":\"");
    json_path(out, audio);
    fprintf(out, "\",\"error\":\"cannot read the audio\"}\n");
    fflush(out);
    return 1;
  }
  TfSessionOptions so = {0};
  so.context = o->context;
  so.sample_rate = rate;
  so.heads = o->heads;
  so.top_k = o->top_k;
  so.end_on_silence_ms = o->end_ms;
  TfSession *s = tf_session_create(m, &so);
  int chunks = 0;
  const double t0 = now_s();
  const int step = rate / 50; /* 20 ms a push */
  for (int at = 0; s && at < n; at += step) {
    const int r = tf_session_push(s, pcm + at, n - at < step ? n - at : step);
    if (r < 0) break; /* the take ended on silence (session_closed), or failed */
    chunks += r;
  }
  const int last = s ? tf_session_finish(s) : -1;
  if (last > 0) chunks += last;
  const double t1 = now_s();
  free(pcm);
  const TfHeads *h = s ? tf_session_heads(s) : NULL;
  fprintf(out, "{\"audio\":\"");
  json_path(out, audio);
  if (!h) {
    fprintf(out, "\",\"error\":\"%s\"}\n", tf_error_name(tf_last_error_code()));
    fflush(out);
    tf_session_destroy(s);
    return 1;
  }
  if (audio_s) *audio_s += (double)n / rate;
  if (wall_s) *wall_s += t1 - t0;
  fprintf(out, "\",\"audio_s\":%.3f,\"compute_s\":%.3f,\"chunks\":%d,\"heads\":[", (double)n / rate, t1 - t0, chunks);
  for (int i = 0; i < tf_heads_count(h); ++i) {
    TfHead hd;
    tf_heads_get(h, tf_heads_name(h, i), &hd);
    fprintf(out, "%s{\"name\":\"%s\",\"kind\":\"%s\",\"frame_ms\":%d,\"frames\":%d,\"classes\":%d", i ? "," : "", hd.name, hd.kind, hd.frame_ms,
            hd.frames, hd.classes);
    if (o->values) {
      fprintf(out, ",\"values\":[");
      for (int f = 0; f < hd.frames; ++f) {
        fprintf(out, f ? ",[" : "[");
        for (int k = 0; k < hd.classes; ++k) fprintf(out, k ? ",%.9g" : "%.9g", (double)hd.values[(size_t)f * hd.classes + k]);
        fprintf(out, "]");
      }
      fprintf(out, "]");
    }
    fprintf(out, "}");
  }
  fprintf(out, "],\"greedy\":{");
  for (int i = 0, first = 1; i < tf_heads_count(h); ++i) {
    TfHead hd;
    const TfToken *tk = NULL;
    tf_heads_get(h, tf_heads_name(h, i), &hd);
    if (strcmp(hd.kind, "ctc")) continue;
    const int nt = tf_heads_greedy(h, hd.name, &tk);
    fprintf(out, "%s\"%s\":[", first ? "" : ",", hd.name);
    for (int k = 0; k < nt; ++k) fprintf(out, k ? ",%d" : "%d", tk[k].id);
    fprintf(out, "]");
    first = 0;
  }
  const TfDecoded *d = tf_heads_decoded(h);
  fprintf(out, "},\"decoded\":%s}\n", d ? tf_decoded_json(d, o->frames) : "null");
  fflush(out);
  tf_session_destroy(s);
  return 0;
}

static int cmd_decode(int argc, char **argv, const Opts *o) {
  if (argc < 4) return 2;
  TfModel *m = open_model(o);
  const int rc = run_take(m, o, argv[3], stdout, NULL, NULL);
  tf_model_close(m);
  return rc;
}

/* JSON lines on stdin: {"audio": "<path>"}; one result line each */
static int cmd_serve(const Opts *o) {
  TfModel *m = open_model(o);
  char line[8192];
  while (fgets(line, sizeof line, stdin)) {
    char audio[4096] = "";
    const char *a = strstr(line, "\"audio\"");
    if (a && (a = strchr(a + 7, '"'))) sscanf(a + 1, "%4095[^\"]", audio);
    if (!*audio) {
      printf("{\"error\":\"expected {\\\"audio\\\": path}\"}\n");
      fflush(stdout);
      continue;
    }
    run_take(m, o, audio, stdout, NULL, NULL);
  }
  tf_model_close(m);
  return 0;
}

/* worker: decode every line of a list (one per line), print a summary line */
static int cmd_worker(const Opts *o, const char *list, int shard, int shards) {
  TfModel *m = open_model(o);
  FILE *f = fopen(list, "r");
  if (!f) return 1;
  char line[4096];
  int k = 0, done = 0;
  double audio = 0, wall = 0;
  FILE *sink = fopen(
#ifdef _WIN32
      "NUL",
#else
      "/dev/null",
#endif
      "w");
  while (sink && fgets(line, sizeof line, f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    if (!len || (k++ % shards) != shard) continue;
    run_take(m, o, line, sink, &audio, &wall);
    done++;
  }
  fclose(f);
  if (sink) fclose(sink);
  printf("{\"worker\":%d,\"clips\":%d,\"audio_s\":%.3f,\"compute_s\":%.3f}\n", shard, done, audio, wall);
  tf_model_close(m);
  return 0;
}

static int cmd_bench(int argc, char **argv, const Opts *o) {
  if (argc < 4) return 2;
  const int w = o->workers > 0 ? o->workers : 1;
  char self[4096];
#ifdef _WIN32
  GetModuleFileNameA(NULL, self, sizeof self);
#else
  snprintf(self, sizeof self, "%s", argv[0]);
#endif
  const double t0 = now_s();
  char sh[16], shs[16], th[16];
  snprintf(shs, sizeof shs, "%d", w);
  snprintf(th, sizeof th, "%d", o->threads ? o->threads : 1);
#ifdef _WIN32
  intptr_t pids[64];
#else
  pid_t pids[64];
#endif
  for (int i = 0; i < w && i < 64; ++i) {
    snprintf(sh, sizeof sh, "%d", i);
    const char *av[16];
    int k = 0;
    av[k++] = self;
    av[k++] = "worker";
    av[k++] = argv[2];
    av[k++] = argv[3];
    av[k++] = sh;
    av[k++] = shs;
    av[k++] = "--threads";
    av[k++] = th;
    if (o->pin > 0) av[k++] = "--pin";
    if (o->pin < 0) av[k++] = "--no-pin";
    if (o->key) {
      av[k++] = "--trusted-key";
      av[k++] = o->key;
    }
    if (o->context) {
      av[k++] = "--context";
      av[k++] = o->context;
    }
    av[k] = NULL;
#ifdef _WIN32
    pids[i] = _spawnv(_P_NOWAIT, self, av);
#else
    pids[i] = fork();
    if (pids[i] == 0) {
      execv(self, (char *const *)av);
      _exit(127);
    }
#endif
  }
  for (int i = 0; i < w && i < 64; ++i) {
#ifdef _WIN32
    int st;
    _cwait(&st, pids[i], 0);
#else
    int st;
    waitpid(pids[i], &st, 0);
#endif
  }
  fprintf(stderr, "bench: %d worker processes, %d threads each, wall %.2f s (each worker prints its own audio and compute)\n", w,
          o->threads ? o->threads : 1, now_s() - t0);
  return 0;
}

static void progress(void *u, long long done, long long total) {
  (void)u;
  fprintf(stderr, "\r  %lld / %lld bytes", done, total);
  if (done >= total) fprintf(stderr, "\n");
}

static TfClient *client(int argc, char **argv) {
  TfClientOptions co = {0};
  co.key = arg(argc, argv, "--key");
  co.app_id = arg(argc, argv, "--app-id");
  co.storage = arg(argc, argv, "--storage");
  co.app_version = "cli";
  co.trusted_key_hex = arg(argc, argv, "--trusted-key");
  TfClient *c = tf_client_create(&co);
  if (!c) exit(fail("client"));
  return c;
}

int main(int argc, char **argv) {
  if (argc < 2 || flag(argc, argv, "--help")) {
    fprintf(stderr, "tajweedformer %s: info | decode | serve | bench | download | diagnose (see the comment at the top of cli/tajweedformer.c)\n",
            tf_version());
    return argc < 2 ? 2 : 0;
  }
  Opts o;
  parse(&o, argc, argv);
  const char *c = argv[1];
  if (!strcmp(c, "download")) {
    TfClient *cl = client(argc, argv);
    TfPrefetchOptions po = {0};
    po.version = arg(argc, argv, "--version");
    po.on_progress = progress;
    const int rc = tf_client_prefetch(cl, &po);
    if (rc) fail("download");
    else printf("%s\n", tf_client_storage_json(cl));
    tf_client_destroy(cl);
    return rc ? 1 : 0;
  }
  if (!strcmp(c, "diagnose")) {
    TfClient *cl = client(argc, argv);
    const char *report = tf_client_diagnose_json(cl);
    printf("%s\n", report ? report : "null");
    tf_client_destroy(cl);
    return report ? 0 : 1;
  }
  if (argc < 3) return 2;
  o.folder = argv[2];
  if (!strcmp(c, "info")) {
    TfModel *m = open_model(&o);
    printf("%s\n", tf_model_info_json(m));
    tf_model_close(m);
    return 0;
  }
  if (!strcmp(c, "decode")) return cmd_decode(argc, argv, &o);
  if (!strcmp(c, "serve")) return cmd_serve(&o);
  if (!strcmp(c, "bench")) return cmd_bench(argc, argv, &o);
  if (!strcmp(c, "worker") && argc >= 6) return cmd_worker(&o, argv[3], atoi(argv[4]), atoi(argv[5]));
  fprintf(stderr, "unknown command %s\n", c);
  return 2;
}
