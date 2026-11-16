# Build and run

How to build, verify and run the Loomora probe. It covers what works today; the macOS and iOS builds are not written (Apple hardware is pending).

Commands start with `PS>` on the Windows host and `wsl$` inside WSL.

## Fetch the submodules

Building the product never needs `docs/implementation/`. To work on the implementation or to run `tools/validate_records.py`, fetch both submodules:

```
git submodule update --init
```

## Prerequisites

- Windows host: Python 3.8+ with `pip install -r tools/requirements.txt`, CMake 3.13+, Ninja, clang, and WSL2 with Ubuntu-24.04.
- Windows Modern UI (WinUI 3): Visual Studio 2026 with the C++ desktop workload (and, for ARM64, "MSVC Build Tools for ARM64/ARM64EC (Latest)"), CMake 4.2 or later (generator `Visual Studio 18 2026`), the .NET SDK 10.0.404 pinned by `ui/modern/windows-winui3/global.json`, and NuGet access (Windows App SDK 2.6.5, the .NET 8 and 10 targeting packs). To run the result the machine needs the .NET Desktop Runtime (8 or 10) and the Windows App Runtime 2.6 of its architecture. No Visual Studio GUI is used.
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

The executables are `out\windows-legacy\x64\loomora.exe` (XP x64 to 11) and `out\windows-legacy\x86\loomora.exe` (Windows 2000 to 11). `build.py` checks every imported function against `tools/win2000_imports.json`. `loomora.exe --probe` shows the window for 2 seconds and writes the probe line to `loomora-probe.json` next to the executable; without `--probe` it opens the window. The Windows 2000 trials run in a VM and are not described here (see `tools/trials/windows-2000-x86/` and the procedure `docs/implementation/validation/procedures/windows.md`).

Linux (x86_64, built on the machine that runs it; `--arch i686` builds the 32-bit executable too, which needs the `:i386` development packages; only the build was run for it):

```
wsl$ python3 tools/build.py linux legacy --arch x86_64
wsl$ python3 tools/run_vectors.py linux-ubuntu-24.04-x86_64 legacy
wsl$ out/linux-legacy/x86_64/loomora-xlib --probe
```

The platform id is `linux-<ID>-<VERSION_ID>-<machine>` of `/etc/os-release`. `--probe` needs an X display (WSLg provides `:0`) and prints one JSON line on standard output. `run_vectors.py` types into the display: on WSLg's shared `:0` another program using the keyboard can corrupt a vector (seen once: extra characters appended to the input), so run the vectors on a private display (`Xvfb :87 -screen 0 1280x800x24 &`, then `export DISPLAY=:87`), as `verify.py` does.

Android (API 9 to 23; built on the Windows host, not in WSL; the emulator can run only from API 16):

```
PS> $env:LOOMORA_ANDROID_NDK = "E:\Program\Android\android-ndk-r15c"   # NDK r15c only: r16b has no android-9
PS> $env:LOOMORA_ANDROID_SDK = "E:\Program\Android\Sdk"                # platforms/android-9, build-tools with d8
PS> python tools/build.py android legacy
PS> emulator -avd API16 -no-window -no-audio -no-snapshot -no-boot-anim   # start it yourself, kill it with: adb emu kill
PS> python tools/run_vectors.py android-4.1.2-x86 legacy
```

`build.py` builds the four ABIs (`armeabi`, `armeabi-v7a`, `x86`: API 9 sysroot; `arm64-v8a`: API 21) with `cmake/toolchains/android-ndk-r15c.cmake` and packages one APK, `out\android-legacy\loomora.apk` (`ui/legacy/android-views/build_apk.py`); the self-tests are in `out\android-legacy\selftest\<abi>\`. Only the x86 build can be run (the platform id is `android-<release>-<abi>`, read from the device). See ADR 0016.

**Scenario 3: one platform, Modern UI**

Linux (built on the machine that runs it):

```
wsl$ python3 tools/build.py linux modern
wsl$ python3 tools/run_vectors.py linux-ubuntu-24.04-x86_64 modern
wsl$ out/linux-modern/loomora-gtk3 --probe
```

Windows 11 (WinUI 3, C#; built on the Windows host, not in WSL): `loomora.dll` by MSVC (`cmake --preset host-msvc`, and `host-msvc-arm64` when the ARM64 tools are installed), then `dotnet publish` of `ui/modern/windows-winui3` for `net8.0-windows10.0.19041.0` (the Windows 10 1809 fallback) and `net10.0-windows10.0.19041.0`, x64 and ARM64, each zipped:

```
PS> python tools/build.py windows modern
PS> python tools/run_vectors.py windows-11-x64 modern
PS> build\windows-modern\net10-x64\Loomora.WinUI.exe --probe
```

The zips are `out\windows-modern\loomora-winui-<net8|net10>-<x64|arm64>.zip`; the published folders stay in `build\windows-modern\`. The ARM64 build is built, never run on an x64 machine. `run_vectors.py ... modern --exe <folder>\Loomora.WinUI.exe` runs another published folder (for example the net8 one).

Android Modern (Kotlin and Compose, minSdk 24; Windows host; needs `LOOMORA_ANDROID_SDK` with NDK 30, `platforms;android-37.0` and build-tools 36.0.0, a JDK 17 or later, and the Internet for Gradle): `PS> python tools/build.py android modern` writes `out/android-modern/loomora.apk`. The vectors run on an API 24 x86_64 emulator (`python tools/run_vectors.py android-7.0-x86_64 modern`, as `android.yml` does); none is set up on the build machine.

`build.py` writes the deliverable record of each build into `docs/implementation/validation/deliverables/` (only when the submodule is checked out); do not commit those records unless the build is the one a verdict refers to.

**Scenario 4: Web, two paths** (WSL; the current Emscripten in `~/emsdk` for the WebAssembly path and Emscripten 1.39.20 fastcomp in `~/emsdk-fastcomp` for the JavaScript path, or `LOOMORA_EMSDK` and `LOOMORA_EMSDK_FASTCOMP`; node and npm):

```
wsl$ python3 tools/build.py web legacy
wsl$ python3 tools/build.py web modern
wsl$ python3 tools/run_vectors.py web-node-js legacy --exe ~/loomora-build/web-legacy/dist
wsl$ python3 tools/run_vectors.py web-node-wasm legacy --exe ~/loomora-build/web-legacy/dist
wsl$ python3 tools/run_vectors.py web-node-wasm modern --exe ~/loomora-build/web-modern/dist
PS> $env:LOOMORA_WEB_DIST = "\\wsl.localhost\Ubuntu-24.04\home\<user>\loomora-build\web-modern\dist"
PS> python tools/run_vectors.py web-chrome-157-windows-11-x64 modern --timeout 60
```

The pages are the `dist/` folders under `$LOOMORA_BUILD_DIR` (default `$HOME/loomora-build`), never in the repository. The node runs execute the page script in a DOM stub; the Chrome run drives the Modern page in the headless Chrome of the Windows host (a temporary profile of its own). The Legacy page in Firefox 10 is a local run (see "Local runs (not CI)").

**Scenario 5: text change.** `python tools/prefix_check.py` changes the prefix in `core/src/greet.cpp` of a copy of the sources, rebuilds from scratch the Windows host, Windows x64 and x86, Linux Legacy x86_64 and i686 and Linux Modern UIs, runs the vectors and checks that only that file differs (`--list` names the targets, `--vm` adds the Windows 2000 VM). It does not build the Web, Android and WinUI 3 UIs yet: change the prefix by hand for those.

**Scenarios 6 and 7**: `python tools/measure_sharing.py [--write]`, `python tools/report.py` and `python tools/report.py --check` (scenario 6); the procedures in `docs/implementation/validation/procedures/` for Windows, Linux, Android and Web (scenario 7). Scenario 0 (spikes) is manual: the status of each spike is in `docs/implementation/validation/spikes.json`.

**Not available yet**: everything for macOS and iOS (needs Apple hardware, pending). The commands of the other scenarios were replayed on 2026-11-15 (T118), except the runs in the Windows 2000 and Firefox 10 virtual machines.

## Continuous integration

Seven workflows in `.github/workflows/` build and test on GitHub what a hosted runner can run (decisions and measured facts: ADR 0018, `docs/implementation/adr/0018-ci-github-hosted-runners.md`). Every one of them runs on each push to any branch and on each pull request from a fork (a pull request from this repository is already covered by the push of its branch). Rules common to all: actions pinned by full commit SHA, `permissions: contents: read`, `persist-credentials: false`, outdated runs cancelled, a timeout on every job, a `changes` job that skips what a change cannot affect, and a final `gate` job.

| Workflow file | Trigger | Runner | What it checks | Artifacts (14 days) | Local equivalent |
|---|---|---|---|---|---|
| `ci.yml` (CI) | push, fork PR, Mondays 03:17 UTC | `ubuntu-22.04`, `ubuntu-24.04`, `ubuntu-24.04-arm`; variants on `ubuntu-24.04` | `verify.py quick`: backend build, ctest (selftest and Catch2), 15 vectors through the Xlib and gtkmm UIs on a private Xvfb, `check_std`, `check_ui_logic`, `validate_records`, the self-tests; the variants (GTK 3.10 floor, GTK 4, i686 Xlib, Xlib core font) when their files change; a documentation-only change runs the records only | `verify-logs-<runner>`, `verify-logs-variants`: build, vector and probe-line logs of `verify.py` (no executable: a Linux binary is built on the machine that runs it) | `python3 tools/verify.py quick` (`--variants` for the variants) |
| `windows.yml` (Windows) | push, fork PR, Mondays 03:47 UTC | `ubuntu-24.04` (MinGW-w64 cross build), `windows-2025` and `windows-11-arm` (runs), `windows-2025` (host build) | cross build of the Windows Legacy UI with the import allowlist check; 15 vectors through the x64 and the x86 executable (x86 under WOW64 or emulation); clang + Ninja host build with `verify.py quick --only build,ctest,vectors,ui-logic,records,diff` | `windows-legacy-exe` (`x64` and `x86` `loomora.exe`), `vectors-logs-<runner>` (vector log and probe lines), `verify-logs-windows-host` | `python3 tools/build.py windows legacy` in WSL, then `python tools/run_vectors.py windows-11-x64 legacy [--arch x86]`; `python tools/verify.py quick` |
| `winui3.yml` (WinUI 3) | push, fork PR, Mondays 04:17 UTC | `windows-2025-vs2026` (build), `windows-2025` (x64 runs), `windows-11-arm` (ARM64 runs) | `loomora.dll` for x64 and ARM64 and the four published folders (net8 and net10); Windows App Runtime 2.6.5 installed from the NuGet package; 15 vectors per folder | `winui3-zips` (the four zips), `winui3-vectors-logs-<arch>` | `python tools/build.py windows modern`, then `python tools/run_vectors.py windows-11-x64 modern` |
| `web.yml` (Web) | push, fork PR, Mondays 03:47 UTC | `ubuntu-24.04` (builds, node runs), `windows-2025` (Chrome) | Emscripten builds of the Legacy (JavaScript and WebAssembly paths) and Modern pages; 15 vectors under node for `web-node-js legacy`, `web-node-wasm legacy` and `web-node-wasm modern`; the Modern page in the headless Chrome of the image | `web-legacy-dist`, `web-modern-dist` (the pages), `web-logs`, `web-chrome-logs` | `python3 tools/build.py web legacy` and `web modern`, then `python3 tools/run_vectors.py web-node-wasm modern --exe ~/loomora-build/web-modern/dist` (and the other ids) |
| `android.yml` (Android) | push, fork PR, Mondays 04:17 UTC | `ubuntu-24.04` (builds; emulator jobs with KVM) | Legacy build with the NDK r15c (download checked by size, SHA-1 and SHA-256), Modern build with the NDK r30; 15 vectors for the Legacy UI on an API 16 x86 emulator and for the Modern UI on an API 24 x86_64 emulator | `android-legacy-apk`, `android-modern-apk`, `android-<ui>-build-log`, `android-<ui>-emulator-logs` | `python tools/build.py android legacy` and `android modern` on the Windows host, an emulator started by hand, `python tools/run_vectors.py android-4.1.2-x86 legacy` |
| `manylinux1.yml` (manylinux1) | push, fork PR, manual | `ubuntu-24.04` with Docker | the Linux Legacy UI built and run in the CentOS 5.11 image (glibc 2.5): kernel and vsyscall probe, 15 vectors, window run on a private Xvfb | `manylinux1-logs` (probes, image and UI builds, vectors, window run) | the commands of procedure L.11 (`docs/implementation/validation/procedures/linux.md`) |
| `quality.yml` (quality) | push, fork PR | `ubuntu-24.04` | `validate_records.py`, `report.py --check`, `diff_verdicts.py` against the target branch, and the self-tests of the tools | `quality-logs` | `python3 tools/validate_records.py`, `python3 tools/report.py --check`, `python3 tools/diff_verdicts.py <revision of docs/implementation>` |

The `quality` workflow builds nothing: the executables, probe lines and vector logs are the artifacts of the other workflows, listed above. Artifacts expire after 14 days and are not evidence: a log is stored in the records only when it proves something new (ADR 0014). Download one with `gh run download <run id> -n <artifact name> -R bansan85/loomora`.

### What `quality.yml` covers

- `validate_records.py` and `report.py --check` need the docs submodule and fail when a record, an evidence hash or `report.md` is out of date.
- `diff_verdicts.py` compares the verdicts with those of the target branch: the base of a pull request or, on a push, the merge base with `master` (the previous commit on `master` itself). The baseline is the docs revision that the target commit points to (`git ls-tree <target> docs/implementation`); the job fetches it from `loomora-docs` when the shallow submodule checkout lacks it. The comparison is a report printed in the job summary: a changed status does not fail the job (it is a decision of the owner), an unreadable baseline does.
- The self-tests of `validate_records`, `report`, `diff_verdicts`, `check_probe`, `check_imports` and `run_vectors` run too; none needs a submodule.
- Replaying the procedures (FR-016, FR-018): the jobs of the other workflows replay the steps of the procedures that a hosted runner can run (reference vectors through each UI, the Linux window run, the L.11 steps). Everything that needs a virtual machine or Apple hardware is a local run (next section). No job replays a procedure that is not listed here.

### The checks to read: the `gate` jobs

Each workflow ends with a job named `gate`, `gate (windows)`, `gate (winui3)`, `gate (manylinux1)` or `gate (quality)`; it always runs and passes when no job it depends on failed (a skipped job is fine). Read these, not the build jobs. Three workflows (`ci.yml`, `web.yml`, `android.yml`) name their job `gate` without a suffix, so three checks have the same name. In `android.yml` the emulator jobs are outside the gate (a flaky boot must not block a merge) but are never `continue-on-error`: read them in the run.

### Read a run

```
PS> & "C:\Program Files\GitHub CLI\gh.exe" run list -R bansan85/loomora --branch <branch> -L 10
PS> & "C:\Program Files\GitHub CLI\gh.exe" run view <run id> -R bansan85/loomora
PS> & "C:\Program Files\GitHub CLI\gh.exe" run view <run id> -R bansan85/loomora --log-failed
```

`run view` lists the jobs and the artifacts; `--log-failed` prints the logs of the failed steps only; `--job <job id>` selects one job. The job summary of `quality.yml` shows one line per check and the verdict differences.

### Run the same checks locally

`python tools/verify.py quick` (Windows host or WSL) is the verification of any branch and what `ci.yml` runs on each Linux image; the table above gives the command of each other workflow. `python tools/verify.py full` is never in CI (graphical changes only, see `CLAUDE.md`).

### How the docs submodule is checked out

The workflows that need the records (`ci.yml`, the host build of `windows.yml`, `manylinux1.yml`, `quality.yml`) use `actions/checkout` with `submodules: recursive`. The submodule URL `../loomora-docs` is relative, so git resolves it against the URL of the repository being checked out and gets `https://github.com/bansan85/loomora-docs`, a public repository: no token is needed. The commit that the submodule pointer names must exist there: push `loomora-docs` first, then `loomora`. A job that only builds (the cross build of `windows.yml`, `winui3.yml`, `android.yml`, the builds of `web.yml`) uses `submodules: false`: building never needs the docs.

### No secret, no self-hosted runner

No workflow uses a secret, and none uses a self-hosted runner. Both repositories are public: a self-hosted runner would run, on the owner's machine, the code of a pull request from any fork (GitHub's own guidance is that self-hosted runners should almost never be used for public repositories). Whatever a hosted runner cannot run is a local run, below.

## Local runs (not CI)

What no GitHub-hosted runner can run is run by the owner on the owner's machine and recorded in the validation records (verdicts, procedures, evidence under ADR 0014). Nothing here is a CI job, and no self-hosted runner is ever added to run it (see above).

| Local run | Command or launcher | Needs | Records |
|---|---|---|---|
| Windows 2000 (and, when a VM exists, XP, Vista, 7, 8.1, 10, and Windows 10 1809 for the .NET 8 build of WinUI 3) | `python tools/run_vectors.py windows-2000-x86 legacy` (`--arch x86`), helpers in `tools/guest/`, drivers in `tools/trials/windows-2000-x86/` | VirtualBox 7.2 with `VBoxManage` (or `LOOMORA_VBOXMANAGE`); the VM `Windows 2000` already running with the user logged on; `LOOMORA_VM_NAME`, `LOOMORA_VM_USER`, `LOOMORA_VM_PASSWORD`, `LOOMORA_VM_DIR` when they differ from the defaults | `validation/procedures/windows.md` part B, `validation/verdicts/windows-2000-x86-legacy.json` |
| Firefox 10 (Legacy Web page) | `python tools/run_vectors.py web-firefox-10-windows-2000-x86 legacy --timeout 120 --evidence-dir <folder>` | the same VM; the Legacy page built by `python3 tools/build.py web legacy`, served by the launcher over HTTP to `10.0.2.2` | `validation/procedures/web.md`, `validation/verdicts/web-firefox-10-windows-2000-x86-legacy.json` |
| Apple trials: macOS 10.6, 10.9 (Xcode 5.1.1) and macOS Tahoe (Xcode 26) in virtual machines; iOS 5 stays `not_tested` | `python tools/macvm.py check\|plan\|create\|start\|stop\|snapshot\|ssh` (VM disks on a VHDX made by `tools/macvm-vhdx.ps1`); the build and run steps are not written yet | WSL2 with QEMU/KVM (`/dev/kvm`, `ignore_msrs`), room on a Windows drive, media supplied or downloaded by the owner (an agent never downloads them); ADR 0019 | `validation/procedures/macos.md`; prepared on 2026-11-15, not run: no VM, no record yet |
| Old Linux distributions in Docker: manylinux2010 (CentOS 6.10), and manylinux1 (CentOS 5.11) on a WSL2 kernel | `docker build -t loomora-manylinux2010 tools/trials/linux-manylinux2010-x86_64` (or `-t loomora-manylinux1 tools/trials/linux-manylinux1-x86_64`), `tools/build.py linux legacy` inside the container, then `python tools/run_vectors.py <platform id> legacy --docker <image> --docker-volume <volume>` | Docker Desktop on the Windows host; for manylinux1 on WSL2, `vsyscall=emulate` in the kernel command line (`kernelCommandLine` of `.wslconfig`, then `wsl --shutdown` and a Docker restart), checked with `docker run --rm quay.io/pypa/manylinux1_x86_64 bash -c "cat /proc/cmdline; echo ok"` (exit 139 means it is missing) | `validation/procedures/linux.md` L.10 and L.11 |

Rules of the VirtualBox runs. The launcher never starts, stops, restores, snapshots or reconfigures a VM: the owner starts it (a desktop visible, the user logged on) and leaves it as found. The only snapshot is the owner's single snapshot `Claude Firefox 10`; nobody creates, restores or deletes another one, and a run that needs a clean state asks the owner to restore that snapshot. The guest has no usable Internet: every file reaches it through `VBoxManage guestcontrol ... copyto` (SHA-256 checked after the copy). Where the system images and installers came from: `validation/procedures/offline-install.md`.

Rules of the Docker runs. `vsyscall=emulate` is a setting of the WSL2 kernel of the owner's machine, needed only for the CentOS 5.11 image there; the hosted `ubuntu-24.04` runners have `CONFIG_LEGACY_VSYSCALL_XONLY=y` and run the same image unmodified, which is why `manylinux1.yml` is a CI job. WSL builds go under `$HOME/loomora-build/<name>`, never in `/tmp`.

Where the results live: `docs/implementation/validation/verdicts/` (one verdict per platform and variant), `procedures/` (how to replay), `evidence/<adr-or-change>/<platform-id>/` (only what proves something new), `deliverables/` (written by `tools/build.py`, committed only when a verdict refers to the build), all under `docs/implementation/validation/`. `python3 tools/report.py --check` and the `quality` workflow check that they agree.

Why no self-hosted runner: both repositories are public, so a pull request from a fork would run its code on the owner's machine, with its files, network and VMs. The local runs above are the replacement; ADR 0018 records the decision.
