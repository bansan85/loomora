# Loomora

## Commit conventions

- Commit messages are written in **English**.
- Conventional Commits prefix (`feat:`, `fix:`, `chore:`, ...), title of at most 50 characters, imperative mood.

## Git workflow

- `master` contains **only merge commits** (`git merge --no-ff`), except the empty root commit (`chore: start the project`), the only commit on `master` that is not a merge. Nothing is ever committed directly on `master`. The same holds for the documentation repository `../loomora-docs`.
- Every change, whoever makes it (Claude or an agent), is committed on a dedicated branch and merged into `master` afterwards, so the content of each change can be reviewed as one merge. An agent works in a separate git worktree created outside the repository (for example `../loomora-<topic>`) on its own branch and does not merge: Claude merges it, and only after the project owner has explicitly validated the branch.
- A branch is never merged on its own initiative. When it is finished, report it (branch, commits, what was verified and what was not) and wait for the owner's explicit approval before merging, moving the docs submodule pointer or deleting the worktree and the branch. A sub-agent's report is not an approval.
- The tag `dont-move-me` points to the old `master` tip (the full history before it was rewritten to this standard); it is kept for reference and never moved.

## Implementation documentation

- Documentation tied to the implementation (not user documentation) lives in a git **submodule**, not in the main repository: the architecture decision records (`adr/`) and the whole validation folder (`validation/`: platforms, verdicts, deliverables, spikes, sources, fallbacks, `evidence/`, `results/<spec>/` for the measurement and test-run records of a spec, and `bug-reports/`). The submodule repository is `../loomora-docs`, checked out at `docs/implementation/`. Building the product never needs it; `git -c protocol.file.allow=always submodule update --init docs/implementation` fetches it when working on the implementation (the relative URL is a local sibling repository, which git refuses as a submodule transport unless `protocol.file.allow=always` is given). `tools/build.py` writes the deliverable records into `docs/implementation/validation/deliverables/` when the submodule is checked out; without it the build still works, prints a warning and writes no record (`tools/validate_records.py` then exits with status 2 and a message). The Spec Kit files stay in `specs/` while a spec is being implemented. **When the implementation of a spec is finished, the whole spec folder moves to `docs/implementation/specs/<id>/`** (a commit on a docs branch that adds it, then a commit in the main repository that removes it from `specs/` and fixes the references); spec 002 (`.loomora` format) is the first one archived. Tools that read a file of an archived spec (the JSON schemas used by `tools/check_loomora.py`) read it there and need the submodule.
- The test drivers (scripts that produced evidence: `gui-driver.py`, `trial-driver.py`, `probe-series.py`, ...) are tooling, not evidence: they live in the main repository under `tools/trials/<platform-id>/` (marked `-text`, because their sha256 is recorded in the verdicts).
- The requirements document is `docs/requirements-specification.md` (English), in the main repository.
- Evidence is sorted: each piece goes in a subfolder dedicated to the ADR or the change it belongs to, then to the platform (`validation/evidence/<adr-or-change>/<platform-id>/`).
- Store evidence only when it proves something new (a new feature or behaviour). Do not store again the same documents that were already validated: the tests (`run_vectors.py`, selftests, Catch2) are the proof that the behaviour is unchanged. A re-run is mentioned in the verdict notes, with its result, not stored again.

## Fallbacks

- Never apply the most restrictive fallback to everyone. Apply a fallback only to the problematic case (a given platform, compiler, OS version or architecture), and use the modern version everywhere else.
- Choose the mechanism per case: a compile-time switch for a target-wide limit (toolchain flag, feature macro in `core/src/internal/config.hpp`), a run-time check (`GetProcAddress`, version query) when one binary must serve old and new systems.
- Every fallback is declared in `docs/implementation/validation/fallbacks.json` with the platforms that take it (FR-021).

## C++ standards

- The backend (`core/`, the library `libloomora` and its tests) is always compiled as **C++11** (`CXX_STANDARD 11`, `CXX_STANDARD_REQUIRED ON`, `CXX_EXTENSIONS OFF`), whatever the UI around it uses, because every UI links it. Set the standard as a target property, never globally, so a UI standard never leaks into the backend.
- Legacy UIs are written in **C++11**.
- Modern UIs may use up to **C++17** (for example gtkmm 4 needs it). Pick the lowest standard the toolkit accepts when it does not need more.
- Linux UIs are written in C++ (Xlib for Legacy, gtkmm for Modern) and built with CMake. Meson and Makefiles are not used.

## Where binaries are built

- Linux binaries (Legacy and Modern) are always compiled **on the target machine**, never built on a recent system and installed on an older one. The glibc and libstdc++ versions of the build host therefore do not matter, and there is no container or static C++ runtime to widen the baseline. The target needs a C++11 compiler, CMake and the development files of the libraries the UI uses.
- Windows binaries (including the Windows 2000 floor) are cross-compiled with MinGW-w64 from WSL, because Visual Studio no longer supports Windows 2000.

## Linux Modern UI (GTK)

- The Modern UI uses gtkmm (the system packages) on top of GTK. The minimum GTK version is **3.10**: the UI must always build and run with only the 3.10 API.
- Newer features, up to the most recent GTK (including GTK 4 and later), are allowed, but each one is enabled only by a compile-time macro that checks the GTK version of the headers being built against (`GTK_CHECK_VERSION`, `GTK_MAJOR_VERSION`, or a project macro built on them). Without the macro the 3.10 code path is used.
- Each such feature is declared as a fallback in `docs/implementation/validation/fallbacks.json`, as the Fallbacks rule requires.

## Builds in WSL

- Any build done in WSL goes in a subfolder of `$HOME` (for example `~/loomora-build/<target>`), never in `/tmp` and never inside the Windows tree.

## Verification and cost

- `python tools/verify.py quick` (the default) is the verification of any branch: build, ctest, the reference vectors through each UI, `check_std`, `validate_records`, one line per check, logs under the build directory. An agent runs it instead of improvising its own checks; the probe line, the vectors, ctest and `validate_records` are the functional proof.
- `python tools/verify.py full` only for a graphical change: a new graphical component, a new graphics platform or toolkit, new font / Unicode / emoji / non-ASCII rendering support, or a change of how text is drawn. Never to "check that it still works". It runs the Linux UIs on a private Xvfb display (never WSLg's `:0`).
- At most one screenshot per distinct rendering scenario, taken only by `full`, never to check that something works.
- Screenshots and other images are not evidence by default: before adding any image to `docs/implementation` the owner's authorization is asked first. An agent does not commit images; it lists the candidates (scenario, why it proves something new, file name) in its report, and they are committed only after the owner approves. `verify.py full` prints its screenshots as candidates, not committed, and never copies them into the repository.
- One default variant per UI during the probe phase. The other variants (`--variants`: GTK 3.10 floor, GTK 4, i686 Xlib, Xlib core font) only on request or when their own files change.
- Sub-agent prompts state the scope and the essential rules inline (language, commit format, worktree, build location, what not to touch) instead of only pointing to linked documents, which cheaper models read badly, and ask for a final report of at most about 25 lines.
- Parallel agents each use their own worktree and their own build directory (the default build directory of `verify.py` is per worktree).
