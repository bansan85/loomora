# JavaScript (no WebAssembly) toolchain of the Legacy Web UI for the oldest browsers: the same as
# emscripten-js.cmake, built with Emscripten 1.39.20 FASTCOMP (spike S5). That release makes
# asm.js whose glue parses as ECMAScript 3; the current Emscripten makes ECMAScript 2021 syntax
# that a Firefox 8 to 12 cannot even parse. Typed arrays are still needed (Firefox 4 and later,
# Chrome 7 and later, Internet Explorer 10 and later have them).
#
#   export EM_CONFIG=$HOME/emsdk-fastcomp/.emscripten
#   export PATH=$HOME/emsdk-fastcomp/fastcomp/emscripten:$HOME/emsdk-fastcomp/node/14.18.2_64bit/bin:$PATH
#   cmake -S . -B ~/loomora-build/web-js-fastcomp -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/emscripten-js-fastcomp.cmake
#
# or: cmake --preset web-js-fastcomp (same environment). Without LOOMORA_EMSCRIPTEN_ROOT the
# platform file is looked for at $EMSDK/fastcomp/emscripten, then at emcc on the PATH
# (see emscripten-wasm.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/emscripten-js.cmake")

# --memory-init-file 0: the memory initialiser stays inside loomora.js instead of a separate
# loomora.js.mem file, so the page loads one file and needs no XMLHttpRequest for it (a .mem file
# cannot be read over file:// by every browser).
loomora_add_init_flag(CMAKE_EXE_LINKER_FLAGS_INIT "--memory-init-file 0")
