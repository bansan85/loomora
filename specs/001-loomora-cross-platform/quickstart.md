# Quickstart: validate a platform

Per-family procedures are in `docs/implementation/validation/procedures/<family>.md` (`windows.md`, `linux.md`, `android.md`, `web.md`, `offline-install.md`; the macOS and iOS ones are not written: Apple hardware is pending). Contracts: [c-api.md](contracts/c-api.md), [ui-contract.md](contracts/ui-contract.md), [probe-result.schema.json](contracts/probe-result.schema.json). Data: [data-model.md](data-model.md). Toolchain choices and spikes: [research.md](research.md). Build commands in detail: `docs/building/README.md`.

Replayed on 2026-11-15 on the build machine (Windows 11 host with WSL Ubuntu 24.04), task T118 (the runs in the Windows 2000 VM and Firefox 10 were not repeated: the VM was powered off, see scenarios 2 and 4). Commands start with `PS>` on the Windows host and `wsl$` inside WSL (from the host, a WSL command is run as `wsl -d Ubuntu-24.04 --cd <repository path> -- <command>`). In WSL every build goes under `$HOME/loomora-build/` (set `LOOMORA_BUILD_DIR` to give each checkout its own folder), never in `/tmp` and never in the Windows tree.

Marked **not available: needs Apple hardware (pending)**: everything for macOS and iOS (`tools/build.py macos|ios legacy|modern` exits with "not implemented yet"). Those families are reported `not_tested`.

## Prerequisites (build machine)

- Both submodules fetched: `git submodule update --init` (`docs/implementation/` holds the records, `third_party/catch2/` the tests; from a local sibling clone use `git -c protocol.file.allow=always submodule update --init`).
- Host: CMake >= 3.13, GCC and Clang (current), Python 3.8+ with `jsonschema` (`pip install -r tools/requirements.txt`). A Linux machine builds its own binaries (C++11 compiler, CMake, libX11 development files).
- Cross toolchains per floor: MinGW-w64 GCC, `-win32` thread model (Windows 2000 to 11, backend and Win32 UI, built in WSL); Android NDK **r15c** (the only one with `android-9`, see ADR 0016), a JDK 8 or later, build-tools with `d8` (36.0.0 measured; `d8 --min-api 9` replaces `dx`), on the Windows host; Emscripten, the current one (`$LOOMORA_EMSDK`, default `~/emsdk`) and 1.39.20 fastcomp (`$LOOMORA_EMSDK_FASTCOMP`, default `~/emsdk-fastcomp`, spike S5), in WSL. Not available: Clang + osxcross with the 10.6 SDK, and Xcode 5.1.1, on Apple hardware (pending).
- Modern UIs: Windows 11 (WinUI 3): Visual Studio 2026 with the C++ desktop workload, the .NET SDK pinned by `ui/modern/windows-winui3/global.json` and NuGet access; Android: NDK r30, `platforms;android-37.0`, build-tools 36.0.0, a JDK 17 or later, and the Gradle wrapper of `ui/modern/android-compose` (needs the Internet); Linux: gtkmm 3 development files (`libgtkmm-3.0-dev`, which pulls `libgtk-3-dev`; `libgtkmm-4.0-dev` for the GTK 4 variant), no Flatpak; Web: Node.js and npm (TypeScript comes from `package-lock.json`). Not available: a current Xcode (macOS, iOS; pending).
- An image or device of the platform to test, described in `docs/implementation/validation/platforms/<id>.json`. The virtual machines (Windows 2000, Firefox 10) must already be running with the user logged on: no tool starts, restores or snapshots one.

## Scenario 0 — spikes

The spikes S1 to S10 of [research.md](research.md) (S4 was removed on 2026-11-14) are recorded in `docs/implementation/validation/spikes.json` (status, result, ADR; `python3 tools/validate_records.py` checks the file). A refuted spike produces an ADR in `docs/implementation/adr/` naming the fallback. Run them in order of risk; where a spike was run, its procedure replays it:

| Spike | Replay | Status on 2026-11-15 |
|---|---|---|
| S1 Windows 2000 | `procedures/windows.md` part B (scenario 2, step 3) | confirmed (ADR 0002) |
| S2 macOS 10.6 and iOS 5 | not available: needs Apple hardware (pending) | open |
| S3 Android API 9 | `procedures/android.md` B and C (scenario 2) | confirmed (ADR 0016) |
| S5 old browsers | `procedures/web.md` (scenario 4) | confirmed (ADR 0017) |
| S6 strict C++11, C boundary | `python3 tools/check_std.py` (scenario 1) | confirmed |
| S7 Xcode 5.1.1 and the 10.6 SDK | not available: needs Apple hardware (pending) | open |
| S8 WinUI 3 and `loomora.dll`, Compose `minSdk` | `procedures/windows.md` C.7 and `procedures/android.md` H (`python tools/build.py android modern --min-sdk 21` fails by design) | confirmed |
| S9 Windows 7 to 11 accept the Windows 2000 executable | `procedures/windows.md` part A (Windows 11 only) | open |
| S10 CI runners (ADR 0018) | reading, no command; the workflows are the replay (see "CI equivalents") | open |

## Scenario 1 — backend and vectors

1. In WSL (or on Linux), `python3 tools/check_std.py`: builds `core/` with GCC and Clang (`-std=c++11 -pedantic-errors -Wall -Wextra -Werror`) and fails if the shared build exports any symbol other than the `loomora_*` C functions. On a Windows host clang uses the MSVC STL, which needs C++14, so the strict steps are skipped there.
2. In WSL: `cmake -S . -B ~/loomora-build/core-gcc -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ && cmake --build ~/loomora-build/core-gcc && ctest --test-dir ~/loomora-build/core-gcc`. On the Windows host: `cmake --preset host-clang && cmake --build --preset host-clang && ctest --test-dir build/host-clang`. ctest runs `loomora_selftest` and the Catch2 tests on the vectors of `core/tests/vectors.json`. (`python tools/verify.py quick` runs all of this in one command; see `docs/building/README.md`.)
3. **Expected**: all vectors pass with both compilers (`check_std.py` ends with "0 failure(s), 2 compiler(s)", ctest with "100% tests passed"). The fallbacks each build takes are written into its deliverable record (`docs/implementation/validation/deliverables/`, field `fallbacks`) and listed in section 7 of the report (scenario 6); `docs/implementation/validation/fallbacks.json` declares them.

`tools/build.py` rewrites the deliverable records of the platform it builds (new hashes, and the sharing figures are dropped until `python3 tools/measure_sharing.py --write` stores them again): after a replay that does not release a build, discard them with `git -C docs/implementation checkout -- validation/deliverables`.

## Scenario 2 — one platform, Legacy UI (example: Windows 2000 SP4)

1. In WSL, `python3 tools/build.py windows legacy` (from the Windows host: `wsl -d Ubuntu-24.04 --cd <repository path> -- python3 tools/build.py windows legacy`): builds the backend and the Win32 UI with MinGW-w64 and produces `out/windows-legacy/x86/loomora.exe` (Windows 2000 to 11) and `out/windows-legacy/x64/loomora.exe` (XP x64 to 11), after checking every import against `tools/win2000_imports.json`. The self-tests are built separately: `wsl -d Ubuntu-24.04 --cd <repository path> -- sh tools/guest/build_selftest.sh` writes `loomora_selftest.exe` (x86), `loomora_selftest-noshim.exe` and `loomora_selftest-x64.exe` into `out/windows-legacy/guest/`.
2. Run on the Windows 11 host first: `PS> python tools/run_vectors.py windows-11-x64 legacy`, then the same with `--arch x86` (the x86 executable under WOW64). `loomora.exe --probe` shows the window for 2 seconds and writes `loomora-probe.json` **next to the executable**.
3. For the Windows 2000 VM (no Internet; the VM running, a desktop visible and the user logged on): `PS> python tools/run_vectors.py windows-2000-x86 legacy --arch x86`. The launcher copies the executable to the guest with `VBoxManage guestcontrol` (SHA-256 checked) and runs each vector. For a manual run, copy the x86 executable and `loomora_selftest.exe` to the VM, run `loomora_selftest.exe` (backend vectors, no UI), then `loomora.exe --probe`. A script never starts, restores or snapshots the VM.
4. **Expected**: window with the field "Coucou" and the greeting "Bonjour Coucou" in under 3 s, typing "Marie" shows "Bonjour Marie", end-of-life warning visible on Windows 2000, one valid JSON line in `loomora-probe.json` (`ui_variant = legacy`), clean exit.
5. `run_vectors.py <platform-id> legacy`: every valid reference input must produce `"Bonjour "` + input (SC-011); the platform id is `windows-2000-x86` for the VM and `windows-11-x64` for the host.
6. Save screenshot and JSON in `docs/implementation/validation/evidence/<change>/<id>/` only when they prove something new (ADR 0014; no image without the owner's authorization); fill in `docs/implementation/validation/verdicts/<id>-legacy.json`.

Other Legacy UIs, same expected result:

- Linux (built on the machine that runs it): `wsl$ python3 tools/build.py linux legacy --arch x86_64`, `wsl$ python3 tools/run_vectors.py linux-ubuntu-24.04-x86_64 legacy` (the platform id is `linux-<ID>-<VERSION_ID>-<machine>` of `/etc/os-release`) and `wsl$ out/linux-legacy/x86_64/loomora-xlib --probe`. The vectors type into the X display: on WSLg's shared `:0` another program using the keyboard can corrupt one (seen once: extra characters appended), so run them on a private display (`Xvfb :87 -screen 0 1280x800x24 &` then `DISPLAY=:87`), as `verify.py` does. Older distributions: `procedures/linux.md` L.10 and L.11 (Docker).
- Android (API 9 to 23; Windows host, not WSL): set `LOOMORA_ANDROID_NDK` (NDK r15c) and `LOOMORA_ANDROID_SDK`, then `PS> python tools/build.py android legacy` (one APK, `out/android-legacy/loomora.apk`); start the API 16 x86 emulator yourself (`emulator -avd <avd> -no-window -no-audio -no-snapshot -no-boot-anim`, from the `emulator` folder of the SDK; stop it with `adb emu kill`), then `PS> python tools/run_vectors.py android-4.1.2-x86 legacy`. Only API 16 and later can be run.
- macOS 10.6 and iOS 5: not available: needs Apple hardware (pending).

## Scenario 3 — one platform, Modern UI (example: Windows 11 x64)

1. `PS> python tools/build.py windows modern` (Windows host, not WSL; about two minutes): builds `loomora.dll` with MSVC and the WinUI 3 UI for x64 and ARM64 (`net8` and `net10`); published folders in `build/windows-modern/`, zips in `out/windows-modern/` (see `docs/building/README.md`).
2. `PS> python tools/run_vectors.py windows-11-x64 modern`, and `build\windows-modern\net10-x64\Loomora.WinUI.exe --probe` (it writes `loomora-probe.json` next to the executable and prints the line). The ARM64 build is built, never run here.
3. **Expected**: native-looking window showing "Bonjour Coucou" in under 3 s (typing updates it), JSON with `ui_variant = modern`, same message as the Legacy UI, 15 of 15 vectors.

Other Modern UIs:

- Linux (gtkmm 3), in WSL or on Linux: `wsl$ python3 tools/build.py linux modern`, then `wsl$ python3 tools/run_vectors.py linux-ubuntu-24.04-x86_64 modern` and `wsl$ out/linux-modern/loomora-gtk3 --probe` (private X display as above).
- Android (Kotlin and Compose, minSdk 24): `PS> python tools/build.py android modern` (needs `LOOMORA_ANDROID_SDK`, `JAVA_HOME` and the Internet for Gradle) produces `out/android-modern/loomora.apk`; the vectors run on an API 24 x86_64 emulator (`python tools/run_vectors.py android-7.0-x86_64 modern`, as `android.yml` does; `procedures/android.md` G).
- macOS and iOS (SwiftUI): not available: needs Apple hardware (pending).

## Scenario 4 — Web, two paths

1. In WSL (from the host the tool re-runs itself in WSL): `python3 tools/build.py web legacy` builds the C++11 backend twice, as WebAssembly (current Emscripten) and as JavaScript (Emscripten 1.39.20 fastcomp), runs both self-tests under node, checks that `app.js` and `loomora.js` parse as ECMAScript 3, and assembles `$HOME/loomora-build/web-legacy/dist/` (`index.html`, `app.js`, `loomora.js`, `loomora-wasm.js`, `loomora.wasm`). `python3 tools/build.py web modern` builds the TypeScript page of `ui/modern/web-ts` (`npm ci`, `tsc`) with the WebAssembly module into `$HOME/loomora-build/web-modern/dist/` (`index.html`, `main.js`, `native.js`, `loomora.js`, `loomora.wasm`).
2. Without a browser, under node (the page script in a DOM stub; the folder as WSL sees it): `python3 tools/run_vectors.py web-node-js legacy --exe <web-legacy dist>`, `... web-node-wasm legacy --exe <web-legacy dist>` and `... web-node-wasm modern --exe <web-modern dist>`.
3. Modern page in a recent browser (WebAssembly path, `core = cpp-wasm`), here Chrome on the Windows host: `PS> $env:LOOMORA_WEB_DIST = "\\wsl.localhost\Ubuntu-24.04\home\<user>\loomora-build\web-modern\dist"`, then `PS> python tools/run_vectors.py web-chrome-157-windows-11-x64 modern --timeout 60` (headless Chrome, a temporary profile of its own).
4. Legacy page in a browser without WebAssembly (JavaScript path, `core = cpp-js`): Firefox 10 in the Windows 2000 VM (the VM running and logged on, `LOOMORA_WEB_DIST` pointing at the Legacy `dist`): `PS> python tools/run_vectors.py web-firefox-10-windows-2000-x86 legacy --timeout 120 --evidence-dir <folder>` (`procedures/web.md`). Internet Explorer is not a target (no typed arrays); Safari is covered by inference and was never run.
5. **Expected**: identical greeting for the same input, 15 of 15 vectors on every path; the oldest platform of each path is recorded.

## Scenario 5 — text change (code sharing)

1. `python tools/prefix_check.py` (Windows host; `--list` names the targets): copies the sources, changes the prefix in `core/src/greet.cpp` of the copy ("Bonjour " to "hello "), rebuilds from scratch every Legacy and Modern UI the machine can build (Windows host build, Windows x64 and x86, Linux Legacy x86_64 and i686, Linux Modern), runs the vectors through each and never touches the repository. `--vm` adds the Windows 2000 VM (it must be running). By hand: change the prefix in `core/src/greet.cpp`, rebuild every platform and both variants.
2. **Expected**: every platform and variant shows "hello Coucou" with no other change; the tool prints "only core/src/greet.cpp changed, nothing under ui/ or bridge/" and exits 0 (by hand: `git diff` only touches `core/` and the test vectors). Limit: `prefix_check.py` does not build the Web, Android and WinUI 3 UIs yet (it lists them as "no UI built yet"); for those, change the prefix by hand and rebuild.

## Scenario 6 — measurement and report

1. `python tools/measure_sharing.py` → sharing ratio per platform and variant (`--write` stores it in the deliverable records, `--strict` exits 1 below `--min-ratio`).
2. `python tools/report.py` → `docs/implementation/validation/report.md`: platform × variant × verdict matrix, hypotheses V1–V20, spikes, fallbacks and disabled features per platform, gaps with the requirements document, recommendations. `python tools/report.py --check` prints the checks and fails when `report.md` is out of date; `python tools/diff_verdicts.py <revision>` lists the verdict changes against a revision of the docs submodule.
3. **Expected**: every attempted platform has verdict + evidence + procedure (SC-004); every verdict change since the previous report is listed (FR-018). Replay of 2026-11-15: `report.py --check` ends with "report.md is up to date"; SC-007 (80% shared code) is NOT MET, every row below 80% being explained in section 13 of the report.

## Scenario 7 — replayability

A third person follows `docs/implementation/validation/procedures/<family>.md` (`windows.md`, `linux.md`, `android.md`, `web.md`) for an old platform and gets the same verdict (SC-008); `offline-install.md` says where the system images and installers came from. The Apple procedures (`macos.md`, `ios.md`) are not written: not available: needs Apple hardware (pending). Checked on 2026-11-15: every file and path the procedures name exists and the first commands of each procedure run as written; no third person replayed one.

## CI equivalents

What each GitHub workflow of `.github/workflows/` runs (see `docs/building/README.md`, "Continuous integration", for the table and "Local runs (not CI)" for what no hosted runner can run):

- `ci.yml` runs scenario 1 and the Linux UIs of scenarios 2 and 3 on Ubuntu 22.04, 24.04 and 24.04 ARM (`python3 tools/verify.py quick`), and the GTK 3.10, GTK 4, i686 and Xlib core font variants (`--variants`).
- `windows.yml` runs scenario 2 for Windows: the MinGW-w64 cross build, the vectors on the x64 and x86 executables, and the clang host build.
- `winui3.yml` runs scenario 3 for Windows 11: the WinUI 3 build for x64 and ARM64 and the vectors on both.
- `web.yml` runs scenario 4: the Legacy and Modern builds, the vectors under node and in headless Chrome (Firefox 10 is a local run).
- `android.yml` runs scenarios 2 and 3 for Android: both builds and the vectors on an API 16 and an API 24 emulator.
- `manylinux1.yml` runs scenario 2 for Linux in the CentOS 5.11 container.
- `quality.yml` runs the checks of scenario 6 and the verdict comparison with the target branch (FR-018): `validate_records.py`, `report.py --check`, `diff_verdicts.py`, and the self-tests of the tools.
- Not in CI: scenario 0 (spikes), the Windows 2000 and Firefox 10 virtual machines, the Apple systems (pending), and scenario 7 (a third person replays a procedure).
