# MinGW-w64 GCC toolchain for 64-bit Windows (x86_64), floor Windows XP x64 Edition (NT 5.2).
#
#   cmake -S . -B build/windows-x64 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-win-x86_64.cmake
#
# The compiler is looked up as <prefix>gcc / <prefix>g++ / <prefix>windres in PATH.
#   LOOMORA_MINGW_PREFIX  tool prefix (default "x86_64-w64-mingw32-"); set it with -D or in the
#                         environment of the same name, for example with a full path
#                         ("/opt/mingw64/bin/x86_64-w64-mingw32-") or a variant prefix such as
#                         "x86_64-w64-mingw32-posix-".
#
# The toolchain must provide the msvcrt.dll C runtime flavour, not UCRT (research R6).
#
# The Windows headers are limited to the API level of the floor, Windows XP x64 / Server 2003 x64
# (NT 5.2, _WIN32_WINNT=0x0502; the 32-bit build, whose floor is Windows 2000, uses 0x0500, see
# mingw-w64-win2000-i686.cmake); -static (with the GCC runtime libraries) leaves no runtime DLL
# to deploy next to the program.
#
# LOOMORA_AVOID_IOSTREAM: the floor is older than Windows Vista, and <iostream> of a static
# libstdc++ build imports the Vista-only condition variable functions (selftest, docs/implementation/adr/0004).

if(NOT DEFINED LOOMORA_MINGW_PREFIX)
  if(DEFINED ENV{LOOMORA_MINGW_PREFIX})
    set(LOOMORA_MINGW_PREFIX "$ENV{LOOMORA_MINGW_PREFIX}")
  else()
    set(LOOMORA_MINGW_PREFIX "x86_64-w64-mingw32-")
  endif()
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LOOMORA_MINGW_PREFIX)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 5.2)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER "${LOOMORA_MINGW_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${LOOMORA_MINGW_PREFIX}g++")
set(CMAKE_RC_COMPILER "${LOOMORA_MINGW_PREFIX}windres")

set(LOOMORA_WINNT 0x0502)
set(LOOMORA_WINNT_DEFINES "-D_WIN32_WINNT=${LOOMORA_WINNT} -DWINVER=${LOOMORA_WINNT}")
set(CMAKE_C_FLAGS_INIT "${LOOMORA_WINNT_DEFINES} -static-libgcc")
set(CMAKE_CXX_FLAGS_INIT
    "${LOOMORA_WINNT_DEFINES} -DLOOMORA_AVOID_IOSTREAM=1 -static-libgcc -static-libstdc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# Build-time programs (Python for the test vectors) are the host ones.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
