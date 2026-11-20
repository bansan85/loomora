# zlib 1.3.1 (git submodule third_party/zlib, tag v1.3.1): deflate, inflate, crc32 and adler32 only.
# The submodule is the upstream tree untouched; the project builds only the sources it needs, as the
# C static library loomora_zlib with its own flags: the warning flags and the C++ standard of the
# project never apply to it (it is a C target). One pinned source, so the compressed bytes depend on
# the project and not on the platform's zlib (FR-004). Upgrading it changes the bytes (reference
# hashes change in a visible commit).
# (a parent project that is not the repository root, such as the Gradle build of the Android Modern UI,
# sets LOOMORA_ZLIB_DIR before including this file)
# LOOMORA_USE_SYSTEM_ZLIB (OFF by default): the macOS modern build links the zlib of the SDK instead
# and never reads third_party/zlib. Only that build sets it, so the compressed bytes of every other
# build stay those of the pinned source.
option(LOOMORA_USE_SYSTEM_ZLIB "Link the zlib of the SDK instead of third_party/zlib (macOS modern only)" OFF)
if(LOOMORA_USE_SYSTEM_ZLIB)
  find_package(ZLIB REQUIRED)
  add_library(loomora_zlib INTERFACE)
  target_link_libraries(loomora_zlib INTERFACE ZLIB::ZLIB)
  return()
endif()
if(NOT LOOMORA_ZLIB_DIR)
  set(LOOMORA_ZLIB_DIR "${PROJECT_SOURCE_DIR}/third_party/zlib")
endif()
if(NOT EXISTS "${LOOMORA_ZLIB_DIR}/zlib.h")
  message(FATAL_ERROR "third_party/zlib is empty: run `git submodule update --init third_party/zlib third_party/rapidjson`")
endif()
set(LOOMORA_ZLIB_SOURCES adler32.c crc32.c deflate.c inflate.c inffast.c inftrees.c trees.c zutil.c)
list(TRANSFORM LOOMORA_ZLIB_SOURCES PREPEND "${LOOMORA_ZLIB_DIR}/")
add_library(loomora_zlib STATIC ${LOOMORA_ZLIB_SOURCES})
set_target_properties(loomora_zlib PROPERTIES LINKER_LANGUAGE C POSITION_INDEPENDENT_CODE ON)
# SYSTEM: the include directory is not subject to the warnings of the targets that use it.
target_include_directories(loomora_zlib SYSTEM PUBLIC "${LOOMORA_ZLIB_DIR}")
if(MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
  target_compile_options(loomora_zlib PRIVATE /W0 /D_CRT_SECURE_NO_DEPRECATE /D_CRT_NONSTDC_NO_DEPRECATE)
else()
  # -w: no warning of any kind from the vendored C sources (clang, gcc, MinGW).
  target_compile_options(loomora_zlib PRIVATE -w)
endif()
