# Linux Modern UI: C++ with gtkmm (the system packages), plain executable, no Flatpak.
#
#   cmake -S . -B ~/loomora-build/linux-modern -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DLOOMORA_BUILD_LINUX_MODERN=ON
#   cmake --build ~/loomora-build/linux-modern      ->  ~/loomora-build/linux-modern/loomora-gtk3
#
# The C++11 backend of core/ is the static library libloomora (core/CMakeLists.txt, which forces
# CXX_STANDARD 11 on it): the UI talks to it only through the C API of core/include/loomora.h.
# The UI target has its own language standard, set as a property of that target only (never
# CMAKE_CXX_STANDARD): C++17 with gtkmm-4.0, which needs it (sigc++-3), and C++11 with gtkmm-3.0,
# the lowest standard whose headers (gtkmm 3.24, glibmm 2.66, sigc++ 2.12) compile; the UI needs
# nothing newer than C++11 itself. The C++ runtime (libstdc++, libgcc) is the system's own, linked
# dynamically (gtkmm needs libstdc++.so.6 in any case). The program is built on the machine it
# runs on (ADR 0007).
#
# GTK version policy (CLAUDE.md, docs/implementation/adr/0005-gtk-version-policy.md, 0006): the
# UI always compiles with GDK_VERSION_MIN_REQUIRED at 3.10. LOOMORA_GTK_API picks GTK 3 with
# gtkmm-3.0 (default) or GTK 4 with gtkmm-4.0; LOOMORA_GTK_FLOOR=ON builds the 3.10 path only and
# makes the compiler reject every GTK/GDK symbol newer than 3.10 (the floor build). Newer
# features are enabled by the macros of gtk_compat.hpp, from the version of the headers, and
# declared in docs/implementation/validation/fallbacks.json.

set(LOOMORA_GTK_API "3" CACHE STRING "GTK major version to build against: 3 (gtkmm-3.0) or 4 (gtkmm-4.0)")
set_property(CACHE LOOMORA_GTK_API PROPERTY STRINGS 3 4)
# Floor build: GTK 3 only, GDK_VERSION_MAX_ALLOWED pinned to 3.10 and
# -Werror=deprecated-declarations, so that any GTK or GDK symbol newer than 3.10 fails the build
# and no version-gated feature is compiled. It proves the 3.10 path; it is not the default.
option(LOOMORA_GTK_FLOOR "Build the GTK 3.10 path only and fail on any newer GTK/GDK symbol" OFF)

if(NOT LOOMORA_GTK_API STREQUAL "3" AND NOT LOOMORA_GTK_API STREQUAL "4")
  message(FATAL_ERROR "LOOMORA_GTK_API must be 3 or 4, not \"${LOOMORA_GTK_API}\"")
endif()
if(LOOMORA_GTK_API STREQUAL "4" AND LOOMORA_GTK_FLOOR)
  message(FATAL_ERROR "LOOMORA_GTK_FLOOR proves the GTK 3.10 path: it cannot be combined with LOOMORA_GTK_API=4")
endif()

find_package(PkgConfig REQUIRED)
if(LOOMORA_GTK_API STREQUAL "4")
  pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0>=4.0)
  set(LOOMORA_GTK_UI_STANDARD 17)
else()
  pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-3.0>=3.10)
  set(LOOMORA_GTK_UI_STANDARD 11)
endif()
message(STATUS "Linux Modern UI: gtkmm ${GTKMM_VERSION} (GTK ${LOOMORA_GTK_API}), C++${LOOMORA_GTK_UI_STANDARD}")

add_executable(loomora_gtk3 main.cpp main_window.cpp main_window.hpp probe.cpp probe.hpp
               project_probe.cpp project_probe.hpp gtk_compat.hpp)
set_target_properties(loomora_gtk3 PROPERTIES
  OUTPUT_NAME loomora-gtk3
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  CXX_STANDARD ${LOOMORA_GTK_UI_STANDARD} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_link_libraries(loomora_gtk3 PRIVATE loomora PkgConfig::GTKMM)
target_compile_options(loomora_gtk3 PRIVATE -Wall -Wextra)
# The round trip mode (--roundtrip-project) calls loomora_project_export_stream: it is compiled when
# the backend has it (core/src/c_api_project_export.cpp), so that this UI builds before and after.
if(EXISTS "${PROJECT_SOURCE_DIR}/core/src/c_api_project_export.cpp")
  target_compile_definitions(loomora_gtk3 PRIVATE LOOMORA_HAVE_PROJECT_EXPORT)
endif()

if(LOOMORA_GTK_API STREQUAL "3")
  target_compile_definitions(loomora_gtk3 PRIVATE GDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_10)
  if(LOOMORA_GTK_FLOOR)
    target_compile_definitions(loomora_gtk3 PRIVATE GDK_VERSION_MAX_ALLOWED=GDK_VERSION_3_10)
    target_compile_options(loomora_gtk3 PRIVATE -Werror=deprecated-declarations)
  endif()
endif()

install(TARGETS loomora_gtk3 RUNTIME DESTINATION bin)
