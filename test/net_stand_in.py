#!/usr/bin/env python3
"""A stand-in model host for the tests of the library's real HTTP code (WinHTTP on Windows, libcurl on Linux).

    python test/net_stand_in.py <tf_tests> <work dir>    start the server on a free port of 127.0.0.1, run
                                                         `tf_tests net http://127.0.0.1:<port> <work dir>`,
                                                         stop the server, exit with the tests' code
    python test/net_stand_in.py --serve [port]           serve until interrupted (for work by hand)

Python's standard library only. Plain HTTP/1.1 with connections that are kept: HTTP/2 and HTTP/3 need TLS and
are not served here, so the tests show the resume, the stall limit, the kept connection and the protocol record
on HTTP/1.1 and nothing about the newer protocols.

What it serves (all of it made here, in memory, the same in every run):

  GET /models/<repo>/<build>/sdk/v4.0/<file>   a small stand-in "model": manifest.json (format 3, signed with a
                                               THROWAWAY Ed25519 key made from a fixed seed: test only, never
                                               Quran Lab's key) and five files that are not a model. Range and
                                               If-Range are honoured, every answer has an ETag.
  GET  /_ctl/setup     what a test needs to know: the public key, the build, the files with size and SHA-256
                       and ETag, and a signed config body that names this build
  POST /_ctl/plan      {"file": "<file>", "steps": [...]}: the next requests for that file are answered step
                       by step, then normally again. A step is one of
                         {"cut": N}                    N bytes of the body, then the connection is ended
                         {"stall": N, "hold_ms": M}    N bytes, then nothing for M ms (or until the other side
                                                       goes), then the connection is ended
                         {"slow": {"bytes": B, "every_ms": E, "for_ms": F}}   B bytes every E ms for F ms,
                                                       then the rest at once
                         {"whole": true}               a Range is ignored: 200 and the whole file
                         {"status": S, "body": {...}}  that status with that JSON body (a refusal, a 503)
                         {"raw": {"status": S, "from": A, "length": N, "content_range": "bytes a-b/t",
                                  "json": bool, "chunked": bool}}   an answer spelt out: status S with N bytes
                                                       of the file from offset A (filled up when the file
                                                       ends), or a small JSON body; that Content-Range or
                                                       none; chunked: without a Content-Length
                         {"status": 302, "location": "/moved/..."}   a redirect (302, 307) to that path of this
                                                       server, or to a full URL (http://localhost:<port>/moved/...:
                                                       another host by name), where the request would be
                                                       served too: a request that follows the redirect is
                                                       recorded there
  GET  /_ctl/stats     {"connections": n, "requests": [{"file", "method", "path", "headers", "body", "localhost",
                       "range", "if_range", "key", "token", "status", "sent", "conn", "at_ms"}]}: the requests since
                       the last reset, in order of arrival, and how many connections carried them. "headers"
                       is the NAMES of the request's headers, lower case and sorted: no value of a header is
                       ever kept. "body" is the size of the request's body in bytes; "localhost" says
                       whether the request was made to the name localhost
  GET, POST /v1/...    a stand-in for the API, for a Client's calls: /v1/attest answers a token, any other
                       path {"ok": true}; a plan for "file": "/v1/<path>" (no query) answers that path
                       step by step. Recorded with "file": "api"
  PUT  /put/...        a stand-in for the storage a recording's segment is put to: answers ok; a plan for
                       "file": "/put/<path>" answers it step by step. Recorded with "file": "put"
  POST /_ctl/reset     forgets the plans and the counts
"""
import base64
import hashlib
import json
import os
import select
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------- Ed25519 (RFC 8032), signing only
P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P


def _add(p, q):
    a = (p[1] - p[0]) * (q[1] - q[0]) % P
    b = (p[1] + p[0]) * (q[1] + q[0]) % P
    c = 2 * p[3] * q[3] * D % P
    d = 2 * p[2] * q[2] % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s, p):
    q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _base():
    y = 4 * pow(5, P - 2, P) % P
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P) % P
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * pow(2, (P - 1) // 4, P) % P
    if x & 1:
        x = P - x
    return (x, y, 1, x * y % P)


G = _base()


def _compress(p):
    zi = pow(p[2], P - 2, P)
    x, y = p[0] * zi % P, p[1] * zi % P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _secret(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def ed_public(seed):
    return _compress(_mul(_secret(seed)[0], G))


def ed_sign(seed, msg):
    a, prefix = _secret(seed)
    pub = _compress(_mul(a, G))
    r = int.from_bytes(hashlib.sha512(prefix + msg).digest(), "little") % L
    big_r = _compress(_mul(r, G))
    h = int.from_bytes(hashlib.sha512(big_r + pub + msg).digest(), "little") % L
    return big_r + ((r + h * a) % L).to_bytes(32, "little")


def ed_self_check():
    """RFC 8032, section 7.1, test 1: the signer above is the standard's."""
    seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
    assert ed_public(seed).hex() == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
    assert ed_sign(seed, b"").hex() == ("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                                         "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")


# ---------------------------------------------------------------- the stand-in model
SEED = bytes.fromhex("5a17c0de0badf00d1122334455667788990aabbccddeeff00112233445566778")  # throwaway test key
REPO = "Quran-Lab/tajweedformer-v4"
FOLDER = "sdk/v4.0"
VERSION = "4.0.0"
BIG = "big.bin"


def filler(name, size):
    """`size` bytes that depend on the name only."""
    out = bytearray()
    n = 0
    while len(out) < size:
        out += hashlib.sha256(f"{name}:{n}".encode()).digest()
        n += 1
    return bytes(out[:size])


def make_content():
    files = {
        "tokens.txt": b"<blk>\n|\na\nb\n",
        "a.bin": filler("a", 3000),
        "b.bin": filler("b", 5000),
        "c.bin": filler("c", 7000),
        BIG: filler("big", 400000),
    }
    roles = {"tokens.txt": "tokens", "a.bin": "weights", "b.bin": "frontend", "c.bin": "frames", BIG: "graph-data"}
    entries = []
    for path, data in files.items():
        entries.append({"path": path, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "platforms": "*",
                        "soc": "*", "context": "*", "role": roles[path], "need": "required", "encoding": "identity"})
    m = {"format": 3, "model_id": "v4.0", "family": "tajweedformer", "revision": "test", "version": VERSION,
         "min_sdk_version": "3.0.0", "engine_format": 3, "coreml_format": 9, "tokens": "tokens.txt",
         "tokens_sha256": hashlib.sha256(files["tokens.txt"]).hexdigest(), "default_context": "c16_l64",
         "contexts": ["c16_l64"], "files": entries}
    # the signed text of a format 3 manifest, as test/make_test_folder.py writes it and the library rebuilds it
    nl = chr(10)
    text = "quranlab-model-manifest 3" + nl
    for k in ["model_id", "family", "revision", "min_sdk_version", "engine_format", "coreml_format", "tokens",
              "tokens_sha256", "default_context"]:
        text += f"{k} {m[k]}" + nl
        if k == "revision":
            text += f"version {m['version']}" + nl
    text += "contexts " + ",".join(m["contexts"]) + nl
    for f in sorted(entries, key=lambda f: f["path"].encode()):
        text += (f"file {f['sha256']} {f['size']} {f['path']} * {f['soc']} {f['context']} {f['role']} {f['need']} "
                 f"identity - - -") + nl
    m["signature"] = base64.b64encode(ed_sign(SEED, text.encode())).decode()
    manifest = json.dumps(m, indent=1).encode()
    files["manifest.json"] = manifest
    build = hashlib.sha256(manifest).hexdigest()[:40]
    pub = ed_public(SEED)
    # a config as the server signs it (test/make_model_configs.py), naming this build
    entry = {"revision": build, "version": VERSION, "source": "stable", "model_id": "v4.0", "folder": FOLDER}
    cfg = {"v": 1, "app": "linux:org.example.Net", "channel": "stable", "issued_at": 1794355200, "expires_at": 4102444800,
           "models": {REPO: entry}, "recommended": dict(entry, model=REPO), "denylist": [],
           "flags": {"analytics": True, "issues": True, "contributions": False, "min_sdk": "3.0.0"},
           "sdk_latest": {"version": "3.0.0", "severity": "optional", "url": "https://github.com/Quran-Lab/tajweedformer-c/releases"}}
    body = json.dumps(cfg, separators=(",", ":"))
    setup = {"public_key_hex": pub.hex(), "key_id": hashlib.sha256(pub).hexdigest()[:16], "repo": REPO, "folder": FOLDER,
             "build": build, "version": VERSION, "big": BIG, "config_body": body,
             "config_signature": "ed25519=" + base64.b64encode(ed_sign(SEED, body.encode())).decode(),
             "files": [{"path": e["path"], "size": e["size"], "sha256": e["sha256"], "etag": etag_of(files[e["path"]])}
                       for e in entries]}
    return files, setup


def etag_of(data):
    return '"' + hashlib.sha256(data).hexdigest()[:16] + '"'


# ---------------------------------------------------------------- the server
class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.files, self.setup = make_content()
        self.t0 = time.monotonic()
        self.conn_ids = 0  # never reset: an id names one connection for the server's whole life
        self.reset()

    def reset(self):
        with self.lock:
            self.plans = {}
            self.requests = []

    def next_step(self, name):
        with self.lock:
            steps = self.plans.get(name)
            return steps.pop(0) if steps else {}


S = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = 60

    def log_message(self, fmt, *args):
        pass

    def answer(self, status, body, headers=None):
        self.send_response(status)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def answer_json(self, status, obj):
        self.answer(status, json.dumps(obj).encode(), {"Content-Type": "application/json"})

    def record(self, name, body=0):
        """One request in the stats: its path, the NAMES of its headers (never a header's value), the size of
        its body."""
        with S.lock:
            if not getattr(self, "counted", 0):
                S.conn_ids += 1
                self.counted = S.conn_ids
            rec = {"file": name, "method": self.command, "path": self.path,
                   "headers": sorted({k.lower() for k in self.headers.keys()}), "body": body,
                   "localhost": (self.headers.get("Host") or "").lower().startswith("localhost"),
                   "range": self.headers.get("Range"), "if_range": self.headers.get("If-Range"),
                   "key": self.headers.get("x-qlab-key") is not None,
                   "token": (self.headers.get("Authorization") or "").startswith("Bearer "), "status": 0, "sent": 0,
                   "conn": self.counted, "at_ms": int((time.monotonic() - S.t0) * 1000)}
            S.requests.append(rec)
        return rec

    def planned(self, step, rec):
        """Answers a step that names a status (a refusal, a 503, a redirect); True when it did."""
        if "status" not in step:
            return False
        rec["status"] = step["status"]
        if "location" in step:
            where = step["location"]
            if not where.startswith("http"):
                where = "http://127.0.0.1:%d%s" % (self.server.server_address[1], where)
            self.answer(step["status"], b"", {"Location": where})
        else:
            self.answer_json(step["status"], step.get("body") or {"error": "unavailable"})
        return True

    def other(self, kind, body):
        """The stand-in API (kind "api": a token from /v1/attest, ok from any other path) and the stand-in
        storage (kind "put"). A path under /moved/ is where a followed redirect arrives: it is answered ok."""
        rec = self.record(kind, body)
        path = self.path.split("?")[0]
        if self.planned(S.next_step(path), rec):
            return
        rec["status"] = 200
        if path == "/v1/attest":
            return self.answer_json(200, {"token": "qlt_v1.standin.sig", "expires_in": 86400, "attested": False})
        self.answer_json(200, {"ok": True})

    def kind(self):
        """"api" or "put" for a path of the stand-in API or storage (also under /moved/), else None."""
        path = self.path[6:] if self.path.startswith("/moved/") else self.path
        return "api" if path.startswith("/v1/") else "put" if path.startswith("/put/") else None

    def raw(self, raw, data, rec):
        """An answer the test spells out, whatever the request asked for."""
        rec["status"] = raw["status"]
        if raw.get("json"):
            body = json.dumps({"error": "unavailable", "message": "this is not the file"}).encode()
        else:
            start, n = raw.get("from", 0), raw.get("length", 0)
            body = data[start:start + n]
            body += b"x" * (n - len(body))
        self.send_response(raw["status"])
        self.send_header("ETag", etag_of(data))
        if raw.get("content_range"):
            self.send_header("Content-Range", raw["content_range"])
        try:
            if raw.get("chunked"):
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                for at in range(0, len(body), 16384):
                    piece = body[at:at + 16384]
                    self.wfile.write(b"%x\r\n" % len(piece) + piece + b"\r\n")
                    rec["sent"] += len(piece)
                self.wfile.write(b"0\r\n\r\n")
            else:
                if raw["status"] != 204:
                    self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                rec["sent"] += len(body)
        except OSError:
            self.close_connection = True  # the other side refused the answer and went

    def do_PUT(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(n)
        if self.kind():
            return self.other(self.kind(), n)
        self.answer_json(404, {"error": "not_found"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        if self.path == "/_ctl/reset":
            S.reset()
            return self.answer_json(200, {"ok": True})
        if self.path == "/_ctl/plan":
            plan = json.loads(body.decode())
            with S.lock:
                S.plans[plan["file"]] = list(plan["steps"])
            return self.answer_json(200, {"ok": True})
        if self.kind():
            return self.other(self.kind(), n)
        self.answer_json(404, {"error": "not_found"})

    def do_GET(self):
        if self.path == "/_ctl/setup":
            return self.answer_json(200, S.setup)
        if self.path == "/_ctl/stats":
            with S.lock:
                return self.answer_json(200, {"connections": len({q["conn"] for q in S.requests}), "requests": S.requests})
        if self.kind():
            return self.other(self.kind(), 0)
        at = self.path.find("/" + FOLDER + "/")
        name = self.path[at + len(FOLDER) + 2:] if at >= 0 else None
        data = S.files.get(name) if name else None
        if data is None:
            return self.answer_json(404, {"error": "not_found"})
        rec = self.record(name)
        step = S.next_step(name)
        if self.planned(step, rec):
            return
        if "raw" in step:
            return self.raw(step["raw"], data, rec)
        etag = etag_of(data)
        start = 0
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes=") and rng.endswith("-") and not step.get("whole"):
            # If-Range: the range only when the validator is this file's; else the whole file with 200
            cond = self.headers.get("If-Range")
            if cond is None or cond == etag:
                start = int(rng[6:-1])
                if start >= len(data):
                    rec["status"] = 416
                    return self.answer(416, b"", {"Content-Range": f"bytes */{len(data)}"})
        ranged = rng is not None and not step.get("whole") and (start > 0 or self.headers.get("If-Range") in (None, etag))
        body = data[start:]
        status = 206 if ranged else 200
        rec["status"] = status
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", etag)
        if ranged:
            self.send_header("Content-Range", f"bytes {start}-{len(data) - 1}/{len(data)}")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.send_body(step, body, rec)
        except OSError:
            self.close_connection = True  # the other side went

    def put(self, chunk, rec):
        self.wfile.write(chunk)
        rec["sent"] += len(chunk)

    def send_body(self, step, body, rec):
        if "cut" in step:
            self.put(body[:step["cut"]], rec)
            self.end_now()
        elif "stall" in step:
            self.put(body[:step["stall"]], rec)
            until = time.monotonic() + step.get("hold_ms", 6000) / 1000.0
            while time.monotonic() < until:
                ready, _, _ = select.select([self.connection], [], [], 0.05)
                if ready:
                    break  # readable while an answer is on its way: the other side closed
            self.end_now()
        elif "slow" in step:
            slow = step["slow"]
            until = time.monotonic() + slow["for_ms"] / 1000.0
            at = 0
            while at < len(body) and time.monotonic() < until:
                self.put(body[at:at + slow["bytes"]], rec)
                at += slow["bytes"]
                time.sleep(slow["every_ms"] / 1000.0)
            self.put(body[at:], rec)
        else:
            self.put(body, rec)

    def end_now(self):
        # the sending side is closed: what was written arrives, then the other side sees the end before the
        # length the headers named
        self.close_connection = True
        try:
            self.connection.shutdown(socket.SHUT_WR)
        except OSError:
            pass


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def handle_error(self, request, client_address):
        pass  # a connection the test ended on purpose


def start(port=0):
    global S
    ed_self_check()
    S = State()
    srv = Server(("127.0.0.1", port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main(argv):
    if argv and argv[0] == "--serve":
        srv = start(int(argv[1]) if len(argv) > 1 else 0)
        print(f"http://127.0.0.1:{srv.server_address[1]}", flush=True)
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            return 0
    if len(argv) != 2:
        print(__doc__)
        return 2
    srv = start()
    url = f"http://127.0.0.1:{srv.server_address[1]}"
    env = dict(os.environ)
    for k in ("http_proxy", "HTTP_PROXY", "all_proxy", "ALL_PROXY"):
        env.pop(k, None)
    env["NO_PROXY"] = env["no_proxy"] = "127.0.0.1"
    try:
        code = subprocess.run([argv[0], "net", url, argv[1]], env=env, timeout=240).returncode
    except subprocess.TimeoutExpired:
        print("FAIL the tests did not end in 240 s", file=sys.stderr)
        code = 124
    srv.shutdown()
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
