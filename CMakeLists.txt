cmake_minimum_required(VERSION 3.24)
project(tajweedformer_native C)

# SDK 4.0: the TajweedFormer C library (include/tajweedformer.h, src/tf) over Quran Lab's closed Core, which is
# linked as a prebuilt library and never built here (PROTECTED_MODEL.md 18.5.1): the engine, the model formats, the
# keys and the recited-ayah detector are inside it. This repository holds the wrappers only: the API, networking,
# the install key store, telemetry and sharing.
#
#   QLCORE_DIR   a folder with the Core's shipped binary for this target: qlcore.dll (Windows x64, MinGW-w64 GCC) or
#                qlcore.so (Linux x86-64). core/qlcore.h is its interface (copied unchanged; core/CORE_VERSION names
#                the Core commit it came from).
#
# Toolchain (tajweedformer-sdk docs/windows.md): MinGW-w64 GCC 13 (Strawberry Perl's c\bin) and Ninja on Windows;
# GCC or clang on Linux. tools/build.py drives a whole build, the tests and the shipped files.
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(QLCORE_DIR "" CACHE PATH "the Core's prebuilt library folder (qlcore.dll or qlcore.so)")
if(WIN32)
  find_file(QLCORE_BIN NAMES qlcore.dll PATHS "${QLCORE_DIR}" NO_DEFAULT_PATH NO_CACHE)
else()
  find_file(QLCORE_BIN NAMES qlcore.so libqlcore.so PATHS "${QLCORE_DIR}" NO_DEFAULT_PATH NO_CACHE)
endif()
if(NOT QLCORE_BIN)
  message(FATAL_ERROR "QLCORE_DIR (${QLCORE_DIR}) has no Core binary for this target: build or download quranlab-core first")
endif()
add_library(qlcore SHARED IMPORTED GLOBAL)
set_target_properties(qlcore PROPERTIES IMPORTED_LOCATION "${QLCORE_BIN}" INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/core")
if(WIN32)
  # MinGW's linker takes a DLL as its own import library
  set_target_properties(qlcore PROPERTIES IMPORTED_IMPLIB "${QLCORE_BIN}")
else()
  # linked by its path: the Core's SONAME is qlcore.so, so that is what the library and the tests NEED, found beside
  # them ($ORIGIN) or through the tests' rpath (IMPORTED_NO_SONAME would link -lqlcore, a libqlcore.so the Core does not
  # ship as)
  set_target_properties(qlcore PROPERTIES IMPORTED_SONAME "qlcore.so")
endif()

# As a subproject: nothing is built unless the app links it.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set_property(DIRECTORY PROPERTY EXCLUDE_FROM_ALL ON)
endif()

set(TF_SOURCES src/tf/tf_util.c src/tf/tf_crypto.c src/tf/tf_model.c src/tf/tf_heads.c src/tf/tf_session.c src/tf/tf_speech.c
    src/tf/tf_decode.c src/tf/tf_topk.c src/tf/tf_rank.c src/tf/tf_reference.c src/tf/tf_http.c src/tf/tf_download.c src/tf/tf_store.c
    src/tf/tf_client.c src/tf/tf_api.c src/tf/tf_sharing.c src/tf/tf_copy_deck.c src/tf/tf_ogg.c src/tf/tf_devicekey.c
    src/tf/tf_inflate.c src/tf/tf_share.c src/tf/tf_share_audio.c src/tf/tf_ikey.c src/tf/tf_platform.c src/tf/tf_lease.c
    src/tf/tf_recited.c src/tf/tf_learner.c src/tf/tf_flac.c src/tf/tf_share_ask.c src/tf/tf_report.c src/tf/tf_endpoints.c
    src/tf/tf_qos.c)
# libopus for the shared-session upload (cmake/opus.cmake: the pinned 1.6.1 release, float, plain C)
include(cmake/opus.cmake)
set(TF_WARN -O2 -Wall -Wextra -Wvla)
function(tf_library name kind)
  add_library(${name} ${kind} ${TF_SOURCES})
  target_include_directories(${name} PUBLIC include PRIVATE src/tf)
  # no compiler ident, each function in its own section (the unused dropped), no frame-pointer chains: as the Core
  target_compile_options(${name} PRIVATE ${TF_WARN} -fno-ident -ffunction-sections -fdata-sections)
  target_link_libraries(${name} PUBLIC qlcore PRIVATE tf_opus)
  set_target_properties(${name} PROPERTIES C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
  if(WIN32)
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${name} PRIVATE winhttp bcrypt ncrypt crypt32 advapi32)
  else()
    find_package(CURL REQUIRED)
    find_package(OpenSSL 3 REQUIRED)
    target_link_libraries(${name} PRIVATE CURL::libcurl OpenSSL::Crypto m pthread)
  endif()
endfunction()
tf_library(tajweedformer SHARED)
target_compile_definitions(tajweedformer PRIVATE TF_BUILD_DLL=1)
if(WIN32)
  # exactly the functions of include/tajweedformer.h (test/api_scan.py keeps the .def, the map and the header equal)
  target_sources(tajweedformer PRIVATE src/tf/tajweedformer.def)
  set_target_properties(tajweedformer PROPERTIES PREFIX "")
  target_link_options(tajweedformer PRIVATE -Wl,--gc-sections -Wl,--no-insert-timestamp -static-libgcc)
else()
  # --exclude-libs: no symbol of a static archive (libopus, a static OpenSSL 3) in the dynamic symbol table, beside the
  # version script, so the system's libcurl and its OpenSSL 1.1 never bind to them
  target_link_options(tajweedformer PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/tf/tajweedformer.map" -Wl,--gc-sections
                      -Wl,--build-id=none "-Wl,-rpath,$ORIGIN" -Wl,--exclude-libs,ALL)
  # only $ORIGIN, as shipped (CMake would add the build machine's Core folder): the Core goes beside the library
  set_target_properties(tajweedformer PROPERTIES SKIP_BUILD_RPATH ON)
  add_custom_command(TARGET tajweedformer POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${QLCORE_BIN}"
                     "$<TARGET_FILE_DIR:tajweedformer>/qlcore.so")
endif()
tf_library(tajweedformer_static STATIC)

# the command-line tool
add_executable(tajweedformer_cli cli/tajweedformer.c)
set_target_properties(tajweedformer_cli PROPERTIES OUTPUT_NAME tajweedformer)
target_compile_options(tajweedformer_cli PRIVATE -O2 -Wall -Wextra)
target_link_libraries(tajweedformer_cli PRIVATE tajweedformer)
if(WIN32)
  target_compile_definitions(tajweedformer_cli PRIVATE TF_USE_DLL=1 _CRT_SECURE_NO_WARNINGS)
else()
  target_link_options(tajweedformer_cli PRIVATE "-Wl,-rpath,$ORIGIN")
  set_target_properties(tajweedformer_cli PROPERTIES SKIP_BUILD_RPATH ON)
endif()

# tajweedformer-eval: the model on a developer's own recordings, through the public API only (cli/tajweedformer_eval.c)
add_executable(tajweedformer_eval cli/tajweedformer_eval.c)
set_target_properties(tajweedformer_eval PROPERTIES OUTPUT_NAME tajweedformer-eval)
target_compile_options(tajweedformer_eval PRIVATE -O2 -Wall -Wextra)
target_link_libraries(tajweedformer_eval PRIVATE tajweedformer)
if(WIN32)
  target_compile_definitions(tajweedformer_eval PRIVATE TF_USE_DLL=1 _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(tajweedformer_eval PRIVATE shell32)
else()
  target_link_libraries(tajweedformer_eval PRIVATE m)
  target_link_options(tajweedformer_eval PRIVATE "-Wl,-rpath,$ORIGIN")
  set_target_properties(tajweedformer_eval PROPERTIES SKIP_BUILD_RPATH ON)
endif()

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  return()
endif()

# the example app, whose marked region is the README's quick start (test/quickstart_check.py)
add_executable(quickstart examples/quickstart.c)
target_compile_options(quickstart PRIVATE -O2 -Wall -Wextra)
target_link_libraries(quickstart PRIVATE tajweedformer)
if(WIN32)
  target_compile_definitions(quickstart PRIVATE TF_USE_DLL=1 _CRT_SECURE_NO_WARNINGS)
endif()

# ------------------------------------------------------------------ tests
# The Core is a DLL beside every test program (Windows finds it there), with MinGW's winpthread it needs.
enable_testing()
set(TF_WORK "${CMAKE_CURRENT_BINARY_DIR}/tf_test_work")
set(TF_DATA "${CMAKE_CURRENT_SOURCE_DIR}/test/data")
set(TF_VEC "${TF_DATA}/sharing_vectors")
set(TF_CONTRACT "" CACHE PATH "a quranlab-contract checkout with sdk_4.0.0 (its vectors and tools/protected40.py); empty: those tests are not registered")
find_package(Python3 COMPONENTS Interpreter)
function(tf_test_exe name src)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE src/tf)
  target_compile_options(${name} PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(${name} PRIVATE tajweedformer_static tf_opus qlcore)
  if(WIN32)
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${name} PRIVATE winhttp bcrypt ncrypt crypt32 advapi32)
  else()
    target_link_libraries(${name} PRIVATE CURL::libcurl OpenSSL::Crypto m pthread)
    # a static OpenSSL 3 stays the program's own: the system's libcurl and libssl 1.1 must not bind to it
    target_link_options(${name} PRIVATE "-Wl,-rpath,${QLCORE_DIR}" -Wl,--exclude-libs,ALL)
  endif()
endfunction()
tf_test_exe(tf_tests test/tf_tests.c)
tf_test_exe(tf40_tests test/tf40_tests.c)
# 4.0.1: the loopback stand-in server of tf40_tests loopback (test/tf_loopback.c: 127.0.0.1 only)
target_sources(tf40_tests PRIVATE test/tf_loopback.c)
if(WIN32)
  target_link_libraries(tf40_tests PRIVATE ws2_32)
endif()
if(WIN32)
  get_filename_component(_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
  foreach(t tf_tests tf40_tests tajweedformer_cli tajweedformer_eval quickstart)
    add_custom_command(TARGET ${t} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${QLCORE_BIN}" "$<TARGET_FILE_DIR:${t}>"
                       COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_cc_dir}/libwinpthread-1.dll" "$<TARGET_FILE_DIR:${t}>")
  endforeach()
endif()
add_test(NAME tf_work_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${TF_WORK}")
set_tests_properties(tf_work_clean PROPERTIES FIXTURES_SETUP tf_work TIMEOUT 120)
# 4.0: the install key store, the recited fallback, 17.9's arguments, the JSON parser's leak fix
foreach(t ikey recited outcome json_leak learner)
  add_test(NAME tf40_${t} COMMAND tf40_tests ${t} - "${TF_WORK}/tf40_${t}")
endforeach()
# tajweedformer-eval's own parts (no model): edit distance, references, WAV and FLAC (test/data/eval: test/make_eval_flac.py)
add_test(NAME tf40_eval_units COMMAND tf40_tests eval_units "${TF_DATA}/eval" "${TF_WORK}/tf40_eval_units")
# error reports (ERRORS_40.md 24): test/data/sdk_4.0.0/errors_40.json is the contract's vectors/errors_40.json at adca6f6,
# byte for byte (tools/errors40.py --check); its codes, queue, telemetry, signatures and (4.0.2, 24.13) causes through
# this library
add_test(NAME tf40_errors COMMAND tf40_tests errors "${TF_DATA}/sdk_4.0.0" "${TF_WORK}/tf40_errors")
# 4.0 with the contract's vectors (TF_CONTRACT, a quranlab-contract checkout; its test keys only): lease, clock,
# container and attest proof through this library and the Core; the whole flow against a stand-in server whose data
# test/make_flow40.py makes from the contract's reference (Python 3 with `cryptography`)
if(TF_CONTRACT)
  # The vectors and the reference (tools/protected40.py) at a pinned commit of the contract, exported by git at
  # configure, as the Core does (another agent edits the contract's working tree): c7f6e9b, spec fix 16 with
  # piece_order.json's four sequences, section 19 (vectors/sharing40) and section 20 (vectors/share_ask.json); the
  # earlier vectors are byte for byte those of fba80eb, the commit quranlab-core d128af8 pins.
  # 4.0.1: 564c144, sections 27 (app_version and app_build, tf_client_create2) and 28 (endpoint sets, four hosts a role),
  # and ERRORS_40 24.3's Core status at the model open (vectors/endpoints_40.json, errors_40.json, standby_40.json).
  set(TF_CONTRACT_COMMIT "564c144" CACHE STRING "the quranlab-contract commit whose sdk_4.0.0 vectors and tools the tests read")
  set(TF_C40 "${CMAKE_BINARY_DIR}/contract-${TF_CONTRACT_COMMIT}/sdk_4.0.0")
  # TF_CONTRACT is a checkout (the files are exported by git at the pinned commit), or a folder that already holds
  # that export (CI: no git access to the contract; tools/ci/contract_export.py writes it with a COMMIT file naming
  # the commit, which must be the pinned one)
  if(NOT EXISTS "${TF_CONTRACT}/.git" AND EXISTS "${TF_CONTRACT}/COMMIT")
    file(STRINGS "${TF_CONTRACT}/COMMIT" _have LIMIT_COUNT 1)
    string(SUBSTRING "${_have}" 0 7 _have7)
    if(NOT _have7 STREQUAL TF_CONTRACT_COMMIT)
      message(FATAL_ERROR "TF_CONTRACT (${TF_CONTRACT}) is an export of ${_have}, not of ${TF_CONTRACT_COMMIT}")
    endif()
    set(_export ON)
  else()
    set(_export OFF)
  endif()
  foreach(f vectors/clock.json vectors/container.json vectors/hkdf.json vectors/install_cert.json vectors/keyattest.json vectors/lease.json
            vectors/manifest_40.json vectors/mark.json vectors/piece.json vectors/proof.json vectors/refusal_signed.json vectors/web_lease.json
            vectors/piece_order.json vectors/share_ask.json vectors/sharing40/flac_cases.json vectors/sharing40/pcm16.json
            vectors/sharing40/device_model.json vectors/sharing40/upload_start.json vectors/sharing40/flac16k/expected.json
            vectors/sharing40/flac16k/seg-00001.oga vectors/sharing40/flac16k/seg-00002.oga vectors/sharing40/flac16k/seg-00003.oga
            vectors/sharing40/flac16k/seg-00004.oga vectors/grace.json tools/protected40.py tools/share_ask.py tools/sharing40.py
            vectors/endpoints_40.json vectors/errors_40.json vectors/standby_40.json)
    get_filename_component(_d "${TF_C40}/${f}" DIRECTORY)
    file(MAKE_DIRECTORY "${_d}")
    if(_export)
      if(EXISTS "${TF_CONTRACT}/sdk_4.0.0/${f}")
        file(COPY_FILE "${TF_CONTRACT}/sdk_4.0.0/${f}" "${TF_C40}/${f}")
        set(_rc 0)
      else()
        set(_rc 1)
      endif()
    else()
      execute_process(COMMAND git -C "${TF_CONTRACT}" show "${TF_CONTRACT_COMMIT}:sdk_4.0.0/${f}" OUTPUT_FILE "${TF_C40}/${f}" RESULT_VARIABLE _rc)
    endif()
    if(NOT _rc EQUAL 0)
      message(WARNING "cannot export sdk_4.0.0/${f} at ${TF_CONTRACT_COMMIT} from ${TF_CONTRACT}")
    endif()
  endforeach()
  # the error catalogue at the same commit (sdk_3.0.0/errors.json): tf_errors compares the embedded bytes with it
  set(TF_C30_ERRORS "${CMAKE_BINARY_DIR}/contract-${TF_CONTRACT_COMMIT}/sdk_3.0.0/errors.json")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/contract-${TF_CONTRACT_COMMIT}/sdk_3.0.0")
  if(_export)
    if(EXISTS "${TF_CONTRACT}/sdk_3.0.0/errors.json")
      file(COPY_FILE "${TF_CONTRACT}/sdk_3.0.0/errors.json" "${TF_C30_ERRORS}")
    endif()
  else()
    execute_process(COMMAND git -C "${TF_CONTRACT}" show "${TF_CONTRACT_COMMIT}:sdk_3.0.0/errors.json" OUTPUT_FILE "${TF_C30_ERRORS}" RESULT_VARIABLE _rc)
  endif()
  set(TF_VEC40 "${TF_C40}/vectors")
  foreach(t lease clock container attest flac share_ask qos)
    add_test(NAME tf40_${t} COMMAND tf40_tests ${t} "${TF_VEC40}" "${TF_WORK}/tf40_${t}")
  endforeach()
  if(Python3_FOUND)
    add_test(NAME tf40_flow_data COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/test/make_flow40.py" --contract "${TF_C40}"
             --out "${TF_WORK}/flow40")
    set_tests_properties(tf40_flow_data PROPERTIES FIXTURES_SETUP tf_flow40 FIXTURES_REQUIRED tf_work TIMEOUT 120)
    add_test(NAME tf40_flow COMMAND tf40_tests flow "${TF_WORK}/flow40" "${TF_WORK}/tf40_flow")
    add_test(NAME tf40_lanes COMMAND tf40_tests lanes "${TF_WORK}/flow40" "${TF_WORK}/tf40_lanes")
    add_test(NAME tf40_switches COMMAND tf40_tests switches "${TF_WORK}/flow40" "${TF_WORK}/tf40_switches")
    add_test(NAME tf40_share_moments COMMAND tf40_tests share_moments "${TF_WORK}/flow40" "${TF_WORK}/tf40_share_moments")
    add_test(NAME tf40_share_flac COMMAND tf40_tests share_flac "${TF_WORK}/flow40" "${TF_WORK}/tf40_share_flac")
    add_test(NAME tf40_errors_flow COMMAND tf40_tests errors_flow "${TF_WORK}/flow40" "${TF_WORK}/tf40_errors_flow" "${TF_DATA}/sdk_4.0.0")
    add_test(NAME tf40_lease_clock COMMAND tf40_tests lease_clock "${TF_WORK}/flow40" "${TF_WORK}/tf40_lease_clock")
    # 4.0.1: section 27 (app_version and app_build, tf_client_create2), section 28's vectors replayed on this SDK's code,
    # and 28.8's checks on the real HTTP code against stand-in servers on 127.0.0.1
    add_test(NAME tf40_app27 COMMAND tf40_tests app27 "${TF_WORK}/flow40" "${TF_WORK}/tf40_app27")
    add_test(NAME tf40_endpoints COMMAND tf40_tests endpoints "${TF_VEC40}" "${TF_WORK}/flow40" "${TF_WORK}/tf40_endpoints")
    add_test(NAME tf40_loopback COMMAND tf40_tests loopback "${TF_WORK}/flow40" "${TF_WORK}/tf40_loopback")
    add_test(NAME tf40_grace COMMAND tf40_tests grace "${TF_WORK}/flow40" "${TF_VEC40}" "${TF_WORK}/tf40_grace")
    add_test(NAME tf40_grace_data COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/test/make_grace40.py" --contract "${TF_C40}"
             --out "${TF_WORK}/grace40")
    set_tests_properties(tf40_grace_data PROPERTIES FIXTURES_SETUP tf_grace40 FIXTURES_REQUIRED tf_work TIMEOUT 120)
    add_test(NAME tf40_grace_vectors COMMAND tf40_tests grace_vectors "${TF_VEC40}" "${TF_WORK}/grace40" "${TF_WORK}/tf40_grace_vectors")
    set_tests_properties(tf40_grace_vectors PROPERTIES FIXTURES_REQUIRED "tf_work;tf_grace40" TIMEOUT 600)
    # 4.0.2, section 33 (NO_WAIT_40.md): test/data/sdk_4.0.0/no_wait_40.json is the contract's vectors/no_wait_40.json at
    # adca6f6, byte for byte; its cases that name c against the stand-in, on a vector build with two contexts and the tj3
    # vocabulary (make_flow40.py --contexts), so a session on c24_l256 fetches that context's files
    add_test(NAME tf40_nowait_data COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/test/make_flow40.py" --contract "${TF_C40}"
             --out "${TF_WORK}/nowait40" --contexts c16_l64,c24_l256)
    set_tests_properties(tf40_nowait_data PROPERTIES FIXTURES_SETUP tf_nowait40 FIXTURES_REQUIRED tf_work TIMEOUT 120)
    add_test(NAME tf40_no_wait COMMAND tf40_tests no_wait "${TF_WORK}/flow40" "${TF_WORK}/nowait40" "${TF_DATA}/sdk_4.0.0" "${TF_WORK}/tf40_no_wait")
    set_tests_properties(tf40_no_wait PROPERTIES FIXTURES_REQUIRED "tf_work;tf_flow40;tf_nowait40" TIMEOUT 600)
    # 4.0.2 under load: the lease's retries (PROTECTED_MODEL.md 18.4.7.3) and a 429 (ENDPOINTS_40.md 28.10), on fixed clocks
    add_test(NAME tf40_load COMMAND tf40_tests load "${TF_WORK}/flow40" "${TF_WORK}/tf40_load")
    set_tests_properties(tf40_load PROPERTIES FIXTURES_REQUIRED "tf_work;tf_flow40" TIMEOUT 600)
    # 4.0.2, SPEC 16.7: one request per model file (the requests of a full install and of one cut, counted)
    add_test(NAME tf40_requests COMMAND tf40_tests requests "${TF_WORK}/flow40" "${TF_WORK}/tf40_requests")
    set_tests_properties(tf40_requests PROPERTIES FIXTURES_REQUIRED "tf_work;tf_flow40" TIMEOUT 600)
    # 4.0.2, STEERING_40.md 34.6: per-host download counts (test/data/sdk_4.0.0/steering_40.json is the contract's
    # vectors/steering_40.json at 9585201, byte for byte)
    add_test(NAME tf40_hosts COMMAND tf40_tests hosts "${TF_WORK}/nowait40" "${TF_DATA}/sdk_4.0.0" "${TF_WORK}/tf40_hosts")
    set_tests_properties(tf40_hosts PROPERTIES FIXTURES_REQUIRED "tf_work;tf_nowait40" TIMEOUT 600)
    # 4.0.2: the objects' lifetimes (a call after an object's end), the one Core per storage folder, and the stress of a
    # Client destroyed while its Model and Session are in use
    add_test(NAME tf40_lifetime COMMAND tf40_tests lifetime "${TF_WORK}/nowait40" "${TF_WORK}/tf40_lifetime")
    set_tests_properties(tf40_lifetime PROPERTIES FIXTURES_REQUIRED "tf_work;tf_nowait40" TIMEOUT 1500)
    # 4.0.3: the Core's one open model is the storage folder's (lifetime_open). It needs real model opens, so an
    # encrypted test build: TF_TESTBUILD, a folder made by quranlab-core's tools/make_test_build.py (cdn/sdk/v4.0 and
    # install_key.der; never inside a repository)
    set(TF_TESTBUILD "" CACHE PATH "an encrypted test build (make_test_build.py) for the tests that open the model")
    if(TF_TESTBUILD)
      add_test(NAME tf40_open_data COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/test/make_flow40.py" --contract "${TF_C40}"
               --out "${TF_WORK}/open40" --testbuild "${TF_TESTBUILD}")
      set_tests_properties(tf40_open_data PROPERTIES FIXTURES_SETUP tf_open40 FIXTURES_REQUIRED tf_work TIMEOUT 120)
      add_test(NAME tf40_lifetime_open COMMAND tf40_tests lifetime_open "${TF_WORK}/open40" "${TF_WORK}/tf40_lifetime_open")
      set_tests_properties(tf40_lifetime_open PROPERTIES FIXTURES_REQUIRED "tf_work;tf_open40" TIMEOUT 900)
    endif()
    set_tests_properties(tf40_flow tf40_lanes tf40_switches tf40_share_moments tf40_share_flac tf40_errors_flow tf40_lease_clock tf40_grace
                         tf40_app27 tf40_endpoints tf40_loopback PROPERTIES FIXTURES_REQUIRED "tf_work;tf_flow40" TIMEOUT 600)
  endif()
endif()
# Production pins (integration): TF_CORE_PRODUCTION names the folder of the Core release this SDK ships with
# (core/CORE_RELEASE: 4.0.0-c3, its shipped files' SHA-256). Every test above runs on the TEST release in QLCORE_DIR (the
# contract's public test root); these check the real one: its SHA-256, its version, that it exports every function of
# core/qlcore.h, and that it refuses a missing Core. Run on a release machine: the production Core is never copied to
# the CI runner.
set(TF_CORE_PRODUCTION "" CACHE PATH "the shipped folder of the production Core release core/CORE_RELEASE names (integration tests)")
if(TF_CORE_PRODUCTION)
  if(WIN32)
    set(_pin_target windows-x64)
    set(_pin_bin "${TF_CORE_PRODUCTION}/qlcore.dll")
  else()
    set(_pin_target linux-x64)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
      set(_pin_target linux-arm64)
    endif()
    set(_pin_bin "${TF_CORE_PRODUCTION}/qlcore.so")
  endif()
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/core/CORE_RELEASE" _pin_line REGEX "^${_pin_target} ")
  string(REGEX REPLACE "^[^ ]+ [^ ]+ ([0-9a-f]+)$" "\\1" _pin_sha "${_pin_line}")
  add_executable(tf40_pin test/tf40_pin.c)
  target_compile_options(tf40_pin PRIVATE -O2 -Wall -Wextra)
  if(NOT WIN32)
    target_link_libraries(tf40_pin PRIVATE ${CMAKE_DL_LIBS})
  endif()
  add_test(NAME tf40_production_sha256 COMMAND ${CMAKE_COMMAND} -DFILE=${_pin_bin} -DWANT=${_pin_sha} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_sha256.cmake")
  add_test(NAME tf40_production_pin COMMAND tf40_pin "${_pin_bin}" "${CMAKE_CURRENT_SOURCE_DIR}/core/CORE_RELEASE"
           "${CMAKE_CURRENT_SOURCE_DIR}/core/qlcore.h" ${_pin_target})
  set_tests_properties(tf40_production_sha256 tf40_production_pin PROPERTIES LABELS production TIMEOUT 60)
endif()
# the 3.x library tests that need no model (the client, sharing, telemetry, the error catalogue)
add_test(NAME tf_ogg COMMAND tf_tests ogg "${TF_VEC}/ogg" "${TF_WORK}/ogg")
add_test(NAME tf_codec COMMAND tf_tests codec "${TF_VEC}" "${TF_WORK}/codec")
add_test(NAME tf_sharing COMMAND tf_tests sharing "${TF_VEC}" "${TF_DATA}/client-fixtures.json" "${TF_WORK}/sharing")
add_test(NAME tf_upload COMMAND tf_tests upload "${TF_WORK}/upload")
add_test(NAME tf_join_window COMMAND tf_tests join_window "${TF_WORK}/join_window")
add_test(NAME tf_client COMMAND tf_tests client "${TF_DATA}/client-fixtures.json" "${TF_WORK}/client")
add_test(NAME tf_bitrate COMMAND tf_tests bitrate "${TF_VEC}" "${TF_WORK}/bitrate")
add_test(NAME tf_consent COMMAND tf_tests consent "${TF_VEC}" "${TF_DATA}/client-fixtures.json" "${TF_WORK}/consent")
add_test(NAME tf_telemetry COMMAND tf_tests telemetry "${TF_WORK}/telemetry")
add_test(NAME tf_sdk_latest COMMAND tf_tests sdk_latest "${TF_DATA}/sdk_latest_configs.json" "${TF_WORK}/sdk_latest")
add_test(NAME tf_errors COMMAND tf_tests errors "${TF_DATA}/sdk_3.0.0")
if(TF_C30_ERRORS AND EXISTS "${TF_C30_ERRORS}")
  # the catalogue of the pinned contract, not a checkout's working tree
  set_tests_properties(tf_errors PROPERTIES ENVIRONMENT "TF_CONTRACT_ERRORS=${TF_C30_ERRORS}")
endif()
add_test(NAME tf_crypto COMMAND tf_tests crypto)
# contract section 29 (SHARE_WRITE_40.md, 4.0.2): test/data/sdk_4.0.0/share_write_40.json is the contract's
# vectors/share_write_40.json at b211fe3 (branch share-write-failure-40), kept here so the test needs no newer contract export
add_test(NAME tf_share_write COMMAND tf_tests share_write "${TF_DATA}/sdk_4.0.0/share_write_40.json" "${TF_WORK}/share_write")
get_property(_all DIRECTORY PROPERTY TESTS)
list(REMOVE_ITEM _all tf40_production_sha256 tf40_production_pin tf_work_clean tf40_flow tf40_lanes tf40_flow_data tf40_switches tf40_share_moments tf40_share_flac tf40_errors_flow tf40_lease_clock tf40_grace tf40_grace_data tf40_grace_vectors
            tf40_app27 tf40_endpoints tf40_loopback tf40_nowait_data tf40_no_wait tf40_load tf40_requests tf40_hosts tf40_lifetime
            tf40_open_data tf40_lifetime_open)
set_tests_properties(${_all} PROPERTIES FIXTURES_REQUIRED tf_work LABELS library TIMEOUT 600)
if(Python3_FOUND)
  add_test(NAME tf_api_scan COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test/api_scan.py")
  add_test(NAME tf_quickstart COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test/quickstart_check.py")
  set_tests_properties(tf_api_scan tf_quickstart PROPERTIES TIMEOUT 120)
endif()
