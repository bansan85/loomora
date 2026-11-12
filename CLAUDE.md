# Loomora

## Commit conventions

- Commit messages are written in **English**.
- Conventional Commits prefix (`feat:`, `fix:`, `chore:`, ...), title of at most 50 characters, imperative mood.

## Builds in WSL

- Any build done in WSL goes in a subfolder of `$HOME` (for example `~/loomora-build/<target>`), never in `/tmp` and never inside the Windows tree.
