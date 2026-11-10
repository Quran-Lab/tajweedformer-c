cmake_minimum_required(VERSION 3.24)
project(tajweedformer_native C CXX)

# The engine (vendor/zipformer-sdk, which becomes tajweedformer-sdk) built by its own CMake, in its
# exact configuration, with the native int8 hook linked into everything that runs it:
#   - NEON emulation on x86 (the engine's exact paths exist only in NEON form; zf/neon_emu.h)
#   - ZF_QUANT_DIVIDE=1, which exactness requires (the release build sets it itself)
#   - no floating-point contraction anywhere (the engine adds -ffp-contract=off; so do we)
# GCC (MinGW-w64 on Windows) or clang. MSVC cl.exe cannot build the engine (GNU vector extensions).
set(CMAKE_C_STANDARD 11)
# everything is position independent, so the engine and the kernels can go into the shared library
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options($<$<COMPILE_LANGUAGE:C>:-ffp-contract=off>)
# MinGW-w64 GCC keeps the stack only 16-byte aligned on Windows x64 but spills 32- and 64-byte vectors (the
# AVX2 / AVX-VNNI / AVX-512 kernels' __m256i / __m512i locals) with ALIGNED moves (GCC bug 54412): a crash on a
# CPU whose tier uses them. The assembler turns every aligned vector move into its unaligned form (binutils
# 2.38 or newer; no change in the arithmetic). clang realigns the stack itself and needs nothing.
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
  add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wa,-muse-unaligned-vector-move>)
endif()

# As a subproject (FetchContent or add_subdirectory in an app's own CMake project): nothing here is built unless
# the app links it, and the engine's release configuration is the default, since the app ships what it builds.
# A top-level build (the presets, CI) builds everything and sets the configuration itself.
set(_tf_release_default OFF)
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set_property(DIRECTORY PROPERTY EXCLUDE_FROM_ALL ON)
  set(_tf_release_default ON)
endif()
option(ZF_NATIVE_RELEASE "The engine's release configuration (ZF_RELEASE: exact only, ZF_* environment ignored)" ${_tf_release_default})
set(ZF_NEON_EMU ON CACHE BOOL "Emulate ARMv7 NEON in portable C (required on x86)")
if(ZF_NATIVE_RELEASE)
  set(ZF_RELEASE ON CACHE BOOL "" FORCE)
else()
  add_compile_definitions(ZF_QUANT_DIVIDE=1)
endif()

set(SDK "${CMAKE_CURRENT_SOURCE_DIR}/vendor/zipformer-sdk")
if(NOT EXISTS "${SDK}/engine/src/kernels.c")
  execute_process(COMMAND git submodule update --init WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
# The engine looks for the ONNX Runtime C API headers in its own third_party, then in ours.
# EXCLUDE_FROM_ALL: only the engine targets this project uses are built (its many development
# harnesses are not all buildable on every Windows toolchain, and are not needed here).
add_subdirectory("${SDK}" engine EXCLUDE_FROM_ALL)

# --- the native int8 kernels: AVX-512 VNNI, AVX-VNNI, AVX2, scalar; picked at run time ---
# Compiled at baseline ISA: each kernel carries its own target attribute and a cpuid + xgetbv check
# selects it, so one binary runs on any x86-64 and uses the widest exact tier present.
# Each x86 tier is its own translation unit with its own -m flags (the rest stays at the baseline),
# which every GCC and clang, including clang on the Microsoft runtime, accepts.
add_library(zf_gemm_vnni STATIC src/zf_gemm_vnni.c)
target_include_directories(zf_gemm_vnni PUBLIC include)
target_compile_options(zf_gemm_vnni PRIVATE -O3 -Wall -Wextra)
if(WIN32)
  target_compile_definitions(zf_gemm_vnni PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
  target_sources(zf_gemm_vnni PRIVATE src/int8_avx512.c src/int8_avxvnni.c src/int8_avx2.c)
  set_source_files_properties(src/int8_avx512.c PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512bw;-mavx512vnni")
  set_source_files_properties(src/int8_avxvnni.c PROPERTIES COMPILE_OPTIONS "-mavx2;-mavxvnni")
  set_source_files_properties(src/int8_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx2")
endif()

# --- the hook: the engine's weak zf_s8_linear_hook, made strong ---
# An OBJECT library, linked directly into each executable: a strong definition inside a static
# archive is never pulled (the engine's weak default already satisfies the reference), so the decode
# would silently stay on the engine's kernel. Linking an object library puts the object itself on the
# link line, where the strong definition wins. test/hooked_check proves it did.
add_library(zf_vnni_hook OBJECT src/zf_vnni_hook.c)
target_include_directories(zf_vnni_hook PUBLIC include "${SDK}/engine/include")
target_compile_options(zf_vnni_hook PRIVATE -O3 -Wall -Wextra)
target_link_libraries(zf_vnni_hook PUBLIC zf_gemm_vnni)

# The engine's own harnesses, with the hook: bench/conformance.sh (v3.2) and bench/v4_conformance.sh
# (V4) then run the whole decode on the native kernels against the engine's expected files.
# zf_tajweed_check is the engine's MinGW / Unix harness; it is not built against the Microsoft runtime.
add_custom_target(zf_harnesses ALL)
foreach(t zf_set_rt zf_stream_check zf_stream_concurrent zf_tajweed_check zf_state_check zf_info_check)
  if(TARGET ${t})
    target_link_libraries(${t} PRIVATE zf_vnni_hook zf_gemm_vnni)
    add_dependencies(zf_harnesses ${t})
  endif()
endforeach()

# --- the streaming decoder: audio in, phonemes out ---
add_executable(zf_decode runner/zf_decode.c)
target_compile_options(zf_decode PRIVATE -O2 -Wall -Wextra)
target_link_libraries(zf_decode PRIVATE zf_stream zf_vnni_hook zf_gemm_vnni)


# --- the TajweedFormer library: include/tajweedformer.h (tf_*) over the engine's zf_tajweed.h ---
# One shared library (tajweedformer.dll / libtajweedformer.so) exporting exactly tf_* and zf_tajweed_*
# (a .def file on Windows, a version script on Linux), and the same as a static library.
if(TARGET zf_tajweed_engine)
  # the engine's glue uses fopen/snprintf; the Microsoft runtime's deprecation notes are not errors here
  if(WIN32)
    target_compile_definitions(zf_tajweed_engine PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  set_property(TARGET zf_tajweed_engine zf_stream zf_rt zf_ort_ops zf_engine PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()
set(TF_SOURCES src/tf/tf_util.c src/tf/tf_crypto.c src/tf/tf_model.c src/tf/tf_stream.c src/tf/tf_decode.c src/tf/tf_topk.c
    src/tf/tf_score.c src/tf/tf_http.c src/tf/tf_download.c src/tf/tf_client.c src/tf/tf_sharing.c
    src/tf/tf_copy_deck.c src/tf/tf_ogg.c src/tf/tf_devicekey.c src/tf/tf_inflate.c src/tf/tf_share.c src/tf/tf_share_audio.c)
# libopus for the shared-session upload (cmake/opus.cmake: the pinned 1.6.1 release, float, plain C)
include(cmake/opus.cmake)
function(tf_library name kind)
  add_library(${name} ${kind} ${TF_SOURCES} $<TARGET_OBJECTS:zf_vnni_hook>)
  target_include_directories(${name} PUBLIC include "${SDK}/tajweed/include" PRIVATE src/tf "${SDK}/engine/include")
  target_compile_options(${name} PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(${name} PRIVATE zf_tajweed_engine zf_gemm_vnni tf_opus)
  set_target_properties(${name} PROPERTIES C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
  if(WIN32)
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${name} PRIVATE winhttp bcrypt crypt32)
  else()
    find_package(CURL REQUIRED)
    find_package(OpenSSL 3 REQUIRED)
    target_link_libraries(${name} PRIVATE CURL::libcurl OpenSSL::Crypto m pthread)
  endif()
endfunction()
tf_library(tajweedformer SHARED)
target_compile_definitions(tajweedformer PRIVATE TF_BUILD_DLL=1)
if(WIN32)
  target_sources(tajweedformer PRIVATE src/tf/tajweedformer.def)
  set_target_properties(tajweedformer PROPERTIES PREFIX "")
else()
  target_link_options(tajweedformer PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/tf/tajweedformer.map")
endif()
tf_library(tajweedformer_static STATIC)

# the command-line tool and server tier
add_executable(tajweedformer_cli cli/tajweedformer.c)
set_target_properties(tajweedformer_cli PROPERTIES OUTPUT_NAME tajweedformer)
target_compile_options(tajweedformer_cli PRIVATE -O2 -Wall -Wextra)
target_link_libraries(tajweedformer_cli PRIVATE tajweedformer)
if(WIN32)
  target_compile_definitions(tajweedformer_cli PRIVATE TF_USE_DLL=1 _CRT_SECURE_NO_WARNINGS)
endif()

# the README's quick start, built with everything else so the code in the README always compiles
# (test/quickstart_check.py keeps the two texts equal). Only in a build of this repository itself: an app that
# takes it as a subproject has its own program, which may well be called quickstart too.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(quickstart examples/quickstart.c)
  target_compile_options(quickstart PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(quickstart PRIVATE tajweedformer)
  if(WIN32)
    target_compile_definitions(quickstart PRIVATE TF_USE_DLL=1 _CRT_SECURE_NO_WARNINGS)
  endif()
endif()

# An app that takes this as a subproject gets the libraries and the command-line tool. What follows is this
# repository's own: its tests, the kernel checks and the optional CUDA tier.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  return()
endif()

# the library's own tests (through the public API only; tf_tests also reads its internal JSON helpers)
add_executable(tf_tests test/tf_tests.c)
target_include_directories(tf_tests PRIVATE src/tf)
target_compile_options(tf_tests PRIVATE -O2 -Wall -Wextra)
target_link_libraries(tf_tests PRIVATE tajweedformer_static zf_tajweed_engine zf_gemm_vnni tf_opus)
if(WIN32)
  target_compile_definitions(tf_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(tf_tests PRIVATE winhttp bcrypt crypt32)
else()
  target_link_libraries(tf_tests PRIVATE CURL::libcurl OpenSSL::Crypto m pthread)
endif()

enable_testing()
foreach(t vnni_check dropin_check hooked_check cpu_detect)
  add_executable(${t} test/${t}.c)
  target_include_directories(${t} PRIVATE include "${SDK}/engine/include")
  add_test(NAME ${t} COMMAND ${t})
endforeach()
# the library's tests that need no model data (only test/data): the Ogg muxer and Opus encoder vectors, the deletion
# crypto, the shared-session upload against a stub server, the platform client against recorded exchanges, and the
# 2.0.1 bitrate ladder, consent wording and telemetry deltas, (2.0.2) the real signed V4 manifests, and (2.0.3) takes
# in a row sharing one recording. Each run starts from an empty work directory (the fixture below). The model tests
# (conformance, scorer vectors, settings) need the release data: test/run_tests.sh.
set(TF_WORK "${CMAKE_CURRENT_BINARY_DIR}/tf_test_work")
set(TF_VEC "${CMAKE_CURRENT_SOURCE_DIR}/test/data/sharing_vectors")
add_test(NAME tf_work_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${TF_WORK}")
set_tests_properties(tf_work_clean PROPERTIES FIXTURES_SETUP tf_work)
add_test(NAME tf_ogg COMMAND tf_tests ogg "${TF_VEC}/ogg" "${TF_WORK}/ogg")
add_test(NAME tf_codec COMMAND tf_tests codec "${TF_VEC}" "${TF_WORK}/codec")
add_test(NAME tf_sharing COMMAND tf_tests sharing "${TF_VEC}" "${CMAKE_CURRENT_SOURCE_DIR}/test/data/client-fixtures.json" "${TF_WORK}/sharing")
add_test(NAME tf_upload COMMAND tf_tests upload "${TF_WORK}/upload")
add_test(NAME tf_client COMMAND tf_tests client "${CMAKE_CURRENT_SOURCE_DIR}/test/data/client-fixtures.json" "${TF_WORK}/client")
add_test(NAME tf_bitrate COMMAND tf_tests bitrate "${TF_VEC}" "${TF_WORK}/bitrate")
add_test(NAME tf_consent COMMAND tf_tests consent "${TF_VEC}" "${CMAKE_CURRENT_SOURCE_DIR}/test/data/client-fixtures.json" "${TF_WORK}/consent")
add_test(NAME tf_telemetry COMMAND tf_tests telemetry "${TF_WORK}/telemetry")
add_test(NAME tf_sdk_latest COMMAND tf_tests sdk_latest "${CMAKE_CURRENT_SOURCE_DIR}/test/data/sdk_latest_configs.json" "${TF_WORK}/sdk_latest")
add_test(NAME tf_manifest COMMAND tf_tests manifest "${CMAKE_CURRENT_SOURCE_DIR}/test/data" "${TF_WORK}/manifest")
add_test(NAME tf_takes COMMAND tf_tests takes "${TF_VEC}" "${TF_WORK}/takes")
set_tests_properties(tf_ogg tf_codec tf_sharing tf_upload tf_client tf_bitrate tf_consent tf_telemetry tf_sdk_latest tf_manifest tf_takes PROPERTIES
                     FIXTURES_REQUIRED tf_work LABELS library TIMEOUT 600)
target_link_libraries(vnni_check PRIVATE zf_gemm_vnni)
target_link_libraries(cpu_detect PRIVATE zf_gemm_vnni)
# dropin compares the native kernels to the engine's OWN kernel, so it links the engine WITHOUT the hook
target_link_libraries(dropin_check PRIVATE zf_engine zf_gemm_vnni)
# hooked runs the engine's entry points through the hook
target_link_libraries(hooked_check PRIVATE zf_engine zf_vnni_hook zf_gemm_vnni)

# --- optional CUDA int8 tier (needs the CUDA toolkit) ---
option(ZF_CUDA "Build the CUDA int8 GEMM tier" OFF)
if(ZF_CUDA)
  enable_language(CUDA)
  add_library(zf_gemm_cuda STATIC cuda/zf_gemm_cuda.cu)
  target_include_directories(zf_gemm_cuda PUBLIC include)
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set_target_properties(zf_gemm_cuda PROPERTIES CUDA_ARCHITECTURES "75;80;86;89;90")
  endif()
  message(STATUS "CUDA int8 tier: enabled (dp4a; verify against the CPU kernel on the target GPU)")
endif()

message(STATUS "tajweedformer-native: engine with the native int8 hook (AVX-512 VNNI / AVX-VNNI / AVX2); run ctest")
