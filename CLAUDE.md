# Loomora

## Commit conventions

- Commit messages are written in **English**.
- Conventional Commits prefix (`feat:`, `fix:`, `chore:`, ...), title of at most 50 characters, imperative mood.

## Fallbacks

- Never apply the most restrictive fallback to everyone. Apply a fallback only to the problematic case (a given platform, compiler, OS version or architecture), and use the modern version everywhere else.
- Choose the mechanism per case: a compile-time switch for a target-wide limit (toolchain flag, feature macro in `core/src/internal/config.hpp`), a run-time check (`GetProcAddress`, version query) when one binary must serve old and new systems.
- Every fallback is declared in `docs/implementation/validation/fallbacks.json` with the platforms that take it (FR-021).

## Linux Modern UI (GTK)

- The minimum GTK version is **3.10**: the UI must always build and run with only the 3.10 API.
- Newer features, up to the most recent GTK (including GTK 4 and later), are allowed, but each one is enabled only by a compile-time macro that checks the GTK version of the headers being built against (`GTK_CHECK_VERSION`, `GTK_MAJOR_VERSION`, or a project macro built on them). Without the macro the 3.10 code path is used.
- Each such feature is declared as a fallback in `docs/implementation/validation/fallbacks.json`, as the Fallbacks rule requires.

## Builds in WSL

- Any build done in WSL goes in a subfolder of `$HOME` (for example `~/loomora-build/<target>`), never in `/tmp` and never inside the Windows tree.
