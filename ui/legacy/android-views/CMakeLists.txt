# Android Legacy UI (Java, Android framework views, no AndroidX): the native part is one
# libloomora_jni.so per ABI = the JNI glue of bridge/jni/ + the static backend libloomora (core/).
# The Java side (AndroidManifest.xml, src/) is packaged by build_apk.py, which is driven by
# tools/build.py android legacy; this file is only used with cmake/toolchains/android-ndk-r15c.cmake:
#
#   cmake -S . -B build/android-armeabi -G Ninja -DLOOMORA_BUILD_TESTS=OFF \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android-ndk-r15c.cmake \
#         -DLOOMORA_ANDROID_NDK=<NDK r15c> -DANDROID_ABI=armeabi
#   cmake --build build/android-armeabi --target loomora_jni

add_library(loomora_jni SHARED ${PROJECT_SOURCE_DIR}/bridge/jni/loomora_jni.cpp)
target_link_libraries(loomora_jni PRIVATE loomora)
target_compile_options(loomora_jni PRIVATE -Wall -Wextra -pedantic)
set_target_properties(loomora_jni PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                                             CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden
                                             VISIBILITY_INLINES_HIDDEN ON)
