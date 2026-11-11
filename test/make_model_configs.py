#!/usr/bin/env python3
"""Signed /v1/config bodies as the 3.0 server sends them, for tf_tests api and tf_tests model30.

    python test/make_model_configs.py test/data/model_configs.json [path/to/local30.json [variant]]

Each body is signed the way the server signs a config (Ed25519 over the exact body bytes, header
`x-qlab-signature: ed25519=<base64>`, `x-qlab-key-id` = the first 16 hex digits of SHA-256 of the public key) with
a THROWAWAY key derived from a fixed seed: test only, never Quran Lab's key. The tests serve these bodies through
their HTTP stub, so they go through the SDK's real verification and parsing.

The cases:
  no model         "models": {} and "recommended": null: what a 3.x install gets while Quran Lab has no build for
                   SDK 3 (the error model_unavailable)
  model            a recommended build (its 40 hex build id as "revision", "version" 4.0.0)
  model c24        the same with "inference": {"context": "c24_l72"} (the portal's Response time setting)
  model unknown context   the same with a context the build does not have (ignored)
  model pin unavailable   the same with "pin": {"version": "9.9.9", "status": "unavailable"}
  model other build       the same version with another build id (the manifest served is not that build)
  model other version     the right build named with version 4.1.0 (the signed manifest says 4.0.0)

The build id is the `full` variant's of the local 3.0 model folders (local30.json, when its path is given), else
the one recorded below; tf_tests model30 skips its download tests when the folder on this machine is another
build.
"""
import base64
import hashlib
import json
import sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

SEED = bytes.fromhex("3c1f0a9b8d7e6f50413223140516f7e8d9cabbac9d8e7f60514233241506f7e8")  # throwaway test key
EXPIRES = 4102444800  # 2100-01-01: never expires during a test run
BUILD = "a4287e7b9c80057dec5df9c1897462ddd7faa326"
OTHER = "0123456789abcdef0123456789abcdef01234567"
REPO = "Quran-Lab/tajweedformer-v4"


def body(build, version, extra):
    d = {"v": 1, "app": "linux:org.example.Api", "channel": "stable", "issued_at": 1794355200, "expires_at": EXPIRES,
         "models": {}, "recommended": None, "denylist": [],
         "flags": {"analytics": True, "issues": True, "contributions": False, "min_sdk": "3.0.0"},
         "sdk_latest": {"version": "3.0.0", "severity": "optional", "url": "https://github.com/Quran-Lab/tajweedformer-c/releases"}}
    if build:
        entry = {"revision": build, "version": version, "source": "stable", "model_id": "v4.0", "folder": "sdk/v4.0"}
        d["models"] = {REPO: entry}
        d["recommended"] = dict(entry, model=REPO)
    d.update(extra)
    return json.dumps(d, separators=(",", ":"))


def main():
    build = BUILD
    if len(sys.argv) > 2:
        build = json.load(open(sys.argv[2], encoding="utf-8"))["variants"][sys.argv[3] if len(sys.argv) > 3 else "full"]["build"]
    cases = [
        ("no model", None, None, {}),
        ("model", build, "4.0.0", {}),
        ("model c24", build, "4.0.0", {"inference": {"context": "c24_l72"}}),
        ("model unknown context", build, "4.0.0", {"inference": {"context": "c99_l1"}}),
        ("model pin unavailable", build, "4.0.0", {"pin": {"version": "9.9.9", "status": "unavailable"}}),
        ("model other build", OTHER, "4.0.0", {}),
        ("model other version", build, "4.1.0", {}),
    ]
    key = Ed25519PrivateKey.from_private_bytes(SEED)
    pub = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    out = {"about": "signed 3.0 configs (make_model_configs.py); throwaway key, test only", "public_key_hex": pub.hex(),
           "key_id": hashlib.sha256(pub).hexdigest()[:16], "build": build, "configs": []}
    for name, b, version, extra in cases:
        text = body(b, version, extra)
        sig = base64.b64encode(key.sign(text.encode("utf-8"))).decode("ascii")
        out["configs"].append({"name": name, "body": text, "signature": "ed25519=" + sig})
    open(sys.argv[1], "w", encoding="utf-8", newline="\n").write(json.dumps(out, indent=1) + "\n")


if __name__ == "__main__":
    main()
