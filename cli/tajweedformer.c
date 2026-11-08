/* tajweedformer: the command-line tool and server tier of the TajweedFormer SDK (Windows, Linux).
 *
 *   tajweedformer info     <folder> [--key HEX | --unsigned]
 *   tajweedformer decode   <folder> <audio> [--ayah S:A] [--context C] [--heads default|all|name,name]
 *                          [--frames] [--settings JSON] [--rate HZ] [--threads N] [--key HEX | --unsigned]
 *   tajweedformer serve    <folder> [options as decode]       JSON lines: {"audio": "...", "ayah": "S:A"} in, results out
 *   tajweedformer bench    <folder> <list.txt> [--workers N] [--threads N] [--pin | --no-pin] [options]   N worker processes
 *   (pinning: the engine's platform default (desktop: not pinned) unless --pin or --no-pin sets it; every command)
 *   tajweedformer download --key KEY --revision REV --dest DIR [--repo R] [--folder F] [--contexts c16_l64,...]
 *   tajweedformer portal   --key KEY --app-id ID --storage DIR [--api URL]   register + verified config
 *
 * Audio: WAV (PCM 16-bit or float 32-bit, mono, any rate 8 to 48 kHz) or raw float32 16 kHz (.pcm/.f32/.raw).
 * One engine stream per process (the engine's rule): the server tier scales with worker processes. */
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
  const char *folder, *key, *context, *settings, *heads, *ayah;
  int unsigned_ok, frames, rate, threads, workers;
  int no_pin; /* -1: the engine's platform default; 0: --pin; 1: --no-pin */
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
static void parse(Opts *o, int argc, char **argv) {
  memset(o, 0, sizeof *o);
  o->key = arg(argc, argv, "--key");
  o->context = arg(argc, argv, "--context");
  o->settings = arg(argc, argv, "--settings");
  o->heads = arg(argc, argv, "--heads");
  o->ayah = arg(argc, argv, "--ayah");
  o->unsigned_ok = flag(argc, argv, "--unsigned");
  o->frames = flag(argc, argv, "--frames");
  o->rate = arg(argc, argv, "--rate") ? atoi(arg(argc, argv, "--rate")) : 0;
  o->threads = arg(argc, argv, "--threads") ? atoi(arg(argc, argv, "--threads")) : 0;
  o->workers = arg(argc, argv, "--workers") ? atoi(arg(argc, argv, "--workers")) : 1;
  /* core pinning: the engine's platform default (engine d0bb2b3: Windows, Linux and macOS do not pin, which is
   * what a server running many streams wants) unless --pin (0) or --no-pin (1) sets it */
  o->no_pin = flag(argc, argv, "--pin") ? 0 : flag(argc, argv, "--no-pin") ? 1 : -1;
}

static unsigned heads_mask(const char *s) {
  if (!s || !strcmp(s, "default")) return 0;
  if (!strcmp(s, "all")) return TF_HEADS_ALL;
  unsigned m = 0;
  char buf[512];
  snprintf(buf, sizeof buf, "%s", s);
  for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
    const int h = tf_head_from_name(t);
    if (h < 0) {
      fprintf(stderr, "unknown head %s\n", t);
      exit(2);
    }
    m |= TF_HEAD_BIT(h);
  }
  return m | TF_HEADS_DEFAULT;
}

static TfModel *open_model(const Opts *o, int rate, int engine) {
  TfModelOptions mo = tf_model_default_options();
  mo.trusted_key_hex = o->key;
  mo.require_signature = !o->unsigned_ok;
  mo.context = o->context;
  mo.threads = o->threads;
  mo.sample_rate = rate == 16000 ? 0 : rate;
  mo.load_engine = engine;
  mo.no_pin = o->no_pin;
  TfModel *m = tf_model_open(o->folder, &mo);
  if (!m) {
    fprintf(stderr, "tajweedformer: %s (%s)\n", tf_last_error(), tf_error_name(tf_last_error_code()));
    exit(1);
  }
  if (o->settings && tf_settings_set_app_json(m, o->settings)) {
    fprintf(stderr, "tajweedformer: settings: %s\n", tf_last_error());
    exit(2);
  }
  return m;
}

/* ------------------------------------------------------------------ audio */
static float *read_audio(const char *path, int *n, int *rate) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  const long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *d = (unsigned char *)malloc((size_t)sz);
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
        if (ch < 1) break;
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

/* one take through the stream, every layer printed as one JSON line */
static int run_take(TfStream *s, const char *audio, const char *ayah, int frames, FILE *out, double *audio_s,
                    double *wall_s) {
  int n = 0, rate = 16000;
  float *pcm = read_audio(audio, &n, &rate);
  if (!pcm) {
    fprintf(out, "{\"audio\":\"%s\",\"error\":\"cannot read the audio\"}\n", audio);
    return 1;
  }
  tf_stream_reset(s);
  const double t0 = now_s();
  const int step = rate / 50;
  for (int at = 0; at < n; at += step)
    if (tf_stream_push_audio(s, pcm + at, n - at < step ? n - at : step) < 0) break;
  tf_stream_finish(s);
  const double t1 = now_s();
  free(pcm);
  if (audio_s) *audio_s += (double)n / rate;
  if (wall_s) *wall_s += t1 - t0;
  TfDecoded *d = tf_stream_decode(s);
  fprintf(out, "{\"audio\":\"");
  for (const char *p = audio; *p; ++p) fprintf(out, *p == '\\' || *p == '"' ? "\\%c" : "%c", *p);
  fprintf(out, "\",\"audio_s\":%.3f,\"compute_s\":%.3f,\"decoded\":%s", (double)n / rate, t1 - t0, d ? tf_decoded_json(d, frames) : "null");
  int su = 0, ay = 0;
  if (ayah && sscanf(ayah, "%d:%d", &su, &ay) == 2) {
    TfResult *r = tf_stream_score(s, su, ay);
    fprintf(out, ",\"tajweed\":%s", r ? tf_result_json(r) : "null");
    if (!r) fprintf(out, ",\"error\":\"%s\"", tf_error_name(tf_last_error_code()));
    tf_result_free(r);
  }
  fprintf(out, "}\n");
  fflush(out);
  tf_decoded_free(d);
  return 0;
}

static int cmd_decode(int argc, char **argv, const Opts *o) {
  if (argc < 4) return 2;
  int n = 0, rate = o->rate ? o->rate : 16000;
  float *probe = read_audio(argv[3], &n, &rate);
  if (!probe) {
    fprintf(stderr, "cannot read %s\n", argv[3]);
    return 1;
  }
  free(probe);
  TfModel *m = open_model(o, rate, 1);
  TfStreamOptions so = tf_stream_default_options();
  so.heads = heads_mask(o->heads);
  TfStream *s = tf_stream_create(m, &so);
  if (!s) {
    fprintf(stderr, "%s\n", tf_last_error());
    return 1;
  }
  const int rc = run_take(s, argv[3], o->ayah, o->frames, stdout, NULL, NULL);
  tf_stream_destroy(s);
  tf_model_close(m);
  return rc;
}

/* JSON lines on stdin: {"audio": "<path>", "ayah": "S:A"}; one result line each (16 kHz or WAV input) */
static int cmd_serve(const Opts *o) {
  TfModel *m = open_model(o, o->rate ? o->rate : 16000, 1);
  TfStreamOptions so = tf_stream_default_options();
  so.heads = heads_mask(o->heads);
  TfStream *s = tf_stream_create(m, &so);
  if (!s) return 1;
  char line[8192];
  while (fgets(line, sizeof line, stdin)) {
    char audio[4096] = "", ayah[32] = "";
    const char *a = strstr(line, "\"audio\""), *y = strstr(line, "\"ayah\"");
    if (a && (a = strchr(a + 7, '"'))) sscanf(a + 1, "%4095[^\"]", audio);
    if (y && (y = strchr(y + 6, '"'))) sscanf(y + 1, "%31[^\"]", ayah);
    if (!*audio) {
      printf("{\"error\":\"expected {\\\"audio\\\": path}\"}\n");
      fflush(stdout);
      continue;
    }
    run_take(s, audio, *ayah ? ayah : o->ayah, o->frames, stdout, NULL, NULL);
  }
  tf_stream_destroy(s);
  tf_model_close(m);
  return 0;
}

/* worker: decode every line of a list (one per line), print a summary line */
static int cmd_worker(const Opts *o, const char *list, int shard, int shards) {
  TfModel *m = open_model(o, 16000, 1);
  TfStream *s = tf_stream_create(m, NULL);
  if (!s) return 1;
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
  while (fgets(line, sizeof line, f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    if (!len || (k++ % shards) != shard) continue;
    run_take(s, line, o->ayah, 0, sink, &audio, &wall);
    done++;
  }
  fclose(f);
  if (sink) fclose(sink);
  printf("{\"worker\":%d,\"clips\":%d,\"audio_s\":%.3f,\"compute_s\":%.3f}\n", shard, done, audio, wall);
  tf_stream_destroy(s);
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
    if (o->unsigned_ok) av[k++] = "--unsigned";
    if (o->no_pin == 0) av[k++] = "--pin";
    if (o->no_pin == 1) av[k++] = "--no-pin";
    if (o->key) {
      av[k++] = "--key";
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

static void progress(void *u, const char *path, long long done, long long total) {
  (void)u;
  fprintf(stderr, "  %s %lld/%lld\n", path, done, total);
}

int main(int argc, char **argv) {
  if (argc < 2 || flag(argc, argv, "--help")) {
    fprintf(stderr, "tajweedformer %s: info | decode | serve | bench | download | portal (see the comment at the top of cli/tajweedformer.c)\n",
            tf_version());
    return argc < 2 ? 2 : 0;
  }
  Opts o;
  parse(&o, argc, argv);
  const char *c = argv[1];
  if (!strcmp(c, "download")) {
    TfDownloadOptions d = tf_download_default_options();
    d.key = arg(argc, argv, "--key");
    d.revision = arg(argc, argv, "--revision");
    d.dest = arg(argc, argv, "--dest");
    if (arg(argc, argv, "--repo")) d.repo = arg(argc, argv, "--repo");
    if (arg(argc, argv, "--folder")) d.folder = arg(argc, argv, "--folder");
    if (arg(argc, argv, "--cdn")) d.cdn_url = arg(argc, argv, "--cdn");
    d.contexts = arg(argc, argv, "--contexts");
    d.progress = progress;
    const int rc = tf_download_model(&d);
    if (rc) fprintf(stderr, "download: %s\n", tf_last_error());
    return rc ? 1 : 0;
  }
  if (!strcmp(c, "portal")) {
    TfClientOptions co = tf_client_default_options();
    co.key = arg(argc, argv, "--key");
    co.app_id = arg(argc, argv, "--app-id");
    co.storage_dir = arg(argc, argv, "--storage");
    co.api_url = arg(argc, argv, "--api");
    co.app_version = "cli";
    co.trusted_key_hex = arg(argc, argv, "--config-key");
    TfClient *cl = tf_client_create(&co);
    if (!cl) {
      fprintf(stderr, "portal: %s\n", tf_last_error());
      return 1;
    }
    const int rc = tf_client_launch(cl);
    if (rc) fprintf(stderr, "portal: %s\n", tf_last_error());
    const char *cfg = tf_client_config_json(cl);
    printf("%s\n", cfg ? cfg : "null");
    tf_client_destroy(cl);
    return rc ? 1 : 0;
  }
  if (argc < 3) return 2;
  o.folder = argv[2];
  if (!strcmp(c, "info")) {
    TfModel *m = open_model(&o, 16000, 0);
    printf("%s\n", tf_model_info_json(m));
    printf("%s\n", tf_settings_json(m));
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
