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

The graphical interface will be native on each platform (Windows, macOS, Linux, Android, iOS, and the web), with a Legacy interface for very old systems and a Modern one for current systems, and with themes. Every interface shows the same results because none of them computes anything: they all rely on the same engine.

The full list of features, platforms and constraints is the [requirements document](docs/requirements-specification.md).

## Where the project stands

The graphical application does not exist yet. The project is in a validation phase: a minimal probe (a text field pre-filled "Coucou" and a label showing "Bonjour " followed by the field) checks, on each platform and for each interface variant, that the chosen technical approach holds, including on systems as old as Windows 2000. Four probe interfaces exist today: Windows Legacy, Linux Legacy, Linux Modern and Android Legacy (built for API 9, run on API 16 only). The other platforms are specified but not written.

## Where to go next

| You want | Read |
|---|---|
| to build and run the probe | [docs/building/README.md](docs/building/README.md) |
| to understand the engine, the code layout and the technical decisions | [docs/implementation/adr/README.md](docs/implementation/adr/README.md) (git submodule, see the build guide to fetch it) |
| the requirements | [docs/requirements-specification.md](docs/requirements-specification.md) |
| the plan and the task list of the validation phase | [specs/001-loomora-cross-platform/](specs/001-loomora-cross-platform/) |
| the rules to follow when contributing | [CLAUDE.md](CLAUDE.md) |
