# Native part of the Android Modern UI, built by Gradle (externalNativeBuild, CMake 3.22.1, NDK r30)
# once per ABI: the C++11 backend (core/) as a static library and the JNI glue
# (bridge/jni/loomora_jni.cpp, shared with the Legacy UI) as libloomora_jni.so.
cmake_minimum_required(VERSION 3.13)
project(loomora_android_modern LANGUAGES C CXX)

get_filename_component(LOOMORA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

# The backend's own target property is CXX_STANDARD 11; nothing is set globally.
set(LOOMORA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${LOOMORA_ROOT}/core ${CMAKE_CURRENT_BINARY_DIR}/core)

add_library(loomora_jni SHARED ${LOOMORA_ROOT}/bridge/jni/loomora_jni.cpp)
target_link_libraries(loomora_jni PRIVATE loomora)
target_compile_options(loomora_jni PRIVATE -Wall -Wextra -pedantic)
set_target_properties(loomora_jni PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                                             CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden
                                             VISIBILITY_INLINES_HIDDEN ON)
# 16 KB page size (Android 15+): NDK r28 and later link with it by default; stated for the record.
target_link_options(loomora_jni PRIVATE -Wl,-z,max-page-size=16384)
# Same as the Legacy toolchain file: the symbols of the static libraries (the backend's loomora_*
# functions, libc++ and libc++abi) are not exported, only the Java_fr_bansan_loomora_probe_Native_* ones.
target_link_options(loomora_jni PRIVATE -Wl,--exclude-libs,ALL)
