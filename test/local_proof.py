#!/usr/bin/env python3
"""The model-backed proof of the C SDK, on a machine that has a model: one bounded command.

    python test/local_proof.py <build dir> [--local30 DIR] [--clips TAR_OR_DIR] [--engine DIR] [--work DIR]
                               [--topk-takes FILE]

No public runner has a model, so this runs where the local 3.0 model folders are. It skips cleanly (exit 0, one
line saying what is missing) when they are not there.

What it runs, for each of the variants `full` and `prod` that local30.json lists (prod is shaped like the
production build: tokens-only self-test files, no tolerances):

1. The 35 conformance clips through the Session API on a folder (the tests' internal open, tf_session_create, tf_session_push,
   the chunks, tf_session_finish; `tf_tests conformance`), all twelve heads of every chunk, for every context
   the engine has audio-level expected rows for (conformance/v4_<context>_audio_expected.jsonl; the rows made
   from stored features cannot be compared with a take that starts from audio). The engine's own checker
   (bench/v4_conformance_check.py) compares tokens, chunk counts and an FNV-1a fingerprint of every output of
   every chunk: one differing bit is a mismatch.
2. Every sample clip of the model through tf_session_listen in every context, against the tokens of the folder's
   selftest/expected_<context>.json, and the rest of `tf_tests model30` (the Model's data, events on real audio,
   the reference lookup for all 6236 ayahs, the Client's download path against a stub CDN).

3. Top-K on real takes, when the shared vector is given (--topk-takes, TF_TOPK_TAKES: the shared vector
   topk_takes.json of the four SDKs): each conformance clip through a Session with top_k 3 and with top_k 38, in
   every context the vector covers (`tf_tests topk`). test/check_outputs.py compares the SHA-256 of each take's
   log_probs first, then every frame's ids, log-probs and margins with the reference's ranking, exactly. Without
   the file the step is skipped and the summary says so.

Inputs (arguments, else the environment): the folder of the local model folders with its local30.json
(--local30, TF_LOCAL30: the test public key and the build ids are read from that file, never recorded here); the
conformance clips as the archive or an unpacked folder with audio/*.pcm (--clips, TF_CLIPS); the engine's source
tree (--engine, TF_ENGINE_DIR, else vendor/zipformer-sdk). The model folders are only read; everything written
goes to --work (a temporary folder by default, removed at the end).

Bounds: every child has a wall-clock limit and is killed with its whole process tree when it passes it;
`tf_tests` caps its own memory at 2 GB; only the last 64 KB of a child's output are read. The summary at the end
gives clips, chunks, values compared, mismatches, wall time and the peak memory the children reported.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
LIMIT_S = 900


def run(cmd, log, limit=LIMIT_S):
    """Run cmd with its output in the file `log`; kill the whole process tree past the limit. -> (rc, tail)."""
    with open(log, "wb") as out:
        kw = {"start_new_session": True} if os.name != "nt" else {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
        p = subprocess.Popen([str(c) for c in cmd], stdout=out, stderr=subprocess.STDOUT, **kw)
        try:
            rc = p.wait(timeout=limit)
        except subprocess.TimeoutExpired:
            if os.name == "nt":
                subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], capture_output=True)
            else:
                os.killpg(p.pid, 9)
            p.wait()
            rc = 124
    size = os.path.getsize(log)
    with open(log, "rb") as f:
        f.seek(max(0, size - 65536))
        return rc, f.read().decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("--local30", default=os.environ.get("TF_LOCAL30", ""))
    ap.add_argument("--clips", default=os.environ.get("TF_CLIPS", ""))
    ap.add_argument("--engine", default=os.environ.get("TF_ENGINE_DIR", str(HERE.parent / "vendor" / "zipformer-sdk")))
    ap.add_argument("--work", default="")
    ap.add_argument("--topk-takes", default=os.environ.get("TF_TOPK_TAKES", ""))
    a = ap.parse_args()
    exe = Path(a.build) / ("tf_tests.exe" if os.name == "nt" else "tf_tests")
    info = Path(a.local30) / "local30.json" if a.local30 else None
    engine = Path(a.engine)
    missing = [what for what, ok in (("the test program " + str(exe), exe.is_file()),
                                     ("the local 3.0 model folders (--local30 or TF_LOCAL30, with local30.json)", bool(info and info.is_file())),
                                     ("the conformance clips (--clips or TF_CLIPS)", bool(a.clips and Path(a.clips).exists())),
                                     ("the engine's conformance rows (--engine or TF_ENGINE_DIR)", (engine / "bench" / "v4_conformance_check.py").is_file())) if not ok]
    if missing:
        print("local proof: skipped, not on this machine: " + "; ".join(missing))
        return 0
    local = json.loads(info.read_text(encoding="utf-8"))
    key = local["public_key_hex"]
    work = Path(a.work) if a.work else Path(tempfile.mkdtemp(prefix="tf_proof_"))
    work.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    # the clips: audio/*.pcm, from the folder or unpacked from the archive (only the audio members, plain names)
    clips_dir = Path(a.clips)
    if clips_dir.is_file():
        clips_dir = work / "clips"
        (clips_dir / "audio").mkdir(parents=True, exist_ok=True)
        with tarfile.open(a.clips) as t:
            for m in t.getmembers():
                name = Path(m.name).name
                if m.isfile() and m.name.replace("\\", "/").startswith("audio/") and name.endswith(".pcm") and m.size < (64 << 20):
                    with t.extractfile(m) as src, open(clips_dir / "audio" / name, "wb") as dst:
                        shutil.copyfileobj(src, dst)
    pcm = sorted((clips_dir / "audio").glob("*.pcm"))
    (work / "clips.txt").write_text("".join(p.as_posix() + "\n" for p in pcm), encoding="utf-8")
    contexts = sorted(p.name[len("v4_"):-len("_audio_expected.jsonl")] for p in (engine / "conformance").glob("v4_*_audio_expected.jsonl"))
    variants = [v for v in ("full", "prod") if v in local.get("variants", {}) and (v + "-decoded") in local.get("decoded", {})]
    total = {"takes": 0, "chunks": 0, "values": 0, "mismatches": 0, "listen": 0, "listen_bad": 0, "peak": 0}
    failed = []
    for v in variants:
        folder = local["decoded"][v + "-decoded"]["path"]
        for ctx in contexts:
            out = work / f"conf_{v}_{ctx}.jsonl"
            rc, tail = run([exe, "conformance", folder, key, work / "clips.txt", out, "0xfff", ctx], work / f"conf_{v}_{ctx}.log")
            m = re.search(r"conformance totals: (\d+) clips, (\d+) chunks, (\d+) values, [\d.]+ s, peak memory (\d+) MB", tail)
            rc2, tail2 = run([sys.executable, engine / "bench" / "v4_conformance_check.py", out, engine / "conformance" / f"v4_{ctx}_audio_expected.jsonl"],
                             work / f"check_{v}_{ctx}.log", 300)
            ok = re.findall(r"(\d+)/(\d+)", tail2)
            passed, of = (int(ok[-1][0]), int(ok[-1][1])) if ok else (0, len(pcm))
            clips, chunks, values, peak = (int(x) for x in m.groups()) if m else (0, 0, 0, 0)
            bad = (of - passed) if (rc == 0 and rc2 == 0 and m) else max(of - passed, 1)
            total["takes"] += clips
            total["chunks"] += chunks
            total["values"] += values
            total["mismatches"] += bad
            total["peak"] = max(total["peak"], peak)
            print(f"  {v}-decoded {ctx}: {clips} clips, {chunks} chunks, {values} values, bit-identical {passed}/{of} (run rc {rc}, checker rc {rc2})")
            if bad:
                failed.append(f"conformance {v} {ctx}")
        configs = work / f"configs_{v}.json"
        subprocess.run([sys.executable, str(HERE / "make_model_configs.py"), str(configs), str(info), v], check=True, capture_output=True, timeout=120)
        rc, tail = run([exe, "model30", a.local30, key, work / f"model30_{v}", configs, v], work / f"model30_{v}.log")
        m = re.search(r"listen: (\d+)/(\d+) takes", tail)
        pk = re.search(r"peak memory (\d+) MB", tail)
        got, of = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        total["listen"] += of
        total["listen_bad"] += of - got if m else 1
        total["peak"] = max(total["peak"], int(pk.group(1)) if pk else 0)
        fails = [ln[:300] for ln in tail.splitlines() if ln.startswith("FAIL")]
        print(f"  {v}: sample clips through tf_session_listen {got}/{of} give the self-test tokens; model30 {'all ok' if rc == 0 else 'FAILED (rc %d)' % rc}")
        for ln in fails[:20]:
            print("    " + ln)
        if rc != 0 or not m or got != of:
            failed.append(f"model30 {v}")
        shutil.rmtree(work / f"model30_{v}", ignore_errors=True)
    # 3. top-K on real takes against the shared vector (the `full` folder: the ranking does not depend on the variant)
    topk = "top-K on real takes: skipped, no --topk-takes"
    if a.topk_takes and Path(a.topk_takes).is_file() and variants:
        vec = json.loads(Path(a.topk_takes).read_text(encoding="utf-8"))
        folder = local["decoded"][variants[0] + "-decoded"]["path"]
        runs = []
        for ctx in vec["contexts"]:
            for k in (vec["k"], vec["classes"]):
                out = work / f"topk_{ctx}_k{k}.jsonl"
                rc, tail = run([exe, "topk", folder, key, work / "clips.txt", out, ctx, k], work / f"topk_{ctx}_k{k}.log")
                if rc != 0:
                    failed.append(f"top-K run {ctx} k {k}")
                runs.append(out)
        rc, tail = run([sys.executable, HERE / "check_outputs.py", "topk", a.topk_takes] + runs, work / "topk_check.log", 600)
        line = [ln for ln in tail.splitlines() if ln.startswith("top-K on real takes")]
        topk = line[-1] if line else "top-K on real takes: the checker printed no summary"
        for ln in tail.splitlines():
            if ln.startswith("  "):
                print("  " + ln[:300])
        if rc != 0 or not line:
            failed.append("top-K on real takes")
    print("  " + topk)
    wall = time.time() - t0
    print(f"local proof ({', '.join(variants)}; contexts with audio rows: {', '.join(contexts)}): {total['takes']} clip takes through the Session API, "
          f"{total['chunks']} chunks, {total['values']} values compared, {total['mismatches']} mismatches; {total['listen']} sample clip takes through "
          f"tf_session_listen, {total['listen_bad']} mismatches; wall time {wall:.0f} s; peak memory {total['peak']} MB")
    if not a.work:
        shutil.rmtree(work, ignore_errors=True)
    if failed:
        print("local proof: FAILED: " + ", ".join(failed))
        return 1
    print("local proof: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
