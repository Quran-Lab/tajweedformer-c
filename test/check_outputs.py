#!/usr/bin/env python3
"""Compare what tf_tests produced through the library's API with the reference files.

    check_outputs.py vectors  <vec.jsonl> <scorer_fixture_dir> <layers_vectors.json> <engine_dir>
    check_outputs.py settings <set.jsonl> <config_vectors.json>

vectors: the reference verdicts (both presets) with the engine's own comparison rules
(tajweed/tests/run_vectors.py compare: flags identical, LLRs within 1e-3, thresholds within 1e-12), the typed
sites against the engine's JSON, the decoded layer against layers_vectors.json (ids and frames exact, numbers
within 1e-9) and the word scores against it (within 1e-9, same evidence class).
settings: every case's expected resolution (every key; numbers within 1e-9), as run_config_vectors.py same().
Exit 1 on any difference.
"""
import importlib.util
import json
import math
import os
import sys


def load_compare(engine_dir):
    p = os.path.join(engine_dir, "tajweed", "tests", "run_vectors.py")
    spec = importlib.util.spec_from_file_location("run_vectors", p)
    m = importlib.util.module_from_spec(spec)
    sys.path.insert(0, os.path.dirname(p))
    spec.loader.exec_module(m)
    return m.compare


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
        bad.append(f"{tag}: {len(gw)} segments, want {len(ww)}")
        return
    for a, b in zip(gw, ww):
        if a["tokens"] != b["tokens"] or a["startMs"] != b["startMs"] or a["endMs"] != b["endMs"] \
                or not close(a["confidence"], b["confidence"], 1e-9) or not close(a["margin"], b["margin"], 1e-9):
            bad.append(f"{tag}: segment {a} != {b}")
            return
    if got["transcript"] != want["transcript"]:
        bad.append(f"{tag}: transcript differs")


TOPK = [0, 0, 0]  # vectors with a topk reference, passed, head rankings


def vectors(run, fx, layers, engine_dir):
    compare = load_compare(engine_dir)
    man = json.load(open(os.path.join(fx, "vectors.json"), encoding="utf-8"))
    refs = {v["name"] + "|" + v["context"]: v for v in man["vectors"]}
    lay = {v["name"] + "|" + v["context"]: v for v in json.load(open(layers, encoding="utf-8"))["vectors"]}
    ok = total = dec_ok = word_ok = typed_ok = 0
    bad_all = []
    for line in open(run, encoding="utf-8"):
        r = json.loads(line)
        key = r["name"] + "|" + r["context"]
        v, lv = refs[key], lay[key]
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
            TOPK[0] += 1
            TOPK[1] += not tbad
            TOPK[2] += len(lv["topk"])
            bad_all += [f"{key}: {x}" for x in tbad[:3]]
        for preset, res in r["results"].items():
            total += 1
            ref = json.load(open(os.path.join(fx, v["refs"][preset]), encoding="utf-8"))
            b = compare(res["raw"], ref)
            # typed sites = one per (window, class) of the engine JSON, flagged on the window's class
            want = [(w["i"], c, w["per_class"][c]) for w in res["raw"]["sites"] for c in w["per_class"]]
            tb = []
            if len(want) != len(res["sites"]):
                tb.append(f"typed sites {len(res['sites'])} != JSON {len(want)}")
            else:
                for s, (i, c, pc) in zip(res["sites"], want):
                    if s["class"] != c or not close(s["fusedLlr"], pc["fused"], 1e-8) or not close(s["threshold"], pc["threshold"], 1e-8):
                        tb.append(f"typed site {i} {c}")
                        break
            typed_ok += not tb
            # word scores against the reference verdicts' per-class fused values
            wb = []
            got = res["words"]
            if "words" in ref:
                # READY4 and later: the reference's word readout (every single edit inside the word), flag at or above tau
                items = ref["words"]["items"]
                if len(items) != len(got):
                    wb.append(f"words: {len(got)} != {len(items)}")
                for w in items[:len(got)]:
                    g = got[w["word"]]
                    if not close(g["score"], w["score"], 1e-3) or bool(g["flagged"]) != bool(w["flagged"]) or                             (w.get("class") and (g["evidence"] or {}).get("class") != w["class"]):
                        wb.append(f"word {w['word']}: {g['score']} {g['flagged']} != {w['score']} {w['flagged']}")
                word_ok += not wb
                if b or tb or wb:
                    bad_all.append(f"{key} {preset}: " + "; ".join((b + tb + wb)[:3]))
                else:
                    ok += 1
                continue
            ws = lv["words"][preset]
            for w in ws:
                g = got[w["word"]] if w["word"] < len(got) else None
                if g is None or not close(g["score"], w["score"], 1e-3) or \
                        (w["class"] is not None and (g["evidence"] or {}).get("class") != w["class"]):
                    wb.append(f"word {w['word']}: {g and g['score']} {(g and g['evidence'] or {}).get('class')} != {w['score']} {w['class']}")
            word_ok += not wb
            if b or tb or wb:
                bad_all.append(f"{key} {preset}: " + "; ".join((b + tb + wb)[:3]))
            else:
                ok += 1
    print(f"top-K through tf_topk vs layers_ref: {TOPK[1]}/{TOPK[0]} vectors ({TOPK[2]} head rankings; ids and values exact, margins within 1e-9)")
    print(f"CAL vectors through tf_score: {ok}/{total} (flags identical, LLRs within 1e-3); typed sites == engine JSON "
          f"{typed_ok}/{total}; word scores {word_ok}/{total}; decoded layer == layers_ref {dec_ok}/{len(lay)}")
    for b in bad_all[:10]:
        print("  " + b)
    topk_ok = TOPK[0] > 0 and TOPK[1] == TOPK[0]
    return 0 if ok == total and dec_ok == len(lay) and topk_ok else 1


def same(a, b, at, bad):
    if isinstance(b, dict):
        if not isinstance(a, dict):
            bad.append(f"{at}: {a!r} expected {b!r}")
            return
        for k, v in b.items():
            if k not in a:
                bad.append(f"{at}.{k}: missing")
            else:
                same(a[k], v, f"{at}.{k}", bad)
    elif isinstance(b, (int, float)) and not isinstance(b, bool) and isinstance(a, (int, float)) and not isinstance(a, bool):
        if not (math.isfinite(a) and abs(a - b) <= 1e-9):
            bad.append(f"{at}: {a} expected {b}")
    elif a != b:
        bad.append(f"{at}: {a!r} expected {b!r}")


def settings(run, vec):
    cases = {c["name"]: c for c in json.load(open(vec, encoding="utf-8"))["cases"]}
    ok = 0
    bad_all = []
    rows = [json.loads(l) for l in open(run, encoding="utf-8")]
    for r in rows:
        c = cases[r["name"]]
        bad = []
        if r["rc"] != 0:
            bad.append(f"app settings refused: {r['rc']}")
        same(r["settings"], c["expect"], "", bad)
        if bad:
            bad_all.append(f"{r['name']}: {'; '.join(bad[:3])}")
        else:
            ok += 1
    print(f"settings vectors through tf_settings_*: {ok}/{len(cases)}")
    for b in bad_all[:10]:
        print("  " + b)
    return 0 if ok == len(cases) == len(rows) else 1


if __name__ == "__main__":
    if sys.argv[1] == "vectors":
        sys.exit(vectors(*sys.argv[2:6]))
    if sys.argv[1] == "settings":
        sys.exit(settings(*sys.argv[2:4]))
    raise SystemExit(__doc__)
