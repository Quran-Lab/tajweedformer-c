#!/usr/bin/env bash
# The library's tests that need model data, through its own API (tf_tests) and the reference checkers.
#
#   test/run_tests.sh <build dir> <data dir> [contexts]
#
# <data dir> is the v4-test-c16 release asset unpacked: v4/ (tokens, calibration, the expected table, c16_l64/
# engine files; c24_l72/ and c24_l256/ too when present), audio/*.pcm (the 35 conformance clips), fixture/ (heads of
# real takes, for the decoded layer), layers_vectors.json, and optionally v4_<ctx>_audio_native_x86.jsonl for the
# contexts other than c16_l64 (c8_l64, c24_l72, c24_l256). Exits nonzero on any failure.
#
# Environment:
#   TF_ENGINE_DIR  the engine's source tree when it is not the vendor/zipformer-sdk submodule (as for cmake)
#   TF_LOCAL30     the folder that holds the local 3.0 model folders and their local30.json (the key they are
#                  signed with and their build ids are read from it): runs the Model, Session, listen, reference
#                  and download tests on the variant full, and on prod when it is there
#   TF_TEST_TIMEOUT seconds one run of the test program may take (900)
set -uo pipefail
B=$1 D=$2 CTXS=${3:-c16_l64}
HERE=$(cd "$(dirname "$0")" && pwd)
ENG=${TF_ENGINE_DIR:-$HERE/../vendor/zipformer-sdk}
PY=${PYTHON:-python3}
"$PY" -c '' 2>/dev/null || PY=python
T=$B/tf_tests; [ -f "$T.exe" ] && T=$T.exe
# every run of the test program has a wall-clock limit (it also caps its own memory at 2 GB): a test that
# hangs or runs away is stopped
TL=${TF_TEST_TIMEOUT:-900}
tt() { if command -v timeout >/dev/null; then timeout "$TL" "$T" "$@"; else "$T" "$@"; fi; }
W=$(mktemp -d)
fails=0
run() { echo "== $1"; shift; "$@" || { echo "FAILED"; fails=$((fails + 1)); }; }
SEED=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f # throwaway test key, never Quran Lab's

run "crypto (SHA-2, Ed25519 RFC 8032, base64, JSON)" tt crypto

# model folders with manifests: the V4 folder signed with the test key (with the calibration and the reference
# table), the same without either (SPEC section 8), and the fixture folders unsigned (tokens only: no engine)
args=()
for c in c16_l64 c8_l64 c24_l72 c24_l256; do
  [ -d "$D/v4/$c" ] || continue
  for f in model_engine2.onnx model_engine2.onnx.zfg model_engine2.onnx.zfg.bin frontend.zfw layers.zfw; do args+=("$D/v4/$c/$f=$c/$f"); done
done
KEY=$("$PY" "$HERE/make_test_folder.py" "$W/v4" --sign $SEED "$D/v4/calibration.json=calibration.json" "$D/v4/tokens.txt=tokens.txt" \
  "$D/v4/expected_tj3.zfx=expected_tj3.zfx" "${args[@]}")
"$PY" "$HERE/make_test_folder.py" "$W/v4bare" --sign $SEED "$D/v4/tokens.txt=tokens.txt" "${args[@]}" >/dev/null
FX=$D/fixture/model
for c in c16_l64 c24_l72; do
  "$PY" "$HERE/make_test_folder.py" "$W/fx_$c" "$FX/calibration.json=calibration.json" "$FX/tokens.txt=tokens.txt"
done

# the folder checks: signed opens; a wrong key, a missing signature and a changed file are refused
expect() { local want=$1; shift; local got; got=$("$@" 2>/dev/null | tail -1); [ "${got:0:1}" = "{" ] && got=ok; [ "$got" = "$want" ] || { echo "expected $want, got $got"; return 1; }; }
run "folder: signed with the trusted key opens" expect ok tt folder "$W/v4" "$KEY"
run "folder: without calibration.json and without the expected table opens" expect ok tt folder "$W/v4bare" "$KEY"
run "folder: another trusted key is refused" expect signature_invalid tt folder "$W/v4" fc02fbd906608ebc206a371d8e61c1062ea555778b44b1ac834061043aee4a33
cp -r "$W/fx_c16_l64" "$W/tamper"
run "folder: unsigned is refused" expect signature_invalid tt folder "$W/tamper" "$KEY"
printf 'x' >> "$W/tamper/tokens.txt"
run "folder: a changed file is refused" expect checksum_mismatch tt folder "$W/tamper" -

ls "$D"/audio/*.pcm > "$W/clips.txt"
command -v cygpath >/dev/null && { cygpath -m -f "$W/clips.txt" > "$W/clips.w" && mv "$W/clips.w" "$W/clips.txt"; }
for c in ${CTXS//,/ }; do
  if [ "$c" = c16_l64 ]; then EXP=$ENG/conformance/v4_c16_l64_audio_expected.jsonl; else EXP=$D/v4_${c}_audio_native_x86.jsonl; fi
  [ -d "$W/v4/$c" ] && [ -f "$EXP" ] || { echo "== $c: no files, skipped"; continue; }
  run "conformance $c, all heads, through a Session per clip (35 clips)" tt conformance "$W/v4" "$KEY" "$W/clips.txt" "$W/conf_$c.jsonl" 0xfff $c
  run "conformance $c: bit-identical" "$PY" "$ENG/bench/v4_conformance_check.py" "$W/conf_$c.jsonl" "$EXP"
done
run "conformance c16_l64 on the folder without calibration.json and the expected table" tt conformance "$W/v4bare" "$KEY" "$W/clips.txt" "$W/conf_bare.jsonl" 0xfff c16_l64
run "conformance c16_l64 on that folder: bit-identical" "$PY" "$ENG/bench/v4_conformance_check.py" "$W/conf_bare.jsonl" "$ENG/conformance/v4_c16_l64_audio_expected.jsonl"
# layout 3 (one shared layers.zfw, a frames.zfw overlay per context, manifest format 3), made from the
# per-context folders with the engine's own tools/zfshare.py
if [ -f "$ENG/tools/zfshare.py" ]; then
  cargs=(); for c in c16_l64 c8_l64 c24_l72 c24_l256; do [ -d "$D/v4/$c" ] && cargs+=(--context "$c=$D/v4/$c"); done
  (cd "$ENG" && "$PY" -m tools.zfshare "${cargs[@]}" --out "$W/l3src" --no-onnx >/dev/null)
  l3=("$W/l3src/layers.zfw=layers.zfw" "$W/l3src/frontend.zfw=frontend.zfw")
  for c in c16_l64 c8_l64 c24_l72 c24_l256; do
    [ -d "$W/l3src/$c" ] || continue
    for f in model_engine2.onnx.zfg model_engine2.onnx.zfg.bin frames.zfw; do l3+=("$W/l3src/$c/$f=$c/$f"); done
  done
  common=("$D/v4/calibration.json=calibration.json" "$D/v4/tokens.txt=tokens.txt" "$D/v4/expected_tj3.zfx=expected_tj3.zfx")
  "$PY" "$HERE/make_test_folder.py" "$W/v4l3" --format3 --sign $SEED "${common[@]}" "${l3[@]}" >/dev/null
  run "layout 3 conformance c16_l64 through a Session per clip" tt conformance "$W/v4l3" "$KEY" "$W/clips.txt" "$W/l3.jsonl" 0xfff c16_l64
  run "layout 3 conformance c16_l64: bit-identical" "$PY" "$ENG/bench/v4_conformance_check.py" "$W/l3.jsonl" "$ENG/conformance/v4_c16_l64_audio_expected.jsonl"
  "$PY" "$HERE/make_test_folder.py" "$W/cdn3" --format3 --gzip --sign $SEED "${common[@]}" "${l3[@]}" >/dev/null
  run "downloader: layout 3, gzip, Range resume, the CPU set of one context, the reference table left out" tt download "$W/cdn3" "$W/dl" "$KEY"
else
  echo "== layout 3: the engine has no tools/zfshare.py, skipped"
fi
run "pinning: 6 pinned takes in a row on one thread keep their 4-thread team and restore the caller's mask" tt pin "$W/v4" "$KEY" "$W/clips.txt" 4 6
run "conformance c16_l64, default heads (selection respected, greedy == decoded)" tt conformance "$W/v4" "$KEY" "$W/clips.txt" "$W/conf_default.jsonl"
# lazy heads: with the default selection the heads nobody asked for are not computed (their fingerprints are 0),
# and every computed head is bit-identical to the all-heads run
if [ -f "$W/conf_c16_l64.jsonl" ]; then
  run "lazy heads: default selection computes only its heads, each bit-identical to the all-heads run" "$PY" "$HERE/check_outputs.py" lazy "$W/conf_default.jsonl" "$W/conf_c16_l64.jsonl"
fi

# The decoded layer and top-K on heads read from files. TF_LAYERS_FIXTURE (the fixture folder: vectors.json, the
# head files, model/) and TF_LAYERS_VECTORS (the layers vectors that go with it) name a newer set than the data
# folder's when given: a copy of the layers vectors from before their top-K reference existed made this step
# compare no ranking at all.
LFX=$D/fixture LVEC=$D/layers_vectors.json
if [ -n "${TF_LAYERS_FIXTURE:-}" ] && [ -f "${TF_LAYERS_VECTORS:-}" ] && [ -d "$TF_LAYERS_FIXTURE/model" ]; then
  LFX=$TF_LAYERS_FIXTURE LVEC=$TF_LAYERS_VECTORS
  for c in c16_l64 c24_l72; do
    "$PY" "$HERE/make_test_folder.py" "$W/lfx_$c" "$LFX/model/calibration.json=calibration.json" "$LFX/model/tokens.txt=tokens.txt"
  done
  run "the decoded layer and top-K on heads from files (TF_LAYERS_FIXTURE)" tt layers "$LFX" "$W/lfx_c16_l64" "$W/lfx_c24_l72" "$W/vec.jsonl"
else
  run "the decoded layer and top-K on heads from files" tt layers "$LFX" "$W/fx_c16_l64" "$W/fx_c24_l72" "$W/vec.jsonl"
fi
run "the decoded layer and top-K against the shared layers vectors" "$PY" "$HERE/check_outputs.py" layers "$W/vec.jsonl" "$LVEC"
# top-K on real takes: every clip through a Session with top_k, against the shared vector topk_takes.json
# (TF_TOPK_TAKES, the same file test/local_proof.py takes)
TK=${TF_TOPK_TAKES:-}
if [ -f "$TK" ]; then
  truns=()
  for c in c16_l64 c8_l64; do
    [ -d "$D/v4/$c" ] || continue
    for k in 3 38; do
      run "top-K on real takes: $c, $k deep, through a Session per clip" tt topk "$W/v4" "$KEY" "$W/clips.txt" "$W/topk_${c}_$k.jsonl" "$c" "$k"
      truns+=("$W/topk_${c}_$k.jsonl")
    done
  done
  run "top-K on real takes against the shared vector (ids, log-probs and margins exact)" "$PY" "$HERE/check_outputs.py" topk "$TK" "${truns[@]}"
else
  echo "== top-K on real takes: TF_TOPK_TAKES is not set (the shared vector topk_takes.json), skipped"
fi

# the platform client against recorded exchanges (signed with a local throwaway key), and the sharing state machine
run "Ogg muxer vs the cross-SDK vector (4 segment files, byte for byte)" tt ogg "$HERE/data/sharing_vectors/ogg" "$W/ogg"
run "deletion crypto (recovery codes, P-256 signing vectors) and the deletion flow (replayed)" tt sharing "$HERE/data/sharing_vectors"   "$HERE/data/client-fixtures.json" "$W/sharing"
run "Opus encoder vs the web vector (byte for byte) and the 48 kHz resampler" tt codec "$HERE/data/sharing_vectors" "$W/codec"
run "shared-session upload (batches, 403, crash recovery, sharing off, key mismatch)" tt upload "$W/upload"
run "the Session's hook: a take shared through a Session (16 kHz, resampled), heads unchanged" tt session_share "$W/v4" "$KEY" "$W/clips.txt" "$W/session_share"
run "platform client (replayed) and sharing state machine" tt client "$HERE/data/client-fixtures.json" "$W/client"
run "bitrate ladder vector, a 150 s stream at 192/96/32/64/192 kbit/s (pages, decode, ffmpeg when present), outbox wiring" tt bitrate "$HERE/data/sharing_vectors" "$W/bitrate"
run "consent wording vector, pause and update ask, session JSON, withdrawal, deletion; copy decks 2026-11-09.1" tt consent "$HERE/data/sharing_vectors" "$HERE/data/client-fixtures.json" "$W/consent"
run "sdk_latest in the signed config: one debug warning per client when newer, silent otherwise and in release builds" tt sdk_latest "$HERE/data/sdk_latest_configs.json" "$W/sdk_latest"
run "telemetry deltas (report_id, retries, midnight, send points, timer, share counters, daily register)" tt telemetry "$W/telemetry"
run "takes in a row: one recording, the take list call, recovery, withdrawal, background, the 20-minute cap" tt takes "$HERE/data/sharing_vectors" "$W/takes"
# 3.0: the error catalogue, the shared 3.0 vectors, the public Client
run "the error catalogue: the embedded copy, the 25 codes, messages and fixes" tt errors "$HERE/data/sdk_3.0.0"
run "the shared 3.0 vectors (manifest, speech, settled, telemetry, contexts, names, sharing)" tt vectors30 "$HERE/data/sdk_3.0.0" "$W/vectors30"
run "the public Client: one per process, options, storage, diagnose" tt api "$W/api" "$HERE/data/sdk_3.0.0"
run "shared recordings in 3.0: the 41 cases of sharing_30.json, the ring's bound, only fed audio in the tail wait" tt share30 "$HERE/data/sdk_3.0.0" "$W/share30"
if [ -n "${TF_LOCAL30:-}" ] && [ -f "$TF_LOCAL30/local30.json" ]; then
  KEY30=$("$PY" -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["public_key_hex"])' "$TF_LOCAL30/local30.json")
  for v in full prod; do
    [ -d "$TF_LOCAL30/$v-decoded" ] || continue
    "$PY" "$HERE/make_model_configs.py" "$W/configs_$v.json" "$TF_LOCAL30/local30.json" $v
    run "the local 3.0 folder $v: Model, Session, listen on the sample clips, the reference lookup, the Client's download path" tt model30 "$TF_LOCAL30" "$KEY30" "$W/model30_$v" "$W/configs_$v.json" $v
  done
else
  echo "== the local 3.0 folders: TF_LOCAL30 is not set, skipped"
fi

rm -rf "$W"
echo "$fails failing"
exit $((fails > 0))
