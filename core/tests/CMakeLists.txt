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

# --- loomora_selftest: part of the default build and of ctest -------------------------------
add_executable(loomora_selftest selftest.cpp "${LOOMORA_VECTORS_INC}")
add_dependencies(loomora_selftest loomora_vectors)
target_include_directories(loomora_selftest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(loomora_selftest PRIVATE loomora)
set_target_properties(loomora_selftest PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                                                  CXX_EXTENSIONS OFF)
# 32-bit MinGW-w64 (the Windows 2000 build): libgcc's gthr-win32.o, pulled in by the exception
# machinery of libstdc++, would import KERNEL32!GetThreadId (Vista and later). The shim of the
# Legacy UI, compiled here from the same source file, satisfies the reference inside the
# executable (docs/implementation/adr/0002). Same condition as ui/legacy/windows-win32/CMakeLists.txt.
if(WIN32 AND NOT MSVC AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"
   AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  target_sources(loomora_selftest PRIVATE
    "${PROJECT_SOURCE_DIR}/ui/legacy/windows-win32/getthreadid_shim.cpp")
endif()
add_test(NAME selftest COMMAND loomora_selftest)

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
  add_executable(loomora_tests
    test_main.cpp test_greet.cpp test_eol.cpp test_probe.cpp "${LOOMORA_VECTORS_INC}")
  add_dependencies(loomora_tests loomora_vectors)
  target_include_directories(loomora_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
                                                   "${LOOMORA_CATCH2_DIR}")
  target_link_libraries(loomora_tests PRIVATE loomora)
  set_target_properties(loomora_tests PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                                                 CXX_EXTENSIONS OFF)
  add_test(NAME loomora_tests COMMAND loomora_tests)
else()
  message(STATUS "Catch2 submodule not checked out (git submodule update --init): "
                 "loomora_tests is not defined")
endif()
