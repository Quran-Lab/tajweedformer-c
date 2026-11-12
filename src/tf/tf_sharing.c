/* Recitation sharing: the consent state machine (the same rules as the web SDK), the embedded copy decks, the
 * app's own wording, the settings calls, and whether this install may upload at all (the platform client doc:
 * flags.contributions and flags.upload_trust[windows|linux]). State: sharing.json in the client's storage
 * directory.
 *
 * Asking (Quran Lab, 11-10): the sheet is shown at most three times per install, ever (asks, persisted, carried
 * over from 2.0.0's sharing.json). The first ask follows a finished session with feedback; "Not now" (or a sheet
 * left unanswered) may lead to a re-ask at least 7 sessions later, until the third showing. A yes ends asking for
 * good, across copy decks, wording and SDK upgrades; so does turning sharing off. After that only the settings
 * switch (tf__client_sharing_set) turns sharing on or off.
 *
 * Consent versions (SDK 2.0.1): the grant records what the learner agreed to at that moment: the copy deck's
 * consent_version, or with the app's own wording (tf_sharing_set_wording) "custom-" + the first 24 hex digits of
 * the SHA-256 of the canonical text (the shared vectors' consent_text.json) and that hash. Every shared session
 * carries the recorded version (and hash), never the one in force later. A grant persists: a new deck or new
 * wording only changes what a new grant records and what the sheet shows.
 * Tried and rejected: comparing the recorded version with the one in force and asking again with an update
 * variant (the recording-fix builds); Quran Lab's rule is that a learner who said yes is never asked again.
 *
 * Upgrades: 2.0.0 wrote the same sharing.json ({"v":1,"consent","asks","lastAskSession","sessions","thanked",
 * "decidedAt","consentVersion","guardian","sharedSessions","pendingAsk"}) and recorded consentVersion with every
 * grant. A granted state without one gets the version 2.0.0 bundled (kVersion200). A state that said yes and was
 * then put back to "unknown" by a deck or wording change (2.0.0 did that at start-up, the recording-fix builds
 * with their update flag) is a grant again. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "tf_internal.h"
#include "tf_thread.h"

typedef struct {
  const char *lang;
  const char *json;
} TfCopyDeck;
extern const TfCopyDeck tf__copy_decks[];

const char *tf__client_dir(const TfClient *c);
const TfClientConfig *tf__client_options(const TfClient *c);
void **tf__client_sharing_slot(TfClient *c);
int tf__client_flag(const TfClient *c, const char *name, int dflt);
void tf__client_upload_trust(const TfClient *c, char *out, size_t n);
void tf__telemetry_add(TfClient *c, const char *field, long long n);

int tf_random_bytes(uint8_t *out, size_t n) {
#ifdef _WIN32
  return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
#else
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) return -1;
  size_t got = 0;
  while (got < n) {
    const ssize_t k = read(fd, out + got, n - got);
    if (k <= 0) {
      close(fd);
      return -1;
    }
    got += (size_t)k;
  }
  close(fd);
  return 0;
#endif
}

/* ------------------------------------------------------------------ copy */
const char *tf__sharing_deck_json(const char *language) {
  const char *want = language && *language ? language : "en";
  for (int i = 0; tf__copy_decks[i].lang; ++i)
    if (!_tf_strnicmp(tf__copy_decks[i].lang, want, 2) && (want[2] == 0 || want[2] == '-' || want[2] == '_'))
      return tf__copy_decks[i].json;
  for (int i = 0; tf__copy_decks[i].lang; ++i)
    if (!strcmp(tf__copy_decks[i].lang, "en")) return tf__copy_decks[i].json;
  return NULL;
}
static void deck_consent_version(const char *lang, char *out, size_t n) {
  const char *d = tf__sharing_deck_json(lang);
  TfJson *j = d ? tf_json_parse(d, strlen(d), NULL) : NULL;
  const char *v = tf_json_str(tf_json_get(j, "consent_version"));
  snprintf(out, n, "%s", v ? v : "");
  tf_json_free(j);
}

/* ------------------------------------------------------------------ state */
#define MAX_ASKS 3           /* sheets shown per install, ever */
#define REASK_SESSIONS 7     /* sessions between a "Not now" and the next ask */
/* the consent_version of the decks 2.0.0 bundled (every language: 2026-11-07.1) */
static const char kVersion200[] = "share-2026-11-07";

typedef struct {
  char consent[12]; /* unknown, granted, declined, final */
  int asks, last_ask_session, sessions, thanked, guardian, shared_sessions, pending_ask;
  long long decided_at;
  char consent_version[40];   /* the version the learner agreed to */
  char consent_hash[65];      /* with the app's own wording: the SHA-256 of the text agreed to */
  /* 3.0.1: how often the learner changed their mind (sharing turned off, a deletion the server took, sharing
   * turned on). It is written with the consent, in the same write, and every recording carries the count it was
   * begun under (the file `stamp` in its folder). A recording with another count than the one in force was begun
   * before a stop: it is never uploaded and is deleted at the next upload pass or launch, also when the process
   * died between the stop and its deletion, or the stop could not delete it. Under `lock`. */
  int stamp;
  /* A deletion is asking the server (under `lock`). While it is set the settings file is written as it will be
   * when the server has taken the deletion: consent off and the count one higher. So the off is on disk before
   * the server is asked, and a process that dies while it is asked, or between the server's answer and the
   * device's deletes, starts with sharing off and deletes what is left. In memory nothing changes meanwhile:
   * a deletion the server does not take changes nothing, and the file is then written back as it was. */
  int deleting;
  /* the app's wording (tf_sharing_set_wording) and what follows from it */
  TfMutex lock;
  char **keys, **texts;
  int n_words;
  char effective[40];         /* the version in force: the deck's, or custom-<24 hex> */
  char effective_hash[65];    /* "" without overrides */
  char *canonical;            /* the canonical text, NULL without overrides */
  char *copy;                 /* the client's copy deck with the overrides, NULL without overrides */
} Share;

static char *state_path(const TfClient *c) { return tf_path_join(tf__client_dir(c), "sharing.json"); }

static void share_save(const TfClient *c, const Share *s) {
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "{\"v\":1,\"consent\":\"%s\",\"asks\":%d,\"lastAskSession\":%d,\"sessions\":%d,\"thanked\":%s,\"decidedAt\":",
                s->deleting ? "final" : s->consent, s->asks, s->last_ask_session, s->sessions, s->thanked ? "true" : "false");
  if (s->decided_at) tf_buf_printf(&b, "%lld", s->decided_at);
  else tf_buf_puts(&b, "null");
  tf_buf_puts(&b, ",\"consentVersion\":");
  if (s->consent_version[0]) tf_buf_json_str(&b, s->consent_version);
  else tf_buf_puts(&b, "null");
  tf_buf_puts(&b, ",\"consentTextSha256\":");
  if (s->consent_hash[0]) tf_buf_json_str(&b, s->consent_hash);
  else tf_buf_puts(&b, "null");
  tf_buf_printf(&b, ",\"guardian\":%s,\"sharedSessions\":%d,\"pendingAsk\":%d,\"stamp\":%d}", s->guardian ? "true" : "false",
                s->shared_sessions, s->pending_ask, s->stamp + (s->deleting ? 1 : 0));
  char *p = state_path(c);
  if (p && b.p) tf_write_file(p, b.p, b.n);
  free(p);
  tf_buf_free(&b);
}

void tf__sharing_init(TfClient *c) {
  Share *s = (Share *)calloc(1, sizeof *s);
  if (!s) return;
  tf_mutex_init(&s->lock);
  snprintf(s->consent, sizeof s->consent, "unknown");
  char *p = state_path(c), *text = NULL;
  size_t n;
  if (p && !tf_read_file(p, &text, &n)) {
    TfJson *j = tf_json_parse(text, n, NULL);
    const char *cs = tf_json_str(tf_json_get(j, "consent"));
    if (cs && (!strcmp(cs, "unknown") || !strcmp(cs, "granted") || !strcmp(cs, "declined") || !strcmp(cs, "final")))
      snprintf(s->consent, sizeof s->consent, "%s", cs);
#define GETI(k, f)                                                \
  do {                                                            \
    const TfJson *v = tf_json_get(j, k);                          \
    if (v && v->type == TJ_NUM) s->f = (int)v->num;               \
    if (v && v->type == TJ_BOOL) s->f = v->boolean;               \
  } while (0)
    GETI("asks", asks);
    GETI("lastAskSession", last_ask_session);
    GETI("sessions", sessions);
    GETI("thanked", thanked);
    GETI("guardian", guardian);
    GETI("sharedSessions", shared_sessions);
    GETI("pendingAsk", pending_ask);
    GETI("stamp", stamp); /* absent in a file of 3.0.0: 0, the stamp of a recording that has none */
#undef GETI
    const TfJson *d = tf_json_get(j, "decidedAt");
    if (d && d->type == TJ_NUM) s->decided_at = (long long)d->num;
    const char *cv = tf_json_str(tf_json_get(j, "consentVersion"));
    if (cv) snprintf(s->consent_version, sizeof s->consent_version, "%s", cv);
    const char *ch = tf_json_str(tf_json_get(j, "consentTextSha256"));
    if (ch) snprintf(s->consent_hash, sizeof s->consent_hash, "%s", ch);
    tf_json_free(j);
    /* upgrades (see the top): a grant keeps the version it was given to, or 2.0.0's when none was written; a yes
     * that a deck or wording change put back to "unknown" is a grant again, without a sheet */
    int migrated = 0;
    if (!strcmp(s->consent, "unknown") && s->consent_version[0]) {
      snprintf(s->consent, sizeof s->consent, "granted");
      s->pending_ask = 0;
      migrated = 1;
    }
    if (!strcmp(s->consent, "granted") && !s->consent_version[0]) {
      snprintf(s->consent_version, sizeof s->consent_version, "%s", kVersion200);
      s->consent_hash[0] = 0;
      migrated = 1;
    }
    if (migrated) share_save(c, s);
  }
  free(text);
  free(p);
  deck_consent_version(tf__client_options(c)->language, s->effective, sizeof s->effective);
  *tf__client_sharing_slot(c) = s;
}
static void words_clear(Share *s) {
  for (int i = 0; i < s->n_words; ++i) {
    free(s->keys[i]);
    free(s->texts[i]);
  }
  free(s->keys);
  free(s->texts);
  free(s->canonical);
  free(s->copy);
  s->keys = s->texts = NULL;
  s->canonical = s->copy = NULL;
  s->n_words = 0;
  s->effective_hash[0] = 0;
}
void tf__sharing_free(TfClient *c) {
  Share *s = (Share *)*tf__client_sharing_slot(c);
  if (s) {
    words_clear(s);
    tf_mutex_destroy(&s->lock);
  }
  free(s);
  *tf__client_sharing_slot(c) = NULL;
}
static Share *share(TfClient *c) { return (Share *)*tf__client_sharing_slot(c); }

/* granted, to whatever version was in force then */
static int granted(Share *s) {
  tf_mutex_lock(&s->lock);
  const int ok = !strcmp(s->consent, "granted");
  tf_mutex_unlock(&s->lock);
  return ok;
}
/* asking is over: a yes (granted, or a version recorded by a yes, whatever came after it), sharing turned off
 * ("final", also after the third "Not now"), or three sheets shown */
static int asking_ended(Share *s) {
  tf_mutex_lock(&s->lock);
  const int ended = !strcmp(s->consent, "granted") || !strcmp(s->consent, "final") || s->consent_version[0] || s->asks >= MAX_ASKS;
  tf_mutex_unlock(&s->lock);
  return ended;
}

/* ------------------------------------------------------------------ the app's own wording */
/* the deck's string keys (impact.minutes by plural category) */
static const char *const kPlural[] = {"zero", "one", "two", "few", "many", "other"};
static int known_key(const char *key) {
  const char *en = tf__sharing_deck_json("en");
  TfJson *j = en ? tf_json_parse(en, strlen(en), NULL) : NULL;
  const TfJson *strs = tf_json_get(j, "strings");
  int ok = 0;
  const TfJson *v = tf_json_get(strs, key);
  if (v && v->type == TJ_STR) ok = 1;
  if (!ok && !strncmp(key, "impact.minutes.", 15) && tf_json_get(strs, "impact.minutes"))
    for (size_t i = 0; i < sizeof kPlural / sizeof kPlural[0]; ++i) ok |= !strcmp(key + 15, kPlural[i]);
  tf_json_free(j);
  return ok;
}
/* well-formed UTF-8 */
static int utf8_ok(const unsigned char *p) {
  while (*p) {
    int k = *p < 0x80 ? 0 : (*p & 0xe0) == 0xc0 ? 1 : (*p & 0xf0) == 0xe0 ? 2 : (*p & 0xf8) == 0xf0 ? 3 : -1;
    if (k < 0 || (k == 1 && *p < 0xc2) || (k == 3 && *p > 0xf4)) return 0;
    for (int i = 1; i <= k; ++i)
      if ((p[i] & 0xc0) != 0x80) return 0;
    p += k + 1;
  }
  return 1;
}
/* only white space (Unicode White_Space, the BOM too), or empty */
static int blank(const char *t) {
  const unsigned char *p = (const unsigned char *)t;
  while (*p) {
    if (*p == ' ' || (*p >= 0x09 && *p <= 0x0d)) {
      p++;
      continue;
    }
    unsigned cp = 0;
    int k = 0;
    if ((*p & 0xe0) == 0xc0) cp = *p & 0x1f, k = 1;
    else if ((*p & 0xf0) == 0xe0) cp = *p & 0x0f, k = 2;
    else if ((*p & 0xf8) == 0xf0) cp = *p & 0x07, k = 3;
    else return 0;
    for (int i = 1; i <= k; ++i) cp = (cp << 6) | (p[i] & 0x3f);
    const int ws = cp == 0x85 || cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
                   cp == 0x202f || cp == 0x205f || cp == 0x3000 || cp == 0xfeff;
    if (!ws) return 0;
    p += k + 1;
  }
  return 1;
}
static char *dup_s(const char *s) {
  const size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}
/* the canonical text of the shared vectors (make_consent_text.py) */
static char *canonical_text(const char *base, char **keys, char **texts, int n) {
  int *order = (int *)malloc(sizeof(int) * (size_t)(n ? n : 1));
  if (!order) return NULL;
  /* keys by their UTF-8 bytes (strcmp compares as unsigned char) */
  for (int i = 0; i < n; ++i) {
    int k = i;
    while (k > 0 && strcmp(keys[order[k - 1]], keys[i]) > 0) {
      order[k] = order[k - 1];
      k--;
    }
    order[k] = i;
  }
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_printf(&b, "quranlab.consent-text/1\nbase %s\n", base);
  for (int i = 0; i < n; ++i) {
    tf_buf_puts(&b, keys[order[i]]);
    tf_buf_puts(&b, "\t");
    for (const char *p = texts[order[i]]; *p; ++p) {
      if (*p == '\\') tf_buf_puts(&b, "\\\\");
      else if (*p == '\n') tf_buf_puts(&b, "\\n");
      else if (*p == '\r') tf_buf_puts(&b, "\\r");
      else if (*p == '\t') tf_buf_puts(&b, "\\t");
      else tf_buf_put(&b, p, 1);
    }
    tf_buf_puts(&b, "\n");
  }
  free(order);
  return tf_buf_take(&b);
}
static const char *override_of(char **keys, char **texts, int n, const char *key) {
  for (int i = 0; i < n; ++i)
    if (!strcmp(keys[i], key)) return texts[i];
  return NULL;
}
/* the deck with the overrides put in (impact.minutes by category) and the effective consent_version */
static char *merged_copy(const char *deck, const char *version, char **keys, char **texts, int n) {
  TfJson *j = deck ? tf_json_parse(deck, strlen(deck), NULL) : NULL;
  if (!j || j->type != TJ_OBJ) {
    tf_json_free(j);
    return NULL;
  }
  TfBuf b;
  tf_buf_init(&b);
  tf_buf_puts(&b, "{");
  for (int i = 0; i < j->n; ++i) {
    const TfJson *k = &j->kids[i];
    if (i) tf_buf_puts(&b, ",");
    tf_buf_json_str(&b, k->key);
    tf_buf_puts(&b, ":");
    if (!strcmp(k->key, "consent_version")) {
      tf_buf_json_str(&b, version);
    } else if (!strcmp(k->key, "strings") && k->type == TJ_OBJ) {
      tf_buf_puts(&b, "{");
      for (int m = 0; m < k->n; ++m) {
        const TfJson *v = &k->kids[m];
        if (m) tf_buf_puts(&b, ",");
        tf_buf_json_str(&b, v->key);
        tf_buf_puts(&b, ":");
        const char *o = v->type == TJ_STR ? override_of(keys, texts, n, v->key) : NULL;
        if (o) {
          tf_buf_json_str(&b, o);
        } else if (!strcmp(v->key, "impact.minutes") && v->type == TJ_OBJ) {
          tf_buf_puts(&b, "{");
          int w = 0;
          char pk[64];
          for (int q = 0; q < v->n; ++q) {
            snprintf(pk, sizeof pk, "impact.minutes.%s", v->kids[q].key);
            const char *po = override_of(keys, texts, n, pk);
            tf_buf_puts(&b, w++ ? "," : "");
            tf_buf_json_str(&b, v->kids[q].key);
            tf_buf_puts(&b, ":");
            if (po) tf_buf_json_str(&b, po);
            else tf_json_write(&b, &v->kids[q]);
          }
          /* a category the app worded that this language's deck lacks */
          for (int q = 0; q < n; ++q)
            if (!strncmp(keys[q], "impact.minutes.", 15) && !tf_json_get(v, keys[q] + 15)) {
              tf_buf_puts(&b, w++ ? "," : "");
              tf_buf_json_str(&b, keys[q] + 15);
              tf_buf_puts(&b, ":");
              tf_buf_json_str(&b, texts[q]);
            }
          tf_buf_puts(&b, "}");
        } else {
          tf_json_write(&b, v);
        }
      }
      tf_buf_puts(&b, "}");
    } else {
      tf_json_write(&b, k);
    }
  }
  tf_buf_puts(&b, "}");
  tf_json_free(j);
  return tf_buf_take(&b);
}

int tf_sharing_set_wording(TfClient *c, const char *const *keys, const char *const *texts, int n) {
  tf__clear_error();
  Share *s = c ? share(c) : NULL;
  if (!s) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  if (n < 0 || (n > 0 && (!keys || !texts))) return tf__fail(TF_ERR_INVALID_ARGUMENT, "keys and texts are required");
  if (!keys) n = 0;
  for (int i = 0; i < n; ++i) {
    if (!keys[i] || !known_key(keys[i])) return tf__fail(TF_ERR_INVALID_ARGUMENT, "wording: unknown key %s", keys[i] ? keys[i] : "(null)");
    if (!texts[i] || !utf8_ok((const unsigned char *)texts[i])) return tf__fail(TF_ERR_INVALID_ARGUMENT, "wording: %s is not UTF-8 text", keys[i]);
    if (blank(texts[i])) return tf__fail(TF_ERR_INVALID_ARGUMENT, "wording: %s is empty or blank", keys[i]);
    for (int k = 0; k < i; ++k)
      if (!strcmp(keys[k], keys[i])) return tf__fail(TF_ERR_INVALID_ARGUMENT, "wording: %s is given twice", keys[i]);
  }
  char base[40];
  deck_consent_version(tf__client_options(c)->language, base, sizeof base);
  char **nk = NULL, **nt = NULL, *canon = NULL, *copy = NULL;
  char eff[40], hash[65] = "";
  snprintf(eff, sizeof eff, "%s", base);
  if (n > 0) {
    nk = (char **)calloc((size_t)n, sizeof(char *));
    nt = (char **)calloc((size_t)n, sizeof(char *));
    int oom = !nk || !nt;
    for (int i = 0; !oom && i < n; ++i) {
      nk[i] = dup_s(keys[i]);
      nt[i] = dup_s(texts[i]);
      oom = !nk[i] || !nt[i];
    }
    canon = oom ? NULL : canonical_text(base, nk, nt, n);
    if (canon) {
      uint8_t h[32];
      tf_sha256(canon, strlen(canon), h);
      tf_hex_encode(h, 32, hash);
      snprintf(eff, sizeof eff, "custom-%.24s", hash);
      copy = merged_copy(tf__sharing_deck_json(tf__client_options(c)->language), eff, nk, nt, n);
    }
    if (!canon || !copy) {
      for (int i = 0; i < n; ++i) {
        if (nk) free(nk[i]);
        if (nt) free(nt[i]);
      }
      free(nk);
      free(nt);
      free(canon);
      free(copy);
      return tf__fail(TF__ERR_NO_MEMORY, "out of memory");
    }
  }
  tf_mutex_lock(&s->lock);
  words_clear(s);
  s->keys = nk;
  s->texts = nt;
  s->n_words = n;
  s->canonical = canon;
  s->copy = copy;
  snprintf(s->effective, sizeof s->effective, "%s", eff);
  snprintf(s->effective_hash, sizeof s->effective_hash, "%s", hash);
  tf_mutex_unlock(&s->lock);
  return 0;
}
const char *tf__sharing_consent_version(TfClient *c) { return c && share(c) ? share(c)->effective : NULL; }
const char *tf__sharing_consent_sha256(TfClient *c) { return c && share(c) && share(c)->effective_hash[0] ? share(c)->effective_hash : NULL; }
const char *tf__sharing_consent_text(TfClient *c) { return c && share(c) ? share(c)->canonical : NULL; }
const char *tf__client_sharing_copy_json(TfClient *c) {
  if (!c || !share(c)) return NULL;
  return share(c)->copy ? share(c)->copy : tf__sharing_deck_json(tf__client_options(c)->language);
}

/* enabled = the signed config in force offers sharing to this app: flags.contributions literally true. Quran Lab
 * decides this per app (11-09); since 2.0.1 the app's contributions option is ignored, and without a config in
 * force nothing is offered (2.0.0 defaulted to on when the config was missing). Enabled is never consent:
 * tf__client_sharing_state and every capture and upload also need the learner's "granted". */
static int sharing_enabled(const TfClient *c) { return tf__client_flag(c, "contributions", 0); }

/* may this install upload (web and desktop upload at the lower trust tier): flags.upload_trust[windows|linux]
 * absent or "capped" = yes (unattested token, per-key and per-contributor caps, sessions marked attested=false by
 * the server); "off", or "attested" (which no desktop install can satisfy) = no */
int tf__sharing_may_upload(const TfClient *c) {
  char t[16];
  tf__client_upload_trust(c, t, sizeof t);
  return sharing_enabled(c) && (!t[0] || !strcmp(t, "capped"));
}
/* what the learner agreed to (the upload's consent_version, and consent_text_sha256 for the app's own wording;
 * hash "" without it) */
void tf__sharing_grant(TfClient *c, char version[40], char hash[65]) {
  Share *s = share(c);
  version[0] = hash[0] = 0;
  if (!s) return;
  tf_mutex_lock(&s->lock);
  snprintf(version, 40, "%s", s->consent_version);
  snprintf(hash, 65, "%s", s->consent_hash);
  tf_mutex_unlock(&s->lock);
}
/* a session went up completely: counted for the status (the thank-you after the first) */
void tf__sharing_uploaded(TfClient *c) {
  Share *s = share(c);
  if (!s) return;
  s->shared_sessions++;
  share_save(c, s);
}
void tf__share_wipe(TfClient *c);
void tf__share_forget_seq(TfClient *c);

/* the SDK asks: the app's callback (its own UI or a sheet it shows) or, without one, a pending ask the app reads
 * from the status. Either way it is one of the three showings (asks, telemetry share_asked). */
static void ask(TfClient *c, int reask) {
  Share *s = share(c);
  const TfClientConfig *o = tf__client_options(c);
  s->asks++;
  s->last_ask_session = s->sessions;
  s->pending_ask = o->on_consent ? 0 : 1;
  share_save(c, s);
  tf__telemetry_add(c, "share_asked", 1);
  if (o->on_consent) {
    TfConsentRequest r;
    memset(&r, 0, sizeof r);
    r.reask = reask ? s->asks - 1 : 0;
    r.guardian = o->children;
    r.copy_json = tf__client_sharing_copy_json(c);
    r.consent_version = s->effective;
    o->on_consent(o->user, &r);
  }
}

void tf__sharing_session(TfClient *c, int feedback, double audio_s) {
  Share *s = share(c);
  if (!s) return;
  (void)audio_s;
  s->sessions++;
  share_save(c, s);
  if (!sharing_enabled(c) || !feedback || asking_ended(s)) return;
  /* the first ask; after a "Not now" (or a sheet left unanswered) a re-ask, 7 sessions later at the earliest */
  if (s->asks == 0 && !strcmp(s->consent, "unknown")) ask(c, 0);
  else if (s->sessions - s->last_ask_session >= REASK_SESSIONS) ask(c, 1);
}

int tf__client_sharing_ask_now(TfClient *c) {
  if (!c || !share(c)) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  Share *s = share(c);
  if (!sharing_enabled(c)) return tf__fail(TF__ERR_NOT_AVAILABLE, "sharing is not offered to this app (the signed config's flags.contributions)");
  if (asking_ended(s)) return 0; /* a yes, sharing turned off, or three sheets shown: nothing to ask */
  ask(c, s->asks > 0);
  return 0;
}

/* the learner agreed (a sheet's answer, or the settings switch): records the version in force now, kept for good */
static void grant(TfClient *c, Share *s) {
  tf_mutex_lock(&s->lock);
  if (strcmp(s->consent, "granted")) s->stamp++; /* sharing comes on: whatever is on disk is from before */
  snprintf(s->consent, sizeof s->consent, "granted");
  snprintf(s->consent_version, sizeof s->consent_version, "%s", s->effective);
  snprintf(s->consent_hash, sizeof s->consent_hash, "%s", s->effective_hash);
  tf_mutex_unlock(&s->lock);
  s->decided_at = (long long)time(NULL);
  s->guardian = tf__client_options(c)->children;
}

/* the learner's answer to a sheet: the SDK's ask through the app's callback or its own UI, only from the learner's
 * own tap (counted: share_yes, share_no) */
int tf__client_sharing_answer(TfClient *c, int yes) {
  Share *s = c ? share(c) : NULL;
  if (!s) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  s->pending_ask = 0;
  if (yes) {
    grant(c, s);
  } else {
    tf_mutex_lock(&s->lock);
    snprintf(s->consent, sizeof s->consent, "%s", s->asks >= MAX_ASKS ? "final" : "declined");
    tf_mutex_unlock(&s->lock);
  }
  share_save(c, s);
  tf__telemetry_add(c, yes ? "share_yes" : "share_no", 1);
  return 0;
}

/* the settings switch (not a sheet answer: not counted as share_yes / share_no) */
int tf__client_sharing_set(TfClient *c, int on) {
  Share *s = c ? share(c) : NULL;
  if (!s) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  s->pending_ask = 0;
  if (on) {
    grant(c, s);
    share_save(c, s);
    return 0;
  }
  tf_mutex_lock(&s->lock);
  snprintf(s->consent, sizeof s->consent, "final");
  s->stamp++; /* in the same write as the consent: every recording on disk is now one from before the stop */
  tf_mutex_unlock(&s->lock);
  share_save(c, s);
  /* off stops uploads at once: the request in flight is aborted, the unsent queue is deleted */
  tf__share_wipe(c);
  /* 3.0.1: written once more now that the uploader has stopped. A recording that finished uploading at the same
   * moment writes this file too (its count), and could have written it with the consent it read before the off. */
  share_save(c, s);
  return 0;
}

int tf__client_sharing_state(TfClient *c) { return c && share(c) && granted(share(c)) && sharing_enabled(c); }
/* the learner's own yes, whatever the signed config offers now */
int tf__sharing_granted(TfClient *c) { return c && share(c) && granted(share(c)); }
int tf__sharing_stamp(TfClient *c) {
  Share *s = c ? share(c) : NULL;
  if (!s) return 0;
  tf_mutex_lock(&s->lock);
  const int v = s->stamp;
  tf_mutex_unlock(&s->lock);
  return v;
}

/* "updateNeeded" stays in the JSON for apps that read it, always false since 2.0.1 */
const char *tf__client_sharing_status_json(TfClient *c) {
  static _Thread_local char buf[1024];
  Share *s = c ? share(c) : NULL;
  if (!s) return NULL;
  char trust[16];
  tf__client_upload_trust(c, trust, sizeof trust);
  tf_mutex_lock(&s->lock);
  snprintf(buf, sizeof buf,
           "{\"consent\":\"%s\",\"enabled\":%s,\"asks\":%d,\"sessions\":%d,\"pendingAsk\":%s,\"thanked\":%s,\"guardian\":%s,"
           "\"sharedSessions\":%d,\"uploadTrust\":\"%s\",\"mayUpload\":%s,\"consentVersion\":\"%s\",\"consentTextSha256\":%s%s%s,"
           "\"effectiveConsentVersion\":\"%s\",\"updateNeeded\":false}",
           s->consent, sharing_enabled(c) ? "true" : "false", s->asks, s->sessions, s->pending_ask ? "true" : "false",
           s->thanked ? "true" : "false", s->guardian ? "true" : "false", s->shared_sessions, trust[0] ? trust : "capped",
           tf__sharing_may_upload(c) ? "true" : "false", s->consent_version, s->consent_hash[0] ? "\"" : "null", s->consent_hash,
           s->consent_hash[0] ? "\"" : "", s->effective);
  tf_mutex_unlock(&s->lock);
  return buf;
}


int tf__client_call(TfClient *c, const char *method, const char *path, const char *body, int auth, const char *extra,
                    TfHttpResponse *r);
const char *tf__client_api(const TfClient *c);
const char *tf__client_app(const TfClient *c);
const char *tf__client_platform(const TfClient *c);
int tf__contributor(const char *dir, int create, char id[33], char contributor_key[90], char recovery_hash[65]);
int tf__contributor_recovery(const char *dir, char out[33]);
int tf__contributor_sign(const char *dir, const char *app, const char *nonce, long long ts, char sig_b64url[90]);
void tf__contributor_forget(const char *dir);
extern long long tf__now_override;

int tf__client_sharing_recovery_code(TfClient *c, char out[33]) {
  if (!c) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  return tf__contributor_recovery(tf__client_dir(c), out) ? tf__fail(TF__ERR_NOT_AVAILABLE, "nothing was shared from this install yet") : 0;
}

/* deletion routes take NO app key or token (403 credential_not_accepted): plain JSON POSTs */
static int post_plain(TfClient *c, const char *path, const char *body, TfHttpResponse *r) {
  char url[1024];
  snprintf(url, sizeof url, "%s%s", tf__client_api(c), path);
  const char *hdr[] = {"content-type: application/json", NULL};
  TfHttpRequest q;
  memset(&q, 0, sizeof q);
  q.method = "POST";
  q.url = url;
  q.headers = hdr;
  q.body = body;
  q.body_n = strlen(body);
  q.timeout_s = 30;
  return tf_http(&q, r);
}

/* The server confirmed "Delete what I shared". Since 3.0.1 sharing goes OFF with it, as the settings switch turns
 * it off (asking has ended: the learner is not asked again, and turns sharing on again with the switch). Then:
 * the unsent queue and the open recording are deleted and the feed's ring is emptied, the contributor id and its
 * key are forgotten (a recording shared after the learner turned sharing on again gets a new one), the count shown
 * to the learner starts again, and the count of shared recordings (take_seq) and the end of the previous one are
 * forgotten. Sharing goes off first, so nothing is captured or sent while the rest is done. */
void tf__sharing_deleted(TfClient *c) {
  Share *s = share(c);
  s->pending_ask = 0;
  tf_mutex_lock(&s->lock);
  snprintf(s->consent, sizeof s->consent, "final");
  s->stamp++;
  s->deleting = 0; /* what the file said while the server was asked is now what memory says */
  tf_mutex_unlock(&s->lock);
  share_save(c, s);
  tf__share_wipe(c);
  tf__contributor_forget(tf__client_dir(c));
  s->shared_sessions = 0;
  share_save(c, s);
  tf__share_forget_seq(c);
}

/* "Delete what I shared": challenge, the device key's signature, delete (PLATFORM_CLIENT.md 9.1). 0 when the
 * server took it (202). */
static int delete_ask(TfClient *c, const char *id);
void tf__share_hold(TfClient *c);
void tf__share_resume(TfClient *c);

/* 3.0.1: the queue is stopped BEFORE the server is asked (the request in flight is aborted, no pass begins), so
 * no recording of the contributor being deleted reaches the server while it deletes or after; and the off is
 * written to disk before the server is asked. When the server took the deletion, sharing goes off and everything
 * unsent is deleted (tf__sharing_deleted). When it did not (an error, no connection), nothing has changed:
 * sharing is as it was, the queue is whole and carries on. A process that dies in between starts with sharing
 * off: the learner asked for a deletion, so nothing is sent until they say otherwise. */
int tf__client_sharing_delete(TfClient *c) {
  tf__clear_error();
  if (!c) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  char id[33];
  if (tf__contributor(tf__client_dir(c), 0, id, NULL, NULL)) return 0; /* nothing was ever shared from this install */
  Share *s = share(c);
  tf__share_hold(c);
  /* the off goes to disk before the server is asked (see `deleting`) */
  tf_mutex_lock(&s->lock);
  s->deleting = 1;
  tf_mutex_unlock(&s->lock);
  share_save(c, s);
  const int rc = delete_ask(c, id);
  if (!rc) {
    tf__sharing_deleted(c);
  } else {
    /* the server did not take it: the settings file is written back as it was (the error stays the call's) */
    tf_mutex_lock(&s->lock);
    s->deleting = 0;
    tf_mutex_unlock(&s->lock);
    share_save(c, s);
  }
  tf__share_resume(c);
  return rc;
}

static int delete_ask(TfClient *c, const char *id) {
  char app[600];
  snprintf(app, sizeof app, "%s:%s", tf__client_platform(c), tf__client_app(c));
  int rc = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    TfBuf b;
    tf_buf_init(&b);
    tf_buf_puts(&b, "{\"app\":");
    tf_buf_json_str(&b, app);
    tf_buf_printf(&b, ",\"contributor\":\"%s\"}", id);
    TfHttpResponse r;
    rc = post_plain(c, "/v1/contributions/challenge", b.p, &r);
    tf_buf_free(&b);
    if (rc) return rc;
    TfJson *j = r.status == 200 ? tf_json_parse(r.body, r.body_n, NULL) : NULL;
    char nonce[200] = "";
    if (tf_json_str(tf_json_get(j, "nonce"))) snprintf(nonce, sizeof nonce, "%s", tf_json_get(j, "nonce")->str);
    tf_json_free(j);
    const int st = r.status;
    tf_http_response_free(&r);
    if (!*nonce) return tf__fail(TF__ERR_HTTP, "deletion challenge: HTTP %d", st);
    const long long ts = tf__now_override ? tf__now_override : (long long)time(NULL);
    char sig[90];
    if (tf__contributor_sign(tf__client_dir(c), app, nonce, ts, sig)) return tf__fail(TF__ERR_NOT_AVAILABLE, "the device key cannot sign");
    tf_buf_init(&b);
    tf_buf_puts(&b, "{\"app\":");
    tf_buf_json_str(&b, app);
    tf_buf_printf(&b, ",\"contributor\":\"%s\",\"nonce\":", id);
    tf_buf_json_str(&b, nonce);
    tf_buf_printf(&b, ",\"timestamp\":%lld,\"action\":\"delete-all\",\"signature\":\"%s\"}", ts, sig);
    rc = post_plain(c, "/v1/contributions/delete", b.p, &r);
    tf_buf_free(&b);
    if (rc) return rc;
    const int ds = r.status;
    const int retry = ds == 401 && r.body && (strstr(r.body, "nonce_invalid") || strstr(r.body, "timestamp_invalid"));
    tf_http_response_free(&r);
    if (ds == 202) return 0;
    if (!retry) return tf__fail(TF__ERR_HTTP, "deletion: HTTP %d", ds);
  }
  return tf__fail(TF__ERR_HTTP, "deletion: the challenge was refused twice");
}

const char *tf__client_sharing_summary_json(TfClient *c) {
  static _Thread_local char out[1024];
  char id[33], hdr[64];
  if (!c || tf__contributor(tf__client_dir(c), 0, id, NULL, NULL)) return NULL;
  snprintf(hdr, sizeof hdr, "x-qlab-contributor: %s", id);
  TfHttpResponse r;
  if (tf__client_call(c, "GET", "/v1/contributions/summary", NULL, 1, hdr, &r)) return NULL;
  const int ok = r.status == 200 && r.body && r.body_n < sizeof out;
  if (ok) memcpy(out, r.body, r.body_n + 1);
  tf_http_response_free(&r);
  return ok ? out : NULL;
}

/* ------------------------------------------------------------------ the public calls (include/tajweedformer.h) */

int tf_sharing_status(TfClient *c, TfSharing *out) {
  tf__clear_error();
  Share *s = c ? share(c) : NULL;
  if (!s || !out) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  memset(out, 0, sizeof *out);
  out->is_available = sharing_enabled(c);
  out->is_on = tf__client_sharing_state(c);
  tf_mutex_lock(&s->lock);
  out->asks = s->asks;
  out->pending_ask = s->pending_ask;
  out->guardian = s->guardian;
  out->shared = s->shared_sessions;
  out->consent_version = s->effective;
  out->consent_text = s->canonical;
  out->consent_sha256 = s->effective_hash[0] ? s->effective_hash : NULL;
  tf_mutex_unlock(&s->lock);
  return 0;
}
int tf_sharing_is_available(TfClient *c) { return c && share(c) && sharing_enabled(c); }
int tf_sharing_is_on(TfClient *c) { return tf__client_sharing_state(c); }

int tf_sharing_set_on(TfClient *c, int on) {
  tf__clear_error();
  if (!c || !share(c)) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  /* off always works (it stops uploads and deletes the unsent queue); on needs Quran Lab's config to offer sharing */
  if (on && !sharing_enabled(c)) return tf__fail(TF_ERR_SHARING_UNAVAILABLE, NULL);
  return tf__client_sharing_set(c, on);
}
int tf_sharing_ask(TfClient *c) {
  tf__clear_error();
  if (!c || !share(c)) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  if (!sharing_enabled(c)) return tf__fail(TF_ERR_SHARING_UNAVAILABLE, NULL);
  return tf__client_sharing_ask_now(c);
}
int tf_sharing_answer(TfClient *c, int share_it) {
  tf__clear_error();
  return tf__client_sharing_answer(c, share_it);
}
const char *tf_sharing_copy_json(TfClient *c) { return tf__client_sharing_copy_json(c); }
const char *tf_sharing_summary_json(TfClient *c) {
  const char *s = tf__client_sharing_summary_json(c);
  tf__clear_error(); /* no summary (not shared yet, no network) is not an error: nothing is shown */
  return s;
}
int tf_sharing_delete(TfClient *c) {
  tf__clear_error();
  if (!c || !share(c)) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  const int rc = tf__client_sharing_delete(c);
  if (!rc) return 0;
  /* the learner must hear that nothing was deleted. The catalogue has no code for a request other than a download
   * that got no answer: the nearest is the server's refusal, with what happened as its detail. */
  if (tf__error_code() == TF__ERR_NOT_AVAILABLE) return tf__error_as(TF_ERR_STORAGE_FAILED);
  if (tf__error_code() == TF__ERR_NETWORK) return tf__fail(TF_ERR_SERVER_REJECTED, "the deletion request got no answer (no connection); nothing was deleted");
  return tf__error_as(TF_ERR_SERVER_REJECTED);
}
int tf_sharing_recovery_code(TfClient *c, char out[33]) {
  tf__clear_error();
  if (!c || !out) return tf__fail(TF_ERR_INVALID_ARGUMENT, "no client");
  out[0] = 0;
  if (tf__client_sharing_recovery_code(c, out)) {
    out[0] = 0; /* nothing was shared from this install yet: no code, and no error */
    tf__clear_error();
  }
  return 0;
}
