/* Speech and silence from the model's own output (SPEC 6.2 and 13.2; the shared vectors' speech.json). One
 * definition for the events, chunk.speech and the telemetry sums.
 *
 * - A 40 ms frame is ACTIVE when the greedy id of log_probs is non-blank on it, or the greedy id of
 *   timing_log_probs is non-blank on one of its two 20 ms frames. (A greedy id: the largest log-prob, the lowest
 *   id on ties; blank is 0 in both heads.) A speech frame is an active frame, a silence frame an inactive one.
 *   There is no padding around a token.
 * - chunk.speech: the chunk holds an active frame.
 * - speechStarted(atMs): at an active frame while the state is silence; atMs = 40 * that frame.
 * - speechEnded(atMs, silenceMs): when W inactive frames in a row have arrived after the last active frame L,
 *   that is with frame L + W. W = ceil(window / 40), the window being end_on_silence_ms when set, else 1500 ms
 *   (W = 38). atMs = 40 * (L + 1); silenceMs = 40 * W.
 * - With end_on_silence_ms the take ends right there (TfSpeech.ended).
 * Events are decided frame by frame as frames arrive, never at the end of the take. All of the timing is in
 * tf__speech_frame. No rule about recitation is applied here: this is the model reporting what it heard.
 *
 * Why the timing head counts: log_probs shows blank while a sound is held (a madd of six counts lasts well over
 * two seconds), and the timing head keeps hearing it. The first rule (log_probs alone, 12 frames of padding
 * either side of a token, speechEnded 0.96 s after the window) ended takes inside recitation: on the fixture's
 * 48 takes a 1500 ms window passed before the last token in one take, a 500 ms window in 13. With this rule
 * the longest inactive run inside a take is 440 ms and none does. */
#include <string.h>

#include "tf_internal.h"

void tf__speech_init(TfSpeech *s, int end_on_silence_ms) {
  memset(s, 0, sizeof *s);
  const int window = end_on_silence_ms > 0 ? end_on_silence_ms : 1500;
  s->window = (window + 39) / 40;
  s->end_on = end_on_silence_ms > 0;
  s->last = -1;
}

int tf__speech_active(int id, int timing_a, int timing_b) { return id != 0 || timing_a != 0 || timing_b != 0; }

int tf__speech_frame(TfSpeech *s, int active, TfSpeechEvent *ev) {
  const int t = s->frames++;
  int n = 0;
  s->active = active != 0;
  if (active) {
    if (!s->in_speech) {
      s->in_speech = 1;
      ev->type = TF_EVENT_SPEECH_STARTED;
      ev->at_ms = 40 * t;
      ev->silence_ms = 0;
      n = 1;
    }
    s->last = t;
  } else if (s->in_speech && t == s->last + s->window) {
    s->in_speech = 0;
    ev->type = TF_EVENT_SPEECH_ENDED;
    ev->at_ms = 40 * (s->last + 1);
    ev->silence_ms = 40 * s->window;
    n = 1;
    if (s->end_on) s->ended = 1;
  }
  return n;
}

int tf__speech_frames(const int *ids, const int *timing, int T) {
  int speech = 0;
  for (int t = 0; t < T; ++t) speech += tf__speech_active(ids[t], timing ? timing[2 * t] : 0, timing ? timing[2 * t + 1] : 0);
  return speech;
}

int tf__token_runs(const int *ids, int T) {
  int n = 0;
  for (int t = 0; t < T; ++t)
    if (ids[t] != 0 && (t == 0 || ids[t - 1] != ids[t])) n++;
  return n;
}
