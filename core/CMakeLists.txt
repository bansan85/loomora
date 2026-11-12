set(LOOMORA_SOURCES
  src/c_api.cpp src/eol.cpp src/greet.cpp src/probe.cpp src/utf8.cpp src/version.cpp)

add_library(loomora STATIC ${LOOMORA_SOURCES})
target_include_directories(loomora PUBLIC include)
set_target_properties(loomora PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                                         CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(loomora PRIVATE -Wall -Wextra -pedantic)
endif()

# Shared library (loomora.dll for WinUI, libloomora.so / .dylib elsewhere). Only the loomora_*
# functions marked LOOMORA_API are exported; tools/check_std.py verifies it.
option(LOOMORA_BUILD_SHARED "Also build the shared library loomora_shared" OFF)
if(LOOMORA_BUILD_SHARED)
  add_library(loomora_shared SHARED ${LOOMORA_SOURCES})
  target_include_directories(loomora_shared PUBLIC include)
  target_compile_definitions(loomora_shared PRIVATE LOOMORA_BUILD_SHARED_LIB)
  target_compile_definitions(loomora_shared INTERFACE LOOMORA_USE_DLL)
  set_target_properties(loomora_shared PROPERTIES
    OUTPUT_NAME loomora CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(loomora_shared PRIVATE -Wall -Wextra -pedantic)
  endif()
  # -fvisibility=hidden does not hide the std templates (libstdc++ marks namespace std as
  # default-visibility): the export list is enforced at link time too.
  if(APPLE)
    set_target_properties(loomora_shared PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/loomora.exp")
    target_link_options(loomora_shared PRIVATE
      "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/loomora.exp")
  elseif(UNIX)
    set_target_properties(loomora_shared PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/loomora.map")
    target_link_options(loomora_shared PRIVATE
      "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/loomora.map")
  endif()
endif()

install(FILES include/loomora.h DESTINATION include)
install(TARGETS loomora ARCHIVE DESTINATION lib)
if(LOOMORA_BUILD_SHARED)
  install(TARGETS loomora_shared ARCHIVE DESTINATION lib LIBRARY DESTINATION lib
                  RUNTIME DESTINATION bin)
endif()

if(LOOMORA_BUILD_TESTS)
  add_subdirectory(tests)
endif()
