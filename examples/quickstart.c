/* The README's quick start: the developer key in, the microphone in (raw samples on stdin), what the model heard
 * out. The microphone is the system's recorder piped in, because capture is a platform API this library does
 * not wrap:
 *
 *   Linux:    arecord -q -f FLOAT_LE -r 16000 -c 1 -t raw | ./quickstart qlk_v1_...
 *   Windows:  ffmpeg -loglevel error -f dshow -i audio="<your microphone>" -ac 1 -ar 16000 -f f32le - | quickstart qlk_v1_...
 *             (ffmpeg -list_devices true -f dshow -i dummy  lists the microphones)
 *
 * test/quickstart_check.py keeps the README's code equal to this file (between the README marks). */
/* README:begin */
#include <stdio.h>
#include <string.h>
#include "tajweedformer.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

typedef struct {
  TfModel *model;
  TfStream *stream;
} Take;

static void on_chunk(void *user, const TfChunk *chunk) {   /* every 320 ms of audio: what the model heard so far */
  const Take *take = user;
  TfDecoded *heard = tf_stream_decode(take->stream);
  const TfToken *t;
  const int n = tf_decoded_tokens(heard, &t);
  printf("%6d ms:", chunk->end_ms);
  for (int i = 0; i < n; ++i) printf(" %s", tf_model_token_name(take->model, t[i].id));
  printf("\n");
  fflush(stdout);
  tf_decoded_free(heard);
}

int main(int argc, char **argv) {
  if (argc < 2) return fprintf(stderr, "usage: quickstart qlk_v1_... < mono float samples at 16 kHz\n"), 2;

  /* 1. the key: Quran Lab's signed config names the model revision this app gets */
  TfClientOptions co = tf_client_default_options();
  co.key = argv[1];
  co.app_id = "com.example.quickstart";      /* your app's id, as registered for the key */
  co.app_version = "1.0";
  co.storage_dir = "quranlab";               /* the SDK's own state */
  TfClient *client = tf_client_create(&co);
  if (!client || tf_client_launch(client)) return fprintf(stderr, "portal: %s\n", tf_last_error()), 1;
  const char *config = tf_client_config_json(client);   /* verified; "recommended":{...,"revision":"..."} */
  const char *at = config ? strstr(config, "\"recommended\"") : NULL;
  char revision[80] = "";
  if (at && (at = strstr(at, "\"revision\"")) && (at = strchr(at + 10, '"'))) {
    const size_t len = strcspn(at + 1, "\"");
    if (len < sizeof revision) memcpy(revision, at + 1, len);
  }
  if (!revision[0]) return fprintf(stderr, "the config recommends no model for this key\n"), 1;

  /* 2. the model: downloaded once (56.5 MB), resumed if interrupted, every file checked */
  TfDownloadOptions dl = tf_download_default_options();
  dl.key = argv[1];
  dl.revision = revision;
  dl.dest = "model";
  dl.contexts = "c16_l64";                   /* the default context: 320 ms chunks */
  if (tf_download_model(&dl)) return fprintf(stderr, "download: %s\n", tf_last_error()), 1;
  TfModel *model = tf_model_open("model", NULL);
  if (!model) return fprintf(stderr, "model: %s\n", tf_last_error()), 1;

  /* 3. audio in, model output out */
  Take take = {model, NULL};
  TfStreamOptions so = tf_stream_default_options();
  so.on_chunk = on_chunk;
  so.user = &take;
  take.stream = tf_stream_create(model, &so);
  if (!take.stream) return fprintf(stderr, "stream: %s\n", tf_last_error()), 1;
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
#endif
  float samples[1600];                       /* 100 ms at 16 kHz */
  size_t n;
  while ((n = fread(samples, sizeof *samples, 1600, stdin)) > 0) tf_stream_push_audio(take.stream, samples, (int)n);
  tf_stream_finish(take.stream);             /* the end of the take: the last chunks arrive */

  tf_stream_destroy(take.stream);
  tf_model_close(model);
  tf_client_destroy(client);
  return 0;
}
/* README:end */
