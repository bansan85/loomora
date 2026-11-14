# Build and run

How to build, verify and run the Loomora probe. It covers what works today; the macOS, iOS, Android and Web builds and the Windows Modern UI are not written yet.

Commands start with `PS>` on the Windows host and `wsl$` inside WSL.

## Fetch the submodules

Building the product never needs `docs/implementation/`. To work on the implementation or to run `tools/validate_records.py`, fetch both submodules:

```
git submodule update --init
```

## Prerequisites

- Windows host: Python 3.8+ with `pip install -r tools/requirements.txt`, CMake 3.13+, Ninja, clang, and WSL2 with Ubuntu-24.04.
- WSL Ubuntu-24.04 (installed on the machine where these commands were run): `sudo apt install cmake ninja-build g++ pkg-config python3-jsonschema libx11-dev libxft-dev libfontconfig-dev libgtkmm-3.0-dev xvfb g++-mingw-w64-i686 g++-mingw-w64-x86-64`. The MinGW-w64 compilers must be the `-win32` thread-model variants (`sudo update-alternatives --config i686-w64-mingw32-g++`, then `i686-w64-mingw32-gcc`, `x86_64-w64-mingw32-g++`, `x86_64-w64-mingw32-gcc`). `tools/build.py` prints what is missing.
- A Linux machine builds its own binaries: it needs a C++11 compiler, CMake and the development files of the libraries listed above (libX11; libXft and fontconfig for the Xft text; gtkmm 3 for the Modern UI).

In WSL always build under `$HOME` (`~/loomora-build/...`), never in `/tmp` or in the Windows tree. `tools/build.py` and `tools/verify.py` already do; with several clones of this repository set `LOOMORA_BUILD_DIR` (for example `export LOOMORA_BUILD_DIR=$HOME/loomora-build/mine`) so that their CMake caches do not clash.

From the Windows host, a WSL command is run as `wsl -d Ubuntu-24.04 --cd "<repository path>" -- <command>`.

## Verify a change (the default)

```
PS> python tools/verify.py quick
wsl$ python3 tools/verify.py quick
```

`quick` builds what exists on the host, runs ctest, the 15 reference vectors through each UI built there, `check_std.py`, `check_ui_logic.py` and `validate_records.py`, one line per check. On the Windows host it builds the backend and the Win32 UI with clang (the strict C++11 check is skipped there and runs in WSL); in WSL it builds the backend and both Linux UIs. `python tools/verify.py full` is only for a graphical change (see `CLAUDE.md`).

## Build and run, by quickstart scenario

[quickstart.md](../../specs/001-loomora-cross-platform/quickstart.md) describes scenarios 0 to 7. What works today:

**Scenario 1: backend and vectors**

```
wsl$ python3 tools/check_std.py
wsl$ cmake -S . -B ~/loomora-build/core-gcc -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
wsl$ cmake --build ~/loomora-build/core-gcc
wsl$ ctest --test-dir ~/loomora-build/core-gcc
PS> cmake --preset host-clang
PS> cmake --build --preset host-clang
PS> ctest --test-dir build/host-clang
```

`check_std.py` compiles the backend as strict C++11 with every GCC and Clang it finds and checks that only the `loomora_*` C functions are exported. ctest runs `loomora_selftest` and the Catch2 tests on `core/tests/vectors.json`.

**Scenario 2: one platform, Legacy UI**

Windows (Windows 2000 to 11): the MinGW-w64 build runs in WSL and builds both executables, then the vectors run on the Windows host.

```
wsl$ python3 tools/build.py windows legacy
PS> python tools/run_vectors.py windows-11-x64 legacy
PS> python tools/run_vectors.py windows-11-x64 legacy --arch x86
```

The executables are `out\windows-legacy\x64\loomora.exe` (XP x64 to 11) and `out\windows-legacy\x86\loomora.exe` (Windows 2000 to 11). `build.py` checks every imported function against `tools/win2000_imports.json`. `loomora.exe --probe` shows the window for 2 seconds and writes the probe line to `loomora-probe.json` in the current folder; without `--probe` it opens the window. The Windows 2000 trials run in a VM and are not described here (see `tools/trials/windows-2000-x86/` and the procedure `docs/implementation/validation/procedures/windows.md`).

Linux (x86_64, built on the machine that runs it; `--arch i686` builds the 32-bit executable too, which needs the `:i386` development packages; only the build was run for it):

```
wsl$ python3 tools/build.py linux legacy --arch x86_64
wsl$ python3 tools/run_vectors.py linux-ubuntu-24.04-x86_64 legacy
wsl$ out/linux-legacy/x86_64/loomora-xlib --probe
```

The platform id is `linux-<ID>-<VERSION_ID>-<machine>` of `/etc/os-release`. `--probe` needs an X display (WSLg provides `:0`) and prints one JSON line on standard output.

**Scenario 3: one platform, Modern UI**

Only Linux exists (Windows 11 WinUI 3 is not written):

```
wsl$ python3 tools/build.py linux modern
wsl$ python3 tools/run_vectors.py linux-ubuntu-24.04-x86_64 modern
wsl$ out/linux-modern/loomora-gtk3 --probe
```

`build.py` writes the deliverable record of each build into `docs/implementation/validation/deliverables/` (only when the submodule is checked out); do not commit those records unless the build is the one a verdict refers to.

**Not available yet**: scenario 4 (Web), scenario 5 (text change across every platform: only three UIs exist), the macOS, iOS and Android builds, and scenario 0 (spikes, manual). Scenario 6 (`tools/measure_sharing.py`, `tools/prefix_check.py`, `tools/report.py`) and scenario 7 (the procedures in `docs/implementation/validation/procedures/`, written for Windows and Linux only) exist in part; run the tools with `--help`. The quickstart still names `tools/build.py windows-2000 legacy` and `windows-11 modern`; the working form is the one above (T118).
