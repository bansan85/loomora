# GCC toolchain for 32-bit Linux (i686) on a 64-bit Linux host: the compiler of the host with -m32.
#
#   cmake -S . -B ~/loomora-build/linux-legacy-i686 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-gcc-i686.cmake
#
# Used by tools/build.py linux legacy for the native i686 executable of the Linux Legacy UI (Xlib).
# This is a native build, made on the machine the program runs on (ADR 0007), not a cross build:
# it needs the 32-bit development files of that machine (Ubuntu / Debian: gcc-multilib
# g++-multilib and libx11-dev:i386, after dpkg --add-architecture i386; libxft-dev:i386 and
# libfontconfig-dev:i386 as well for the Xft text path, the UI is built with the core font
# without them).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_C_FLAGS_INIT "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")

# Debian / Ubuntu multiarch directory of the 32-bit libraries (libX11.so is in
# /usr/lib/i386-linux-gnu); harmless where it does not exist.
set(CMAKE_LIBRARY_ARCHITECTURE i386-linux-gnu)

# pkg-config must describe the 32-bit libraries, not the 64-bit ones of the host (xft.pc of
# /usr/lib/x86_64-linux-gnu would put a 64-bit libXft on a 32-bit link line). Where the 32-bit
# development package is not installed pkg-config finds nothing, and the UI is built without Xft.
set(ENV{PKG_CONFIG_LIBDIR}
    "/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig")

# Floating point (spike S-F2, spec 002 T059): on 32-bit x86 GCC computes doubles on the x87 unit with
# 80-bit intermediates by default, and the test fixtures that build the reference projects (cos, sin,
# rounding to 3 decimals) then differ from x86-64 in some nails, so the bytes of `multi` differ. The
# test programs are therefore compiled with SSE2 arithmetic (core/tests/CMakeLists.txt,
# loomora_test_fp); libloomora is unchanged. Needs a CPU with SSE2 to run the tests (Pentium 4 and later).
# Fallback id x87-double-fixtures, platform linux-legacy-i686.
set(LOOMORA_TEST_FP_FLAGS -msse2 -mfpmath=sse)
