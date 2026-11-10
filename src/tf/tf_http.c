/* HTTP for the platform client and the model downloader: WinHTTP on Windows, libcurl elsewhere.
 * One synchronous request at a time per call; TLS is the system's (Schannel / libcurl's). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_internal.h"

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

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

static wchar_t *widen(const char *s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n > 0 ? n : 1);
  return w;
}

/* requests that can be cancelled from another thread: WinHttpCloseHandle on a handle a synchronous call is using
 * makes that call fail at once (the documented way to cancel); the slot says who closes it */
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

static int http_native(const TfHttpRequest *q, TfHttpResponse *r) {
  int rc = -1, slot = -1;
  HINTERNET ses = NULL, con = NULL, req = NULL;
  wchar_t *wurl = widen(q->url), *wmethod = widen(q->method ? q->method : "GET");
  FILE *up = NULL, *down = NULL;
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
    tf__fail(TF_ERR_ARGUMENT, "bad URL");
    goto done;
  }
  wchar_t full[8192];
  _snwprintf(full, 8192, L"%s%s", path, extra);
  full[8191] = 0;
  ses = WinHttpOpen(L"TajweedFormer-native/2.0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) {
    tf__fail(TF_ERR_NETWORK, "WinHttpOpen failed (%lu)", GetLastError());
    goto done;
  }
  const int to = (q->timeout_s > 0 ? q->timeout_s : 60) * 1000;
  WinHttpSetTimeouts(ses, to, to, to, to);
  con = WinHttpConnect(ses, host, uc.nPort, 0);
  req = con ? WinHttpOpenRequest(con, wmethod, full, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
            : NULL;
  if (!req) {
    tf__fail(TF_ERR_NETWORK, "cannot open the request (%lu)", GetLastError());
    goto done;
  }
  if (q->cancel) {
    slot = slot_add(req, q->cancel);
    if (slot < 0) {
      tf__fail(TF_ERR_NETWORK, "cancelled");
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
  }
  const void *body = q->body;
  size_t body_n = q->body_n;
  if (q->upload_file) {
    if (tf_read_file(q->upload_file, &upbuf, &body_n)) {
      tf__fail(TF_ERR_IO, "cannot read the upload file");
      goto done;
    }
    body = upbuf;
  }
  if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body, (DWORD)body_n, (DWORD)body_n, 0) ||
      !WinHttpReceiveResponse(req, NULL)) {
    if (q->cancel && *q->cancel) tf__fail(TF_ERR_NETWORK, "cancelled");
    else tf__fail(TF_ERR_NETWORK, "the request failed (%lu)", GetLastError());
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
  const int to_file = q->download_file && (r->status == 200 || r->status == 206);
  if (to_file) {
    down = fopen(q->download_file, r->status == 206 ? "ab" : "wb");
    if (!down) {
      tf__fail(TF_ERR_IO, "cannot write the download");
      goto done;
    }
  }
  TfBuf b;
  tf_buf_init(&b);
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail)) {
      tf_buf_free(&b);
      tf__fail(TF_ERR_NETWORK, "reading the response failed (%lu)", GetLastError());
      goto done;
    }
    if (!avail) break;
    char buf[65536];
    DWORD got = 0;
    if (!WinHttpReadData(req, buf, avail > sizeof buf ? (DWORD)sizeof buf : avail, &got) || !got) break;
    if (to_file) {
      if (fwrite(buf, 1, got, down) != got) {
        tf_buf_free(&b);
        tf__fail(TF_ERR_IO, "writing the download failed");
        goto done;
      }
    } else {
      tf_buf_put(&b, buf, got);
    }
  }
  r->body_n = b.n;
  r->body = tf_buf_take(&b);
  rc = 0;
done:
  if (down) fclose(down);
  if (up) fclose(up);
  free(upbuf);
  if (req && (slot < 0 || slot_remove(slot))) WinHttpCloseHandle(req);
  if (con) WinHttpCloseHandle(con);
  if (ses) WinHttpCloseHandle(ses);
  free(wurl);
  free(wmethod);
  return rc;
}

#else
#include <curl/curl.h>

typedef struct {
  TfBuf body;
  FILE *file;
  TfHttpResponse *r;
  const TfHttpRequest *q;
  int status_seen;
} CurlCtx;

static size_t on_header(char *p, size_t s, size_t n, void *u) {
  CurlCtx *c = (CurlCtx *)u;
  keep_header(c->r, p, s * n);
  return s * n;
}
static size_t on_body(char *p, size_t s, size_t n, void *u) {
  CurlCtx *c = (CurlCtx *)u;
  if (!c->status_seen) {
    long code = 0;
    c->status_seen = 1;
    (void)code;
  }
  if (c->file) return fwrite(p, 1, s * n, c->file);
  tf_buf_put(&c->body, p, s * n);
  return c->body.oom ? 0 : s * n;
}

void tf__http_cancel(volatile int *flag) {
  if (flag) __atomic_store_n(flag, 1, __ATOMIC_SEQ_CST);
}
static int on_progress(void *u, curl_off_t a, curl_off_t b, curl_off_t c2, curl_off_t d) {
  (void)a, (void)b, (void)c2, (void)d;
  const CurlCtx *c = (const CurlCtx *)u;
  return c->q->cancel && __atomic_load_n(c->q->cancel, __ATOMIC_SEQ_CST) ? 1 : 0;
}

static int http_native(const TfHttpRequest *q, TfHttpResponse *r) {
  static int inited;
  if (!inited) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    inited = 1;
  }
  CURL *h = curl_easy_init();
  if (!h) return tf__fail(TF_ERR_NETWORK, "curl_easy_init failed");
  CurlCtx c;
  memset(&c, 0, sizeof c);
  c.r = r;
  c.q = q;
  tf_buf_init(&c.body);
  struct curl_slist *hs = NULL;
  for (const char *const *x = q->headers; x && *x; ++x) hs = curl_slist_append(hs, *x);
  hs = curl_slist_append(hs, "Expect:");
  char *upbuf = NULL;
  size_t up_n = 0;
  curl_easy_setopt(h, CURLOPT_URL, q->url);
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(h, CURLOPT_USERAGENT, "TajweedFormer-native/" TF_SDK_VERSION);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(h, CURLOPT_TIMEOUT, (long)(q->timeout_s > 0 ? q->timeout_s : 600));
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &c);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &c);
  if (q->cancel) {
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &c);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
  }
  const char *method = q->method ? q->method : "GET";
  if (q->upload_file) {
    if (tf_read_file(q->upload_file, &upbuf, &up_n)) {
      curl_slist_free_all(hs);
      curl_easy_cleanup(h);
      return tf__fail(TF_ERR_IO, "cannot read the upload file");
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
  if (q->download_file) {
    /* opened lazily below would need the status first; open in append mode for ranges, truncate otherwise */
    c.file = fopen(q->download_file, q->range_from > 0 ? "ab" : "wb");
    if (!c.file) {
      curl_slist_free_all(hs);
      curl_easy_cleanup(h);
      free(upbuf);
      return tf__fail(TF_ERR_IO, "cannot write the download");
    }
  }
  const CURLcode cc = curl_easy_perform(h);
  long code = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
  r->status = (int)code;
  if (c.file) fclose(c.file);
  curl_slist_free_all(hs);
  curl_easy_cleanup(h);
  free(upbuf);
  if (cc != CURLE_OK) {
    tf_buf_free(&c.body);
    return tf__fail(TF_ERR_NETWORK, "%s", curl_easy_strerror(cc));
  }
  r->body_n = c.body.n;
  r->body = tf_buf_take(&c.body);
  return 0;
}
#endif

int tf_http(const TfHttpRequest *q, TfHttpResponse *r) {
  memset(r, 0, sizeof *r);
  r->content_length = -1;
  if (g_transport) return g_transport(q, r, g_transport_user);
  return http_native(q, r);
}
