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
add_test(NAME selftest COMMAND loomora_selftest)

# --- loomora_tests: Catch2 --------------------------------------------------------------
# Until step 2b implements loomora_is_eol, loomora_eol_warning and loomora_probe_record
# (T020, T022), test_eol.cpp and test_probe.cpp cannot link, so this target is
# EXCLUDE_FROM_ALL and is not registered with ctest: the default build stays green.
# Build it explicitly with `cmake --build <dir> --target loomora_tests`. Turn
# LOOMORA_BUILD_CATCH2_TESTS ON (default OFF) once the backend is complete to put it in the
# default build and in ctest.
option(LOOMORA_BUILD_CATCH2_TESTS "Build and register the Catch2 tests (needs the full backend)" OFF)

set(LOOMORA_CATCH2_DIR "${PROJECT_SOURCE_DIR}/third_party/catch2/single_include")
if(EXISTS "${LOOMORA_CATCH2_DIR}/catch2/catch.hpp")
  if(LOOMORA_BUILD_CATCH2_TESTS)
    set(LOOMORA_TESTS_ALL "")
  else()
    set(LOOMORA_TESTS_ALL EXCLUDE_FROM_ALL)
  endif()
  add_executable(loomora_tests ${LOOMORA_TESTS_ALL}
    test_main.cpp test_greet.cpp test_eol.cpp test_probe.cpp "${LOOMORA_VECTORS_INC}")
  add_dependencies(loomora_tests loomora_vectors)
  target_include_directories(loomora_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
                                                   "${LOOMORA_CATCH2_DIR}")
  target_link_libraries(loomora_tests PRIVATE loomora)
  set_target_properties(loomora_tests PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                                                 CXX_EXTENSIONS OFF)
  if(LOOMORA_BUILD_CATCH2_TESTS)
    add_test(NAME loomora_tests COMMAND loomora_tests)
  endif()
else()
  message(STATUS "Catch2 submodule not checked out (git submodule update --init): "
                 "loomora_tests is not defined")
endif()
