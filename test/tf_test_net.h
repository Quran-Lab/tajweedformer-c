/* The transport every test program starts with, and the one a test puts back when it is done with its own: a request
 * to this machine (127.0.0.1, [::1], localhost: the loopback stand-ins of tf_loopback.c and the stubs) goes to the
 * system's transport (WinHTTP, libcurl); a request to any other host fails as no connection and never leaves the
 * process. So no test reaches Quran Lab's hosts, the standbys or anyone else's, whatever key or hosts its client has.
 *
 * Why (2026-11-21): seven tf40_tests tests (lease, clock, container, attest, recited, outcome, grace_vectors) made
 * clients with the made-up key qlk_v1_test_... and no stand-in host, and with the system's transport each run sent 41
 * POST /v1/attest to api.quranlab.ai (401 every time). Those runs opened bad_key_probing incidents on the networks of
 * the CI machine and of the PC. Every test passes the same without the network.
 *
 * Tests only; never part of the library. */
#ifndef TF_TEST_NET_H
#define TF_TEST_NET_H

#include <string.h>

#include "tf_internal.h"

static int tf_test_url_is_loopback(const char *url) {
  static const char *const here[] = {"127.0.0.1", "[::1]", "localhost"};
  const char *h = url ? strstr(url, "://") : NULL;
  if (!h) return 0;
  h += 3;
  for (size_t i = 0; i < sizeof here / sizeof *here; ++i) {
    const size_t n = strlen(here[i]);
    if (!strncmp(h, here[i], n) && (h[n] == ':' || h[n] == '/' || h[n] == 0)) return 1;
  }
  return 0;
}

static int tf_test_net(const TfHttpRequest *q, TfHttpResponse *r, void *user) {
  (void)user;
  if (tf_test_url_is_loopback(q->url)) return tf__http_native(q, r);
  return tf__fail(TF__ERR_NETWORK, "tests reach no host but this machine");
}

#endif
