/* Included by tf_tests.c: the platform client against the portal team's recorded fixtures (quranlab-local
 * p2-portalux-client fixtures/client-fixtures.json, signed with the LOCAL throwaway key), and the sharing state
 * machine. */
extern long long tf__now_override;
typedef struct {
  TfJson *fx;
  const char *config_name; /* which recorded config to answer */
  int tamper, wrong_kid;
  char last_path[256], last_inm[128], config_path[256];
  char *bodies[16]; /* request bodies seen, by route */
} Replay;
static Replay g_rp;
static const TfJson *exchange(const char *name) {
  const TfJson *ex = tf_json_get(g_rp.fx, "exchanges");
  for (int i = 0; i < ex->n; ++i)
    if (!strcmp(tf_json_str(tf_json_get(&ex->kids[i], "name")), name)) return &ex->kids[i];
  return NULL;
}
static int route_of(const char *path) {
  static const char *const r[] = {"/v1/attest", "/v1/register", "/v1/config", "/v1/events", "/v1/issues"};
  for (int i = 0; i < 5; ++i)
    if (!strncmp(path, r[i], strlen(r[i]))) return i;
  return -1;
}
static int replay(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  const char *path = strstr(q->url, "/v1/");
  snprintf(g_rp.last_path, sizeof g_rp.last_path, "%s", path ? path : "");
  g_rp.last_inm[0] = 0;
  for (const char *const *h = q->headers; h && *h; ++h)
    if (!_tf_strnicmp(*h, "if-none-match:", 14)) snprintf(g_rp.last_inm, sizeof g_rp.last_inm, "%s", *h + 15);
  const int rt = path ? route_of(path) : -1;
  if (rt == 2) snprintf(g_rp.config_path, sizeof g_rp.config_path, "%s", path);
  if (rt >= 0 && q->body) {
    free(g_rp.bodies[rt]);
    g_rp.bodies[rt] = (char *)malloc(q->body_n + 1);
    memcpy(g_rp.bodies[rt], q->body, q->body_n);
    g_rp.bodies[rt][q->body_n] = 0;
  }
  const char *name = rt == 0   ? "attest unattested (native linux)"
                     : rt == 1 ? "register (linux)"
                     : rt == 3 ? "events v2 (linux)"
                     : rt == 4 ? "issue (linux)"
                               : (g_rp.last_inm[0] ? "config 304 (unchanged)" : g_rp.config_name);
  const TfJson *e = name ? exchange(name) : NULL;
  if (!e) return tf__fail(TF_ERR_NETWORK, "no recorded exchange");
  const TfJson *resp = tf_json_get(e, "response"), *hd = tf_json_get(resp, "headers");
  r->status = (int)tf_json_get(resp, "status")->num;
  const char *b = tf_json_str(tf_json_get(resp, "body"));
  r->body_n = b ? strlen(b) : 0;
  r->body = (char *)malloc(r->body_n + 1);
  memcpy(r->body, b ? b : "", r->body_n + 1);
  if (g_rp.tamper && rt == 2 && r->body_n > 10) r->body[5] ^= 1;
  const char *v;
  if ((v = tf_json_str(tf_json_get(hd, "x-qlab-signature")))) snprintf(r->signature, sizeof r->signature, "%s", v);
  if ((v = tf_json_str(tf_json_get(hd, "x-qlab-key-id"))))
    snprintf(r->key_id, sizeof r->key_id, "%s", g_rp.wrong_kid ? "0000000000000000" : v);
  if ((v = tf_json_str(tf_json_get(hd, "etag")))) snprintf(r->etag, sizeof r->etag, "%s", v);
  return 0;
}
/* every key of `got` must be a key of `want`; `exact`: and every key of `want` in `got` */
static void keys_within(const TfJson *got, const TfJson *want, const char *at, int exact) {
  CHECK(got && got->type == TJ_OBJ, "%s: no body", at);
  if (!got || got->type != TJ_OBJ) return;
  for (int i = 0; i < got->n; ++i) CHECK(tf_json_get(want, got->kids[i].key) != NULL, "%s.%s is not in the recorded schema", at, got->kids[i].key);
  if (exact)
    for (int i = 0; want && i < want->n; ++i) CHECK(tf_json_get(got, want->kids[i].key) != NULL, "%s.%s missing", at, want->kids[i].key);
}
static TfJson *parse_or_null(const char *t) { return t ? tf_json_parse(t, strlen(t), NULL) : NULL; }

/* A verified config as the client stores it (config.body, and config_exp in client.json; the stored body was
 * verified when it arrived). Since 2.0.1 sharing is offered only where the signed config says so, and these tests
 * have no config server. flags: the "flags" object; NULL removes any stored config. */
static void seed_config(const char *dir, const char *flags) {
  tf_mkdirs(dir);
  char *pb = tf_path_join(dir, "config.body"), *pc = tf_path_join(dir, "client.json");
  TfBuf b;
  tf_buf_init(&b);
  if (flags) {
    tf_buf_printf(&b, "{\"v\":1,\"app\":\"linux:org.example.test\",\"flags\":%s}", flags);
    tf_write_file(pb, b.p, b.n);
  } else {
    remove(pb);
  }
  tf_buf_free(&b);
  tf_buf_init(&b);
  tf_buf_printf(&b, "{\"bucket\":7,\"pending_new_install\":false,\"token\":\"\",\"token_exp\":0,\"etag\":\"\",\"config_exp\":%lld}",
                flags ? (long long)time(NULL) + 86400 : 0LL);
  tf_write_file(pc, b.p, b.n);
  tf_buf_free(&b);
  free(pb);
  free(pc);
}

static int client_tests(const char *fx_path, const char *dir, const char *model_dir) {
  char *text = NULL;
  size_t n;
  if (tf_read_file(fx_path, &text, &n)) return 2;
  g_rp.fx = tf_json_parse(text, n, NULL);
  free(text);
  if (!g_rp.fx) return 2;
  g_rp.config_name = "config sdk 2.0.0 (apple, V4)";
  tf__now_override = 1794057929 + 60; /* the recorded config's issued_at, plus a minute */
  tf_http_set_transport(replay, NULL);
  const char *key = tf_json_str(tf_json_get(g_rp.fx, "local_public_key_hex"));
  TfClientOptions o = tf_client_default_options();
  o.key = "qlk_v1_local_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "org.example.Clientmw6frljs";
  o.platform = "linux";
  o.app_name = "Client e2e";
  o.app_version = "1.0";
  o.api_url = "http://localhost:8787";
  o.storage_dir = dir;
  o.trusted_key_hex = key;
  TfClient *c = tf_client_create(&o);
  CHECK(c != NULL, "client: %s", tf_last_error());
  if (!c) return 1;
  CHECK(tf_client_launch(c) == 0, "launch: %s", tf_last_error());
  CHECK(tf_client_config_json(c) != NULL, "the recorded config was not accepted (signature, key id, expiry)");
  TfJson *att = parse_or_null(g_rp.bodies[0]), *reg = parse_or_null(g_rp.bodies[1]);
  keys_within(att, tf_json_get(tf_json_get(exchange("attest unattested (native linux)"), "request"), "body"), "attest", 1);
  keys_within(reg, tf_json_get(tf_json_get(exchange("register (linux)"), "request"), "body"), "register", 0);
  CHECK(reg && tf_json_get(reg, "new_install") && tf_json_get(reg, "new_install")->boolean, "register: new_install on the first launch");
  tf_json_free(att);
  tf_json_free(reg);
  CHECK(strstr(g_rp.config_path, "sdk=2.0.1") && strstr(g_rp.config_path, "bucket="), "config query %s", g_rp.config_path);
  CHECK(tf_client_refresh_config(c) == 0 && g_rp.last_inm[0] == '"', "304 refresh (If-None-Match %s)", g_rp.last_inm);
  CHECK(tf_client_config_json(c) != NULL, "config lost after 304");
  /* within 1 h of expiry: an unconditional fetch; a tampered body and an unknown key id are refused */
  tf__now_override = 1794144329 - 1800;
  g_rp.config_name = "config 200 after a portal change";
  g_rp.tamper = 1;
  {
    const int rc = tf_client_refresh_config(c);
    CHECK(rc == TF_ERR_CONFIG, "a tampered config was accepted (%d %s, If-None-Match %s)", rc, tf_last_error() ? tf_last_error() : "", g_rp.last_inm);
  }
  g_rp.tamper = 0;
  g_rp.wrong_kid = 1;
  CHECK(tf_client_refresh_config(c) == TF_ERR_CONFIG, "an unknown key id was accepted");
  g_rp.wrong_kid = 0;
  CHECK(tf_client_config_json(c) != NULL, "the last good config was dropped");
  CHECK(tf_client_refresh_config(c) == 0, "the changed config: %s", tf_last_error());
  if (model_dir) {
    /* the verified config's tajweed block reaches a model's portal layer */
    TfModelOptions mo = tf_model_default_options();
    mo.require_signature = 0;
    mo.load_engine = 0;
    TfModel *m = tf_model_open(model_dir, &mo);
    CHECK(m && tf_client_apply_config(c, m) == 0, "apply config: %s", tf_last_error());
    if (m) {
      CHECK(strstr(tf_settings_json(m), "\"source\":\"remote\"") != NULL || 0,
            "the tajweed block did not reach the settings");
      tf_model_close(m);
    }
  }
  TfSessionStats st;
  memset(&st, 0, sizeof st);
  st.struct_size = sizeof st;
  st.audio_s = 6.3;
  st.compute_s = 1.2;
  st.chunks = 20;
  tf_client_session_finished(c, &st);
  CHECK(tf_client_report_issue(c, "model_load", "client_e2e", "free text is never sent") == 0, "issue");
  CHECK(tf_client_report_issue(c, "model_load", "client_e2e", NULL) == 0, "issue dedupe");
  CHECK(tf_client_report_issue(c, "made_up", "x", NULL) != 0, "an unknown issue kind was accepted");
  const int sent = tf_client_flush(c, 1);
  CHECK(sent == 1, "events: %d deltas sent", sent);
  TfJson *ev = parse_or_null(g_rp.bodies[3]), *is = parse_or_null(g_rp.bodies[4]);
  const TfJson *rec = tf_json_get(tf_json_get(exchange("events v2 (linux)"), "request"), "body");
  /* the recorded 2.0.0 body's fields, and the 2.0.1 ones (each optional on the server; report_id always sent) */
  {
    static const char *const added[] = {"report_id", "launches", "ayahs", "surah_ayahs", "words", "rule_checks", "rule_flags",
                                        "share_asked", "share_yes", "share_no"};
    TfBuf wb;
    tf_buf_init(&wb);
    tf_json_write(&wb, rec);
    if (wb.n > 1) wb.n--; /* drop the closing brace */
    for (size_t i = 0; i < sizeof added / sizeof added[0]; ++i) tf_buf_printf(&wb, ",\"%s\":0", added[i]);
    tf_buf_puts(&wb, "}");
    TfJson *rec201 = wb.p ? tf_json_parse(wb.p, wb.n, NULL) : NULL;
    keys_within(ev, rec201, "events", 0);
    keys_within(rec, ev, "events (recorded fields present)", 0);
    tf_json_free(rec201);
    tf_buf_free(&wb);
    const char *rid = tf_json_str(tf_json_get(ev, "report_id"));
    CHECK(rid && strlen(rid) == 32 && strspn(rid, "0123456789abcdef") == 32, "report_id %s", rid ? rid : "missing");
  }
  keys_within(tf_json_get(ev, "device"), tf_json_get(rec, "device"), "events.device", 1);
  const TfJson *co = tf_json_get(ev, "cohorts");
  CHECK(co && co->n == 1, "events: one cohort");
  if (co && co->n) keys_within(&co->kids[0], &tf_json_get(rec, "cohorts")->kids[0], "events.cohort", 1);
  CHECK(tf_json_str(tf_json_get(ev, "product")) && !strcmp(tf_json_get(ev, "product")->str, "native"), "product native");
  keys_within(is, tf_json_get(tf_json_get(exchange("issue (linux)"), "request"), "body"), "issue", 0);
  CHECK(is && !tf_json_get(is, "message"), "an issue carried free text");
  tf_json_free(ev);
  tf_json_free(is);
  tf_client_destroy(c);
  printf("client replay (the portal team fixtures, local key id %s): %s\n", tf_json_str(tf_json_get(g_rp.fx, "local_key_id")), fails ? "FAIL" : "ok");

  /* the sharing state machine: never before a session with feedback; the first ask and two re-asks, 7 sessions apart,
   * then never again */
  tf_http_set_transport(NULL, NULL);
  tf__now_override = 0;
  char sdir[1024];
  snprintf(sdir, sizeof sdir, "%s/sharing", dir);
  TfClientOptions so = tf_client_default_options();
  so.app_id = "org.example.Sharing";
  /* 2.0.1: the backend decides whether the sheet is offered; the app's option is ignored either way */
  {
    char d0[1100];
    int asked = 0;
    struct { const char *flags; int option, want; const char *what; } cases[] = {
        {NULL, 1, 0, "no config in force: nothing offered, even with the option"},
        {"{\"contributions\":false}", 1, 0, "the config says no: the option cannot force it"},
        {"{\"contributions\":\"true\"}", 1, 0, "only a literal true"},
        {"{\"contributions\":true}", 0, 1, "the config says yes: offered without the option"},
    };
    for (int k = 0; k < 4; ++k) {
      snprintf(d0, sizeof d0, "%s/offer%d", dir, k);
      seed_config(d0, cases[k].flags);
      so.storage_dir = d0;
      so.contributions = cases[k].option;
      TfClient *oc = tf_client_create(&so);
      TfSessionStats ss;
      memset(&ss, 0, sizeof ss);
      ss.struct_size = sizeof ss;
      ss.feedback_shown = 1;
      tf_client_session_finished(oc, &ss);
      asked = oc && strstr(tf_client_sharing_status_json(oc), "\"pendingAsk\":true") != NULL;
      CHECK(asked == cases[k].want, "%s: %s", cases[k].what, oc ? tf_client_sharing_status_json(oc) : "no client");
      /* offered is never consent: not sharing until the learner answers */
      CHECK(oc && tf_client_sharing_state(oc) == 0, "%s: sharing before any answer", cases[k].what);
      tf_client_destroy(oc);
    }
  }
  seed_config(sdir, "{\"contributions\":true}");
  so.storage_dir = sdir;
  so.contributions = 1;
  TfClient *sc = tf_client_create(&so);
  int asks_at[8], na = 0;
  for (int i = 1; i <= 40 && sc; ++i) {
    TfSessionStats ss;
    memset(&ss, 0, sizeof ss);
    ss.struct_size = sizeof ss;
    ss.feedback_shown = i != 1;
    tf_client_session_finished(sc, &ss);
    if (strstr(tf_client_sharing_status_json(sc), "\"pendingAsk\":true")) {
      if (na < 8) asks_at[na] = i;
      na++;
      tf_client_sharing_answer(sc, 0);
    }
  }
  CHECK(na == 3 && asks_at[0] == 2 && asks_at[1] == 9 && asks_at[2] == 16, "asks: %d, at %d %d %d", na, na > 0 ? asks_at[0] : -1,
        na > 1 ? asks_at[1] : -1, na > 2 ? asks_at[2] : -1);
  CHECK(sc && strstr(tf_client_sharing_status_json(sc), "\"consent\":\"final\""), "final after the third Not now");
  /* desktop uploads at the capped tier when the config names no tier for it */
  CHECK(sc && strstr(tf_client_sharing_status_json(sc), "\"uploadTrust\":\"capped\"") && strstr(tf_client_sharing_status_json(sc), "\"mayUpload\":true"),
        "a desktop install uploads at the capped tier by default: %s", sc ? tf_client_sharing_status_json(sc) : "");
  /* three sheets per install, ever: none after a restart either, and ask_now asks nothing */
  tf_client_destroy(sc);
  sc = tf_client_create(&so);
  for (int i = 0; i < 30 && sc; ++i) {
    TfSessionStats ss;
    memset(&ss, 0, sizeof ss);
    ss.struct_size = sizeof ss;
    ss.feedback_shown = 1;
    tf_client_session_finished(sc, &ss);
  }
  CHECK(sc && tf_client_sharing_ask_now(sc) == 0 && strstr(tf_client_sharing_status_json(sc), "\"asks\":3,") &&
            strstr(tf_client_sharing_status_json(sc), "\"pendingAsk\":false"),
        "a fourth sheet after a restart: %s", sc ? tf_client_sharing_status_json(sc) : "");
  tf_client_sharing_set(sc, 1);
  CHECK(tf_client_sharing_state(sc) == 1, "the settings switch turns sharing on");
  tf_client_destroy(sc);
  const char *en = tf_sharing_copy_json("en-GB"), *ar = tf_sharing_copy_json("ar");
  CHECK(en && strstr(en, "\"lang\": \"en\"") && ar && strstr(ar, "\"lang\": \"ar\"") && tf_sharing_copy_json("xx"), "copy decks");
  printf("sharing state machine and copy decks: %s\n", fails ? "FAIL" : "ok");
  tf_json_free(g_rp.fx);
  return fails ? 1 : 0;
}

/* the Ogg muxer against the cross-SDK vector: deterministic packets in, the four
 * segment files byte for byte out */
#include "tf_ogg.h"
static int ogg_tests(const char *vdir, const char *dir) {
  char *p = tf_path_join(vdir, "expected.json"), *text = NULL;
  size_t n;
  if (!p || tf_read_file(p, &text, &n)) return 2;
  free(p);
  TfJson *e = tf_json_parse(text, n, NULL);
  free(text);
  const TfJson *in = tf_json_get(e, "input");
  const int packets = (int)tf_json_get(in, "packets")->num;
  tf_mkdirs(dir);
  TfOgg o;
  CHECK(tf_ogg_open(&o, dir, (uint32_t)tf_json_get(in, "serial")->num, (int)tf_json_get(in, "pre_skip")->num,
                    (int)tf_json_get(in, "input_rate")->num, tf_json_str(tf_json_get(in, "vendor"))) == 0, "ogg open");
  uint8_t buf[300];
  for (int i = 0; i < packets; ++i) {
    const int len = 3 + (i * 37 % 290);
    for (int j = 0; j < len; ++j) buf[j] = (uint8_t)((i * 31 + j * 7 + 1) & 255);
    if (tf_ogg_packet(&o, buf, len)) {
      CHECK(0, "ogg packet %d", i);
      break;
    }
  }
  CHECK(tf_ogg_finish(&o, (long long)tf_json_get(in, "final_granule")->num) == 0, "ogg finish");
  const TfJson *segs = tf_json_get(e, "segments");
  int same = 0;
  for (int i = 0; i < segs->n; ++i) {
    const char *f = tf_json_str(tf_json_get(&segs->kids[i], "file"));
    char *a = tf_path_join(dir, f), *b = tf_path_join(vdir, f), *x = NULL, *y = NULL;
    size_t nx = 0, ny = 0;
    const int ok = !tf_read_file(a, &x, &nx) && !tf_read_file(b, &y, &ny) && nx == ny && !memcmp(x, y, nx);
    CHECK(ok, "%s differs (%zu bytes, want %zu)", f, nx, ny);
    same += ok;
    free(a);
    free(b);
    free(x);
    free(y);
  }
  {
    char name[64];
    snprintf(name, sizeof name, "seg-%05d.opus", segs->n + 1);
    char *extra = tf_path_join(dir, name);
    CHECK(!tf_file_exists(extra), "an extra segment file %s", name);
    free(extra);
  }
  printf("ogg muxer vs the cross-SDK vector: %d/%d segment files byte-identical\n", same, segs->n);
  tf_json_free(e);
  return fails ? 1 : 0;
}

/* the deletion crypto against the cross-SDK vectors (signing.json, recovery.json), and the deletion flow against
 * the recorded exchanges */
void tf_recovery_code(const uint8_t b[16], char out[33]);
int tf_recovery_normalise(const char *typed, char out[27]);
void tf_recovery_hash(const char *normalised, char out[65]);
char *tf_deletion_message(const char *app, const char *contributor, const char *nonce, long long ts);
int tf_p256_sign(const uint8_t priv[96], const void *msg, size_t n, uint8_t sig[64]);
int tf_p256_verify(const uint8_t pub[65], const void *msg, size_t n, const uint8_t sig[64]);
int tf__contributor(const char *dir, int create, char id[33], char contributor_key[90], char recovery_hash[65]);
static TfJson *load_json(const char *dir, const char *name) {
  char *p = tf_path_join(dir, name), *t = NULL;
  size_t n;
  TfJson *j = p && !tf_read_file(p, &t, &n) ? tf_json_parse(t, n, NULL) : NULL;
  free(p);
  free(t);
  return j;
}
static int replay_sharing(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  const char *name = strstr(q->url, "/contributions/challenge") ? "deletion challenge"
                     : strstr(q->url, "/contributions/delete")  ? "deletion signed by the device key"
                     : strstr(q->url, "/contributions/summary") ? "contributions summary"
                     : strstr(q->url, "/v1/attest")             ? "attest unattested (native linux)"
                                                                : NULL;
  for (const char *const *h = q->headers; h && *h; ++h)
    if (strstr(q->url, "/contributions/c") || strstr(q->url, "/contributions/d"))
      CHECK(_tf_strnicmp(*h, "x-qlab-key", 10) && _tf_strnicmp(*h, "authorization", 13), "a credential was sent to a deletion route");
  const TfJson *e = name ? exchange(name) : NULL;
  if (!e) return tf__fail(TF_ERR_NETWORK, "no recorded exchange for %s", q->url);
  if (strstr(q->url, "/contributions/delete")) {
    /* the body is the recorded one's fields, and its signature verifies with this install's device key */
    TfJson *b = tf_json_parse((const char *)q->body, q->body_n, NULL);
    keys_within(b, tf_json_get(tf_json_get(e, "request"), "body"), "delete", 1);
    tf_json_free(b);
  }
  const TfJson *resp = tf_json_get(e, "response");
  r->status = (int)tf_json_get(resp, "status")->num;
  const char *body = tf_json_str(tf_json_get(resp, "body"));
  r->body_n = body ? strlen(body) : 0;
  r->body = (char *)malloc(r->body_n + 1);
  memcpy(r->body, body ? body : "", r->body_n + 1);
  return 0;
}
static int sharing_tests(const char *vdir, const char *fx_path, const char *dir) {
  /* recovery codes */
  TfJson *rec = load_json(vdir, "recovery.json"), *sig = load_json(vdir, "signing.json");
  if (!rec || !sig) return 2;
  const TfJson *cases = tf_json_get(rec, "cases");
  int ok = 0;
  for (int i = 0; i < cases->n; ++i) {
    uint8_t b[16];
    char code[33], norm[27], h[65];
    tf_hex_decode(tf_json_str(tf_json_get(&cases->kids[i], "bytes_hex")), b, 16);
    tf_recovery_code(b, code);
    const int good = !strcmp(code, tf_json_str(tf_json_get(&cases->kids[i], "code"))) && !tf_recovery_normalise(code, norm) &&
                     !strcmp(norm, tf_json_str(tf_json_get(&cases->kids[i], "normalised"))) &&
                     (tf_recovery_hash(norm, h), !strcmp(h, tf_json_str(tf_json_get(&cases->kids[i], "recovery_hash"))));
    CHECK(good, "recovery case %d: %s", i, code);
    ok += good;
  }
  const TfJson *typed = tf_json_get(rec, "typed");
  for (int i = 0; i < typed->n; ++i) {
    char norm[27];
    const char *want = tf_json_str(tf_json_get(&typed->kids[i], "normalised"));
    const int rc = tf_recovery_normalise(tf_json_str(tf_json_get(&typed->kids[i], "typed")), norm);
    const int good = want ? (!rc && !strcmp(norm, want)) : rc != 0;
    CHECK(good, "typed recovery %d", i);
    ok += good;
  }
  /* the deletion message, the vector signatures (P1363) with the vector key, our own signature with the vector key */
  uint8_t pub[70], priv[96], d[40];
  CHECK(tf_base64_decode(tf_json_str(tf_json_get(sig, "public_key_uncompressed_b64url")), pub, sizeof pub) == 65, "vector public key");
  const TfJson *jwk = tf_json_get(sig, "private_key_jwk");
  memcpy(priv, pub + 1, 64);
  CHECK(tf_base64_decode(tf_json_str(tf_json_get(jwk, "d")), d, sizeof d) == 32, "vector private key");
  memcpy(priv + 64, d, 32);
  const TfJson *sc = tf_json_get(sig, "cases");
  for (int i = 0; i < sc->n; ++i) {
    const TfJson *f = tf_json_get(&sc->kids[i], "fields");
    char *m = tf_deletion_message(tf_json_str(tf_json_get(f, "app")), tf_json_str(tf_json_get(f, "contributor")), tf_json_str(tf_json_get(f, "nonce")),
                                  (long long)tf_json_get(f, "timestamp")->num);
    const int msg_ok = m && !strcmp(m, tf_json_str(tf_json_get(&sc->kids[i], "message")));
    uint8_t s[70], mine[64];
    const int vs = tf_base64_decode(tf_json_str(tf_json_get(&sc->kids[i], "signature_p1363_b64url")), s, sizeof s) == 64 && tf_p256_verify(pub, m, strlen(m), s);
    const int own = !tf_p256_sign(priv, m, strlen(m), mine) && tf_p256_verify(pub, m, strlen(m), mine);
    mine[10] ^= 1;
    const int forged = tf_p256_verify(pub, m, strlen(m), mine);
    CHECK(msg_ok && vs && own && !forged, "signing case %d: message %d vector %d own %d forged %d", i, msg_ok, vs, own, forged);
    ok += msg_ok && vs && own && !forged;
    free(m);
  }
  printf("deletion crypto vs the cross-SDK vectors: %d/%d (recovery codes, typed codes, signing cases)\n", ok, cases->n + typed->n + sc->n);
  tf_json_free(rec);
  tf_json_free(sig);
  /* the deletion flow: a contributor made, challenge, signed delete without credentials, local state forgotten */
  char *text = NULL;
  size_t n;
  if (tf_read_file(fx_path, &text, &n)) return 2;
  g_rp.fx = tf_json_parse(text, n, NULL);
  free(text);
  tf_http_set_transport(replay_sharing, NULL);
  TfClientOptions o = tf_client_default_options();
  o.key = "qlk_v1_local_test_aaaaaaaaaaaaaaaaaaaaaa";
  o.app_id = "org.example.Sharing";
  o.platform = "linux";
  o.storage_dir = dir;
  o.api_url = "http://localhost:8787";
  TfClient *c = tf_client_create(&o);
  char id[33], key[90], rh[65], code[33];
  CHECK(tf__contributor(dir, 1, id, key, rh) == 0 && strlen(id) == 32 && strlen(key) == 87 && strlen(rh) == 64, "contributor made");
  CHECK(tf_client_sharing_recovery_code(c, code) == 0 && strlen(code) == 32, "recovery code shown");
  CHECK(tf_client_sharing_summary_json(c) != NULL, "summary");
  CHECK(tf_client_sharing_delete(c) == 0, "delete: %s", tf_last_error() ? tf_last_error() : "");
  CHECK(tf_client_sharing_recovery_code(c, code) != 0, "the local contributor survived the deletion");
  tf_client_destroy(c);
  tf_http_set_transport(NULL, NULL);
  tf_json_free(g_rp.fx);
  printf("deletion flow (recorded exchanges): %s\n", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}

/* the downloader against a local "CDN" folder (a layout-3 folder published with gzip files): the native profile,
 * Range resume after an interrupted transfer, gzip decoding, every hash; then the downloaded folder opens */
static char g_cdn[1024];
static int g_cut_once, g_requests, g_ranged, g_bad_url;
static int serve_cdn(const TfHttpRequest *q, TfHttpResponse *r, void *u) {
  (void)u;
  g_requests++;
  /* the default base: cdn.quranlab.ai/models, no /hf/ */
  static const char want[] = "https://cdn.quranlab.ai/models/Quran-Lab/tajweedformer-v4/test/sdk/v4.0/";
  if (strncmp(q->url, want, sizeof want - 1)) g_bad_url++;
  const char *rel = strstr(q->url, "/sdk/v4.0/");
  if (!rel) return tf__fail(TF_ERR_NETWORK, "unexpected URL");
  rel += 10;
  char *p = tf_path_join(g_cdn, rel), *data = NULL;
  size_t n = 0;
  const int have = p && !tf_read_file(p, &data, &n);
  free(p);
  if (!have) {
    r->status = 404;
    return 0;
  }
  if (!q->download_file) {
    r->status = 200;
    r->body = data;
    r->body_n = n;
    return 0;
  }
  const long long from = q->range_from > 0 ? q->range_from : 0;
  g_ranged += from > 0;
  FILE *f = fopen(q->download_file, from > 0 ? "ab" : "wb");
  size_t len = n - (size_t)from;
  int cut = 0;
  if (g_cut_once && strstr(rel, "layers.zfw.gz") && from == 0) {
    len /= 2; /* the connection drops half way */
    cut = 1;
    g_cut_once = 0;
  }
  fwrite(data + from, 1, len, f);
  fclose(f);
  free(data);
  if (cut) return tf__fail(TF_ERR_NETWORK, "connection reset (simulated)");
  r->status = from > 0 ? 206 : 200;
  return 0;
}
static int download_tests(const char *cdn, const char *dest, const char *key) {
  snprintf(g_cdn, sizeof g_cdn, "%s", cdn);
  tf_http_set_transport(serve_cdn, NULL);
  g_cut_once = 1;
  TfDownloadOptions d = tf_download_default_options();
  d.key = "qlk_v1_local_test_aaaaaaaaaaaaaaaaaaaaaa";
  d.revision = "test";
  d.dest = dest;
  d.contexts = "c16_l64";
  d.trusted_key_hex = key;
  const int first = tf_download_model(&d);
  CHECK(first == TF_ERR_NETWORK, "the interrupted download: %d", first);
  const int second = tf_download_model(&d);
  CHECK(second == 0, "the resumed download: %s", tf_last_error() ? tf_last_error() : "");
  CHECK(g_ranged >= 1, "no Range request after the interruption");
  CHECK(g_bad_url == 0, "a download URL is not under https://cdn.quranlab.ai/models/Quran-Lab/tajweedformer-v4/test/sdk/v4.0/");
  {
    /* the base: the /models shape; a bare cdn.quranlab.ai (or regional) base keeps its /hf/ path */
    static const char *const cases[][2] = {
      {"https://cdn.quranlab.ai/models/", "https://cdn.quranlab.ai/models/Quran-Lab/m/r/sdk/v4.0/a"},
      {"https://cdn.quranlab.ai", "https://cdn.quranlab.ai/hf/Quran-Lab/m/r/sdk/v4.0/a"},
      {"https://oc.cdn.quranlab.ai", "https://oc.cdn.quranlab.ai/hf/Quran-Lab/m/r/sdk/v4.0/a"},
      {"https://mirror.test/x", "https://mirror.test/x/Quran-Lab/m/r/sdk/v4.0/a"},
    };
    char u[256];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
      CHECK(!tf__model_url(u, sizeof u, cases[i][0], "Quran-Lab/m", "r", "sdk/v4.0", "a") && !strcmp(u, cases[i][1]), "model URL for %s: %s", cases[i][0], u);
    }
  }
  char *c24 = tf_path_join(dest, "c24_l72/frames.zfw"), *gzleft = tf_path_join(dest, "layers.zfw.gz");
  CHECK(!tf_file_exists(c24), "a context that was not asked for was downloaded");
  CHECK(!tf_file_exists(gzleft), "the stored gz file was kept");
  free(c24);
  free(gzleft);
  tf_http_set_transport(NULL, NULL);
  TfModelOptions mo = tf_model_default_options();
  mo.trusted_key_hex = key;
  mo.context = "c16_l64";
  TfModel *m = tf_model_open(dest, &mo);
  CHECK(m != NULL, "the downloaded folder does not open: %s", tf_last_error() ? tf_last_error() : "");
  if (m) {
    TfStream *s = tf_stream_create(m, NULL);
    CHECK(s != NULL, "no stream on the downloaded folder");
    tf_stream_destroy(s);
    tf_model_close(m);
  }
  printf("downloader (layout 3, gzip, Range resume, native profile c16_l64): %s, %d requests\n", fails ? "FAIL" : "ok", g_requests);
  return fails ? 1 : 0;
}
