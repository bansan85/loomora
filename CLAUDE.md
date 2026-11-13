# Loomora

## Commit conventions

- Commit messages are written in **English**.
- Conventional Commits prefix (`feat:`, `fix:`, `chore:`, ...), title of at most 50 characters, imperative mood.

## Fallbacks

- Never apply the most restrictive fallback to everyone. Apply a fallback only to the problematic case (a given platform, compiler, OS version or architecture), and use the modern version everywhere else.
- Choose the mechanism per case: a compile-time switch for a target-wide limit (toolchain flag, feature macro in `core/src/internal/config.hpp`), a run-time check (`GetProcAddress`, version query) when one binary must serve old and new systems.
- Every fallback is declared in `docs/implementation/validation/fallbacks.json` with the platforms that take it (FR-021).

## Builds in WSL

- Any build done in WSL goes in a subfolder of `$HOME` (for example `~/loomora-build/<target>`), never in `/tmp` and never inside the Windows tree.
