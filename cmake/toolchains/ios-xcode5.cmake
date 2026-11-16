# Xcode 5.1.1 toolchain for iOS 5 (armv7, armv7s, arm64), run on a Mac with Xcode 5.1.1.
#
#   cmake -S . -B build/ios-5 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios-xcode5.cmake
#
# Spike S2 (T067) and T028. NOT VERIFIED: no Mac with Xcode 5.1.1 was available when this file was
# written. Every run on iOS 5 is `not_tested` until a real device or simulator is available. The
# iOS 5 simulator is reported as not usable on OS X 10.9 and later (spike S7, T065), so the
# realistic check is a device.
#
#   LOOMORA_IOS_SDK          SDK name passed to xcodebuild (default "iphoneos7.0", the newest SDK of
#                            Xcode 5.1.1; the iOS 5.0 SDK is not required because the deployment
#                            target is 5.0).
#   LOOMORA_IOS_DEPLOYMENT   deployment target (default "5.0", the floor of the iOS Legacy UI;
#                            tools/build.py overrides it to 12.0 for the arm64 compile on macos-26,
#                            fallback ios-legacy-deployment-override, which is NOT this file).
#
# Slices: armv7, armv7s and arm64. A device of the 32-bit family is needed to run armv7 and armv7s;
# the arm64 slice needs the iOS 7 SDK of Xcode 5.1.1, which this file selects.

if(NOT DEFINED LOOMORA_IOS_SDK)
  if(DEFINED ENV{LOOMORA_IOS_SDK})
    set(LOOMORA_IOS_SDK "$ENV{LOOMORA_IOS_SDK}")
  else()
    set(LOOMORA_IOS_SDK "iphoneos7.0")
  endif()
endif()
if(NOT DEFINED LOOMORA_IOS_DEPLOYMENT)
  if(DEFINED ENV{LOOMORA_IOS_DEPLOYMENT})
    set(LOOMORA_IOS_DEPLOYMENT "$ENV{LOOMORA_IOS_DEPLOYMENT}")
  else()
    set(LOOMORA_IOS_DEPLOYMENT "5.0")
  endif()
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LOOMORA_IOS_SDK LOOMORA_IOS_DEPLOYMENT)

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_VERSION ${LOOMORA_IOS_DEPLOYMENT})
set(CMAKE_OSX_SYSROOT "${LOOMORA_IOS_SDK}")
set(CMAKE_OSX_DEPLOYMENT_TARGET "${LOOMORA_IOS_DEPLOYMENT}")
set(CMAKE_OSX_ARCHITECTURES "armv7;armv7s;arm64")

# The Xcode 5.1.1 toolchain provides clang and clang++ in the developer directory of the SDK.
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

set(LOOMORA_TARGET_IOS_5 ON)
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")

# Build-time programs (Python for the test vectors) are the host ones.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
