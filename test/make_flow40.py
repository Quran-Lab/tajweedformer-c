"""The stand-in server's data for the 4.0 flow tests and the conformance run (test/tf40_tests.c `flow`, `conformance`).

  python test/make_flow40.py --contract <quranlab-contract sdk_4.0.0 folder> --out <folder> [--testbuild <folder>]

Everything is made with the contract's TEST keys (sdk_4.0.0/tools/protected40.py: the test model root, the test Core
root of release 4.0.0-c1, the test signing key); no Quran Lab server trusts any of it, and nothing here talks to a
network. Writes <out>/flow.json:

  build, manifest (its exact bytes), files {path: container or plain file, hex} (vector build only),
  config_body / config_sig / config_kid (a signed config naming the build and its key id, as qlab-api's sdk4-server
  signs one for SDK 4.0: recommended.key_id, no base), config_off_body / config_off_sig (the same with every analytics
  group switched off, flags.metrics), config_nokid_* (the build without a key id), config_badkid_* (another key id),
  cert (the install certificate of the test install key), install_pkcs8 (that key, base64url), lease {lease, iat,
  exp, refresh_after, base} (a lease for the build, wrapped for the test key, release 4.0.0-c1; its base is a model
  folder apart from the host's default URL, so the tests see where each file was read), lease_later (the same
  install's lease issued 30 hours after it), challenge, refusal_signed (a
  signed stop for that challenge and key at the lease's refresh time + 60 s), refusal_unsigned, pieces {piece7,
  piece8, piece_bad} (the contract's piece.json: pieces 7 and 8 and the one with a changed signature byte, base64url),
  order_pieces {piece7, piece8, piece9, piece_bad_sig} (the pieces piece_order.json names, as quranlab-core's
  tools/make_test_pieces.py makes them),
  plain_marker (hex of 32 plaintext bytes of the weights: the tests search every file of the SDK's folder for them),
  plain_markers (--testbuild: {path: hex of 32 plaintext bytes} of each encrypted model file, from the test build's
  plain folder; `eval` searches its whole work folder for them).

Two builds:
  - default: the contract's synthetic vector build (protected40.py manifest_files: the manifest_40.json build, small
    random files, a three-token vocabulary). No model data at all: the flow runs to the Core's install, and the
    Model is refused at open for its vocabulary, as it must be.
  - --testbuild: a folder made by quranlab-core tools/make_test_build.py from the local V4 build (the conformance run).
    Its containers are served from that folder (never copied); its install key (install_key.der) is the one the
    lease is wrapped for. Never point it at a folder inside a repository.

Tried and rejected: the lease of make_test_build.py itself (its build is a fixed label, not the manifest's hash, and an
SDK holds a lease to the build the signed config named and the manifest's hash proves); the contract's lease.json (its
build is a random label for the same reason).
"""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
import sys
import time


def load_p40(contract):
    spec = importlib.util.spec_from_file_location("protected40", os.path.join(contract, "tools", "protected40.py"))
    p40 = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(p40)
    return p40


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--contract", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--testbuild")
    a = ap.parse_args(argv)
    p40 = load_p40(a.contract)
    from cryptography.hazmat.primitives import serialization
    os.makedirs(a.out, exist_ok=True)
    out = {}
    if a.testbuild:
        man_path = os.path.join(a.testbuild, "cdn", "sdk", "v4.0", "manifest.json")
        mbytes = open(man_path, "rb").read()
        der = open(os.path.join(a.testbuild, "install_key.der"), "rb").read()
        install = serialization.load_der_private_key(der, password=None)
        out["cdn_dir"] = os.path.join(a.testbuild, "cdn", "sdk", "v4.0")
        marker = None
        # 32 plaintext bytes of each model file the build encrypts (make_test_build.py keeps the plaintext folder
        # beside the containers): the eval test searches its whole work folder for every one of them
        plain = os.path.join(a.testbuild, "plain", "sdk", "v4.0")
        markers = {}
        for e in json.loads(mbytes)["files"]:
            p = os.path.join(plain, e["decoded"]["path"]) if e.get("encoding") == "qle1" else None
            if not p or not os.path.isfile(p) or os.path.getsize(p) < 4096:
                continue
            b = open(p, "rb").read()
            # a run of 32 bytes with at least 16 distinct values from the middle on (never a header or padding)
            at = next((i for i in range(len(b) // 2, len(b) - 32, 32) if len(set(b[i:i + 32])) >= 16), None)
            if at is not None:
                markers[e["decoded"]["path"]] = b[at:at + 32].hex()
        out["plain_markers"] = markers
    else:
        files = p40.manifest_files()
        tokens = next(b for e, _, b in files if e["role"] == "tokens")
        m = {"format": 3, "model_id": p40.MODEL_ID, "family": "tajweedformer", "revision": "v4.1-vectors-40", "version": p40.VERSION,
             "min_sdk_version": "4.0.0", "engine_format": 3, "coreml_format": 9, "tokens": "tokens.txt",
             "tokens_sha256": hashlib.sha256(tokens).hexdigest(), "default_context": "c16_l64", "contexts": ["c16_l64"],
             "encryption": {"scheme": "qle1", "key_id": p40.KEY_ID.hex(), "root": 1},
             "files": [{k: e[k] for k in ("path", "size", "sha256", "platforms", "soc", "context", "role", "need", "encoding", "decoded")}
                       for e, _, _ in files]}
        m["signature"] = base64.b64encode(p40.SIGNER.sign(p40.manifest_text(m).encode())).decode()
        mbytes = json.dumps(m, indent=1).encode()
        out["files"] = {e["path"]: data.hex() for e, data, _ in files}
        layers = next(b for e, _, b in files if e["decoded"]["path"] == "layers.zfw")
        marker = layers[1000:1032].hex()
        install = p40.p256_key("quranlab-test-install-ecdh")
    man = json.loads(mbytes)
    build = hashlib.sha256(mbytes).hexdigest()[:40]
    key_id = bytes.fromhex(man["encryption"]["key_id"])
    spki = p40.spki(install.public_key())
    ih = p40.sha256(spki)
    now = int(time.time())
    # the signed config: the build for this app (SPEC 2a) and its key id, no base (qlab-api sdk4-server, 18.4.5),
    # analytics on
    rec = {"model": "Quran-Lab/tajweedformer-v4", "model_id": "v4.0", "folder": "sdk/v4.0", "revision": build,
           "version": man["version"], "key_id": man["encryption"]["key_id"]}
    cfg = {"v": 1, "expires_at": now + 10 * 365 * 86400, "app": "windows:com.example.recite", "recommended": rec,
           "flags": {"analytics": True, "sharing": False}}
    pub = p40.raw_pub(p40.SIGNER)
    out["signer_hex"] = pub.hex()
    out["config_kid"] = hashlib.sha256(pub).hexdigest()[:16]

    def signed(name, c):
        body = json.dumps(c, separators=(",", ":"))
        out[name + "_body"] = body
        out[name + "_sig"] = "ed25519=" + base64.b64encode(p40.SIGNER.sign(body.encode())).decode()

    signed("config", cfg)
    # the same config with every analytics group of 17.8 and 17.9 switched off (flags.metrics), signed the same way
    signed("config_off", dict(cfg, flags=dict(cfg["flags"], metrics={g: False for g in ("juz", "hours", "lengths", "learner", "repeats", "health", "judged")})))
    # sharing offered to this app (flags.contributions, SHARE_ASK_40.md 20.2 "offered"), the share_ask block absent
    signed("config_share", dict(cfg, flags=dict(cfg["flags"], contributions=True)))
    # and with the lossless form switched on (SHARING_40.md 19.3, flags.share_flac)
    signed("config_flac", dict(cfg, flags=dict(cfg["flags"], contributions=True, share_flac=True)))
    # the build without its key id (not a build for SDK 4.0), and with another key id than the manifest's
    signed("config_nokid", dict(cfg, recommended={k: v for k, v in rec.items() if k != "key_id"}))
    signed("config_badkid", dict(cfg, recommended=dict(rec, key_id=("0" * 31 + "1") if rec["key_id"] != "0" * 31 + "1" else "0" * 32)))
    # the install certificate (18.4.3): desktop, unattested, the test key does both (one software key)
    cert = {"app": "windows:com.example.recite", "platform": "windows", "key": "apps-one", "install": ih.hex(), "sign": ih.hex(),
            "att": "none", "hw": 0, "iat": now - 60, "exp": now + 10 * 365 * 86400, "recheck": 0, "batch": "-", "digest": "-"}
    out["cert"] = p40.sign_cert(p40.SIGNER, cert)
    out["install_pkcs8"] = p40.b64u(p40.pkcs8(install))
    out["install_hash"] = ih.hex()
    # the lease (18.4.6) for the build the config names: a 24-hour lease of the unattested class
    iat = now - 30
    exp = iat + 86400 - 600
    ra = iat + 43200 + 1234
    bk = p40.build_key(p40.MODEL_ROOT, key_id, man["model_id"], man["version"])
    f = {"flags": 0, "lease_id": os.urandom(16), "iat": iat, "exp": exp, "refresh_after": ra, "key_id": key_id, "build": build,
         "install_hash": ih, "mark": p40.mark(p40.k_mark(p40.MODEL_ROOT), "apps-one", ih), "model": man["model_id"],
         "version": man["version"], "release": "4.0.0-c1"}
    made = p40.make_lease(f, bk=bk, kcore=p40.k_core(p40.CORE_ROOT, "4.0.0-c1"), signer=p40.SIGNER, wrap=1,
                          install_pub=install.public_key(), eph_priv=p40.ec.generate_private_key(p40.ec.SECP256R1()))
    out["lease"] = {"v": 1, "lease": p40.b64u(made["lease"]), "iat": iat, "exp": exp, "refresh_after": ra,
                    "base": f"https://cdn.quranlab.ai/models/lease/Quran-Lab/tajweedformer-v4/{build}/sdk/v4.0/", "install_cert": out["cert"]}
    # the same install's next lease, issued 30 hours after the first (past its exp, as a server whose clock moved on with
    # the device's issues it): tf40_tests lease_clock, the staging run's clock case of 11-15
    iat2 = iat + 30 * 3600
    f2 = dict(f, lease_id=os.urandom(16), iat=iat2, exp=iat2 + 86400 - 600, refresh_after=iat2 + 43200 + 1234)
    made2 = p40.make_lease(f2, bk=bk, kcore=p40.k_core(p40.CORE_ROOT, "4.0.0-c1"), signer=p40.SIGNER, wrap=1,
                           install_pub=install.public_key(), eph_priv=p40.ec.generate_private_key(p40.ec.SECP256R1()))
    out["lease_later"] = dict(out["lease"], lease=p40.b64u(made2["lease"]), iat=iat2, exp=f2["exp"], refresh_after=f2["refresh_after"])
    # tf40_tests grace (18.4.7.1, contract ca3f9a5): a lease whose bytes the Core refuses (one byte of its signature
    # changed), and a signed stop for the fixture's challenge made 2 hours and 30 seconds past the first lease's exp
    # (the moment the grace test replays grace.json's outcomes at)
    bad = bytearray(made2["lease"])
    bad[-1] ^= 1
    out["lease_bad"] = dict(out["lease_later"], lease=p40.b64u(bytes(bad)))
    # a 7-day lease for this unattested desktop install, as the server's lease_grace switch capped_7d gives one in an
    # emergency: the SDK takes the lease's own exp, never a 24-hour cap of its class
    f7 = dict(f, lease_id=os.urandom(16), exp=iat + 604800 - 900, refresh_after=iat + 72000 + 1234)
    made7 = p40.make_lease(f7, bk=bk, kcore=p40.k_core(p40.CORE_ROOT, "4.0.0-c1"), signer=p40.SIGNER, wrap=1,
                           install_pub=install.public_key(), eph_priv=p40.ec.generate_private_key(p40.ec.SECP256R1()))
    out["lease_7d"] = dict(out["lease"], lease=p40.b64u(made7["lease"]), exp=f7["exp"], refresh_after=f7["refresh_after"])
    # the weekly data pieces (18.6.7), from the contract's vectors: signed by the test signer the test Core pins
    pv = json.load(open(os.path.join(a.contract, "vectors", "piece.json"), encoding="utf-8"))
    byid = {p["id"]: p["b64u"] for p in pv["pieces"]}
    bad = next(r for r in pv["refused"] if r["refused"] == "signature")
    out["pieces"] = {"piece7": byid[7], "piece8": byid[8], "piece_bad": p40.b64u(bytes.fromhex(bad["bytes_hex"]))}
    # the pieces piece_order.json names (quranlab-core tools/make_test_pieces.py, made the same way here)
    import struct

    def core_piece(pid, fields):
        body = bytes([1]) + struct.pack("<I", pid) + bytes([len(fields)])
        for tag, v in fields:
            body += bytes([tag]) + struct.pack("<H", len(v)) + v
        return body + hashlib.sha256(p40.raw_pub(p40.SIGNER)).digest()[:8] + p40.SIGNER.sign(b"quranlab-piece 1\n" + body)

    p7 = core_piece(7, [(1, p40.stream("quranlab-test-piece-7", 16)), (2, bytes([1, 3])), (3, bytes([64])), (9, b"later")])
    bad7 = bytearray(p7)
    bad7[-1] ^= 1
    out["order_pieces"] = {"piece7": p40.b64u(p7), "piece8": p40.b64u(core_piece(8, [(1, p40.stream("quranlab-test-piece-8", 16))])),
                           "piece9": p40.b64u(core_piece(9, [(1, p40.stream("quranlab-test-piece-9", 16))])),
                           "piece_bad_sig": p40.b64u(bytes(bad7))}
    out["build"] = build
    out["manifest"] = mbytes.decode()
    ch = "c1.1790000300.AAAAAAAAAAAAAAAAAAAAAA.BBBBBBBBBBBBBBBBBBBBBB"
    out["challenge"] = ch
    stop = {"error": "invalid_key", "message": "missing or invalid developer key", "reason": "key_revoked"}
    out["refusal_signed"] = dict(stop, signed=p40.sign_refusal(p40.SIGNER, "apps-one", "key_revoked", ih.hex(), ch, ra + 60))
    out["refusal_signed_now"] = dict(stop, signed=p40.sign_refusal(p40.SIGNER, "apps-one", "key_revoked", ih.hex(), ch, iat + 30))
    out["refusal_signed_grace"] = dict(stop, signed=p40.sign_refusal(p40.SIGNER, "apps-one", "key_revoked", ih.hex(), ch, exp + 7200 + 30))
    out["refusal_unsigned"] = stop
    out["plain_marker"] = marker
    with open(os.path.join(a.out, "flow.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(out, fh, indent=1)
    print(json.dumps({"build": build, "out": os.path.join(a.out, "flow.json")}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
