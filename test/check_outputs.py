#!/usr/bin/env python3
"""Compare what tf_tests produced through the library's API with the reference files.

    check_outputs.py layers <vec.jsonl> <layers_vectors.json>
    check_outputs.py lazy   <default heads conformance.jsonl> <all heads conformance.jsonl>
    check_outputs.py topk   <topk_takes.json> <tf_tests topk run.jsonl> [<another run.jsonl> ...]

layers: the decoded layer of every fixture take against layers_vectors.json (the shared vectors every SDK's
decoded layer is held to): frames, token ids and frames exact, numbers within 1e-9, words, transcript; and top-K
of every ranked head (ids and values exact, margins within 1e-9). The 3.0 fields `settled` and `isFinal` are not in
those vectors (they are held to settled.json by tf_tests vectors30) and are ignored here.
lazy: with the default head selection the heads nobody asked for are not computed (their fingerprints are 0),
and every computed head is bit-identical to the all-heads run.
topk: what `tf_tests topk` wrote (each conformance clip through a Session with top_k) against the shared vector
topk_takes.json of the four SDKs, whose rankings are the reference's. Per take the SHA-256 of
log_probs first (a different engine output is not a ranking fault), then the ids, the log-probs and the margins
of every frame, all exact; a run 38 deep is compared with the vector's full ranking of the first frames, and its
first three ranks of every frame with the 3-deep lists. Every take of the vector in a context a run covers must
be in that run, and a run that compared nothing fails.
Exit 1 on any difference.
"""
import json
import struct
import sys


def close(a, b, tol):
    if a is None or b is None:
        return a is None and b is None
    return abs(a - b) <= tol


def check_decoded(got, want, tag, bad):
    gf, wf = got.get("frames", []), want["frames"]
    if len(gf) != len(wf):
        bad.append(f"{tag}: {len(gf)} frames, want {len(wf)}")
        return
    for t, (a, b) in enumerate(zip(gf, wf)):
        if a["top1"] != b["top1"] or a["runnerUp"] != b["runnerUp"] or not close(a["top1LogProb"], b["top1LogProb"], 1e-9) \
                or not close(a["runnerUpLogProb"], b["runnerUpLogProb"], 1e-9) or not close(a["margin"], b["margin"], 1e-9):
            bad.append(f"{tag}: frame {t} {a} != {b}")
            return
    gt, wt = got["tokens"], want["tokens"]
    if [x["id"] for x in gt] != [x["id"] for x in wt]:
        bad.append(f"{tag}: token ids differ")
        return
    for a, b in zip(gt, wt):
        for k in ("frame0", "frame1", "startMs", "endMs", "peakFrame", "runnerUp", "unit"):
            if a[k] != b[k]:
                bad.append(f"{tag}: token {k} {a[k]} != {b[k]}")
                return
        for k in ("logProb", "runnerUpLogProb", "margin", "confidence"):
            if not close(a[k], b[k], 1e-9):
                bad.append(f"{tag}: token {k} {a[k]} != {b[k]}")
                return
    gw, ww = got["words"], want["words"]
    if len(gw) != len(ww):
        bad.append(f"{tag}: {len(gw)} words, want {len(ww)}")
        return
    for a, b in zip(gw, ww):
        if a["tokens"] != b["tokens"] or a["startMs"] != b["startMs"] or a["endMs"] != b["endMs"] \
                or not close(a["confidence"], b["confidence"], 1e-9) or not close(a["margin"], b["margin"], 1e-9):
            bad.append(f"{tag}: word {a} != {b}")
            return
    if got["transcript"] != want["transcript"]:
        bad.append(f"{tag}: transcript differs")


def layers(run, layers_path):
    lay = {v["name"] + "|" + v["context"]: v for v in json.load(open(layers_path, encoding="utf-8"))["vectors"]}
    dec_ok = rows = 0
    topk = [0, 0, 0]  # vectors with a topk reference, passed, head rankings
    bad_all = []
    for line in open(run, encoding="utf-8"):
        r = json.loads(line)
        rows += 1
        key = r["name"] + "|" + r["context"]
        lv = lay[key]
        bad = []
        check_decoded(r["decoded"], lv["decoded"], key, bad)
        dec_ok += not bad
        bad_all += bad
        # top-K (layers_ref.py "topk", every 6th vector): ids exact, values exact, margins within 1e-9
        if "topk" in lv:
            tbad = []
            for name, want in lv["topk"].items():
                got = r["topk"].get(name)
                if got is None or got["k"] != want["k"] or got["ids"] != want["ids"]:
                    tbad.append(f"topk {name}: ids")
                    continue
                if any(a != b for ga, wa in zip(got["values"], want["values"]) for a, b in zip(ga, wa)):
                    tbad.append(f"topk {name}: values")
                if any(abs(a - b) > 1e-9 for ga, wa in zip(got["margins"], want["margins"]) for a, b in zip(ga, wa)):
                    tbad.append(f"topk {name}: margins")
            topk[0] += 1
            topk[1] += not tbad
            topk[2] += len(lv["topk"])
            bad_all += [f"{key}: {x}" for x in tbad[:3]]
    print(f"top-K vs the layers vectors: {topk[1]}/{topk[0]} takes ({topk[2]} head rankings; ids and values exact, margins within 1e-9)")
    print(f"decoded layer vs the layers vectors: {dec_ok}/{len(lay)} takes")
    for b in bad_all[:10]:
        print("  " + b)
    if topk[0] == 0:
        # layers vectors from before their top-K reference existed: nothing to compare here. The ranking is held
        # to real takes by `topk` below (the shared vector topk_takes.json) and to topk.json in ctest.
        print("  these layers vectors carry no top-K reference: none compared in this step")
    return 0 if dec_ok == len(lay) == rows and topk[1] == topk[0] else 1


def lazy(default_run, all_run):
    sel = {"log_probs", "inter1_log_probs", "timing_log_probs", "gem", "attr_qalqalah", "conf"}
    a = [json.loads(l) for l in open(default_run, encoding="utf-8") if l.strip()]
    b = {r["clip"]: r for r in (json.loads(l) for l in open(all_run, encoding="utf-8") if l.strip())}
    bad = 0
    for r in a:
        o = b[r["clip"]]
        for k, v in r["out_fnv"].items():
            if k in sel:
                bad += v != o["out_fnv"][k]
            else:
                bad += any(x != "0000000000000000" for x in v)
    print(f"lazy heads: {len(a)} clips, {bad} differences (selected heads vs the all-heads run; unselected heads not computed)")
    return 1 if bad or not a else 0


def topk_takes(vec_path, runs):
    vec = json.load(open(vec_path, encoding="utf-8"))
    k3, full_frames, classes = vec["k"], vec["full_frames"], vec["classes"]
    want = {(t["clip"], t["context"]): t for t in vec["takes"]}
    bad, near, values, frames, takes, other_engine = [], 0, 0, 0, 0, 0
    seen = set()

    def same(tag, got, exp, margin=False):
        nonlocal near, values
        values += 1
        if got == exp:
            return
        if margin and abs(got - exp) <= 1e-9:
            near += 1
        bad.append(f"{tag}: {got!r} != {exp!r}")

    for run in runs:
        for line in open(run, encoding="utf-8"):
            if not line.strip():
                continue
            r = json.loads(line)
            key = (r["clip"], r["context"])
            w = want.get(key)
            tag = f"{r['clip']} {r['context']} k {r['k']}"
            if w is None:
                bad.append(f"{tag}: not a take of the vectors")
                continue
            seen.add((key, r["k"]))
            takes += 1
            if r["log_probs_sha256"] != w["log_probs_sha256"]:
                other_engine += 1
                bad.append(f"{tag}: the engine's log_probs differ (SHA-256), so the ranking was not compared")
                continue
            if r["frames"] != w["frames"]:
                bad.append(f"{tag}: {r['frames']} frames, want {w['frames']}")
                continue
            k = r["k"]
            lp = [struct.unpack(">f", bytes.fromhex(x))[0] for x in r["log_probs_bits"]]
            ma = [struct.unpack(">d", bytes.fromhex(x))[0] for x in r["margins_bits"]]
            if k < k3 or len(r["ids"]) != r["frames"] * k or len(lp) != len(r["ids"]) or len(ma) != len(lp):
                bad.append(f"{tag}: the run's lists do not hold frames x k values")
                continue
            for f in range(r["frames"]):
                for j in range(k3):  # the first three ranks of every frame
                    at, wat = f * k + j, f * k3 + j
                    same(f"{tag} frame {f} rank {j + 1} id", r["ids"][at], w["ids"][wat])
                    same(f"{tag} frame {f} rank {j + 1} log-prob", lp[at], w["log_probs"][wat])
                    # a margin is to the next rank of the whole row, so it is the same number at every depth
                    same(f"{tag} frame {f} rank {j + 1} margin", ma[at], w["margins"][wat], True)
            if k == classes:
                for f in range(full_frames):
                    for j in range(classes):
                        at = f * classes + j
                        same(f"{tag} frame {f} full rank {j + 1} id", r["ids"][at], w["full"]["ids"][at])
                        same(f"{tag} frame {f} full rank {j + 1} log-prob", lp[at], w["full"]["log_probs"][at])
                        same(f"{tag} frame {f} full rank {j + 1} margin", ma[at], w["full"]["margins"][at], True)
            frames += r["frames"]
    covered = {(c, k) for ((_, c), k) in seen}
    missing = [f"{clip} {ctx} k {k}" for (clip, ctx) in want for (c, k) in covered if c == ctx and ((clip, ctx), k) not in seen]
    for m in missing[:5]:
        bad.append(f"{m}: a take of the vectors that the run does not have")
    print(f"top-K on real takes vs the shared vector: {takes} takes, {frames} frames, {values} values compared, "
          f"{len(bad)} differences ({other_engine} takes with other log_probs, {near} margins within 1e-9 but not equal)")
    for x in bad[:12]:
        print("  " + x)
    return 0 if takes and not bad else 1


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "topk":
        sys.exit(topk_takes(sys.argv[2], sys.argv[3:]))
    if len(sys.argv) >= 4 and sys.argv[1] == "layers":
        sys.exit(layers(sys.argv[2], sys.argv[3]))
    if len(sys.argv) >= 4 and sys.argv[1] == "lazy":
        sys.exit(lazy(sys.argv[2], sys.argv[3]))
    raise SystemExit(__doc__)
