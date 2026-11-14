cmake_minimum_required(VERSION 3.13)
project(loomora LANGUAGES C CXX)

option(LOOMORA_BUILD_TESTS "Build the backend self-test" ON)

if(LOOMORA_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)

if(WIN32)
  add_subdirectory(ui/legacy/windows-win32)
endif()

# Linux Legacy UI (Xlib): skipped, with a message, when the libX11 development files are missing
# (LOOMORA_XLIB_REQUIRED=ON makes that an error).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(ui/legacy/linux-xlib)
endif()
