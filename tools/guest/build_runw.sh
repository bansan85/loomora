#!/bin/sh
# Builds tools/guest/runw.c with MinGW-w64 (i686) in $HOME/loomora-build/guest-helper (never in
# /tmp or on the Windows tree) and copies runw.exe to out/windows-legacy/guest/. Run from the
# repository root inside WSL: wsl -d Ubuntu-24.04 --cd <repo> -- sh tools/guest/build_runw.sh
set -e
build="$HOME/loomora-build/guest-helper"
mkdir -p "$build" out/windows-legacy/guest
i686-w64-mingw32-gcc -Os -s -Wall -Wextra -Werror -ffreestanding -fno-builtin \
    -fno-stack-protector -mincoming-stack-boundary=2 -D_WIN32_WINNT=0x0500 -DWINVER=0x0500 \
    -nostartfiles -nostdlib -Wl,-e,_start -Wl,--subsystem,console \
    -o "$build/runw.exe" tools/guest/runw.c -lkernel32
cp "$build/runw.exe" out/windows-legacy/guest/runw.exe
ls -l out/windows-legacy/guest/runw.exe
