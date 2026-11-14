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
