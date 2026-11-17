cmake_minimum_required(VERSION 3.13)
# CMP0091 NEW makes MSVC_RUNTIME_LIBRARY (the /MT of loomora.dll) take effect.
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
project(loomora LANGUAGES C CXX)

option(LOOMORA_BUILD_TESTS "Build the backend self-test" ON)
option(LOOMORA_BUILD_WIN32_LEGACY "Build the Windows Legacy UI (ui/legacy/windows-win32) on a Windows host" ON)
option(LOOMORA_BUILD_LINUX_MODERN "Build the Linux Modern UI (C++, gtkmm; needs its development files)" OFF)

if(LOOMORA_BUILD_TESTS)
  enable_testing()
endif()

# zlib (submodule third_party/zlib; deflate/inflate of the .loomora format), before the core that links it.
include(cmake/loomora_zlib.cmake)
add_subdirectory(core)

# The host-msvc preset (loomora.dll for the WinUI 3 UI) turns the Legacy UI off: it is built with
# MinGW-w64 by tools/build.py windows legacy.
if(WIN32 AND LOOMORA_BUILD_WIN32_LEGACY)
  add_subdirectory(ui/legacy/windows-win32)
endif()

# Linux Legacy UI (Xlib): skipped, with a message, when the libX11 development files are missing
# (LOOMORA_XLIB_REQUIRED=ON makes that an error).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(ui/legacy/linux-xlib)
endif()

# Android Legacy UI (JNI glue + backend): only with cmake/toolchains/android-ndk-r15c.cmake.
if(LOOMORA_TARGET_ANDROID)
  add_subdirectory(ui/legacy/android-views)
endif()

# Web module (loomora.js and loomora.wasm): only with cmake/toolchains/emscripten-wasm.cmake or
# emscripten-js.cmake.
if(LOOMORA_TARGET_WEB)
  add_subdirectory(bridge/wasm)
endif()

if(LOOMORA_BUILD_LINUX_MODERN)
  add_subdirectory(ui/modern/linux-gtk3)
endif()
