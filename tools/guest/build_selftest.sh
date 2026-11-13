#!/bin/sh
# Builds loomora_selftest (core/tests/selftest.cpp, spike S1 / T066) for Windows 2000 with the
# MinGW-w64 i686 toolchain of cmake/toolchains/mingw-w64-win2000-i686.cmake, in
# $HOME/loomora-build (never in /tmp or on the Windows tree), and copies three executables to
# out/windows-legacy/guest/:
#   loomora_selftest.exe         what CMake links (core/tests/CMakeLists.txt): selftest.cpp built
#                                with LOOMORA_AVOID_IOSTREAM (no <iostream>) plus the GetThreadId
#                                shim of ADR 0002, both selected by the Windows 2000 toolchain
#                                file (ADR 0004), the way the Legacy UI links; no import above
#                                Windows 2000
#   loomora_selftest-noshim.exe  control: the same sources compiled by hand WITHOUT the shim, to
#                                show that the shim is still needed (it imports
#                                KERNEL32!GetThreadId, Vista and later)
#   loomora_selftest-x64.exe     the x86_64 build of the same self-test (XP x64 floor, toolchain
#                                cmake/toolchains/mingw-w64-win-x86_64.cmake, which also defines
#                                LOOMORA_AVOID_IOSTREAM); the Windows 11 platform runs this one
# Then runs tools/check_imports.py on loomora_selftest.exe against the Windows 2000 allowlist and
# on loomora_selftest-x64.exe against the XP x64 one.
# Run from the repository root inside WSL:
#   wsl -d Ubuntu-24.04 --cd <repo> -- sh tools/guest/build_selftest.sh
set -e
build="$HOME/loomora-build/selftest-w2k"
out=out/windows-legacy/guest
rm -rf "$build"
mkdir -p "$out"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-win2000-i686.cmake \
    -DLOOMORA_MINGW_PREFIX=i686-w64-mingw32- -DLOOMORA_BUILD_TESTS=ON \
    -DLOOMORA_BUILD_CATCH2_TESTS=OFF > "$build.cmake.log" 2>&1 || { cat "$build.cmake.log"; exit 1; }
cmake --build "$build" --target loomora_selftest
cp "$build/core/tests/loomora_selftest.exe" "$out/loomora_selftest.exe"
i686-w64-mingw32-g++ -std=c++11 -O2 -D_WIN32_WINNT=0x0500 -DWINVER=0x0500 -DLOOMORA_AVOID_IOSTREAM=1 \
    -static -static-libgcc -static-libstdc++ -Icore/include -Icore/src -I"$build/core/tests" \
    core/src/*.cpp core/tests/selftest.cpp \
    -o "$out/loomora_selftest-noshim.exe"
ls -l "$out"/loomora_selftest*.exe
python3 tools/check_imports.py --arch x86 --list --objdump i686-w64-mingw32-objdump "$out/loomora_selftest.exe"
build64="$HOME/loomora-build/selftest-x64"
rm -rf "$build64"
cmake -S . -B "$build64" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-win-x86_64.cmake \
    -DLOOMORA_MINGW_PREFIX=x86_64-w64-mingw32- -DLOOMORA_BUILD_TESTS=ON \
    -DLOOMORA_BUILD_CATCH2_TESTS=OFF > "$build64.cmake.log" 2>&1 || { cat "$build64.cmake.log"; exit 1; }
cmake --build "$build64" --target loomora_selftest
cp "$build64/core/tests/loomora_selftest.exe" "$out/loomora_selftest-x64.exe"
python3 tools/check_imports.py --arch x64 --list --objdump x86_64-w64-mingw32-objdump "$out/loomora_selftest-x64.exe"
