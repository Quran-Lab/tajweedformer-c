#!/bin/bash
# The engine's V4 conformance on this build (the native int8 hook linked into every harness):
#   scripts/conformance.sh BUILD_DIR MODEL_FOLDER [DATA_DIR]
# MODEL_FOLDER is sdk/v4.0 of Quran-Lab/tajweedformer-v4. DATA_DIR defaults to data/ (fetched once).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:?usage: conformance.sh BUILD_DIR MODEL_FOLDER [DATA_DIR]} M=${2:?model folder} D=${3:-$ROOT/data}
[ -d "$D/feats" ] || "$ROOT/vendor/zipformer-sdk/scripts/fetch-conformance-data.sh" "$D"
cmake --build "$B" --target zf_harnesses
ZF_NATIVE_STATS=1 "$ROOT/vendor/zipformer-sdk/bench/v4_conformance.sh" "$B/engine" "$M" "$D"
