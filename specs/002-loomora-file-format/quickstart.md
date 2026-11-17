# Quickstart: validating the `.loomora` format

How to prove, end to end, that the feature works. It lists what to run and what to expect; the details of each check are in the [contracts](contracts/) and the [data model](data-model.md). Commands are for the project's usual hosts (Windows host, WSL Ubuntu 24.04); builds in WSL go in a subfolder of `$HOME` (CLAUDE.md).

## Prerequisites

- The repository on branch `feat/loomora-format` (or a worktree outside the repository, with its own build directory).
- `git -c protocol.file.allow=always submodule update --init docs/implementation` when you want deliverable records, ADRs and evidence written (the build works without it).
- CMake 3.13+, a C++11 compiler (GCC or Clang), Python 3.8+; `pip install -r tools/requirements.txt` for the schema check. For fuzzing: Clang with libFuzzer.
- `third_party/zlib` and `third_party/rapidjson` are vendored: nothing to fetch.

## 1. The whole verification of the branch

```text
python tools/verify.py quick
```

Expected: one line per check, all `PASS`: build, `ctest` (Catch2 format tests and `loomora_format_selftest`), `loomora validate` on the valid references and on the invalid corpus, the independent checker on the references, `check_std`, `validate_records`. Exit status 0. No screenshot is taken (`full` is not needed: nothing graphical changes).

## 2. Round trip and determinism (User Story 1, FR-004, SC-001, SC-002)

```text
cmake --build <build-dir> --target loomora_format_selftest
<build-dir>/core/tests/loomora_format_selftest
```

Expected: the self-test builds the reference project in memory, writes it, prints `sha256 = <64 hex>` equal to the constant in `format_selftest.cpp`, reads it back and reports `0 differences`, then runs the truncation sweep and the embedded hostile inputs (`rejected N of N`). Run the same executable on every target that can run code (Windows 2000 VM, Linux i686 and x86_64, Android, Web through Node or a browser): every one prints the same hash.

## 3. Several frames, layouts, images, spools and results (User Story 3, SC-011, SC-013)

The reference project `multi` (3 frames: two 2D, one 3D; 5 layouts, two on one 2D frame; 4 images of mixed color mode and shape; 6 spools; 5 results, one frame-and-layout combination without result) is built by `tools/gen_reference.py` and `loomora_refgen`.

```text
<build-dir>/core/tests/loomora_refgen --out <dir>
python tools/check_loomora.py <dir>/multi-full.loomora <dir>/multi-light.loomora
loomora validate <dir>/multi-full.loomora
```

Expected: the checker and the command both report `valid`, with the counts of the manifest; the combination without result raises no issue. The Catch2 test `editor_add_result_keeps_others` adds a result, saves, and checks that every other entry has the same `sha256` as before (SC-013), and that removing a layout asks for confirmation (`LOOMORA_ERR_HAS_DEPENDENTS`) and then removes its results.

## 4. Use mode never writes (User Story 2, SC-012)

```text
python tools/check_readonly.py <dir>/multi-full.loomora
```

Expected: the script marks the file read-only, records its bytes and modification time, runs `loomora_project_open_path`, reads a result's steps and a thumbnail through the `loomora_use_demo` program, and checks that the file is unchanged and that no file appeared beside it. It then replaces the file atomically with another valid one while a second process holds the old one open and checks that the reader sees only the old file.

## 5. Invalid, hostile and very large files (User Story 5, SC-003, SC-005)

```text
python tools/gen_invalid_corpus.py --out <dir>/corpus
for f in <dir>/corpus/*.loomora; do loomora validate --quiet "$f"; done
```

Expected: every `invalid-*` and `hostile-*` file exits with status 1 and prints the code listed in its name (`hostile-zipslip-dotdot` → `zip.bad_name`, `hostile-bomb` → `entry.bomb`, `invalid-sha256` → `entry.sha256`, and so on); every `large-*` file exits with status 0 and prints `warn.count`; a truncation of the small reference at every byte exits with status 1 or 2, never a crash.

## 6. Light copies (User Story 7, SC-006, SC-014)

```text
loomora_use_demo export <dir>/multi-full.loomora <dir>/one.loomora --profile light --results 3
python tools/check_loomora.py <dir>/one.loomora
```

Expected: `one.loomora` holds result 3 and only the frame, layout, images (thumbnails only) and spools it uses; the mesh of its 3D frame has the same `sha256` as in the full file; editing it is refused with `LOOMORA_ERR_READ_ONLY_PROJECT`; it is at least 50% smaller than the full file when the images have pixels.

## 7. Versions and features (User Story 4, SC-007)

The Catch2 test `version_skew` opens files with a newer minor (extra fields and entries: opens, unknown entries kept on rewrite), a newer major (rejected with `This file needs Loomora format version 2.0 or newer`) and an unknown required feature (rejected naming it).

## 8. Fuzzing (SC-004)

```text
cmake -S . -B ~/loomora-build/fuzz -DLOOMORA_BUILD_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build ~/loomora-build/fuzz --target loomora_fuzz_project
~/loomora-build/fuzz/core/fuzz/loomora_fuzz_project -max_total_time=3600 -timeout=5 <dir>/corpus <dir>
```

Expected: no crash, no timeout above 5 s, no sanitizer report over at least 10 million inputs or one hour, whichever comes first.

## 9. Every UI (SC-010)

```text
python tools/run_vectors.py --validate-project <dir>/multi-full.loomora
python tools/run_vectors.py --roundtrip-project <dir>/multi-full.loomora
```

Expected: for each UI built on the host, the UI started with `--validate-project` prints the same validation line as `loomora validate` (same counts, same issues); started with `--roundtrip-project` it opens the file, writes a full copy of it in memory, validates the copy and prints `roundtrip ok` with the validation line of the copy, as the probe mode of spec 001 does for greetings. Per spec 001's PoC scope, Legacy UIs run on the lowest testable system, Modern UIs on one system and one 64-bit processor; other rows stay `not_tested`.

## 10. Cross-architecture reference files (SC-002)

Run step 3 on each architecture the project can run (x86_64, i686, a big-endian system or a PowerPC emulation if available) and compare:

```text
python tools/check_loomora.py --compare <dirA> <dirB>
```

Expected: every file of the same name is byte-identical; the only stored evidence is the first set of files and the hash table, under `docs/implementation/validation/evidence/adr-0021/<platform-id>/` (CLAUDE.md: store only what proves something new).

## What is not covered here

The transports (HTTP, Bluetooth, QR), file association, the screens of edit and use modes, image decoding, path computation and the weaving progress are other features (spec, Assumptions).
