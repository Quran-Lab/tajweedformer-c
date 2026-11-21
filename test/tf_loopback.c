/* The loopback stand-in server of tf_loopback.h (tests only). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET lb_sock;
#define LB_BAD INVALID_SOCKET
#define lb_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int lb_sock;
#define LB_BAD (-1)
#define lb_close close
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_loopback.h"

struct TfLbServer {
  char buf[1 << 20]; /* this server's own: several servers run at once (a download's lanes beside the lease) */
  lb_sock fd;
  int port;
  volatile int stop;
  int silent;        /* accept and hold, never answer */
  lb_sock held[32];
  int nheld;
  TfLbHandler h;
  void *user;
#ifdef _WIN32
  HANDLE t;
#else
  pthread_t t;
#endif
};

static void lb_init(void) {
#ifdef _WIN32
  static int done;
  if (!done) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    done = 1;
  }
#endif
}
static lb_sock lb_listen(int *port, int listen_too) {
  lb_init();
  lb_sock fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == LB_BAD) return LB_BAD;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 only: nothing outside this machine reaches it */
  a.sin_port = 0;
  socklen_t al = sizeof a;
  if (bind(fd, (struct sockaddr *)&a, sizeof a) || getsockname(fd, (struct sockaddr *)&a, &al) || (listen_too && listen(fd, 16))) {
    lb_close(fd);
    return LB_BAD;
  }
  *port = ntohs(a.sin_port);
  return fd;
}
int tf_lb_closed_port(void) {
  int port = 0;
  lb_sock fd = lb_listen(&port, 0);
  if (fd == LB_BAD) return 0;
  lb_close(fd);
  return port;
}
static void send_all(lb_sock c, const char *p, size_t n) {
  while (n) {
    const int k = send(c, p, (int)(n > 65536 ? 65536 : n), 0);
    if (k <= 0) return;
    p += k;
    n -= (size_t)k;
  }
}
/* a reset, not an orderly end: the client sees the connection fail */
static void reset_close(lb_sock c) {
  struct linger l = {1, 0};
  setsockopt(c, SOL_SOCKET, SO_LINGER, (const char *)&l, sizeof l);
  lb_close(c);
}
const char *tf_lb_header(const TfLbRequest *q, const char *name, char *out, size_t n) {
  out[0] = 0;
  const size_t k = strlen(name);
  for (const char *p = q->headers; *p;) {
    const char *e = strstr(p, "\r\n");
    if (!e) break;
    if ((size_t)(e - p) > k && !strncmp(p, name, k) && p[k] == ':') {
      const char *v = p + k + 1;
      while (*v == ' ') ++v;
      snprintf(out, n, "%.*s", (int)(e - v), v);
      return out;
    }
    p = e + 2;
  }
  return out;
}
static void serve_one(TfLbServer *s, lb_sock c) {
  char *const buf = s->buf;
  size_t n = 0;
  /* a client that neither sends nor closes holds the server for 5 s at most */
#ifdef _WIN32
  DWORD to = 5000;
#else
  struct timeval to = {5, 0};
#endif
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
  const char *end = NULL;
  /* the request line and headers */
  while (n + 1 < sizeof s->buf) {
    const int k = recv(c, buf + n, (int)(sizeof s->buf - 1 - n), 0);
    if (k <= 0) break;
    n += (size_t)k;
    buf[n] = 0;
    if ((end = strstr(buf, "\r\n\r\n")) != NULL) break;
  }
  if (!end) {
    lb_close(c);
    return;
  }
  TfLbRequest *q = (TfLbRequest *)calloc(1, sizeof *q);
  TfLbAnswer *a = (TfLbAnswer *)calloc(1, sizeof *a);
  if (!q || !a) {
    free(q);
    free(a);
    lb_close(c);
    return;
  }
  const char *line_end = strstr(buf, "\r\n");
  sscanf(buf, "%15s %2047s", q->method, q->path);
  /* header lines, names in lower case */
  size_t hn = 0;
  for (const char *p = line_end + 2; p < end + 2 && hn + 1 < sizeof q->headers; ++p) {
    const char *colon = strchr(p, ':'), *eol = strstr(p, "\r\n");
    if (!eol || eol > end + 2) break;
    for (const char *t = p; t < eol && hn + 3 < sizeof q->headers; ++t) q->headers[hn++] = (char)(colon && t < colon ? tolower((unsigned char)*t) : *t);
    q->headers[hn++] = '\r';
    q->headers[hn++] = '\n';
    p = eol + 1;
  }
  q->headers[hn] = 0;
  char cl[32];
  const long long want = atoll(tf_lb_header(q, "content-length", cl, sizeof cl));
  size_t have = n - (size_t)(end + 4 - buf);
  if (want > 0) {
    q->body = (char *)malloc((size_t)want + 1);
    if (q->body) {
      memcpy(q->body, end + 4, have);
      while ((long long)have < want) {
        const int k = recv(c, q->body + have, (int)((size_t)want - have), 0);
        if (k <= 0) break;
        have += (size_t)k;
      }
      q->body[have] = 0;
      q->body_n = have;
    }
  }
  a->cut = -1;
  s->h(s->user, q, a);
  if (a->close_now || !a->status) {
    reset_close(c);
  } else {
    char head[4096];
    const int hl = snprintf(head, sizeof head, "HTTP/1.1 %d Stand-in\r\nContent-Length: %llu\r\nConnection: close\r\n%s\r\n", a->status,
                            (unsigned long long)a->body_n, a->headers);
    send_all(c, head, (size_t)hl);
    const size_t body = a->cut >= 0 && (size_t)a->cut < a->body_n ? (size_t)a->cut : a->body_n;
    if (a->body && body) send_all(c, a->body, body);
    if (a->cut >= 0 && (size_t)a->cut < a->body_n) {
      /* the bytes sent reach the client first (a reset at once can discard what it has not read yet), then the reset */
#ifdef _WIN32
      Sleep(300);
#else
      usleep(300000);
#endif
      reset_close(c);
    } else {
#ifdef _WIN32
      shutdown(c, SD_SEND);
#else
      shutdown(c, SHUT_WR);
#endif
      char drain[256];
      while (recv(c, drain, sizeof drain, 0) > 0) {
      }
      lb_close(c);
    }
  }
  free(a->body);
  free(q->body);
  free(q);
  free(a);
}
#ifdef _WIN32
static DWORD WINAPI lb_thread(LPVOID p) {
#else
static void *lb_thread(void *p) {
#endif
  TfLbServer *s = (TfLbServer *)p;
  while (!s->stop) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s->fd, &r);
    struct timeval tv = {0, 100000};
    if (select((int)s->fd + 1, &r, NULL, NULL, &tv) <= 0) continue;
    lb_sock c = accept(s->fd, NULL, NULL);
    if (c == LB_BAD) continue;
    if (s->silent) {
      if (s->nheld < 32) s->held[s->nheld++] = c;
      else lb_close(c);
      continue;
    }
    serve_one(s, c);
  }
  return 0;
}
TfLbServer *tf_lb_start(TfLbHandler h, void *user) {
  TfLbServer *s = (TfLbServer *)calloc(1, sizeof *s);
  if (!s) return NULL;
  s->h = h;
  s->user = user;
  s->fd = lb_listen(&s->port, 1);
  if (s->fd == LB_BAD) {
    free(s);
    return NULL;
  }
#ifdef _WIN32
  s->t = CreateThread(NULL, 0, lb_thread, s, 0, NULL);
  if (!s->t) {
#else
  if (pthread_create(&s->t, NULL, lb_thread, s)) {
#endif
    lb_close(s->fd);
    free(s);
    return NULL;
  }
  return s;
}
TfLbServer *tf_lb_start_silent(void) {
  TfLbServer *s = (TfLbServer *)calloc(1, sizeof *s);
  if (!s) return NULL;
  s->silent = 1;
  s->fd = lb_listen(&s->port, 1);
  if (s->fd == LB_BAD) {
    free(s);
    return NULL;
  }
#ifdef _WIN32
  s->t = CreateThread(NULL, 0, lb_thread, s, 0, NULL);
  if (!s->t) {
#else
  if (pthread_create(&s->t, NULL, lb_thread, s)) {
#endif
    lb_close(s->fd);
    free(s);
    return NULL;
  }
  return s;
}
int tf_lb_port(const TfLbServer *s) { return s ? s->port : 0; }
void tf_lb_stop(TfLbServer *s) {
  if (!s) return;
  s->stop = 1;
#ifdef _WIN32
  WaitForSingleObject(s->t, INFINITE);
  CloseHandle(s->t);
#else
  pthread_join(s->t, NULL);
#endif
  for (int i = 0; i < s->nheld; ++i) lb_close(s->held[i]);
  lb_close(s->fd);
  free(s);
}
