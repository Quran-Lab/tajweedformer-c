#!/usr/bin/env bash
# The library's tests, through its own API (tf_tests) and the reference checkers.
#
#   test/run_tests.sh <build dir> <data dir> [contexts]
#
# <data dir> is the v4-test-c16 release asset unpacked: v4/ (calibration, tokens, expected table, c16_l64/ engine
# files; c24_l72/ and c24_l256/ too when present), audio/*.pcm (the 35 conformance clips), fixture/ (the reference's scorer
# fixture), layers_vectors.json, config_vectors.json, calibration_fixture.json, and optionally
# v4_<ctx>_audio_native_x86.jsonl for the chunk-24 contexts. Exits nonzero on any failure.
set -uo pipefail
B=$1 D=$2 CTXS=${3:-c16_l64}
HERE=$(cd "$(dirname "$0")" && pwd)
ENG=$HERE/../vendor/zipformer-sdk
PY=${PYTHON:-python3}
"$PY" -c '' 2>/dev/null || PY=python
T=$B/tf_tests; [ -f "$T.exe" ] && T=$T.exe
W=$(mktemp -d)
fails=0
run() { echo "== $1"; shift; "$@" || { echo "FAILED"; fails=$((fails + 1)); }; }
SEED=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f # throwaway test key, never Quran Lab's

run "crypto (SHA-2, Ed25519 RFC 8032, base64, JSON)" "$T" crypto

# model folders with manifests: the V4 folder signed with the test key; the scorer and settings fixtures unsigned
args=()
for c in c16_l64 c24_l72 c24_l256; do
  [ -d "$D/v4/$c" ] || continue
  for f in model_engine2.onnx model_engine2.onnx.zfg model_engine2.onnx.zfg.bin frontend.zfw layers.zfw; do args+=("$D/v4/$c/$f=$c/$f"); done
done
KEY=$("$PY" "$HERE/make_test_folder.py" "$W/v4" --sign $SEED "$D/v4/calibration.json=calibration.json" "$D/v4/tokens.txt=tokens.txt" \
  "$D/v4/expected_tj3.zfx=expected_tj3.zfx" "${args[@]}")
FX=$D/fixture/model
for c in c16_l64 c24_l72; do
  "$PY" "$HERE/make_test_folder.py" "$W/fx_$c" "$FX/calibration.json=calibration.json" "$FX/tokens.txt=tokens.txt" "$FX/expected_tj3.zfx=expected_tj3.zfx"
done
"$PY" "$HERE/make_test_folder.py" "$W/settings" --fix-tokens-sha "$D/calibration_fixture.json=calibration.json" "$FX/tokens.txt=tokens.txt" \
  "$FX/expected_tj3.zfx=expected_tj3.zfx"

# the folder checks: signed opens; a wrong key, a missing signature and a changed file are refused
expect() { local want=$1; shift; local got; got=$("$@" 2>/dev/null | tail -1); [ "${got:0:1}" = "{" ] && got=ok; [ "$got" = "$want" ] || { echo "expected $want, got $got"; return 1; }; }
run "folder: signed with the trusted key opens" expect ok "$T" folder "$W/v4" "$KEY"
run "folder: another trusted key is refused" expect signature_invalid "$T" folder "$W/v4" fc02fbd906608ebc206a371d8e61c1062ea555778b44b1ac834061043aee4a33
cp -r "$W/fx_c16_l64" "$W/tamper"
run "folder: unsigned is refused when a signature is required" expect signature_required "$T" folder "$W/tamper" "$KEY"
printf 'x' >> "$W/tamper/tokens.txt"
run "folder: a changed file is refused" expect checksum_mismatch "$T" folder "$W/tamper" -

ls "$D"/audio/*.pcm > "$W/clips.txt"
command -v cygpath >/dev/null && { cygpath -m -f "$W/clips.txt" > "$W/clips.w" && mv "$W/clips.w" "$W/clips.txt"; }
for c in ${CTXS//,/ }; do
  if [ "$c" = c16_l64 ]; then EXP=$ENG/conformance/v4_c16_l64_audio_expected.jsonl; else EXP=$D/v4_${c}_audio_native_x86.jsonl; fi
  [ -d "$W/v4/$c" ] && [ -f "$EXP" ] || { echo "== $c: no files, skipped"; continue; }
  run "conformance $c, all heads, through tf_stream (35 clips)" "$T" conformance "$W/v4" "$KEY" "$W/clips.txt" "$W/conf_$c.jsonl" 0xfff $c
  run "conformance $c: bit-identical" "$PY" "$ENG/bench/v4_conformance_check.py" "$W/conf_$c.jsonl" "$EXP"
done
# layout 3 (DELIVERY: one shared layers.zfw, a frames.zfw overlay per context, manifest format 3), made from the
# per-context folders with the engine's own tools/zfshare.py
if [ -f "$ENG/tools/zfshare.py" ]; then
  cargs=(); for c in c16_l64 c24_l72 c24_l256; do [ -d "$D/v4/$c" ] && cargs+=(--context "$c=$D/v4/$c"); done
  (cd "$ENG" && "$PY" -m tools.zfshare "${cargs[@]}" --out "$W/l3src" --no-onnx >/dev/null)
  l3=("$W/l3src/layers.zfw=layers.zfw" "$W/l3src/frontend.zfw=frontend.zfw")
  for c in c16_l64 c24_l72 c24_l256; do
    [ -d "$W/l3src/$c" ] || continue
    for f in model_engine2.onnx.zfg model_engine2.onnx.zfg.bin frames.zfw; do l3+=("$W/l3src/$c/$f=$c/$f"); done
  done
  common=("$D/v4/calibration.json=calibration.json" "$D/v4/tokens.txt=tokens.txt" "$D/v4/expected_tj3.zfx=expected_tj3.zfx")
  "$PY" "$HERE/make_test_folder.py" "$W/v4l3" --format3 --sign $SEED "${common[@]}" "${l3[@]}" >/dev/null
  run "layout 3 conformance c16_l64 through tf_stream" "$T" conformance "$W/v4l3" "$KEY" "$W/clips.txt" "$W/l3.jsonl" 0xfff c16_l64
  run "layout 3 conformance c16_l64: bit-identical" "$PY" "$ENG/bench/v4_conformance_check.py" "$W/l3.jsonl" "$ENG/conformance/v4_c16_l64_audio_expected.jsonl"
  "$PY" "$HERE/make_test_folder.py" "$W/cdn3" --format3 --gzip --sign $SEED "${common[@]}" "${l3[@]}" >/dev/null
  run "downloader: layout 3, gzip, Range resume, native profile" "$T" download "$W/cdn3" "$W/dl" "$KEY"
else
  echo "== layout 3: the engine has no tools/zfshare.py, skipped"
fi
run "pinning: 6 pinned streams in a row on one thread keep their 4-thread team and restore the caller's mask" "$T" pin "$W/v4" "$KEY" "$W/clips.txt" 4 6
run "conformance c16_l64, default heads (selection respected, greedy == decoded)" "$T" conformance "$W/v4" "$KEY" "$W/clips.txt" "$W/conf_default.jsonl"
# lazy heads (engine API 3): with the default selection the heads nobody asked for are not computed (their
# fingerprints are 0: NULL), and every computed head is bit-identical to the all-heads run
if [ -f "$W/conf_c16_l64.jsonl" ]; then
  run "lazy heads: default selection computes only its heads, each bit-identical to the all-heads run" "$PY" - "$W/conf_default.jsonl" "$W/conf_c16_l64.jsonl" <<'EOF'
import json, sys
sel = {"log_probs", "inter1_log_probs", "timing_log_probs", "gem", "attr_qalqalah", "conf"}
a = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
b = {r["clip"]: r for r in (json.loads(l) for l in open(sys.argv[2]) if l.strip())}
bad = 0
for r in a:
    o = b[r["clip"]]
    for k, v in r["out_fnv"].items():
        if k in sel:
            bad += v != o["out_fnv"][k]
        else:
            bad += any(x != "0000000000000000" for x in v)
print(f"lazy heads: {len(a)} clips, {bad} differences (selected heads vs all-heads run; unselected heads NULL)")
sys.exit(1 if bad or not a else 0)
EOF
fi

run "CAL vectors and layers through tf_score / tf_decode" "$T" vectors "$D/fixture" "$W/fx_c16_l64" "$W/fx_c24_l72" "$W/vec.jsonl"
run "CAL vectors 96/96, typed sites, word scores, decoded layer" "$PY" "$HERE/check_outputs.py" vectors "$W/vec.jsonl" "$D/fixture" "$D/layers_vectors.json" "$ENG"
run "settings vectors through tf_settings" "$T" settings "$D/config_vectors.json" "$W/settings" "$W/set.jsonl"
run "settings vectors 39/39" "$PY" "$HERE/check_outputs.py" settings "$W/set.jsonl" "$D/config_vectors.json"

# the platform client against the portal team's recorded exchanges (quranlab-local p2-portalux-client
# fixtures/client-fixtures.json, signed with the local throwaway key), and the sharing state machine
run "Ogg muxer vs the cross-SDK vector (4 segment files, byte for byte)" "$T" ogg "$HERE/data/sharing_vectors/ogg" "$W/ogg"
run "deletion crypto (recovery codes, P-256 signing vectors) and the deletion flow (replayed)" "$T" sharing "$HERE/data/sharing_vectors"   "$HERE/data/client-fixtures.json" "$W/sharing"
run "Opus encoder vs the web vector (byte for byte) and the 48 kHz resampler" "$T" codec "$HERE/data/sharing_vectors" "$W/codec"
run "shared-session upload (batches, 403, crash recovery, sharing off, key mismatch)" "$T" upload "$W/upload"
run "the stream hook: a take shared through tf_stream (16 kHz, resampled), heads unchanged" "$T" stream_share "$W/v4" "$KEY" "$W/clips.txt" "$W/stream_share"
run "platform client (replayed) and sharing state machine" "$T" client "$HERE/data/client-fixtures.json" "$W/client" "$W/fx_c16_l64"

rm -rf "$W"
echo "$fails failing"
exit $((fails > 0))
