#!/bin/sh
# Builds loomora_selftest (core/tests/selftest.cpp, spike S1 / T066) for Windows 2000 with the
# MinGW-w64 i686 toolchain of cmake/toolchains/mingw-w64-win2000-i686.cmake, in
# $HOME/loomora-build (never in /tmp or on the Windows tree), and copies two executables to
# out/windows-legacy/guest/:
#   loomora_selftest.exe         what CMake links (core/tests/CMakeLists.txt): selftest.cpp without
#                                <iostream> plus the GetThreadId shim of ADR 0002, the way the
#                                Legacy UI links; no import above Windows 2000
#   loomora_selftest-noshim.exe  control: the same sources compiled by hand WITHOUT the shim, to
#                                show that the shim is still needed (it imports
#                                KERNEL32!GetThreadId, Vista and later)
# Then runs tools/check_imports.py on loomora_selftest.exe against the Windows 2000 allowlist.
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
i686-w64-mingw32-g++ -std=c++11 -O2 -D_WIN32_WINNT=0x0500 -DWINVER=0x0500 \
    -static -static-libgcc -static-libstdc++ -Icore/include -Icore/src -I"$build/core/tests" \
    core/src/*.cpp core/tests/selftest.cpp \
    -o "$out/loomora_selftest-noshim.exe"
ls -l "$out"/loomora_selftest*.exe
python3 tools/check_imports.py --arch x86 --list --objdump i686-w64-mingw32-objdump "$out/loomora_selftest.exe"
