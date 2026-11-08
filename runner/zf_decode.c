/* Native streaming decoder: audio in, phonemes out, over the engine's stream API (zf/stream.h) with
 * the int8 linears routed through the native hook (src/zf_vnni_hook.c).
 *
 * Everything about the model comes from the model: the vocabulary size and the blank from the
 * graph (zf_stream_model_info), the chunk geometry (window, advance, step) from the graph, the
 * symbols from tokens.txt. So the same binary decodes TajweedFormer v3.2 (251 tokens, blank 250,
 * 480 ms) and V4 (tj3, 38 tokens, blank 0; 320 or 480 ms by context), and V4's eleven extra outputs
 * (the tajweed heads) are fingerprinted chunk by chunk next to the log-probs, never interpreted.
 *
 *   zf_decode [options] <model_engine2.onnx> <audio> [tokens.txt]
 *     audio: .wav (16 kHz mono PCM16 or float32), .f32 / .pcm (raw float32, 16 kHz)
 *     --tokens FILE     the symbols (or the third positional argument)
 *     --threads N       engine threads for the stream (default 1)
 *     --heads           print every extra output's shape and fingerprint
 *     --quiet           no phoneme list
 *   zf_decode --bench S --seconds T [--threads N] <model_engine2.onnx> <list.txt>
 *     S streams at once, each on its own thread with N engine threads, decoding the clips in
 *     list.txt (one path a line) round robin for T seconds; prints the aggregate real-time factor.
 *
 * The .zfg/.zfg.bin and the .zfw weight files sit next to the model path. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
static double now_s(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
static double rss_gb(void) {
  PROCESS_MEMORY_COUNTERS pmc;
  return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.WorkingSetSize / 1073741824.0 : 0.0;
}
#else
#include <unistd.h>
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double rss_gb(void) {
  long pages = 0; FILE *f = fopen("/proc/self/statm", "r");
  if (f) { long size; if (fscanf(f, "%ld %ld", &size, &pages) != 2) pages = 0; fclose(f); }
  return pages * (double)sysconf(_SC_PAGESIZE) / 1073741824.0;
}
#endif

#include "zf/stream.h"
#include "zf_gemm_vnni.h"

#define RSS_LIMIT_GB 16.0

/* --- tokens.txt: "<symbol> <id>" per line, any vocabulary size --- */
static char **g_tok;
static int g_ntok;
static int load_tokens(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
  char line[256];
  int n = 0;
  while (fgets(line, sizeof line, f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    char *sp = strrchr(line, ' ');
    if (!sp) sp = strrchr(line, '\t');
    if (!sp) continue;
    const int id = atoi(sp + 1);
    *sp = 0;
    if (id < 0 || id > 1 << 20) continue;
    if (id >= g_ntok) {
      char **t = realloc(g_tok, (size_t)(id + 1) * sizeof *t);
      if (!t) break;
      for (int i = g_ntok; i <= id; ++i) t[i] = NULL;
      g_tok = t; g_ntok = id + 1;
    }
    free(g_tok[id]);
    g_tok[id] = strdup(line);
    ++n;
  }
  fclose(f);
  return n;
}
static const char *sym(int id) { return id >= 0 && id < g_ntok && g_tok[id] ? g_tok[id] : "?"; }

/* --- audio: raw float32 (.f32/.pcm) or a PCM16 / float32 mono WAV --- */
static float *read_audio(const char *path, int *n_out) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  const char *dot = strrchr(path, '.');
  if (dot && (!strcmp(dot, ".f32") || !strcmp(dot, ".pcm"))) {
    const int n = (int)(sz / 4);
    float *s = malloc((size_t)n * 4 + 4);
    if (!s || fread(s, 4, n, f) != (size_t)n) { free(s); fclose(f); return NULL; }
    fclose(f); *n_out = n; return s;
  }
  unsigned char h[44];
  if (fread(h, 1, 44, f) != 44 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
    fprintf(stderr, "%s: not a WAV, .f32 or .pcm\n", path); fclose(f); return NULL;
  }
  const int fmt = h[20] | (h[21] << 8), ch = h[22] | (h[23] << 8);
  const int rate = h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24), bits = h[34] | (h[35] << 8);
  long data_off = 44, data_len = sz - 44;
  if (memcmp(h + 36, "data", 4)) {
    fseek(f, 12, SEEK_SET);
    char id[4]; unsigned char ln[4];
    while (fread(id, 1, 4, f) == 4 && fread(ln, 1, 4, f) == 4) {
      long l = ln[0] | (ln[1] << 8) | (ln[2] << 16) | ((long)ln[3] << 24);
      if (!memcmp(id, "data", 4)) { data_off = ftell(f); data_len = l; break; }
      fseek(f, l, SEEK_CUR);
    }
  }
  if (rate != 16000 || ch != 1)
    fprintf(stderr, "warning: WAV is %d Hz %d-ch; expected 16000 Hz mono\n", rate, ch);
  fseek(f, data_off, SEEK_SET);
  int n;
  float *s;
  if (fmt == 3 && bits == 32) {
    n = (int)(data_len / 4); s = malloc((size_t)n * 4 + 4);
    if (!s || fread(s, 4, n, f) != (size_t)n) { free(s); fclose(f); return NULL; }
  } else if (fmt == 1 && bits == 16) {
    n = (int)(data_len / 2); s = malloc((size_t)n * 4 + 4);
    int16_t *tmp = malloc((size_t)n * 2 + 2);
    if (!s || !tmp || fread(tmp, 2, n, f) != (size_t)n) { free(tmp); free(s); fclose(f); return NULL; }
    for (int i = 0; i < n; ++i) s[i] = tmp[i] / 32768.0f;
    free(tmp);
  } else {
    fprintf(stderr, "%s: unsupported WAV (fmt %d bits %d)\n", path, fmt, bits); fclose(f); return NULL;
  }
  fclose(f); *n_out = n; return s;
}

/* --- per-chunk fingerprints of log_probs and of every extra output --- */
#define MAX_OUT 32
typedef struct {
  ZfStream *s;
  uint64_t logp_fnv;
  uint64_t out_fnv[MAX_OUT];
  int out_rows[MAX_OUT], out_cols[MAX_OUT];
  long long chunks;
  int vocab;
} Run;
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}
static void on_chunk(void *user, const float *logp, int frames, int vocab) {
  Run *r = (Run *)user;
  r->logp_fnv = fnv(r->logp_fnv, logp, (size_t)frames * vocab * sizeof(float));
  r->vocab = vocab;
  ++r->chunks;
  if (!r->s) return;
  const int n = zf_stream_output_count(r->s);
  for (int i = 0; i < n && i < MAX_OUT; ++i) {
    int rows = 0, cols = 0;
    const float *v = zf_stream_last_output(r->s, zf_stream_output_name(r->s, i), &rows, &cols);
    if (!v) continue;
    r->out_fnv[i] = fnv(r->out_fnv[i], v, (size_t)rows * cols * sizeof(float));
    r->out_rows[i] = rows; r->out_cols[i] = cols;
  }
}
static void run_init(Run *r) {
  memset(r, 0, sizeof *r);
  r->logp_fnv = 1469598103934665603ull;
  for (int i = 0; i < MAX_OUT; ++i) r->out_fnv[i] = 1469598103934665603ull;
}

/* Decode one clip on a stream: returns phonemes (or -1), tokens in *out (grown as needed). */
static int decode(ZfStream *s, const float *audio, int n, ZfToken **out, int *cap) {
  int total = 0;
  zf_stream_reset(s);
  const int piece = 1600; /* 100 ms pushes, as a live client would */
  for (int off = 0; off <= n; off += piece) {
    const int len = off + piece <= n ? piece : n - off;
    if (len > 0 && zf_stream_push_audio(s, audio + off, len, NULL, 0) < 0) return -1;
  }
  if (zf_stream_finish_audio(s, NULL, 0) < 0) return -1;
  const int pend = zf_stream_pending(s);
  if (pend > *cap) { ZfToken *t = realloc(*out, (size_t)pend * sizeof *t); if (!t) return -1; *out = t; *cap = pend; }
  if (pend > 0) total = zf_stream_read(s, *out, pend);
  return total;
}

/* --- bench: S streams at once --- */
typedef struct {
  const char *model;
  float **clips; int *lens; int nclips;
  int threads, index;
  double stop_at;
  double audio_s, busy_s;
  long long chunks;
  int failed;
} Worker;
static volatile int g_abort;
static void *worker(void *arg) {
  Worker *w = (Worker *)arg;
  ZfStreamOptions opt = zf_stream_default_options();
  opt.threads = w->threads;
  const char *err = NULL;
  ZfStream *s = zf_stream_create(w->model, &opt, &err);
  if (!s) { fprintf(stderr, "stream %d: %s\n", w->index, err ? err : "create failed"); w->failed = 1; return NULL; }
  ZfToken *out = NULL; int cap = 0;
  for (int c = w->index; now_s() < w->stop_at && !g_abort; ++c) {
    const int i = c % w->nclips;
    const double t0 = now_s();
    if (decode(s, w->clips[i], w->lens[i], &out, &cap) < 0) { w->failed = 1; break; }
    w->busy_s += now_s() - t0;
    w->audio_s += w->lens[i] / 16000.0;
    w->chunks += zf_stream_chunks(s);
  }
  free(out);
  zf_stream_destroy(s);
  return NULL;
}

static int bench(const char *model, const char *list, int streams, double seconds, int threads) {
  FILE *f = fopen(list, "r");
  if (!f) { fprintf(stderr, "cannot open %s\n", list); return 1; }
  float **clips = NULL; int *lens = NULL, nclips = 0;
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    if (!len) continue;
    int n = 0;
    float *a = read_audio(line, &n);
    if (!a) continue;
    clips = realloc(clips, (size_t)(nclips + 1) * sizeof *clips);
    lens = realloc(lens, (size_t)(nclips + 1) * sizeof *lens);
    clips[nclips] = a; lens[nclips] = n; ++nclips;
  }
  fclose(f);
  if (!nclips) { fprintf(stderr, "no clips in %s\n", list); return 1; }
  Worker *w = calloc((size_t)streams, sizeof *w);
  pthread_t *th = calloc((size_t)streams, sizeof *th);
  const double t0 = now_s();
  for (int i = 0; i < streams; ++i) {
    w[i].model = model; w[i].clips = clips; w[i].lens = lens; w[i].nclips = nclips;
    w[i].threads = threads; w[i].index = i; w[i].stop_at = t0 + seconds;
    pthread_create(&th[i], NULL, worker, &w[i]);
  }
  /* the RSS guard: a runaway process aborts itself at 16 GB on a shared machine */
  double peak = 0;
  for (;;) {
    const double r = rss_gb();
    if (r > peak) peak = r;
    if (r > RSS_LIMIT_GB) { fprintf(stderr, "RSS %.1f GB over %.0f GB: aborting\n", r, RSS_LIMIT_GB); g_abort = 1; }
    if (now_s() > t0 + seconds || g_abort) break;
#ifdef _WIN32
    Sleep(200);
#else
    usleep(200000);
#endif
  }
  double audio = 0, busy = 0; long long chunks = 0; int failed = 0;
  for (int i = 0; i < streams; ++i) {
    pthread_join(th[i], NULL);
    audio += w[i].audio_s; busy += w[i].busy_s; chunks += w[i].chunks; failed |= w[i].failed;
  }
  const double wall = now_s() - t0;
  printf("{\"streams\": %d, \"engine_threads\": %d, \"int8\": \"%s\", \"clips\": %d, \"wall_s\": %.2f, "
         "\"audio_s\": %.1f, \"chunks\": %lld, \"aggregate_x_realtime\": %.2f, \"per_stream_x_realtime\": %.2f, "
         "\"peak_rss_gb\": %.2f, \"failed\": %d}\n",
         streams, threads, zf_int8_tier_name(zf_int8_hook_tier()), nclips, wall, audio, chunks, audio / wall,
         busy > 0 ? audio / busy : 0.0, peak, failed || g_abort);
  for (int i = 0; i < nclips; ++i) free(clips[i]);
  free(clips); free(lens); free(w); free(th);
  return failed || g_abort;
}

int main(int argc, char **argv) {
  const char *tokens = NULL, *pos[3] = {0};
  int npos = 0, threads = 1, heads = 0, quiet = 0, bench_streams = 0;
  double seconds = 30;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--tokens") && i + 1 < argc) tokens = argv[++i];
    else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--heads")) heads = 1;
    else if (!strcmp(argv[i], "--quiet")) quiet = 1;
    else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench_streams = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
    else if (npos < 3) pos[npos++] = argv[i];
  }
  if (npos < 2) {
    fprintf(stderr, "usage: %s [--tokens tokens.txt] [--threads N] [--heads] [--quiet] <model_engine2.onnx> <audio> [tokens.txt]\n"
                    "       %s --bench STREAMS --seconds T [--threads N] <model_engine2.onnx> <list.txt>\n", argv[0], argv[0]);
    return 2;
  }
  if (threads < 1) threads = 1;
  if (threads > 8) threads = 8;
  if (bench_streams > 0) return bench(pos[0], pos[1], bench_streams, seconds, threads);
  if (!tokens && npos > 2) tokens = pos[2];
  const int have_tokens = tokens && load_tokens(tokens) > 0;

  Run run; run_init(&run);
  ZfStreamOptions opt = zf_stream_default_options();
  opt.on_chunk = on_chunk;
  opt.user = &run;
  opt.threads = threads;
  const char *err = NULL;
  ZfStream *s = zf_stream_create(pos[0], &opt, &err);
  if (!s) { fprintf(stderr, "zf_stream_create: %s\n", err ? err : "failed"); return 1; }
  run.s = s;

  ZfModelInfo info;
  memset(&info, 0, sizeof info);
  if (zf_stream_model_info_sized(s, &info, sizeof info) != 0) { fprintf(stderr, "zf_stream_model_info failed\n"); zf_stream_destroy(s); return 1; }
  if (have_tokens && g_ntok != info.vocab)
    fprintf(stderr, "warning: tokens.txt has %d ids, the model's vocabulary is %d\n", g_ntok, info.vocab);
  const int nout = zf_stream_output_count(s);

  int n = 0;
  float *audio = read_audio(pos[1], &n);
  if (!audio) { zf_stream_destroy(s); return 1; }
  ZfToken *out = NULL; int cap = 0;
  const double t0 = now_s();
  const int total = decode(s, audio, n, &out, &cap);
  const double ms = (now_s() - t0) * 1e3;
  if (total < 0) { fprintf(stderr, "decode error\n"); free(audio); zf_stream_destroy(s); return 1; }

  printf("model: vocab %d, blank %d (%s), window %d, advance %d (%d ms), %d layers, left %d, %d extra outputs\n",
         info.vocab, info.blank, have_tokens ? sym(info.blank) : "-", info.window, info.advance,
         info.advance * 10, info.layers, info.left_context, nout);
  if (!quiet) {
    printf("phonemes (%d):", total);
    for (int i = 0; i < total; ++i) printf(" %s", have_tokens ? sym(out[i].token) : "");
    if (!have_tokens) for (int i = 0; i < total; ++i) printf(" %d", out[i].token);
    printf("\n");
  }
  const double audio_ms = n / 16.0;
  printf("audio %.0f ms, %lld chunks, %d phonemes, decode %.1f ms (%.1fx realtime), %d engine thread(s), int8 %s\n",
         audio_ms, run.chunks, total, ms, audio_ms / ms, threads, zf_int8_tier_name(zf_int8_hook_tier()));
  printf("logp_fnv = %016llx\n", (unsigned long long)run.logp_fnv);
  if (heads)
    for (int i = 0; i < nout && i < MAX_OUT; ++i)
      printf("  %-18s [%d x %d] fnv %016llx\n", zf_stream_output_name(s, i), run.out_rows[i], run.out_cols[i],
             (unsigned long long)run.out_fnv[i]);
  free(out);
  free(audio);
  zf_stream_destroy(s);
  return 0;
}
