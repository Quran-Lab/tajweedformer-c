"""The leases and signed stops tf40_tests grace_vectors replays grace.json's sequences with (PROTECTED_MODEL.md
18.4.7.1, contract 1c971ab).

  python test/make_grace40.py --contract <quranlab-contract sdk_4.0.0 folder> --out <folder>

grace.json's sequences name each lease by its iat and exp only (the Core's own run_vectors makes the bytes). This
makes them for lease.json's ECDH install with the contract's TEST keys (tools/protected40.py: the test model root, the
test Core root of release 4.0.0-c1, the test signer): lease.json's build, key id, model, version, mark and install,
flags 0, refresh_after at iat + 43200 (or exp, if sooner); and, for a sequence's `stop`, a stop signed for that install
and the challenge `challenge` at the Core's time of the check before it. Writes <out>/grace_seq.json: {challenge, key,
sequences: grace.json's, each lease step with lease_hex, each stop step with refusal (the body)}. No network, nothing
any Quran Lab server trusts.
"""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
import sys


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--contract", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    spec = importlib.util.spec_from_file_location("protected40", os.path.join(a.contract, "tools", "protected40.py"))
    p40 = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(p40)
    from cryptography.hazmat.primitives import serialization
    lj = json.load(open(os.path.join(a.contract, "vectors", "lease.json"), encoding="utf-8"))
    gj = json.load(open(os.path.join(a.contract, "vectors", "grace.json"), encoding="utf-8"))
    if p40.raw_pub(p40.SIGNER).hex() != lj["signer"]["public_hex"]:
        raise SystemExit("protected40's test signer is not lease.json's")
    s8 = lj["install_ecdh"]["pkcs8_b64u"]
    install = serialization.load_der_private_key(base64.urlsafe_b64decode(s8 + "=" * (-len(s8) % 4)), password=None)
    ih = bytes.fromhex(lj["install_ecdh"]["install_hash_hex"])
    f0 = lj["accept"][0]["fields"]
    key, challenge = "apps-one", "c1.1790000300.AAAAAAAAAAAAAAAAAAAAAA.BBBBBBBBBBBBBBBBBBBBBB"
    bk, kcore = bytes.fromhex(lj["build_key_hex"]), bytes.fromhex(lj["k_core_hex"])
    out = {"challenge": challenge, "key": key, "sequences": []}
    for n, s in enumerate(gj["sequences"]):
        steps, last_now = [], None
        for i, st in enumerate(s["steps"]):
            st = dict(st)
            if "lease" in st:
                iat, exp = st["lease"]["iat"], st["lease"]["exp"]
                lid = hashlib.sha256(f"quranlab-grace-seq {n} {i}".encode()).digest()[:16]
                f = {"flags": 0, "lease_id": lid, "iat": iat, "exp": exp, "refresh_after": min(iat + 43200, exp),
                     "key_id": bytes.fromhex(lj["key_id_hex"]), "build": lj["build"], "install_hash": ih,
                     "mark": bytes.fromhex(f0["mark_hex"]), "model": lj["model"], "version": lj["version"], "release": lj["release"]}
                eph = p40.ec.derive_private_key(int.from_bytes(hashlib.sha256(lid).digest(), "big") % (2 ** 255) + 1, p40.ec.SECP256R1())
                made = p40.make_lease(f, bk=bk, kcore=kcore, signer=p40.SIGNER, wrap=1, install_pub=install.public_key(), eph_priv=eph)
                st["lease_hex"] = made["lease"].hex()
                last_now = st.get("anchor", iat)
            if "check" in st:
                last_now = st["now"]
            if st.get("stop"):
                st["refusal"] = {"error": "invalid_key", "message": "missing or invalid developer key", "reason": "key_revoked",
                                 "signed": p40.sign_refusal(p40.SIGNER, key, "key_revoked", ih.hex(), challenge, last_now)}
            steps.append(st)
        out["sequences"].append({"name": s["name"], "steps": steps})
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "grace_seq.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(out, fh, indent=1)
    print(json.dumps({"sequences": len(out["sequences"]), "out": os.path.join(a.out, "grace_seq.json")}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
