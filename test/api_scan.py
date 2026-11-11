"""The public surface of the C SDK is the naming table's and nothing else (SDK 3.0.0).

  python test/api_scan.py        exit 1 with what differs

What it checks, from the files themselves:
- include/tajweedformer.h is the only public header; it includes nothing but <stddef.h>, names no engine type and
  no zf_ name, and no name of the removed scorer, settings or streams.
- Its types are exactly the list below: the 22 types of the naming table without Microphone, plus TfModelOptions,
  TfPrefetchOptions and TfUnits, plus TfConsentRequest (the C SDK has no sharing UI: the app's own sheet gets
  this struct). A type added or removed fails here, so the surface cannot grow back unnoticed.
- Its functions are exactly the list below, and the shared library exports exactly them: the Windows .def file
  and the Linux version script name the same functions.
- The command-line tool and the example use the public header only.
- docs/api.md names every public function and type.
"""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
header = (root / "include" / "tajweedformer.h").read_text(encoding="utf-8")
bad = []

TYPES = """TfClient TfModel TfSession TfHeads TfBackend TfClientOptions TfSessionOptions TfChunk TfDecoded TfToken TfWord
TfFrame TfHead TfEvent TfTokens TfCalibration TfReference TfClip TfSharing TfAnalytics TfDiagnosis TfError
TfModelOptions TfPrefetchOptions TfUnits TfConsentRequest""".split()

FUNCTIONS = """tf_version tf_last_error_code tf_last_error tf_last_error_fix tf_error_name tf_error_fix tf_error_docs
tf_decoded_json tf_heads_count tf_heads_name tf_heads_get tf_heads_greedy tf_heads_decoded
tf_client_create tf_client_destroy tf_client_model tf_client_prefetch tf_client_storage_json tf_client_remove_models
tf_client_diagnose tf_client_diagnose_json tf_client_report_checks tf_client_background tf_client_foreground
tf_model_open tf_model_close tf_model_id tf_model_version tf_model_build tf_model_info_json tf_model_chunk_ms
tf_model_tokens tf_model_token_count tf_model_token_name tf_model_token_id
tf_model_calibration_json tf_model_calibration tf_model_calibration_number
tf_model_reference tf_reference_is_downloaded tf_reference_fetch tf_reference_units
tf_model_sample_clip_count tf_model_sample_clip tf_model_backend
tf_session_create tf_session_listen tf_session_push tf_session_finish tf_session_heads tf_session_cancel
tf_session_backend tf_session_report_recited tf_session_destroy
tf_sharing_status tf_sharing_is_available tf_sharing_is_on tf_sharing_set_on tf_sharing_ask tf_sharing_answer
tf_sharing_copy_json tf_sharing_feed tf_sharing_items_json tf_sharing_summary_json tf_sharing_delete
tf_sharing_recovery_code tf_sharing_set_wording""".split()

# names of what 3.0 removed (SPEC sections 3 and 8, NAMING section 1): none may be left in the public header
REMOVED = ["tf_score", "tf_stream", "TfStream", "TfResult", "TfWordScore", "tf_result", "tf_settings", "_default_options",
           "TF_ERR_SETTINGS", "TF_ERR_CALIBRATION", "TF_HEADS_SCORER", "remote_json", "debug_build", "expected_tokens",
           "expected_words", "ayah_count", "verdict", "preset", "TfSegment", "TfTopK", "tf_topk", "TfSessionStats",
           "tf_client_launch", "tf_client_apply_config", "tf_download_model", "TfDownloadOptions", "TfShareSession",
           "zf_platform", "struct_size", "tajweed block", "tf_free"]

includes = re.findall(r"^\s*#\s*include\s+(\S+)", header, re.M)
if includes != ["<stddef.h>"]:
    bad.append(f"tajweedformer.h includes {includes}, want only <stddef.h>")
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
print(f"public surface: {len(functions)} functions, {len(types)} types; header, .def and .map agree; no engine or removed name; docs/api.md covers it")
