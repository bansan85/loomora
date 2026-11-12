# MinGW-w64 GCC toolchain for 32-bit Windows (i686), floor Windows 2000 SP4.
#
#   cmake -S . -B build/windows-x86 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-win2000-i686.cmake
#
# The compiler is looked up as <prefix>gcc / <prefix>g++ / <prefix>windres in PATH.
#   LOOMORA_MINGW_PREFIX  tool prefix (default "i686-w64-mingw32-"); set it with -D or in the
#                         environment of the same name, for example with a full path
#                         ("/opt/mingw32/bin/i686-w64-mingw32-") or a variant prefix such as
#                         "i686-w64-mingw32-posix-".
#
# What the toolchain must provide (checked by spike S1, research R6):
#   - the msvcrt.dll C runtime flavour (NOT UCRT: Windows 2000 has no UCRT);
#   - ideally the win32 thread model (the posix model links winpthreads statically, which is
#     fine, but the win32 model is the safer choice for a 2000-era system).
#
# Windows 2000 is NT 5.0: _WIN32_WINNT=0x0500 limits the Windows headers to the APIs of 2000, and
# -static (with the GCC runtime libraries) leaves no runtime DLL to deploy next to the program.

if(NOT DEFINED LOOMORA_MINGW_PREFIX)
  if(DEFINED ENV{LOOMORA_MINGW_PREFIX})
    set(LOOMORA_MINGW_PREFIX "$ENV{LOOMORA_MINGW_PREFIX}")
  else()
    set(LOOMORA_MINGW_PREFIX "i686-w64-mingw32-")
  endif()
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LOOMORA_MINGW_PREFIX)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 5.0)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_C_COMPILER "${LOOMORA_MINGW_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${LOOMORA_MINGW_PREFIX}g++")
set(CMAKE_RC_COMPILER "${LOOMORA_MINGW_PREFIX}windres")

set(LOOMORA_WIN2000_DEFINES "-D_WIN32_WINNT=0x0500 -DWINVER=0x0500")
set(CMAKE_C_FLAGS_INIT "${LOOMORA_WIN2000_DEFINES} -static-libgcc")
set(CMAKE_CXX_FLAGS_INIT "${LOOMORA_WIN2000_DEFINES} -static-libgcc -static-libstdc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# Build-time programs (Python for the test vectors) are the host ones.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
