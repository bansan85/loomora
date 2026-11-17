# Android toolchain of the Legacy UI: NDK r15c, API level 9 (Android 2.3), built with Clang 5 and
# the unified headers, WITHOUT ndk-build and WITHOUT the CMake support of the NDK.
#
#   cmake -S . -B build/android-armeabi -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android-ndk-r15c.cmake \
#         -DLOOMORA_ANDROID_NDK=E:/Program/Android/android-ndk-r15c -DANDROID_ABI=armeabi
#
# Why not ndk-build / Android.mk (docs/implementation/adr/0016-android-legacy-ndk-r15c.md): r16b
# ships platforms from android-14 up, and the ndk-build of r15c raises every APP_PLATFORM below 14
# to 14 (build/core/init.mk, NDK_MIN_PLATFORM_LEVEL := 14), so neither of them produces an API 9
# library. r15c still carries platforms/android-9 (arch-arm, arch-mips, arch-x86), which Clang
# accepts as a plain --sysroot: this file does what ndk-build would do, without the clamp.
#
# Parameters:
#   LOOMORA_ANDROID_NDK  path of the r15c NDK (or the environment variable of the same name)
#   ANDROID_ABI          armeabi (default), armeabi-v7a, x86, x86_64 or arm64-v8a
#                        (mips builds too, but the spec has no MIPS platform: not shipped)
#   LOOMORA_ANDROID_PIE  ON: position independent code and executables (-fPIE, -pie). Android 5.0 (API 21)
#                        and later refuse to run a non-PIE executable, and Android before 4.1 (API 16) cannot
#                        run a PIE one: the test programs (loomora_selftest, loomora_format_selftest,
#                        loomora_refgen) of the x86 ABI are built twice, with LOOMORA_ANDROID_API=9 (not PIE,
#                        for the API 16 emulator) and with LOOMORA_ANDROID_API=16 and PIE ON (for API 21 and
#                        later). The libraries of the APK are shared objects: not affected. Default OFF.
#   LOOMORA_ANDROID_API  API level of the sysroot (default 9; 21 for arm64-v8a and x86_64, the first level
#                        that has the 64-bit ABI, so the 64-bit library is a 21+ library)
#
# The C++ runtime is gnustl linked STATICALLY into the library (libgnustl_static.a, libsupc++.a):
# Android 2.3 has no C++ library but the tiny system libstdc++.so (new / delete only), and a
# shared STL would have to be shipped next to the library. The backend is a static library of
# this project (core/), the Java UI loads one libloomora_jni.so per ABI.
#
# Consumers can test LOOMORA_TARGET_ANDROID (the CMake variable ANDROID is not set: this is a
# plain Linux cross build).

set(CMAKE_SYSTEM_NAME Linux)
# The test projects of CMake (compiler detection) read the same toolchain file.
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LOOMORA_ANDROID_NDK ANDROID_ABI LOOMORA_ANDROID_API
    LOOMORA_ANDROID_PIE)
set(LOOMORA_TARGET_ANDROID TRUE)

if(NOT LOOMORA_ANDROID_NDK AND DEFINED ENV{LOOMORA_ANDROID_NDK})
  set(LOOMORA_ANDROID_NDK "$ENV{LOOMORA_ANDROID_NDK}")
endif()
if(NOT LOOMORA_ANDROID_NDK)
  message(FATAL_ERROR "android-ndk-r15c.cmake: give the NDK r15c directory with "
                      "-DLOOMORA_ANDROID_NDK=<dir> (or the environment variable)")
endif()
if(NOT ANDROID_ABI)
  set(ANDROID_ABI armeabi)
endif()
if(NOT LOOMORA_ANDROID_API)
  set(LOOMORA_ANDROID_API 9)
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(_host_tag windows-x86_64)
  set(_exe ".exe")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(_host_tag darwin-x86_64)
  set(_exe "")
else()
  set(_host_tag linux-x86_64)
  set(_exe "")
endif()

# ABI -> (clang target, sysroot architecture, GCC toolchain directory and tool prefix, flags)
if(ANDROID_ABI STREQUAL "armeabi")
  set(_target armv5te-none-linux-androideabi)
  set(_arch arm)
  set(_gcc_dir arm-linux-androideabi-4.9)
  set(_tool_prefix arm-linux-androideabi-)
  set(_include_triple arm-linux-androideabi)
  set(CMAKE_SYSTEM_PROCESSOR armv5te)
  set(_abi_flags "-march=armv5te -mtune=xscale -mfloat-abi=soft -mthumb")
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
  set(_target armv7-none-linux-androideabi)
  set(_arch arm)
  set(_gcc_dir arm-linux-androideabi-4.9)
  set(_tool_prefix arm-linux-androideabi-)
  set(_include_triple arm-linux-androideabi)
  set(CMAKE_SYSTEM_PROCESSOR armv7-a)
  set(_abi_flags "-march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16 -mthumb")
elseif(ANDROID_ABI STREQUAL "x86")
  set(_target i686-none-linux-android)
  set(_arch x86)
  set(_gcc_dir x86-4.9)
  set(_tool_prefix i686-linux-android-)
  set(_include_triple i686-linux-android)
  set(CMAKE_SYSTEM_PROCESSOR i686)
  set(_abi_flags "-mstackrealign")
elseif(ANDROID_ABI STREQUAL "arm64-v8a")
  set(_target aarch64-none-linux-android)
  set(_arch arm64)
  set(_gcc_dir aarch64-linux-android-4.9)
  set(_tool_prefix aarch64-linux-android-)
  set(_include_triple aarch64-linux-android)
  set(CMAKE_SYSTEM_PROCESSOR aarch64)
  set(_abi_flags "")
  if(LOOMORA_ANDROID_API LESS 21)
    set(LOOMORA_ANDROID_API 21)
  endif()
elseif(ANDROID_ABI STREQUAL "x86_64")
  # Not in the APK of the Legacy UI (its range stops at API 23, where x86_64 devices run the x86 library);
  # built for the test programs run natively on the x86_64 emulators (API 24 and 35).
  set(_target x86_64-none-linux-android)
  set(_arch x86_64)
  set(_gcc_dir x86_64-4.9)
  set(_tool_prefix x86_64-linux-android-)
  set(_include_triple x86_64-linux-android)
  set(CMAKE_SYSTEM_PROCESSOR x86_64)
  set(_abi_flags "")
  if(LOOMORA_ANDROID_API LESS 21)
    set(LOOMORA_ANDROID_API 21)
  endif()
elseif(ANDROID_ABI STREQUAL "mips")
  set(_target mipsel-none-linux-android)
  set(_arch mips)
  set(_gcc_dir mipsel-linux-android-4.9)
  set(_tool_prefix mipsel-linux-android-)
  set(_include_triple mipsel-linux-android)
  set(CMAKE_SYSTEM_PROCESSOR mips)
  set(_abi_flags "-mips32")
else()
  message(FATAL_ERROR "android-ndk-r15c.cmake: unsupported ANDROID_ABI '${ANDROID_ABI}'")
endif()

set(_llvm_bin "${LOOMORA_ANDROID_NDK}/toolchains/llvm/prebuilt/${_host_tag}/bin")
set(_gcc_root "${LOOMORA_ANDROID_NDK}/toolchains/${_gcc_dir}/prebuilt/${_host_tag}")
set(_sysroot "${LOOMORA_ANDROID_NDK}/platforms/android-${LOOMORA_ANDROID_API}/arch-${_arch}")
set(_unified "${LOOMORA_ANDROID_NDK}/sysroot/usr/include")
set(_stl "${LOOMORA_ANDROID_NDK}/sources/cxx-stl/gnu-libstdc++/4.9")
foreach(_dir "${_llvm_bin}" "${_gcc_root}" "${_sysroot}" "${_stl}")
  if(NOT EXISTS "${_dir}")
    message(FATAL_ERROR "android-ndk-r15c.cmake: ${_dir} does not exist (is LOOMORA_ANDROID_NDK "
                        "an NDK r15c, and does it have an android-${LOOMORA_ANDROID_API} "
                        "platform for ${ANDROID_ABI}?)")
  endif()
endforeach()

set(CMAKE_C_COMPILER "${_llvm_bin}/clang${_exe}")
set(CMAKE_CXX_COMPILER "${_llvm_bin}/clang++${_exe}")
set(CMAKE_AR "${_gcc_root}/bin/${_tool_prefix}ar${_exe}" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_gcc_root}/bin/${_tool_prefix}ranlib${_exe}" CACHE FILEPATH "")
set(CMAKE_STRIP "${_gcc_root}/bin/${_tool_prefix}strip${_exe}" CACHE FILEPATH "")
set(CMAKE_NM "${_gcc_root}/bin/${_tool_prefix}nm${_exe}" CACHE FILEPATH "")
set(CMAKE_OBJDUMP "${_gcc_root}/bin/${_tool_prefix}objdump${_exe}" CACHE FILEPATH "")
set(CMAKE_READELF "${_gcc_root}/bin/${_tool_prefix}readelf${_exe}" CACHE FILEPATH "")

# Unified headers of the NDK (sysroot/usr/include) selected for the API level by
# __ANDROID_API__, then the headers of the platform sysroot, as ndk-build does.
set(_common "-target ${_target} --sysroot=${_sysroot} -gcc-toolchain ${_gcc_root} \
-isystem ${_unified} -isystem ${_unified}/${_include_triple} -D__ANDROID_API__=${LOOMORA_ANDROID_API} \
-fPIC -ffunction-sections -fdata-sections -funwind-tables -fstack-protector-strong -no-canonical-prefixes \
${_abi_flags}")
if(LOOMORA_ANDROID_PIE)
  set(_common "${_common} -fPIE")
endif()
set(CMAKE_C_FLAGS_INIT "${_common}")
set(CMAKE_CXX_FLAGS_INIT
    "${_common} -isystem ${_stl}/include -isystem ${_stl}/libs/${ANDROID_ABI}/include")
# No -nostdlib++ in Clang 5: the driver adds -lstdc++ (the tiny system library of the platform
# sysroot); --as-needed drops it when gnustl and libsupc++ (linked first) satisfy everything.
set(_link "-target ${_target} --sysroot=${_sysroot} -gcc-toolchain ${_gcc_root} \
-no-canonical-prefixes -Wl,--as-needed -Wl,--gc-sections -Wl,-z,noexecstack -Wl,-z,relro \
-Wl,-z,now -Wl,--exclude-libs,ALL -L${_stl}/libs/${ANDROID_ABI}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_link}")
if(LOOMORA_ANDROID_PIE)
  set(CMAKE_EXE_LINKER_FLAGS_INIT "${_link} -pie")
endif()
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_link} -Wl,--no-undefined")
set(_stl_libs "-lgnustl_static -lsupc++")
if(ANDROID_ABI STREQUAL "armeabi")
  # ARMv5TE has no ldrex / strex: Clang turns the atomic reference counts of gnustl into calls to
  # __atomic_fetch_add_4 and friends, which libatomic.a of the GCC toolchain provides (ndk-build
  # does the same for this ABI only).
  set(_stl_libs "${_stl_libs} -latomic")
  set(LOOMORA_ANDROID_LINK_LIBATOMIC TRUE) # fallback android-armeabi-libatomic of fallbacks.json
endif()
set(CMAKE_CXX_STANDARD_LIBRARIES "${_stl_libs}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH "${_sysroot}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# What the selftest / Java build scripts need to know about this build.
set(LOOMORA_ANDROID_ABI "${ANDROID_ABI}")
set(LOOMORA_ANDROID_TOOL_PREFIX "${_tool_prefix}")
