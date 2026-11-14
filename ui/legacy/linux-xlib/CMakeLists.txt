# Linux Legacy UI: C++11 and Xlib, one plain executable (loomora-xlib).
#
# Built with the top-level project (the backend library "loomora" of core/ is linked into it):
#
#   cmake -S . -B build/linux-legacy -DCMAKE_BUILD_TYPE=Release -DLOOMORA_BUILD_TESTS=OFF
#   cmake --build build/linux-legacy --target loomora_xlib
#
# tools/build.py linux legacy does this for x86_64 and i686, natively and in the manylinux2010
# container. The 32-bit build on a 64-bit host takes the toolchain file
# cmake/toolchains/linux-gcc-i686.cmake (gcc -m32; needs the i386 libX11, see below).
#
# The only library linked dynamically besides libc (and libdl, where dlopen lives before glibc
# 2.34) is libX11: libXft and fontconfig, which draw the text, are opened at run time with dlopen
# (xftdl.cpp), so neither their development files nor the libraries themselves are needed to
# build or to start the program. The C++ runtime (libstdc++, libgcc) is linked DYNAMICALLY by
# default (the distribution's own, the modern path); -DLOOMORA_STATIC_CXX=ON links it statically:
# only the manylinux2010 container build, the old-glibc baseline target, sets it (CentOS 6 has no
# libstdc++ new enough for a C++11 backend: fallback "static-cxx-runtime-xlib" of
# validation/fallbacks.json).
#
# The floor of the UI (glibc 2.12, kernel 2.6.32) is only reached with a toolchain whose C library
# is that old (the manylinux2010 image, spike S4 / T069): a build on a recent distribution needs
# that distribution's recent glibc (see the report of tools/build.py).
#
# 32-bit build on Ubuntu / Debian: sudo dpkg --add-architecture i386 && sudo apt update &&
#   sudo apt install gcc-multilib g++-multilib libx11-dev:i386

option(LOOMORA_STATIC_CXX "Link libstdc++ and libgcc statically into loomora-xlib (manylinux2010 build)" OFF)
option(LOOMORA_XLIB_REQUIRED "Fail when the libX11 development files are missing (otherwise loomora_xlib is skipped)" OFF)
set(LOOMORA_X11_INCLUDE_DIRS "" CACHE STRING "Extra include directories of the X11 headers (semicolon separated)")
set(LOOMORA_X11_LIBRARIES "X11" CACHE STRING "Libraries to link for Xlib (names or paths)")

# Does a program using Xlib link with the flags of this build (32-bit or not)? The libraries are
# named plainly: the linker of the build picks the one of its architecture.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${LOOMORA_X11_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${LOOMORA_X11_LIBRARIES})
check_cxx_source_compiles("
#include <X11/Xlib.h>
int main() { return XOpenDisplay(0) != 0; }
" LOOMORA_HAVE_X11)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT LOOMORA_HAVE_X11)
  set(LOOMORA_X11_MESSAGE "libX11 development files not found (or not usable with these compiler flags): loomora_xlib is not built (Debian/Ubuntu: libx11-dev; 32-bit: libx11-dev:i386)")
  if(LOOMORA_XLIB_REQUIRED)
    message(FATAL_ERROR "${LOOMORA_X11_MESSAGE}")
  endif()
  message(STATUS "${LOOMORA_X11_MESSAGE}")
  return()
endif()

set(LOOMORA_XLIB_SOURCES main.cpp keys.cpp keys.hpp probe.cpp probe.hpp xftdl.cpp xftdl.hpp)

add_executable(loomora_xlib ${LOOMORA_XLIB_SOURCES})
set_target_properties(loomora_xlib PROPERTIES
  OUTPUT_NAME loomora-xlib CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_include_directories(loomora_xlib SYSTEM PRIVATE ${LOOMORA_X11_INCLUDE_DIRS})
target_compile_options(loomora_xlib PRIVATE -Wall -Wextra -pedantic)
# --as-needed comes before the libraries: libdl is only named where it is really used.
target_link_options(loomora_xlib PRIVATE -Wl,--as-needed)
target_link_libraries(loomora_xlib PRIVATE loomora ${LOOMORA_X11_LIBRARIES} ${CMAKE_DL_LIBS})
if(LOOMORA_STATIC_CXX)
  target_link_options(loomora_xlib PRIVATE -static-libstdc++ -static-libgcc)
endif()
