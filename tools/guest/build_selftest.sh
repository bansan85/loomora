#!/bin/sh
# Builds loomora_selftest (core/tests/selftest.cpp, spike S1 / T066) for Windows 2000 with the
# MinGW-w64 i686 toolchain of cmake/toolchains/mingw-w64-win2000-i686.cmake, in
# $HOME/loomora-build (never in /tmp or on the Windows tree), and copies two executables to
# out/windows-legacy/guest/:
#   loomora_selftest-noshim.exe  exactly what the toolchain links: it imports KERNEL32!GetThreadId
#                                (Vista and later), which the loader of Windows 2000 refuses (ADR 0002)
#   loomora_selftest.exe         the same sources plus ui/legacy/windows-win32/getthreadid_shim.cpp,
#                                the way the Legacy UI links; no import above Windows 2000
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
cp "$build/core/tests/loomora_selftest.exe" "$out/loomora_selftest-noshim.exe"
i686-w64-mingw32-g++ -std=c++11 -O2 -D_WIN32_WINNT=0x0500 -DWINVER=0x0500 \
    -static -static-libgcc -static-libstdc++ -Icore/include -Icore/src -I"$build/core/tests" \
    core/src/*.cpp core/tests/selftest.cpp ui/legacy/windows-win32/getthreadid_shim.cpp \
    -o "$out/loomora_selftest.exe"
ls -l "$out"/loomora_selftest*.exe
