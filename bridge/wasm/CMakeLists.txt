# The Web module: libloomora (C++11) compiled by Emscripten and its six C functions exported
# (bridge/wasm/exports.txt, passed to the linker by cmake/toolchains/emscripten-wasm.cmake).
# Only built with an Emscripten toolchain file (LOOMORA_TARGET_WEB).
#
# Output, in the build directory:
#   loomora.js + loomora.wasm     WebAssembly (emscripten-wasm.cmake)
#   loomora.js                    JavaScript only (emscripten-js.cmake, LOOMORA_WEB_JS)
# The page calls createLoomora() (a factory function, MODULARIZE) and then ccall / cwrap.

# Emscripten needs one translation unit to link a module; the code is all in libloomora.
add_executable(loomora_web module.cpp)
target_link_libraries(loomora_web PRIVATE loomora)
set_target_properties(loomora_web PROPERTIES OUTPUT_NAME loomora
                                             CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
# The two lists (exports.txt, runtime-methods.txt) are JSON arrays: that is the one form of a list
# file that every Emscripten from 1.39 to the current one reads.
set_target_properties(loomora_web PROPERTIES LINK_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/exports.txt;${CMAKE_CURRENT_SOURCE_DIR}/runtime-methods.txt")
# No main(): the module only exports functions. ENVIRONMENT web,worker,node: the page, a worker,
# and node (the checks of tools/ run the module under node).
target_link_options(loomora_web PRIVATE
  -sMODULARIZE=1 -sEXPORT_NAME=createLoomora --no-entry
  "-sEXPORTED_RUNTIME_METHODS=@${CMAKE_CURRENT_SOURCE_DIR}/runtime-methods.txt"
  -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web,worker,node)
