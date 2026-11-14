# Quickstart: validate a platform

Detailed per-family procedures will be in `docs/implementation/validation/procedures/<family>.md` (created during implementation). Contracts: [c-api.md](contracts/c-api.md), [ui-contract.md](contracts/ui-contract.md), [probe-result.schema.json](contracts/probe-result.schema.json). Data: [data-model.md](data-model.md). Toolchain choices and spikes: [research.md](research.md).

## Prerequisites (build machine)

- Host: CMake ≥ 3.13, GCC and Clang (current), Python 3.8+ with `jsonschema`. A Linux machine builds its own binaries (C++11 compiler, CMake, libX11 development files).
- Cross toolchains per floor: MinGW-w64 GCC (Windows 2000 → 11, backend and Win32 UI); Clang + osxcross and the 10.6 SDK, and Xcode 5.1.1, on Apple hardware; Android NDK **r16b**, JDK 8, build-tools 28.0.3 or older; Emscripten (current, plus 1.39.20 fastcomp if spike S5 requires it).
- Modern UIs: Visual Studio 2022 with .NET 7+ and the Windows App SDK; a current Xcode; Android Studio with AGP 9 and `compileSdk` 37; gtkmm 3 development files (`libgtkmm-3.0-dev`, which pulls `libgtk-3-dev`; `libgtkmm-4.0-dev` for the GTK 4 build) and CMake on the Linux build machine (no Flatpak); Node.js + TypeScript compiler.
- An image or device of the platform to test, described in `docs/implementation/validation/platforms/<id>.json`.

## Scenario 0 — spikes

Run spikes S1–S9 (S4 removed on 2026-11-14) of [research.md](research.md) first, in order of risk (S1 Windows 2000, S2 macOS 10.6 and iOS 5, S3 Android 2.3, S5 old browsers). Record each result in `docs/implementation/validation/spikes.json`; a refuted spike produces an ADR in `docs/implementation/adr/` naming the fallback.

## Scenario 1 — backend and vectors

1. In WSL (or on Linux), `python3 tools/check_std.py`: builds `core/` with GCC and Clang (`-std=c++11 -pedantic-errors -Wall -Wextra -Werror`) and fails if the shared build exports any symbol other than the `loomora_*` C functions. On a Windows host clang uses the MSVC STL, which needs C++14, so the strict steps are skipped there.
2. In WSL: `cmake -S . -B ~/loomora-build/core-gcc -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ && cmake --build ~/loomora-build/core-gcc && ctest --test-dir ~/loomora-build/core-gcc`. On the Windows host: `cmake --preset host-clang && cmake --build --preset host-clang && ctest --test-dir build/host-clang`. ctest runs `loomora_selftest` and the Catch2 tests on the vectors of `core/tests/vectors.json`. (`python tools/verify.py quick` runs all of this in one command; see `docs/building/README.md`.)
3. **Expected**: all vectors pass with both compilers; the list of active fallbacks per compiler is produced (`docs/implementation/validation/fallbacks.json` up to date).

## Scenario 2 — one platform, Legacy UI (example: Windows 2000 SP4)

1. In WSL, `python3 tools/build.py windows legacy` (from the Windows host: `wsl -d Ubuntu-24.04 --cd <repository path> -- python3 tools/build.py windows legacy`): builds the backend and the Win32 UI with MinGW-w64 and produces `out/windows-legacy/x86/loomora.exe` (Windows 2000 to 11) and `out/windows-legacy/x64/loomora.exe` (XP x64 to 11), after checking every import against `tools/win2000_imports.json`. The self-test is built separately: `wsl -d Ubuntu-24.04 --cd <repository path> -- sh tools/guest/build_selftest.sh` writes `out/windows-legacy/guest/loomora_selftest.exe`.
2. Copy the x86 executable and `loomora_selftest.exe` to the Windows 2000 VM (no Internet).
3. Run `loomora_selftest.exe` (backend vectors, no UI), then `loomora.exe --probe`.
4. **Expected**: window with the field "Coucou" and the greeting "Bonjour Coucou" in under 3 s, typing "Marie" shows "Bonjour Marie", end-of-life warning visible, one valid JSON line in `loomora-probe.json` (`ui_variant = legacy`), clean exit.
5. Run `python tools/run_vectors.py <platform-id> legacy` (for the VM, `windows-2000-x86`; `--arch x86|x64` picks the executable on a Windows host): every valid reference input must produce `"Bonjour "` + input (SC-011).
6. Save screenshot and JSON in `docs/implementation/validation/evidence/<change>/<id>/`; fill in `docs/implementation/validation/verdicts/<id>-legacy.json`.

## Scenario 3 — one platform, Modern UI (example: Windows 11 x64)

The Windows 11 (WinUI 3) UI is not written yet: `python tools/build.py windows modern` fails with "not implemented yet". The steps below are the target; the only Modern UI that exists today is Linux (see the end of this scenario).

1. `python tools/build.py windows modern`: produces the package (WinUI 3 UI + `loomora.dll` built by current MSVC).
2. Run it with `--probe` on a Windows 11 machine.
3. **Expected**: native-looking window showing "Bonjour Coucou" in under 3 s (typing updates it), JSON with `ui_variant = modern`, same message as the Legacy UI.

Available today, Linux Modern (gtkmm 3), in WSL or on Linux: `python3 tools/build.py linux modern`, then `python3 tools/run_vectors.py linux-ubuntu-24.04-x86_64 modern` (the platform id is `linux-<ID>-<VERSION_ID>-<machine>` of `/etc/os-release`) and `out/linux-modern/loomora-gtk3 --probe`.

## Scenario 4 — Web, two paths

Not available yet: `python tools/build.py web legacy` and `web modern` fail with "not implemented yet".

1. `python tools/build.py web <legacy|modern>`: produces `loomora.wasm` and `loomora.js` (WebAssembly-free build) from the same C++ code.
2. Open the Modern page in a recent browser: it loads the WebAssembly path (`core = cpp-wasm`).
3. Open the Legacy page in a browser without WebAssembly (for example IE 9 or Firefox 8): it loads the JavaScript path (`core = cpp-js`).
4. **Expected**: identical greeting for the same input; the oldest platform of each path is recorded.

## Scenario 5 — text change (code sharing)

1. Change the greeting prefix in `core/src/greet.cpp` ("hello "), rebuild every platform and both variants.
2. **Expected**: every platform and variant shows "hello Coucou" with no other change; `git diff` only touches `core/` (and test vectors).

## Scenario 6 — measurement and report

1. `python tools/measure_sharing.py` → sharing ratio per platform and variant.
2. `python tools/report.py` → `docs/implementation/validation/report.md`: platform × variant × verdict matrix, hypotheses V1–V20, spikes, fallbacks and disabled features per platform, gaps with the requirements document, recommendations.
3. **Expected**: every attempted platform has verdict + evidence + procedure (SC-004); every verdict change since the previous report is listed (FR-018).

## Scenario 7 — replayability

A third person follows `docs/implementation/validation/procedures/<family>.md` for an old platform and gets the same verdict (SC-008).
