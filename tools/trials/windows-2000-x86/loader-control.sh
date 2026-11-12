#!/bin/sh
# Control programs of the Windows 2000 trial (spike S1): two tiny console programs built with the
# same MinGW-w64 i686 compiler, one that imports only what Windows 2000 exports (plain.exe) and
# one that imports KERNEL32!InitializeConditionVariable (vista.exe, Windows Vista and later). They
# show what the loader of the guest does with an executable whose import table names a function
# its DLL does not export. Built in $HOME/loomora-build, copied to out/windows-legacy/guest/.
# Run from the repository root inside WSL:
#   wsl -d Ubuntu-24.04 --cd <repo> -- sh validation/evidence/windows-2000-x86/loader-control.sh
set -e
build="$HOME/loomora-build/loader-control"
out=out/windows-legacy/guest
mkdir -p "$build" "$out"
cat > "$build/vista.c" <<'EOF'
#include <windows.h>
#include <stdio.h>
int main(void) {
    CONDITION_VARIABLE cv;
    InitializeConditionVariable(&cv);
    printf("vista.exe: ran\n");
    return 0;
}
EOF
cat > "$build/plain.c" <<'EOF'
#include <windows.h>
#include <stdio.h>
int main(void) {
    printf("plain.exe: ran\n");
    return 0;
}
EOF
i686-w64-mingw32-gcc -O1 -static -D_WIN32_WINNT=0x0600 -o "$out/vista.exe" "$build/vista.c"
i686-w64-mingw32-gcc -O1 -static -D_WIN32_WINNT=0x0500 -o "$out/plain.exe" "$build/plain.c"
ls -l "$out/vista.exe" "$out/plain.exe"
