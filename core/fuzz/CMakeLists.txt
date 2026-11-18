# Fuzz targets of the .loomora reader (spec 002, T072). Included by core/CMakeLists.txt only when
# LOOMORA_BUILD_FUZZ is ON. Both targets run an input through fuzz::exercise (fuzz_common.hpp):
# validate, open and read every item of an archive in memory.
#
#   loomora_fuzz_project  libFuzzer entry (Clang with -fsanitize=fuzzer); defined only when the
#                         compiler has libFuzzer. Run: loomora_fuzz_project -timeout=5 corpus seeds
#   loomora_fuzz_loop     a mutation loop over corpus files for hosts without libFuzzer (g++, MSVC-less
#                         hosts); same entry point. Run: loomora_fuzz_loop --seconds 300 seeds
#
# Both link loomora_fuzzlib, a copy of the backend built with AddressSanitizer and
# UndefinedBehaviorSanitizer when the compiler has them (FUZZ_SANITIZERS, default ON), so a
# memory error inside the library is reported, not only one in the driver. Seed corpus: the target
# loomora_fuzz_seeds writes the files of tools/gen_invalid_corpus.py (the T065 corpus: every valid
# reference, every invalid, hostile and warning file) into <build>/fuzz-seeds.
option(FUZZ_SANITIZERS "Build the fuzz targets with ASan and UBSan when the compiler has them" ON)

include(CheckCXXSourceCompiles)
set(FUZZ_SAN_FLAGS "")
if(FUZZ_SANITIZERS AND NOT MSVC)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
  check_cxx_source_compiles("int main() { return 0; }" FUZZ_HAS_SANITIZERS)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(FUZZ_HAS_SANITIZERS)
    set(FUZZ_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  endif()
endif()
if(NOT FUZZ_SAN_FLAGS)
  message(STATUS "fuzz: the compiler has no ASan/UBSan (or FUZZ_SANITIZERS is OFF): fuzz targets are built without")
endif()

# The backend, instrumented (LOOMORA_SOURCES holds paths relative to core/).
set(FUZZ_SOURCES "")
foreach(f IN LISTS LOOMORA_SOURCES)
  if(IS_ABSOLUTE "${f}")
    list(APPEND FUZZ_SOURCES "${f}")
  else()
    list(APPEND FUZZ_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../${f}")
  endif()
endforeach()
add_library(loomora_fuzzlib STATIC ${FUZZ_SOURCES})
target_include_directories(loomora_fuzzlib PUBLIC "${PROJECT_SOURCE_DIR}/core/include")
target_include_directories(loomora_fuzzlib PUBLIC "${PROJECT_SOURCE_DIR}/core/src")
target_include_directories(loomora_fuzzlib SYSTEM PRIVATE "${LOOMORA_RAPIDJSON_DIR}")
target_link_libraries(loomora_fuzzlib PUBLIC loomora_zlib)
set_target_properties(loomora_fuzzlib PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                                                 CXX_VISIBILITY_PRESET hidden)
if(FUZZ_SAN_FLAGS)
  target_compile_options(loomora_fuzzlib PRIVATE ${FUZZ_SAN_FLAGS})
  target_link_options(loomora_fuzzlib PUBLIC ${FUZZ_SAN_FLAGS})
endif()

# The mutation loop.
add_executable(loomora_fuzz_loop fuzz_loop.cpp fuzz_project.cpp)
target_link_libraries(loomora_fuzz_loop PRIVATE loomora_fuzzlib)
set_target_properties(loomora_fuzz_loop PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(FUZZ_SAN_FLAGS)
  target_compile_options(loomora_fuzz_loop PRIVATE ${FUZZ_SAN_FLAGS})
endif()

# The libFuzzer target, when the compiler has libFuzzer.
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
check_cxx_source_compiles("#include <cstddef>\n#include <cstdint>\nextern \"C\" int LLVMFuzzerTestOneInput(const std::uint8_t *, std::size_t) { return 0; }"
                          FUZZ_HAS_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(FUZZ_HAS_LIBFUZZER)
  add_executable(loomora_fuzz_project fuzz_project.cpp)
  target_compile_definitions(loomora_fuzz_project PRIVATE FUZZ_LIBFUZZER=1)
  target_link_libraries(loomora_fuzz_project PRIVATE loomora_fuzzlib)
  target_compile_options(loomora_fuzz_project PRIVATE -fsanitize=fuzzer)
  target_link_options(loomora_fuzz_project PRIVATE -fsanitize=fuzzer)
  set_target_properties(loomora_fuzz_project PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(FUZZ_SAN_FLAGS)
    target_compile_options(loomora_fuzz_project PRIVATE ${FUZZ_SAN_FLAGS})
  endif()
else()
  message(STATUS "fuzz: no libFuzzer (-fsanitize=fuzzer) in this compiler: loomora_fuzz_project is not defined, use loomora_fuzz_loop")
endif()

# Seed corpus.
find_package(Python3 3.8 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_custom_target(loomora_fuzz_seeds
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gen_invalid_corpus.py"
            "${CMAKE_CURRENT_BINARY_DIR}/../fuzz-seeds" --skip-truncate
    COMMENT "Writing the fuzz seed corpus"
    VERBATIM)
endif()
