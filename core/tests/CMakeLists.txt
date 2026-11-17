# Backend tests: loomora_selftest (plain C++11, no framework, cross-compiled and run on
# every target) and loomora_tests (Catch2, host only).
#
# Both are driven by vectors.inc, generated into the build directory from vectors.json by
# tools/gen_vectors.py (a host tool: Python 3.8+). The generated file is not committed.

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

set(LOOMORA_TOOLS_DIR "${PROJECT_SOURCE_DIR}/tools")
set(LOOMORA_VECTORS_JSON "${CMAKE_CURRENT_SOURCE_DIR}/vectors.json")
set(LOOMORA_VECTORS_INC "${CMAKE_CURRENT_BINARY_DIR}/vectors.inc")

add_custom_command(
  OUTPUT "${LOOMORA_VECTORS_INC}"
  COMMAND "${Python3_EXECUTABLE}" "${LOOMORA_TOOLS_DIR}/gen_vectors.py"
          -o "${LOOMORA_VECTORS_INC}" "${LOOMORA_VECTORS_JSON}"
  DEPENDS "${LOOMORA_VECTORS_JSON}" "${LOOMORA_TOOLS_DIR}/gen_vectors.py"
          "${LOOMORA_TOOLS_DIR}/common.py"
  COMMENT "Generating vectors.inc from vectors.json"
  VERBATIM)
add_custom_target(loomora_vectors DEPENDS "${LOOMORA_VECTORS_INC}")

# Per-target floating-point switch (docs/implementation/adr/0004): a toolchain whose doubles are not
# the SSE2/IEEE ones (32-bit x86 with the x87 unit) sets LOOMORA_TEST_FP_FLAGS. The reference fixtures
# (basic_project.hpp, multi_project.hpp: cos/sin, round3, 0.1 + 0.2 of the tests) are computed with
# doubles and must give the bytes of the other targets (FR-004); only the test programs take the flags,
# libloomora itself is not changed. Empty on every other toolchain.
function(loomora_test_fp target)
  if(LOOMORA_TEST_FP_FLAGS)
    target_compile_options(${target} PRIVATE ${LOOMORA_TEST_FP_FLAGS})
  endif()
endfunction()

# --- loomora_selftest: part of the default build and of ctest -------------------------------
add_executable(loomora_selftest selftest.cpp "${LOOMORA_VECTORS_INC}")
add_dependencies(loomora_selftest loomora_vectors)
target_include_directories(loomora_selftest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(loomora_selftest PRIVATE loomora)
set_target_properties(loomora_selftest PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                                                  CXX_EXTENSIONS OFF)
# Windows 2000 build (cmake/toolchains/mingw-w64-win2000-i686.cmake sets LOOMORA_TARGET_WIN2000):
# libgcc's gthr-win32.o, pulled in by the exception machinery of libstdc++, would import
# KERNEL32!GetThreadId (Vista and later). The shim of the Legacy UI, compiled here from the same
# source file, satisfies the reference inside the executable (docs/implementation/adr/0002). Every other target
# links the system's own GetThreadId. The same toolchain defines LOOMORA_AVOID_IOSTREAM, which
# makes selftest.cpp print through <cstdio> (docs/implementation/adr/0004).
if(LOOMORA_TARGET_WIN2000 OR LOOMORA_TARGET_WINXP64)
  target_sources(loomora_selftest PRIVATE
    "${PROJECT_SOURCE_DIR}/ui/legacy/windows-win32/getthreadid_shim.cpp")
endif()
# Emscripten (cmake/toolchains/emscripten-*.cmake): the toolchain exports the six loomora_*
# functions of the module; the self-test needs its own main() kept instead, otherwise the linker
# drops it as dead code and the test would pass without running anything.
if(LOOMORA_TARGET_WEB)
  target_link_options(loomora_selftest PRIVATE
    "-sEXPORTED_FUNCTIONS=@${PROJECT_SOURCE_DIR}/bridge/wasm/exports-selftest.txt")
endif()
add_test(NAME selftest COMMAND loomora_selftest)

# --- loomora_format_selftest: the .loomora format, plain C++11 without a framework -----------
# Same special cases as loomora_selftest (the Windows 2000 GetThreadId shim, LOOMORA_AVOID_IOSTREAM
# honoured by format_selftest.cpp, the Emscripten exported main). Cross-compiled and run on every
# target. It uses the internal headers of core/src (limits(), testing.hpp) through the static
# library, so it needs no compile definition.
add_executable(loomora_format_selftest format_selftest.cpp)
target_include_directories(loomora_format_selftest PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
target_include_directories(loomora_format_selftest SYSTEM PRIVATE "${LOOMORA_RAPIDJSON_DIR}")
target_link_libraries(loomora_format_selftest PRIVATE loomora)
set_target_properties(loomora_format_selftest PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                                                         CXX_EXTENSIONS OFF)
if(LOOMORA_TARGET_WIN2000 OR LOOMORA_TARGET_WINXP64)
  target_sources(loomora_format_selftest PRIVATE
    "${PROJECT_SOURCE_DIR}/ui/legacy/windows-win32/getthreadid_shim.cpp")
endif()
if(LOOMORA_TARGET_WEB)
  target_link_options(loomora_format_selftest PRIVATE
    "-sEXPORTED_FUNCTIONS=@${PROJECT_SOURCE_DIR}/bridge/wasm/exports-selftest.txt")
endif()
loomora_test_fp(loomora_format_selftest)
add_test(NAME format_selftest COMMAND loomora_format_selftest)

# --- loomora_tests: Catch2 --------------------------------------------------------------
# The backend is complete (greet, is_eol, eol_warning, probe_record), so loomora_tests is part
# of the default build and registered with ctest. It needs the Catch2 submodule
# (git submodule update --init); without it the target is skipped with a status message.
# Turn LOOMORA_BUILD_CATCH2_TESTS OFF to leave it out (for example on a cross build).
option(LOOMORA_BUILD_CATCH2_TESTS "Build and register the Catch2 tests" ON)

set(LOOMORA_CATCH2_DIR "${PROJECT_SOURCE_DIR}/third_party/catch2/single_include")
if(NOT LOOMORA_BUILD_CATCH2_TESTS)
  message(STATUS "Catch2 tests disabled (LOOMORA_BUILD_CATCH2_TESTS=OFF)")
elseif(EXISTS "${LOOMORA_CATCH2_DIR}/catch2/catch.hpp")
  # The tests of the .loomora format (spec 002) are every format/test_*.cpp, picked up by glob
  # (CONFIGURE_DEPENDS): a new test file needs no edit here.
  file(GLOB LOOMORA_FORMAT_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/format/test_*.cpp")
  # The headers of the public C API are compiled as C89 and as C++98 by two tiny libraries
  # (format/header_c89.c, format/header_cpp98.cpp) that test_c_header.cpp calls.
  add_library(loomora_header_c89 STATIC format/header_c89.c)
  target_include_directories(loomora_header_c89 PRIVATE "${PROJECT_SOURCE_DIR}/core/include")
  set_target_properties(loomora_header_c89 PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED ON
                                                      C_EXTENSIONS OFF)
  add_library(loomora_header_cpp98 STATIC format/header_cpp98.cpp)
  target_include_directories(loomora_header_cpp98 PRIVATE "${PROJECT_SOURCE_DIR}/core/include")
  set_target_properties(loomora_header_cpp98 PROPERTIES CXX_STANDARD 98 CXX_STANDARD_REQUIRED ON
                                                        CXX_EXTENSIONS OFF)
  if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(loomora_header_c89 PRIVATE -pedantic-errors -Wall -Wextra -Werror)
    target_compile_options(loomora_header_cpp98 PRIVATE -pedantic-errors -Wall -Wextra -Werror)
  endif()

  add_executable(loomora_tests
    test_main.cpp test_greet.cpp test_eol.cpp test_probe.cpp ${LOOMORA_FORMAT_TESTS}
    "${LOOMORA_VECTORS_INC}")
  add_dependencies(loomora_tests loomora_vectors)
  target_include_directories(loomora_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
                                                   "${LOOMORA_CATCH2_DIR}"
                                                   "${PROJECT_SOURCE_DIR}/core/src")
  target_include_directories(loomora_tests SYSTEM PRIVATE "${LOOMORA_RAPIDJSON_DIR}")
  target_link_libraries(loomora_tests PRIVATE loomora loomora_zlib loomora_header_c89
                                              loomora_header_cpp98)
  set_target_properties(loomora_tests PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                                                 CXX_EXTENSIONS OFF)
  loomora_test_fp(loomora_tests)
  add_test(NAME loomora_tests COMMAND loomora_tests)
else()
  message(STATUS "Catch2 submodule not checked out (git submodule update --init): "
                 "loomora_tests is not defined")
endif()

# --- spike S-F1 (spec 002, T009): zlib and RapidJSON on a toolchain ---------------------------
# Not part of the default build or of ctest: build it by hand with
# `cmake --build <dir> --target loomora_spike_fmt_deps` and run it on each toolchain.
add_executable(loomora_spike_fmt_deps EXCLUDE_FROM_ALL spike_fmt_deps.cpp)
target_include_directories(loomora_spike_fmt_deps SYSTEM PRIVATE "${LOOMORA_RAPIDJSON_DIR}")
target_link_libraries(loomora_spike_fmt_deps PRIVATE loomora_zlib)
set_target_properties(loomora_spike_fmt_deps PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                                                         CXX_EXTENSIONS OFF)
if(LOOMORA_TARGET_WIN2000 OR LOOMORA_TARGET_WINXP64)
  target_sources(loomora_spike_fmt_deps PRIVATE
    "${PROJECT_SOURCE_DIR}/ui/legacy/windows-win32/getthreadid_shim.cpp")
endif()

# --- loomora_refgen: writes the `basic` reference project (spec 002, T039). Host only ----------
# Also built by the MinGW-w64 Windows toolchains (an .exe run on Windows) and by the i686 Linux
# toolchain (run natively on the 64-bit host) for the cross-architecture identity of the files (T059);
# not by the Web, Android or iOS cross builds, which cannot run it.
if(NOT LOOMORA_TARGET_WEB AND NOT LOOMORA_TARGET_ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS"
   AND (NOT CMAKE_CROSSCOMPILING OR CMAKE_SYSTEM_NAME STREQUAL "Windows"
        OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  add_executable(loomora_refgen refgen.cpp)
  target_link_libraries(loomora_refgen PRIVATE loomora)
  loomora_test_fp(loomora_refgen)
  if(LOOMORA_TARGET_WIN2000 OR LOOMORA_TARGET_WINXP64)
    target_sources(loomora_refgen PRIVATE
      "${PROJECT_SOURCE_DIR}/ui/legacy/windows-win32/getthreadid_shim.cpp")
  endif()
  set_target_properties(loomora_refgen PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                                                  CXX_EXTENSIONS OFF)
endif()
# --- loomora_use_demo: the use-mode client run by tools/check_readonly.py (spec 002, T042). Host only ---
if(NOT LOOMORA_TARGET_WIN2000 AND NOT LOOMORA_TARGET_WEB AND NOT CMAKE_CROSSCOMPILING)
  add_executable(loomora_use_demo use_demo.cpp)
  target_include_directories(loomora_use_demo SYSTEM PRIVATE "${LOOMORA_RAPIDJSON_DIR}")
  target_link_libraries(loomora_use_demo PRIVATE loomora)
  set_target_properties(loomora_use_demo PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                                                    CXX_EXTENSIONS OFF)
endif()
# --- end of loomora_use_demo ------------------------------------------------------------------------
# --- reader corpus (spec 002, T035) ---------------------------------------------------------
# format/test_project_reader.cpp opens the files of tools/gen_invalid_corpus.py (independent of
# the C++ code). The corpus is generated into the build directory at build time and its path is
# passed to the tests as FMT_CORPUS_DIR; the tests say so and do nothing when it is absent.
if(TARGET loomora_tests)
  set(FMT_CORPUS_DIR "${CMAKE_CURRENT_BINARY_DIR}/corpus")
  add_custom_command(
    OUTPUT "${FMT_CORPUS_DIR}/corpus.json"
    COMMAND "${Python3_EXECUTABLE}" "${LOOMORA_TOOLS_DIR}/gen_invalid_corpus.py"
            "${FMT_CORPUS_DIR}" --skip-truncate
    DEPENDS "${LOOMORA_TOOLS_DIR}/gen_invalid_corpus.py" "${LOOMORA_TOOLS_DIR}/loomora_pyfmt.py"
    COMMENT "Generating the .loomora corpus"
    VERBATIM)
  add_custom_target(loomora_corpus DEPENDS "${FMT_CORPUS_DIR}/corpus.json")
  add_dependencies(loomora_tests loomora_corpus)
  target_compile_definitions(loomora_tests PRIVATE FMT_CORPUS_DIR="${FMT_CORPUS_DIR}")
  # The expected content of `multi` written by tools/gen_reference.py (T054).
  target_compile_definitions(loomora_tests PRIVATE
    FMT_MULTI_EXPECTED="${CMAKE_CURRENT_SOURCE_DIR}/format/multi.expected.json")
endif()
# --- end of reader corpus ---------------------------------------------------------------------
# --- loomora_bench_format: the SC-005 measurement of the reader (spec 002, T074). Host only, never built by default ---
if(NOT LOOMORA_TARGET_WIN2000 AND NOT LOOMORA_TARGET_WEB AND NOT CMAKE_CROSSCOMPILING)
  add_executable(loomora_bench_format EXCLUDE_FROM_ALL bench_format.cpp)
  target_link_libraries(loomora_bench_format PRIVATE loomora)
  if(WIN32)
    target_link_libraries(loomora_bench_format PRIVATE psapi)
  endif()
  set_target_properties(loomora_bench_format PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                                                        CXX_EXTENSIONS OFF)
endif()
# --- end of loomora_bench_format --------------------------------------------------------------------
