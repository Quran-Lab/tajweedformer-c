/* Included by tf_tests.c: tajweedformer-eval (cli/tajweedformer_eval.c, 3.1.4) inside the tests, its own functions
 * and its main with a Client of the model tests' stub.
 *
 *   eval_units <test/data/eval> <work dir>     no model: the edit distance and its split, the reference's words with
 *                                               an edit on them, ayah ranges, CSV and JSONL references and how a file
 *                                               finds its reference, the JSON escapes, WAV in every form the tool
 *                                               reads, FLAC from ffmpeg's encoder (LPC, stereo decorrelation, 24-bit,
 *                                               wasted bits: test/make_eval_flac.py), raw float32
 *   eval30 <local30 dir> <key_hex> <clips dir> <expected.jsonl> <work dir> <configs.json> [full|prod]
 *                                               the 35 conformance clips (<clips dir>: the archive unpacked, its
 *                                               .pcm files in audio) through the tool with a reference each (the
 *                                               ayah of the clip's name),
 *                                               the Client of model30 against its stub (the local 3.0 folders): every
 *                                               line's fields, the units equal the engine's expected tokens
 *                                               (<expected.jsonl>: conformance/v4_c16_l64_audio_expected.jsonl), the
 *                                               error counts equal a plain edit distance of the reference data's
 *                                               units, the totals; then again with no network at all and JSONL
 *                                               references, one of them units text. */
#define TF_EVAL_EMBED 1
#include "../cli/tajweedformer_eval.c"

/* the fixtures' signal (test/make_eval_flac.py): integers only, so Python and C agree to the sample */
static void ev_t_signal(int64_t *a, int64_t *b, int n, int64_t amp, uint32_t seed, int64_t mul) {
  uint32_t st = seed;
  for (int i = 0; i < n; ++i) {
    const int64_t tri = (int64_t)((i * 7) % 400) - 200;
    st = (st * 1103515245u + 12345u) & 0x7FFFFFFFu;
    const int64_t nz = (int64_t)((st >> 16) % 33) - 16;
    const int64_t x = (tri * amp + nz) * mul;
    a[i] = x;
    if (b) b[i] = x - (x >> 2) + (i * 5) % 50 - 25;
  }
}
static int ev_t_flac_fixture(const char *dir, const char *name, int rate, int bits, int stereo, int n, int64_t amp, uint32_t seed, int64_t mul) {
  char *p = tf_path_join(dir, name);
  int64_t *x = (int64_t *)malloc(sizeof(int64_t) * (size_t)n), *y = stereo ? (int64_t *)malloc(sizeof(int64_t) * (size_t)n) : NULL;
  ev_t_signal(x, y, n, amp, seed, mul);
  EvAudio a;
  char err[256] = "";
  const int rc = ev_read_audio(p, &a, err, sizeof err);
  int same = !rc && a.rate == rate && a.channels == (stereo ? 2 : 1) && a.n == n;
  const double scale = (double)(1ll << (bits - 1));
  for (int i = 0; same && i < n; ++i) {
    double sum = (double)x[i] / scale;
    if (y) sum += (double)y[i] / scale;
    same = a.x[i] == (float)(sum / (stereo ? 2 : 1));
  }
  CHECK(same, "eval: %s decodes to the signal it was made from (%s, %lld samples at %d Hz)", name, rc ? err : "read", a.n, a.rate);
  free(a.x);
  free(x);
  free(y);
  free(p);
  return same;
}

/* a WAV file: fmt 1 or 3, plain or WAVE_FORMAT_EXTENSIBLE, an odd chunk before fmt, a data length of 0xFFFFFFFF */
static void ev_t_wav(const char *path, int fmt, int bits, int ch, int rate, const void *data, uint32_t bytes, int ext, int odd, int unknown) {
  FILE *f = fopen(path, "wb");
  uint8_t b[64];
#define EV_T_LE(p, v, n) for (int q_ = 0; q_ < (n); ++q_) (p)[q_] = (uint8_t)((uint32_t)(v) >> (8 * q_))
  fwrite("RIFF\0\0\0\0WAVE", 1, 12, f);
  if (odd) fwrite("LIST\3\0\0\0abc\0", 1, 12, f);
  const int block = ch * bits / 8;
  memset(b, 0, sizeof b);
  memcpy(b, "fmt ", 4);
  EV_T_LE(b + 4, ext ? 40 : 16, 4);
  EV_T_LE(b + 8, ext ? 0xFFFE : fmt, 2);
  EV_T_LE(b + 10, ch, 2);
  EV_T_LE(b + 12, rate, 4);
  EV_T_LE(b + 16, rate * block, 4);
  EV_T_LE(b + 20, block, 2);
  EV_T_LE(b + 22, bits, 2);
  if (ext) {
    EV_T_LE(b + 24, 22, 2);
    EV_T_LE(b + 26, bits, 2);
    EV_T_LE(b + 32, fmt, 2);
    memcpy(b + 34, "\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 14);
  }
  fwrite(b, 1, ext ? 48 : 24, f);
  memcpy(b, "data", 4);
  EV_T_LE(b + 4, unknown ? 0xFFFFFFFFu : bytes, 4);
  fwrite(b, 1, 8, f);
  fwrite(data, 1, bytes, f);
  fclose(f);
#undef EV_T_LE
}

static int ev_t_align(const char *r, const char *h, long long s, long long d, long long i) {
  int rr[64], hh[64];
  const int nr = (int)strlen(r), nh = (int)strlen(h);
  for (int k = 0; k < nr; ++k) rr[k] = r[k];
  for (int k = 0; k < nh; ++k) hh[k] = h[k];
  EvEd e;
  ev_align(rr, nr, hh, nh, &e, NULL);
  const int ok = e.ref == nr && e.hyp == nh && e.sub == s && e.del == d && e.ins == i;
  CHECK(ok, "eval: \"%s\" against \"%s\": sub %lld del %lld ins %lld, want %lld %lld %lld", r, h, e.sub, e.del, e.ins, s, d, i);
  return ok;
}
static int ev_t_range(const char *s, int want, int s1, int a1, int s2, int a2) {
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  const int got = ev_parse_range(s, &x1, &y1, &x2, &y2);
  const int ok = got == want && (got != 1 || (x1 == s1 && y1 == a1 && x2 == s2 && y2 == a2));
  CHECK(ok, "eval: the range \"%s\" reads as %d (%d:%d-%d:%d), want %d", s, got, x1, y1, x2, y2, want);
  return ok;
}

static int eval_units_tests(const char *data, const char *work) {
  tf__remove_tree(work);
  tf_mkdirs(work);
  int ok = 0, all = 0;
  /* the edit distance and its split (a match or substitution first at equal cost, then a deletion) */
  ok += ev_t_align("abc", "abc", 0, 0, 0), all++;
  ok += ev_t_align("abc", "axc", 1, 0, 0), all++;
  ok += ev_t_align("abc", "ac", 0, 1, 0), all++;
  ok += ev_t_align("ac", "abc", 0, 0, 1), all++;
  ok += ev_t_align("", "ab", 0, 0, 2), all++;
  ok += ev_t_align("ab", "", 0, 2, 0), all++;
  ok += ev_t_align("kitten", "sitting", 2, 0, 1), all++;
  ok += ev_t_align("ab", "ba", 2, 0, 0), all++;
  ok += ev_t_align("abcdef", "azced", 2, 1, 0), all++;
  /* the reference's words with an edit on them: ref [1 2] [3] [4 5]; the transcript has no words of its own */
  {
    static const struct {
      int n, u[8], wrong; /* the transcript's units and the words wrong */
    } wc[6] = {{5, {1, 2, 3, 4, 5}, 0}, {6, {1, 2, 3, 6, 4, 5}, 1}, {4, {1, 2, 3, 4}, 1}, {6, {9, 1, 2, 3, 4, 5}, 1}, {5, {1, 7, 3, 4, 5}, 1}, {3, {1, 2, 3}, 1}};
    const int ru[5] = {1, 2, 3, 4, 5}, rw[5] = {0, 0, 1, 2, 2};
    int good = 0;
    for (int c = 0; c < 6; ++c) {
      EvSeq r = {0}, h = {0};
      for (int k = 0; k < 5; ++k) ev_seq_add(&r, ru[k], rw[k]);
      for (int k = 0; k < wc[c].n; ++k) ev_seq_add(&h, wc[c].u[k], 0);
      EvEd u;
      EvWer w;
      ev_score(&r, &h, &u, &w);
      const int one = w.ref == 3 && w.wrong == wc[c].wrong && r.words == 3 && u.ref == 5 && u.hyp == wc[c].n;
      CHECK(one, "eval: words, case %d: %lld of %lld wrong, want %d", c, w.wrong, w.ref, wc[c].wrong);
      good += one;
      ev_seq_free(&r);
      ev_seq_free(&h);
    }
    EvBuf b = {0};
    const EvWer w1 = {3, 1}, w0 = {0, 0};
    ev_json_wer(&b, &w1);
    CHECK(!strcmp(b.p, "{\"ref\":3,\"wrong\":1,\"rate\":0.333333}"), "eval: a word count as JSON: %s", b.p);
    b.n = 0;
    ev_json_wer(&b, &w0);
    CHECK(strstr(b.p, "\"rate\":null"), "eval: no reference words, no rate");
    b.n = 0;
    const EvEd e0 = {0, 0, 0, 0, 0};
    ev_json_ed(&b, &e0);
    CHECK(!strcmp(b.p, "{\"ref\":0,\"hyp\":0,\"sub\":0,\"del\":0,\"ins\":0,\"rate\":null}"), "eval: a unit count as JSON: %s", b.p);
    free(b.p);
    ok += good == 6, all++;
  }
  printf("eval: edit distance %d/%d cases\n", ok, all);
  /* ayah ranges */
  int rg = 0;
  rg += ev_t_range("1:1", 1, 1, 1, 1, 1);
  rg += ev_t_range(" 2:255 ", 1, 2, 255, 2, 255);
  rg += ev_t_range("1:1-7", 1, 1, 1, 1, 7);
  rg += ev_t_range("2:286-3:1", 1, 2, 286, 3, 1);
  rg += ev_t_range("112:1 - 112:4", 1, 112, 1, 112, 4);
  rg += ev_t_range("1:", -1, 0, 0, 0, 0);
  rg += ev_t_range("0:1", -1, 0, 0, 0, 0);
  rg += ev_t_range("115:1", -1, 0, 0, 0, 0);
  rg += ev_t_range("1:5-3", -1, 0, 0, 0, 0);
  rg += ev_t_range("3:1-2:5", -1, 0, 0, 0, 0);
  rg += ev_t_range("1:1-2-3", -1, 0, 0, 0, 0);
  rg += ev_t_range("a i u", 0, 0, 0, 0, 0);
  rg += ev_t_range("|", 0, 0, 0, 0, 0);
  printf("eval: ayah ranges %d/13\n", rg);
  /* references: CSV and JSONL, and how a file finds its own */
  {
    char *p = tf_path_join(work, "refs.csv");
    const char csv[] = "\xEF\xBB\xBF" "file,reference\r\nAbdul_001001.wav,1:1\r\n\"sub\\a,b.wav\",\"2:255\"\r\n\r\nHusary_001002, a  i | u \n./x/y.flac,\"say \"\"q\"\"\"\n";
    tf_write_file(p, csv, sizeof csv - 1);
    EvRefs r = {0};
    char err[256] = "";
    const int rc = ev_load_refs(p, &r, err, sizeof err);
    EvRef *a = ev_ref_find(&r, "Abdul_001001.wav"), *a2 = ev_ref_find(&r, "deep/dir/Abdul_001001.wav"), *b = ev_ref_find(&r, "sub/a,b.wav"),
          *c = ev_ref_find(&r, "c/Husary_001002.flac"), *d = ev_ref_find(&r, "x\\y.flac"), *none = ev_ref_find(&r, "Abdul_001001.flac");
    CHECK(!rc && r.n == 4 && a && a == a2 && !strcmp(a->ref, "1:1") && b && !strcmp(b->ref, "2:255") && c && !strcmp(c->ref, "a  i | u") && d &&
              !strcmp(d->ref, "say \"q\"") && !none,
          "eval: CSV references (BOM, header, CRLF, quotes, an empty line) and the file's path, name and stem (%s)", err);
    ev_refs_free(&r);
    const char dup[] = "a.wav,1:1\nb.wav,1:2\na.wav,1:3\n", bad[] = "a.wav,1:1\njust-a-name\n";
    tf_write_file(p, dup, sizeof dup - 1);
    CHECK(ev_load_refs(p, &r, err, sizeof err) && strstr(err, "line 3"), "eval: a file listed twice is refused (%s)", err);
    ev_refs_free(&r);
    tf_write_file(p, bad, sizeof bad - 1);
    CHECK(ev_load_refs(p, &r, err, sizeof err) && strstr(err, "line 2"), "eval: a row without a reference is refused (%s)", err);
    ev_refs_free(&r);
    free(p);
    p = tf_path_join(work, "refs.jsonl");
    const char jl[] = "{\"file\": \"a\\u0628.wav\", \"reference\": \"1:1\", \"extra\": [1, {\"x\": \"}\"}], \"n\": -2.5e3}\n\n"
                      "{\"reference\":\"2:3\",\"file\":\"b\\\\c.wav\",\"ok\":true}\r\n";
    tf_write_file(p, jl, sizeof jl - 1);
    const int jrc = ev_load_refs(p, &r, err, sizeof err);
    EvRef *ja = ev_ref_find(&r, "a\xd8\xa8.wav"), *jb = ev_ref_find(&r, "b/c.wav");
    CHECK(!jrc && r.n == 2 && ja && !strcmp(ja->ref, "1:1") && jb && !strcmp(jb->ref, "2:3"), "eval: JSONL references (\\u escapes, other keys) (%s)",
          err);
    ev_refs_free(&r);
    const char jbad[] = "{\"file\": \"a.wav\"}\n";
    tf_write_file(p, jbad, sizeof jbad - 1);
    CHECK(ev_load_refs(p, &r, err, sizeof err) && strstr(err, "line 1"), "eval: a JSONL line without a reference is refused (%s)", err);
    ev_refs_free(&r);
    free(p);
  }
  /* JSON strings in ASCII */
  {
    EvBuf b = {0};
    ev_json_str(&b, "a\"b\\c\n\xd8\xa8\xf0\x9f\x98\x80\xff!");
    CHECK(!strcmp(b.p, "\"a\\\"b\\\\c\\u000a\\u0628\\ud83d\\ude00\\ufffd!\""), "eval: JSON escapes: %s", b.p);
    free(b.p);
  }
  /* WAV: 16-bit stereo after an odd chunk; 24-bit extensible; float 32 of unknown length; 8-bit; float 64 stereo */
  {
    char *p = tf_path_join(work, "t.wav");
    EvAudio a;
    char err[256] = "";
    int16_t s16[600];
    for (int i = 0; i < 300; ++i) s16[2 * i] = (int16_t)(i * 37 - 3000), s16[2 * i + 1] = (int16_t)(1000 - i * 11);
    ev_t_wav(p, 1, 16, 2, 44100, s16, sizeof s16, 0, 1, 0);
    int same = !ev_read_audio(p, &a, err, sizeof err) && a.n == 300 && a.rate == 44100 && a.channels == 2;
    for (int i = 0; same && i < 300; ++i) same = a.x[i] == (float)(((double)s16[2 * i] / 32768.0 + (double)s16[2 * i + 1] / 32768.0) / 2);
    CHECK(same, "eval: WAV 16-bit stereo after an odd chunk (%s)", err);
    free(a.x);
    uint8_t s24[3 * 500];
    int32_t v24[500];
    for (int i = 0; i < 500; ++i) {
      v24[i] = (int32_t)((i * 40001LL) % 16000000) - 8000000;
      for (int k = 0; k < 3; ++k) s24[3 * i + k] = (uint8_t)((uint32_t)v24[i] >> (8 * k));
    }
    ev_t_wav(p, 1, 24, 1, 22050, s24, sizeof s24, 1, 0, 0);
    same = !ev_read_audio(p, &a, err, sizeof err) && a.n == 500 && a.rate == 22050 && a.channels == 1;
    for (int i = 0; same && i < 500; ++i) same = a.x[i] == (float)((double)v24[i] / 8388608.0);
    CHECK(same, "eval: WAV 24-bit, WAVE_FORMAT_EXTENSIBLE (%s)", err);
    free(a.x);
    float f32[400];
    for (int i = 0; i < 400; ++i) f32[i] = (float)(i % 100) / 100.0f - 0.5f;
    ev_t_wav(p, 3, 32, 1, 16000, f32, sizeof f32, 0, 0, 1);
    same = !ev_read_audio(p, &a, err, sizeof err) && a.n == 400 && a.rate == 16000;
    for (int i = 0; same && i < 400; ++i) same = a.x[i] == f32[i];
    CHECK(same, "eval: WAV float 32 with an unknown data length (%s)", err);
    free(a.x);
    uint8_t u8[256];
    for (int i = 0; i < 256; ++i) u8[i] = (uint8_t)i;
    ev_t_wav(p, 1, 8, 1, 8000, u8, sizeof u8, 0, 0, 0);
    same = !ev_read_audio(p, &a, err, sizeof err) && a.n == 256 && a.rate == 8000;
    for (int i = 0; same && i < 256; ++i) same = a.x[i] == (float)((i - 128) / 128.0);
    CHECK(same, "eval: WAV 8-bit (%s)", err);
    free(a.x);
    double f64[200];
    for (int i = 0; i < 200; ++i) f64[i] = (i % 2 ? -0.25 : 0.75) * (i / 200.0);
    ev_t_wav(p, 3, 64, 2, 48000, f64, sizeof f64, 1, 0, 0);
    same = !ev_read_audio(p, &a, err, sizeof err) && a.n == 100 && a.rate == 48000 && a.channels == 2;
    for (int i = 0; same && i < 100; ++i) same = a.x[i] == (float)((f64[2 * i] + f64[2 * i + 1]) / 2);
    CHECK(same, "eval: WAV float 64 stereo, extensible (%s)", err);
    free(a.x);
    tf_write_file(p, "OggS not a wav", 14);
    CHECK(ev_read_audio(p, &a, err, sizeof err) && strstr(err, "convert"), "eval: what is not WAV, FLAC or raw is refused (%s)", err);
    free(p);
    p = tf_path_join(work, "t.pcm");
    tf_write_file(p, f32, sizeof f32);
    same = !ev_read_audio(p, &a, err, sizeof err) && a.n == 400 && a.rate == 16000 && a.channels == 1;
    for (int i = 0; same && i < 400; ++i) same = a.x[i] == f32[i];
    CHECK(same, "eval: raw float32 at 16 kHz (%s)", err);
    free(a.x);
    free(p);
  }
  /* FLAC from ffmpeg's encoder */
  {
    int ff = 0;
    ff += ev_t_flac_fixture(data, "s16_44k_stereo.flac", 44100, 16, 1, 2205, 40, 1, 1);
    ff += ev_t_flac_fixture(data, "s24_48k_mono.flac", 48000, 24, 0, 2400, 10000, 2, 1);
    ff += ev_t_flac_fixture(data, "s16_8k_wasted.flac", 8000, 16, 0, 1000, 40, 3, 4);
    ff += ev_t_flac_fixture(data, "s24_16k_stereo.flac", 16000, 24, 1, 800, 10000, 4, 1);
    printf("eval: FLAC from ffmpeg's encoder %d/4\n", ff);
  }
  printf("eval_units: %s\n", fails ? "FAIL" : "all ok");
  return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ the tool with the model */
/* the plain table, and the reference units its edits fall on (the same rule as the tool: a match or substitution,
 * then a deletion, then an insertion; an insertion counts against the unit before it) */
static int ev_t_lev(const int *a, int na, const int *b, int nb, unsigned char *bad) {
  int *t = (int *)malloc(sizeof(int) * (size_t)(na + 1) * (nb + 1));
#define EV_T(i, j) t[(size_t)(i) * (nb + 1) + (j)]
  for (int i = 0; i <= na; ++i) EV_T(i, 0) = i;
  for (int j = 0; j <= nb; ++j) EV_T(0, j) = j;
  for (int i = 1; i <= na; ++i)
    for (int j = 1; j <= nb; ++j) {
      int v = EV_T(i - 1, j - 1) + (a[i - 1] != b[j - 1]);
      if (EV_T(i - 1, j) + 1 < v) v = EV_T(i - 1, j) + 1;
      if (EV_T(i, j - 1) + 1 < v) v = EV_T(i, j - 1) + 1;
      EV_T(i, j) = v;
    }
  for (int i = na, j = nb; i > 0 || j > 0;) {
    int m = i && j ? 0 : i ? 1 : 2, best = i && j ? EV_T(i - 1, j - 1) + (a[i - 1] != b[j - 1]) : 1 << 30;
    if (i && EV_T(i - 1, j) + 1 < best) best = EV_T(i - 1, j) + 1, m = 1;
    if (j && EV_T(i, j - 1) + 1 < best) m = 2;
    if (m == 0) {
      if (a[i - 1] != b[j - 1]) bad[i - 1] = 1;
      i--, j--;
    } else if (m == 1) {
      bad[i - 1] = 1;
      i--;
    } else {
      if (na) bad[i ? i - 1 : 0] = 1;
      j--;
    }
  }
  const int d = EV_T(na, nb);
#undef EV_T
  free(t);
  return d;
}
static int ev_t_lines(const char *path, TfJson **lines, int max) {
  char *t = NULL;
  size_t n = 0;
  if (tf_read_file(path, &t, &n)) return -1;
  int k = 0, ascii = 1;
  for (char *p = t; *p && k < max;) {
    char *e = strchr(p, '\n');
    if (e) *e = 0;
    for (const char *q = p; *q; ++q) ascii = ascii && (unsigned char)*q < 0x80; /* the lines are plain ASCII */
    if (*p) lines[k++] = tf_json_parse(p, strlen(p), NULL);
    p = e ? e + 1 : p + strlen(p);
  }
  free(t);
  return ascii ? k : -1;
}
static int ev_t_isnum(const TfJson *j) { return j && j->type == TJ_NUM; }
static int ev_t_ed_ok(const TfJson *e) {
  const char *const k[5] = {"ref", "hyp", "sub", "del", "ins"};
  for (int i = 0; i < 5; ++i)
    if (!ev_t_isnum(tf_json_get(e, k[i])) || !tf_json_is_int(tf_json_get(e, k[i]))) return 0;
  const double ref = tf_json_get(e, "ref")->num, sum = tf_json_get(e, "sub")->num + tf_json_get(e, "del")->num + tf_json_get(e, "ins")->num;
  const TfJson *r = tf_json_get(e, "rate");
  return ref ? ev_t_isnum(r) && fabs(r->num - sum / ref) < 1e-6 && tf_json_get(e, "hyp")->num == ref - tf_json_get(e, "del")->num + tf_json_get(e, "ins")->num
             : r && r->type == TJ_NULL;
}
static int ev_t_wer_ok(const TfJson *e) {
  const TfJson *ref = tf_json_get(e, "ref"), *wrong = tf_json_get(e, "wrong"), *r = tf_json_get(e, "rate");
  if (!ev_t_isnum(ref) || !ev_t_isnum(wrong) || wrong->num > ref->num || wrong->num < 0) return 0;
  return ref->num ? ev_t_isnum(r) && fabs(r->num - wrong->num / ref->num) < 1e-6 : r && r->type == TJ_NULL;
}
/* one file's line: every field of the tool's header comment, with its type */
static int ev_t_line_ok(const TfJson *j, int with_ref) {
  const TfJson *tr = tf_json_get(j, "transcript"), *u = tf_json_get(tr, "units"), *w = tf_json_get(tr, "words"), *hd = tf_json_get(j, "heads");
  int ok = tf_json_str(tf_json_get(j, "file")) && ev_t_isnum(tf_json_get(j, "duration_s")) && tf_json_get(j, "duration_s")->num > 0 &&
           ev_t_isnum(tf_json_get(j, "sample_rate")) && ev_t_isnum(tf_json_get(j, "channels")) && tf_json_get(j, "limit") &&
           tf_json_get(j, "limit")->type == TJ_BOOL && !tf_json_get(j, "limit")->boolean && ev_t_isnum(tf_json_get(j, "processing_s")) &&
           ev_t_isnum(tf_json_get(j, "rtf")) && tf_json_str(tf_json_get(tr, "text")) && u && u->type == TJ_ARR && u->n > 0 && w && w->type == TJ_ARR &&
           w->n > 0 && hd && hd->type == TJ_OBJ && !tf_json_get(j, "error");
  const double dur_ms = ok ? tf_json_get(j, "duration_s")->num * 1000 + 40 : 0;
  for (int i = 0; ok && i < u->n; ++i) {
    const TfJson *x = &u->kids[i], *c = tf_json_get(x, "confidence");
    ok = ev_t_isnum(tf_json_get(x, "id")) && tf_json_str(tf_json_get(x, "unit")) && ev_t_isnum(tf_json_get(x, "start_ms")) &&
         ev_t_isnum(tf_json_get(x, "end_ms")) && tf_json_get(x, "start_ms")->num < tf_json_get(x, "end_ms")->num &&
         tf_json_get(x, "end_ms")->num <= dur_ms && c && (c->type == TJ_NUM || c->type == TJ_NULL);
  }
  for (int i = 0; ok && i < w->n; ++i) {
    const TfJson *x = &w->kids[i];
    ok = tf_json_str(tf_json_get(x, "text")) && ev_t_isnum(tf_json_get(x, "start_ms")) && ev_t_isnum(tf_json_get(x, "end_ms")) &&
         tf_json_get(x, "start_ms")->num < tf_json_get(x, "end_ms")->num && (!i || tf_json_get(x, "start_ms")->num >= tf_json_get(&w->kids[i - 1], "end_ms")->num);
  }
  /* the SDK's default heads besides log_probs: two ctc heads, gem's two classes, qalqalah's three, conf */
  const TfJson *gem = tf_json_get(tf_json_get(hd, "gem"), "classes"), *qal = tf_json_get(tf_json_get(hd, "attr_qalqalah"), "classes"),
               *conf = tf_json_get(hd, "conf");
  ok = ok && hd->n == 5 && !tf_json_get(hd, "log_probs") && ev_t_isnum(tf_json_get(tf_json_get(hd, "inter1_log_probs"), "units")) &&
       ev_t_isnum(tf_json_get(tf_json_get(hd, "timing_log_probs"), "units")) && gem && gem->n == 2 && ev_t_isnum(tf_json_get(tf_json_get(gem, "held"), "mean")) &&
       qal && qal->n == 3 && ev_t_isnum(tf_json_get(tf_json_get(qal, "kubra"), "frames")) && ev_t_isnum(tf_json_get(conf, "mean")) &&
       ev_t_isnum(tf_json_get(conf, "frames_over_half"));
  const TfJson *ref = tf_json_get(j, "reference"), *er = tf_json_get(j, "errors");
  if (with_ref)
    ok = ok && ref && tf_json_str(tf_json_get(ref, "source")) && tf_json_str(tf_json_get(ref, "text")) && ev_t_isnum(tf_json_get(ref, "units")) &&
         ev_t_isnum(tf_json_get(ref, "words")) && er && ev_t_ed_ok(tf_json_get(er, "units")) && ev_t_wer_ok(tf_json_get(er, "words")) &&
         tf_json_get(tf_json_get(er, "units"), "ref")->num == tf_json_get(ref, "units")->num &&
         tf_json_get(tf_json_get(er, "words"), "ref")->num == tf_json_get(ref, "words")->num;
  else ok = ok && !ref && !er && !tf_json_get(j, "reference_error");
  return ok;
}

/* the reference data's units of surah:ayah against `hyp`: the reference's units, the edit distance, the words and the
 * words wrong (plain tables, the tool's rule); -1 when the ayah cannot be read */
static int ev_t_expect(TfReference *refd, int bar, int surah, int ayah, const int *hyp, int nh, int *nr_out, int *dist, int *words, int *wrong) {
  TfUnits ru;
  if (!refd || tf_reference_units(refd, surah, ayah, &ru)) return -1;
  int *rr = (int *)malloc(sizeof(int) * ((size_t)ru.n + 1)), *rwd = (int *)malloc(sizeof(int) * ((size_t)ru.n + 1)), nr = 0;
  for (int q = 0; q < ru.n; ++q)
    if (ru.ids[q] != bar) rr[nr] = ru.ids[q], rwd[nr++] = ru.word[q];
  unsigned char *badu = (unsigned char *)calloc((size_t)nr + 1, 1);
  *dist = ev_t_lev(rr, nr, hyp, nh, badu);
  *words = *wrong = 0;
  for (int q = 0; q < nr;) {
    int e = q, x = 0;
    while (e < nr && rwd[e] == rwd[q]) x |= badu[e++];
    (*words)++, *wrong += x, q = e;
  }
  *nr_out = nr;
  free(rr);
  free(rwd);
  free(badu);
  return 0;
}
static int ev_t_counts_ok(const TfJson *j, int nr, int nh, int dist, int words, int wrong, long long *ue, long long *we) {
  const TfJson *eu = tf_json_get(tf_json_get(j, "errors"), "units"), *ew = tf_json_get(tf_json_get(j, "errors"), "words");
  *ue = eu ? (long long)(tf_json_get(eu, "sub")->num + tf_json_get(eu, "del")->num + tf_json_get(eu, "ins")->num) : -1;
  *we = ew ? (long long)tf_json_get(ew, "wrong")->num : -1;
  return eu && ew && *ue == dist && (long long)tf_json_get(eu, "ref")->num == nr && (long long)tf_json_get(eu, "hyp")->num == nh && *we == wrong &&
         (long long)tf_json_get(ew, "ref")->num == words;
}

#define EV_T_CLIPS 64
static int eval30_tests(const char *local, const char *key, const char *clips, const char *expected, const char *work, const char *configs, const char *variant) {
  char full[1100], store[1100], csv[1100], jsonl[1100], out1[1100], out2[1100], in[1100];
  if (!variant) variant = "full";
  snprintf(full, sizeof full, "%s/%s-decoded/sdk/v4.0", local, variant);
  snprintf(in, sizeof in, "%s/audio", clips);
  tf__remove_tree(work);
  tf_mkdirs(work);
  snprintf(store, sizeof store, "%s/store", work);
  snprintf(csv, sizeof csv, "%s/refs.csv", work);
  snprintf(jsonl, sizeof jsonl, "%s/refs.jsonl", work);
  snprintf(out1, sizeof out1, "%s/out1.jsonl", work);
  snprintf(out2, sizeof out2, "%s/out2.jsonl", work);
  /* the clips and their expected tokens (the engine's c16_l64 audio rows), and a reference each: the clip's name
   * ends in SSSAAA, the surah and the ayah */
  char names[EV_T_CLIPS][64];
  int want[EV_T_CLIPS][512], nwant[EV_T_CLIPS], sur[EV_T_CLIPS], aya[EV_T_CLIPS], nc = 0;
  {
    char *t = NULL;
    size_t n = 0;
    if (tf_read_file(expected, &t, &n)) {
      fprintf(stderr, "eval30: cannot read %s\n", expected);
      return 2;
    }
    for (char *p = t; *p && nc < EV_T_CLIPS;) {
      char *e = strchr(p, '\n');
      if (e) *e = 0;
      TfJson *j = *p ? tf_json_parse(p, strlen(p), NULL) : NULL;
      const char *clip = tf_json_str(tf_json_get(j, "clip"));
      const TfJson *tk = tf_json_get(j, "tokens");
      const size_t cl = clip ? strlen(clip) : 0;
      if (clip && tk && tk->type == TJ_ARR && tk->n <= 512 && cl > 7 && cl < 60) {
        snprintf(names[nc], sizeof names[nc], "%s", clip);
        nwant[nc] = tk->n;
        for (int q = 0; q < tk->n; ++q) want[nc][q] = (int)tk->kids[q].num;
        sur[nc] = atoi(clip + cl - 6) / 1000;
        aya[nc] = atoi(clip + cl - 3);
        nc++;
      }
      tf_json_free(j);
      p = e ? e + 1 : p + strlen(p);
    }
    free(t);
  }
  CHECK(nc == 35, "eval30: %d clips in %s, want 35", nc, expected);
  EvBuf rc = {0};
  ev_puts(&rc, "file,reference\n");
  for (int k = 0; k < nc; ++k) ev_printf(&rc, "%s.pcm,%d:%d\n", names[k], sur[k], aya[k]);
  tf_write_file(csv, rc.p, rc.n);
  free(rc.p);

  /* the stub of model30: signed configs, a CDN serving the stored folder */
  g_ap.fx = read_json_file(configs);
  snprintf(g_m30.cdn, sizeof g_m30.cdn, "%s/%s/sdk/v4.0", local, variant);
  {
    TfModel *probe = tf__model_open(full, &(TfModelOpen){.trusted_key_hex = key, .no_engine = 1});
    snprintf(g_m30.build, sizeof g_m30.build, "%s", probe ? tf_model_build(probe) : "");
    tf_model_close(probe);
  }
  if (!g_ap.fx || strcmp(g_m30.build, tf_json_str(tf_json_get(g_ap.fx, "build")))) {
    fprintf(stderr, "eval30: the configs name build %s and the folder is %s (test/make_model_configs.py with local30.json and the variant)\n",
            g_ap.fx ? tf_json_str(tf_json_get(g_ap.fx, "build")) : "(none)", g_m30.build);
    return 2;
  }
  tf_http_set_transport(m30_stub, NULL);
  tf__now_override = 1794355200 + 9 * 3600;
  tf__model_key_override = key;
  g_ap.mode = 4;
  g_ap.serve = "model";

  /* 1. the tool downloads the model and the reference data through the Client, as an app does */
  TfClient *c = m30_client(store, 0);
  char *argv1[] = {"tajweedformer-eval", in, "--refs", csv, "--out", out1, NULL};
  const int rc1 = c ? ev_main(6, argv1, c) : -1;
  CHECK(rc1 == 0, "eval30: the first run (%d)", rc1);
  TfJson *L1[EV_T_CLIPS + 2] = {0};
  const int n1 = ev_t_lines(out1, L1, EV_T_CLIPS + 2);
  CHECK(n1 == nc + 1, "eval30: %d lines, want %d file lines and a total, in plain ASCII", n1, nc + 1);
  TfModel *m = c ? tf_client_model(c, NULL) : NULL;
  TfReference *refd = m ? tf_model_reference(m) : NULL;
  const int bar = m ? tf_model_tokens(m)->word_bar : 1;
  int schema = 0, same_units = 0, same_err = 0;
  long long tu[5] = {0}, tw[2] = {0};
  for (int i = 0; i < n1 - 1 && m; ++i) {
    const TfJson *j = L1[i];
    const char *file = tf_json_str(tf_json_get(j, "file"));
    int k = -1;
    for (int q = 0; q < nc && file; ++q)
      if (!strncmp(file, names[q], strlen(names[q])) && !strcmp(file + strlen(names[q]), ".pcm")) k = q;
    const int good = k >= 0 && ev_t_line_ok(j, 1);
    CHECK(good, "eval30: the line of %s has every field", file ? file : "(no file)");
    schema += good;
    if (k < 0) continue;
    /* the units are the engine's expected tokens for the clip (V4 emits no word bars) */
    const TfJson *u = tf_json_get(tf_json_get(j, "transcript"), "units");
    int eq = u && u->n == nwant[k];
    for (int q = 0; eq && q < u->n; ++q) eq = (int)tf_json_get(&u->kids[q], "id")->num == want[k][q];
    CHECK(eq, "eval30: %s: the units are the engine's expected tokens", names[k]);
    same_units += eq;
    /* the error counts against a plain edit distance of the reference data's units */
    int nr = 0, dist = -1, words = 0, wrong = 0;
    long long ue = -1, we = -1;
    const int urc = ev_t_expect(refd, bar, sur[k], aya[k], want[k], nwant[k], &nr, &dist, &words, &wrong);
    const int eok = !urc && ev_t_counts_ok(j, nr, nwant[k], dist, words, wrong, &ue, &we);
    CHECK(eok, "eval30: %s: the error counts (units %lld, want %d; words %lld, want %d of %d)", names[k], ue, dist, we, wrong, words);
    same_err += eok;
    const TfJson *eu = tf_json_get(tf_json_get(j, "errors"), "units"), *ew = tf_json_get(tf_json_get(j, "errors"), "words");
    if (eu && ew) {
      const char *const kk[5] = {"ref", "hyp", "sub", "del", "ins"};
      for (int q = 0; q < 5; ++q) tu[q] += (long long)tf_json_get(eu, kk[q])->num;
      tw[0] += (long long)tf_json_get(ew, "ref")->num, tw[1] += (long long)tf_json_get(ew, "wrong")->num;
      if (ue || we) printf("eval30: %-14s %3d:%-3d units %3d ref, %lld edits; words %2d ref, %lld wrong\n", names[k], sur[k], aya[k], nr, ue, words, we);
    }
  }
  const TfJson *T = n1 == nc + 1 ? tf_json_get(L1[nc], "total") : NULL;
  int tok = T && tf_json_get(T, "files")->num == nc && tf_json_get(T, "evaluated")->num == nc && tf_json_get(T, "failed")->num == 0 &&
            tf_json_get(T, "with_reference")->num == nc && ev_t_ed_ok(tf_json_get(T, "units")) && ev_t_wer_ok(tf_json_get(T, "words")) &&
            !strcmp(tf_json_str(tf_json_get(T, "sdk")), TF_SDK_VERSION) && !strcmp(tf_json_str(tf_json_get(T, "model")), "4.0.0") &&
            !strcmp(tf_json_str(tf_json_get(T, "context")), "c16_l64");
  for (int q = 0; tok && q < 5; ++q) {
    const char *const kk[5] = {"ref", "hyp", "sub", "del", "ins"};
    tok = (long long)tf_json_get(tf_json_get(T, "units"), kk[q])->num == tu[q];
  }
  tok = tok && (long long)tf_json_get(tf_json_get(T, "words"), "ref")->num == tw[0] && (long long)tf_json_get(tf_json_get(T, "words"), "wrong")->num == tw[1];
  CHECK(tok, "eval30: the total line sums the files");
  printf("eval30: run 1 (the stub's API and CDN): %d/%d lines with every field, %d/%d transcripts equal the engine's expected tokens, %d/%d error "
         "counts equal a plain edit distance\n",
         schema, nc, same_units, nc, same_err, nc);
  if (T)
    printf("eval30: total UER %.2f%% (%lld edits over %lld units: sub %lld, del %lld, ins %lld), WER %.2f%% (%lld of %lld words); RTF %.3f\n",
           100.0 * (tu[2] + tu[3] + tu[4]) / (tu[0] ? tu[0] : 1), tu[2] + tu[3] + tu[4], tu[0], tu[2], tu[3], tu[4], 100.0 * tw[1] / (tw[0] ? tw[0] : 1),
           tw[1], tw[0], tf_json_get(T, "rtf")->num);
  /* the units text of the first clip's reference, as its line writes it */
  char unit_ref[4096] = "";
  if (n1 == nc + 1) snprintf(unit_ref, sizeof unit_ref, "%s", tf_json_str(tf_json_get(tf_json_get(L1[0], "reference"), "text")));
  const char *first = n1 == nc + 1 ? tf_json_str(tf_json_get(L1[0], "file")) : "";
  if (m) tf_model_close(m);
  tf_client_destroy(c);

  /* 2. no network at all (the stub answers nothing), a new Client on the same storage, JSONL references: the first
   * clip's as units text, the second line's clip held against the next ayah, the others by their stem */
  const char *second = n1 == nc + 1 ? tf_json_str(tf_json_get(L1[1], "file")) : "";
  int k2 = -1;
  EvBuf jl = {0};
  for (int k = 0; k < nc; ++k) {
    char f[80];
    snprintf(f, sizeof f, "%s.pcm", names[k]);
    if (!strcmp(f, second)) {
      k2 = k;
      ev_printf(&jl, "{\"file\": \"%s\", \"reference\": \"%d:%d\"}\n", names[k], sur[k], aya[k] + 1);
    } else if (!strcmp(f, first)) {
      ev_puts(&jl, "{\"file\": ");
      ev_json_str(&jl, f);
      ev_puts(&jl, ", \"reference\": ");
      ev_json_str(&jl, unit_ref);
      ev_puts(&jl, "}\n");
    } else {
      ev_printf(&jl, "{\"file\": \"%s\", \"reference\": \"%d:%d\"}\n", names[k], sur[k], aya[k]);
    }
  }
  tf_write_file(jsonl, jl.p, jl.n);
  free(jl.p);
  g_ap.mode = 0;
  TfClient *c2 = m30_client(store, 0);
  char *argv2[] = {"tajweedformer-eval", in, "--refs", jsonl, "--out", out2, NULL};
  const int rc2 = c2 ? ev_main(6, argv2, c2) : -1;
  CHECK(rc2 == 0, "eval30: the second run, offline (%d)", rc2);
  TfJson *L2[EV_T_CLIPS + 2] = {0};
  const int n2 = ev_t_lines(out2, L2, EV_T_CLIPS + 2);
  int same = n2 == nc + 1 && n1 == nc + 1;
  for (int i = 0; same && i < nc; ++i) {
    TfBuf a, b;
    tf_buf_init(&a);
    tf_buf_init(&b);
    tf_json_write(&a, tf_json_get(L1[i], "transcript"));
    tf_json_write(&b, tf_json_get(L2[i], "transcript"));
    same = a.p && b.p && !strcmp(a.p, b.p) && ev_t_line_ok(L2[i], 1);
    tf_buf_free(&a);
    tf_buf_free(&b);
    tf_buf_init(&a);
    tf_buf_init(&b);
    tf_json_write(&a, tf_json_get(L1[i], "errors"));
    tf_json_write(&b, tf_json_get(L2[i], "errors"));
    same = same && a.p && b.p && (i == 1 || !strcmp(a.p, b.p)); /* the second line: another reference, below */
    tf_buf_free(&a);
    tf_buf_free(&b);
    CHECK(same, "eval30: offline, %s: the same transcript and error counts", tf_json_str(tf_json_get(L1[i], "file")));
  }
  CHECK(same && !strcmp(tf_json_str(tf_json_get(tf_json_get(L2[0], "reference"), "source")), "units"), "eval30: offline: the first reference as units text");
  /* the clip held against the next ayah: errors, counted as a plain edit distance counts them */
  {
    TfModel *m2 = c2 && k2 >= 0 ? tf_client_model(c2, NULL) : NULL;
    int nr = 0, dist = -1, words = 0, wrong = 0;
    long long ue = -1, we = -1;
    const int ok2 = m2 && n2 == nc + 1 &&
                    !ev_t_expect(tf_model_reference(m2), tf_model_tokens(m2)->word_bar, sur[k2], aya[k2] + 1, want[k2], nwant[k2], &nr, &dist, &words, &wrong) &&
                    ev_t_counts_ok(L2[1], nr, nwant[k2], dist, words, wrong, &ue, &we) && dist > 0;
    CHECK(ok2, "eval30: offline, %s against %d:%d: units %lld edits (want %d), words %lld wrong (want %d)", k2 >= 0 ? names[k2] : "?", k2 >= 0 ? sur[k2] : 0,
          k2 >= 0 ? aya[k2] + 1 : 0, ue, dist, we, wrong);
    if (k2 >= 0)
      printf("eval30: offline, %s held against %d:%d: %lld edits over %d units, %lld of %d words wrong, as a plain edit distance counts them\n",
             names[k2], sur[k2], aya[k2] + 1, ue, nr, we, words);
    if (m2) tf_model_close(m2);
  }
  printf("eval30: run 2 (no network): %s\n", same ? "the same transcripts and error counts, one reference as units text" : "DIFFERENT");
  if (c2) tf_client_destroy(c2);
  for (int i = 0; i < EV_T_CLIPS + 2; ++i) tf_json_free(L1[i]), tf_json_free(L2[i]);
  tf_http_set_transport(NULL, NULL);
  tf__now_override = 0;
  tf__model_key_override = NULL;
  printf("eval30: %s\n", fails ? "FAIL" : "all ok");
  return fails ? 1 : 0;
}
