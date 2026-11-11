/* The example app: one console program that shows, in this order,
 *   a. the model loading, with the download's progress in bytes (tf_client_prefetch, tf_client_model);
 *   b. a sample clip of the model played through tf_session_listen, so output appears with no microphone;
 *   c. reciting: audio from standard input, take after take (each take ends by itself on silence);
 *   d. the tokens the model heard so far, live: settled ones plain, tentative ones in [brackets];
 *   e. the diagnose report as JSON.
 * Nothing here compares a recitation with an expected ayah: the SDK is a pure inference engine.
 *
 * This library has no microphone: the system's recorder is piped in as mono float samples at 16 kHz.
 *   Linux:    arecord -q -f FLOAT_LE -r 16000 -c 1 -t raw | ./quickstart qlk_v1_...
 *   Windows:  ffmpeg -loglevel error -f dshow -i audio="<your microphone>" -ac 1 -ar 16000 -f f32le - | quickstart qlk_v1_...
 *             (ffmpeg -list_devices true -f dshow -i dummy  lists the microphones)
 *
 *   quickstart KEY                 the example: a to e
 *   quickstart KEY --quick         only the README's quick start (the marked region below)
 *   quickstart --folder DIR [HEX]  b to d on a model folder already on disk, without a key or a network
 *                                  (HEX: the public key of a folder signed with a test key)
 *
 * test/quickstart_check.py keeps the README's code equal to the marked region of this file. */
#include <stdio.h>
#include <string.h>
#include "tajweedformer.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* README:begin */
static void on_chunk(void *user, const TfChunk *chunk) { (void)user, puts(chunk->decoded->text); }

static int recite(const char *key) {
  TfClient *client = tf_client_create(&(TfClientOptions){.key = key, .app_id = "com.example.quickstart"});
  TfModel *model = tf_client_model(client, NULL);
  TfSession *session = tf_session_create(model, &(TfSessionOptions){.on_chunk = on_chunk, .end_on_silence_ms = 1500});
  float samples[1600];
  for (size_t n; (n = fread(samples, sizeof *samples, 1600, stdin)) > 0 && tf_session_push(session, samples, (int)n) >= 0;) {}
  return tf_session_finish(session);
}
/* README:end */

/* a. the download's progress */
static void on_progress(void *user, long long bytes_done, long long bytes_total) {
  (void)user;
  fprintf(stderr, "\rmodel: %lld of %lld bytes%s", bytes_done, bytes_total, bytes_done == bytes_total ? "\n" : "");
}

/* d. each chunk: the take so far. The first `settled` tokens will not change any more; the rest still can. */
static void show(void *user, const TfChunk *chunk) {
  const TfDecoded *heard = chunk->decoded;
  (void)user;
  printf("%6d ms %s", chunk->end_ms, chunk->speech ? "speech " : "silence");
  for (int i = 0; i < heard->n_tokens; ++i) printf(i < heard->settled ? " %s" : " [%s]", heard->tokens[i].name);
  printf("\n");
}

static void on_event(void *user, const TfEvent *event) {
  (void)user;
  if (event->type == TF_EVENT_ENDED) fprintf(stderr, "[%s: %s]\n", event->name, event->reason_name);
  else fprintf(stderr, "[%s at %d ms]\n", event->name, event->at_ms);
}

static int failed(const char *what) {
  fprintf(stderr, "%s: %s\n  %s\n  %s\n", what, tf_last_error(), tf_last_error_fix(), tf_error_docs(tf_last_error_code()));
  return 1;
}

int main(int argc, char **argv) {
  if (argc < 2 || (!strcmp(argv[1], "--folder") && argc < 3))
    return fprintf(stderr, "usage: quickstart qlk_v1_... [--quick] < mono float samples at 16 kHz\n"
                           "       quickstart --folder DIR [PUBLIC_KEY_HEX] < mono float samples at 16 kHz\n"), 2;
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
#endif
  if (argc > 2 && !strcmp(argv[2], "--quick")) return recite(argv[1]) < 0 ? failed("quick start") : 0;
  const char *folder = strcmp(argv[1], "--folder") ? NULL : argv[2];

  /* a. the model: downloaded once (resumed when interrupted), verified, opened and warm */
  TfClient *client = folder ? NULL : tf_client_create(&(TfClientOptions){.key = argv[1], .app_id = "com.example.quickstart", .app_version = "1.0"});
  if (!folder && tf_client_prefetch(client, &(TfPrefetchOptions){.on_progress = on_progress}) < 0) return failed("model download");
  TfModel *model = folder ? tf_model_open(folder, &(TfModelOptions){.trusted_key_hex = argc > 3 ? argv[3] : NULL}) : tf_client_model(client, NULL);
  if (!model) return failed("model");
  fprintf(stderr, "model %s, version %s\n", tf_model_id(model), tf_model_version(model));
  const TfSessionOptions options = {.on_chunk = show, .on_event = on_event, .end_on_silence_ms = 1500};

  /* b. play a sample: a real recitation that ships with the model, through the same path as pushed audio */
  TfClip clip;
  if (tf_model_sample_clip(model, 0, &clip) == 0) {
    fprintf(stderr, "sample clip %s, %d ms\n", clip.name, clip.duration_ms);
    TfSession *played = tf_session_listen(model, &clip, &options);
    if (!played) return failed("sample clip");
    tf_session_destroy(played);
  }

  /* c. recite: a take ends by itself after 1.5 s of silence; the next take is a new Session (one line). An app
   * with a window does the same when it returns from the background: tf_client_foreground, then that line. */
  fprintf(stderr, "recite (mono float samples at 16 kHz on standard input)\n");
  TfSession *session = tf_session_create(model, &options);
  float samples[1600];
  for (size_t n; session && (n = fread(samples, sizeof *samples, 1600, stdin)) > 0;) {
    if (tf_session_push(session, samples, (int)n) >= 0) continue;
    tf_session_destroy(session);
    session = tf_session_create(model, &options); /* the next take */
    if (session) tf_session_push(session, samples, (int)n);
  }
  if (!session) return failed("session");
  tf_session_finish(session);
  tf_session_destroy(session);

  /* e. diagnose: one report, safe to paste into an issue */
  if (client) puts(tf_client_diagnose_json(client));
  tf_model_close(model);
  tf_client_destroy(client);
  return 0;
}
