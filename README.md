# Loomora

Loomora is a minimal probe application: a text field pre-filled "Coucou" and a label that shows "Bonjour " followed by the field, computed by one C++11 backend behind a handwritten C API and displayed by thin native UIs. It exists to validate, from Windows 2000 and other very old systems up to today, the technical choices of [docs/requirements-specification.md](docs/requirements-specification.md), and to produce a verdict per platform and UI variant.

Today three UIs exist: Windows Legacy (plain Win32), Linux Legacy (Xlib) and Linux Modern (gtkmm). The macOS, iOS, Android and Web UIs, and the Windows Modern UI, are specified but not written.

## Layout

| Path | Content |
|---|---|
| `core/` | the backend (strict C++11, namespace `loomora`), `include/loomora.h` (the only API), `tests/` (Catch2, `selftest`, `vectors.json`) |
| `bridge/` | glue with no logic: `jni/` (Android), `wasm/` (Emscripten); not built yet |
| `ui/legacy/`, `ui/modern/` | one folder per platform and UI variant, each in the native language; no logic in them |
| `cmake/toolchains/` | toolchain files: MinGW-w64 (Windows 2000 i686, Windows x86_64) and Linux i686 |
| `tools/` | Python host tools: `verify.py`, `build.py`, `run_vectors.py`, `check_std.py`, `check_imports.py`, `check_probe.py`, `validate_records.py`, `trials/` (drivers that produced the evidence) |
| `specs/001-loomora-cross-platform/` | specification, plan, research, tasks, [quickstart.md](specs/001-loomora-cross-platform/quickstart.md), contracts |
| `docs/requirements-specification.md` | the requirements document |
| `docs/implementation/` | git submodule (`../loomora-docs`): ADRs in `adr/`, the validation data in `validation/` (platforms, verdicts, deliverables, fallbacks, evidence) |
| `third_party/catch2/` | git submodule, Catch2 (tests only) |

Building the product never needs `docs/implementation/`. To work on the implementation or to run `tools/validate_records.py`, fetch both submodules:

```
git submodule update --init
```

## Prerequisites

- Windows host: Python 3.8+ with `pip install -r tools/requirements.txt`, CMake 3.13+, Ninja, clang, and WSL2 with Ubuntu-24.04. Commands below start with `PS>` on the Windows host and `wsl$` inside WSL.
- WSL Ubuntu-24.04 (installed on the machine where these commands were run): `sudo apt install cmake ninja-build g++ pkg-config python3-jsonschema libx11-dev libxft-dev libfontconfig-dev libgtkmm-3.0-dev xvfb g++-mingw-w64-i686 g++-mingw-w64-x86-64`. The MinGW-w64 compilers must be the `-win32` thread-model variants (`sudo update-alternatives --config i686-w64-mingw32-g++`, then `i686-w64-mingw32-gcc`, `x86_64-w64-mingw32-g++`, `x86_64-w64-mingw32-gcc`). `tools/build.py` prints what is missing.
- A Linux machine builds its own binaries: it needs a C++11 compiler, CMake and the development files of the libraries listed above (libX11; libXft and fontconfig for the Xft text; gtkmm 3 for the Modern UI).

In WSL always build under `$HOME` (`~/loomora-build/...`), never in `/tmp` or in the Windows tree. `tools/build.py` and `tools/verify.py` already do; with several clones of this repository set `LOOMORA_BUILD_DIR` (for example `export LOOMORA_BUILD_DIR=$HOME/loomora-build/mine`) so that their CMake caches do not clash.

From the Windows host, a WSL command is run as `wsl -d Ubuntu-24.04 --cd "<repository path>" -- <command>`.

## Verify a change (the default)

```
PS> python tools/verify.py quick
wsl$ python3 tools/verify.py quick
```

`quick` builds what exists on the host, runs ctest, the 15 reference vectors through each UI built there, `check_std.py` and `validate_records.py`, one line per check. On the Windows host it builds the backend and the Win32 UI with clang (the strict C++11 check is skipped there and runs in WSL); in WSL it builds the backend and both Linux UIs. `python tools/verify.py full` is only for a graphical change (see `CLAUDE.md`).

## Build and run, by quickstart scenario

[quickstart.md](specs/001-loomora-cross-platform/quickstart.md) describes scenarios 0 to 7. What works today:

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

The executables are `out\windows-legacy\x64\loomora.exe` (XP x64 to 11) and `out\windows-legacy\x86\loomora.exe` (Windows 2000 to 11). `build.py` checks every imported function against `tools/win2000_imports.json`. `loomora.exe --probe` shows the window for 2 seconds and writes the probe line to `loomora-probe.json` in the current folder; without `--probe` it opens the window. The Windows 2000 trials run in a VM and are not described here (see `tools/trials/windows-2000-x86/` and the verdict `docs/implementation/validation/verdicts/windows-2000-x86-legacy.json`).

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

**Not available yet**: scenario 4 (Web), scenario 5 (text change across every platform: only three UIs exist), scenario 6 (`tools/measure_sharing.py` and `tools/report.py` are not written), scenario 7 (the procedures of `docs/implementation/validation/procedures/` are not written), the macOS, iOS and Android builds, and scenario 0 (spikes, manual). The quickstart still names `tools/build.py windows-2000 legacy` and `windows-11 modern`; the working form is the one above (T118).

## Rules

The full list is in [CLAUDE.md](CLAUDE.md). In short:

- Everything written in the repository is English (French only for product data such as "Bonjour " and "Coucou" and test strings). Commits are Conventional Commits, title of at most 50 characters, imperative.
- `master` holds only merge commits (`git merge --no-ff`); every change is committed on its own branch, in a separate worktree outside the repository when an agent makes it.
- The backend is C++11 whatever the UI; Legacy UIs are C++11, Modern UIs up to C++17. Linux UIs are C++ built with CMake.
- Linux binaries are built on the machine that runs them; Windows binaries are cross-compiled with MinGW-w64 from WSL.
- A fallback applies only to the problematic case (platform, compiler, OS version) and is declared in `docs/implementation/validation/fallbacks.json`; the GTK floor is 3.10, newer features sit behind compile-time macros.
- Implementation documentation (ADRs, validation data, evidence) lives in the `docs/implementation` submodule. Evidence is stored only when it proves something new, and no image is added without the owner's authorization.
- `python tools/verify.py quick` verifies any branch; `full` only for a graphical change.
