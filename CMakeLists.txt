cmake_minimum_required(VERSION 3.13)
project(loomora LANGUAGES C CXX)

option(LOOMORA_BUILD_TESTS "Build the backend self-test" ON)

if(LOOMORA_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)

if(WIN32)
  add_executable(loomora_win32 WIN32 ui/legacy/windows-win32/main.c)
  target_link_libraries(loomora_win32 PRIVATE loomora user32 gdi32)
endif()
