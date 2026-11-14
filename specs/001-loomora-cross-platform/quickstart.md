# Quickstart: validate a platform

Detailed per-family procedures will be in `docs/implementation/validation/procedures/<family>.md` (created during implementation). Contracts: [c-api.md](contracts/c-api.md), [ui-contract.md](contracts/ui-contract.md), [probe-result.schema.json](contracts/probe-result.schema.json). Data: [data-model.md](data-model.md). Toolchain choices and spikes: [research.md](research.md).

## Prerequisites (build machine)

- Host: CMake ≥ 3.13, GCC and Clang (current), Python 3.8+ with `jsonschema`, Docker (manylinux2010 image).
- Cross toolchains per floor: MinGW-w64 GCC (Windows 2000 → 11, backend and Win32 UI); Clang + osxcross and the 10.6 SDK, and Xcode 5.1.1, on Apple hardware; Android NDK **r16b**, JDK 8, build-tools 28.0.3 or older; Emscripten (current, plus 1.39.20 fastcomp if spike S5 requires it).
- Modern UIs: Visual Studio 2022 with .NET 7+ and the Windows App SDK; a current Xcode; Android Studio with AGP 9 and `compileSdk` 37; gtkmm 3 development files (`libgtkmm-3.0-dev`, which pulls `libgtk-3-dev`; `libgtkmm-4.0-dev` for the GTK 4 build) and CMake on the Linux build machine (no Flatpak); Node.js + TypeScript compiler.
- An image or device of the platform to test, described in `docs/implementation/validation/platforms/<id>.json`.

## Scenario 0 — spikes

Run spikes S1–S9 of [research.md](research.md) first, in order of risk (S1 Windows 2000, S2 macOS 10.6 and iOS 5, S3 Android 2.3, S5 old browsers). Record each result in `docs/implementation/validation/spikes.json`; a refuted spike produces an ADR in `docs/implementation/adr/` naming the fallback.

## Scenario 1 — backend and vectors

1. `python tools/check_std.py`: builds `core/` with GCC and Clang (`-std=c++11 -pedantic-errors -Wall -Wextra -Werror`) and fails if the shared build exports any symbol other than the `loomora_*` C functions.
2. `cmake --preset host-gcc && cmake --build --preset host-gcc && ctest`, then the same with `host-clang`: runs the vectors of `core/tests/vectors.json`.
3. **Expected**: all vectors pass with both compilers; the list of active fallbacks per compiler is produced (`docs/implementation/validation/fallbacks.json` up to date).

## Scenario 2 — one platform, Legacy UI (example: Windows 2000 SP4)

1. `python tools/build.py windows-2000 legacy`: builds the backend and the Win32 UI with MinGW-w64 and produces the executable.
2. Copy the executable to the Windows 2000 VM (no Internet).
3. Run `loomora_selftest.exe` (backend vectors, no UI), then `loomora.exe --probe`.
4. **Expected**: window with the field "Coucou" and the greeting "Bonjour Coucou" in under 3 s, typing "Marie" shows "Bonjour Marie", end-of-life warning visible, one valid JSON line in `loomora-probe.json` (`ui_variant = legacy`), clean exit.
5. Run `python tools/run_vectors.py <platform-id> legacy`: every valid reference input must produce `"Bonjour "` + input (SC-011).
6. Save screenshot and JSON in `docs/implementation/validation/evidence/<change>/<id>/`; fill in `docs/implementation/validation/verdicts/<id>-legacy.json`.

## Scenario 3 — one platform, Modern UI (example: Windows 11 x64)

1. `python tools/build.py windows-11 modern`: produces the package (WinUI 3 UI + `loomora.dll` built by current MSVC).
2. Run it with `--probe` on a Windows 11 machine.
3. **Expected**: native-looking window showing "Bonjour Coucou" in under 3 s (typing updates it), JSON with `ui_variant = modern`, same message as the Legacy UI.

## Scenario 4 — Web, two paths

1. `python tools/build.py web`: produces `loomora.wasm` and `loomora.js` (WebAssembly-free build) from the same C++ code.
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
