/* A stand-in HTTP/1.1 server on 127.0.0.1 for tests of the real HTTP code (WinHTTP, libcurl): one thread, one
 * connection at a time, "Connection: close" on every answer. Its own file because winsock2.h must come before
 * windows.h. Tests only; never part of the library. */
#ifndef TF_LOOPBACK_H
#define TF_LOOPBACK_H

#include <stddef.h>

typedef struct {
  char method[16];
  char path[2048];      /* the request target: path and query */
  char headers[8192];   /* the header lines as received, each ending in \r\n, names in lower case */
  char *body;           /* NUL-terminated; NULL without one */
  size_t body_n;
} TfLbRequest;
typedef struct {
  int status;           /* 0 with close_now: no answer at all */
  char headers[2048];   /* more lines, each "name: value\r\n" */
  char *body;           /* malloc'd; the server frees it */
  size_t body_n;
  long long cut;        /* >= 0: send the headers (Content-Length of the whole body) and only this many body bytes,
                           then reset the connection; -1: the whole answer */
  int close_now;        /* reset the connection without an answer */
} TfLbAnswer;
typedef void (*TfLbHandler)(void *user, const TfLbRequest *q, TfLbAnswer *a);
typedef struct TfLbServer TfLbServer;

/* listens on 127.0.0.1 on a port the system picks; NULL when it cannot */
TfLbServer *tf_lb_start(TfLbHandler h, void *user);
/* the same, but every connection is accepted and then held open with nothing read or sent (a host that accepts and
 * never answers; a client speaking TLS to it waits in its handshake) until tf_lb_stop */
TfLbServer *tf_lb_start_silent(void);
int tf_lb_port(const TfLbServer *s);
void tf_lb_stop(TfLbServer *s);
/* a port of 127.0.0.1 nothing listens on (bound, then closed): a connection to it is refused */
int tf_lb_closed_port(void);
/* the value of a header of the request ("" when it has none); name in lower case */
const char *tf_lb_header(const TfLbRequest *q, const char *name, char *out, size_t n);

#endif
