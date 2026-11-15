# WebAssembly toolchain of the Web UIs: the current Emscripten (the one of the emsdk), no
# JavaScript fallback in this file (see emscripten-js.cmake for that).
#
#   source ~/emsdk/emsdk_env.sh
#   cmake -S . -B ~/loomora-build/web-wasm -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/emscripten-wasm.cmake
#
# or, with the presets of CMakePresets.json: cmake --preset web-wasm
#
# Parameter: LOOMORA_EMSCRIPTEN_ROOT, the directory that holds emcc and cmake/Modules/Platform/
# Emscripten.cmake (variable or environment variable of the same name). Without it the file looks
# at the environment of emsdk_env.sh: $EMSDK/upstream/emscripten (Emscripten 2.x and later),
# $EMSDK/fastcomp/emscripten or $EMSCRIPTEN (older ones), then at emcc on the PATH.
#
# The platform file of Emscripten does the cross-compiling work (emcc / em++ / emar, the ".js"
# executable suffix, node as CMAKE_CROSSCOMPILING_EMULATOR so that ctest runs the self-test). This
# file adds what is specific to the project:
#   * LOOMORA_TARGET_WEB is TRUE (top-level CMakeLists.txt then adds bridge/wasm);
#   * LOOMORA_WEB_JS is OFF here (emscripten-js.cmake turns it ON);
#   * the exported functions are listed in bridge/wasm/exports.txt (the C API of loomora.h).

set(LOOMORA_TARGET_WEB TRUE)
if(NOT DEFINED LOOMORA_WEB_JS)
  set(LOOMORA_WEB_JS OFF)
endif()
# The test projects of CMake (compiler detection) read the same toolchain file.
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LOOMORA_EMSCRIPTEN_ROOT LOOMORA_WEB_JS)

if(NOT LOOMORA_EMSCRIPTEN_ROOT AND DEFINED ENV{LOOMORA_EMSCRIPTEN_ROOT})
  set(LOOMORA_EMSCRIPTEN_ROOT "$ENV{LOOMORA_EMSCRIPTEN_ROOT}")
endif()
if(NOT LOOMORA_EMSCRIPTEN_ROOT AND DEFINED ENV{EMSDK})
  foreach(_dir "$ENV{EMSDK}/upstream/emscripten" "$ENV{EMSDK}/fastcomp/emscripten")
    if(NOT LOOMORA_EMSCRIPTEN_ROOT AND EXISTS "${_dir}/emcc")
      set(LOOMORA_EMSCRIPTEN_ROOT "${_dir}")
    endif()
  endforeach()
endif()
if(NOT LOOMORA_EMSCRIPTEN_ROOT AND DEFINED ENV{EMSCRIPTEN} AND EXISTS "$ENV{EMSCRIPTEN}/emcc")
  set(LOOMORA_EMSCRIPTEN_ROOT "$ENV{EMSCRIPTEN}")
endif()
if(NOT LOOMORA_EMSCRIPTEN_ROOT)
  find_program(_loomora_emcc emcc)
  if(_loomora_emcc)
    get_filename_component(_loomora_emcc "${_loomora_emcc}" REALPATH)
    get_filename_component(LOOMORA_EMSCRIPTEN_ROOT "${_loomora_emcc}" DIRECTORY)
  endif()
endif()
set(_loomora_platform "${LOOMORA_EMSCRIPTEN_ROOT}/cmake/Modules/Platform/Emscripten.cmake")
if(NOT LOOMORA_EMSCRIPTEN_ROOT OR NOT EXISTS "${_loomora_platform}")
  message(FATAL_ERROR "emscripten-wasm.cmake: Emscripten not found. Run `source <emsdk>/emsdk_env.sh` "
                      "or give -DLOOMORA_EMSCRIPTEN_ROOT=<dir with emcc and cmake/Modules/Platform/"
                      "Emscripten.cmake>")
endif()
include("${_loomora_platform}")

set(LOOMORA_WEB_EXPORTS "${CMAKE_CURRENT_LIST_DIR}/../../bridge/wasm/exports.txt")
get_filename_component(LOOMORA_WEB_EXPORTS "${LOOMORA_WEB_EXPORTS}" ABSOLUTE)

# C++ exceptions. The backend throws and catches inside loomora_* (the C boundary catches every
# exception, S6); Emscripten's default is -fno-exceptions, where a throw aborts the module. The
# JavaScript-based mechanism (-fexceptions) is used for both builds: it works in every
# WebAssembly browser and with -sWASM=0, whereas -fwasm-exceptions needs a recent engine (Chrome
# 95, Firefox 100, Safari 15.2) and does not exist for the JavaScript build.
# A toolchain file is read more than once per configure: add each flag only the first time.
function(loomora_add_init_flag variable flag)
  if(NOT "${${variable}}" MATCHES "${flag}")
    set(${variable} "${${variable}} ${flag}" PARENT_SCOPE)
  endif()
endfunction()
loomora_add_init_flag(CMAKE_C_FLAGS_INIT "-fexceptions")
loomora_add_init_flag(CMAKE_CXX_FLAGS_INIT "-fexceptions")
loomora_add_init_flag(CMAKE_EXE_LINKER_FLAGS_INIT "-fexceptions")

# Link flag of every executable of this build (the module and loomora_selftest): the six
# loomora_* functions of the C API, with the "_" prefix of the C symbols, are kept and exported.
# The flags that only the module needs (runtime methods, factory name, no main) are in
# bridge/wasm/CMakeLists.txt.
loomora_add_init_flag(CMAKE_EXE_LINKER_FLAGS_INIT "-sEXPORTED_FUNCTIONS=@${LOOMORA_WEB_EXPORTS}")
