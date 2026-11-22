"""The public surface of the C SDK is the naming table's and nothing else (SDK 3.0.0; SDK 4.0.0 adds 17.9's
report of the recited range with the app's outcome: TfRecitedOutcome, TfRecitedAyah,
tf_session_report_recited_outcome, a function of its own since C has no optional arguments; and section 20's ask:
TfAskKind, tf_sharing_shown, tf_sharing_open; SDK 4.0.2 adds 24.13's cause on the app's error: TfErrorCause,
tf_last_error_cause).

  python test/api_scan.py        exit 1 with what differs

What it checks, from the files themselves:
- include/tajweedformer.h is the only public header; it includes nothing but <stddef.h>, names no engine type and
  no zf_ name, and no name of the removed scorer, settings or streams.
- Its types are exactly the list below: the 22 types of the naming table without Microphone, plus TfModelOptions,
  TfPrefetchOptions and TfUnits, plus TfConsentRequest (the C SDK has no sharing UI: the app's own sheet gets
  this struct). A type added or removed fails here, so the surface cannot grow back unnoticed.
- Its functions are exactly the list below, and the shared library exports exactly them: the Windows .def file
  and the Linux version script name the same functions.
- One sheet for every app: no member, parameter, enum or constant of
  the header is named audience, children, guardian or guardian_confirmed (in any case, or as part of a name), and
  its comments do not mention them either. tf_sharing_answer takes the client and the answer, nothing else.
- The command-line tool and the example use the public header only.
- docs/api.md names every public function and type.
"""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
header = (root / "include" / "tajweedformer.h").read_text(encoding="utf-8")
bad = []

TYPES = """TfClient TfModel TfSession TfHeads TfBackend TfClientOptions TfClientOptionsExt TfSessionOptions TfChunk TfDecoded TfToken TfWord
TfFrame TfHead TfEvent TfTokens TfCalibration TfReference TfClip TfSharing TfDiagnosis TfError
TfModelOptions TfPrefetchOptions TfUnits TfConsentRequest TfRecitedOutcome TfRecitedAyah TfAskKind TfErrorCause""".split()

FUNCTIONS = """tf_version tf_last_error_code tf_last_error tf_last_error_fix tf_last_error_cause tf_error_name tf_error_fix tf_error_docs
tf_decoded_json tf_heads_count tf_heads_name tf_heads_get tf_heads_greedy tf_heads_decoded
tf_client_create tf_client_create2 tf_client_destroy tf_client_model tf_client_prefetch tf_client_storage_json tf_client_remove_models
tf_client_diagnose tf_client_diagnose_json tf_client_report_checks tf_client_background tf_client_foreground
tf_model_close tf_model_id tf_model_version tf_model_build tf_model_info_json tf_model_chunk_ms
tf_model_tokens tf_model_token_count tf_model_token_name tf_model_token_id
tf_model_calibration_json tf_model_calibration tf_model_calibration_number
tf_model_reference tf_reference_is_downloaded tf_reference_fetch tf_reference_units
tf_model_sample_clip_count tf_model_sample_clip tf_model_backend
tf_session_create tf_session_listen tf_session_push tf_session_finish tf_session_heads tf_session_cancel
tf_session_backend tf_session_report_recited tf_session_report_recited_outcome tf_session_destroy
tf_sharing_status tf_sharing_is_available tf_sharing_is_on tf_sharing_set_on tf_sharing_ask tf_sharing_answer
tf_sharing_copy_json tf_sharing_feed tf_sharing_items_json tf_sharing_summary_json tf_sharing_delete
tf_sharing_recovery_code tf_sharing_set_wording tf_sharing_shown tf_sharing_open""".split()

# names of what 3.0 removed (SPEC sections 3 and 8, NAMING section 1): none may be left in the public header
REMOVED = ["tf_score", "tf_stream", "TfStream", "TfResult", "TfWordScore", "tf_result", "tf_settings", "_default_options",
           "TF_ERR_SETTINGS", "TF_ERR_CALIBRATION", "TF_HEADS_SCORER", "remote_json", "debug_build", "expected_tokens",
           "expected_words", "ayah_count", "verdict", "preset", "TfSegment", "TfTopK", "tf_topk", "TfSessionStats",
           "tf_client_launch", "tf_client_apply_config", "tf_download_model", "TfDownloadOptions", "TfShareSession",
           "zf_platform", "struct_size", "tajweed block", "tf_free"]

includes = re.findall(r"^\s*#\s*include\s+(\S+)", header, re.M)
# <stdint.h> from 4.0.1: TfClientOptionsExt starts with a uint32_t
if includes != ["<stddef.h>", "<stdint.h>"]:
    bad.append(f"tajweedformer.h includes {includes}, want only <stddef.h> and <stdint.h>")
for m in re.finditer(r"\b(zf_\w+|Zf[A-Z]\w+|ZF_[A-Z0-9_]+)\b", header):
    bad.append(f"an engine name in the public header: {m.group(1)}")
for name in REMOVED:
    if name in header:
        bad.append(f"a removed name in the public header: {name}")
others = sorted(p.name for p in (root / "include").glob("*.h") if p.name not in ("tajweedformer.h", "zf_gemm_vnni.h", "zf_gemm_cuda.h"))
if others:
    bad.append(f"other headers in include/: {others}")

types = re.findall(r"^typedef\s+(?:struct|enum|union)?\s*\w*\s*(?:\{[^}]*\})?\s*(\w+)\s*;", header, re.M | re.S)
if sorted(types) != sorted(TYPES):
    bad.append(f"public types differ: extra {sorted(set(types) - set(TYPES))}, missing {sorted(set(TYPES) - set(types))}")
if re.search(r"^typedef\s+[^;{]*\(\s*\*", header, re.M):
    bad.append("a function pointer typedef in the public header (callbacks are spelled in their struct)")

functions = re.findall(r"^TF_API\s+[^;(]*?\b(\w+)\s*\(", header, re.M)
if sorted(functions) != sorted(FUNCTIONS):
    bad.append(f"public functions differ: extra {sorted(set(functions) - set(FUNCTIONS))}, missing {sorted(set(FUNCTIONS) - set(functions))}")
if len(set(functions)) != len(functions):
    bad.append("a function is declared twice")

deff = (root / "src" / "tf" / "tajweedformer.def").read_text(encoding="utf-8").split("EXPORTS", 1)[1].split()
if sorted(deff) != sorted(functions):
    bad.append(f"tajweedformer.def differs from the header: extra {sorted(set(deff) - set(functions))}, missing {sorted(set(functions) - set(deff))}")
mapf = (root / "src" / "tf" / "tajweedformer.map").read_text(encoding="utf-8")
exported = re.findall(r"^\s+(\w+);", mapf.split("local:", 1)[0], re.M)
if sorted(exported) != sorted(functions) or "*" in mapf.split("local:", 1)[0]:
    bad.append(f"tajweedformer.map differs from the header: extra {sorted(set(exported) - set(functions))}, missing {sorted(set(functions) - set(exported))}")

# SDK 4.0: no audience option and no guardian variant of the sheet.
# The 3.x names (TfClientOptions.audience, TfConsentRequest.guardian, TfSharing.guardian, the guardian_confirmed
# argument and the guardian_not_confirmed reason of the other SDKs) stay gone: no name of the header holds one of
# these words, and no comment speaks of them.
ONE_SHEET_GONE = re.compile(r"audience|children|guardian", re.I)
code = re.sub(r"/\*.*?\*/", " ", header, flags=re.S)
code = re.sub(r"//[^\n]*", " ", code)
for name in sorted(set(re.findall(r"\b[A-Za-z_]\w*\b", code))):
    if ONE_SHEET_GONE.search(name):
        bad.append(f"include/tajweedformer.h names {name}: 4.0 has no audience option and no guardian variant (section 26)")
for m in ONE_SHEET_GONE.finditer(header):
    line = header.count("\n", 0, m.start()) + 1
    bad.append(f"include/tajweedformer.h line {line} mentions {m.group(0)}: 4.0 has one sheet for every app (section 26)")
answer = re.search(r"^TF_API\s+int\s+tf_sharing_answer\s*\(([^)]*)\)", header, re.M)
if not answer or [p.split()[-1].lstrip("*") for p in answer.group(1).split(",")] != ["c", "share"]:
    bad.append(f"tf_sharing_answer takes {answer.group(1) if answer else '(not found)'}: want (TfClient *c, int share), nothing else")

# SDK 3.1.0 (SPEC section 15): no public way to hand the SDK a model. The call that opened a folder and the
# option that named another signing key are gone from the header and from the exports, and stay gone.
for gone in ("tf_model_open", "trusted_key_hex"):
    if re.search(r"\b" + gone + r"\b", header):
        bad.append(f"include/tajweedformer.h names {gone}: an app must not be able to open a model it supplies")
if "tf_model_open" in deff or "tf_model_open" in exported or any(n.startswith("tf__") for n in deff + exported):
    bad.append("the shared library exports a way to open a model folder")
for rel in ("cli/tajweedformer.c", "examples/quickstart.c"):
    text = (root / rel).read_text(encoding="utf-8")
    for inc in re.findall(r'^\s*#\s*include\s+"([^"]+)"', text, re.M):
        if inc != "tajweedformer.h":
            bad.append(f"{rel} includes {inc}: the tool and the example use the public header only")
    if re.search(r"\b(zf_\w+|tf__\w+)\b", text):
        bad.append(f"{rel} uses a name that is not public")

api_md = root / "docs" / "api.md"
if api_md.exists():
    doc = api_md.read_text(encoding="utf-8")
    missing = [n for n in functions + TYPES if not re.search(r"\b" + re.escape(n) + r"\b", doc)]
    if missing:
        bad.append(f"docs/api.md does not name: {missing}")
    stale = [n for n in set(re.findall(r"\b(tf_[a-z0-9_]+)\b", doc)) if n not in functions and not n.startswith("tf_tests")]
    if stale:
        bad.append(f"docs/api.md names functions that are not public: {sorted(stale)}")
else:
    bad.append("docs/api.md is missing")

if bad:
    print("\n".join(bad))
    sys.exit(1)
print(f"public surface: {len(functions)} functions, {len(types)} types; header, .def and .map agree; no engine or removed name; no audience, "
      "children or guardian name (section 26); docs/api.md covers it")
