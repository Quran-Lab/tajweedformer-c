# Changelog

## 2.0.0 (first public release)

- TajweedFormer V4 on Windows and Linux: `tajweedformer.h`, a C library (DLL or shared object, and a
  static library) over the exact engine: verified model folders (signed manifest, every file hashed,
  layout 3), streams with every head, the decoded layer (top-1 and runner-up per frame, tokens with
  confidence) and the tajweed layer (per-site and per-word verdicts with their evidence).
- The Quran Lab platform client (register, signed configuration with the portal's tajweed block,
  anonymous telemetry, issues), resumable model downloads, and opt-in recitation sharing.
- The `tajweedformer` command-line tool: info, decode, serve, bench, download.
- x86 int8 kernels picked at run time (AVX2, AVX-VNNI, AVX-512 VNNI), each equal to the engine's own
  kernel; the V4 conformance (35 clips, every output of every chunk, every context) passes through
  the library's own API.
