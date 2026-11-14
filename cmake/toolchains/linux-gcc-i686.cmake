# GCC toolchain for 32-bit Linux (i686) on a 64-bit Linux host: the compiler of the host with -m32.
#
#   cmake -S . -B build/linux-legacy-i686 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-gcc-i686.cmake
#
# Used by tools/build.py linux legacy for the native i686 executable of the Linux Legacy UI (Xlib).
# It needs the 32-bit development files of the host (Ubuntu / Debian: gcc-multilib g++-multilib
# and libx11-dev:i386, after dpkg --add-architecture i386). The manylinux2010 i686 container image
# is a 32-bit system already and does not use this file.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_C_FLAGS_INIT "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")

# Debian / Ubuntu multiarch directory of the 32-bit libraries (libX11.so is in
# /usr/lib/i386-linux-gnu); harmless where it does not exist.
set(CMAKE_LIBRARY_ARCHITECTURE i386-linux-gnu)
