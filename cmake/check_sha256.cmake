# cmake -DFILE=<file> -DWANT=<64 hex> -P check_sha256.cmake: fails unless the file's SHA-256 is WANT
if(NOT EXISTS "${FILE}")
  message(FATAL_ERROR "${FILE} does not exist")
endif()
file(SHA256 "${FILE}" _have)
if(NOT _have STREQUAL WANT)
  message(FATAL_ERROR "${FILE}: SHA-256 ${_have}, the pin says ${WANT}")
endif()
message(STATUS "${FILE}: SHA-256 ${_have}, as pinned")
