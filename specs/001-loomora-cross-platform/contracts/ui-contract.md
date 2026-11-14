# Contract: native UI per platform (Legacy and Modern)

Each platform has **two UI implementations** on the same C++11 backend:

- **Legacy UI**: compatible with very old systems; features may be disabled (each listed in the verdict and in the probe record).
- **Modern UI**: the most modern interface technology available on the platform, preferably native.

Each UI is written in the **native language** of its platform and only calls the API of [c-api.md](c-api.md). Every UI, in either variant, MUST:

1. Check `loomora_abi_version()` at startup.
2. Show a text field pre-filled with "Coucou" (input limited to 1,000 characters) and, at startup and on every change of the field, call `loomora_greet` with the field content and display the returned greeting legibly in the middle of the window or screen (contrast ≥ 4.5:1; dark and high-contrast system themes respected when they exist). The UI never builds the greeting itself (FR-029).
3. If `loomora_is_eol(family, major, minor)` returns 1: show `loomora_eol_warning()` non-blockingly.
4. Contain **no application logic**: only window creation, event loop, encoding conversion, reading the system version, measuring display time, probe mode.
5. Open no network connection, require no administrator rights, write nothing but the probe result.
6. Exit cleanly on close (return code 0).
7. A Legacy UI that disables a feature MUST report it in `disabled_features` and still display the greeting.

## Probe mode

Enabled by `--probe` (Android: intent extra; iOS: launch argument; Web: `?probe` URL parameter). The UI:

- displays normally and measures the delay from launch to first display;
- asks the backend for the JSON line (`loomora_probe_record`) and emits it: stdout (Windows, macOS, Linux), `logcat` with tag `LOOMORA_PROBE` (Android), `NSLog` (iOS), `console.log` + a `<pre id="probe">` element (Web). The Windows Legacy GUI executable has no console, so it writes the line to a file next to the executable (`loomora-probe.json`) and to `OutputDebugString`;
- exits by itself after 2 seconds (except Web).

## Probe input override and memory measurement

- **Probe input override** (SC-011, FR-027): in probe mode a UI MUST accept an input text instead of the default `Coucou`: `--probe-input <utf8>` (Windows, macOS, Linux), intent extra `probe_input` (Android), launch argument `-probe_input <utf8>` (iOS), URL parameter `?probe&input=<percent-encoded utf8>` (Web). The UI puts that text in its field, displays the greeting, and emits the probe line whose `input` and `message` reflect it. `tools/run_vectors.py` uses this to feed every valid reference input of `core/tests/vectors.json`.
- **Peak memory** (`peak_rss_kb`, best effort, `null` if the platform cannot measure it): Windows `GetProcessMemoryInfo` (`PeakWorkingSetSize`; absent on Windows 2000 without `psapi.dll`, loaded dynamically, otherwise `null`), Linux `VmHWM` in `/proc/self/status`, macOS and iOS `task_info` (`resident_size`), Android `/proc/self/status` `VmHWM`, Web `performance.memory` where available.

## UI per platform

The Legacy columns are decided (floors from the Clarifications). The Modern columns were chosen at planning (research R3–R8, R12); their floors are those of the toolkit, and items marked "to verify" in research.md are confirmed by the spikes S1–S9. Below a Modern UI's floor, the platform uses its Legacy UI.

| Platform | Legacy UI (language, toolkit) | Legacy floor and backend toolchain | Modern UI (language, toolkit) | Modern floor and backend toolchain |
|---|---|---|---|---|
| Windows | C++, plain Win32 API (no MFC), `EDIT` + `STATIC` controls, one executable for 2000 → 11 | Windows 2000 SP4; backend and UI by MinGW-w64 GCC (`i686`, `x86_64`) | C#, WinUI 3 (Windows App SDK), `loomora.dll` through `LibraryImport` | Windows 10 1809 (build 17763); x64 and ARM64; current MSVC |
| macOS | Objective-C, AppKit, `i386` + `x86_64` | 10.6; Clang + osxcross + 10.6 SDK | Swift, SwiftUI (`App` lifecycle) | macOS 11; Universal 2; current Xcode |
| iOS | Objective-C, UIKit, `armv7` + `armv7s` + `arm64` | iOS 5; Xcode 5.1.1 (Apple Clang 5.1) | Swift, SwiftUI | iOS 15; arm64; current Xcode |
| Android | Java, Android framework views (no AndroidX), scripted APK build | API 9 (2.3); NDK r16b (`armeabi`, `armeabi-v7a`, `x86`, `arm64-v8a`) | Kotlin, Jetpack Compose | API 24 (21 only if the pinned Compose accepts it); current NDK |
| Linux | C++11, Xlib (`libX11` only), `x86_64` + `i686` | any distribution with a C++11 compiler installable (built on the target machine, ADR 0007); native compiler of the machine | C++, GTK 3 with gtkmm (3.10 API subset), plain executable built per distribution with CMake (no Flatpak) | any distribution packaging GTK 3.10+ (about 2014, to verify); distribution compiler |
| Web | JavaScript (ES5), WebAssembly or JavaScript build | Nov-2011 browsers (IE 9, Firefox 8, Chrome 15, Safari 5.1, Opera 11.5); older best effort; Emscripten (JS build: current or 1.39.20 fastcomp) | TypeScript (ES modules), WebAssembly only, same `loomora.wasm` | about Chrome 61 / Firefox 60 / Safari 11 / Edge 16 (to verify); current Emscripten |

## Forbidden

APIs newer than the floor of a Legacy deliverable; mandatory GPU access in a Legacy UI; mandatory embedded fonts; remote resources; logic duplicated from the backend.
