# Loomora

[![CI](https://github.com/bansan85/loomora/actions/workflows/ci.yml/badge.svg)](https://github.com/bansan85/loomora/actions/workflows/ci.yml) [![Windows](https://github.com/bansan85/loomora/actions/workflows/windows.yml/badge.svg)](https://github.com/bansan85/loomora/actions/workflows/windows.yml) [![WinUI 3](https://github.com/bansan85/loomora/actions/workflows/winui3.yml/badge.svg)](https://github.com/bansan85/loomora/actions/workflows/winui3.yml) [![Web](https://github.com/bansan85/loomora/actions/workflows/web.yml/badge.svg)](https://github.com/bansan85/loomora/actions/workflows/web.yml) [![Android](https://github.com/bansan85/loomora/actions/workflows/android.yml/badge.svg)](https://github.com/bansan85/loomora/actions/workflows/android.yml) [![manylinux1](https://github.com/bansan85/loomora/actions/workflows/manylinux1.yml/badge.svg)](https://github.com/bansan85/loomora/actions/workflows/manylinux1.yml) [![quality](https://github.com/bansan85/loomora/actions/workflows/quality.yml/badge.svg)](https://github.com/bansan85/loomora/actions/workflows/quality.yml)

Loomora is universal String Art software. String Art reproduces an image by stretching a continuous string (or several strings of different colors) between nails driven into a physical support: a circle, a rectangle, a free shape, possibly with nails inside.

## What Loomora will do

From a digital image (photo, logo, illustration, silhouette), Loomora computes an optimized path of string segments, then provides:

- a nail placement template (1:1 PDF, machine export);
- a step-by-step weaving guide, manual, automatic or voice-driven, with error recovery;
- exports usable by digital machines (CNC, plotters, robots).

It is meant for creators, craftspeople, makers and digital artists, for FabLabs and users of plotters, CNC machines and robots, and for the general public as a computer-assisted creative hobby.

## One application, a native interface per platform

The graphical interface will be native on each platform (Windows, macOS, Linux, Android, iOS, and the web), with a Legacy interface for very old systems and a Modern one for current systems, and with themes. Every interface shows the same results because none of them computes anything: they all rely on the same engine, a C++11 backend behind a C API.

The full list of features, platforms and constraints is the [requirements document](docs/requirements-specification.md).

## Where the project stands

The graphical application does not exist yet. The project is in a validation phase: a minimal probe (a text field pre-filled "Coucou" and a label showing "Bonjour " followed by the field) checks, on each platform and for each interface variant, that the chosen technical approach holds, including on systems as old as Windows 2000. Eight probe interfaces exist: Windows Legacy (Win32) and Modern (WinUI 3), Linux Legacy (Xlib) and Modern (gtkmm), Android Legacy (Java views, API 9 to 23) and Modern (Kotlin and Compose, API 24 and later), Web Legacy (a JavaScript page) and Modern (TypeScript on WebAssembly). The macOS and iOS ones are written (Legacy AppKit and UIKit, Modern SwiftUI) and were run on the systems of the table below, except where it says otherwise.

Floors validated so far, under the PoC rule (the Legacy UI is run on the lowest system that can be tested, the Modern UI on one current system and one 64-bit CPU). Every verdict is "OK with restrictions": read the restrictions in the [validation report](docs/implementation/validation/report.md) before relying on a floor.

| Family | Legacy UI, lowest system run | Modern UI, system run |
|---|---|---|
| Windows | Windows 2000 (build 2195, in a virtual machine) | Windows 11 x64 (WinUI 3; the ARM64 build is never run) |
| Linux | CentOS 5.11 userland (glibc 2.5, in Docker), built on the machine that runs it | Ubuntu 24.04, GTK 3.24 (the GTK 3.10 floor is only compiled) |
| Android | built for API 9, run on Android 4.1.2 (API 16, emulator) | Android 7.0 (API 24, emulator) |
| Web | Firefox 10 on Windows 2000 (JavaScript path) | Chrome 157 (WebAssembly path) |
| macOS | Mac OS X 10.9 Mavericks (minimum, tested) | macOS 11 Big Sur, 11.7 (minimum and newest tested; Universal 2 build, Intel run) |
| iOS | not run (the UIKit build is written, not run) | iOS 14.4 (minimum; the iOS 14.4 simulator run, device build compiled but not run) |

## Repository layout

| Path | Content |
|---|---|
| `core/` | the backend: C++11 library `libloomora`, its C API (`core/include/loomora.h`, and `core/include/loomora_project.h` for the `.loomora` project file) and its tests (Catch2, self-test, reference vectors in `core/tests/vectors.json`); `core/src/format/` is the project file format module (ZIP container, JSON, validation); `core/fuzz/` holds the fuzz targets (option `LOOMORA_BUILD_FUZZ`) |
| `cli/loomora/` | the host command `loomora validate` (Windows, Linux and macOS hosts) |
| `ui/legacy/<platform>/`, `ui/modern/<platform>/` | one native interface per platform and variant (Win32, Xlib, Android views, web page, WinUI 3, gtkmm, Compose, TypeScript; AppKit, UIKit and SwiftUI folders are empty until Apple hardware is available), with no logic |
| `bridge/` | glue with no logic: `jni/` (Android) and `wasm/` (Emscripten) |
| `cmake/` | toolchain files: MinGW-w64, Linux i686, Android NDK r15c, Emscripten |
| `tools/` | Python host tools: `verify.py`, `build.py`, `run_vectors.py`, `check_std.py`, `measure_sharing.py`, `prefix_check.py`, `report.py`, `validate_records.py`, and `trials/` (the drivers that produced the evidence) |
| `specs/001-loomora-cross-platform/` | specification, plan, research, tasks, [quickstart](specs/001-loomora-cross-platform/quickstart.md), contracts |
| `docs/implementation/specs/002-loomora-file-format/` | the `.loomora` project file format: specification, plan, research, data model, tasks, [quickstart](docs/implementation/specs/002-loomora-file-format/quickstart.md), [contracts](docs/implementation/specs/002-loomora-file-format/contracts/) |
| `docs/` | [requirements](docs/requirements-specification.md) and the [build guide](docs/building/README.md) |
| `docs/implementation/` | git submodule (`loomora-docs`): the architecture decision records and `validation/` (platforms, verdicts, procedures, evidence, report) |
| `third_party/catch2/` | git submodule: Catch2, used by the tests only |
| `third_party/zlib/` | git submodule: zlib at tag v1.3.1, pinned for deterministic deflate output; only the sources the format needs are built, through `cmake/loomora_zlib.cmake` (zlib license) |
| `third_party/rapidjson/` | git submodule: RapidJSON at commit `297637bc0a0f00dc504dbc1a47ebdaa577f8ae19`, header-only JSON reader and writer (MIT license) |
| `.github/workflows/` | the seven CI workflows |

Building the product never needs `docs/implementation/`, but it needs the three third-party submodules (Catch2, zlib, RapidJSON: they are submodules, not copies in the repository). Clone with all the submodules from GitHub:

```
git clone --recurse-submodules https://github.com/bansan85/loomora.git
```

For a clone made from a local sibling checkout of `loomora-docs` (the submodule URL is relative), git refuses the file transport unless told otherwise:

```
git -c protocol.file.allow=always submodule update --init docs/implementation
```

For a clone made without `--recurse-submodules`, fetch the third-party libraries (tests and the project file format) with:

```
git submodule update --init third_party/catch2 third_party/zlib third_party/rapidjson
```

## Build and run

`python tools/verify.py quick` (Windows host or WSL) builds what exists on the machine, runs ctest, the 15 reference vectors through each UI built there and the record checks. The quickstart has seven scenarios; the [build guide](docs/building/README.md) has the exact commands of each, the prerequisites and the environment variables.

| Scenario | What | Entry point |
|---|---|---|
| 0 | spikes | status in `docs/implementation/validation/spikes.json`, procedures in `docs/implementation/validation/procedures/` |
| 1 | backend and vectors | `python3 tools/check_std.py`, then CMake and ctest (or `verify.py quick`) |
| 2 | Legacy UI of a platform | `python3 tools/build.py <windows\|linux\|android> legacy`, then `python tools/run_vectors.py <platform-id> legacy` |
| 3 | Modern UI of a platform | `python3 tools/build.py <windows\|linux\|android> modern`, then `python tools/run_vectors.py <platform-id> modern` |
| 4 | Web, two paths | `python3 tools/build.py web legacy` and `web modern`, vectors under node or in Chrome |
| 5 | text change, code sharing | `python tools/prefix_check.py` |
| 6 | measurement and report | `python tools/measure_sharing.py`, `python tools/report.py [--check]` |
| 7 | replayability | the procedures in `docs/implementation/validation/procedures/` (Windows, Linux, Android, Web) |

CMake options of the backend: `LOOMORA_BUILD_TESTS` (self-test and Catch2 tests, ON), `LOOMORA_BUILD_SHARED` (also build the shared library, OFF) and `LOOMORA_BUILD_FUZZ` (OFF): builds the fuzz target `loomora_fuzz_project` of `core/fuzz/`, which feeds mutated `.loomora` files to the reader. It needs Clang with libFuzzer (`-DCMAKE_CXX_COMPILER=clang++`); it is a build of its own (for example `cmake -S . -B ~/loomora-build/fuzz -DLOOMORA_BUILD_FUZZ=ON`), never part of `verify.py quick`.

Check a project file with the host command (it opens the file read-only, never changes it):

```
loomora validate [--json] [--quiet] <file> [<file> ...]
```

It prints one line per issue and one summary line per file; the exit status is 0 when every file is valid (warnings allowed), 1 when a file has an error, 2 for a usage error or an unreadable file. See [contracts/cli-validate.md](docs/implementation/specs/002-loomora-file-format/contracts/cli-validate.md); the format itself is specified in [contracts/project-format.md](docs/implementation/specs/002-loomora-file-format/contracts/project-format.md).

Where each one runs: Windows Legacy cross-built in WSL with MinGW-w64; Windows Modern (WinUI 3), Android and the Chrome run on the Windows host; Linux and Web in WSL or on Linux (Linux binaries are always built on the machine that runs them). macOS and iOS: not available, needs Apple hardware (pending). The Windows 2000 and Firefox 10 runs need an already running virtual machine; see "Local runs (not CI)" in the build guide. Full details: [quickstart.md](specs/001-loomora-cross-platform/quickstart.md) and [docs/building/README.md](docs/building/README.md).

## Continuous integration

Seven GitHub Actions workflows (the badges above) build and test on hosted runners what they can run: Linux, Windows (MinGW-w64 cross build and clang), WinUI 3, Web, Android and manylinux1. What no hosted runner can run (Windows 2000 and Firefox 10 virtual machines, Apple systems, old Linux distributions in Docker) is a local run. See [Continuous integration](docs/building/README.md#continuous-integration) and [Local runs (not CI)](docs/building/README.md#local-runs-not-ci) in the build guide.

## Contributing and branch workflow

`master` contains only merge commits (`git merge --no-ff`); nothing is committed directly on it. Every change is committed on its own branch and merged afterwards, once the owner has validated it. Commit messages are in English, with a Conventional Commits prefix (`feat:`, `fix:`, `docs:`, ...), a title of at most 50 characters and the imperative mood. The documentation repository `loomora-docs` follows the same rules. All the rules are in [CLAUDE.md](CLAUDE.md).

## License and status

No license has been chosen yet (license and business model are still open in section 16.2 of the requirements document), so the repository carries no license file. The project is a validation phase, not a released product.

## Where to go next

| You want | Read |
|---|---|
| to build and run the probe | [docs/building/README.md](docs/building/README.md) |
| to understand the engine, the code layout and the technical decisions | [docs/implementation/adr/README.md](docs/implementation/adr/README.md) (git submodule, see above to fetch it) |
| the requirements | [docs/requirements-specification.md](docs/requirements-specification.md) |
| the plan and the task list of the validation phase | [specs/001-loomora-cross-platform/](specs/001-loomora-cross-platform/) |
| the `.loomora` project file format | [docs/implementation/specs/002-loomora-file-format/](docs/implementation/specs/002-loomora-file-format/) |
| the results of the validation | [docs/implementation/validation/report.md](docs/implementation/validation/report.md) |
| the rules to follow when contributing | [CLAUDE.md](CLAUDE.md) |
