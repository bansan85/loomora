# The host command `loomora validate` (spec 002, contracts/cli-validate.md). Added by the top-level
# CMakeLists.txt only for host builds. It calls the public C API of the backend (loomora.h) and,
# for the hidden test-only option --set-limit, one internal function of the static library
# (core/src/format/limits.hpp); nothing is exported from the shared library for it.
add_executable(loomora_cli main.cpp)
target_include_directories(loomora_cli PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
target_link_libraries(loomora_cli PRIVATE loomora)
# On Windows the file name loomora.exe is the Legacy UI (ui/legacy/windows-win32): the command is
# loomora-cli.exe there, loomora everywhere else.
if(WIN32)
  set(LOOMORA_CLI_NAME loomora-cli)
  target_link_libraries(loomora_cli PRIVATE shell32)
else()
  set(LOOMORA_CLI_NAME loomora)
endif()
set_target_properties(loomora_cli PROPERTIES OUTPUT_NAME ${LOOMORA_CLI_NAME}
  CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(loomora_cli PRIVATE -Wall -Wextra -pedantic)
endif()
install(TARGETS loomora_cli RUNTIME DESTINATION bin)
