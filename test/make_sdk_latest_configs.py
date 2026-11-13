#!/usr/bin/env python3
"""Signed /v1/config bodies with and without the optional top-level `sdk_latest` field, for tf_tests sdk_latest.

    python test/make_sdk_latest_configs.py test/data/sdk_latest_configs.json

Each body is signed the way the server signs a config (Ed25519 over the exact body bytes, header
`x-qlab-signature: ed25519=<base64>`, `x-qlab-key-id` = the first 16 hex digits of SHA-256 of the public key) with
a THROWAWAY key derived from a fixed seed: test only, never Quran Lab's key. The test serves these bodies through
its HTTP stub, so they go through the SDK's real verification and parsing: an added top-level field must neither
break the signature check (it covers the raw bytes) nor be refused by the parser.

`sdk_latest` is {"version", "severity", "url"} for the app's platform. The SDK logs one warning per client in debug
builds when the version is newer than its own; equal, older, junk versions and malformed objects are ignored.
Tried and rejected: building the bodies inside the C test (the SDK has Ed25519 verification only, and a signing
implementation in the test would sign with the same code that verifies).
"""
import base64, hashlib, json, sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

SEED = bytes.fromhex("5ad7a7e57c0f1e9a0b1c2d3e4f5061728394a5b6c7d8e9f00112233445566778")  # throwaway test key
EXPIRES = 4102444800  # 2100-01-01: never expires during a test run
URL = "https://github.com/Quran-Lab/tajweedformer-c/releases"

CASES = [
    ("newer", {"version": "3.1.3", "severity": "recommended", "url": URL}),
    ("newer major", {"version": "4.0", "severity": "required", "url": URL}),
    ("equal", {"version": "3.1.2", "severity": "recommended", "url": URL}),
    ("older", {"version": "2.0.3", "severity": "recommended", "url": URL}),
    ("absent", None),
    ("junk version", {"version": "3.0.x", "severity": "recommended", "url": URL}),
    ("prerelease version", {"version": "3.1.3-beta", "severity": "recommended", "url": URL}),
    ("empty version", {"version": "", "severity": "recommended", "url": URL}),
    ("version a number", {"version": 4, "severity": "recommended", "url": URL}),
    ("no severity", {"version": "3.1.3", "url": URL}),
    ("no url", {"version": "3.1.3", "severity": "recommended"}),
    ("url not a string", {"version": "3.1.3", "severity": "recommended", "url": ["x"]}),
    ("not an object", "3.1.3"),
    ("null", "__null__"),
]


def body(sdk_latest):
    d = {"v": 1, "app": "linux:org.example.Latest", "issued_at": 1794182400, "expires_at": EXPIRES,
         "flags": {"analytics": True, "contributions": False}, "inference": {"context": "c16_l64"}}
    if sdk_latest == "__null__":
        d["sdk_latest"] = None
    elif sdk_latest is not None:
        d["sdk_latest"] = sdk_latest
    return json.dumps(d, separators=(",", ":"))


def main():
    key = Ed25519PrivateKey.from_private_bytes(SEED)
    pub = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    out = {"about": "signed configs with the optional sdk_latest field (make_sdk_latest_configs.py); throwaway key, test only",
           "public_key_hex": pub.hex(), "key_id": hashlib.sha256(pub).hexdigest()[:16], "configs": []}
    for name, latest in CASES:
        b = body(latest)
        sig = base64.b64encode(key.sign(b.encode("utf-8"))).decode("ascii")
        out["configs"].append({"name": name, "body": b, "signature": "ed25519=" + sig})
    open(sys.argv[1], "w", encoding="utf-8", newline="\n").write(json.dumps(out, indent=1) + "\n")


if __name__ == "__main__":
    main()
