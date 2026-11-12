---

description: "Task list for Loomora across 15 years of platforms (C++11 backend, Legacy + Modern UIs)"
---

# Tasks: Loomora Across 15 Years of Platforms

**Input**: Design documents from `/specs/001-loomora-cross-platform/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/ (c-api.md, ui-contract.md, probe-result.schema.json), quickstart.md

**Tests**: requested by the spec (FR-027, FR-028, SC-011): backend vector tests (Catch2 on the host, `loomora_selftest` on every target) and probe-mode checks on every UI. They are written before the code they cover.

**Organization**: tasks are grouped by user story. Paths are relative to the repository root. The WinUI 3 Modern UI uses **C# on .NET 8 (LTS) or later** with the Windows App SDK (no C++/CLI); the Legacy Windows UI is plain Win32 in C++ (no MFC).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel (different files, no dependency on an unfinished task)
- **[Story]**: US1 to US5 (spec.md); none for Setup, Foundational and Polish
- Trial tasks (running a build on an old system) need the system image or device described in `docs/implementation/validation/platforms/<id>.json`; if it cannot be obtained, the task ends with a `not_tested` verdict and its reason (spec edge case), never skipped silently.

> **Scope note (2026-11-12)**: for now only the technique is being tested: an input field and a label, C++11 backend + C UI. Done in reduced scope: T015, T017, T019, T023, T024 (backend: `greet`, `abi_version`, `build_compiler`, strict UTF-8 in `core/src/utf8.cpp`, self-test `core/tests/selftest.cpp`) and a Win32 UI written **in C** at `ui/legacy/windows-win32/main.c` (instead of C++, built with host clang + CMake/Ninja). Everything else (eol, probe, other platforms, toolchains, validation tooling) is deferred.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: repository skeleton, build entry points, seeded validation data

- [X] T001 Create the directory skeleton of plan.md with `.gitkeep` files: `core/{include,src/internal,tests}`, `bridge/{jni,wasm}`, `third_party/catch2`, `cmake/toolchains`, `ui/legacy/{windows-win32,macos-appkit,ios-uikit,android-views,linux-xlib,web-es5}`, `ui/modern/{windows-winui3,macos-swiftui,ios-swiftui,android-compose,linux-gtk3,web-ts}`, `tools`, `docs/implementation/validation/{platforms,verdicts,deliverables,evidence,procedures}`, `docs/implementation/adr`
- [X] T002 [P] Create `.gitignore` (build outputs, `.venv*`, IDE files, large evidence videos), `.editorconfig` and `.clang-format` at the repository root
- [X] T003 Create root `CMakeLists.txt` (project `loomora`, languages C and CXX, option `LOOMORA_BUILD_TESTS`, `add_subdirectory(core)`) and `CMakePresets.json` with the presets `host-gcc`, `host-clang` and `host-msvc`
- [X] T004 [P] Add Catch2 as a git submodule `third_party/catch2` pinned to tag v2.13.10 (header: `third_party/catch2/single_include/catch2/catch.hpp`); the header is never copied into the repository
- [X] T005 [P] Create `tools/requirements.txt` (`jsonschema`) and `tools/common.py` (repository paths, JSON read/write helpers, subprocess helpers) for the Python 3.8+ host tools
- [X] T006 [P] Create `docs/implementation/adr/0000-template.md` and `docs/implementation/adr/0001-backend-cpp11-c-api.md` (decision, rejected alternatives: C99 backend, header-only backend, Rust; reason: old platforms; FR-017, FR-019)
- [X] T007 [P] Seed `docs/implementation/validation/sources.json` with every link of the Sources section of research.md (`id`, `url`, `title`, `accessed` = 2026-11-12, `claim`)
- [X] T008 [P] Seed `docs/implementation/validation/spikes.json` with spikes S1–S9 of research.md (`id`, `question`, `status` = `open`, `result` empty, `adr` empty)
- [X] T009 [P] Create `docs/implementation/validation/fallbacks.json` as an empty list (entries: `id`, `description`, `macro`, `min_compiler`, `platform_ids[]`, `reason`; data-model.md)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: the C++11 backend, its tests, the glue files, the cross-compile entry points and the host tools every story needs

**⚠️ CRITICAL**: no user story starts before this phase is complete

### Backend tests first

- [ ] T010 Write `core/tests/vectors.json` with the reference inputs and exact expected outputs: empty string, ASCII, accented, CJK, right-to-left, emoji, combining characters, 1,000 characters, exactly 4,096 bytes (accepted), 4,097 bytes (error -1), invalid UTF-8 (error -3), cap too small (error -2); `loomora_is_eol` cases per family (negative values → 0; Web never end of life); the shape of one probe record (SC-011, FR-028)
- [ ] T011 [P] Write `tools/gen_vectors.py` generating `core/tests/vectors.inc` (C++11 initializer lists) from `core/tests/vectors.json`
- [ ] T012 [P] Write Catch2 tests `core/tests/test_greet.cpp` (every greet vector, exact bytes `"Bonjour "` + input; limit 4,096 bytes; error codes -1/-2/-3) — must fail until T018
- [ ] T013 [P] Write Catch2 tests `core/tests/test_eol.cpp` (every end-of-life vector; `loomora_eol_warning()` never NULL) — must fail until T019
- [ ] T014 [P] Write Catch2 tests `core/tests/test_probe.cpp` (`loomora_probe_record` output validates against the shape in vectors.json; `greeting_matches` true/false; truncation returns the needed length; JSON escaping of quotes, backslashes and control characters) — must fail until T021
- [X] T015 (reduced scope: greet + abi_version + build_compiler only) Write `core/tests/selftest.cpp`: a plain C++11 `main` (STL allowed) using `vectors.inc`, no Catch2, returning non-zero on any failed vector, to be cross-compiled and run on every target (research R9)
- [ ] T016 Write `core/tests/CMakeLists.txt` (Catch2 test executable `loomora_tests`, executable `loomora_selftest`, custom command running `tools/gen_vectors.py`, `add_test`)

### Backend implementation

- [X] T017 (reduced scope: greet + abi_version + build_compiler only) Write `core/include/loomora.h` exactly as in contracts/c-api.md: `LOOMORA_ABI_VERSION` 1, all functions and macros, `LOOMORA_API` export macro, `extern "C"` guard, portability block (own `int32_t`/`uint32_t`/`uint8_t`/`int64_t` typedefs when `_MSC_VER < 1600` or `<stdint.h>` is absent), valid as C89 and C++98
- [ ] T018 [P] Write `core/src/internal/config.hpp` (feature macros `LOOMORA_HAS_BUILTIN_EXPECT`, `LOOMORA_HAS_CXX17_NODISCARD`, compiler detection; no include of any C++ standard header) and `core/src/internal/utf8.hpp` + `core/src/utf8.cpp` (strict UTF-8 validator rejecting overlong forms, surrogates and code points above U+10FFFF)
- [X] T019 (reduced scope: greet + abi_version + build_compiler only) Write `core/src/greet.cpp`: namespace `loomora`, the prefix `"Bonjour "` defined in this single place, input limit 4,096 bytes, returns the byte length or -1 (too long), -2 (cap too small), -3 (null pointer or invalid UTF-8); no allocation, no STL, no exception
- [ ] T020 [P] Write `core/src/eol.cpp`: end-of-life table per family as of 2026-11-12 (Windows before 10 and Windows 10 itself, macOS and iOS versions no longer patched by Apple, Android versions without security patches, Linux kernels past their long-term support; Web always 0), static UTF-8 warning text from FR-013; cite each threshold in `docs/implementation/validation/sources.json`
- [ ] T021 [P] Write `core/src/version.cpp`: `loomora_abi_version()` and `loomora_build_compiler()` (from `__VERSION__`, `__clang_version__` or `_MSC_FULL_VER`, static, never NULL)
- [ ] T022 Write `core/src/probe.cpp`: builds the ProbeResult JSON line (`std::string`) and copies it into the caller buffer, catching every exception at the C boundary, fields and types of contracts/probe-result.schema.json, `greeting_matches` computed with `loomora::greet`, returns the total length needed
- [X] T023 (reduced scope: greet + abi_version + build_compiler only) Write `core/src/c_api.cpp`: the `extern "C"` functions of loomora.h forwarding to the `loomora` namespace; `-1`/negative `family` handled as in the contract
- [X] T024 (reduced scope: greet + abi_version + build_compiler only) Write `core/CMakeLists.txt`: static library `loomora` with `-std=c++11 -fvisibility=hidden` (GCC/Clang) and `/std:c++14` (MSVC); the C++ runtime is linked statically where the platform lacks one, option `LOOMORA_BUILD_SHARED` producing `loomora.dll` for WinUI, header install rule

### Glue with no logic

- [ ] T025 [P] Write `bridge/jni/loomora_jni.cpp`: `Java_fr_loomora_probe_Native_*` functions for abi version, greet, is_eol, eol_warning, probe_record, build_compiler; only `jstring` ↔ UTF-8 conversion, no logic
- [ ] T026 [P] Write `bridge/wasm/exports.txt` (the `_loomora_*` exported symbols and `_malloc`/`_free`) and `bridge/wasm/README.md` (`ccall`/`cwrap` signatures)

### Cross-compile entry points and host tools

- [ ] T027 [P] Write `cmake/toolchains/mingw-w64-win2000-i686.cmake` and `cmake/toolchains/mingw-w64-win-x86_64.cmake` (`_WIN32_WINNT=0x0500`, `WINVER=0x0500`, `-static`, `-static-libgcc`, `-static-libstdc++`)
- [ ] T028 [P] Write `cmake/toolchains/osxcross-macos10.6.cmake` (Clang, `-mmacosx-version-min=10.6`, `i386;x86_64`, 10.6 SDK path variable) and `cmake/toolchains/ios-xcode5.cmake` (Xcode 5.1.1, deployment 5.0, `armv7;armv7s;arm64`)
- [ ] T029 [P] Write `cmake/toolchains/android-ndk-r16b.cmake` (parameter `ANDROID_ABI` among `armeabi`, `armeabi-v7a`, `x86`, `arm64-v8a`; `android-9` platform, `android-21` for `arm64-v8a`; `APP_STL=c++_static`) and `cmake/toolchains/linux-glibc2.12.cmake` (manylinux2010 compiler, `-static-libgcc -static-libstdc++`, `i686` and `x86_64`)
- [ ] T030 [P] Write `cmake/toolchains/emscripten-wasm.cmake` and `cmake/toolchains/emscripten-js.cmake` (`-sWASM=0 -sLEGACY_VM_SUPPORT=1` for the JS build, exports from `bridge/wasm/exports.txt`) and add all cross presets to `CMakePresets.json`
- [ ] T031 Write `tools/build.py <platform> <variant>`: dispatches to the CMake preset or native UI build script, produces the artifact and writes `docs/implementation/validation/deliverables/<platform-id>-<variant>.json` (fields of data-model.md: `compiler`, `target`, `output`, `artifact` name and size, `ui_language`, `ui_toolkit`, `ui_compiler`)
- [ ] T032 [P] Write `tools/check_std.py`: builds `core/` with GCC and Clang using `-std=c++11 -pedantic-errors -Wall -Wextra -Werror` and also builds `libloomora.so` with `-fvisibility=hidden` and fails if `nm -D --defined-only` shows any exported symbol that is not a `loomora_*` C function (research R2)
- [ ] T033 [P] Write `tools/check_probe.py`: validates a probe line against `contracts/probe-result.schema.json` with `jsonschema` and checks `message == "Bonjour " + input`
- [ ] T034 [P] Write `tools/validate_records.py`: checks every file of `docs/implementation/validation/` against data-model.md, quoting its rules: `verdict.status` ∈ {`ok`, `ok_restricted`, `ko`, `not_tested`}; `web_path` mandatory for the `web` family and forbidden otherwise; `restrictions[]` mandatory if `ok_restricted`; `cause` and `attempts[]` mandatory if `ko`; `reason` mandatory if `not_tested`; `evidence[]` (path + sha256) mandatory unless `not_tested`; `in_scope = true` only for a platform with a verdict
- [ ] T035 Write `tools/run_vectors.py <platform-id> <variant>`: launches the built UI with `--probe-input` (or the intent extra, launch argument or URL parameter of ui-contract.md) for every valid vector of `core/tests/vectors.json`, collects each probe line, runs `check_probe.py`, and exits non-zero on any mismatch (SC-011, FR-027)

### Foundational checks

- [ ] T036 Build and run `loomora_tests` with `host-gcc` and `host-clang`; all of T012–T014 must pass
- [ ] T037 Run `python tools/check_std.py` and record spike S6 (strict C++11 under GCC and Clang, only C symbols exported, exceptions caught at the boundary) in `docs/implementation/validation/spikes.json`; if refuted, write the ADR in `docs/implementation/adr/` naming the fallback

**Checkpoint**: backend verified on the host; user stories can start

---

## Phase 3: User Story 1 - See the greeting on current platforms with the Modern UI (Priority: P1) 🎯 MVP

**Goal**: on a current system of each family the Modern UI shows a text field with "Coucou" and the greeting "Bonjour Coucou", updating as the person types.

**Independent Test**: install the Modern UI deliverable on a current device, launch it, see "Bonjour Coucou" in under 3 seconds, type "Marie", see "Bonjour Marie" within 1 second, close cleanly (spec User Story 1).

### Web Modern UI

- [ ] T038 [P] [US1] Create `ui/modern/web-ts/package.json`, `tsconfig.json` and `index.html` (text field pre-filled "Coucou", `maxlength` 1000, greeting element, `<pre id="probe">`)
- [ ] T039 [US1] Write `ui/modern/web-ts/src/native.ts` (loads `loomora.wasm` with `WebAssembly.instantiateStreaming`, wraps `loomora_greet` with `cwrap`, UTF-8 conversion with `TextEncoder`/`TextDecoder`) and `ui/modern/web-ts/src/main.ts` (greeting on load and on every `input` event, `?probe` / `?probe&input=` handling, `core = cpp-wasm` probe record via `loomora_probe_record`, `console.log` + `<pre id="probe">`)
- [ ] T040 [US1] Add the `web` target to `tools/build.py` building `loomora.wasm` with the current Emscripten and the TypeScript compile into `ui/modern/web-ts/dist/`

### Windows Modern UI (C#, .NET, WinUI 3)

- [ ] T041 [P] [US1] Add the `host-msvc` build of `loomora.dll` (x64 and ARM64) in `CMakePresets.json` and `core/CMakeLists.txt` (`LOOMORA_BUILD_SHARED=ON`, `/MT`, exports through `LOOMORA_API`)
- [ ] T042 [US1] Create `ui/modern/windows-winui3/Loomora.WinUI.csproj`: WinUI 3 on **.NET 8 or later** (`net8.0-windows10.0.19041.0`, `TargetPlatformMinVersion` 10.0.17763.0), Windows App SDK, unpackaged, self-contained, platforms x64 and ARM64, `AllowUnsafeBlocks`
- [ ] T043 [US1] Write `ui/modern/windows-winui3/Native.cs`: `[LibraryImport("loomora")]` partial methods for `loomora_abi_version`, `loomora_greet`, `loomora_is_eol`, `loomora_eol_warning`, `loomora_build_compiler`, `loomora_probe_record` with UTF-8 marshalling; no C++/CLI
- [ ] T044 [US1] Write `ui/modern/windows-winui3/App.xaml`, `App.xaml.cs`, `MainWindow.xaml` and `MainWindow.xaml.cs`: `TextBox` pre-filled "Coucou" (`MaxLength` 1000), `TextBlock` with wrapping showing the result of `Native.Greet`, `InfoBar` for the end-of-life warning, theme resources so dark and high-contrast themes keep contrast ≥ 4.5:1, ABI version check at startup
- [ ] T045 [US1] Write `ui/modern/windows-winui3/Probe.cs`: `--probe`, `--probe-input <utf8>`, time from launch to first frame, `Process.PeakWorkingSet64`, probe line to stdout (attach to the parent console with `AttachConsole`) and to `loomora-probe.json`, exit after 2 seconds
- [ ] T046 [US1] Add the `windows-11` target to `tools/build.py`: `dotnet publish` for x64 and ARM64 plus the matching `loomora.dll`, zipped unpackaged deliverables

### macOS Modern UI (Swift, SwiftUI)

- [ ] T047 [P] [US1] Create `ui/modern/macos-swiftui/Loomora.xcodeproj` (deployment target macOS 11, Universal 2) with `module.modulemap` exposing `core/include/loomora.h`
- [ ] T048 [US1] Write `ui/modern/macos-swiftui/Native.swift`, `LoomoraApp.swift`, `ContentView.swift` (`TextField` "Coucou" limited to 1,000 characters, `Text` with the greeting from `loomora_greet`, end-of-life banner) and `Probe.swift` (`--probe`, `--probe-input`, `task_info` resident size, stdout line, exit after 2 seconds)
- [ ] T049 [US1] Add the `macos-modern` target to `tools/build.py`: static `libloomora.a` for `x86_64` and `arm64` merged with `lipo`, then `xcodebuild`

### iOS Modern UI (Swift, SwiftUI)

- [ ] T050 [P] [US1] Create `ui/modern/ios-swiftui/Loomora.xcodeproj` (deployment target iOS 15, arm64) with the same `module.modulemap`
- [ ] T051 [US1] Write `ui/modern/ios-swiftui/Native.swift`, `LoomoraApp.swift`, `ContentView.swift` and `Probe.swift` (launch arguments `-probe` and `-probe_input <utf8>`, `NSLog` probe line, `task_info`) mirroring T048
- [ ] T052 [US1] Add the `ios-modern` target to `tools/build.py` (static library `aarch64-apple-ios` with the current Xcode, `xcodebuild archive`)

### Android Modern UI (Kotlin, Jetpack Compose)

- [ ] T053 [P] [US1] Create the Gradle project `ui/modern/android-compose/` (AGP 9, `compileSdk` 37, `minSdk` 24, Compose BOM, `externalNativeBuild` CMake building `core/` and `bridge/jni/loomora_jni.cpp` with the current NDK, ABIs `armeabi-v7a`, `arm64-v8a`, `x86`, `x86_64`)
- [ ] T054 [US1] Write `ui/modern/android-compose/app/src/main/java/fr/loomora/probe/Native.kt`, `MainActivity.kt` (Compose `OutlinedTextField` "Coucou" limited to 1,000 characters, `Text` with the greeting, end-of-life notice, dark theme and high contrast) and `Probe.kt` (intent extras `probe` and `probe_input`, `/proc/self/status` `VmHWM`, `Log` tag `LOOMORA_PROBE`, finish after 2 seconds)
- [ ] T055 [US1] Add the `android-modern` target to `tools/build.py` (`gradlew assembleRelease`, signed APK)

### Linux Modern UI (C, GTK 3)

- [ ] T056 [P] [US1] Create `ui/modern/linux-gtk3/meson.build` (dependency `gtk+-3.0`, static `libloomora.a` from `core/`, no Flatpak)
- [ ] T057 [US1] Write `ui/modern/linux-gtk3/main.c` using only the GTK 3.10 API subset (`GtkWindow`, `GtkBox`, `GtkEntry` pre-filled "Coucou" with max length 1,000, `GtkLabel` wrapping the greeting from `loomora_greet`, `GtkInfoBar` for the end-of-life warning) and `ui/modern/linux-gtk3/probe.c`/`probe.h` (`--probe`, `--probe-input <utf8>`, `VmHWM`, stdout line, `g_timeout_add` exit after 2 seconds)
- [ ] T058 [US1] Add the `linux-modern` target to `tools/build.py` (`meson setup` and `ninja` on the build distribution)

### Trials of the Modern UIs

- [ ] T059 [P] [US1] Create `docs/implementation/validation/platforms/*.json` (data-model.md Platform) for the current targets: Windows 11 x64, Windows 11 ARM64, Windows 10 1809 x64, macOS (Apple Silicon and Intel), iOS 15+ device or simulator, Android 7.0 and a current Android, Ubuntu 22.04 and a current distribution, a current Chrome, Firefox and Safari
- [ ] T060 [US1] Run Windows 11 x64 and ARM64 trials: install, launch with `--probe`, check "Bonjour Coucou" under 3 s, type "Marie" → "Bonjour Marie" within 1 s, dark theme and high contrast legible, clean close with no leftover process; save screenshot and probe JSON in `docs/implementation/validation/evidence/<change>/<id>/` and fill `docs/implementation/validation/verdicts/<id>-modern.json`
- [ ] T061 [P] [US1] Run the macOS and iOS Modern trials (same checks) and save evidence and verdicts under `docs/implementation/validation/evidence/` and `docs/implementation/validation/verdicts/`
- [ ] T062 [P] [US1] Run the Android and Linux Modern trials (same checks) and save evidence and verdicts under `docs/implementation/validation/evidence/` and `docs/implementation/validation/verdicts/`
- [ ] T063 [P] [US1] Run the Web Modern trial in current Chrome, Firefox and Safari (`core = cpp-wasm`, `web_path = wasm`) and save evidence and verdicts
- [ ] T064 [US1] Run `tools/run_vectors.py` for every Modern UI of T060–T063 (SC-011) and attach the log as evidence; record spike S8 (WinUI 3 calls `loomora.dll`; Compose accepts `minSdk` 21 or not) in `docs/implementation/validation/spikes.json`

**Checkpoint**: User Story 1 is complete — the Modern UIs work on current systems (MVP)

---

## Phase 4: User Story 2 - Push the compatibility floor back with the Legacy UI (Priority: P1)

**Goal**: for each family, run the Legacy UI on older and older systems down to the decided floors and record whether the greeting is displayed and updates.

**Independent Test**: pick a family, install an old system within its range (physical, virtual or emulated), launch the Legacy UI, record the verdict (spec User Story 2).

### Spikes (gate the toolchain tasks of each family)

- [ ] T065 [US2] Spike S7: confirm that the 10.6 SDK, Xcode 5.1.1, Windows 2000 SP4 and the old Android and iOS images can be obtained legitimately; record the result and any "not tested" consequence in `docs/implementation/validation/spikes.json`
- [ ] T066 [P] [US2] Spike S1: cross-compile `loomora_selftest` with `cmake/toolchains/mingw-w64-win2000-i686.cmake`, check imports with `objdump -p` (nothing above Windows 2000), run it on a Windows 2000 SP4 VM; record the result and, if refuted, the ADR naming the fallback (research R6)
- [ ] T067 [P] [US2] Spike S2: cross-compile `loomora_selftest` for macOS 10.6 (`osxcross-macos10.6.cmake`) and run it on 10.6; build the same for iOS 5 with Xcode 5.1.1 and run it on an iOS 5 simulator or device; record the results
- [ ] T068 [P] [US2] Spike S3: build `loomora_selftest` for the four Android ABIs with NDK r16b `android-9` and run it on an Android 2.3 emulator or device with `adb`; record the result
- [ ] T069 [P] [US2] Spike S4: build `loomora_selftest` in the manylinux2010 container (i686 and x86_64), check `objdump -T` shows glibc symbols ≤ 2.12, run it on a CentOS 6 or Debian 6 image; record the result
- [ ] T070 [P] [US2] Spike S5: build the JavaScript path first with the current Emscripten (`emscripten-js.cmake`), run a minimal page on IE 9, Firefox 8, Chrome 15, Safari 5.1 and Opera 11.5; if it fails, rebuild with Emscripten 1.39.20 fastcomp and write the ADR (research R8)

### Windows Legacy UI (C++, plain Win32, no MFC)

- [ ] T071 [US2] Create `ui/legacy/windows-win32/CMakeLists.txt` (executable `loomora`, `-mwindows -static`, `_WIN32_WINNT=0x0500`, links `libloomora.a`) and `ui/legacy/windows-win32/loomora.rc` + `loomora.manifest` (no post-2000 requirement, UTF-8 resource strings)
- [ ] T072 [US2] Write `ui/legacy/windows-win32/main.cpp`: `WinMain`, window with an `EDIT` control pre-filled "Coucou" (limit 1,000 characters) and a `STATIC` control showing the greeting from `loomora_greet`, UTF-16 ↔ UTF-8 conversion with `WideCharToMultiByte`/`MultiByteToWideChar`, ABI version check, clean exit code 0
- [ ] T073 [P] [US2] Write `ui/legacy/windows-win32/sysinfo.cpp` (Windows version through `RtlGetVersion` loaded dynamically, falling back to `GetVersionEx`, end-of-life warning through `loomora_is_eol`) and `ui/legacy/windows-win32/dynfeatures.cpp` (dark mode, DPI awareness, high-contrast query, `GetProcessMemoryInfo` all loaded with `GetProcAddress` and skipped when absent; each skipped feature added to `disabled_features` and declared in `docs/implementation/validation/fallbacks.json`)
- [ ] T074 [US2] Write `ui/legacy/windows-win32/probe.cpp`: `--probe`, `--probe-input <utf8>`, probe line written to `loomora-probe.json` next to the executable and to `OutputDebugString`, exit after 2 seconds
- [ ] T075 [US2] Add the `windows-legacy` target to `tools/build.py` (x86 for Windows 2000 → 11, x64 for XP x64 → 11)

### macOS Legacy UI (Objective-C, AppKit)

- [ ] T076 [P] [US2] Write `ui/legacy/macos-appkit/main.m`, `AppDelegate.h`, `AppDelegate.m` (an `NSWindow` with an `NSTextField` pre-filled "Coucou" and a label showing the greeting from `loomora_greet`, 1,000-character limit, end-of-life banner), `Probe.m` (`--probe`, `--probe-input`, `task_info`, stdout line, exit after 2 seconds) and `Info.plist` (`LSMinimumSystemVersion` 10.6)
- [ ] T077 [US2] Add the `macos-legacy` target to `tools/build.py` (Clang + osxcross + 10.6 SDK, `i386` and `x86_64` fat binary, `.app` bundle)

### iOS Legacy UI (Objective-C, UIKit)

- [ ] T078 [P] [US2] Write `ui/legacy/ios-uikit/main.m`, `AppDelegate.m`, `ViewController.m` (a `UITextField` pre-filled "Coucou" and a `UILabel` showing the greeting from `loomora_greet`, manual retain/release, 1,000-character limit), `Probe.m` (`-probe`, `-probe_input`, `NSLog`) and `Info.plist` (`MinimumOSVersion` 5.0), plus `ui/legacy/ios-uikit/Loomora.xcodeproj` for Xcode 5.1.1 (`armv7`, `armv7s`, `arm64`)
- [ ] T079 [US2] Add the `ios-legacy` target to `tools/build.py` (Xcode 5.1.1, deployment 5.0)

### Android Legacy UI (Java, framework views)

- [ ] T080 [P] [US2] Write `ui/legacy/android-views/AndroidManifest.xml` (`minSdkVersion` 9), `src/fr/loomora/probe/Native.java`, `MainActivity.java` (an `EditText` "Coucou" limited to 1,000 characters with a `TextWatcher`, a `TextView` showing the greeting, end-of-life notice) and `Probe.java` (intent extras `probe` and `probe_input`, `VmHWM`, `Log` tag `LOOMORA_PROBE`, finish after 2 seconds)
- [ ] T081 [US2] Write `ui/legacy/android-views/Android.mk` and `Application.mk` (NDK r16b, `APP_PLATFORM := android-9`, `APP_STL := c++_static`, ABIs `armeabi armeabi-v7a x86 arm64-v8a`, building `core/` and `bridge/jni/loomora_jni.cpp`) and `ui/legacy/android-views/build.sh` (`aapt`, `javac -source 7 -target 7`, `dx`, `zipalign`, `jarsigner` v1 signing; build-tools 28.0.3 or older)
- [ ] T082 [US2] Add the `android-legacy` target to `tools/build.py` calling `build.sh`

### Linux Legacy UI (C, Xlib)

- [ ] T083 [P] [US2] Write `ui/legacy/linux-xlib/main.c` (an Xlib window, an input line pre-filled "Coucou" handled with `KeyPress` and `XLookupString` limited to 1,000 bytes, the greeting from `loomora_greet` drawn with the core font `fixed`, wrapped within the window, ABI check, clean close through `WM_DELETE_WINDOW`) and `ui/legacy/linux-xlib/probe.c` (`--probe`, `--probe-input <utf8>`, `VmHWM`, stdout line, exit after 2 seconds, end-of-life banner drawn in the window)
- [ ] T084 [US2] Write `ui/legacy/linux-xlib/Makefile` (links only `libX11` dynamically and the static `libloomora.a`) and add the `linux-legacy` target to `tools/build.py` running the manylinux2010 container for `i686` and `x86_64`

### Web Legacy UI (ES5)

- [ ] T085 [P] [US2] Write `ui/legacy/web-es5/index.html`, `loader.js` (ES5 only: if `typeof WebAssembly === "object"` load `loomora.wasm`, otherwise `loomora.js`) and `app.js` (text field "Coucou" with `maxlength` 1000 and `onkeyup`/`oninput`/`onpropertychange` listeners, greeting through `ccall`, `?probe&input=` parameter, `core = cpp-wasm` or `cpp-js` in the probe record, `<pre id="probe">`)
- [ ] T086 [US2] Add the `web-legacy` target to `tools/build.py` producing `loomora.wasm` (current Emscripten) and `loomora.js` (current Emscripten or 1.39.20 fastcomp, as decided by T070) into `ui/legacy/web-es5/dist/`

### Platform trials (one verdict per platform and variant; record disabled features and fallbacks)

- [ ] T087 [US2] Create `docs/implementation/validation/platforms/*.json` for the Legacy ladders (`in_scope` and `is_floor` set per data-model.md): Windows 2000 SP4 x86, XP SP3 x86, XP x64, Vista, 7, 8.1, 10, 11; macOS 10.6, 10.7, 10.9, 10.13, 11+; iOS 5, 6, 7, 9, 12, 15+; Android 2.3, 4.0, 4.4, 5.0, 7.0, 14+; Linux kernel 2.6.32 / glibc 2.12 (CentOS 6 or Debian 6), Ubuntu 14.04, Ubuntu 22.04; browsers IE 9, Firefox 8, Chrome 15, Safari 5.1, Opera 11.5 (JS path) and a current browser (WebAssembly path), plus IE 6 to 8 as best effort
- [ ] T088 [P] [US2] Run the Windows Legacy ladder: for every platform of T087 launch with `--probe`, check "Bonjour Coucou" and typing "Marie", the end-of-life warning on every end-of-life system, then save evidence and fill `docs/implementation/validation/verdicts/<id>-legacy.json`; the oldest working Windows is recorded with evidence (FR-005)
- [ ] T089 [P] [US2] Run the macOS Legacy ladder with the same checks and verdict files
- [ ] T090 [P] [US2] Run the iOS Legacy ladder (arm64, armv7, armv7s slices) with the same checks; a missing 32-bit slice or device is recorded as a restriction or `not_tested`
- [ ] T091 [P] [US2] Run the Android Legacy ladder (all four ABIs) with the same checks and verdict files
- [ ] T092 [P] [US2] Run the Linux Legacy ladder (i686 and x86_64) with the same checks and verdict files
- [ ] T093 [P] [US2] Run the Web Legacy ladder: one verdict per browser **and per path** (`web_path` = `wasm` or `js`), recording the oldest browser that works for each path (FR-023)
- [ ] T094 [US2] Run `tools/run_vectors.py` for every Legacy UI that reached a verdict `ok` or `ok_restricted` (SC-011) and attach the logs as evidence
- [ ] T095 [US2] For each KO platform, write the cause and `attempts[]` (at least two solutions tried, SC-003) in its verdict; for each platform without Internet access write the offline installation route (file or removable media) in `docs/implementation/validation/procedures/offline-install.md`
- [ ] T096 [US2] Run `python tools/validate_records.py` and fix every violation of the verdict, platform and deliverable rules

**Checkpoint**: User Stories 1 and 2 work — the oldest working platform is known per family and variant

---

## Phase 5: User Story 3 - Validation report of the technical specifications (Priority: P1)

**Goal**: one report confronting the requirements document with the old platforms actually tested.

**Independent Test**: read `docs/implementation/validation/report.md` alone and find for each platform and variant its verdict, evidence, exact system version and how to reproduce (spec User Story 3).

- [ ] T097 [US3] Extract rows V1–V20 of docs/requirements-specification.md §15.1.4 into `docs/implementation/validation/hypotheses.json` (`id`, `title`, `criterion`, `fallback`, `covered_by_probe`, `platform_ids[]`, `status`); mark V2–V5 and V11–V18 `not_covered` with the reason unless the probe exercises them (FR-007, SC-005)
- [ ] T098 [P] [US3] Write `tools/report.py` generating `docs/implementation/validation/report.md`: platform × variant × verdict matrix (including `web_path`), oldest working platform per family and variant, measurements (artifact size, `display_ms`, `peak_rss_kb` or "not measured" with the reason), fallbacks and disabled features per platform, spikes S1–S9 with results, hypotheses V1–V20
- [ ] T099 [US3] Add to `tools/report.py` the recommendation sections: floors of the requirements document contradicted by the trials (macOS 11, iOS 15, Windows 10, Ubuntu 20.04, API 21), tiers A/B/C, disabled features, the proposed change for each (FR-008, SC-009)
- [ ] T100 [US3] Add the "deviations from the requirements document" section to `tools/report.py`: backend in C++11 instead of Rust, Win32 instead of MFC for the Windows Legacy UI, GTK 3 instead of GTK 4 for the Linux Modern UI (spec Clarifications)
- [ ] T101 [US3] Write the go / no-go conclusion on Windows 2000 and Windows XP (OK, OK with restrictions or KO) in `docs/implementation/validation/report.md` through `tools/report.py` (SC-009)
- [ ] T102 [US3] Generate `docs/implementation/validation/report.md` and check that every attempted platform and variant has a verdict, evidence and a reproduction path (SC-004, SC-010); a missing variant is justified in the report
- [ ] T103 [P] [US3] Add a check to `tools/report.py` that SC-002 and SC-003 hold (at least 3 families validated on a system 10 years old or more; at least 1 family on a system 15 years old or more, or the impossibility demonstrated with two solutions tried per family) and print the result at the top of the report

**Checkpoint**: User Stories 1, 2 and 3 work — the report supports the go / no-go decision

---

## Phase 6: User Story 4 - Share as much code as possible (Priority: P2)

**Goal**: one change of the greeting appears on every platform and variant; the shared-code share is measured and published.

**Independent Test**: change the prefix to "hello " in `core/src/greet.cpp`, rebuild everything, see "hello Coucou" everywhere with no other change (spec User Story 4).

- [ ] T104 [US4] Write `tools/measure_sharing.py`: counts non-empty, non-comment lines of `core/` (backend), `bridge/` (glue) and each `ui/<variant>/<platform>/` (native UI), excludes generated code and packaging, and writes `sharing_ratio = backend / (backend + glue + ui)` into every `docs/implementation/validation/deliverables/*.json` (research R9)
- [ ] T105 [US4] Write `tools/prefix_check.py`: temporarily changes the prefix in `core/src/greet.cpp` to `"hello "`, rebuilds every platform and variant available on the build machine, runs `tools/run_vectors.py` expecting `"hello " + input`, verifies with `git diff` that only `core/` and the generated vectors changed, then restores the file (SC-006)
- [ ] T106 [US4] Run `tools/prefix_check.py` and `tools/measure_sharing.py` and save the output as evidence under `docs/implementation/validation/evidence/sharing/`
- [ ] T107 [US4] Add to `tools/report.py` the shared-code table per platform and variant and the list of every piece of platform-specific code with its justification (FR-011, FR-012, SC-007); any share below 80% is explained
- [ ] T108 [US4] Check that no UI builds the greeting itself or contains application logic: add `tools/check_ui_logic.py` searching `ui/` for the literal `Bonjour` and `hello` outside test and documentation files, and run it (FR-010, FR-029)

**Checkpoint**: User Stories 1–4 work

---

## Phase 7: User Story 5 - Replay the validation identically (Priority: P3)

**Goal**: a person who did not run the trials gets the same verdict from the documentation alone.

**Independent Test**: someone else follows the procedure of an old platform and finds the recorded verdict (spec User Story 5).

- [ ] T109 [P] [US5] Write `docs/implementation/validation/procedures/windows.md` and `docs/implementation/validation/procedures/macos.md` (system image or machine, exact tool versions, build commands from `tools/build.py`, run commands, expected probe line, where to save evidence; FR-016)
- [ ] T110 [P] [US5] Write `docs/implementation/validation/procedures/ios.md` and `docs/implementation/validation/procedures/android.md` in the same format
- [ ] T111 [P] [US5] Write `docs/implementation/validation/procedures/linux.md` and `docs/implementation/validation/procedures/web.md` in the same format
- [ ] T112 [US5] Write `tools/diff_verdicts.py` comparing two sets of `docs/implementation/validation/verdicts/` (two git revisions or two folders) and printing every `status` change; add its output as a section of `tools/report.py` (FR-018, spec User Story 5 scenario 2)
- [ ] T113 [US5] Have a person who did not take part in the trials replay at least 3 old platforms from the procedures alone and record the comparison in `docs/implementation/validation/evidence/replay/` (SC-008); any difference leads to a fix of the procedure

**Checkpoint**: all user stories work

---

## Phase 8: Polish & Cross-Cutting Concerns

- [ ] T114 [P] Write an ADR in `docs/implementation/adr/` for every structuring decision: the cross toolchain per family, Win32 instead of MFC, GTK 3 instead of GTK 4, no C++/CLI for WinUI 3, Python host tools, the JavaScript build tool version chosen by spike S5 (FR-017)
- [ ] T115 [P] Fill `docs/implementation/validation/fallbacks.json` from the code: every `LOOMORA_HAS_*` macro and every feature skipped in `ui/legacy/windows-win32/dynfeatures.cpp`, with `platform_ids[]` and `reason` (FR-021)
- [ ] T116 [P] Update `docs/requirements-specification.md` with the proposed changes of `docs/implementation/validation/report.md` only after the project owner accepts them (the document is not modified automatically)
- [ ] T117 Write `README.md` at the repository root: goal, layout, how to build and run each scenario of quickstart.md
- [ ] T118 Run every scenario of quickstart.md (0 to 7) on the build machine and fix the commands that no longer match the code
- [ ] T119 Run `python tools/validate_records.py`, `python tools/check_std.py` and `python tools/check_ui_logic.py` one last time and regenerate `docs/implementation/validation/report.md`
- [ ] T120 Run `/speckit-constitution` if the project owner wants principles recorded before the product phases (constitution.md is still the unfilled template)

---

## Dependencies & Execution Order

- **Setup (Phase 1)** → **Foundational (Phase 2)** → user stories. Phase 2 blocks everything.
- **US1** (Modern UIs) and **US2** (Legacy UIs) both depend only on Phase 2 and can run in parallel; the Android and Web Legacy UIs reuse the glue of T025/T026.
- **US3** (report) needs the verdicts of US1 and US2 for its final content; its tools (T098–T101) can be written earlier.
- **US4** needs built UIs from US1/US2; **US5** needs the trials of US2.
- Inside a story: tests/spikes first, then code, then trials.

### Spike gates

| Spike | Gates |
|-------|-------|
| S6 (T037) | everything in Phase 3 and 4 (backend compiles strictly) |
| S1 (T066) | T071–T075, T088 |
| S2 (T067) | T076–T079, T089, T090 |
| S3 (T068) | T080–T082, T091 |
| S4 (T069) | T083–T084, T092 |
| S5 (T070) | T085–T086, T093 |
| S7 (T065) | every trial that needs an old image, SDK or device |
| S8 (T064) | Android Modern floor (API 21 or 24) in the report |

## Parallel Examples

- Phase 2: T011–T014 together after T010; T025–T030 together; T032–T034 together.
- US1: Web (T038–T040), Windows (T041–T046), Apple (T047–T052), Android (T053–T055) and Linux (T056–T058) are independent families; T060–T063 trials run on separate machines.
- US2: spikes T066–T070 run in parallel; the six family UIs (T071–T086) are independent once their spike is confirmed; ladders T088–T093 run on separate machines.

## Implementation Strategy

1. **MVP**: Phase 1 + Phase 2 + **US1** — the greeting works on current systems through the Modern UIs, with the backend fully tested. Stop and validate.
2. Add **US2** family by family, starting with the spike of that family; a refuted spike produces an ADR and a fallback (or a `ko` / `not_tested` verdict) instead of blocking the other families.
3. Add **US3** to produce the go / no-go report, then **US4** and **US5** to harden sharing and replayability.
4. After each family, run `tools/validate_records.py` so no platform stays without a verdict (SC-004).
