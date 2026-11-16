# osxcross Clang toolchain for macOS 10.6 (Snow Leopard), x86_64 and i386, from a Linux host.
#
#   cmake -S . -B build/macos-10.6 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/osxcross-macos10.6.cmake \
#         -DLOOMORA_MACOS_SDK_PATH=/opt/osxcross/SDK/MacOSX10.6.sdk
#
# Spike S2 (T067) and T028. NOT VERIFIED: no osxcross and no 10.6 SDK were available when this file
# was written (the owner has no old Mac). The run on 10.6 is `not_tested` until a real machine is
# available (docs/implementation/adr/0019 and the Apple ADR of this series).
#
# The compiler is the osxcross driver of the target triple, looked up in PATH:
#   LOOMORA_OSXCROSS_PREFIX  tool prefix (default "o64-"): "o64-clang" and "o64-clang++" for the
#                            x86_64 slice, "o32-clang" and "o32-clang++" for i386 (osxcross wrappers).
#   LOOMORA_MACOS_SDK_PATH   the MacOSX10.6.sdk directory (required: the SDK is not redistributable
#                            and is not in the repository; its origin is spike S7, T065).
#
# The deployment target is 10.6. -mmacosx-version-min is passed explicitly because a non-Apple host
# does not read CMAKE_OSX_DEPLOYMENT_TARGET. The slices are i386 and x86_64; the Legacy macOS UI
# itself is built for x86_64 only on macos-26 (tools/build.py macos legacy, fallback
# macos-legacy-x86_64-only), so i386 is only what this toolchain can offer.
#
# Libraries: static libc++ is requested. Whether a 10.6 system provides libc++ as a shared library
# is not verified (spike S11 records the question); the static link is the safer choice here.

if(NOT DEFINED LOOMORA_OSXCROSS_PREFIX)
  if(DEFINED ENV{LOOMORA_OSXCROSS_PREFIX})
    set(LOOMORA_OSXCROSS_PREFIX "$ENV{LOOMORA_OSXCROSS_PREFIX}")
  else()
    set(LOOMORA_OSXCROSS_PREFIX "o64-")
  endif()
endif()
if(NOT DEFINED LOOMORA_MACOS_SDK_PATH)
  if(DEFINED ENV{LOOMORA_MACOS_SDK_PATH})
    set(LOOMORA_MACOS_SDK_PATH "$ENV{LOOMORA_MACOS_SDK_PATH}")
  else()
    message(FATAL_ERROR "Set LOOMORA_MACOS_SDK_PATH to the MacOSX10.6.sdk directory (spike S7).")
  endif()
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LOOMORA_OSXCROSS_PREFIX LOOMORA_MACOS_SDK_PATH)

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_VERSION 10.6)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER "${LOOMORA_OSXCROSS_PREFIX}clang")
set(CMAKE_CXX_COMPILER "${LOOMORA_OSXCROSS_PREFIX}clang++")
set(CMAKE_OSX_SYSROOT "${LOOMORA_MACOS_SDK_PATH}")
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.6")
set(CMAKE_OSX_ARCHITECTURES "i386;x86_64")

set(LOOMORA_TARGET_MACOS_10_6 ON)
set(CMAKE_C_FLAGS_INIT "-mmacosx-version-min=10.6")
set(CMAKE_CXX_FLAGS_INIT "-mmacosx-version-min=10.6 -stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-mmacosx-version-min=10.6 -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-mmacosx-version-min=10.6")

# Build-time programs (Python for the test vectors) are the host ones.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
