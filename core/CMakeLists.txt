set(LOOMORA_SOURCES
  src/c_api.cpp src/eol.cpp src/greet.cpp src/probe.cpp src/utf8.cpp src/version.cpp)

# The .loomora file format (spec 002): every .cpp of src/format/ (any depth) and every
# src/c_api_project*.cpp is picked up by glob, so a new file needs no edit here (CONFIGURE_DEPENDS
# re-globs at build time). The platform files of src/format/io/ are #if-guarded: they compile to
# nothing on the other platforms.
file(GLOB_RECURSE LOOMORA_FORMAT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/format/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/c_api_project*.cpp")
list(APPEND LOOMORA_SOURCES ${LOOMORA_FORMAT_SOURCES})

# Internal headers are included as "format/report.hpp", "internal/config.hpp"; RapidJSON is a
# SYSTEM include so -pedantic-errors -Werror never applies to it; zlib is the C library
# loomora_zlib (cmake/loomora_zlib.cmake, its own flags).
# A parent project that is not the repository root (the Gradle build of the Android Modern UI) sets it first.
if(NOT LOOMORA_RAPIDJSON_DIR)
  set(LOOMORA_RAPIDJSON_DIR "${PROJECT_SOURCE_DIR}/third_party/rapidjson/include")
endif()

# Fuzz targets (core/fuzz, libFuzzer; Clang only): OFF by default.
option(LOOMORA_BUILD_FUZZ "Build the fuzz targets of core/fuzz (Clang with libFuzzer)" OFF)

# Sanitizer build of the backend and of its Catch2 tests, for a check by hand (never part of
# tools/verify.py): -DLOOMORA_SANITIZE=thread (ThreadSanitizer, the test of the threads) or address
# (AddressSanitizer + UndefinedBehaviorSanitizer). The flags go to the loomora and loomora_tests
# targets only: build that one target (`cmake --build <dir> --target loomora_tests`), the other
# programs of the tree are not linked with the sanitizer runtime. GCC or Clang on Linux.
set(LOOMORA_SANITIZE "" CACHE STRING "Sanitizer for loomora and loomora_tests: thread, address or empty")
set_property(CACHE LOOMORA_SANITIZE PROPERTY STRINGS "" thread address)
set(LOOMORA_SANITIZE_FLAGS "")
if(LOOMORA_SANITIZE STREQUAL "thread")
  set(LOOMORA_SANITIZE_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
elseif(LOOMORA_SANITIZE STREQUAL "address")
  set(LOOMORA_SANITIZE_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined
                             -fno-omit-frame-pointer)
elseif(NOT LOOMORA_SANITIZE STREQUAL "")
  message(FATAL_ERROR "LOOMORA_SANITIZE must be empty, thread or address (got '${LOOMORA_SANITIZE}')")
endif()

add_library(loomora STATIC ${LOOMORA_SOURCES})
if(LOOMORA_SANITIZE_FLAGS)
  target_compile_options(loomora PRIVATE ${LOOMORA_SANITIZE_FLAGS})
endif()
target_include_directories(loomora PUBLIC include)
target_include_directories(loomora PRIVATE src)
target_include_directories(loomora SYSTEM PRIVATE "${LOOMORA_RAPIDJSON_DIR}")
target_link_libraries(loomora PRIVATE loomora_zlib)
set_target_properties(loomora PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                                         CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(loomora PRIVATE -Wall -Wextra -pedantic)
endif()

# Shared library (loomora.dll for WinUI, libloomora.so / .dylib elsewhere). Only the loomora_*
# functions marked LOOMORA_API are exported; tools/check_std.py verifies it.
option(LOOMORA_BUILD_SHARED "Also build the shared library loomora_shared" OFF)
if(LOOMORA_BUILD_SHARED)
  add_library(loomora_shared SHARED ${LOOMORA_SOURCES})
  target_include_directories(loomora_shared PUBLIC include)
  target_include_directories(loomora_shared PRIVATE src)
  target_include_directories(loomora_shared SYSTEM PRIVATE "${LOOMORA_RAPIDJSON_DIR}")
  target_link_libraries(loomora_shared PRIVATE loomora_zlib)
  target_compile_definitions(loomora_shared PRIVATE LOOMORA_BUILD_SHARED_LIB)
  target_compile_definitions(loomora_shared INTERFACE LOOMORA_USE_DLL)
  set_target_properties(loomora_shared PROPERTIES
    OUTPUT_NAME loomora CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  # MSVC (host-msvc preset, loomora.dll of the WinUI 3 UI): the C runtime is linked statically
  # (/MT, /MTd in Debug), so the DLL needs no Visual C++ redistributable. MSVC has no /std:c++11:
  # CXX_STANDARD 11 adds no flag and cl.exe compiles in its default, /std:c++14, the lowest it
  # accepts; the sources stay strict C++11 (checked by the GCC and Clang builds).
  if(MSVC)
    set_target_properties(loomora_shared PROPERTIES
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(loomora_shared PRIVATE -Wall -Wextra -pedantic)
  endif()
  # -fvisibility=hidden does not hide the std templates (libstdc++ marks namespace std as
  # default-visibility): the export list is enforced at link time too.
  if(APPLE)
    set_target_properties(loomora_shared PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/loomora.exp")
    target_link_options(loomora_shared PRIVATE
      "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/loomora.exp")
  elseif(UNIX)
    set_target_properties(loomora_shared PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/loomora.map")
    target_link_options(loomora_shared PRIVATE
      "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/loomora.map")
  endif()
endif()

install(FILES include/loomora.h include/loomora_project.h DESTINATION include)
install(TARGETS loomora ARCHIVE DESTINATION lib)
if(LOOMORA_BUILD_SHARED)
  install(TARGETS loomora_shared ARCHIVE DESTINATION lib LIBRARY DESTINATION lib
                  RUNTIME DESTINATION bin)
endif()

if(LOOMORA_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# --- fuzz targets (spec 002, T072): core/fuzz/CMakeLists.txt ---------------------------------------
if(LOOMORA_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()
# --- end of fuzz targets ----------------------------------------------------------------------------
