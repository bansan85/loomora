# Windows Legacy UI: plain Win32 API in C++11 (no MFC), one executable for Windows 2000 -> 11.
#
# The release build is made with MinGW-w64 GCC through cmake/toolchains/mingw-w64-*.cmake
# (tools/build.py windows legacy): -mwindows -static, _WIN32_WINNT=0x0500, linked to the static
# backend libloomora.a. The same sources also build with the host toolchain (for example clang
# with the MSVC ABI), so the default build and ctest cover this target; the GNU-only flags are
# skipped there.
#
# The target is called loomora_win32 because the backend library already owns the name
# "loomora"; the executable file is still loomora.exe (OUTPUT_NAME).

set(LOOMORA_WIN32_SOURCES main.cpp resource.h)
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
  set(LOOMORA_WIN32_MSVC_ABI ON)
else()
  set(LOOMORA_WIN32_MSVC_ABI OFF)
endif()

if(CMAKE_RC_COMPILER)
  # Version information and string table (UTF-8 source, stored as UTF-16). With the MinGW
  # resource compiler the manifest is embedded by the .rc file itself; with the MSVC ABI the
  # linker embeds it (a .manifest source), and the .rc must not carry a second copy.
  list(APPEND LOOMORA_WIN32_SOURCES loomora.rc)
  if(LOOMORA_WIN32_MSVC_ABI)
    list(APPEND LOOMORA_WIN32_SOURCES loomora.manifest)
    # llvm-rc takes the code page of the UTF-8 source from its command line.
    set_source_files_properties(loomora.rc PROPERTIES COMPILE_OPTIONS "/C;65001")
  else()
    set_property(SOURCE loomora.rc APPEND PROPERTY COMPILE_DEFINITIONS LOOMORA_RC_MANIFEST)
    set_source_files_properties(loomora.rc PROPERTIES COMPILE_OPTIONS "--codepage=65001")
    set_source_files_properties(loomora.rc PROPERTIES OBJECT_DEPENDS
                                "${CMAKE_CURRENT_SOURCE_DIR}/loomora.manifest")
  endif()
else()
  message(WARNING "No resource compiler found: loomora_win32 is built without loomora.rc")
endif()

add_executable(loomora_win32 WIN32 ${LOOMORA_WIN32_SOURCES})
set_target_properties(loomora_win32 PROPERTIES
  OUTPUT_NAME loomora CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_definitions(loomora_win32 PRIVATE
  UNICODE _UNICODE _WIN32_WINNT=0x0500 WINVER=0x0500)
target_link_libraries(loomora_win32 PRIVATE loomora user32 gdi32)

if(NOT LOOMORA_WIN32_MSVC_ABI)
  target_compile_options(loomora_win32 PRIVATE -Wall -Wextra -pedantic)
  target_link_options(loomora_win32 PRIVATE -mwindows -static)
endif()
