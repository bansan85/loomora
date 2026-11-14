# Research: Loomora cross-platform (single C++11 backend, Legacy + Modern UIs)

Date: 2026-11-12. Rewritten after the backend moved from Rust to C++11 (spec, second Clarifications session), then updated for the third session (Win32 instead of MFC, GTK 3, no C++/CLI). Floors come from the spec (Windows 2000, macOS 10.6, Android 2.3, iOS 5, Linux with a C++11 compiler installable on it, Nov-2011 browsers). Every technical statement below is a **working hypothesis** drawn from published documentation and prior knowledge; each one is confirmed or refuted by a numbered **spike (S1–S9, S4 removed on 2026-11-14, ADR 0007)** before the matching tasks rely on it. Anything not yet verified is marked "to verify".

## R1 — Architecture: one C++11 backend behind a C API, two native UIs per platform

- **Decision**: one static library, `libloomora` (namespace `loomora`), written in **strict C++11** and exposing a **C API** (`loomora_` prefix, handwritten header `loomora.h`, see [c-api.md](contracts/c-api.md)). Each platform has **two thin UIs in its native language** (Legacy and Modern, [ui-contract.md](contracts/ui-contract.md)) calling only that API. Thin glue files adapt the C API to JNI (Android) and to Emscripten exports (Web); they contain no logic.
- **Backend language rule**: the backend is written in **idiomatic C++11** and may use the whole C++ language and the C++11 standard library: STL containers and strings, dynamic allocation, exceptions, RTTI, `<iostream>` where useful. Everything that is C++ stays **behind the C API**: (a) no C++ type, template or exception appears in `loomora.h`; (b) every exported function catches all exceptions and turns them into a negative error code, so none crosses the C boundary; (c) memory crosses the boundary only in caller-supplied buffers (no "free this" function); (d) the library links the C++ runtime **statically** where the platform has none or an old one (Windows: `-static`, `-static-libgcc -static-libstdc++`; Android: `APP_STL := c++_static`), so a UI in any language only needs the C symbols; on Linux libstdc++ and libgcc are always linked **dynamically** (the system's own, ADR 0007).
- **Rationale**: the user dropped Rust because supporting old platforms was too complicated; a C++11 library keeps the single-implementation rule (FR-019). The user lifted the earlier "runtime-free" restriction (2026-11-12): only the C binding needs care.
- **Alternatives rejected**: a C89/C99 backend (forbidden by the user: C++11 requested); a header-only backend (forces every UI toolchain to parse C++11, which Xcode 3.2.6 and other old UI compilers cannot); Qt/Flutter (floors too high); one UI per platform (rejected by the Legacy/Modern decision).

## R2 — One C++11 source, many compilers, declared fallbacks

- **Conformance check**: CI builds `core/` with GCC and Clang on the host using `-std=c++11 -pedantic-errors -Wall -Wextra -Werror`, and a link check (`nm -D` on a shared build with `-fvisibility=hidden`) that the only symbols exported are the `loomora_*` C functions. MSVC cannot be put in strict C++11 mode (its lowest selectable standard is `/std:c++14`), so MSVC is never the conformance reference; it only proves the same source compiles.
- **Fallbacks (FR-021, FR-022)**: recent compiler or system features are used only behind feature macros defined in one internal header (`core/src/internal/config.hpp`): for example `LOOMORA_HAS_BUILTIN_EXPECT`, `LOOMORA_HAS_CXX17_NODISCARD` (from `__cplusplus >= 201703L`), `LOOMORA_HAS_CLOCK_MONOTONIC` (UI-side timing). Each fallback is declared in `docs/implementation/validation/fallbacks.json` with the platforms that take the fallback path. The backend never *requires* a feature newer than C++11.
- **Compiler version**: the library embeds a compiler identifier string (`__VERSION__`, `__clang_version__` or `_MSC_FULL_VER`) returned by `loomora_build_compiler()` and copied to the probe record (`compiler` field).

## R3 — Android (Android 2.3 / API 9 → current)

- **Findings**: the NDK dropped API levels below 14 and the ARMv5 (`armeabi`), MIPS and MIPS64 ABIs in **r17**; **r16b** is the last NDK with `android-9` platforms and `armeabi`. r16b ships Clang 5 and GCC 4.9 and supports `-std=c++11`.
- **Legacy UI**: **Java** (`Activity` + `EditText` + `TextView`, plain Android framework, no AndroidX) with `minSdkVersion` 9. JNI glue (`bridge/jni/loomora_jni.cpp`) and the backend are built with **NDK r16b**, `APP_PLATFORM=android-9`, **no STL** (`APP_STL := system`, unused by the runtime-free backend). ABIs: `armeabi` (ARMv5/v6), `armeabi-v7a`, `x86`, plus `arm64-v8a` (built with the same r16b, needs API 21+ platform) so the Legacy APK also runs on 64-bit-only phones.
- **Java/APK build**: Gradle plugins refuse API levels this low, so the Legacy APK is built with a **scripted command-line build** (`aapt`, `javac -source 7 -target 7` from JDK 8, `dx`, `zipalign`, `jarsigner` for the v1 signing scheme, which is all API 9 understands). Build-tools 28.0.3 or earlier (the last with `dx`).
- **Modern UI** (decided): **Kotlin + Jetpack Compose**, current NDK (r27+), `minSdk` 24 planned (21 only if the pinned Compose/AndroidX versions accept it). Below it, the Legacy UI is the fallback.
- **To verify (S3)**: an API 9/10 emulator (or a device) boots and installs the APK; `armeabi` library loads; the API 9 `EditText` text-change listener works as expected.

## R4 — macOS (10.6 Snow Leopard → current)

- **Findings**: Xcode 3.2.6 (the last Xcode for 10.6) only ships GCC 4.2 and llvm-gcc: no C++11. Recent upstream **LLVM Clang** can still target `-mmacosx-version-min=10.6` when given the 10.6 SDK, using a cross toolchain such as **osxcross** (cctools-port ld64) on a Mac; the 10.6 system C++ library is the old GCC 4.2 `libstdc++`, which is irrelevant here because the backend is runtime-free (R1).
- **Legacy UI**: **Objective-C / AppKit** (`NSWindow`, `NSTextField` for input, `NSTextField` label) compiled against `MacOSX10.6.sdk`, linked with the static `libloomora.a`. Architectures: `i386` and `x86_64` in one fat binary (spec: x86 and x64). Older macOS (10.5 and earlier, PowerPC) is out of scope; best-effort attempts below 10.6 are recorded only if a machine is at hand.
- **Modern UI** (decided): **Swift + SwiftUI** (`App` lifecycle), deployment target **macOS 11**, Universal 2 (x86_64 + arm64), current Xcode; the C header is imported through a module map.
- **To verify (S2)**: modern Clang + the 10.6 SDK produces a binary that launches on a 10.6 machine or VM; the license of the 10.6 SDK/VM requires Apple hardware (build and test on a Mac).

## R5 — iOS (5 → current)

- **Findings**: **Xcode 5.1.1** is the last Xcode that can deploy to iOS 4.3–5.x and builds `armv7`, `armv7s` and `arm64`; its Apple Clang 5.1 implements C++11. Current Xcode deploys to iOS 12 at the lowest, so the Legacy UI needs the old Xcode on a Mac running OS X 10.8/10.9 (physical or VM on Apple hardware).
- **Legacy UI**: **Objective-C / UIKit** (`UITextField` + `UILabel` in a `UIViewController`), deployment target **5.0**, slices `armv7`, `armv7s`, `arm64` (the `arm64` slice only runs on iOS 7+ devices, which is a property of the hardware, not a restriction). ARC with `weak` is available from iOS 5; the UI uses manual retain/release to stay safe on 5.0.
- **Modern UI** (decided): **Swift + SwiftUI**, deployment target **iOS 15** (requirements document decision no. 2), `arm64`, current Xcode.
- **To verify (S2)**: Xcode 5.1.1 installs and signs on a supported build Mac; an iOS 5.x simulator or device is available (otherwise verdict "not tested" with the reason); how code signing for iOS 5 devices still works today.

## R6 — Windows (2000 → 11)

- **Findings**: Visual Studio 2005 SP1 is the **last MSVC that produces binaries for Windows 2000** (VS2008 and later drop it: their C runtime needs APIs such as `EncodePointer` from XP SP2 and side-by-side manifests), but it does not support C++11, and no MSVC that supports C++11 targets Windows 2000 (VS2012+ targets XP SP3 at best). MFC therefore cannot be combined with a C++11 backend on Windows 2000, and the user dropped MFC (spec, third Clarifications session). **MinGW-w64** (GCC, `-std=c++11`) with `-D_WIN32_WINNT=0x0500 -DWINVER=0x0500` and the system `msvcrt.dll` can produce binaries for Windows 2000; this is the only practical C++11 compiler for that system.
- **Decision (one toolchain)**: build the **backend** (R1) **and** the **Legacy UI** with **MinGW-w64 GCC**, linking `libstdc++` and `libgcc` statically (`-static`). The Legacy UI is written in C++ on the **plain Win32 API** (`CreateWindowEx`, an `EDIT` control and a `STATIC` control, `-mwindows -static`, no MFC), linked to the static `libloomora.a` (`i686` for 2000 → 11, `x86_64` for XP x64 → 11). The UI is limited to the Win32 calls available on Windows 2000 (Unicode `W` functions, `_WIN32_WINNT=0x0500`); newer features (theme/dark mode, DPI awareness, high contrast queries) are loaded dynamically with `GetProcAddress` and skipped when absent (fallbacks, FR-021). One executable runs on Windows 2000 → 11.
- **Fallbacks, in order, if a spike fails** (each recorded as an ADR): (1) ship the backend as `loomora.dll` (still MinGW) instead of a static library; (2) a different MinGW-w64 version or thread/exception model; (3) give Windows 2000 a "KO" or "not tested" verdict with the cause.
- **Modern UI** (decided): **WinUI 3** (C#, Windows App SDK, Windows 10 1809+, x64 and ARM64) calling a `loomora.dll` built by current MSVC (`/std:c++14`, lowest MSVC accepts; source stays C++11) through `LibraryImport` (.NET 7+). Windows 7 → 10 1809 use the Legacy Win32 UI.
- **To verify (S1)**: Windows 2000 SP4 boots in a VM; a MinGW-w64 binary with `_WIN32_WINNT=0x0500` runs (no import of post-2000 APIs: check with `objdump -p`); the Win32 UI shows the greeting and updates it on typing; a Windows 2000 SP4 image can be obtained legitimately.

## R7 — Linux (any distribution with a C++11 compiler → current)

- **Findings**: Linux binaries are **always built on the machine that runs them** (owner decision, 2026-11-14, ADR 0007): never built on a recent OS and installed on an old one, so the glibc baseline of a build host does not matter and there is no container and no cross compiler for Linux. The target machine needs a C++11 compiler, CMake and the libX11 development files (libXft and fontconfig ones for the Xft text path); a very old distribution may need a newer compiler installed.
- **Legacy UI**: **C++ + Xlib** (C++11, RAII classes for the Xlib and Xft handles; decided by the owner on 2026-11-14, it was C with a Makefile before; built with CMake; `libX11`, and `libXft` with fontconfig when CMake finds them, linked normally, plus the system's C++ runtime linked dynamically; works under XWayland), built natively on the target machine and linked to the static `libloomora.a`; architectures `x86_64` and `i686` (spec). Text drawn with Xft when it was found; when CMake finds no Xft the UI is built with the core X11 font `fixed` (fallback `xlib-core-font-glyphs`); no toolkit.
- **Modern UI** (decided by the user, 2026-11-12): **GTK 3 with gtkmm, written in C++ and built with CMake** (owner decision 2026-11-14: it first was GTK 3 written in C with Meson; no libadwaita; GTK 4 with gtkmm-4.0 is a CMake option), delivered as a **plain executable, no Flatpak**, built on any distribution that packages the `gtkmm-3.0` development files (`pkg-config`, dynamic link to distribution libraries; gtkmm, glibmm, giomm, pangomm, atkmm and libsigc++ are added to the run-time dependencies, ADR 0006). The UI uses **only the GTK 3.10 API subset** (`Gtk::Window`, `Gtk::Box`, `Gtk::Entry`, `Gtk::Label`, `Gtk::InfoBar`, no later symbol outside a version macro), so the floor is **GTK 3.10** (about Ubuntu 14.04, 2014; **to verify** against the distribution archives) and no recent distribution is forced onto the Legacy UI. GTK 3 is still packaged by current distributions (Ubuntu 22.04 and later included). Systems without GTK 3.10 use the Legacy Xlib UI.
- S4 removed on 2026-11-14: Linux binaries are built on the target machine, so the glibc baseline of a build host does not matter (ADR 0007). To verify instead: a build and run on a very old distribution once one is reachable (T092).

## R8 — Web: WebAssembly mandatory, JavaScript fallback for Nov-2011 browsers

- **Findings**: Emscripten compiles the same C++11 to WebAssembly (`-sWASM=1`) or to JavaScript. Recent Emscripten (`-sWASM=0`) produces JavaScript through `wasm2js`, with `LEGACY_VM_SUPPORT` and `MIN_*_VERSION` settings, but its oldest supported browser baseline has been raised over time (IE is no longer a supported minimum in recent releases — to verify). **Emscripten 1.39.20 "fastcomp"** is the last release that emits true **asm.js** and its `-s LEGACY_VM_SUPPORT=1` adds the polyfills (typed arrays among them) that IE 9 needs; IE 9 has no typed arrays.
- **Decision**: WebAssembly path built with the **current Emscripten**; JavaScript path built **first** with the current Emscripten (`-sWASM=0 -sLEGACY_VM_SUPPORT=1`) and **kept only if** it works on the Nov-2011 browsers (IE 9, Firefox 8, Chrome 15, Safari 5.1, Opera 11.5, spec); otherwise built with **Emscripten 1.39.20 fastcomp** (same C++ source, different tool version; recorded as an ADR and in `docs/implementation/validation/fallbacks.json`).
- **Legacy UI**: plain HTML + **ES5** page: if `typeof WebAssembly === "object"` load `loomora.wasm`, otherwise load `loomora.js`; same exported C functions through `ccall`/`cwrap`.
- **Modern UI** (decided): **TypeScript** (ES modules, `WebAssembly.instantiateStreaming`), WebAssembly only, same `loomora.wasm`; about Chrome 61 / Firefox 60 / Safari 11 / Edge 16 (to verify). Other browsers get the Legacy page.
- **To verify (S5)**: IE 9 and Safari 5.1 run the JavaScript build (obtain period browsers from archive installers or a VM); the IE 9 typed-array emulation is fast enough for a one-line function.

## R9 — Build system, tests and host tools

- **Build system**: **CMake** (≥ 3.13 on the build host) with `CMakePresets.json` and one **toolchain file per cross target** under `cmake/toolchains/` (`mingw-w64-win2000-i686`, `mingw-w64-win-x86_64`, `osxcross-macos10.6`, `ios-xcode5`, `android-ndk-r16b-*`, `emscripten-wasm`, `emscripten-js`). Only the **backend and glue** use CMake; each UI also keeps its native project (Xcode project, `build.sh` for the Android Legacy APK, `.csproj`, Gradle for Compose, `ui/modern/linux-gtk3/CMakeLists.txt` for the GTK UI, which is CMake like the backend, `tsconfig.json`).
- **Backend tests**: **Catch2 v2.13.x** single header (C++11, BSL-1.0, git submodule in `third_party/catch2`, pinned to v2.13.10, tests only, never shipped) built on the host with GCC and Clang `-std=c++11`, driven by a **generated** `vectors.inc` (from `core/tests/vectors.json`). A tiny **`loomora_selftest`** executable (plain C++11, `printf` only, no STL) embeds the same vectors and is cross-compiled for every target, so the backend can be verified on an old OS before any UI exists.
- **Host tools** (`tools/`, **Python 3.8+**, standard library plus `jsonschema`): header and vector generation, floor-compiler checks, shared-code measurement, probe JSON validation, Markdown report, `run-vectors`. Python is a host-only tool, not shipped and not part of the backend; the repository already carries a Python virtual environment at its root. Alternative considered: C++ tools (rejected: slower to write, no benefit for report generation).
- **Shared-code share**: non-empty, non-comment source lines. Share of a platform variant = backend lines / (backend lines + native UI lines + glue lines). Generated code is not counted.

## R10 — Test environments

- **Decision**: virtual machines or emulators for Windows 2000/XP/Vista/7, Android 2.3 → 4.4, Linux (containers + VMs), old browsers (VMs, archive installers); **macOS 10.6 and iOS 5 only on Apple hardware** (license: Mac physical host, 10.6 Server VM); real devices when available. Images and tool versions are recorded (FR-016) and obtained legitimately.
- **Unknown to settle during implementation**: which old physical hardware is available; Windows 2000 and macOS 10.6 images that can be licensed.

## R11 — The greeting function and string ownership

- **Decision**: `loomora_greet(input, input_len, out, cap)` — caller-supplied UTF-8 input and output buffer; writes `"Bonjour "` + input; returns the byte length, or a negative error code when the input exceeds 4,096 bytes, `cap` is too small, or the arguments are invalid (null pointer; invalid UTF-8 is rejected too). The UI calls it on every change of its text field.
- **Rationale**: a caller-supplied buffer avoids any "free this string" function across seven calling conventions (C, Win32/C++, Objective-C, Swift, C#, JNI, Emscripten).
- **Tests**: `core/tests/vectors.json` lists the reference inputs (empty, ASCII, accented, CJK, right-to-left, emoji, 1,000 characters, 4,096-byte limit, 4,097-byte rejection, invalid UTF-8) with the exact expected output; they run on the host (Catch2), on each target (`loomora_selftest`) and through every UI in probe mode.

## R12 — Modern UI choices and their relation to the Legacy UI

- **Decision** (unchanged except Linux): WinUI 3 (Windows 10 1809+), SwiftUI (macOS 11+, iOS 15+), Jetpack Compose (Android API 24 planned), **GTK 3 with gtkmm in C++** (any distribution packaging GTK 3.10+ with its gtkmm), TypeScript (modern browsers). Floors are what the toolkit and its toolchain allow (spec Assumptions).
- **Rule**: below a Modern UI's floor, the platform's Legacy UI is the fallback; on systems above it both UIs can be installed side by side (spec edge case).
- **Interop per UI language**: Win32/Xlib/GTK with gtkmm (C/C++) and Objective-C → C header + static library (a C++ UI uses the C API too: the backend is C++11 behind it whatever the UI standard); Swift → same C header via a module map; C# → `loomora.dll` + `LibraryImport`; Java/Kotlin → JNI glue; JavaScript/TypeScript → Emscripten exports.
- **WinUI 3 interop**: C# calls the C API through `LibraryImport`; **C++/CLI rejected** (user decision: it adds a mixed-mode managed layer with no benefit over P/Invoke of a plain C API); C++/WinRT would remove .NET entirely and stays an option if .NET must be avoided.
- **Alternatives rejected**: Qt / Flutter / Compose Multiplatform (floors too high for the Legacy side, not native); MAUI/Avalonia (extra runtime, no floor advantage).

## Spikes (each one gates the tasks that depend on it)

| ID | Question | Verifies |
|----|----------|----------|
| S1 | Does a MinGW-w64 C++11 static library, linked into a MinGW-w64 Win32 executable, run on Windows 2000 SP4? | R6 |
| S2 | Does modern Clang + osxcross + the 10.6 SDK run on 10.6; does Xcode 5.1.1 build and deploy to iOS 5? | R4, R5 |
| S3 | Does NDK r16b + the scripted APK build run on an Android 2.3 emulator (all four ABIs)? | R3 |
| S5 | Does the JavaScript build run on IE 9 / Firefox 8 / Chrome 15 / Safari 5.1 / Opera 11.5; modern Emscripten or fastcomp 1.39.20? | R8 |
| S6 | Does the backend compile with strict C++11 under GCC and Clang and export only C symbols, with every exception caught at the C boundary? | R1, R2 |
| S7 | Can the 10.6 SDK and Xcode 5.1.1 be obtained legitimately? | R4, R5 |
| S8 | Can WinUI 3 call a C++11 `loomora.dll`, and does Compose accept `minSdk` 21? | R6, R3 |
| S9 | Do Windows 7 → 11 accept the Windows 2000-targeted Win32 executable (all versions)? | R6 |

## Sources

- [C++11 compiler support (cppreference)](https://en.cppreference.com/w/cpp/compiler_support/11) · [MSVC language standard switch `/std`](https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version)
- [Android NDK revision history (r16b, r17)](https://developer.android.com/ndk/downloads/revision_history) · [Android NDK r25 note on API levels](https://android-developers.googleblog.com/2022/10/android-ndk-r25-released.html)
- [osxcross](https://github.com/tpoechtrager/osxcross) · [MinGW-w64](https://www.mingw-w64.org/)
- [Emscripten settings reference](https://emscripten.org/docs/tools_reference/settings_reference.html) · [Emscripten compiler support / asm.js removal](https://emscripten.org/docs/compiling/WebAssembly.html)
- [Catch2](https://github.com/catchorg/Catch2) · [CMake presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)
- [Windows App SDK support and requirements](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/support) · [Jetpack Compose compiler setup](https://developer.android.com/develop/ui/compose/compiler) · [AndroidX versions (default minSdk)](https://developer.android.com/jetpack/androidx/versions)
- [GTK 3 reference](https://docs.gtk.org/gtk3/) · [Apple forum – deployment targets with Xcode 16](https://developer.apple.com/forums/thread/763782)
- [Visual Studio for Windows XP targets](https://learn.microsoft.com/is-is/cpp/build/configuring-programs-for-windows-xp?view=msvc-170)
