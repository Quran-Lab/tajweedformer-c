/* 4.0.1, Windows: the engine's threads are never power-throttled (tf_qos.c). The calling thread gets high QoS for a
 * push and gets back what it had after it; a thread made after a snapshot (as the Core makes its engine threads inside
 * qlc_model_open and qlc_stream_create) is marked by tf__qos_mark_new; tf__qos_off changes nothing. Elsewhere the calls
 * are no-ops and the test says so.
 *   tf40_tests qos - <work> */
static volatile int g_qos_go, g_qos_seen, g_qos_done;
static void qos_thread(void *arg) {
  (void)arg;
  while (!g_qos_go) tf__sleep_ms(1);
  g_qos_seen = tf__qos_thread_high();
  g_qos_done = 1;
}
static int t_qos(void) {
  const int before = tf__qos_thread_high();
  if (before < 0) {
    printf("qos: this system has no thread power throttling (not Windows 10 1709 or later): skipped\n");
    return done("qos");
  }
  TfQosSave q;
  tf__qos_enter(&q);
  CHECK(tf__qos_thread_high() == 1, "qos: the calling thread runs with high QoS inside a push");
  tf__qos_leave(&q);
  CHECK(tf__qos_thread_high() == before, "qos: the calling thread gets back what it had after the push (%d)", before);
  /* a thread made after the snapshot is marked; the threads that were there are not */
  for (int off = 0; off <= 1; ++off) {
    tf__qos_off = off;
    g_qos_go = g_qos_seen = g_qos_done = 0;
    TfThreadSnap snap;
    tf__qos_snapshot(&snap);
    TfThread th;
    const int started = !tf_thread_start(&th, qos_thread, NULL);
    const int marked = started ? tf__qos_mark_new(&snap) : 0;
    g_qos_go = 1;
    if (started) tf_thread_join(th);
    if (!off)
      CHECK(started && marked >= 1 && g_qos_seen == 1 && tf__qos_thread_high() == before,
            "qos: the thread made after the snapshot runs with high QoS (%d marked, it saw %d), this thread is unchanged", marked, g_qos_seen);
    else
      CHECK(started && marked == 0 && g_qos_seen == 0, "qos: with tf__qos_off nothing is marked (%d, it saw %d)", marked, g_qos_seen);
  }
  tf__qos_off = 0;
  /* the walk is over this process's threads only: a model open must not wait for it (the machine-wide Toolhelp
   * snapshot took 0.4 s a call on a busy PC) */
  const double t0 = tf__now_s();
  TfThreadSnap s2;
  for (int i = 0; i < 20; ++i) tf__qos_snapshot(&s2);
  const double ms = (tf__now_s() - t0) * 1000.0 / 20;
  CHECK(s2.taken && s2.n >= 1 && ms < 20, "qos: a snapshot of this process's %d threads takes %.3f ms", s2.n, ms);
  return done("qos");
}
