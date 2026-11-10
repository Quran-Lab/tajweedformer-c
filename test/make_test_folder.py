#!/usr/bin/env python3
"""Make a model folder for the tests: files linked (or copied) in, a format 2 manifest, optionally signed.

    make_test_folder.py OUT [--sign SEED_HEX] [--fix-tokens-sha] SRC=DEST ...

SRC=DEST puts SRC at OUT/DEST (a hard link when possible, else a copy). --fix-tokens-sha rewrites
calibration.json's tokens_sha256 to tokens.txt's (the settings fixture carries a placeholder). With --sign the
manifest is signed with the Ed25519 key of that 32-byte seed (a throwaway test key; never Quran Lab's), exactly
as tajweedformer-coreml's make_manifest.py signs: the text below. Prints the public key (hex).
"""
import hashlib
import json
import os
import shutil
import sys

F2 = ["model_id", "family", "revision", "min_sdk_version", "engine_format", "coreml_format", "tokens_sha256", "coreml",
      "sidecar", "keeps", "tokens"]


def signed_text(m):
    t = "quranlab-model-manifest 2\n" + "".join(f"{k} {m[k]}\n" for k in F2)
    for f in sorted(m["files"], key=lambda f: f["path"].encode()):
        t += f"file {f['sha256'].lower()} {f['size']} {f['path']}\n"
    return t


ROLES = {"tokens.txt": "tokens", "calibration.json": "calibration", "expected_tj3.zfx": "expected", "layers.zfw": "weights",
         "frontend.zfw": "frontend", "model_engine2.onnx.zfg": "graph", "model_engine2.onnx.zfg.bin": "graph-data",
         "frames.zfw": "frames", "model_engine2.onnx": "ort-graph"}
F3 = ["model_id", "family", "revision", "min_sdk_version", "engine_format", "coreml_format", "tokens", "tokens_sha256",
      "default_context"]


def signed_text3(m):
    nl = chr(10)
    t = "quranlab-model-manifest 3" + nl + "".join(f"{k} {m[k]}" + nl for k in F3) + "contexts " + ",".join(m["contexts"]) + nl
    # golden_tolerance (signed since SDK 2.0.1): one line per build, sorted by the build key's UTF-8 bytes; none when absent
    gt = m.get("golden_tolerance") or {}
    for b in sorted(gt, key=lambda k: k.encode()):
        t += f"golden_tolerance {b} {gt[b]['token_diffs']} {gt[b]['site_flag_diffs']} {gt[b]['word_flag_diffs']}" + nl
    order = ["android", "apple", "web", "native"]
    for f in sorted(m["files"], key=lambda f: f["path"].encode()):
        pl = "*" if "*" in f["platforms"] else ",".join(x for x in order if x in f["platforms"])
        d = f.get("decoded")
        enc = f.get("encoding", "identity")
        tail = f"{d['sha256']} {d['size']} {d['path']}" if enc != "identity" and d else "- - -"
        t += f"file {f['sha256'].lower()} {f['size']} {f['path']} {pl} {f['soc']} {f['context']} {f['role']} {f['need']} {enc} {tail}" + nl
    return t


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main(argv):
    out, seed, fix, fmt3, gz = argv[0], None, False, False, False
    pairs = []
    i = 1
    while i < len(argv):
        if argv[i] == "--sign":
            seed = argv[i + 1]
            i += 2
            continue
        if argv[i] == "--gzip":
            gz = True
            i += 1
            continue
        if argv[i] == "--format3":
            fmt3 = True
            i += 1
            continue
        if argv[i] == "--fix-tokens-sha":
            fix = True
            i += 1
            continue
        src, dst = argv[i].split("=", 1)
        pairs.append((src, dst))
        i += 1
    os.makedirs(out, exist_ok=True)
    for src, dst in pairs:
        full = os.path.join(out, dst)
        os.makedirs(os.path.dirname(full) or out, exist_ok=True)
        if os.path.exists(full):
            os.remove(full)
        if dst == "calibration.json" and fix:
            continue
        try:
            os.link(src, full)
        except OSError:
            shutil.copyfile(src, full)
    tok = os.path.join(out, "tokens.txt")
    if fix:
        src = dict((d, s) for s, d in pairs)["calibration.json"]
        cal = json.load(open(src, encoding="utf-8"))
        cal["tokens_sha256"] = sha(tok)
        with open(os.path.join(out, "calibration.json"), "w", encoding="utf-8") as f:
            json.dump(cal, f, ensure_ascii=False)
    cal = json.load(open(os.path.join(out, "calibration.json"), encoding="utf-8"))
    files = []
    for _, dst in pairs:
        p = os.path.join(out, dst)
        files.append({"path": dst, "size": os.path.getsize(p), "sha256": sha(p)})
    ctx = cal["default_context"]
    m = {"format": 2, "model_id": cal["model_id"], "family": "tajweedformer", "revision": "test", "min_sdk_version": "2.0.0",
         "engine_format": 2, "coreml_format": 9, "tokens_sha256": sha(tok), "coreml": "-",
         "sidecar": f"{ctx}/layers.zfw" if any(d == f"{ctx}/layers.zfw" for _, d in pairs) else "-", "keeps": "-",
         "tokens": "tokens.txt", "files": files}
    tok_sha = m["tokens_sha256"]
    if fmt3:
        for f in files:
            name = f["path"].rsplit("/", 1)[-1]
            # shared files: the string "*", exactly as publish/make_manifest3.py writes them (the downloader once
            # accepted only a list and skipped every shared file of a real published folder, 11-08)
            f.update({"platforms": ["native"] if name == "model_engine2.onnx" else "*", "soc": "*",
                      "context": f["path"].split("/")[0] if "/" in f["path"] else "*", "role": ROLES.get(name, "other"),
                      "need": "optional" if name == "model_engine2.onnx" else "required", "encoding": "identity"})
        if gz:
            import gzip
            for f in files:
                src = os.path.join(out, f["path"])
                data = open(src, "rb").read()
                packed = gzip.compress(data, compresslevel=9, mtime=0)
                open(src + ".gz", "wb").write(packed)
                os.remove(src)
                f["decoded"] = {"path": f["path"], "size": f["size"], "sha256": f["sha256"]}
                f.update({"path": f["path"] + ".gz", "size": len(packed), "sha256": hashlib.sha256(packed).hexdigest(), "encoding": "gzip"})
        m = {"format": 3, "model_id": cal["model_id"], "family": "tajweedformer", "revision": "test", "min_sdk_version": "2.0.0",
             "engine_format": 3, "coreml_format": 9, "tokens": "tokens.txt", "tokens_sha256": tok_sha,
             "default_context": cal["default_context"], "contexts": list(cal["contexts"].keys()), "files": files}
        signed_text_fn = signed_text3
    else:
        signed_text_fn = signed_text
    if seed:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        from cryptography.hazmat.primitives import serialization
        import base64
        k = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(seed))
        m["signature"] = base64.b64encode(k.sign(signed_text_fn(m).encode())).decode()
        pub = k.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()
        print(pub)
    with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(m, f, indent=1)


if __name__ == "__main__":
    main(sys.argv[1:])
