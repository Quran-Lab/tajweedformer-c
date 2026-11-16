/* tajweedformer-eval: evaluate the TajweedFormer model on your own recordings, on your own machine (SDK 3.1.4;
 * Windows and Linux). It uses the public C API only (include/tajweedformer.h) with an app's desktop key, as any app
 * does: the model is downloaded once into the SDK's folder, verified against its signed manifest, and run by the
 * SDK. No raw weights are needed: the tool writes its JSON lines and nothing else. Once the model is downloaded it
 * runs without a network.
 *
 *   tajweedformer-eval [--key KEY] --app-id ID <input> [--refs FILE] [--context C] [--threads N] [--heads LIST]
 *                      [--out FILE] [--storage DIR] [--version X.Y.Z]
 *
 *   <input>          a folder (every .wav, .flac, .pcm, .f32 and .raw file in it and its subfolders, in name order;
 *                    names starting with a dot are skipped), a list file (.txt or .lst: one audio path per line,
 *                    relative to the list's folder; empty lines and lines starting with # are skipped), or one file
 *   --key KEY        the app's desktop key from sdk.quranlab.ai; default: the QURANLAB_KEY environment variable
 *   --app-id ID      the app's id as registered for the key
 *   --refs FILE      references, UTF-8: CSV (file,reference; a first line naming the columns is skipped) or JSONL
 *                    ({"file": ..., "reference": ...} per line). A reference is an ayah range, "S:A", "S:A-B" or
 *                    "S:A-S2:B", whose expected units come from the model's own reference data, or the expected units
 *                    written as the transcript's text writes them (unit names separated by spaces, "|" between words).
 *                    A file is matched by its path as listed, else by its file name, else by its name without the
 *                    extension
 *   --context C      the inference context (the model info's "contexts"); default: the one in force for the app
 *   --threads N      engine threads; default 0: the fast cores
 *   --heads LIST     the heads to compute, comma-separated, or "all"; default: the SDK's default heads
 *   --out FILE       the JSON lines go to FILE (default: standard output)
 *   --storage DIR    the SDK's folder; default: the app's folder for this app id, as for any app
 *   --version X.Y.Z  pin a model version
 *
 * Audio: WAV (PCM 8, 16, 24 or 32-bit, float 32 or 64-bit, WAVE_FORMAT_EXTENSIBLE), FLAC (any bit depth up to 32,
 * every subframe type) and raw float32 at 16 kHz (.pcm, .f32, .raw), any number of channels (averaged to one), at
 * any rate the SDK takes (8 to 48 kHz; the SDK resamples). Other formats: convert first (ffmpeg -i in.mp3 out.flac).
 * A take holds 20 minutes of audio: a longer file is evaluated on its first 20 minutes and its line says
 * "limit": true. One take runs at a time (the engine's rule): for more at once, run one process per file list, each
 * with its own --storage.
 *
 * Output: JSON lines, plain ASCII (other characters as \u escapes, which every JSON reader decodes). One line per file:
 *   {"file", "duration_s", "sample_rate", "channels", "limit", "processing_s", "rtf",
 *    "transcript": {"text", "units": [{"id", "unit", "start_ms", "end_ms", "confidence"}],
 *                   "words": [{"text", "start_ms", "end_ms", "confidence"}]},
 *    "heads": {"<head>": ...},  every head computed besides log_probs (whose output is the transcript): a ctc head
 *             {"frame_ms", "frames", "units"} (its greedy unit count), a class head {"frame_ms", "frames",
 *             "classes": {"<class>": {"frames", "mean"}}} (frames where the class is the most likely one, its mean
 *             probability), boundary and conf {"frame_ms", "frames", "mean", "frames_over_half"} (of the sigmoid)
 *    "reference": {"source", "text", "units", "words"},
 *    "errors": {"units": {"ref", "hyp", "sub", "del", "ins", "rate"}, "words": {"ref", "wrong", "rate"}}   with one
 *   } or {"file", "error", "code"} when the file could not be read or run, and with a reference that cannot be used
 *   "reference_error" in place of "reference" and "errors". The last line is {"total": {...}}: the files, the audio
 *   and processing seconds, the sums of the error counts over the files with a reference and their rates, the SDK
 *   and model versions, the context and the threads.
 *
 * Error rates. The units are the model's own (tf_model_tokens: the consonant letters, the vowels and <foreign>; the
 * word bar and the blank are not units).
 *   unit error rate (UER, a phoneme error rate) = (sub + del + ins) / ref: the Levenshtein distance between the
 *     reference's units and the transcript's (the fewest substitutions, deletions and insertions), over the
 *     reference's units. Of the alignments with the fewest edits, the split is the one that, filling the table from
 *     the start, prefers a match or substitution, then a deletion, then an insertion.
 *   word error rate (WER) = wrong / ref over the reference's words: on that same alignment, a word is wrong when one
 *     of its units is substituted or deleted or an inserted unit follows one of its units (an insertion before the
 *     first unit counts against the first word). The words are the reference's (an ayah's words in the reference
 *     data, or the "|" of a units reference): V4 emits no word bars, so its transcript is one word per take.
 * "rate" is null when ref is 0. The total line sums the counts over the files and divides once (a corpus rate, not a
 * mean of the files' rates).
 *
 * Exit status: 0 when every file was evaluated, 1 when some file could not be (its line has "error"), 2 when nothing
 * ran (the arguments, the references, or no model). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "tajweedformer.h"

#define EV_MAX_S (20 * 60) /* a take holds 20 minutes of audio */

static double ev_now(void) {
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

static void *ev_alloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) {
    fprintf(stderr, "tajweedformer-eval: out of memory\n");
    exit(2);
  }
  return q;
}
static char *ev_strdup(const char *s) {
  const size_t n = strlen(s);
  char *d = (char *)ev_alloc(NULL, n + 1);
  memcpy(d, s, n + 1);
  return d;
}

/* ------------------------------------------------------------------ text and JSON */
typedef struct {
  char *p;
  size_t n, cap;
} EvBuf;
static void ev_put(EvBuf *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    size_t c = b->cap ? b->cap : 256;
    while (c < b->n + n + 1) c *= 2;
    b->p = (char *)ev_alloc(b->p, c);
    b->cap = c;
  }
  if (n) memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}
static void ev_puts(EvBuf *b, const char *s) { ev_put(b, s, strlen(s)); }
static void ev_printf(EvBuf *b, const char *fmt, ...) {
  char tmp[256];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof tmp) {
    ev_put(b, tmp, (size_t)n);
    return;
  }
  char *big = (char *)ev_alloc(NULL, (size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  ev_put(b, big, (size_t)n);
  free(big);
}
/* a JSON string in ASCII: UTF-8 beyond it as \u escapes (a pair above U+FFFF); a byte that is not UTF-8 is U+FFFD */
static void ev_json_str(EvBuf *b, const char *s) {
  ev_put(b, "\"", 1);
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  while (*p) {
    const unsigned c = *p;
    if (c < 0x80) {
      if (c == '"' || c == '\\') {
        const char e[2] = {'\\', (char)c};
        ev_put(b, e, 2);
      } else if (c < 0x20) {
        ev_printf(b, "\\u%04x", c);
      } else {
        ev_put(b, (const char *)p, 1);
      }
      p++;
      continue;
    }
    unsigned cp;
    int len;
    if ((c & 0xE0) == 0xC0) cp = c & 0x1F, len = 2;
    else if ((c & 0xF0) == 0xE0) cp = c & 0x0F, len = 3;
    else if ((c & 0xF8) == 0xF0) cp = c & 0x07, len = 4;
    else cp = 0xFFFD, len = 1;
    for (int k = 1; k < len; ++k) {
      if ((p[k] & 0xC0) != 0x80) { /* also the string's end */
        cp = 0xFFFD;
        len = 1;
        break;
      }
      cp = cp << 6 | (p[k] & 0x3F);
    }
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
      cp -= 0x10000;
      ev_printf(b, "\\u%04x\\u%04x", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
    } else {
      ev_printf(b, "\\u%04x", cp > 0xFFFF ? 0xFFFD : cp);
    }
    p += len;
  }
  ev_put(b, "\"", 1);
}
static void ev_json_num(EvBuf *b, double v, const char *fmt) {
  if (isfinite(v)) ev_printf(b, fmt, v);
  else ev_puts(b, "null");
}
static void ev_utf8(EvBuf *b, unsigned cp) {
  char o[4];
  if (cp < 0x80) {
    o[0] = (char)cp;
    ev_put(b, o, 1);
  } else if (cp < 0x800) {
    o[0] = (char)(0xC0 | cp >> 6), o[1] = (char)(0x80 | (cp & 0x3F));
    ev_put(b, o, 2);
  } else if (cp < 0x10000) {
    o[0] = (char)(0xE0 | cp >> 12), o[1] = (char)(0x80 | (cp >> 6 & 0x3F)), o[2] = (char)(0x80 | (cp & 0x3F));
    ev_put(b, o, 3);
  } else {
    o[0] = (char)(0xF0 | cp >> 18), o[1] = (char)(0x80 | (cp >> 12 & 0x3F)), o[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    ev_put(b, o, 4);
  }
}
static const char *ev_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  return p;
}
static int ev_hex4(const char *p, unsigned *out) {
  unsigned v = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return -1;
  }
  *out = v;
  return 0;
}
/* a JSON string at p (its opening quote) into out, as UTF-8; the text after it, or NULL */
static const char *ev_jstr(const char *p, EvBuf *out) {
  ev_put(out, "", 0);
  if (*p != '"') return NULL;
  for (p++; *p && *p != '"'; p++) {
    if (*p != '\\') {
      ev_put(out, p, 1);
      continue;
    }
    p++;
    unsigned cp;
    switch (*p) {
      case '"': case '\\': case '/': ev_put(out, p, 1); break;
      case 'b': ev_put(out, "\b", 1); break;
      case 'f': ev_put(out, "\f", 1); break;
      case 'n': ev_put(out, "\n", 1); break;
      case 'r': ev_put(out, "\r", 1); break;
      case 't': ev_put(out, "\t", 1); break;
      case 'u':
        if (ev_hex4(p + 1, &cp)) return NULL;
        p += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
          unsigned lo;
          if (!ev_hex4(p + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
          }
        }
        ev_utf8(out, cp);
        break;
      default: return NULL;
    }
  }
  return *p == '"' ? p + 1 : NULL;
}
/* any JSON value: the text after it, or NULL */
static const char *ev_jskip(const char *p) {
  p = ev_ws(p);
  EvBuf t = {0};
  if (*p == '"') {
    p = ev_jstr(p, &t);
    free(t.p);
    return p;
  }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (*p) {
      if (*p == '"') {
        p = ev_jstr(p, &t);
        if (!p) break;
        continue;
      }
      if (*p == '{' || *p == '[') depth++;
      else if ((*p == '}' || *p == ']') && --depth == 0) {
        free(t.p);
        return p + 1;
      }
      p++;
    }
    free(t.p);
    return NULL;
  }
  const char *s = p;
  while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
  return p > s ? p : NULL;
}

/* ------------------------------------------------------------------ files */
static int ev_absolute(const char *p) {
  return p[0] == '/' || p[0] == '\\' || (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':');
}
static char *ev_join(const char *dir, const char *name) {
  if (!dir || !*dir || ev_absolute(name)) return ev_strdup(name);
  const size_t a = strlen(dir), b = strlen(name);
  char *s = (char *)ev_alloc(NULL, a + b + 2);
  memcpy(s, dir, a);
  size_t n = a;
  if (dir[a - 1] != '/' && dir[a - 1] != '\\') s[n++] = '/';
  memcpy(s + n, name, b + 1);
  return s;
}
static const char *ev_base(const char *p) {
  const char *b = p;
  for (const char *q = p; *q; ++q)
    if (*q == '/' || *q == '\\') b = q + 1;
  return b;
}
static int ev_ext_is(const char *p, const char *ext) {
  const char *d = strrchr(ev_base(p), '.');
  if (!d || strlen(d) != strlen(ext)) return 0;
  for (size_t i = 0; d[i]; ++i)
    if ((d[i] | 32) != ext[i]) return 0;
  return 1;
}
static int ev_audio_name(const char *p) {
  return ev_ext_is(p, ".wav") || ev_ext_is(p, ".wave") || ev_ext_is(p, ".flac") || ev_ext_is(p, ".pcm") || ev_ext_is(p, ".f32") ||
         ev_ext_is(p, ".raw");
}

#ifdef _WIN32
static wchar_t *ev_wide(const char *s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = (wchar_t *)ev_alloc(NULL, sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
  if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
  return w;
}
static char *ev_narrow(const wchar_t *w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  char *s = (char *)ev_alloc(NULL, (size_t)(n > 0 ? n : 1));
  if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) s[0] = 0;
  return s;
}
/* paths are UTF-8 everywhere in this tool; Windows opens them as UTF-16 */
static FILE *ev_fopen(const char *path, const char *mode) {
  wchar_t *w = ev_wide(path), m[8];
  int i = 0;
  for (; mode[i] && i < 7; ++i) m[i] = (wchar_t)mode[i];
  m[i] = 0;
  FILE *f = _wfopen(w, m);
  free(w);
  return f;
}
static int ev_is_dir(const char *path) {
  wchar_t *w = ev_wide(path);
  const DWORD a = GetFileAttributesW(w);
  free(w);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
#else
static FILE *ev_fopen(const char *path, const char *mode) { return fopen(path, mode); }
static int ev_is_dir(const char *path) {
  struct stat st;
  return !stat(path, &st) && S_ISDIR(st.st_mode);
}
#endif

static int ev_skip(FILE *f, uint64_t n) {
  while (n) {
    const long step = n > (1u << 30) ? (long)(1u << 30) : (long)n;
    if (fseek(f, step, SEEK_CUR)) return -1;
    n -= (uint64_t)step;
  }
  return 0;
}
static int ev_read_text(const char *path, char **out) {
  FILE *f = ev_fopen(path, "rb");
  if (!f) return -1;
  EvBuf b = {0};
  char tmp[65536];
  size_t n;
  ev_put(&b, "", 0);
  while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) ev_put(&b, tmp, n);
  fclose(f);
  /* a UTF-8 byte order mark is not text */
  if (b.n >= 3 && !memcmp(b.p, "\xEF\xBB\xBF", 3)) memmove(b.p, b.p + 3, b.n - 2), b.n -= 3;
  *out = b.p;
  return 0;
}

typedef struct {
  char **v;
  int n, cap;
} EvList;
static void ev_list_add(EvList *l, char *s) {
  if (l->n == l->cap) {
    l->cap = l->cap ? 2 * l->cap : 64;
    l->v = (char **)ev_alloc(l->v, sizeof(char *) * (size_t)l->cap);
  }
  l->v[l->n++] = s;
}
static int ev_strcmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
/* the audio files under root/rel, as paths relative to root with "/" */
static void ev_walk(const char *root, const char *rel, EvList *out, int depth) {
  if (depth > 32) return;
  char *dir = *rel ? ev_join(root, rel) : ev_strdup(root);
#ifdef _WIN32
  char *pat = ev_join(dir, "*");
  wchar_t *w = ev_wide(pat);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(w, &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      char *name = ev_narrow(fd.cFileName);
      if (name[0] && name[0] != '.') {
        char *r = *rel ? ev_join(rel, name) : ev_strdup(name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          ev_walk(root, r, out, depth + 1);
          free(r);
        } else if (ev_audio_name(name)) {
          ev_list_add(out, r);
        } else {
          free(r);
        }
      }
      free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  free(w);
  free(pat);
#else
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char *r = *rel ? ev_join(rel, e->d_name) : ev_strdup(e->d_name);
    char *full = ev_join(root, r);
    if (ev_is_dir(full)) {
      ev_walk(root, r, out, depth + 1);
      free(r);
    } else if (ev_audio_name(e->d_name)) {
      ev_list_add(out, r);
    } else {
      free(r);
    }
    free(full);
  }
  if (d) closedir(d);
#endif
  free(dir);
}

/* ------------------------------------------------------------------ audio */
typedef struct {
  float *x;
  long long n, cap, max; /* max: the samples kept (20 minutes and one second: the take shows that it was cut) */
  int rate, channels;
} EvAudio;
static void ev_audio_start(EvAudio *a, int rate, int channels) {
  a->rate = rate;
  a->channels = channels;
  a->max = (long long)EV_MAX_S * rate + rate;
}
static void ev_add(EvAudio *a, float v) {
  if (a->n == a->cap) {
    a->cap = a->cap ? 2 * a->cap : 1 << 16;
    if (a->cap > a->max) a->cap = a->max;
    a->x = (float *)ev_alloc(a->x, sizeof(float) * (size_t)a->cap);
  }
  a->x[a->n++] = v;
}
static uint32_t ev_le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t ev_le32(const uint8_t *p) { return ev_le16(p) | ev_le16(p + 2) << 16; }

/* WAV: the chunks after "RIFF....WAVE" */
static int ev_wav(FILE *f, EvAudio *a, char *err, size_t en) {
  int fmt = 0, ch = 0, bits = 0, have_fmt = 0;
  uint32_t rate = 0;
  for (;;) {
    uint8_t c[8];
    if (fread(c, 1, 8, f) != 8) {
      snprintf(err, en, "WAV without a data chunk");
      return -1;
    }
    const uint32_t len = ev_le32(c + 4);
    if (!memcmp(c, "fmt ", 4)) {
      uint8_t q[40];
      memset(q, 0, sizeof q);
      const size_t take = len < sizeof q ? len : sizeof q;
      if (len < 16 || fread(q, 1, take, f) != take || ev_skip(f, (uint64_t)(len - take) + (len & 1))) {
        snprintf(err, en, "WAV with a bad fmt chunk");
        return -1;
      }
      fmt = (int)ev_le16(q);
      ch = (int)ev_le16(q + 2);
      rate = ev_le32(q + 4);
      bits = (int)ev_le16(q + 14);
      if (fmt == 0xFFFE && len >= 26) fmt = (int)ev_le16(q + 24); /* WAVE_FORMAT_EXTENSIBLE: the sub-format's code */
      have_fmt = 1;
    } else if (!memcmp(c, "data", 4)) {
      if (!have_fmt) {
        snprintf(err, en, "WAV data before its fmt chunk");
        return -1;
      }
      const int ok = (fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && (bits == 32 || bits == 64));
      if (!ok || ch < 1 || ch > 64 || !rate || rate > 1000000) {
        snprintf(err, en, "WAV format %d with %d bits, %d channels at %u Hz is not one this tool reads (PCM 8 to 32-bit, float 32 or 64)", fmt, bits,
                 ch, (unsigned)rate);
        return -1;
      }
      ev_audio_start(a, (int)rate, ch);
      const int bps = bits / 8, frame = bps * ch;
      /* 0 and 0xFFFFFFFF: a stream whose writer did not know the length; read to the file's end */
      long long frames = len == 0 || len == 0xFFFFFFFFu ? LLONG_MAX : (long long)(len / (uint32_t)frame);
      uint8_t *buf = (uint8_t *)ev_alloc(NULL, (size_t)frame * 4096);
      while (frames > 0 && a->n < a->max) {
        const size_t want = frames < 4096 ? (size_t)frames : 4096;
        const size_t got = fread(buf, (size_t)frame, want, f);
        for (size_t i = 0; i < got && a->n < a->max; ++i) {
          double sum = 0;
          for (int k = 0; k < ch; ++k) {
            const uint8_t *s = buf + i * (size_t)frame + (size_t)k * bps;
            double v;
            if (fmt == 3 && bits == 32) {
              float x;
              memcpy(&x, s, 4);
              v = x;
            } else if (fmt == 3) {
              double x;
              memcpy(&x, s, 8);
              v = x;
            } else if (bits == 8) {
              v = (s[0] - 128) / 128.0;
            } else if (bits == 16) {
              v = (int16_t)ev_le16(s) / 32768.0;
            } else if (bits == 24) {
              v = (int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) / 2147483648.0;
            } else {
              v = (int32_t)ev_le32(s) / 2147483648.0;
            }
            sum += v;
          }
          ev_add(a, (float)(sum / ch));
        }
        if (got < want) break;
        frames -= (long long)got;
      }
      free(buf);
      return 0;
    } else if (ev_skip(f, (uint64_t)len + (len & 1))) {
      snprintf(err, en, "WAV cut short");
      return -1;
    }
  }
}

/* FLAC (the native container; RFC 9639): a bit reader over the file */
typedef struct {
  FILE *f;
  uint8_t buf[1 << 16];
  size_t n, pos;
  uint64_t acc;
  int bits, eof;
} EvBits;
static int ev_fill(EvBits *b) {
  if (b->pos == b->n) {
    b->n = fread(b->buf, 1, sizeof b->buf, b->f);
    b->pos = 0;
    if (!b->n) {
      b->eof = 1;
      return -1;
    }
  }
  b->acc = b->acc << 8 | b->buf[b->pos++];
  b->bits += 8;
  return 0;
}
static uint32_t ev_bits(EvBits *b, int n) {
  if (n <= 0) return 0;
  while (b->bits < n)
    if (ev_fill(b)) { /* past the end: zeros, and eof says so */
      b->acc <<= 8;
      b->bits += 8;
    }
  b->bits -= n;
  return (uint32_t)(b->acc >> b->bits & (n == 32 ? 0xFFFFFFFFull : (1ull << n) - 1));
}
static int64_t ev_sbits(EvBits *b, int n) { /* n up to 33 (a side channel of 32-bit audio) */
  if (n <= 0) return 0;
  uint64_t v = n > 32 ? (uint64_t)ev_bits(b, n - 32) << 32 | ev_bits(b, 32) : ev_bits(b, n);
  if (v >> (n - 1) & 1) v |= ~0ull << n;
  return (int64_t)v;
}
static uint32_t ev_unary(EvBits *b) {
  uint32_t q = 0;
  while (!ev_bits(b, 1) && !b->eof) q++;
  return q;
}
static void ev_skip_bytes(EvBits *b, uint64_t n) {
  while (n && b->bits >= 8) b->bits -= 8, n--;
  const size_t take = (uint64_t)(b->n - b->pos) < n ? b->n - b->pos : (size_t)n;
  b->pos += take;
  n -= take;
  if (n && ev_skip(b->f, n)) b->eof = 1;
}
static int ev_at_end(EvBits *b) {
  if (b->bits >= 8 || b->pos < b->n) return 0;
  b->n = fread(b->buf, 1, sizeof b->buf, b->f);
  b->pos = 0;
  return b->n == 0;
}
static int ev_residual(EvBits *b, int64_t *x, int bs, int order) {
  const int method = (int)ev_bits(b, 2);
  if (method > 1) return -1;
  const int pbits = method ? 5 : 4, esc = method ? 31 : 15, po = (int)ev_bits(b, 4), parts = 1 << po;
  if (bs % parts || (bs >> po) < order) return -1;
  int i = order;
  for (int p = 0; p < parts; ++p) {
    const int k = (int)ev_bits(b, pbits), cnt = (bs >> po) - (p ? 0 : order);
    if (k == esc) {
      const int nb = (int)ev_bits(b, 5);
      for (int j = 0; j < cnt; ++j) x[i++] = ev_sbits(b, nb);
    } else {
      for (int j = 0; j < cnt; ++j) {
        const uint64_t u = (uint64_t)ev_unary(b) << k | ev_bits(b, k);
        x[i++] = (int64_t)(u >> 1) ^ -(int64_t)(u & 1);
      }
    }
    if (b->eof) return -1;
  }
  return 0;
}
static int ev_subframe(EvBits *b, int64_t *x, int bs, int bps) {
  if (ev_bits(b, 1)) return -1;
  const int type = (int)ev_bits(b, 6);
  int wasted = 0;
  if (ev_bits(b, 1)) {
    wasted = (int)ev_unary(b) + 1;
    bps -= wasted;
    if (bps < 1) return -1;
  }
  if (type == 0) {
    const int64_t v = ev_sbits(b, bps);
    for (int i = 0; i < bs; ++i) x[i] = v;
  } else if (type == 1) {
    for (int i = 0; i < bs; ++i) x[i] = ev_sbits(b, bps);
  } else if (type >= 8 && type <= 12) {
    const int order = type - 8;
    if (order > bs) return -1;
    for (int i = 0; i < order; ++i) x[i] = ev_sbits(b, bps);
    if (ev_residual(b, x, bs, order)) return -1;
    for (int i = order; i < bs; ++i) {
      if (order == 1) x[i] += x[i - 1];
      else if (order == 2) x[i] += 2 * x[i - 1] - x[i - 2];
      else if (order == 3) x[i] += 3 * x[i - 1] - 3 * x[i - 2] + x[i - 3];
      else if (order == 4) x[i] += 4 * x[i - 1] - 6 * x[i - 2] + 4 * x[i - 3] - x[i - 4];
    }
  } else if (type >= 32) {
    const int order = type - 31;
    if (order > bs) return -1;
    for (int i = 0; i < order; ++i) x[i] = ev_sbits(b, bps);
    const int prec = (int)ev_bits(b, 4) + 1;
    const int shift = (int)ev_sbits(b, 5);
    if (prec == 16 || shift < 0) return -1;
    int64_t coef[32];
    for (int j = 0; j < order; ++j) coef[j] = ev_sbits(b, prec);
    if (ev_residual(b, x, bs, order)) return -1;
    for (int i = order; i < bs; ++i) {
      int64_t sum = 0;
      for (int j = 0; j < order; ++j) sum += coef[j] * x[i - 1 - j];
      x[i] += sum >> shift;
    }
  } else {
    return -1;
  }
  if (wasted)
    for (int i = 0; i < bs; ++i) x[i] *= (int64_t)1 << wasted;
  return b->eof ? -1 : 0;
}
static int ev_flac(FILE *f, EvAudio *a, char *err, size_t en) {
  EvBits *b = (EvBits *)ev_alloc(NULL, sizeof *b);
  memset(b, 0, sizeof *b);
  b->f = f;
  int64_t *s[8] = {0};
  int rc = -1;
  uint8_t m[4];
  for (int i = 0; i < 4; ++i) m[i] = (uint8_t)ev_bits(b, 8);
  if (!memcmp(m, "ID3", 3)) { /* an ID3v2 tag before the stream */
    ev_bits(b, 8); /* the tag's revision */
    const uint32_t flags = ev_bits(b, 8);
    uint32_t size = 0;
    for (int i = 0; i < 4; ++i) size = size << 7 | (ev_bits(b, 8) & 0x7F);
    ev_skip_bytes(b, (uint64_t)size + (flags & 0x10 ? 10 : 0));
    for (int i = 0; i < 4; ++i) m[i] = (uint8_t)ev_bits(b, 8);
  }
  int rate = 0, ch = 0, bps = 0, last = 0, have_info = 0;
  if (memcmp(m, "fLaC", 4)) {
    snprintf(err, en, "not a FLAC stream");
    goto out;
  }
  while (!last) {
    const uint32_t hd = ev_bits(b, 8), len = ev_bits(b, 24);
    last = (int)(hd >> 7);
    if (b->eof) break;
    if ((hd & 0x7F) == 0 && len >= 34) { /* STREAMINFO */
      ev_bits(b, 32);
      ev_bits(b, 32);
      ev_bits(b, 16);
      rate = (int)ev_bits(b, 20);
      ch = (int)ev_bits(b, 3) + 1;
      bps = (int)ev_bits(b, 5) + 1;
      ev_bits(b, 4);
      ev_bits(b, 32);
      ev_skip_bytes(b, 16 + (uint64_t)(len - 34));
      have_info = 1;
    } else {
      ev_skip_bytes(b, len);
    }
  }
  if (!have_info || !rate || b->eof) {
    snprintf(err, en, "FLAC without its STREAMINFO block");
    goto out;
  }
  ev_audio_start(a, rate, ch);
  for (int c = 0; c < 8; ++c) s[c] = (int64_t *)ev_alloc(NULL, sizeof(int64_t) * 65536);
  while (a->n < a->max && !ev_at_end(b)) {
    if (ev_bits(b, 14) != 0x3FFE) {
      if (b->eof) break;
      snprintf(err, en, "FLAC: no frame where one should start");
      goto out;
    }
    ev_bits(b, 2); /* reserved, blocking strategy */
    const int bsc = (int)ev_bits(b, 4), rc4 = (int)ev_bits(b, 4), ca = (int)ev_bits(b, 4), ssc = (int)ev_bits(b, 3);
    ev_bits(b, 1);
    const uint32_t lead = ev_bits(b, 8); /* the frame or sample number, UTF-8 coded */
    int more = 0;
    if (lead >= 0xFE) more = lead == 0xFE ? 6 : -1;
    else if (lead >= 0xFC) more = 5;
    else if (lead >= 0xF8) more = 4;
    else if (lead >= 0xF0) more = 3;
    else if (lead >= 0xE0) more = 2;
    else if (lead >= 0xC0) more = 1;
    else if (lead >= 0x80) more = -1;
    for (int i = 0; i < more; ++i) ev_bits(b, 8);
    int bs = 0;
    if (bsc == 1) bs = 192;
    else if (bsc >= 2 && bsc <= 5) bs = 576 << (bsc - 2);
    else if (bsc == 6) bs = (int)ev_bits(b, 8) + 1;
    else if (bsc == 7) bs = (int)ev_bits(b, 16) + 1;
    else if (bsc >= 8) bs = 256 << (bsc - 8);
    if (rc4 == 12) ev_bits(b, 8);
    else if (rc4 == 13 || rc4 == 14) ev_bits(b, 16);
    static const int kBits[8] = {0, 8, 12, -1, 16, 20, 24, 32};
    const int fb = ssc ? kBits[ssc] : bps;
    ev_bits(b, 8); /* CRC-8 */
    const int nch = ca < 8 ? ca + 1 : 2;
    if (more < 0 || !bs || bs > 65536 || rc4 == 15 || ca > 10 || fb < 4 || fb > 32) {
      snprintf(err, en, "FLAC: a frame header this tool does not read");
      goto out;
    }
    for (int c = 0; c < nch; ++c) {
      const int side = (ca == 8 && c == 1) || (ca == 9 && c == 0) || (ca == 10 && c == 1);
      if (ev_subframe(b, s[c], bs, fb + side)) {
        snprintf(err, en, "FLAC: a damaged or cut frame");
        goto out;
      }
    }
    for (int i = 0; i < bs; ++i) {
      if (ca == 8) s[1][i] = s[0][i] - s[1][i];
      else if (ca == 9) s[0][i] += s[1][i];
      else if (ca == 10) {
        const int64_t mid = s[0][i] * 2 + (s[1][i] & 1), sd = s[1][i];
        s[0][i] = (mid + sd) >> 1;
        s[1][i] = (mid - sd) >> 1;
      }
    }
    b->bits -= b->bits & 7; /* the frame's padding to a byte */
    ev_bits(b, 16);         /* CRC-16 */
    const double scale = (double)(1ull << (fb - 1));
    for (int i = 0; i < bs && a->n < a->max; ++i) {
      double sum = 0;
      for (int c = 0; c < nch; ++c) sum += (double)s[c][i] / scale;
      ev_add(a, (float)(sum / nch));
    }
  }
  rc = 0;
out:
  for (int c = 0; c < 8; ++c) free(s[c]);
  free(b);
  return rc;
}

/* any of the formats, by its first bytes (raw float32 by its name); 0, or -1 and err */
static int ev_read_audio(const char *path, EvAudio *a, char *err, size_t en) {
  memset(a, 0, sizeof *a);
  FILE *f = ev_fopen(path, "rb");
  if (!f) {
    snprintf(err, en, "cannot open the file");
    return -1;
  }
  uint8_t h[12] = {0};
  const size_t got = fread(h, 1, 12, f);
  int rc;
  if (got == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
    rc = ev_wav(f, a, err, en);
  } else if (got >= 4 && (!memcmp(h, "fLaC", 4) || !memcmp(h, "ID3", 3))) {
    rewind(f);
    rc = ev_flac(f, a, err, en);
  } else if (ev_ext_is(path, ".pcm") || ev_ext_is(path, ".f32") || ev_ext_is(path, ".raw")) {
    rewind(f);
    ev_audio_start(a, 16000, 1);
    float buf[4096];
    size_t n;
    while (a->n < a->max && (n = fread(buf, sizeof(float), 4096, f)) > 0)
      for (size_t i = 0; i < n && a->n < a->max; ++i) ev_add(a, buf[i]);
    rc = 0;
  } else {
    snprintf(err, en, "not WAV, FLAC or raw float32 (.pcm, .f32, .raw); convert it first (ffmpeg -i in.mp3 out.flac)");
    rc = -1;
  }
  fclose(f);
  if (rc) {
    free(a->x);
    memset(a, 0, sizeof *a);
  }
  return rc;
}

/* ------------------------------------------------------------------ references */
typedef struct {
  char *key, *ref;
  int line, used;
} EvRef;
typedef struct {
  EvRef *v;
  int n, cap;
} EvRefs;
static void ev_norm_key(char *k) {
  for (char *p = k; *p; ++p)
    if (*p == '\\') *p = '/';
  while (k[0] == '.' && k[1] == '/') memmove(k, k + 2, strlen(k + 2) + 1);
}
static void ev_ref_add(EvRefs *r, char *key, char *ref, int line) {
  if (r->n == r->cap) {
    r->cap = r->cap ? 2 * r->cap : 64;
    r->v = (EvRef *)ev_alloc(r->v, sizeof(EvRef) * (size_t)r->cap);
  }
  ev_norm_key(key);
  r->v[r->n++] = (EvRef){key, ref, line, 0};
}
static int ev_lower_is(const char *s, const char *word) {
  size_t i = 0;
  for (; s[i] && word[i]; ++i)
    if ((s[i] | 32) != word[i]) return 0;
  return !s[i] && !word[i];
}
/* CSV (RFC 4180 quoting): file,reference per row */
static int ev_load_csv(const char *t, EvRefs *r, char *err, size_t en) {
  size_t i = 0;
  const size_t n = strlen(t);
  int line = 1, row = 0;
  while (i < n) {
    char *field[2] = {NULL, NULL};
    int nf = 0;
    const int row_line = line;
    for (;;) {
      EvBuf fb = {0};
      ev_put(&fb, "", 0);
      while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
      if (i < n && t[i] == '"') {
        for (i++;; i++) {
          if (i >= n) {
            snprintf(err, en, "line %d: a quote is not closed", row_line);
            free(fb.p);
            free(field[0]);
            free(field[1]);
            return -1;
          }
          if (t[i] == '"') {
            if (i + 1 < n && t[i + 1] == '"') {
              ev_put(&fb, "\"", 1);
              i++;
              continue;
            }
            i++;
            break;
          }
          if (t[i] == '\n') line++;
          ev_put(&fb, t + i, 1);
        }
        while (i < n && t[i] != ',' && t[i] != '\n' && t[i] != '\r') i++;
      } else {
        const size_t s0 = i;
        while (i < n && t[i] != ',' && t[i] != '\n' && t[i] != '\r') i++;
        size_t e = i;
        while (e > s0 && (t[e - 1] == ' ' || t[e - 1] == '\t')) e--;
        ev_put(&fb, t + s0, e - s0);
      }
      if (nf < 2) field[nf] = fb.p;
      else free(fb.p);
      nf++;
      if (i < n && t[i] == ',') {
        i++;
        continue;
      }
      if (i < n && t[i] == '\r') i++;
      if (i < n && t[i] == '\n') i++, line++;
      break;
    }
    row++;
    const int empty = nf == 1 && !field[0][0];
    const int header = row == 1 && nf >= 2 && (ev_lower_is(field[0], "file") || ev_lower_is(field[0], "path") || ev_lower_is(field[0], "audio"));
    if (empty || header) {
      free(field[0]);
      free(field[1]);
      continue;
    }
    if (nf < 2 || !field[0][0] || !field[1][0]) {
      snprintf(err, en, "line %d: expected file,reference", row_line);
      free(field[0]);
      free(field[1]);
      return -1;
    }
    ev_ref_add(r, field[0], field[1], row_line);
  }
  return 0;
}
/* JSONL: {"file": ..., "reference": ...} per line; other keys are ignored */
static int ev_load_jsonl(const char *t, EvRefs *r, char *err, size_t en) {
  int line = 0;
  const char *p = t;
  while (*p) {
    line++;
    const char *e = strchr(p, '\n');
    const size_t len = e ? (size_t)(e - p) : strlen(p);
    char *ln = (char *)ev_alloc(NULL, len + 1);
    memcpy(ln, p, len);
    ln[len] = 0;
    p += len + (e ? 1 : 0);
    const char *q = ev_ws(ln);
    if (!*q) {
      free(ln);
      continue;
    }
    char *file = NULL, *ref = NULL;
    int bad = *q != '{';
    if (!bad) {
      q = ev_ws(q + 1);
      while (!bad && *q != '}') {
        EvBuf key = {0}, val = {0};
        q = ev_jstr(q, &key);
        bad = !q;
        if (!bad) {
          q = ev_ws(q);
          bad = *q != ':';
        }
        if (!bad) {
          q = ev_ws(q + 1);
          const int want = (!strcmp(key.p, "file") && !file) || (!strcmp(key.p, "reference") && !ref);
          if (want && *q == '"') {
            q = ev_jstr(q, &val);
            if (q && !strcmp(key.p, "file")) file = val.p, val.p = NULL;
            else if (q) ref = val.p, val.p = NULL;
          } else {
            q = ev_jskip(q);
          }
          bad = !q;
        }
        free(key.p);
        free(val.p);
        if (!bad) {
          q = ev_ws(q);
          if (*q == ',') q = ev_ws(q + 1);
          else bad = *q != '}';
        }
      }
    }
    free(ln);
    if (bad || !file || !ref || !*file || !*ref) {
      snprintf(err, en, "line %d: expected {\"file\": ..., \"reference\": ...}", line);
      free(file);
      free(ref);
      return -1;
    }
    ev_ref_add(r, file, ref, line);
  }
  return 0;
}
static int ev_ref_cmp(const void *a, const void *b) {
  const EvRef *x = (const EvRef *)a, *y = (const EvRef *)b;
  const int c = strcmp(x->key, y->key);
  return c ? c : x->line - y->line;
}
static int ev_load_refs(const char *path, EvRefs *r, char *err, size_t en) {
  char *t = NULL;
  if (ev_read_text(path, &t)) {
    snprintf(err, en, "cannot read %s", path);
    return -1;
  }
  const int rc = *ev_ws(t) == '{' ? ev_load_jsonl(t, r, err, en) : ev_load_csv(t, r, err, en);
  free(t);
  if (rc) return rc;
  qsort(r->v, (size_t)r->n, sizeof(EvRef), ev_ref_cmp);
  for (int i = 1; i < r->n; ++i)
    if (!strcmp(r->v[i].key, r->v[i - 1].key)) {
      snprintf(err, en, "line %d: %s has a reference already (line %d)", r->v[i].line, r->v[i].key, r->v[i - 1].line);
      return -1;
    }
  return 0;
}
static EvRef *ev_ref_get(EvRefs *r, const char *key) {
  int lo = 0, hi = r->n - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2, c = strcmp(r->v[mid].key, key);
    if (!c) return &r->v[mid];
    if (c < 0) lo = mid + 1;
    else hi = mid - 1;
  }
  return NULL;
}
/* the file as listed, else its file name, else its name without the extension */
static EvRef *ev_ref_find(EvRefs *r, const char *file) {
  if (!r->n) return NULL;
  char *k = ev_strdup(file);
  ev_norm_key(k);
  EvRef *f = ev_ref_get(r, k);
  if (!f) f = ev_ref_get(r, ev_base(k));
  if (!f) {
    char *stem = ev_strdup(ev_base(k)), *dot = strrchr(stem, '.');
    if (dot && dot != stem) *dot = 0;
    f = ev_ref_get(r, stem);
    free(stem);
  }
  free(k);
  return f;
}
static void ev_refs_free(EvRefs *r) {
  for (int i = 0; i < r->n; ++i) free(r->v[i].key), free(r->v[i].ref);
  free(r->v);
  memset(r, 0, sizeof *r);
}

/* "S:A", "S:A-B" or "S:A-S2:B": 1 a range (set), 0 not written as one, -1 written as one but impossible */
static int ev_parse_range(const char *s, int *s1, int *a1, int *s2, int *a2) {
  int colon = 0;
  for (const char *p = s; *p; ++p) {
    if (*p == ':') colon++;
    else if (!(*p >= '0' && *p <= '9') && *p != '-' && *p != ' ' && *p != '\t') return 0;
  }
  if (!colon) return 0;
  long v[4] = {0};
  int k = 0;
  const char *p = ev_ws(s);
  char *e;
  v[k++] = strtol(p, &e, 10);
  if (e == p || *e != ':') return -1;
  p = e + 1;
  v[k++] = strtol(p, &e, 10);
  if (e == p) return -1;
  p = ev_ws(e);
  if (!*p) {
    v[2] = v[0], v[3] = v[1];
  } else {
    if (*p != '-') return -1;
    p = ev_ws(p + 1);
    const long x = strtol(p, &e, 10);
    if (e == p) return -1;
    if (*e == ':') {
      v[2] = x;
      p = e + 1;
      v[3] = strtol(p, &e, 10);
      if (e == p) return -1;
    } else {
      v[2] = v[0], v[3] = x;
    }
    if (*ev_ws(e)) return -1;
  }
  if (v[0] < 1 || v[0] > 114 || v[2] < 1 || v[2] > 114 || v[1] < 1 || v[3] < 1 || v[1] > 999 || v[3] > 999) return -1;
  if (v[2] < v[0] || (v[2] == v[0] && v[3] < v[1])) return -1;
  *s1 = (int)v[0], *a1 = (int)v[1], *s2 = (int)v[2], *a2 = (int)v[3];
  return 1;
}

/* ------------------------------------------------------------------ units, words and edit distance */
typedef struct {
  int *u;   /* unit ids */
  int *w;   /* the word of each unit (0, 1, ...) */
  int n, cap, words;
} EvSeq;
static void ev_seq_add(EvSeq *s, int id, int word) {
  if (s->n == s->cap) {
    s->cap = s->cap ? 2 * s->cap : 64;
    s->u = (int *)ev_alloc(s->u, sizeof(int) * (size_t)s->cap);
    s->w = (int *)ev_alloc(s->w, sizeof(int) * (size_t)s->cap);
  }
  s->u[s->n] = id;
  s->w[s->n++] = word;
  if (word + 1 > s->words) s->words = word + 1;
}
static void ev_seq_free(EvSeq *s) {
  free(s->u);
  free(s->w);
  memset(s, 0, sizeof *s);
}

typedef struct {
  long long ref, hyp, sub, del, ins; /* units */
} EvEd;
typedef struct {
  long long ref, wrong; /* the reference's words */
} EvWer;
static void ev_ed_add(EvEd *t, const EvEd *e) {
  t->ref += e->ref, t->hyp += e->hyp, t->sub += e->sub, t->del += e->del, t->ins += e->ins;
}
/* The Levenshtein distance between r (the reference) and h, with its split and the reference units the edits fall
 * on. At equal cost a match or substitution comes first, then a deletion (a unit of r missing from h), then an
 * insertion. bad[i] (when not NULL) becomes 1 for a unit of r substituted or deleted, or followed by an inserted
 * unit (an insertion before r's first unit falls on that unit). Two rows of costs and a table of 2-bit moves,
 * (nr + 1)(nh + 1) / 4 bytes: about 36 MB for two takes of 20 minutes. */
static void ev_align(const int *r, int nr, const int *h, int nh, EvEd *out, unsigned char *bad) {
  const size_t cols = (size_t)nh + 1;
  long long *prev = (long long *)ev_alloc(NULL, sizeof(long long) * cols), *cur = (long long *)ev_alloc(NULL, sizeof(long long) * cols);
  unsigned char *mv = (unsigned char *)calloc(((size_t)nr + 1) * cols / 4 + 1, 1);
  if (!mv) {
    fprintf(stderr, "tajweedformer-eval: out of memory\n");
    exit(2);
  }
#define EV_AT(i, j) ((size_t)(i) * cols + (size_t)(j))
#define EV_SET(i, j, v) (mv[EV_AT(i, j) >> 2] |= (unsigned char)((v) << (EV_AT(i, j) & 3) * 2))
#define EV_GET(i, j) (mv[EV_AT(i, j) >> 2] >> (EV_AT(i, j) & 3) * 2 & 3)
  for (int j = 0; j <= nh; ++j) {
    prev[j] = j;
    if (j) EV_SET(0, j, 2);
  }
  for (int i = 1; i <= nr; ++i) {
    cur[0] = i;
    EV_SET(i, 0, 1);
    for (int j = 1; j <= nh; ++j) {
      long long c = prev[j - 1] + (r[i - 1] != h[j - 1]);
      int m = 0;
      if (prev[j] + 1 < c) c = prev[j] + 1, m = 1;
      if (cur[j - 1] + 1 < c) c = cur[j - 1] + 1, m = 2;
      cur[j] = c;
      if (m) EV_SET(i, j, m);
    }
    long long *t = prev;
    prev = cur;
    cur = t;
  }
  *out = (EvEd){nr, nh, 0, 0, 0};
  for (int i = nr, j = nh; i > 0 || j > 0;) {
    const int m = EV_GET(i, j);
    if (m == 0) {
      if (r[i - 1] != h[j - 1]) {
        out->sub++;
        if (bad) bad[i - 1] = 1;
      }
      i--, j--;
    } else if (m == 1) {
      out->del++;
      if (bad) bad[i - 1] = 1;
      i--;
    } else {
      out->ins++;
      if (bad && nr) bad[i ? i - 1 : 0] = 1;
      j--;
    }
  }
#undef EV_AT
#undef EV_SET
#undef EV_GET
  free(mv);
  free(prev);
  free(cur);
}
/* the units' errors, and the reference's words with an error on one of their units */
static void ev_score(const EvSeq *ref, const EvSeq *hyp, EvEd *units, EvWer *words) {
  unsigned char *bad = (unsigned char *)calloc((size_t)ref->n + 1, 1);
  if (!bad) {
    fprintf(stderr, "tajweedformer-eval: out of memory\n");
    exit(2);
  }
  ev_align(ref->u, ref->n, hyp->u, hyp->n, units, bad);
  *words = (EvWer){0, 0};
  for (int i = 0; i < ref->n;) {
    int j = i, wrong = 0;
    while (j < ref->n && ref->w[j] == ref->w[i]) wrong |= bad[j++];
    words->ref++;
    words->wrong += wrong;
    i = j;
  }
  free(bad);
}
static void ev_json_ed(EvBuf *b, const EvEd *e) {
  ev_printf(b, "{\"ref\":%lld,\"hyp\":%lld,\"sub\":%lld,\"del\":%lld,\"ins\":%lld,\"rate\":", e->ref, e->hyp, e->sub, e->del, e->ins);
  if (e->ref) ev_printf(b, "%.6f}", (double)(e->sub + e->del + e->ins) / (double)e->ref);
  else ev_puts(b, "null}");
}
static void ev_json_wer(EvBuf *b, const EvWer *w) {
  ev_printf(b, "{\"ref\":%lld,\"wrong\":%lld,\"rate\":", w->ref, w->wrong);
  if (w->ref) ev_printf(b, "%.6f}", (double)w->wrong / (double)w->ref);
  else ev_puts(b, "null}");
}

/* ------------------------------------------------------------------ a run */
typedef struct {
  TfModel *m;
  TfReference *refdata;
  const char *context, *heads;
  int word_bar;
  EvRefs refs;
  EvEd units;
  EvWer words;
  int files, done, failed, with_ref;
  double audio_s, proc_s;
} EvRun;

/* the units of a reference: an ayah range from the reference data, or unit names ("|" between words) */
static int ev_resolve(EvRun *R, const char *text, EvSeq *out, EvBuf *shown, char *err, size_t en) {
  int s1, a1, s2, a2;
  const int range = ev_parse_range(text, &s1, &a1, &s2, &a2);
  if (range < 0) {
    snprintf(err, en, "%s is not an ayah range (S:A, S:A-B or S:A-S2:B)", text);
    return -1;
  }
  if (range > 0) {
    if (!R->refdata) {
      snprintf(err, en, "this model build has no reference data for ayah ranges");
      return -1;
    }
    int word0 = 0;
    for (int su = s1; su <= s2; ++su)
      for (int a = su == s1 ? a1 : 1; a <= 999 && (su < s2 || a <= a2); ++a) {
        TfUnits u;
        const int rc = tf_reference_units(R->refdata, su, a, &u);
        /* a surah before the range's last one runs to its last ayah: the next one does not exist */
        if (rc == TF_ERR_INVALID_ARGUMENT && su < s2 && a > (su == s1 ? a1 : 1)) break;
        if (rc) {
          snprintf(err, en, "ayah %d:%d: %s", su, a, tf_last_error() ? tf_last_error() : tf_error_name(rc));
          return -1;
        }
        int last = -1;
        for (int k = 0; k < u.n; ++k) {
          if (u.ids[k] == R->word_bar || u.ids[k] == 0) continue;
          ev_seq_add(out, u.ids[k], word0 + u.word[k]);
          if (u.word[k] > last) last = u.word[k];
        }
        word0 += (u.n_words > last + 1 ? u.n_words : last + 1);
      }
  } else {
    int word = 0, open = 0;
    const char *p = text;
    while (*(p = ev_ws(p))) {
      const char *e = p;
      while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++;
      char name[64];
      const size_t len = (size_t)(e - p);
      if (len >= sizeof name) {
        snprintf(err, en, "a unit name too long in the reference");
        return -1;
      }
      memcpy(name, p, len);
      name[len] = 0;
      p = e;
      if (!strcmp(name, "|")) {
        if (open) word++, open = 0;
        continue;
      }
      const int id = tf_model_token_id(R->m, name);
      if (id <= 0 || id == R->word_bar) {
        snprintf(err, en, "the reference has \"%s\", which is not one of the model's units", name);
        return -1;
      }
      ev_seq_add(out, id, word);
      open = 1;
    }
  }
  /* as the transcript writes it */
  ev_put(shown, "", 0);
  for (int k = 0; k < out->n; ++k) {
    if (k) ev_puts(shown, out->w[k] != out->w[k - 1] ? " | " : " ");
    ev_puts(shown, tf_model_token_name(R->m, out->u[k]));
  }
  return 0;
}

typedef struct {
  int limit;
} EvTake;
static void ev_on_event(void *user, const TfEvent *e) {
  if (e->type == TF_EVENT_ENDED && e->reason == TF_ENDED_LIMIT) ((EvTake *)user)->limit = 1;
}

static void ev_heads(EvBuf *L, const TfHeads *h) {
  ev_puts(L, ",\"heads\":{");
  for (int i = 0, first = 1; i < tf_heads_count(h); ++i) {
    const char *name = tf_heads_name(h, i);
    TfHead hd;
    if (!name || !strcmp(name, "log_probs") || tf_heads_get(h, name, &hd)) continue;
    if (!first) ev_puts(L, ",");
    first = 0;
    ev_json_str(L, name);
    ev_printf(L, ":{\"frame_ms\":%d,\"frames\":%d", hd.frame_ms, hd.frames);
    if (!strcmp(hd.kind, "ctc")) {
      const TfToken *tk = NULL;
      ev_printf(L, ",\"units\":%d}", tf_heads_greedy(h, name, &tk));
    } else if (hd.class_names && hd.classes > 1) {
      ev_puts(L, ",\"classes\":{");
      for (int k = 0; k < hd.classes; ++k) {
        long long top = 0;
        double sum = 0;
        for (int t = 0; t < hd.frames; ++t) {
          const float *row = hd.values + (size_t)t * hd.classes;
          int best = 0;
          for (int c = 1; c < hd.classes; ++c)
            if (row[c] > row[best]) best = c;
          top += best == k;
          sum += row[k];
        }
        if (k) ev_puts(L, ",");
        ev_json_str(L, hd.class_names[k]);
        ev_printf(L, ":{\"frames\":%lld,\"mean\":", top);
        ev_json_num(L, hd.frames ? sum / hd.frames : NAN, "%.6f");
        ev_puts(L, "}");
      }
      ev_puts(L, "}}");
    } else {
      double sum = 0;
      long long over = 0;
      for (int t = 0; t < hd.frames; ++t) {
        const double p = 1.0 / (1.0 + exp(-(double)hd.values[(size_t)t * hd.classes]));
        sum += p;
        over += p > 0.5;
      }
      ev_puts(L, ",\"mean\":");
      ev_json_num(L, hd.frames ? sum / hd.frames : NAN, "%.6f");
      ev_printf(L, ",\"frames_over_half\":%lld}", over);
    }
  }
  ev_puts(L, "}");
}

/* one file: one line */
static int ev_file(EvRun *R, const char *shown, const char *path, FILE *out) {
  EvBuf L = {0};
  EvAudio a;
  EvSeq ref = {0}, hyp = {0};
  EvBuf reftext = {0};
  char err[512] = "", referr[512] = "";
  const char *code = NULL;
  TfSession *s = NULL;
  EvTake take = {0};
  int rc = 1;
  R->files++;
  ev_puts(&L, "{\"file\":");
  ev_json_str(&L, shown);
  if (ev_read_audio(path, &a, err, sizeof err)) goto fail;
  /* the reference before the take: the reference data is not read while a take runs */
  EvRef *rf = ev_ref_find(&R->refs, shown);
  if (rf) {
    rf->used = 1;
    if (ev_resolve(R, rf->ref, &ref, &reftext, referr, sizeof referr)) {
      ev_seq_free(&ref);
      if (!referr[0]) snprintf(referr, sizeof referr, "the reference cannot be used");
    }
  }
  TfSessionOptions so;
  memset(&so, 0, sizeof so);
  so.context = R->context;
  so.sample_rate = a.rate;
  so.heads = R->heads;
  so.on_event = ev_on_event;
  so.user = &take;
  const double t0 = ev_now();
  s = tf_session_create(R->m, &so);
  if (!s) {
    snprintf(err, sizeof err, "%s", tf_last_error() ? tf_last_error() : "the session was refused");
    code = tf_error_name(tf_last_error_code());
    goto fail;
  }
  for (long long at = 0; at < a.n; at += a.rate) {
    const int k = (int)(a.n - at < a.rate ? a.n - at : a.rate);
    if (tf_session_push(s, a.x + at, k) < 0) {
      if (take.limit) break;
      snprintf(err, sizeof err, "%s", tf_last_error() ? tf_last_error() : "the take failed");
      code = tf_error_name(tf_last_error_code());
      goto fail;
    }
  }
  if (tf_session_finish(s) < 0 && !take.limit) {
    snprintf(err, sizeof err, "%s", tf_last_error() ? tf_last_error() : "the take failed");
    code = tf_error_name(tf_last_error_code());
    goto fail;
  }
  const TfHeads *h = tf_session_heads(s);
  const TfDecoded *d = h ? tf_heads_decoded(h) : NULL;
  const double t1 = ev_now();
  if (!d) {
    snprintf(err, sizeof err, "%s", tf_last_error() ? tf_last_error() : "the take has no output");
    code = tf_error_name(tf_last_error_code());
    goto fail;
  }
  const double dur = (double)a.n / a.rate, proc = t1 - t0;
  ev_printf(&L, ",\"duration_s\":%.3f,\"sample_rate\":%d,\"channels\":%d,\"limit\":%s,\"processing_s\":%.3f,\"rtf\":", dur, a.rate, a.channels,
            take.limit ? "true" : "false", proc);
  ev_json_num(&L, dur > 0 ? proc / dur : NAN, "%.4f");
  /* the transcript, as the public API gives it */
  ev_puts(&L, ",\"transcript\":{\"text\":");
  ev_json_str(&L, d->text);
  ev_puts(&L, ",\"units\":[");
  for (int i = 0, word = 0, open = 0, first = 1; i < d->n_tokens; ++i) {
    const TfToken *t = &d->tokens[i];
    if (t->id == R->word_bar) {
      if (open) word++, open = 0;
      continue;
    }
    ev_seq_add(&hyp, t->id, word);
    open = 1;
    if (!first) ev_puts(&L, ",");
    first = 0;
    ev_printf(&L, "{\"id\":%d,\"unit\":", t->id);
    ev_json_str(&L, t->name ? t->name : tf_model_token_name(R->m, t->id));
    ev_printf(&L, ",\"start_ms\":%d,\"end_ms\":%d,\"confidence\":", t->start_ms, t->end_ms);
    ev_json_num(&L, t->confidence, "%.4f");
    ev_puts(&L, "}");
  }
  ev_puts(&L, "],\"words\":[");
  for (int w = 0; w < d->n_words; ++w) {
    const TfWord *wd = &d->words[w];
    EvBuf wt = {0};
    ev_put(&wt, "", 0);
    for (int k = 0; k < wd->n_tokens; ++k) {
      const TfToken *t = &d->tokens[wd->first_token + k];
      if (k) ev_puts(&wt, " ");
      ev_puts(&wt, t->name ? t->name : tf_model_token_name(R->m, t->id));
    }
    ev_puts(&L, w ? ",{\"text\":" : "{\"text\":");
    ev_json_str(&L, wt.p);
    free(wt.p);
    ev_printf(&L, ",\"start_ms\":%d,\"end_ms\":%d,\"confidence\":", wd->start_ms, wd->end_ms);
    ev_json_num(&L, wd->confidence, "%.4f");
    ev_puts(&L, "}");
  }
  ev_puts(&L, "]}");
  ev_heads(&L, h);
  EvEd eu = {0};
  EvWer ew = {0};
  if (rf && !referr[0]) {
    ev_score(&ref, &hyp, &eu, &ew);
    ev_puts(&L, ",\"reference\":{\"source\":");
    ev_json_str(&L, ev_parse_range(rf->ref, &(int){0}, &(int){0}, &(int){0}, &(int){0}) > 0 ? rf->ref : "units");
    ev_puts(&L, ",\"text\":");
    ev_json_str(&L, reftext.p);
    ev_printf(&L, ",\"units\":%d,\"words\":%d},\"errors\":{\"units\":", ref.n, ref.words);
    ev_json_ed(&L, &eu);
    ev_puts(&L, ",\"words\":");
    ev_json_wer(&L, &ew);
    ev_puts(&L, "}");
    ev_ed_add(&R->units, &eu);
    R->words.ref += ew.ref, R->words.wrong += ew.wrong;
    R->with_ref++;
  } else if (rf) {
    ev_puts(&L, ",\"reference_error\":");
    ev_json_str(&L, referr);
  }
  ev_puts(&L, "}\n");
  R->done++;
  R->audio_s += dur;
  R->proc_s += proc;
  fprintf(stderr, "  %d  %s: %.1f s, %d units", R->files, shown, dur, hyp.n);
  if (rf && !referr[0])
    fprintf(stderr, ", UER %.1f%%, WER %.1f%%", eu.ref ? 100.0 * (eu.sub + eu.del + eu.ins) / eu.ref : 0.0,
            ew.ref ? 100.0 * ew.wrong / ew.ref : 0.0);
  if (referr[0]) fprintf(stderr, " (reference: %s)", referr);
  fprintf(stderr, "%s\n", take.limit ? " (the first 20 minutes)" : "");
  rc = 0;
  goto done;
fail:
  ev_puts(&L, ",\"error\":");
  ev_json_str(&L, err);
  ev_puts(&L, ",\"code\":");
  ev_json_str(&L, code ? code : "unreadable_audio");
  ev_puts(&L, "}\n");
  R->failed++;
  fprintf(stderr, "  %d  %s: %s\n", R->files, shown, err);
done:
  fwrite(L.p, 1, L.n, out);
  fflush(out);
  if (s) tf_session_destroy(s);
  free(L.p);
  free(reftext.p);
  free(a.x);
  ev_seq_free(&ref);
  ev_seq_free(&hyp);
  return rc;
}

static void ev_progress(void *u, long long done, long long total) {
  (void)u;
  fprintf(stderr, "\r  the model: %lld / %lld bytes", done, total);
  if (done >= total) fprintf(stderr, "\n");
}

static int ev_usage(const char *why) {
  if (why) fprintf(stderr, "tajweedformer-eval: %s\n", why);
  fprintf(stderr,
          "usage: tajweedformer-eval [--key KEY] --app-id ID <folder | list.txt | audio file> [--refs refs.csv|refs.jsonl]\n"
          "                          [--context C] [--threads N] [--heads all|name,name] [--out results.jsonl]\n"
          "                          [--storage DIR] [--version X.Y.Z]\n"
          "  The key: --key or the QURANLAB_KEY environment variable (the app's desktop key from sdk.quranlab.ai).\n"
          "  One JSON line per file, then a total line. The whole description: the comment at the top of\n"
          "  cli/tajweedformer_eval.c, and the README's \"Evaluate the model on your own data\".\n");
  return 2;
}

static int ev_fail_sdk(const char *what) {
  const int code = tf_last_error_code();
  fprintf(stderr, "tajweedformer-eval: %s: %s (%s)\n  %s\n  %s\n", what, tf_last_error() ? tf_last_error() : "failed", tf_error_name(code),
          tf_last_error_fix() ? tf_last_error_fix() : tf_error_fix(code), tf_error_docs(code));
  return 2;
}

/* the whole tool; `given`: a Client to use instead of making one (the tests' stand-in server) */
static int ev_main(int argc, char **argv, TfClient *given) {
  const char *key = NULL, *app_id = NULL, *refs_path = NULL, *context = NULL, *heads = NULL, *out_path = NULL, *storage = NULL,
             *version = NULL, *input = NULL;
  int threads = 0;
  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (!strcmp(a, "--help") || !strcmp(a, "-h")) return ev_usage(NULL), 0;
    if (!strncmp(a, "--", 2)) {
      if (i + 1 >= argc) return ev_usage("a flag without its value");
      const char *v = argv[++i];
      if (!strcmp(a, "--key")) key = v;
      else if (!strcmp(a, "--app-id")) app_id = v;
      else if (!strcmp(a, "--refs")) refs_path = v;
      else if (!strcmp(a, "--context")) context = v;
      else if (!strcmp(a, "--heads")) heads = v;
      else if (!strcmp(a, "--out")) out_path = v;
      else if (!strcmp(a, "--storage")) storage = v;
      else if (!strcmp(a, "--version")) version = v;
      else if (!strcmp(a, "--threads")) {
        char *e;
        threads = (int)strtol(v, &e, 10);
        if (*e || threads < 0) return ev_usage("--threads takes a number, 0 or more");
      } else {
        fprintf(stderr, "tajweedformer-eval: unknown flag %s\n", a);
        return ev_usage(NULL);
      }
    } else if (!input) {
      input = a;
    } else {
      return ev_usage("one input: a folder, a list file or one audio file");
    }
  }
  if (!input) return ev_usage("no input");
  if (!key) key = getenv("QURANLAB_KEY");
  if (!given && (!key || !*key || !app_id)) return ev_usage("needs the app's desktop key (--key or QURANLAB_KEY) and --app-id");

  /* the files */
  EvList files = {0};
  char *base = NULL;
  if (ev_is_dir(input)) {
    ev_walk(input, "", &files, 0);
    qsort(files.v, (size_t)files.n, sizeof(char *), ev_strcmp);
    base = ev_strdup(input);
  } else if (ev_ext_is(input, ".txt") || ev_ext_is(input, ".lst")) {
    char *t = NULL;
    if (ev_read_text(input, &t)) {
      fprintf(stderr, "tajweedformer-eval: cannot read %s\n", input);
      return 2;
    }
    for (char *p = t; *p;) {
      char *e = strchr(p, '\n');
      if (e) *e = 0;
      char *q = (char *)ev_ws(p);
      size_t n = strlen(q);
      while (n && (q[n - 1] == '\r' || q[n - 1] == ' ' || q[n - 1] == '\t')) q[--n] = 0;
      if (*q && *q != '#') ev_list_add(&files, ev_strdup(q));
      p = e ? e + 1 : p + strlen(p);
    }
    free(t);
    base = ev_strdup(input);
    char *cut = (char *)ev_base(base);
    *cut = 0;
  } else {
    ev_list_add(&files, ev_strdup(input));
  }
  if (!files.n) {
    fprintf(stderr, "tajweedformer-eval: no audio file in %s (.wav, .flac, .pcm, .f32, .raw)\n", input);
    free(base);
    return 2;
  }
  EvRun R;
  memset(&R, 0, sizeof R);
  R.context = context;
  R.heads = heads;
  char err[512];
  if (refs_path && ev_load_refs(refs_path, &R.refs, err, sizeof err)) {
    fprintf(stderr, "tajweedformer-eval: %s: %s\n", refs_path, err);
    return 2;
  }
  int ranges = 0;
  for (int i = 0; i < R.refs.n; ++i) {
    int x;
    ranges += ev_parse_range(R.refs.v[i].ref, &x, &x, &x, &x) > 0;
  }
  FILE *out = out_path ? ev_fopen(out_path, "wb") : stdout;
  if (!out) {
    fprintf(stderr, "tajweedformer-eval: cannot write %s\n", out_path);
    return 2;
  }
  int rc = 2;
  TfClient *c = given;
  if (!c) {
    TfClientOptions co;
    memset(&co, 0, sizeof co);
    co.key = key;
    co.app_id = app_id;
    co.storage = storage;
    co.app_version = "eval";
    c = tf_client_create(&co);
    if (!c) {
      rc = ev_fail_sdk("client");
      goto end;
    }
  }
  /* the model: downloaded once (with its progress), then from the SDK's folder, with or without a network */
  const char *st = tf_client_storage_json(c);
  if (st && strstr(st, "\"models\":[]")) {
    fprintf(stderr, "tajweedformer-eval: downloading the model (once)\n");
    TfPrefetchOptions po;
    memset(&po, 0, sizeof po);
    po.on_progress = ev_progress;
    po.version = version;
    if (tf_client_prefetch(c, &po)) {
      rc = ev_fail_sdk("download");
      goto end;
    }
  }
  TfModelOptions mo;
  memset(&mo, 0, sizeof mo);
  mo.version = version;
  mo.threads = threads;
  R.m = tf_client_model(c, &mo);
  if (!R.m) {
    rc = ev_fail_sdk("model");
    goto end;
  }
  const TfTokens *tok = tf_model_tokens(R.m);
  R.word_bar = tok ? tok->word_bar : -1;
  if (ranges) {
    R.refdata = tf_model_reference(R.m);
    if (R.refdata && !tf_reference_is_downloaded(R.refdata)) {
      fprintf(stderr, "tajweedformer-eval: fetching the reference data (once)\n");
      if (tf_reference_fetch(R.refdata)) {
        rc = ev_fail_sdk("reference data");
        goto end;
      }
    }
  }
  TfDiagnosis dg;
  memset(&dg, 0, sizeof dg);
  tf_client_diagnose(c, &dg);
  const char *ctx = context ? context : dg.context;
  char *ctx_copy = ev_strdup(ctx ? ctx : "");
  fprintf(stderr, "tajweedformer-eval: model %s, context %s, %d files\n", tf_model_version(R.m), ctx_copy[0] ? ctx_copy : "default", files.n);
  for (int i = 0; i < files.n; ++i) {
    char *path = ev_join(base && *base ? base : NULL, files.v[i]);
    ev_file(&R, files.v[i], path, out);
    free(path);
  }
  /* the total */
  EvBuf T = {0};
  ev_printf(&T, "{\"total\":{\"files\":%d,\"evaluated\":%d,\"failed\":%d,\"with_reference\":%d,\"audio_s\":%.3f,\"processing_s\":%.3f,\"rtf\":", R.files,
            R.done, R.failed, R.with_ref, R.audio_s, R.proc_s);
  ev_json_num(&T, R.audio_s > 0 ? R.proc_s / R.audio_s : NAN, "%.4f");
  ev_puts(&T, ",\"units\":");
  ev_json_ed(&T, &R.units);
  ev_puts(&T, ",\"words\":");
  ev_json_wer(&T, &R.words);
  ev_puts(&T, ",\"sdk\":");
  ev_json_str(&T, tf_version());
  ev_puts(&T, ",\"model\":");
  ev_json_str(&T, tf_model_version(R.m));
  ev_puts(&T, ",\"context\":");
  if (ctx_copy[0]) ev_json_str(&T, ctx_copy);
  else ev_puts(&T, "null");
  ev_printf(&T, ",\"threads\":%d}}\n", threads);
  fwrite(T.p, 1, T.n, out);
  fflush(out);
  free(T.p);
  free(ctx_copy);
  fprintf(stderr, "tajweedformer-eval: %d of %d files evaluated", R.done, R.files);
  if (R.with_ref)
    fprintf(stderr, "; on the %d with a reference UER %.2f%%, WER %.2f%%", R.with_ref,
            R.units.ref ? 100.0 * (R.units.sub + R.units.del + R.units.ins) / R.units.ref : 0.0,
            R.words.ref ? 100.0 * R.words.wrong / R.words.ref : 0.0);
  fprintf(stderr, "; RTF %.3f\n", R.audio_s > 0 ? R.proc_s / R.audio_s : 0.0);
  int unused = 0;
  for (int i = 0; i < R.refs.n; ++i) unused += !R.refs.v[i].used;
  if (unused) fprintf(stderr, "tajweedformer-eval: %d references matched no file\n", unused);
  rc = R.failed ? 1 : 0;
end:
  if (R.m) tf_model_close(R.m);
  if (c && !given) tf_client_destroy(c);
  if (out != stdout) fclose(out);
  for (int i = 0; i < files.n; ++i) free(files.v[i]);
  free(files.v);
  free(base);
  ev_refs_free(&R.refs);
  return rc;
}

#ifndef TF_EVAL_EMBED
#ifdef _WIN32
/* the arguments as UTF-8, whatever the console's code page */
int main(void) {
  int argc = 0;
  LPWSTR *wv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!wv) return 2;
  char **argv = (char **)ev_alloc(NULL, sizeof(char *) * ((size_t)argc + 1));
  for (int i = 0; i < argc; ++i) argv[i] = ev_narrow(wv[i]);
  argv[argc] = NULL;
  LocalFree(wv);
  const int rc = ev_main(argc, argv, NULL);
  for (int i = 0; i < argc; ++i) free(argv[i]);
  free(argv);
  return rc;
}
#else
int main(int argc, char **argv) { return ev_main(argc, argv, NULL); }
#endif
#endif
