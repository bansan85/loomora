# JavaScript (no WebAssembly) toolchain of the Legacy Web UI, for browsers that have no
# WebAssembly: Internet Explorer 9, Firefox 8, Chrome 15, Safari 5.1, Opera 11.5 (research R8,
# spike S5). The same C++11 sources and the same exports as emscripten-wasm.cmake, built to a
# plain loomora.js: `-sWASM=0 -sLEGACY_VM_SUPPORT=1`.
#
#   source ~/emsdk/emsdk_env.sh
#   cmake -S . -B ~/loomora-build/web-js -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/emscripten-js.cmake
#
# or: cmake --preset web-js
#
# Parameters: those of emscripten-wasm.cmake (LOOMORA_EMSCRIPTEN_ROOT, ...), which this file
# includes first. With the current Emscripten, WASM=0 is deprecated (a warning, then the module is
# JavaScript made by wasm2js); with Emscripten 1.39.20 fastcomp it is asm.js. What each version
# produces and runs is recorded in spike S5 (docs/implementation/validation/spikes.json).
set(LOOMORA_WEB_JS ON)
include("${CMAKE_CURRENT_LIST_DIR}/emscripten-wasm.cmake")

# -sWASM=0 and -sLEGACY_VM_SUPPORT=1 are link settings; a fastcomp Emscripten (1.x) also needs
# -sWASM=0 when compiling, so it picks the asm.js target instead of the WebAssembly one.
loomora_add_init_flag(CMAKE_EXE_LINKER_FLAGS_INIT "-sWASM=0 -sLEGACY_VM_SUPPORT=1")
