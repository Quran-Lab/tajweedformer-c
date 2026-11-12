/* SDK 3.0.1: sharing turned off, or what was shared deleted, while an upload is in flight.
 *
 *   tf_tests sharing_off <work dir> [runs] [first situation, 1 to 19: the table from there on]
 *
 * The situations and the assertion keys are the shared sharing_off.json's (the four SDKs report the same table):
 * a recording is being uploaded, the stub server HOLDS one request so that it never answers by itself (a segment
 * PUT, the upload's start, a batch of upload URLs, the take list), and the learner turns sharing off, or deletes
 * what they shared (the server takes the deletion, or does not); in two cases sharing is turned on again as soon
 * as the call returned. Every case runs 20 times, because a race passes most single runs. The rules (the
 * contract's SPEC 11.7):
 *   R1  after the call returns no byte of audio leaves the device, whatever was in flight
 *   R2  the call returns within 3000 ms even when a request hangs
 *   R3  nothing unsent is left on the device, also after the held request has come back
 *   R4  turning sharing on again afterwards works in the same run
 * A deletion the server took turns sharing off (the learner turns it on again with the switch and is not asked);
 * one the server did not take changes nothing, and the upload carries on.
 * The uploader is one thread and is inside the held request when the call is made, so "a request that starts
 * after the call" is exact here: nothing else can start one. For the same reason
 * `queue_stopped_before_the_server_is_asked` is held to more than its sentence: no upload request started since
 * the call AND the held request had ended when the deletion's first request reached the server.
 *
 * Two rows of this SDK's own follow the shared twelve: the start is held by a transport that CANNOT abort it (the
 * stub does not look at the request's cancel flag; WinHTTP and libcurl can always abort). The call must still
 * return within the bound (it waits 2 s for the uploader, once), delete everything, and the request that comes
 * back later must send nothing and write nothing.
 *
 * The restart situations (the shared file's `restart_cases`, R5): NINE recordings wait in the queue and from the
 * moment of the call no recording can be deleted (a test switch makes every deletion fail: files in use, or the
 * process dying before the stop's deletion ran). A recording made under an earlier consent must never be
 * uploaded, however sharing comes on again, and be deleted as soon as deleting works; and a new process must
 * find sharing off. The process "ends" by closing the client and opening a new one on the same folder. In the
 * last situation the process ends while the server is asked to delete: the stub copies the settings file at
 * that moment, the server refuses (so the queue stays as it was), and the copy is put back before the new
 * client opens: the device exactly as a kill at that moment leaves it.
 * What holds R5 is on disk (a recording's stamp against the count saved with the consent), not a list in memory,
 * so there is no number of recordings at which it stops holding. */
enum {
  OFF_SETUP = 1,         /* the situation could not be built (the held request was never made) */
  OFF_BOUND = 2,         /* returns_within_bound */
  OFF_REQUEST = 4,       /* no_request_after */
  OFF_AUDIO = 8,         /* no_audio_after */
  OFF_DEVICE = 16,       /* nothing_unsent_on_device */
  OFF_RELEASE = 32,      /* still_nothing_after_release */
  OFF_ORDER = 64,        /* queue_stopped_before_the_server_is_asked (delete) */
  OFF_IS_OFF = 128,      /* sharing_is_off_after_an_accepted_delete */
  OFF_ON_AGAIN = 256,    /* on_again_works */
  OFF_OLD_GONE = 512,    /* the_old_recording_is_gone */
  OFF_UNCHANGED = 1024,  /* a_refused_delete_changes_nothing */
  OFF_RESUMES = 2048,    /* queue_resumes */
  OFF_NEVER = 4096,      /* earlier_consent_never_uploaded */
  OFF_LATER = 8192,      /* earlier_consent_deleted */
  OFF_DISK = 16384       /* off_is_on_disk_first */
};
#define OFF_KEYS 15
static const char *const kOffKeys[OFF_KEYS] = {"setup",
                                               "returns_within_bound",
                                               "no_request_after",
                                               "no_audio_after",
                                               "nothing_unsent_on_device",
                                               "still_nothing_after_release",
                                               "queue_stopped_before_the_server_is_asked",
                                               "sharing_is_off_after_an_accepted_delete",
                                               "on_again_works",
                                               "the_old_recording_is_gone",
                                               "a_refused_delete_changes_nothing",
                                               "queue_resumes",
                                               "earlier_consent_never_uploaded",
                                               "earlier_consent_deleted",
                                               "off_is_on_disk_first"};
enum { ACT_OFF, ACT_DELETE, ACT_REFUSED };
enum { LEFT_NONE, LEFT_SAME_RUN, LEFT_RESTART_OFF, LEFT_RESTART_ON, LEFT_DELETED_RESTART_ON, LEFT_ASKED_RESTART_ON };
typedef struct {
  const char *name, *held;
  int act, then_on, deaf, left;
} OffCase;
#define OFF_UPLOAD 12 /* the shared file's situations during an upload */
#define OFF_SHARED 17 /* with its five restart situations; the rest are this SDK's own rows */
static const OffCase kOffCases[] = {{"put held, off", "put", ACT_OFF, 0, 0, 0},
                                    {"put held, delete", "put", ACT_DELETE, 0, 0, 0},
                                    {"start held, off", "start", ACT_OFF, 0, 0, 0},
                                    {"start held, delete", "start", ACT_DELETE, 0, 0, 0},
                                    {"urls held, off", "urls", ACT_OFF, 0, 0, 0},
                                    {"urls held, delete", "urls", ACT_DELETE, 0, 0, 0},
                                    {"takes held, off", "takes", ACT_OFF, 0, 0, 0},
                                    {"takes held, delete", "takes", ACT_DELETE, 0, 0, 0},
                                    {"put held, delete refused", "put", ACT_REFUSED, 0, 0, 0},
                                    {"start held, delete refused", "start", ACT_REFUSED, 0, 0, 0},
                                    {"put held, off, on again at once", "put", ACT_OFF, 1, 0, 0},
                                    {"start held, off, on again at once", "start", ACT_OFF, 1, 0, 0},
                                    {"nine left by off, on again", "", ACT_OFF, 1, 0, LEFT_SAME_RUN},
                                    {"nine left by off, the process ends, sharing off", "", ACT_OFF, 0, 0, LEFT_RESTART_OFF},
                                    {"nine left by off, the process ends, on again", "", ACT_OFF, 1, 0, LEFT_RESTART_ON},
                                    {"nine left by an accepted delete, the process ends, on again", "", ACT_DELETE, 1, 0, LEFT_DELETED_RESTART_ON},
                                    {"delete, the process ends while the server is asked", "", ACT_REFUSED, 1, 0, LEFT_ASKED_RESTART_ON},
                                    {"start held and cannot be aborted, off", "start", ACT_OFF, 0, 1, 0},
                                    {"start held and cannot be aborted, delete", "start", ACT_DELETE, 0, 1, 0}};

const char *tf__client_dir(const TfClient *c);
int tf__contributor(const char *dir, int create, char id[33], char contributor_key[90], char recovery_hash[65]);

/* stub sessions that received at least one segment */
static int off_uploaded(void) {
  int n = 0;
  for (int i = 0; i < g_st.n_s; ++i) n += g_st.s[i].max_n > 0;
  return n;
}
static int off_clean(TfClient *c) { return tf__share_bytes(c) == 0 && tf__sharing_outbox_pending(c) == 0 && tf__share_lists_waiting(c) == 0; }

/* a recording made now goes up in this run: 1 when it did, and it is the only one that gained a segment */
static int off_new_recording(TfClient *c, const float *x, long long clock) {
  const int before = off_uploaded(), puts0 = g_st.puts;
  tk_take(c, 48000, x, 96000, 960, clock, clock + 2000, 1);
  tk_check_at(c, clock + 2000 + 3001);
  int left = 1;
  for (int i = 0; i < 8 && left; ++i) left = tk_pass(c);
  return left == 0 && off_uploaded() == before + 1 && g_st.puts >= puts0 + 2;
}

extern int tf__share_no_delete;
/* a client on a folder as it is, without the learner's answer (takes_client says yes when sharing is off) */
static TfClient *off_client_as_is(const char *dir) {
  seed_config(dir, TK_ON);
  TfClientConfig o = tf__client_default_config();
  o.key = "qlk_v1_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "org.example.TakesTest";
  o.platform = "linux";
  o.api_url = "https://api.test";
  o.storage_dir = dir;
  o.telemetry = 0;
  o.debug_build = 1;
  o.on_log = tk_log;
  return tf__client_open(&o);
}
/* the restart situations: nine recordings a stop leaves behind */
static unsigned off_run_left(const char *dir, const OffCase *k, const float *x, int *ms, long long *audio) {
  unsigned bad = 0;
  long long clock = 0;
  stub_reset();
  tf__share_no_delete = 0;
  TfClient *c = takes_client(dir, TK_ON);
  if (!c) return OFF_SETUP;
  /* nine closed recordings; the uploader has not run in this client (no pass was asked for) */
  for (int i = 0; i < 9; ++i) {
    tk_take(c, 48000, x, 96000, 960, clock, clock + 2000, 1);
    tk_check_at(c, clock + 2000 + 3001);
    clock += 20000;
  }
  if (tf__sharing_outbox_pending(c) != 9) {
    tf__client_close(c);
    return OFF_SETUP;
  }
  char settings[1100];
  snprintf(settings, sizeof settings, "%s/sharing.json", dir);
  if (k->left == LEFT_ASKED_RESTART_ON) {
    g_st.delete_status = 500;    /* the server never takes it: the queue stays as it was while it was asked */
    g_st.snap_path = settings;   /* and the stub copies the settings file at the moment it is asked */
  }
  /* the call: from here on nothing can be deleted */
  tf__share_no_delete = 1;
  g_st.mark = 1;
  const double t0 = tf__now_s();
  const int rc = k->act == ACT_OFF ? tf__client_sharing_set(c, 0) : tf__client_sharing_delete(c);
  *ms = (int)((tf__now_s() - t0) * 1000);
  if (*ms > 3000 || (rc != 0) != (k->act == ACT_REFUSED)) bad |= OFF_BOUND;
  if (tf__sharing_outbox_pending(c) != 9 || tf__client_sharing_state(c) != (k->act == ACT_REFUSED)) bad |= OFF_SETUP;
  if (k->left != LEFT_SAME_RUN) {
    /* the process ends here; a new one opens the same folder */
    tf__client_close(c);
    if (k->left == LEFT_ASKED_RESTART_ON) {
      /* the settings file as it was while the server was asked */
      if (!g_st.snap || tf_write_file(settings, g_st.snap, g_st.snap_n)) bad |= OFF_SETUP;
      free(g_st.snap);
      g_st.snap = NULL;
      g_st.snap_path = NULL;
    }
    if (k->left == LEFT_RESTART_OFF) tf__share_no_delete = 0; /* deleting works again at this launch */
    c = off_client_as_is(dir);
    if (!c) return bad | OFF_SETUP;
    if (tf__client_sharing_state(c)) bad |= OFF_DISK; /* the new process must find sharing off */
    if (k->left == LEFT_RESTART_OFF) {
      /* sharing stays off: gone at launch, before any pass was asked for, and nothing is ever sent */
      if (!off_clean(c)) bad |= OFF_LATER;
      for (int i = 0; i < 2; ++i) tk_pass(c);
      if (g_st.after_requests || off_uploaded()) bad |= OFF_NEVER;
      *audio = g_st.after_audio;
      tf__client_close(c);
      return bad;
    }
  }
  /* the learner turns sharing on again while the nine still cannot be deleted */
  tf__client_sharing_set(c, 1);
  if (!tf__client_sharing_state(c) || tf__sharing_outbox_pending(c) != 9) bad |= OFF_SETUP;
  for (int i = 0; i < 3; ++i) tk_pass(c);
  if (g_st.after_requests || off_uploaded()) bad |= OFF_NEVER;
  /* deleting works again: the next pass's sweep deletes them, and still nothing of them is sent */
  tf__share_no_delete = 0;
  for (int i = 0; i < 2; ++i) tk_pass(c);
  if (!off_clean(c)) bad |= OFF_LATER;
  if (g_st.after_requests || off_uploaded()) bad |= OFF_NEVER;
  if (g_st.after_audio) bad |= OFF_AUDIO;
  *audio = g_st.after_audio;
  g_st.mark = 0;
  if (!off_new_recording(c, x, clock) || off_uploaded() != 1) bad |= OFF_ON_AGAIN;
  tf__client_close(c);
  return bad;
}

/* one run of one case: the failed assertions as bits (0: all hold); *ms is how long the call took, *audio the
 * bytes of audio that left after the call was made */
static unsigned off_run(const char *dir, const OffCase *k, const float *x, int *ms, long long *audio) {
  unsigned bad = 0;
  long long clock = 0;
  if (k->left) return off_run_left(dir, k, x, ms, audio);
  stub_reset();
  g_st.hold = k->held;
  g_st.hold_deaf = k->deaf;
  if (k->act == ACT_REFUSED) g_st.delete_status = 500;
  TfClient *c = takes_client(dir, TK_ON);
  if (!c) return OFF_SETUP;
  /* the recording whose upload reaches the request to hold */
  if (!strcmp(k->held, "takes")) {
    tk_two_takes(c, x, &clock); /* two takes in one recording: a take list follows its last segment */
  } else {
    const long long len = !strcmp(k->held, "urls") ? 25000 : 3000; /* 25 segments need a second batch of URLs */
    tk_take(c, 48000, x, len * 48, 960, clock, clock + len, 1);
    tk_check_at(c, clock + len + 3001);
    clock += len + 20000;
  }
  for (int i = 0; i < 4000 && !g_st.held; ++i) {
    tf__sharing_outbox_resume(c);
    SLEEP_MS(5);
  }
  char who[33] = "", who_now[33] = "";
  if (!g_st.held || tf__contributor(tf__client_dir(c), 0, who, NULL, NULL)) {
    g_st.release = 1;
    tf__client_close(c);
    return OFF_SETUP;
  }
  /* the learner acts */
  g_st.mark = 1;
  const double t0 = tf__now_s();
  const int rc = k->act == ACT_OFF ? tf__client_sharing_set(c, 0) : tf__client_sharing_delete(c);
  *ms = (int)((tf__now_s() - t0) * 1000);
  if (k->act == ACT_REFUSED) {
    /* the server did not take the deletion: the call says so, nothing changed, and the upload carries on */
    if (rc == 0 || *ms > 3000) bad |= OFF_BOUND;
    if (!g_st.ask_seen || g_st.ask_requests || !g_st.ask_held_done) bad |= OFF_ORDER;
    if (!tf__client_sharing_state(c) || tf__sharing_outbox_pending(c) != 1 || tf__share_bytes(c) <= 0 ||
        tf__contributor(tf__client_dir(c), 0, who_now, NULL, NULL) || strcmp(who, who_now))
      bad |= OFF_UNCHANGED;
    g_st.release = 1;
    int left = 1, segments = 0;
    for (int i = 0; i < 8 && left; ++i) left = tk_pass(c);
    if (left || tf__share_lists_waiting(c) || g_st.n_s != 1 || !joined_ok(&g_st.s[0], 1, &segments)) bad |= OFF_RESUMES;
    *audio = 0;
    tf__client_close(c);
    return bad;
  }
  if (rc != 0 || *ms > 3000) bad |= OFF_BOUND;
  if (!off_clean(c)) bad |= OFF_DEVICE;
  /* A request that cannot be aborted is still out, and it holds the client's lock: whatever reads the signed
   * config (the sharing state below) would wait until it ends. It is let go here, after the call has returned and
   * the device was looked at, so a run does not wait out the stub's hold. */
  if (k->deaf) g_st.release = 1;
  if (k->act == ACT_DELETE) {
    /* (a request that cannot be aborted is still out when the server is asked: only its sentence is held then) */
    if (!g_st.ask_seen || g_st.ask_requests || (!g_st.ask_held_done && !k->deaf)) bad |= OFF_ORDER;
    if (tf__client_sharing_state(c) || !status_has(c, "\"consent\":\"final\"") || !status_has(c, "\"pendingAsk\":false")) bad |= OFF_IS_OFF;
  } else if (tf__client_sharing_state(c)) {
    bad |= OFF_SETUP; /* the switch did not turn sharing off */
  }
  if (k->then_on) tf__client_sharing_set(c, 1); /* on again as soon as the call returned */
  /* the held request is let go and answers; whatever the uploader then does must leave nothing and send nothing */
  g_st.release = 1;
  for (int i = 0; i < 600 && !g_st.held_done; ++i) SLEEP_MS(5);
  for (int i = 0; i < 3; ++i) tk_pass(c);
  if (!off_clean(c)) bad |= k->then_on ? OFF_OLD_GONE : OFF_RELEASE;
  if (g_st.after_requests) bad |= OFF_REQUEST;
  if (g_st.after_audio) bad |= OFF_AUDIO;
  *audio = g_st.after_audio;
  g_st.mark = 0;
  if (k->act == ACT_DELETE) {
    /* the learner turns sharing on with the switch: a recording made now goes up, under a new contributor */
    tf__client_sharing_set(c, 1);
    const int up = tf__client_sharing_state(c) && off_new_recording(c, x, clock);
    if (!up || tf__contributor(tf__client_dir(c), 0, who_now, NULL, NULL) || !strcmp(who, who_now) || strcmp(g_st.start_contributor, who_now))
      bad |= OFF_ON_AGAIN;
  } else if (k->then_on) {
    if (!off_new_recording(c, x, clock)) bad |= OFF_ON_AGAIN;
    if (off_uploaded() != 1) bad |= OFF_OLD_GONE; /* the old recording had sent no segment and sends none now */
  }
  tf__client_close(c);
  return bad;
}

static int sharing_off_tests(const char *dir, int runs, int first) {
  tf_mutex_init(&g_st.m);
  tf_mkdirs(dir);
  tf_http_set_transport(stub, NULL);
  tf__share_plan[0] = tf__share_plan[1] = tf__share_plan[2] = tf__share_plan[3] = 1; /* 1 s segments */
  tf__share_now_ms = TK_T0;
  const int n = 48000 * 26;
  float *x = (float *)malloc(sizeof(float) * (size_t)n);
  lcg_noise(x, n, 0.2);
  int green = 0, own_green = 0;
  long long most_audio = 0;
  const int cases = (int)(sizeof kOffCases / sizeof kOffCases[0]);
  printf("sharing off during an upload: runs passed of %d per case\n", runs);
  for (int ci = first > 1 ? first - 1 : 0; ci < cases; ++ci) {
    if (ci == OFF_UPLOAD) printf(" the restart situations (nine recordings a stop leaves behind):\n");
    if (ci == OFF_SHARED) printf(" this SDK's own rows (a request the transport cannot abort):\n");
    int passed = 0, worst_ms = 0, per_key[OFF_KEYS] = {0};
    long long worst_audio = 0;
    for (int r = 0; r < runs; ++r) {
      char d[1100];
      snprintf(d, sizeof d, "%s/c%d_r%d", dir, ci, r);
      int ms = 0;
      long long audio = 0;
      const unsigned bad = off_run(d, &kOffCases[ci], x, &ms, &audio);
      passed += bad == 0;
      if (ms > worst_ms) worst_ms = ms;
      if (audio > worst_audio) worst_audio = audio;
      for (int b = 0; b < OFF_KEYS; ++b) per_key[b] += (bad >> b) & 1;
    }
    if (worst_audio > most_audio) most_audio = worst_audio;
    char why[600] = "";
    for (int b = 0; b < OFF_KEYS; ++b)
      if (per_key[b]) snprintf(why + strlen(why), sizeof why - strlen(why), " %s x%d", kOffKeys[b], per_key[b]);
    printf("  %-60s %2d of %d   (the call took at most %d ms; audio after it: at most %lld bytes)%s%s\n", kOffCases[ci].name, passed, runs,
           worst_ms, worst_audio, *why ? "   failed:" : "", why);
    fflush(stdout);
    green += passed == runs && ci < OFF_SHARED;
    own_green += passed == runs && ci >= OFF_SHARED;
    CHECK(passed == runs, "sharing off, %s: %d of %d runs passed;%s", kOffCases[ci].name, passed, runs, why);
  }
  free(x);
  printf("sharing off: %d of %d shared situations green (%d during an upload, %d restart) and %d of %d own rows (%d runs each), at most %lld bytes of "
         "audio after a call: %s\n",
         green, OFF_SHARED, OFF_UPLOAD, OFF_SHARED - OFF_UPLOAD, own_green, cases - OFF_SHARED, runs, most_audio, fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}
