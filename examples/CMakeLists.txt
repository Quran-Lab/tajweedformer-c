# The example app as its own CMake project: the quick start, takes in a row, and a sample clip without a
# microphone (see the top of quickstart.c).
#
#   cmake -S examples -B build-example -G Ninja -DCMAKE_BUILD_TYPE=Release           (Linux)
#   cmake -S examples -B build-example -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++   (Windows)
#   cmake --build build-example
#
# It takes the SDK from the repository it sits in. An app outside the repository takes it by tag instead, with
# FetchContent as the README's Install shows; the rest is the same.
cmake_minimum_required(VERSION 3.24)
project(tajweedformer_example C CXX)

add_subdirectory(.. tajweedformer)

add_executable(quickstart quickstart.c)
target_link_libraries(quickstart PRIVATE tajweedformer)
if(WIN32)   # the library beside the program
  target_compile_definitions(quickstart PRIVATE _CRT_SECURE_NO_WARNINGS)
  add_custom_command(TARGET quickstart POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:quickstart> $<TARGET_FILE_DIR:quickstart> COMMAND_EXPAND_LISTS)
endif()
