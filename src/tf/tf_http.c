/* HTTP for the platform client and the model downloader: WinHTTP on Windows, libcurl elsewhere.
 * One synchronous request at a time per call; TLS is the system's (Schannel / libcurl's).
 *
 * NO request follows a redirect (SPEC 15.4, 3.1.3): the key, the app token, a recording's id and, on a PUT or a
 * 307, the body would go with it to the place the redirect names. Both transports are told so for every
 * request, here and nowhere else; a 3xx answer is the response. Quran Lab's hosts answer none.
 *
 * A model request (3.1.3) differs from the client's other calls in three ways, each asked for by the request:
 * - stall_ms: it ends when NO BYTE of the answer arrives for that long, and has no limit on its whole time, so a
 *   slow link that keeps delivering is never cut. What measures it is said at each transport below.
 * - conn: it goes on a connection that is kept for the whole download (TfHttpConn).
 * - on that connection HTTP/2 is asked for (WinHTTP: HTTP/3 too), and the answer says which protocol it came on. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

int tf__http_opened;
static TfHttpTransport g_transport;
static void *g_transport_user;
void tf_http_set_transport(TfHttpTransport t, void *user) {
  g_transport = t;
  g_transport_user = user;
}

void tf_http_response_free(TfHttpResponse *r) {
  if (!r) return;
  free(r->body);
  memset(r, 0, sizeof *r);
}

/* header lines we keep */
static void keep_header(TfHttpResponse *r, const char *line, size_t n) {
  const char *c = memchr(line, ':', n);
  if (!c) return;
  size_t kn = (size_t)(c - line);
  const char *v = c + 1;
  while (v < line + n && (*v == ' ' || *v == '\t')) v++;
  size_t vn = (size_t)(line + n - v);
  while (vn && (v[vn - 1] == '\r' || v[vn - 1] == '\n' || v[vn - 1] == ' ')) vn--;
#define HN(name) (kn == strlen(name) && !_tf_strnicmp(line, name, kn))
  if (HN("x-qlab-signature") && vn < sizeof r->signature) {
    memcpy(r->signature, v, vn);
    r->signature[vn] = 0;
  } else if (HN("x-qlab-key-id") && vn < sizeof r->key_id) {
    memcpy(r->key_id, v, vn);
    r->key_id[vn] = 0;
  } else if (HN("etag") && vn < sizeof r->etag) {
    memcpy(r->etag, v, vn);
    r->etag[vn] = 0;
  } else if (HN("content-length")) {
    r->content_length = atoll(v);
  } else if (HN("content-range") && vn > 6 && !_tf_strnicmp(v, "bytes ", 6)) {
    /* bytes S-E/T (a 416 has a star in the place of S-E): the first byte of the body in the file, and the
     * file's size */
    const char *sl = memchr(v, '/', vn);
    r->range_start = v[6] >= '0' && v[6] <= '9' ? atoll(v + 6) : -1;
    r->range_total = sl && sl + 1 < v + vn && sl[1] >= '0' && sl[1] <= '9' ? atoll(sl + 1) : -1;
  }
#undef HN
}

int _tf_strnicmp(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    int x = (unsigned char)a[i], y = (unsigned char)b[i];
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) return x - y;
    if (!x) return 0;
  }
  return 0;
}

/* The file of a download once the status is known: a 206 to our Range continues the file; a 200 is the whole
 * file. A 200 to a request without a Range starts the file. A 200 to a request WITH a Range (the server did not
 * honour it) goes to download_whole when the request names one, so that the bytes already in the file stay
 * until the caller has seen that all of the whole file came. Any other status leaves the files alone (its body
 * goes to the response). 0 when no file is to be written, 1 when *f is open, -1 when it cannot be opened. */
static int open_download(const TfHttpRequest *q, int status, FILE **f) {
  *f = NULL;
  if (!q->download_file || (status != 200 && status != 206)) return 0;
  if (status == 200 && q->range_from > 0 && q->download_whole) *f = fopen(q->download_whole, "wb");
  else *f = fopen(q->download_file, status == 206 && q->range_from > 0 ? "ab" : "wb");
  return *f ? 1 : -1;
}
/* how many bytes of body the file of a download may take, -1 for no limit (download_size) */
static long long download_cap(const TfHttpRequest *q, int status) {
  if (!q->download_file || q->download_size <= 0 || (status != 200 && status != 206)) return -1;
  return status == 206 ? q->download_size - q->range_from : q->download_size;
}

/* resolving, connecting and sending a model request: 30 s each (the stall limit is about the answer) */
#define TF_HTTP_STEP_MS 30000

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#include <winhttp.h>

/* Windows 10 1607 and later; headers older than that (and some MinGW ones) do not name them */
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif
#ifndef WINHTTP_OPTION_HTTP_PROTOCOL_USED
#define WINHTTP_OPTION_HTTP_PROTOCOL_USED 134
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP3
#define WINHTTP_PROTOCOL_FLAG_HTTP3 0x2
#endif
#ifndef WINHTTP_OPTION_REDIRECT_POLICY
#define WINHTTP_OPTION_REDIRECT_POLICY 88
#endif
#ifndef WINHTTP_OPTION_REDIRECT_POLICY_NEVER
#define WINHTTP_OPTION_REDIRECT_POLICY_NEVER 0
#endif

struct TfHttpConn {
  HINTERNET ses, con;
  wchar_t host[256];
  INTERNET_PORT port;
};
TfHttpConn *tf__http_conn_open(void) { return (TfHttpConn *)calloc(1, sizeof(TfHttpConn)); }
void tf__http_conn_close(TfHttpConn *k) {
  if (!k) return;
  if (k->con) WinHttpCloseHandle(k->con);
  if (k->ses) WinHttpCloseHandle(k->ses);
  free(k);
}

static wchar_t *widen(const char *s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n > 0 ? n : 1);
  return w;
}

/* A session. `newer`: HTTP/2 and HTTP/3 are asked for (the session of a kept connection). HTTP/3 is a flag of
 * Windows 11 and Server 2022; where the system refuses the pair, HTTP/2 alone is asked for, and where it refuses
 * that too (before Windows 10 1607) the session stays on HTTP/1.1. None of the three is an error. */
static HINTERNET open_session(int newer) {
  HINTERNET ses = WinHttpOpen(L"TajweedFormer-native/" TF_SDK_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
  if (ses) tf__http_opened++;
  if (ses && newer) {
    DWORD flags = WINHTTP_PROTOCOL_FLAG_HTTP2 | WINHTTP_PROTOCOL_FLAG_HTTP3;
    if (!WinHttpSetOption(ses, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &flags, sizeof flags)) {
      flags = WINHTTP_PROTOCOL_FLAG_HTTP2;
      WinHttpSetOption(ses, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &flags, sizeof flags);
    }
  }
  return ses;
}

/* requests that can be cancelled from another thread: WinHttpCloseHandle on a handle a synchronous call is using
 * makes that call fail at once (the documented way to cancel); the slot says who closes it. Only the request's
 * handle is closed: the session and the connection of a kept connection stay for whoever opened them. */
typedef struct {
  HINTERNET h;
  volatile int *cancel;
} CancelSlot;
static CancelSlot g_slots[32];
static SRWLOCK g_slot_lock = SRWLOCK_INIT;
static int slot_add(HINTERNET h, volatile int *cancel) {
  int at = -1;
  AcquireSRWLockExclusive(&g_slot_lock);
  if (!*cancel)
    for (int i = 0; i < 32; ++i)
      if (!g_slots[i].h) {
        g_slots[i].h = h;
        g_slots[i].cancel = cancel;
        at = i;
        break;
      }
  ReleaseSRWLockExclusive(&g_slot_lock);
  return at;
}
/* 1: the caller still owns the handle (close it); 0: the canceller closed it */
static int slot_remove(int at) {
  AcquireSRWLockExclusive(&g_slot_lock);
  const int mine = g_slots[at].h != NULL;
  g_slots[at].h = NULL;
  g_slots[at].cancel = NULL;
  ReleaseSRWLockExclusive(&g_slot_lock);
  return mine;
}
void tf__http_cancel(volatile int *flag) {
  if (!flag) return;
  AcquireSRWLockExclusive(&g_slot_lock);
  *flag = 1;
  for (int i = 0; i < 32; ++i)
    if (g_slots[i].h && g_slots[i].cancel == flag) {
      WinHttpCloseHandle(g_slots[i].h);
      g_slots[i].h = NULL;
    }
  ReleaseSRWLockExclusive(&g_slot_lock);
}

/* why a WinHTTP call failed, as the thread's error: a cancel, a stall, or the system's code */
static int fail_winhttp(const TfHttpRequest *q, const char *what) {
  const DWORD e = GetLastError();
  if (q->cancel && *q->cancel) return tf__fail(TF__ERR_NETWORK, "cancelled");
  if (e == ERROR_WINHTTP_TIMEOUT && q->stall_ms > 0) return tf__fail(TF__ERR_NETWORK, "%s: no byte for %d ms", what, q->stall_ms);
  return tf__fail(TF__ERR_NETWORK, "%s (%lu)", what, e);
}

static int http_native(const TfHttpRequest *q, TfHttpResponse *r) {
  int rc = -1, slot = -1;
  TfHttpConn *k = q->conn;
  HINTERNET ses = NULL, con = NULL, req = NULL;
  wchar_t *wurl = widen(q->url), *wmethod = widen(q->method ? q->method : "GET");
  FILE *down = NULL;
  char *upbuf = NULL;
  URL_COMPONENTS uc;
  memset(&uc, 0, sizeof uc);
  uc.dwStructSize = sizeof uc;
  wchar_t host[256], path[4096];
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 4096;
  wchar_t extra[4096];
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 4096;
  if (!wurl || !wmethod || !WinHttpCrackUrl(wurl, 0, 0, &uc)) {
    tf__fail(TF_ERR_INVALID_ARGUMENT, "bad URL");
    goto done;
  }
  wchar_t full[8192];
  _snwprintf(full, 8192, L"%s%s", path, extra);
  full[8191] = 0;
  if (k) {
    /* the kept connection: its session and its connection handle are made by the first request and used by
     * every later one (a request to another host or port, which a download never makes, gets a new handle) */
    if (!k->ses) k->ses = open_session(1);
    if (k->con && (wcscmp(k->host, host) || k->port != uc.nPort)) {
      WinHttpCloseHandle(k->con);
      k->con = NULL;
    }
    if (k->ses && !k->con) {
      k->con = WinHttpConnect(k->ses, host, uc.nPort, 0);
      wcsncpy(k->host, host, 255);
      k->host[255] = 0;
      k->port = uc.nPort;
    }
    ses = k->ses;
    con = k->con;
  } else {
    ses = open_session(0);
    con = ses ? WinHttpConnect(ses, host, uc.nPort, 0) : NULL;
  }
  if (!ses) {
    tf__fail(TF__ERR_NETWORK, "WinHttpOpen failed (%lu)", GetLastError());
    goto done;
  }
  req = con ? WinHttpOpenRequest(con, wmethod, full, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
            : NULL;
  if (!req) {
    tf__fail(TF__ERR_NETWORK, "cannot open the request (%lu)", GetLastError());
    goto done;
  }
  {
    /* SPEC 15.4: WinHTTP follows a redirect by itself (every one but https to plain http) and sends the
     * request's own headers on with it, and its body on a PUT and on a 307. Told never to, it hands the 3xx
     * answer back. A request whose policy cannot be set is not sent. (Before 3.1.3 nothing set it.) */
    DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never)) {
      tf__fail(TF__ERR_NETWORK, "cannot turn redirects off for the request (%lu)", GetLastError());
      goto done;
    }
  }
  if (q->stall_ms > 0) {
    /* The stall limit is WinHTTP's receive timeout. It is a limit on ONE wait for the answer: the wait for the
     * response headers (WinHttpReceiveResponse), and each wait for the next bytes of the body
     * (WinHttpQueryDataAvailable, WinHttpReadData). Every wait that returns bytes starts a new one, so it measures
     * the time without a byte, never the time of the transfer. Resolving, connecting and sending have their own
     * limit.
     * WinHTTP looks at its waits about every four seconds, so a stalled request ends up to four seconds after
     * the limit, never before it. Measured on Windows 11 with test/net_stand_in.py (TF_NET_PROBE_STALL_MS): limits
     * of 0.5, 1.5, 5 and 10 s ended a stalled request after 4, 4, 8 and 12 s. With 30 s that is 30 to 34 s. */
    WinHttpSetTimeouts(req, TF_HTTP_STEP_MS, TF_HTTP_STEP_MS, TF_HTTP_STEP_MS, q->stall_ms);
  } else {
    const int to = (q->timeout_s > 0 ? q->timeout_s : 60) * 1000;
    WinHttpSetTimeouts(req, to, to, to, to);
  }
  if (q->cancel) {
    slot = slot_add(req, q->cancel);
    if (slot < 0) {
      tf__fail(TF__ERR_NETWORK, "cancelled");
      goto done;
    }
  }
  for (const char *const *h = q->headers; h && *h; ++h) {
    wchar_t *w = widen(*h);
    if (w) WinHttpAddRequestHeaders(req, w, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    free(w);
  }
  if (q->range_from > 0) {
    wchar_t rh[64];
    _snwprintf(rh, 64, L"Range: bytes=%lld-", q->range_from);
    rh[63] = 0;
    WinHttpAddRequestHeaders(req, rh, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    if (q->if_range && *q->if_range) {
      char line[128];
      snprintf(line, sizeof line, "If-Range: %s", q->if_range);
      wchar_t *w = widen(line);
      if (w) WinHttpAddRequestHeaders(req, w, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
      free(w);
    }
  }
  const void *body = q->body;
  size_t body_n = q->body_n;
  if (q->upload_file) {
    if (tf_read_file(q->upload_file, &upbuf, &body_n)) {
      tf__fail(TF__ERR_IO, "cannot read the upload file");
      goto done;
    }
    body = upbuf;
  }
  if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body, (DWORD)body_n, (DWORD)body_n, 0) ||
      !WinHttpReceiveResponse(req, NULL)) {
    fail_winhttp(q, "the request failed");
    goto done;
  }
  DWORD st = 0, sz = sizeof st;
  WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz,
                      WINHTTP_NO_HEADER_INDEX);
  r->status = (int)st;
  DWORD hl = 0;
  WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &hl, WINHTTP_NO_HEADER_INDEX);
  if (hl) {
    wchar_t *hw = (wchar_t *)malloc(hl + 2);
    if (hw && WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, hw, &hl, WINHTTP_NO_HEADER_INDEX)) {
      const int n8 = WideCharToMultiByte(CP_UTF8, 0, hw, (int)(hl / 2), NULL, 0, NULL, NULL);
      char *h8 = (char *)malloc((size_t)n8 + 1);
      if (h8) {
        WideCharToMultiByte(CP_UTF8, 0, hw, (int)(hl / 2), h8, n8, NULL, NULL);
        h8[n8] = 0;
        for (char *s = h8; *s;) {
          char *e = strstr(s, "\r\n");
          const size_t n = e ? (size_t)(e - s) : strlen(s);
          keep_header(r, s, n);
          if (!e) break;
          s = e + 2;
        }
        free(h8);
      }
    }
    free(hw);
  }
  {
    /* the protocol this answer came on; a system that does not know the question has no HTTP/2 either */
    DWORD used = 0, usz = sizeof used;
    if (!WinHttpQueryOption(req, WINHTTP_OPTION_HTTP_PROTOCOL_USED, &used, &usz)) used = 0;
    snprintf(r->protocol, sizeof r->protocol, "%s", used & WINHTTP_PROTOCOL_FLAG_HTTP3 ? "h3" : used & WINHTTP_PROTOCOL_FLAG_HTTP2 ? "h2" : "http/1.1");
  }
  /* not the answer that was asked for: nothing of it is written, and the request has failed */
  if (q->accept && !q->accept(q->bytes_user, r)) {
    tf__fail(TF__ERR_NETWORK, "the answer (HTTP %d) is not the one that was asked for", r->status);
    goto done;
  }
  const int to_file = open_download(q, r->status, &down);
  if (to_file < 0) {
    tf__fail(TF__ERR_IO, "cannot write the download");
    goto done;
  }
  if (q->on_head) q->on_head(q->bytes_user, r);
  TfBuf b;
  tf_buf_init(&b);
  long long got_all = 0, in_file = 0;
  const long long cap = download_cap(q, r->status);
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail)) {
      tf_buf_free(&b);
      fail_winhttp(q, "reading the response failed");
      goto done;
    }
    if (!avail) break;
    char buf[65536];
    DWORD got = 0;
    if (!WinHttpReadData(req, buf, avail > sizeof buf ? (DWORD)sizeof buf : avail, &got)) {
      /* before 3.1.3 a failed read ended the loop as if the body were complete */
      tf_buf_free(&b);
      fail_winhttp(q, "reading the response failed");
      goto done;
    }
    if (!got) break;
    got_all += got;
    if (to_file) {
      /* a body that runs past the file's size is cut off there, and the request has failed */
      const int past = cap >= 0 && in_file + (long long)got > cap;
      const DWORD take = past ? (DWORD)(cap - in_file) : got;
      if (take && fwrite(buf, 1, take, down) != take) {
        tf_buf_free(&b);
        tf__fail(TF__ERR_IO, "writing the download failed");
        goto done;
      }
      in_file += take;
      if (take && q->on_bytes) q->on_bytes(q->bytes_user, (long long)take);
      if (past) {
        tf_buf_free(&b);
        tf__fail(TF__ERR_NETWORK, "the answer is longer than the file (cut off after %lld bytes)", in_file);
        goto done;
      }
    } else {
      tf_buf_put(&b, buf, got);
    }
  }
  /* WinHTTP reports a connection the server closed in the middle of the body as the end of the body. The length
   * the headers named says otherwise (before 3.1.3 the short body was taken for the whole answer). */
  if (r->content_length > got_all && r->status != 204 && r->status != 304 && strcmp(q->method ? q->method : "GET", "HEAD")) {
    tf_buf_free(&b);
    tf__fail(TF__ERR_NETWORK, "the connection ended after %lld of %lld bytes", got_all, r->content_length);
    goto done;
  }
  r->body_n = b.n;
  r->body = tf_buf_take(&b);
  rc = 0;
done:
  if (down) fclose(down);
  free(upbuf);
  if (req && (slot < 0 || slot_remove(slot))) WinHttpCloseHandle(req);
  if (!k) {
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
  }
  free(wurl);
  free(wmethod);
  return rc;
}

#else
#include <curl/curl.h>

struct TfHttpConn {
  CURL *h; /* one easy handle: libcurl keeps its connections with the handle and uses them again */
};
TfHttpConn *tf__http_conn_open(void) { return (TfHttpConn *)calloc(1, sizeof(TfHttpConn)); }
void tf__http_conn_close(TfHttpConn *k) {
  if (!k) return;
  if (k->h) curl_easy_cleanup(k->h);
  free(k);
}

typedef struct {
  TfBuf body;
  FILE *file;
  TfHttpResponse *r;
  const TfHttpRequest *q;
  CURL *h;
  int head_done;      /* the status is known: the protocol is read, the file is open, on_head was called */
  int got_byte;       /* a byte of the answer has arrived */
  int io_failed;      /* the download's file could not be opened or written */
  int refused;        /* accept said this is not the answer that was asked for: nothing of it is written */
  int too_long;       /* the body ran past the file's size: cut off there */
  long long cap, in_file; /* how many bytes the file may take (-1: no limit), and has taken */
  int stalled;        /* the stall limit ended the transfer */
  int answering;      /* the request is on its way: the stall clock runs */
  double start, last; /* when the transfer began; when the last byte of the answer arrived (tf__now_s) */
} CurlCtx;

/* once per answer, when its status is known (at the first byte of the body, or after an answer without one) */
static void on_status(CurlCtx *c) {
  if (c->head_done) return;
  c->head_done = 1;
  long code = 0;
  curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &code);
  c->r->status = (int)code;
#if LIBCURL_VERSION_NUM >= 0x073200 /* CURLINFO_HTTP_VERSION: 7.50.0 */
  long v = 0;
  curl_easy_getinfo(c->h, CURLINFO_HTTP_VERSION, &v);
  /* CURL_HTTP_VERSION_3 is 30 (7.66.0), CURL_HTTP_VERSION_2_0 is 3 */
  if (v > 0) snprintf(c->r->protocol, sizeof c->r->protocol, "%s", v >= 30 ? "h3" : v == CURL_HTTP_VERSION_2_0 ? "h2" : "http/1.1");
#endif
  if (c->q->accept && !c->q->accept(c->q->bytes_user, c->r)) {
    c->refused = 1;
    return;
  }
  c->cap = download_cap(c->q, c->r->status);
  if (open_download(c->q, c->r->status, &c->file) < 0) c->io_failed = 1;
  if (!c->io_failed && c->q->on_head) c->q->on_head(c->q->bytes_user, c->r);
}
static size_t on_header(char *p, size_t s, size_t n, void *u) {
  CurlCtx *c = (CurlCtx *)u;
  c->last = tf__now_s();
  c->got_byte = 1;
  keep_header(c->r, p, s * n);
  return s * n;
}
static size_t on_body(char *p, size_t s, size_t n, void *u) {
  CurlCtx *c = (CurlCtx *)u;
  c->last = tf__now_s();
  c->got_byte = 1;
  /* the body goes to the file only when the status says it is the file: before 3.1.3 the body of a refusal was
   * written into the partial file and its reason was never read, and a 200 to a Range was appended */
  on_status(c);
  if (c->io_failed || c->refused) return 0; /* ends the transfer */
  if (c->file) {
    /* a body that runs past the file's size is cut off there, and the request has failed */
    size_t take = s * n;
    if (c->cap >= 0 && c->in_file + (long long)take > c->cap) {
      take = (size_t)(c->cap - c->in_file);
      c->too_long = 1;
    }
    const size_t w = take ? fwrite(p, 1, take, c->file) : 0;
    c->in_file += (long long)w;
    if (w && c->q->on_bytes) c->q->on_bytes(c->q->bytes_user, (long long)w);
    if (w != take) c->io_failed = 1;
    return c->too_long || c->io_failed ? 0 : s * n;
  }
  tf_buf_put(&c->body, p, s * n);
  return c->body.oom ? 0 : s * n;
}

void tf__http_cancel(volatile int *flag) {
  if (flag) __atomic_store_n(flag, 1, __ATOMIC_SEQ_CST);
}
/* libcurl calls this about once a second and whenever something happens on the connection */
static int on_progress(void *u, curl_off_t a, curl_off_t b, curl_off_t c2, curl_off_t d) {
  (void)a, (void)b, (void)c2, (void)d;
  CurlCtx *c = (CurlCtx *)u;
  if (c->q->cancel && __atomic_load_n(c->q->cancel, __ATOMIC_SEQ_CST)) return 1;
  if (c->q->stall_ms > 0) {
    /* The stall limit: the time since the last byte of the answer arrived (a header line or a piece of the
     * body), and before the first byte the time since the connection was ready for the request (connecting has
     * its own limit).
     * libcurl's own low-speed limit is not used: it compares an average over several seconds with a rate, so it
     * would let a stall run for about five seconds more than asked and would cut a link that delivers less than
     * one byte a second, which is slow and not stalled. */
    const double now = tf__now_s();
    if (!c->answering) {
      double ready = 0;
      curl_easy_getinfo(c->h, CURLINFO_PRETRANSFER_TIME, &ready);
      if (c->got_byte || ready > 0 || (now - c->start) * 1000.0 > TF_HTTP_STEP_MS + 1000) {
        c->answering = 1;
        if (!c->got_byte) c->last = now; /* no byte of the answer yet: the clock starts here */
      }
    } else if ((now - c->last) * 1000.0 > c->q->stall_ms) {
      c->stalled = 1;
      return 1;
    }
  }
  return 0;
}

static int http_native(const TfHttpRequest *q, TfHttpResponse *r) {
  static int inited;
  if (!inited) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    inited = 1;
  }
  TfHttpConn *k = q->conn;
  CURL *h = NULL;
  if (k) {
    /* the kept connection: the handle of the first request, with its options set anew for each one (a reset
     * keeps the handle's live connections) */
    if (k->h) {
      curl_easy_reset(k->h);
    } else {
      k->h = curl_easy_init();
      if (k->h) tf__http_opened++;
    }
    h = k->h;
  } else {
    h = curl_easy_init();
    if (h) tf__http_opened++;
  }
  if (!h) return tf__fail(TF__ERR_NETWORK, "curl_easy_init failed");
  CurlCtx c;
  memset(&c, 0, sizeof c);
  c.r = r;
  c.q = q;
  c.h = h;
  c.start = c.last = tf__now_s();
  c.cap = -1;
  tf_buf_init(&c.body);
  struct curl_slist *hs = NULL;
  for (const char *const *x = q->headers; x && *x; ++x) hs = curl_slist_append(hs, *x);
  hs = curl_slist_append(hs, "Expect:");
  char cond[128];
  if (q->range_from > 0 && q->if_range && *q->if_range) {
    snprintf(cond, sizeof cond, "If-Range: %s", q->if_range);
    hs = curl_slist_append(hs, cond);
  }
  char *upbuf = NULL;
  size_t up_n = 0;
  curl_easy_setopt(h, CURLOPT_URL, q->url);
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(h, CURLOPT_USERAGENT, "TajweedFormer-native/" TF_SDK_VERSION);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  /* SPEC 15.4: libcurl follows no redirect unless it is asked to, and this library never asked. Said here all
   * the same, for every request: a 3xx answer is the response. The list of protocols a redirect may go to is
   * emptied as well where libcurl takes that (one that refuses an empty list keeps its own, which nothing
   * reaches while no redirect is followed) */
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500 /* CURLOPT_REDIR_PROTOCOLS_STR: 7.85.0 */
  curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "");
#else
  curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS, 0L);
#endif
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, (long)(TF_HTTP_STEP_MS / 1000));
  /* with a stall limit there is no limit on the whole transfer (on_progress ends a stalled one) */
  curl_easy_setopt(h, CURLOPT_TIMEOUT, q->stall_ms > 0 ? 0L : (long)(q->timeout_s > 0 ? q->timeout_s : 600));
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &c);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &c);
  if (q->cancel || q->stall_ms > 0) {
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &c);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
  }
#if LIBCURL_VERSION_NUM >= 0x072f00 /* CURL_HTTP_VERSION_2TLS: 7.47.0 */
  /* HTTP/2 over TLS where this libcurl has it; one built without it refuses the option and stays on HTTP/1.1 */
  if (k) curl_easy_setopt(h, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
#endif
  const char *method = q->method ? q->method : "GET";
  if (q->upload_file) {
    if (tf_read_file(q->upload_file, &upbuf, &up_n)) {
      curl_slist_free_all(hs);
      if (!k) curl_easy_cleanup(h);
      return tf__fail(TF__ERR_IO, "cannot read the upload file");
    }
  }
  if (strcmp(method, "GET")) {
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, upbuf ? upbuf : (q->body ? (const char *)q->body : ""));
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)(upbuf ? up_n : q->body_n));
  }
  if (q->range_from > 0) {
    char rg[48];
    snprintf(rg, sizeof rg, "%lld-", q->range_from);
    curl_easy_setopt(h, CURLOPT_RANGE, rg);
  }
  const CURLcode cc = curl_easy_perform(h);
  /* an answer without a body (or one that failed before its first byte): the status is read here */
  long code = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
  if (cc == CURLE_OK) on_status(&c);
  r->status = (int)code;
  if (c.file) fclose(c.file);
  curl_slist_free_all(hs);
  /* the options above point into this call's stack: a kept handle is not left with them */
  if (k) curl_easy_reset(h);
  else curl_easy_cleanup(h);
  free(upbuf);
  if (c.io_failed) {
    tf_buf_free(&c.body);
    return tf__fail(TF__ERR_IO, "cannot write the download");
  }
  if (c.refused) {
    tf_buf_free(&c.body);
    return tf__fail(TF__ERR_NETWORK, "the answer (HTTP %d) is not the one that was asked for", r->status);
  }
  if (c.too_long) {
    tf_buf_free(&c.body);
    return tf__fail(TF__ERR_NETWORK, "the answer is longer than the file (cut off after %lld bytes)", c.in_file);
  }
  if (cc != CURLE_OK) {
    tf_buf_free(&c.body);
    if (q->cancel && __atomic_load_n(q->cancel, __ATOMIC_SEQ_CST)) return tf__fail(TF__ERR_NETWORK, "cancelled");
    if (c.stalled) return tf__fail(TF__ERR_NETWORK, "the request failed: no byte for %d ms", q->stall_ms);
    return tf__fail(TF__ERR_NETWORK, "%s", curl_easy_strerror(cc));
  }
  r->body_n = c.body.n;
  r->body = tf_buf_take(&c.body);
  return 0;
}
#endif

int tf__http_native(const TfHttpRequest *q, TfHttpResponse *r) {
  memset(r, 0, sizeof *r);
  r->content_length = r->range_start = r->range_total = -1;
  return http_native(q, r);
}
int tf_http(const TfHttpRequest *q, TfHttpResponse *r) {
  if (!g_transport) return tf__http_native(q, r);
  memset(r, 0, sizeof *r);
  r->content_length = r->range_start = r->range_total = -1;
  return g_transport(q, r, g_transport_user);
}

/* ------------------------------------------------------------------ the kind of connection */
int tf__connection_override = -1;
int tf__connection(void) { return tf__connection_override >= 0 ? tf__connection_override : TF__CONNECTION_UNKNOWN; }
const char *tf__connection_name(int kind) {
  static const char *const names[] = {"unmetered", "metered", "none", "unknown"};
  return kind >= 0 && kind <= 3 ? names[kind] : "unknown";
}
