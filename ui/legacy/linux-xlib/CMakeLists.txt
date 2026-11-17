# Linux Legacy UI: C++11 and Xlib, one plain executable (loomora-xlib).
#
# Built with the top-level project (the backend library "loomora" of core/ is linked into it):
#
#   cmake -S . -B ~/loomora-build/linux-legacy -DCMAKE_BUILD_TYPE=Release -DLOOMORA_BUILD_TESTS=OFF
#   cmake --build ~/loomora-build/linux-legacy --target loomora_xlib
#
# The program is ALWAYS built on the machine it runs on (docs/implementation/adr/0007), natively:
# tools/build.py linux legacy does it for x86_64 and i686. The 32-bit build on a 64-bit host takes
# the toolchain file cmake/toolchains/linux-gcc-i686.cmake (gcc -m32; needs the 32-bit development
# files, see below). What the machine needs: a C++11 compiler, CMake and the libX11 development
# files, and the libXft and fontconfig ones for the normal text path.
#
# Libraries: libX11, and libXft with fontconfig when CMake finds them (pkg-config: xft and
# fontconfig). The C++ runtime (libstdc++, libgcc) is the system's own, linked dynamically.
#
# TEXT. LOOMORA_XLIB_XFT=AUTO (default) draws with Xft when CMake finds it, and defines
# LOOMORA_XLIB_HAVE_XFT for the sources. When CMake does NOT find it, the UI is built with the core
# X11 font path (a '?' for a character the font lacks, the marker "glyph-substitution" in the
# probe line): that is the only case of the fallback xlib-core-font-glyphs of
# docs/implementation/validation/fallbacks.json. LOOMORA_XLIB_XFT=ON makes a missing Xft an
# error; OFF builds the core-font variant on purpose (to test it).
#
# 32-bit build on Ubuntu / Debian: sudo dpkg --add-architecture i386 && sudo apt update &&
#   sudo apt install gcc-multilib g++-multilib libx11-dev:i386 libxft-dev:i386 libfontconfig-dev:i386
# (the last two are optional: without them the 32-bit program is built with the core font path).

set(LOOMORA_XLIB_XFT "AUTO" CACHE STRING "Text with Xft: AUTO (when CMake finds libXft and fontconfig), ON (error when missing) or OFF (core X11 font only, to test it)")
set_property(CACHE LOOMORA_XLIB_XFT PROPERTY STRINGS AUTO ON OFF)
option(LOOMORA_XLIB_REQUIRED "Fail when the libX11 development files are missing (otherwise loomora_xlib is skipped)" OFF)
set(LOOMORA_X11_INCLUDE_DIRS "" CACHE STRING "Extra include directories of the X11 headers (semicolon separated)")
set(LOOMORA_X11_LIBRARIES "X11" CACHE STRING "Libraries to link for Xlib (names or paths)")

if(NOT LOOMORA_XLIB_XFT STREQUAL "AUTO" AND NOT LOOMORA_XLIB_XFT STREQUAL "ON" AND NOT LOOMORA_XLIB_XFT STREQUAL "OFF")
  message(FATAL_ERROR "LOOMORA_XLIB_XFT must be AUTO, ON or OFF, not \"${LOOMORA_XLIB_XFT}\"")
endif()

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

# Xft and fontconfig: pkg-config finds them, then a program that uses them is linked with the
# flags of this build, so that a 64-bit library never reaches a 32-bit link.
set(LOOMORA_XLIB_HAVE_XFT OFF)
if(NOT LOOMORA_XLIB_XFT STREQUAL "OFF")
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(LOOMORA_XFT QUIET xft fontconfig)
  endif()
  if(LOOMORA_XFT_FOUND)
    set(CMAKE_REQUIRED_INCLUDES ${LOOMORA_X11_INCLUDE_DIRS} ${LOOMORA_XFT_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${LOOMORA_X11_LIBRARIES} ${LOOMORA_XFT_LINK_LIBRARIES})
    check_cxx_source_compiles("
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
int main() { return FcInit() && XftFontOpenName(0, 0, \"sans\") != 0; }
" LOOMORA_HAVE_XFT)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
    set(LOOMORA_XLIB_HAVE_XFT ${LOOMORA_HAVE_XFT})
  endif()
  if(LOOMORA_XLIB_HAVE_XFT)
    message(STATUS "Linux Legacy UI: text drawn with Xft (xft ${LOOMORA_XFT_xft_VERSION}, fontconfig ${LOOMORA_XFT_fontconfig_VERSION})")
  else()
    set(LOOMORA_XFT_MESSAGE "libXft and fontconfig development files not found (or not usable with these compiler flags; Debian/Ubuntu: libxft-dev libfontconfig-dev; 32-bit: libxft-dev:i386 libfontconfig-dev:i386)")
    if(LOOMORA_XLIB_XFT STREQUAL "ON")
      message(FATAL_ERROR "${LOOMORA_XFT_MESSAGE}")
    endif()
    message(STATUS "${LOOMORA_XFT_MESSAGE}")
  endif()
endif()
# What tools/build.py reads back to record and check the build.
if(LOOMORA_XLIB_HAVE_XFT)
  set(LOOMORA_XLIB_TEXT_PATH "xft" CACHE INTERNAL "Text path of loomora_xlib: xft or core-font")
  set(LOOMORA_XLIB_XFT_INFO "xft ${LOOMORA_XFT_xft_VERSION}, fontconfig ${LOOMORA_XFT_fontconfig_VERSION}" CACHE INTERNAL "Versions of the Xft libraries found")
else()
  set(LOOMORA_XLIB_TEXT_PATH "core-font" CACHE INTERNAL "Text path of loomora_xlib: xft or core-font")
  set(LOOMORA_XLIB_XFT_INFO "" CACHE INTERNAL "Versions of the Xft libraries found")
endif()
if(NOT LOOMORA_XLIB_HAVE_XFT)
  message(STATUS "Linux Legacy UI: built with the CORE FONT text path (fallback xlib-core-font-glyphs: '?' for a character the core font lacks, probe marker glyph-substitution)")
endif()

set(LOOMORA_XLIB_SOURCES main.cpp keys.cpp keys.hpp probe.cpp probe.hpp project_probe.cpp project_probe.hpp)
if(LOOMORA_XLIB_HAVE_XFT)
  list(APPEND LOOMORA_XLIB_SOURCES xftfonts.cpp xftfonts.hpp)
endif()

add_executable(loomora_xlib ${LOOMORA_XLIB_SOURCES})
set_target_properties(loomora_xlib PROPERTIES
  OUTPUT_NAME loomora-xlib CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_include_directories(loomora_xlib SYSTEM PRIVATE ${LOOMORA_X11_INCLUDE_DIRS})
# The round trip mode (--roundtrip-project) calls loomora_project_export_stream: it is compiled when
# the backend has it (core/src/c_api_project_export.cpp), so that this UI builds before and after.
if(EXISTS "${PROJECT_SOURCE_DIR}/core/src/c_api_project_export.cpp")
  target_compile_definitions(loomora_xlib PRIVATE LOOMORA_HAVE_PROJECT_EXPORT)
endif()
target_compile_options(loomora_xlib PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(loomora_xlib PRIVATE loomora ${LOOMORA_X11_LIBRARIES})
if(LOOMORA_XLIB_HAVE_XFT)
  target_include_directories(loomora_xlib SYSTEM PRIVATE ${LOOMORA_XFT_INCLUDE_DIRS})
  target_compile_definitions(loomora_xlib PRIVATE LOOMORA_XLIB_HAVE_XFT=1)
  target_link_libraries(loomora_xlib PRIVATE ${LOOMORA_XFT_LINK_LIBRARIES})
endif()
