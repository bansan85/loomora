# Implementation Plan: Loomora Across 15 Years of Platforms

**Branch**: `001-loomora-cross-platform` | **Date**: 2026-11-12 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/001-loomora-cross-platform/spec.md`

## Summary

Build a minimal probe (Loomora) that runs on systems from the decided floors (Windows 2000, macOS 10.6, Linux kernel 2.6 / glibc 2.12, Android 2.3, iOS 5, Web on Nov-2011 browsers with or without WebAssembly) up to today, and produce the report that validates or invalidates the technical choices of the [requirements document](../../docs/requirements-specification.md) (phase 0, §15.1).

Approach ([research.md](research.md)): **a single C++11 backend** built as a **static library** (full C++11 and its standard library allowed inside; the C++ runtime linked statically where needed) behind a handwritten **C API** ([c-api.md](contracts/c-api.md)) that no exception crosses; **two thin native UIs per platform** ([ui-contract.md](contracts/ui-contract.md)): a Legacy UI for very old systems (features possibly disabled) and a Modern UI as modern and native as possible; **one cross-compiler per floor** building the same source (MinGW-w64, osxcross + Clang, Xcode 5.1.1, NDK r16b, manylinux2010, Emscripten); recent compiler or system features stay behind declared fallbacks; a **probe mode** producing one JSON record per trial; Python host tools for checking, measurement and reporting. Nine **spikes** (research S1–S9) settle the riskiest toolchain hypotheses first.

## Technical Context

**Language/Version**: backend **C++11** (ISO/IEC 14882:2011), checked with GCC and Clang `-std=c++11 -pedantic-errors`; compilers per floor: MinGW-w64 GCC (Windows 2000 → 11), Clang 16+ with osxcross and the 10.6 SDK (macOS 10.6), Apple Clang 5.1 in Xcode 5.1.1 (iOS 5), Clang 5 in NDK r16b (Android 2.3), GCC 8 in manylinux2010 (Linux glibc 2.12), Emscripten (current, plus 1.39.20 fastcomp if needed). Legacy UIs: C++ (plain Win32 API, built with MinGW-w64), Objective-C (macOS, iOS), Java (Android), C (Xlib), ES5 JavaScript (Web). Modern UIs: C#/WinUI 3 (.NET 7+), Swift/SwiftUI, Kotlin/Jetpack Compose, C/GTK 3, TypeScript. Host tools: Python 3.8+.

**Primary Dependencies**: backend with no dependency (not even the C++ standard library at run time); CMake ≥ 3.13 and one toolchain file per cross target; Catch2 v2.13.10 (tests only, git submodule); Python 3 + `jsonschema` (host tools); Emscripten (Web); JNI (Android); Modern UIs: Windows App SDK, SwiftUI, Jetpack Compose (AGP 9, `compileSdk` 37), GTK 3 (distribution packages)

**Storage**: N/A (versioned validation files; the application stores nothing)

**Testing**: Catch2 tests on the host with `core/tests/vectors.json` (GCC and Clang `-std=c++11`); `loomora_selftest` cross-compiled and run on every target; `--probe` mode on every platform and variant; probe JSON validated against the schema; replay by a third person (SC-008)

**Target Platform**: Legacy UIs — Windows 2000 SP4 → 11 (x86, x64; ARM64 is Modern-only); macOS 10.6 → current Intel (i386, x86_64); Linux kernel 2.6 / glibc 2.12 → current (i686, x86_64); Android 2.3 → current (armeabi, armeabi-v7a, x86, arm64-v8a); iOS 5 → current (armv7, armv7s, arm64); browsers of Nov 2011 with WebAssembly or JavaScript fallback. Modern UIs — Windows 10 1809+ (x64, ARM64); macOS 11+ (Universal 2); iOS 15+; Android 7.0+ (API 24); Linux distributions packaging GTK 3.10+ (plain executable, no Flatpak); browsers with ES modules + WebAssembly

**Project Type**: native multi-platform (shared C++11 backend + two thin native UIs per platform) with validation tooling

**Performance Goals**: first frame in < 3 s (SC-001), measured by the probe; no computation goal

**Constraints**: backend in idiomatic C++11 (STL, allocation, exceptions and RTTI allowed) but its C binding must stay clean: no C++ type in `loomora.h`, no exception crossing the boundary, memory exchanged only in caller-supplied buffers, C++ runtime linked statically where the platform lacks one; no network connection, no administrator rights, no GPU in Legacy UIs; 32-bit compatible; offline installation possible; statically linked runtimes when feasible; header `loomora.h` consumable by the oldest UI compilers, including old Xcode and GCC (C89-style typedefs when `stdint.h` is absent)

**Scale/Scope**: 6 families, ~25–35 platforms attempted, 2 variants each, 1 library (< 300 lines) + 2 glue files, 12 native UIs (~100–200 lines each); the main volume is validation documentation

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

`.specify/memory/constitution.md` is still the **unfilled template**: no principle is defined, so there is **no gate to evaluate** (before or after design). Recommendation: run `/speckit-constitution` before the product phases. The plan respects the decisions of the spec (single C++11 backend, strict C++11 source, native UIs with no logic, two UI variants per platform) and the principles of the requirements document (thin UI, explicit degradation).

**Post-design re-check**: unchanged (no principle to check).

## Project Structure

### Documentation (this feature)

```text
specs/001-loomora-cross-platform/
├── plan.md              # This file (/speckit-plan command output)
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output
│   ├── c-api.md
│   ├── ui-contract.md
│   └── probe-result.schema.json
└── tasks.md             # Phase 2 output (/speckit-tasks command - NOT created by /speckit-plan)
```

### Source Code (repository root)

```text
CMakeLists.txt  CMakePresets.json
cmake/toolchains/             # one file per cross target (mingw-w64, osxcross, ios-xcode5, android-ndk-r16b, linux-glibc2.12, emscripten)
core/                         # the backend: namespace loomora, strict C++11, C API binding
├── include/loomora.h         # handwritten public C header (extern "C"), the only API
├── src/                      # greet.cpp  eol.cpp  probe.cpp  version.cpp  internal/config.hpp
└── tests/                    # vectors.json, Catch2 tests, selftest.cpp (cross-compiled)
bridge/                       # glue with no logic
├── jni/loomora_jni.cpp       # Android
└── wasm/exports.txt          # Emscripten exported functions
third_party/catch2/           # git submodule, Catch2 v2.13.10 (tests only)
ui/                           # two thin native UIs per platform
├── legacy/
│   ├── windows-win32/        # C++ (plain Win32 API, MinGW-w64; no MFC)
│   ├── macos-appkit/         # Objective-C
│   ├── ios-uikit/            # Objective-C
│   ├── android-views/        # Java (scripted APK build, no Gradle)
│   ├── linux-xlib/           # C
│   └── web-es5/              # HTML + ES5 JavaScript (wasm / js loader)
└── modern/
    ├── windows-winui3/       # C#
    ├── macos-swiftui/        # Swift
    ├── ios-swiftui/          # Swift
    ├── android-compose/      # Kotlin
    ├── linux-gtk3/           # C (GTK 3), plain executable (no Flatpak)
    └── web-ts/               # TypeScript (shares loomora.wasm with web-es5)
tools/                        # Python host tools: check-std, gen-vectors, build, run-vectors, measure-sharing, check-probe, report
docs/requirements-specification.md  # the requirements document (English)
docs/implementation/          # git submodule ../loomora-docs: implementation documentation
├── validation/               # validation data
│   ├── platforms/  verdicts/  deliverables/  evidence/<change>/<platform-id>/  procedures/
│   ├── fallbacks.json  hypotheses.json  sources.json
│   └── report.md
└── adr/                      # decisions (FR-017)
tools/trials/<platform-id>/   # drivers that produced the evidence (tooling, hashed in the verdicts)
```

**Structure Decision**: single repository organized by role. The backend is one C++11 library; each platform has a Legacy and a Modern UI folder under `ui/` with no logic; any difference between compilers is a feature macro in `core/src/internal/config.hpp`, never a second source file (FR-019, FR-021).

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| ~7 cross toolchains (MinGW-w64, osxcross Clang, Xcode 5.1.1, NDK r16b, manylinux2010, Emscripten ×2) | Each decided floor predates what current compilers target; no single toolchain reaches Windows 2000 and iOS 5 | A single modern toolchain would raise the floors, outside the spec's decisions |
| Static C++ runtime and a clean C binding | Old systems have a different or missing C++ runtime; the UIs (C, Objective-C, Swift, C#, Java, JS) only see C symbols | Dynamic runtimes would have to be shipped or found on each target |
| Feature macros for fallbacks | "Recent features if possible, fallback otherwise" (FR-021) | Code limited to C++11 features only would deprive modern platforms of recent compiler features |
| Two native UIs per platform (12 UIs in ~9 languages) | User decision: Legacy UI for very old systems + Modern UI as native as possible | One UI per platform cannot be both compatible with 2000-era systems and modern |
| Windows Legacy UI on the raw Win32 API instead of MFC | MFC needs Visual Studio 2005 for Windows 2000 (no C++11, hard to obtain); user decision 2026-11-12 | MFC would need a second Windows compiler and break the single-toolchain build (R6) |
