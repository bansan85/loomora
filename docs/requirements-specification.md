# Loomora — Functional and Technical Requirements Document (V6, consolidated)

Universal String Art software (the art of stretched string).

| | |
|---|---|
| Version | V6.6 — `.loomora` format rewritten from spec 002 (Annex B); V6.5 was the update after the phase 0 probe (changes and sources in Annex B); V6.4 was the consolidation of versions V1 to V5 (Gemini export "String Art Requirements Document") + the decisions of §16.1; native interface per platform, themes, non-flat supports (3D, meshes) |
| Date | 2026-11-17 (V6.5: 2026-11-15; V6.4: 2026-11-11) |
| Status | Draft to be validated; the phase 0 probe settled only the points listed in Annex B, **every other technical choice stays provisional until the phase 0 PoC (§15.1)**; points still open in §16.2 |
| Convention | **[P0]** essential · **[P1]** major · **[P2]** optional / V2 of the product. Unmarked: P1. |

Reading notes:
- The words "must" / "may" are normative: *must* = requirement, *may* = option.
- Items marked **[Proposal]** were not explicitly requested; they fill a gap noticed during the consolidation and are to be validated.
- Annex A traces each user request to its section; Annex B is the revision history.
- **Backend language (since 2026-11-12, ADR 0001)**: the backend is one C++11 code base behind a handwritten C API; Rust, Cargo, `cbindgen` and UniFFI are abandoned. Where this document still names a Rust crate or tool (`rayon`, `wgpu`, `axum`, `tokio`, `btleplug`, `rusqlite`, `fluent-rs`, `gtk-rs`, `wasm-bindgen`, `parry3d`, the `loomora-*` crate names…), read the role it plays (parallel CPU engine, GPU compute, embedded server, BLE, persistence, translation, geometry); the C++ library for each role is chosen when the feature is built, and the phase 0 probe tried none of them.

---

## 1. Introduction

### 1.1 Context
String Art consists of reproducing an image by stretching a continuous string (or several strings of different colors) between nails driven into a physical support (circle, rectangle, free shape, possibly with nails inside).

### 1.2 Objective
Loomora automatically turns a digital image (photo, logo, illustration, silhouette) into an optimized path of string segments, then provides:
- a nail placement template (1:1 PDF, machine export);
- a step-by-step weaving guide, manual, automatic or voice-driven, with error recovery;
- exports usable by digital machines (CNC, plotters, robots).

### 1.3 Target users
- Creators, craftspeople, makers, digital artists.
- FabLabs and users of plotters, CNC machines, robots.
- General public (computer-assisted creative hobby).

---

## 2. Platforms, architecture and degradation

### 2.1 Target matrix **[P0]**

| Platform | Architecture | Backend toolchain | Minimum / constraints |
|---|---|---|---|
| Windows | x86_64 | MSVC (`loomora.dll` called from C#, spike S8) | **Windows 10 version 1809+** (build 17763, the minimum of WinUI 3 on the Windows App SDK); WinUI 3 UI, framework-dependent (the .NET Desktop Runtime and the Windows App Runtime must be installed) |
| Windows | **ARM64** | MSVC (ARM64 `loomora.dll`) | Full native support (Surface, Copilot+, Snapdragon X); WinUI 3 UI |
| Windows **legacy** | x86 (Windows 2000 and later), x86_64 (XP x64 and later) | MinGW-w64 GCC, cross-compiled from WSL (ADR 0012) | **Windows 2000 minimum** (validated on build 2195 without service pack, §2.1.2); plain **Win32** UI, no MFC (ADR 0009); restricted features (§2.1.2) |
| macOS | ARM64 + x86_64 | To be chosen (spikes S2 and S7 open) | macOS 11+, Universal 2 binary (the spec 001 target macOS 10.6 is untried: no trial yet) |
| Linux | x86_64 + AArch64 (AArch64 outside the probe scope) | The C++11 compiler and CMake of the target machine: binaries are built on the machine that runs them (ADR 0007) | **Legacy UI (Xlib)**: any distribution with a C++11 compiler, CMake and the libX11 development files; the lowest userland tried is CentOS 5.11 (manylinux1 image, glibc 2.5). **Modern UI (gtkmm)**: GTK 3.10 API as the floor (§2.2.1). Plain executables: no AppImage, no Flatpak |
| Android | armeabi (ARMv5TE), ARMv7, ARM64, x86, x86_64 | Legacy UI: NDK r15c driven by a CMake toolchain file (ADR 0016); Modern UI: a current NDK with the Android Gradle plugin | **Legacy UI: minSdk = API 9 (Android 2.3)**, built for API 9 and run on API 16 (ADR 0016); **Modern UI: minSdk = API 24 (Android 7.0)**, native Kotlin UI (the pinned Compose and AndroidX libraries need API 23, spike S8); targetSdk according to the Google Play requirement at the time of publication (§2.2) |
| iOS / iPadOS | ARM64 | To be chosen (spikes S2 and S7 open) | **iOS 15.0 minimum**, with functional restrictions if an API is missing (the spec 001 target iOS 5 is untried: no trial yet); native SwiftUI UI (§2.2) |
| Web (PWA) **[P2, in scope]** | WebAssembly; JavaScript for the browsers without WebAssembly | Emscripten 7.0.0 (WebAssembly path); Emscripten 1.39.20 fastcomp (JavaScript path, ADR 0017) | Chrome, Safari, Firefox with WebAssembly: dedicated TypeScript web UI (Modern), bound to the WebAssembly engine; Legacy page in ECMAScript 3 JavaScript, down to Firefox 10.0 on Windows 2000 through the JavaScript path; Internet Explorer up to 9 is dropped; Safari is covered by inference (ADR 0017); WebGPU if available, otherwise CPU engine; engine in Web Workers; OPFS / IndexedDB storage; no embedded server and no BLE on Safari (the browser is a client of a Loomora server) |

Guiding principle: target the oldest devices accepted by the stores. **It is acceptable to disable features** on those devices (§2.3) rather than raise the floor.

#### 2.1.1 Extended architectures ("as many architectures as possible")
Support is organized in tiers, according to the availability of a C++11 compiler and of the platform libraries (the backend no longer depends on a Rust toolchain) and of the platform's native UI toolkit. The list must be revalidated when a target is added.

| Tier | Deliverable | Architectures |
|---|---|---|
| **A** | Complete packaged application (native UI); not confirmed for any platform as a packaged application: the phase 0 probe built plain executables only (V20, validation report) | The table in §2.1 |
| **B** | Complete application if the UI toolkit exists on the target (WinUI 3, GTK), otherwise tier C | Windows x86 32-bit (WinUI 3 or Win32 Legacy UI); Linux x86 32-bit; **Linux ARM 32-bit** (ARMv7, ARMv6); Linux RISC-V 64; **Linux PowerPC 64 little-endian** |
| **C** | CPU engine + CLI + embedded server; the UI is the PWA served to a local or remote browser (§9.2) | **Linux PowerPC 32-bit and 64-bit big-endian**; s390x; LoongArch64; FreeBSD; other targets with a C++11 compiler |

Cross-cutting constraints:
- **32-bit**: never assume `size_t` or `long` = 64 bits; addressable memory budget of 2 to 4 GB; 64-bit atomics through a portable layer.
- **Big-endian**: all serialization (files, BLE, database) is explicitly little-endian; CI tests under emulation (QEMU).
- **SIMD**: a portable scalar path is mandatory; NEON, SSE / AVX, AltiVec / VSX are optional optimizations.
- **GPU**: absent or unsupported on these targets → CPU engine.
- **Outside the probe scope** (owner decision 2026-11-15): PowerPC, MIPS, Itanium, Linux AArch64, Linux ARM 32-bit and RISC-V stay in the targets of this document, but the phase 0 probe does not build or test them (spec 001 Clarifications 2026-11-12).
- "Best-effort" support outside tier A, with CI building and testing the engine on every declared target that a GitHub-hosted runner can run; the other targets (old systems, virtual machines, emulators) are local runs recorded in the validation records (ADR 0018).

#### 2.1.2 Windows legacy (Windows 2000 and later) — Win32 UI

Windows has **two UIs**: **WinUI 3** (modern) and a plain **Win32** UI (Windows 2000-compatible). They rely on the same backend and the same `.loomora` files.

- **Win32 scope**: Windows 2000 and later, x86 (the i686 executable runs on Windows 2000 to 11) and x64 (the x86_64 executable, Windows XP x64 and later), ADR 0009. The same executable also runs on Windows 10 and 11; the user can choose WinUI 3 or Win32 there. The floor system validated is Windows 2000 build 2195 without service pack, in a virtual machine (SP4 itself was not run; spike S1 rests on an inference accepted by the owner). Windows XP is covered by inference from Windows 2000, no XP system was run (go / no-go of §15.1.5).
- **Language and toolchain**: C++11 on the plain Win32 API (`CreateWindowEx` and the standard controls), no MFC; the UI layer is thin and free of business logic, and calls the backend through its C API (ADR 0001). It is built with MinGW-w64 GCC cross-compiled from WSL, like the backend (ADR 0012): no MSVC toolset that compiles C++11 targets Windows 2000, and recent Visual Studio no longer targets XP. A Windows function newer than Windows 2000 is looked up at run time and skipped when absent, each skip being a declared fallback (ADR 0009, ADR 0004).
- **Toolchain risk lifted**: Rust is abandoned (ADR 0001), so the major risk of an unofficial Rust toolchain for XP no longer exists; the question became whether a C++11 Win32 executable runs on the old system, answered on Windows 2000 (spike S1).
- **Features explicitly disabled on XP** (§2.3; not exercised by the probe, which has none of these functions):
  - GPU (CPU engine only); 2 GB of addressable memory, hence reduced resolution (§6.1).
  - Bluetooth Low Energy (absent from XP): transfer by file or local HTTP.
  - Modern HTTPS / TLS (TLS 1.2 absent by default): the embedded server stays on HTTP on the local network; no mDNS, pairing by QR or IP address.
  - Simplified embedded server (`std::net` + threads): the modern asynchronous I/O used by `tokio` does not exist on XP.
  - Voice: SAPI 5 if installed, otherwise the feature is disabled (decision 16.1 no. 4).
  - No PWA and no embedded webview.
  - Recorded by the trials on Windows 2000: no DPI awareness and no dark theme (§2.3).
- **Drawing and themes**: plain GDI for path drawing; GDI+ is an optional improvement, looked up at run time and skipped on systems without it (a plain-GDI path always exists); light / dark themes drawn by the application (Win32 *owner-draw*); high contrast follows the Windows contrast scheme.
- **Security**: Windows 2000 and XP are no longer maintained by Microsoft. The application displays an end-of-life warning (shown on Windows 2000) and exposes nothing outside the local network.

### 2.2 Technical stack
- **Backend entirely in strict C++11** (`-std=c++11 -pedantic-errors`): one code base behind a handwritten C API (`core/include/loomora.h`, valid as C89); exceptions never cross that boundary (ADR 0001, spike S6). Rust is abandoned. The backend is always compiled as C++11 whatever the UI uses; Legacy UIs are written in C++11, Modern UIs may use up to C++17 (ADR 0011).
- A feature **may be reimplemented several times depending on the platform** (audio, Bluetooth, printing, speech recognition…), but universal methods should be preferred when they exist.
- **User interface: native on each platform** (decision 16.1 no. 1). Tauri and webviews are ruled out for installed applications; only the PWA is a web UI. Each platform has **two UIs on the same backend**: a Legacy UI for the oldest systems (features may be disabled, §2.3) and a Modern UI (ADR 0008).

#### 2.2.1 UI per platform

| Platform | UI technology | Remarks |
|---|---|---|
| Android | Modern: Kotlin, Jetpack Compose (API 24+); Legacy: Java views with JNI glue (API 9 to 23) | Compose and AndroidX need API 23, the Modern floor is 24 (spike S8); ADR 0008, ADR 0016 |
| iOS / iPadOS | SwiftUI (Swift); Legacy: Objective-C (UIKit), decided in ADR 0008, not built | iOS 15; occasional UIKit if a SwiftUI API is missing |
| macOS | SwiftUI, module shared with iOS / iPadOS; Legacy: Objective-C (AppKit), decided in ADR 0008, not built | Occasional AppKit |
| Windows (x86, x64, ARM64) | WinUI 3 (C#, Windows App SDK) | Modern UI; Windows 10 version 1809+; calls the backend C API through `LibraryImport`, no C++/CLI (ADR 0008) |
| Windows 2000 and later (x86, x64) | Win32 (C++11, MinGW-w64) | Legacy UI (§2.1.2), no MFC (ADR 0009); thin layer over the backend's C API |
| Linux | Modern: gtkmm (C++, CMake) on GTK 3 with the 3.10 API as the floor, GTK 4 with gtkmm-4.0 as a build option; Legacy: Xlib with Xft (C++11, CMake) | Plain executables built on the target machine, no libadwaita, no Flatpak, no AppImage (§16.1 no. 9; ADR 0005, 0006, 0007, 0010) |
| Web (PWA) | Modern: TypeScript + lightweight web framework, engine in WebAssembly; Legacy: ECMAScript 3 JavaScript page, engine in WebAssembly or compiled to JavaScript | Same application model as the other UIs (ADR 0017) |
| Tier C (§2.1.1) | CLI + web UI served by the embedded server (§9.2) | |

Principles:
- **Thin UIs, zero business logic.** States, use cases and view-models live in the C++11 backend, whose only interface is the handwritten C API `core/include/loomora.h` (ADR 0001, ADR 0008): header and static library for the C++ and Objective-C UIs (Win32, Xlib, gtkmm), a module map for Swift, `LibraryImport` for C# (WinUI 3), JNI glue for Java and Kotlin, Emscripten exports for the web.
- **Functional parity.** This document is the single specification. Common acceptance scenarios are run on every UI. A function missing from a platform is disabled explicitly (§2.3), never silently.
- **Path drawing.** The core provides primitives (polylines, nails, colors, opacity) that each UI draws with its native canvas (Compose Canvas, SwiftUI Canvas / Metal, Direct2D / Win2D, Cairo / GL, Canvas 2D / WebGL). A shared `wgpu` rendering on a native surface (`loomora-render3d`) is adopted for the 3D view (§3.6.7) **[P2]**.
- **System services** (Bluetooth, voice / TTS, printing, sharing, files, notifications): implemented natively by each UI behind interfaces defined in the backend (callbacks across the C API: mechanism to be defined, not tried by the probe). On desktop, Bluetooth may go through a cross-platform BLE library.
- **Texts**: provided by the core (§11), to keep a single translation source.

### 2.3 Graceful degradation (old systems and devices: Windows 2000, Android API 9–25, 32-bit, 1–2 GB of RAM)
- GPU acceleration disabled automatically; strict fallback to the CPU engine.
- Simulation canvas resolution reduced (§6.1).
- Advanced offline speech recognition disabled, with fallback to the manual and automatic (timer) modes. Speech synthesis remains in use if the system provides it.
- Upfront memory allocation to avoid fragmentation; buffers that can be shrunk dynamically (tiled cache or temporary downsampling during computation).
- No instruction newer than ARMv7 is required in the 32-bit fallback binary.

**Disabled features recorded by the phase 0 trials** (each is a declared fallback, `validation/fallbacks.json`; the reasons are in the verdicts):

| Platform (UI) | Disabled feature | Reason |
|---|---|---|
| Windows 2000 (Legacy) | DPI awareness | `user32.dll` does not export `SetProcessDPIAware`; the window is drawn at 96 DPI |
| Windows 2000 (Legacy) | Dark theme | No `AppsUseLightTheme` setting on this system; system colours are used |
| Linux, CentOS 5.11 and 6.10, Ubuntu 24.04, i686 (Legacy) | Missing glyph | A code point that no installed font has is drawn as an empty box (the CentOS images have no CJK and no emoji font) |
| Linux, CentOS 5.11 and 6.10, Ubuntu 24.04, i686 (Legacy) | Text shaping and bidirectional layout | Not implemented, by decision: logical order, left to right, isolated Arabic forms, ZWJ sequences drawn as components |
| Linux, Ubuntu 24.04 and i686 (Legacy) | Glyph substitution | Core-font path, taken only when libXft and fontconfig are missing at build time or switched off at run time (not the path of the builds tried): a character the core font cannot draw is drawn as `?` |
| Android 7.0 (Modern) | High contrast through `UiModeManager.getContrast()` | The call needs API 34; below it the standard Material 3 light or dark scheme is used |
| Web, Firefox 10 (Legacy) | Peak memory | Firefox 10 has no `performance.memory` |

### 2.4 Compute engines: CPU and GPU **[P0]**
Two interchangeable implementations behind a single interface, with automatic selection at startup according to the detected capabilities (the user can force an engine).

```
                 ┌──────────────────────┐
                 │   Compute controller  │
                 └──────────┬───────────┘
              ┌─────────────┴─────────────┐
              ▼                           ▼
     [ CPU backend ]              [ GPU backend ]
     rayon + SIMD                 wgpu + WGSL
     guaranteed fallback          compute shaders
```

- **CPU backend** (universal, mandatory everywhere): `rayon` parallelization of the evaluation of candidate segments; portable SIMD (NEON on ARM64, SSE2/AVX on x86_64).
- **GPU backend**: `wgpu`, WGSL shaders written once, translated to DirectX 12 (Windows x64 and ARM64), Metal (macOS, iOS), Vulkan (Linux, modern Android), OpenGL ES 3.x (Android fallback floor). The error matrix is a texture in VRAM; the error reduction of each segment is evaluated in parallel.
- Both backends must produce equivalent results (non-regression tests comparing CPU and GPU).
- **No saving of the computation state**: if interrupted, the computation is restarted. Only the weaving progress is persistent (§10.2).

---

## 3. Physical support and nails

### 3.1 Topology: shape within a shape **[P0]**
The nails form a shape (outer contour). One or more **interior exclusion shapes** can be added to it:

- Weaving domain: `Ω = Ω_ext \ ⋃ Ω_int_k`. The space between the outer contour and each inner contour is filled with string; **the inside of an inner contour stays free of string**.
- Nails can be placed on the outer boundary `∂Ω_ext`, on the inner boundaries `∂Ω_int` (to outline the negative shape) and inside the domain (§3.4).
- **Visibility constraint**: two nails A and B can be connected only if the segment AB, widened by the string thickness, crosses no exclusion zone **and no other nail** (nail radius + string radius). Pre-filtering by a boolean visibility matrix or BVH acceleration.

```
┌──────────────────────────────────────────┐
│              OUTER CONTOUR               │
│   •   •   •   •   •   •   •   •   •   •  │
│  •      \   /   \   /   \   /        •   │
│  •       X   ┌──────────┐   X        •   │
│  •      / \  │ EXCLUSION │  / \       •   │
│  •          │ (0 string)│            •   │
│   •   •   •  └──────────┘ •   •   •   •  │
└──────────────────────────────────────────┘
```

### 3.2 Predefined shapes **[P0 for circle and rectangle]**
Each shape is parameterizable (real dimensions in cm/mm, ratio, smoothing) and can be used as an outer contour **or** as a negative interior mask.

| Shape | Parameters |
|---|---|
| Circle, ellipse | Radii |
| Rectangle / square | Width, height, optional rounded corners |
| Regular polygon | N sides |
| **Heart** | `x = 16 sin³t`, `y = 13 cos t − 5 cos 2t − 2 cos 3t − cos 4t` |
| **Drop / teardrop** | `x = a cos t`, `y = b sin t · sinᵐ(t/2)` |
| **Star** | N points (4 to 12), inner radius r and outer radius R |
| Clover / rosette | Polar curve `r = a cos(kθ)` or `a sin(kθ)`, k = 3, 4, 5 |
| Crescent moon | Boolean difference of two offset circles |
| Free silhouette | Closed SVG path converted to a boundary polygon |

### 3.3 Import of supports and nails **[P0]**
- **Tabular nail import** (CSV / TSV / JSON): `id: u32`, `x_mm: f64`, `y_mm: f64`, `diameter_mm: f64`, `type` (`boundary` / `interior`).
- **Vector import** (SVG, DXF) to extract a contour or drilling points.
- A free shape can be defined by any set of nail coordinates.

### 3.4 Automatic nail generation **[P1]**
Computation of the ideal nail locations, on the contour **and inside** the image, using three methods:
1. **Edge detection** (Canny / Sobel): nails distributed along strong structural edges (eyes, silhouette).
2. **Ink density** (blue noise, Voronoi halftoning, Poisson disk sampling): the darker or the richer in detail an area is, the higher the nail density.
3. **Adaptive grid / Delaunay triangulation**: structured internal mesh.

Constraints:
- Minimum nail-to-nail distance configurable in mm (e.g. 5 mm) so as not to split the support.
- Uniform distribution on a circle: θᵢ = 2πi/N; regular spacing along the sides of a rectangle.
- Adjustable number of nails (100 to 500 as standard, 200–288 typical; extensible range).
- **Minimum step between two consecutive nails** in the path (connecting two adjacent nails is forbidden).

### 3.5 Nail and string parameters
- Nail diameter (per nail if imported), string diameter, measured string opacity, color.
- Estimated maximum string length (m) and maximum number of lines (typ. 2,000 to 5,000) as stopping criteria.

### 3.6 Non-flat supports (3D) **[P2]**

The support can be a three-dimensional surface or volume, **convex or concave**: cylinder, sphere, cone, torus, box, lampshade, cage, scanned object… The planar support (§3.1) is the special case z = 0.

Priorities: the complete feature is **[P2]**, after the MVP. However, the **data model and the 3D-ready interfaces are [P0] from phase 1** (§3.6.1), to avoid having to redo everything.

#### 3.6.1 Geometric model **[P0 for the types and interfaces]**
- **Coordinate system**: right-handed, in millimeters (§9.1). Points are (x, y, z); z = 0 for a planar support.
- **Support surface**: triangle mesh, canonical internal representation whatever the origin (import, primitive, construction).
- **Dimension-independent interfaces**: "does segment AB meet an obstacle?" (§3.1), "what are the tangents between two nails?" (§6.2), "which pixels does a segment cover?" (§5.2). 2D and 3D are two implementations of these interfaces.
- **Support type** (`body`):

| Value | Meaning | Required mesh |
|---|---|---|
| `solid` | Solid volume: the interior is inaccessible, the string can only be outside. | Closed (watertight) |
| `shell` | Thin wall (lampshade, shell, bowl): interior and exterior are accessible but separated by the wall, which the string crosses only through the mesh openings. | Closed or open, oriented normals |
| `guide` | Reference surface only: it does not obstruct the string; it is used to place the nails and project the image (cage, openwork structure). | Any |

- **Orientation**: normals point to the outside. For an open mesh, the side the normals point to is the outside.
- **Convex / concave**: no assumption is made; the software analyzes the shape (convex hull, curvature) and reports the consequences (§3.6.6).

#### 3.6.2 Entering the surface mesh **[P2]**

| Method | Detail |
|---|---|
| **Mesh file import** | STL (binary and ASCII), OBJ first; PLY, 3MF, glTF / GLB afterwards. CAD formats (STEP, IGES) are not imported directly: prior conversion to a mesh (§17). On import: unit (mm, cm, inch, m), scale, vertical axis orientation, origin, mirror. |
| **Tabular import** | Vertices (`id, x_mm, y_mm, z_mm`) and triangles (`a, b, c`) as CSV / JSON: the 3D equivalent of the nail import (§3.3). |
| **Parametric primitives** | Box, cylinder, cone and truncated cone, sphere, ellipsoid, torus, capsule, pyramid; dimensions in mm and mesh resolution. |
| **Extrusion of a 2D shape** | Any shape from the library (§3.2) or an SVG, over a given height, with optional draft. |
| **Revolution of a 2D profile** | Profile drawn or imported (polyline, SVG) rotated around an axis: vase, lampshade, spinning top. |
| **Boolean operations** | Union, difference, intersection between primitives and extrusions, to obtain composite concave shapes. |
| **Scanned object** | Via PLY / OBJ import of a photogrammetry or scanner mesh. A point cloud alone is not supported (§17). |

**Mesh preparation and checking** in the application:
- Transformations: scale, rotation, translation, mirror, recentering.
- Repair: normal flipping, consistent orientation, vertex welding, removal of degenerate and duplicate triangles, hole filling.
- Simplification (decimation) with error control; cutting by a plane.
- Check report: watertight or not, manifold, self-intersections, volume, bounding box, triangle count.
- No manual vertex editing: out of scope, use an external tool.

**Limits**: 5,000,000 triangles importable; above 500,000, simplification is offered; lower thresholds on limited devices (§2.3).

#### 3.6.3 Nails in 3D **[P2]**
- Each nail has: a position (x, y, z), an **axis** (unit vector from the base to the tip), a length protruding from the surface, a diameter, a **side** (`exterior`: the nail protrudes toward the outside of the surface; `interior`: toward the inside) and, optionally, its attachment to the surface (triangle and barycentric coordinates) so that editing follows the surface. A "free" nail (independent rod) is allowed.
- **Nails can be on both sides** of the same mesh, inside and outside.
- Default axis: surface normal; configurable inclination, globally or per region.
- **Tabular import**: `id, x_mm, y_mm, z_mm, ax, ay, az, diameter_mm, length_mm, side, kind`.
- **Automatic generation on the surface**:
  - Poisson disk on the surface, with a minimum 3D distance in mm;
  - sharp edges (dihedral angle threshold);
  - density driven by the projected image (§3.6.5) or by curvature;
  - regular grid on primitives (cylinder: columns × rows; sphere: parallels and meridians);
  - selection of regions to fill or to exclude, and choice of the side(s).
- **Constraints**: minimum 3D distance between nails; minimum distance between nails located on either side of a thin wall; nail length less than the space available behind the surface.

#### 3.6.4 String in 3D: domain, admissibility, tangents **[P2]**
**String domain**, chosen per spool: `exterior`, `interior` or `both`.

| Support type | `exterior` | `interior` | `both` |
|---|---|---|---|
| `solid` | Yes: around the object | Impossible | Equivalent to `exterior` |
| `shell` | Yes: outside the wall | Yes: inside the cavity | Yes; each segment stays on one side only (opaque wall), except through the openings |
| `guide` | Not applicable: no obstacle | Not applicable | Not applicable |

**A segment AB is admissible if:**
1. it crosses neither the support material (outside the mesh openings) nor another nail (cylinder of radius nail radius + string radius);
2. it stays entirely within the allowed domain;
3. its length is less than or equal to the set maximum;
4. its **inclination relative to the nails** is limited: the angle between the segment and the plane perpendicular to the nail axis stays below θmax (default 30°), otherwise the string slides toward the tip or the base.

**Consequences of shapes** (computed, never assumed):
- *Convex, nails and string outside*: every chord between two points of the surface crosses the solid. Only segments between the tips of sufficiently long nails or near edges remain admissible. The diagnosis (§3.6.6) announces this.
- *Convex, nails and string inside (shell)*: all chords are admissible, this is the favorable case.
- *Concave*: some chords leave the volume (interior string) or cross a protrusion (exterior string); they are discarded by the intersection test.

**Method**: segment-mesh intersection by BVH; domain membership by *winding number* for a closed mesh, or by the side of the normal at the first hit point for an open mesh.

**3D tangents**: a nail is a cylinder with an arbitrary axis. The tangents between two non-parallel cylinders have no simple closed form: numerical solution (Newton) for each of the four CW / CCW combinations, with the length of the winding arcs.

**Pass levels**: each pass has a height along the nail (string stack). Nail capacity = usable length / string diameter. The weaving order fixes the stacking, and the instruction gives the level (low / middle / high or a value in mm). The level is added to `sequence.bin` (§9.1).

#### 3.6.5 Target image and error function **[P2]**
- **Projection views** (basic method): one or more cameras (position, orientation, orthographic by default), each with its target image and weight. The error is the sum of the per-view errors. Each view rasterizes the visible strings, occluded by the support material (`solid`, `shell`) or not (`guide`).
- **Mapping**: planar, cylindrical or spherical projection of the image onto the surface, so that a cylinder or a sphere carries the image all around.
- **Resolution**: rule of §6.1 applied to each view (1 px = 1 string diameter at the scale of the object, orthographic views).
- **Engines**: the greedy algorithm (§5.2) works on the graph of admissible segments; the coverage of a segment is the set of its pixels in each view (CPU or GPU computation). For LSQR / NNLS, the per-view matrices are stacked. Multi-color (§7) is unchanged, and the domain may be specific to each spool.
- The vector modes (§5.1) on developable surfaces are deferred (§17).

#### 3.6.6 Diagnostics and design assistance **[P2]**
- Automatic analysis: watertightness, global convexity, concave regions.
- **Heat map** of the number of admissible segments per nail; warning if a nail has fewer than N admissible neighbors; estimate of the total number of admissible segments.
- Recommendations: run the string inside, lengthen the nails, add nails on the opposite face.
- Display of the admissible segments of a selected nail.

#### 3.6.7 3D visualization and workshop **[P2]**
- **Interactive 3D view**: rotation, zoom, section cut; solid, wireframe or transparent mesh; nails with their axis and side; strings woven in 3D; current and next nails highlighted; "from the nail" view.
- **Shared rendering** `wgpu` (`loomora-render3d`) in a native surface of each UI: Android `SurfaceView`, iOS and macOS Metal layer, WinUI `SwapChainPanel`, GTK `GLArea`, web WebGPU with WebGL2 fallback. Software wireframe fallback without GPU.
- **Player**: nail number, CW / CCW direction seen from the tip, pass level, side (interior, exterior, face), and a 3D view that follows the current nail. 3D clock reference: 12 o'clock = projection of the world +Y onto the plane perpendicular to the nail axis (+X if the axis is parallel to Y), seen from the tip.
- **Unweaving** (§10.3): logic unchanged; the side and the level can resolve ambiguities.

#### 3.6.8 Exports and templates **[P2]**
- 3D nail list (CSV / JSON: `x_mm, y_mm, z_mm, ax, ay, az, length_mm, side`); mesh with nails as STL / OBJ.
- **Placement template**: for developable surfaces (cylinder, cone, extrusions), an unfolded pattern as a 1:1 PDF with markers and tiling (§8.1); for the others, the coordinates of each nail (cylindrical, spherical or Cartesian from reference marks) and dimensioned orthographic views.
- Machine JSON (§8.3): additional fields `level` and `side`, 3D positions.

#### 3.6.9 Degradation and performance
- The precomputation of the admissibility matrix (N²/2 segment-mesh tests by BVH) is parallelized on CPU / GPU, with pruning by distance, angle and bounding boxes. The numerical targets will be set by benchmark (phases 1 and 8).
- Limited devices: automatic mesh simplification, wireframe 3D view without GPU.
- **Windows 2000 and XP (Win32 Legacy UI)**: CPU computation and export, 100,000-triangle cap, basic software wireframe 3D view.

---

## 4. Source image handling **[P0]**

- Import of PNG, JPG, WebP (other formats if possible).
- Cropping with a circular or rectangular centering tool on the area of interest (face…).
- Adjustments: brightness, contrast, gamma curve (to emphasize key dark areas), light Gaussian blur (noise), grayscale conversion, thresholding.
- Automatic grayscale inversion: a dark string on a light support darkens; the error target image is therefore the inverse of the visual image (depending on the support color).
- **Image type detection** (100% opaque black/white flat areas vs. photo with shades) to suggest the suitable computation mode (§5), which the user can change.
- Images never leave the device by default (§13).

---

## 5. Path computation engines

Several computation modes to choose from, suited to the image type.

### 5.1 Vector mode / solid shapes (100% opaque black and white) **[P1]**
For logos, typefaces, icons, silhouettes. The user **selects the fill pattern**:
- **Hatching / parallel lines**: angle θ and spacing step.
- **Triangulation (constrained Delaunay)**: the string weaves a network of triangles between the contour nails.
- **Polygonal patterns**: squares, diamonds, honeycomb, generated inside the vectorized masks.
- **Eulerian continuous path (Chinese postman problem)**: route optimized to draw the edges without cutting the string.

### 5.2 Photographic mode / gradients **[P0 for the greedy algorithm]**
1. **Iterative greedy (Greedy Line Search)** — fast and robust:
   1. Initialize the error matrix from the target image (virtual canvas).
   2. From the current nail, test all admissible nails (non-adjacent, visible, valid tangents).
   3. Compute the error reduction (sum of the intensities of the pixels covered by the segment).
   4. Choose the segment that maximizes similarity to the target.
   5. Subtract the string intensity along the segment in the error matrix.
   6. Repeat until the line / length limit or an error threshold is reached.
2. **Regularized least squares (LSQR / NNLS)**: system `A·x ≈ b` (b = vectorized image, A = matrix of possible segments, x = pass coefficients). More homogeneous rendering, avoids the local saturation of the greedy algorithm. The resulting coefficients are then discretized into a continuous path.
3. **Metaheuristics (genetic algorithm, simulated annealing)**: mutation of populations of paths (adding, removing, inverting chords); suited to an elegant path with few lines.
4. Open architecture: the algorithm interface must allow other generic algorithms to be added.

### 5.3 Target performance
Computing 3,000 lines for 250 nails must not exceed 5 to 10 s on a modern consumer processor (CPU backend, standard resolution).

---

## 6. Physical modeling

### 6.1 Simulation resolution **[P0]**
The computation canvas depends on the string diameter.

- **Standard recommendation: 1 pixel = 1 string diameter.** `N_pixels = ⌈D_support / e_string⌉`.
  - Example: 500 mm support, 0.2 mm string → 2,500 × 2,500 px; 600 mm support → 3,000 × 3,000 px.
- Fewer than 1 px per string causes overlap errors (staircase, aliasing).
- **High quality (optional)**: 2 px per string (sub-pixel), memory ×4.
- **Low-memory fallback** (API 21, < 2 GB of RAM): 0.5 px per string (e.g. 1,250 × 1,250 px), memory ÷4.
- The engine selects the level according to the available RAM and can degrade it automatically.

### 6.2 Passing around the nail: tangents **[P0]**
The string does not go through the nail axis but around it.

- Model: nail = cylinder of radius `R_nail`; effective winding radius `R_eff = R_nail + R_string`.
- For each pair (A, B) of effective radii `R_A`, `R_B`, the algorithm computes **4 tangents**: 2 external (same direction of rotation at both nails) and 2 crossed internal ones (opposite directions).
- The choice of the tangent depends on the arrival angle from the previous nail (string continuity, tension, avoiding the string slipping off at sharp angles).
- The length of each segment is computed on the actual tangents, including the winding arc around the nails.

### 6.3 Winding direction: universal reference frame **[P0]**
"Right / left" is ambiguous depending on the approach direction. The internal model is therefore **the physical direction of rotation of the string around the nail**:
- **CW** (clockwise) or **CCW** (counterclockwise).

**Convention**: the direction is read **from the nail tip, looking toward its base** (in 2D, the nail axis points toward the viewer: this is the front view; in 3D, it is the nail axis, §3.6). Each instruction is expressed as: `nail A [CW|CCW] → nail B [CW|CCW]`. The tangent types are therefore `CW→CW`, `CCW→CCW` (external), `CW→CCW`, `CCW→CW` (internal).

Projection for the user (interface and voice), selectable in the preferences:
1. **Relative to the direction of travel** (vector `u = AB/‖AB‖`): "go around on the right / left".
2. **Absolute on the nail dial** (clock or cardinal points): the string arrives from the left → "pass over the top (12 o'clock)"; arrives from the bottom → "pass on the right (3 o'clock)".
3. **Technical**: CW / CCW display with a pictogram.

Voice prompts adapt their vocabulary so that they are immediately understandable by the operator facing their board.

---

## 7. Multi-spool (multi-color)

### 7.1 Palettes
- CMYK, grayscale (black, charcoal, light gray, white), or a custom palette of up to **12 string colors** (HEX/RGB code, measured opacity).
- Black/white or colored support.

### 7.2 Weaving strategies **[P2 for multi-color; both strategies are required]**

| Strategy | Principle | Advantages | Disadvantages |
|---|---|---|---|
| **Sequential** (spool by spool) | Color A is woven entirely, then B, then C, then D | Only one string change per color | The last colors strongly mask the first ones; optical blends are more rigid |
| **Interleaved** (optimal color at each step) | At each iteration, all colors are evaluated; the segment with the best colorimetric gain is applied | Clearly superior rendering and gradients | Requires juggling between strings or frequently tying / cutting |

The rendering differs significantly between the two: the user must be able to **compare the simulations** before choosing. Each instruction carries a spool identifier.

---

## 8. Exports and printing

### 8.1 PDF nail template **[P0]**
- **1:1** scale (actual size), vector, with a centering crosshair and legible (oriented) numbering for each nail, alignment marks.
- Customizable **scale factor**: zoom in % or forced target dimension; enlargement or not.
- **Tiled printing (tiling)**: automatic splitting across several A4 / A3 sheets with overlap / glue margins and cross-shaped registration marks.
- **Direct printing** to the system printers: Windows spooler, macOS/CUPS, Android Print, AirPrint.

### 8.2 Manual instructions
- Text / CSV list: `12 → 154 → 3 → 89 …` with winding direction and spool.
- **Illustrated PDF data sheet**: path diagram, total string length per color (with a 10% safety margin), number of passes per nail (mechanical saturation), number of lines, project parameters.

### 8.3 Exports for digital machines **[P2]**
**Nail list export** (positions, for automated drilling or nail insertion) and **path export**:

| Format | Content | Use |
|---|---|---|
| **Nails CSV / JSON** | `id, x_mm, y_mm, diameter_mm, type` (3D: + `z_mm, ax, ay, az, length_mm, side`, §3.6.8) | Drilling / nailing machines, import into other software |
| **Nails DXF / SVG (circles)** | One point or circle per nail at 1:1 scale | Laser cutters (pre-drilling), CAD |
| **Drilling G-code** | Drilling cycle at each nail, configurable feed rates | CNC milling machines |
| **Weaving G-code** (`.gcode`, `.nc`) | XY(Z) tangent points, `F` speeds, head orientation according to CW/CCW, `E`/`W` string payout and tension axis | Winding heads, CNC, modified 3D printers |
| **Structured SVG** | Continuous 1:1 polyline, metadata and one layer per spool (`layer_spool_1`…) | Plotters (AxiDraw), lasers |
| **Machine JSON / CSV** | See schema below | Robots, controllers (PLC), API |

Machine JSON schema (fields `from_side` / `to_side` ∈ `CW` / `CCW`):
```json
[
  {
    "step": 1,
    "from_nail": 12, "from_side": "CW",
    "to_nail": 87,   "to_side": "CCW",
    "spool_id": 0,
    "length_mm": 412.35
  }
]
```

---

## 9. Path exchange, server and Bluetooth

A path computed on a powerful machine (PC, Mac, Linux, server) must be importable on a workshop terminal (phone, tablet, controller).

### 9.1 `.loomora` project and path format **[P1]** (validated, decision 16.5; rewritten 2026-11-17 from spec 002)

The byte-level specification is `docs/implementation/specs/002-loomora-file-format/contracts/project-format.md`; this section states the requirements and the structure. Where the two differ, the contract wins.

**Principles**
- A **project** file: one self-describing archive that holds everything the person created: **several frames** (2D and 3D side by side), **several nail layouts** per frame, **several source images**, **several spools** and **several results** (string paths). One format for the file (import / export / sharing), the network (§9.2), Bluetooth (§9.3) and the PWA.
- Self-describing, versioned, verifiable, readable without Loomora (JSON in a ZIP archive).
- Architecture-independent: UTF-8 text, **explicit little-endian** binary (no "native" serialization: cf. big-endian PowerPC, §2.1.1). The writer is deterministic: the same project and options give the same bytes on one platform, and the same file structure and decompressed content on every architecture (FR-004 of spec 002; the compressed bytes of deflated entries may differ between platforms).
- The path is **stored, not recomputed** on import: the results of a computation may differ slightly from one architecture to another (§13).
- The file is changed **only by the application's edit mode**, on an explicit save (atomic: the previous file stays complete until the new one is). **Use mode** (select a result and weave) only reads it. The file holds **no weaving progress** and nothing the application changes while the person weaves: progress is **application data** outside the file, keyed by the project identifier and the result identifier (§10.2).
- When a frame, a layout, an image or a spool is modified, the results computed from it are deleted, after the person confirms. Identifiers are stable and **never reused** in a project, so application data of a deleted result can never attach itself to another one.

**Container**
- ZIP archive (PKWARE APPNOTE 6.3; methods *stored* and *deflate* only; ZIP64 when needed), extension `.loomora`, MIME type `application/vnd.loomora+zip`. First entry `mimetype`, stored, at offset 0.
- Strict reader: relative UTF-8 entry names without `..`, `.`, empty component, `\`, drive letter or control character; no duplicate name after ASCII case folding; no symbolic link, device or socket; entries that do not overlap; no encryption.
- File association in each OS (double-click, "Open with", Android / iOS share sheet, drag and drop, PWA *File Handling*).

**Geometric convention (common to the whole file)**
- Unit: millimeter. Origin: center of the support's bounding box. X to the right, Y up; Z toward the viewer (right-handed coordinate system); z = 0 for a planar support.
- **Front** view (the viewer's side of the artwork). **CW / CCW** (§6.3) are defined in this coordinate system, seen from the nail tip (in 2D, the nail axis points toward the viewer: front view). A "mirror" export is an explicit processing step, never implicit.

**Archive entries** (one folder per item, so that adding, removing or replacing an item does not rewrite the others; `<fid>`, `<lid>`, `<iid>`, `<rid>` are the decimal identifiers of the frame, layout, image and result)

| Entry | Req. | Content |
|---|---|---|
| `mimetype` | yes | `application/vnd.loomora+zip` |
| `manifest.json` | yes | See below |
| `spools.json` | yes | The spools (strings) of the project |
| `frames/<fid>/frame.json` | per frame | Frame: name, kind (`2d` / `3d`), dimensions, background color, outline and exclusion zones (type, parameters **and** resolved polyline), surface and body (§3.6.1, §3.6.2) |
| `frames/<fid>/mesh.bin` | 3D frame with a mesh | Support mesh (§3.6) |
| `frames/<fid>/mesh-source.<ext>` | no (full only) | Original mesh file |
| `frames/<fid>/layouts/<lid>/layout.json`, `nails.json` | per layout | A nail layout of the frame: its description and the list of nails |
| `images/<iid>/image.json` | per image | Image description (name, color mode, shape, dimensions) |
| `images/<iid>/source.<ext>`, `target.png` | no (full only) | Source image, processed target image |
| `images/<iid>/thumb.png` | yes | Thumbnail |
| `results/<rid>/result.json`, `sequence.bin`, `thumb.png` | per result | A result: the frame, the layout, the image or images (3D: the views, §3.6.5) and the spools it uses, its weaving sequence and its thumbnail |
| `results/<rid>/settings.json` | no (full only) | Computation parameters for reproducibility (algorithm, seed, resolution, preprocessing, engine versions); not interpreted |
| `x-<namespace>/…` | no | Unknown entries: kept and copied on rewrite |

A result refers to **one frame, one layout of that frame, one or more images and a selection of spools**; the spool numbers of its `sequence.bin` are indices into that selection.

**`manifest.json`**
- `format`: `{ "name": "loomora", "major": 1, "minor": 0 }`; `min_reader`: minimum reader version.
- `project_id` (UUID v4, stable across exports), `title`, `created_at`, `modified_at` (UTC, ISO 8601).
- `generator`: application, version, platform, architecture.
- `profile`: `full` | `light` (see below); `editable` (false for `light`).
- `counts`: number of frames, layouts, images, spools, results; `next_id`: the next identifier of each kind (it only increases); `order`: the display order of each kind.
- `entries`: for each entry except `mimetype` and `manifest.json`, the uncompressed `size` and the `sha256`.
- `required_features`: list of features the reader must handle (empty for a planar project; derived from the content: `surface_mesh`, `string_levels`, `multi_view_target`, §3.6); a reader that does not handle one rejects the file and names it.
- `extensions`: free object with keys prefixed by a namespace (ignored by readers that do not know it).

**`spools.json`** — array of `{ id, name, color, opacity, diameter_mm, available_length_mm?, material?, domain? }` (`domain`: `exterior` | `interior` | `both`, §3.6.4).

**`nails.json`** (one per layout) — array of `{ id, x, y, diameter, kind, label? }`; a 3D nail adds `z`, `axis`, `length`, `side`, `face` (§3.6.3):
- `id`: unique u32 within the layout, not necessarily contiguous;
- `kind`: `boundary` | `exclusion_boundary` | `interior`;
- `label`: number displayed on the template and in the player, if it differs from `id`;
- `axis`: unit vector base → tip (default: toward the viewer); `length`: length protruding from the surface; `side`: `exterior` | `interior`; `face`: attachment triangle and barycentric coordinates.

**`mesh.bin`** (non-flat supports, §3.6; little-endian) — header (`"LMMS"`, version, flags), vertex and triangle counts, positions (f32, mm), triangle indices (u32), optional normals and UVs, then a CRC32 per 1 MiB block so that ranges can be read and verified. `frame.json` carries the `body`, the flags (watertight, manifold, counts), the name and fingerprint of the original file and the optional parametric construction (primitive, extrusion, revolution, booleans) so that the frame can be edited; the original file may be attached as `mesh-source.<ext>`.

**`result.json`** — id, name, creation time, `frame`, `layout`, the `images` used (3D: each with its view: position, target, up, orthographic or perspective projection, size and weight, §3.6.5), the `spools` used with the string length of each, the step count, whether levels are present, the thumbnail and the settings.

**`sequence.bin`** (little-endian)
```
"LMSQ" | version u8 | header flags u8 (bit 0: levels present) | count (varint LEB128) | records… | CRC32 u32
record: nail_id (varint) | flags u8 | level u8 (if header bit 0; pass height, 0 = base, §3.6.4)
flags: bit 0      = rotation at this nail (0 = CW, 1 = CCW)
       bits 1–2   = type (0 segment, 1 start, 2 end, 3 reserved)
       bits 3–7   = spool index (0–30 into the result's spool selection; 31 = escape, followed by a varint)
```
Semantics: the records are in weaving order.
- `start`: the spool's string is attached to the nail (knot).
- `segment`: the spool's string goes from the nail of its previous record to `nail_id`, with the winding direction indicated at `nail_id`; the rotation at the starting nail is that of the previous record of this spool. The tangent type (CW→CW, CCW→CCW, CW→CCW, CCW→CW) is deduced from it.
- `end`: the string is cut / fixed at this nail.

The step list of the machine JSON (§8.3) can be reconstructed losslessly from this encoding. About 3 bytes per step (≈ 15 KB for 5,000 steps), which suits Bluetooth.

**No weaving progress in the file.** There is no `progress.json` and no field for the position, the string consumed or the bookmarks. The application keeps them (§10.2), keyed by `project_id` and the result identifier. Opening a project in use mode never changes a byte of it, nor its modification time.

**Export profiles and result subsets**
- `full`: everything, including the source and target images, the mesh source and the parameters. Only a `full` project can be edited.
- `light`: without the source and target images, `settings.json`, the mesh source and the mesh UVs, and **read-only**; used for Bluetooth and small terminals. The mesh geometry (positions, indices, normals) keeps identical values; thumbnails are kept.
- Both profiles can be exported with **a subset of the results**: the file then holds the chosen results and exactly the frames, layouts, images and spools they use; `project_id` is kept.
- The EXIF / GPS metadata of embedded images is **always stripped**.

**Versioning**
- *Minor* (added optional fields, entries or enumeration values): a reader ignores unknown fields and keeps unknown entries when rewriting.
- *Major* (any other change): an old reader rejects the file with an explicit message (`min_reader`). A recent reader can read all previous majors.

**Mandatory validation on import** (the reader reports every issue; the project is valid only without error)
- Container checks; `sha256` of every entry and CRC32 (ZIP, `sequence.bin`, `mesh.bin` blocks); JSON schemas; unique identifiers per kind and valid references between entries (a layout belongs to the frame of its result, every spool of a sequence is in the result's selection); finite numbers; nails within the frame (tolerance); nails of a sequence exist in its layout; warning if the minimum nail-to-nail distance is not respected.
- **No hard limit** on the number of elements or on the size: nothing is refused for being large. Above thresholds (starting values: 100,000 nails per layout, 5,000,000 steps per result, 5,000,000 triangles per mesh, 256 MiB decompressed, 1,000 items per kind) a **warning** is shown on opening and on saving, and never blocks. A file is refused only for an abnormal decompression ratio (a *decompression bomb*) or when an allocation or a write fails, with a defined error and nothing damaged. Reading is streamed, so memory stays bounded on 32-bit and 1–2 GB machines.
- Rejection of absolute paths, `..` and symbolic links (*zip-slip*); no executable content.
- `loomora validate <file>` command (CLI, Windows, Linux and macOS hosts), also available as a probe mode of every UI.

**Mapping to transports**
- **File**: system save / open / share; PWA: *File System Access*, download, *Web Share Target*.
- **HTTP** (§9.2): `GET /project/<token>/export.loomora?profile=light|full&results=<ids>` (resumable via `Range`); the `results` parameter selects the results to export (all by default). No route carries progress into a project file.
- **BLE** (§9.3): `light` profile of one or several results sent as a series of entries (name, size, content) with a CRC32 per block, then verification of the `sha256` of each entry against the manifest; the receiver rebuilds a local `.loomora`.
- **QR code**: contains only the pairing link, never the project.

**Tests**: reference files produced on each architecture and read back on the others (including big-endian and 32-bit), byte-identical output of the writer, lossless round trip (open, export, validate), a corpus of invalid files, reader fuzzing, and an independent checker written without the library.

### 9.2 Embedded web server **[P1]**
- The software includes a micro HTTP / WebSocket server (Rust: `axum`/`tokio`), with no external dependency; its implementation is part of Loomora.
- It can also run **headless on a server** (CLI binary) to compute remotely.
- **QR code pairing**: URL `http://<IP>:<PORT>/project/<TOKEN>`; the phone scans it and loads the project (all of it, or the results the person chooses: `export.loomora?results=…`, §9.1) into its persistent local cache. mDNS / Zeroconf discovery.
- **Bidirectional** synchronization: weaving progress is reported in real time from the phone to the PC (WebSocket). Progress is **application data** keyed by the project identifier and the result identifier, never part of the `.loomora` file; its messages and its merge rules are specified with the application data, not with the file format.
- Security: limited-use access token, listening on the local network by default, optional TLS for remote access.
- **Privacy**: remote computation is an explicit option. The image is sent only to a server chosen and operated by the user, with their consent, never to a third-party service by default.

### 9.3 Bluetooth Low Energy **[P1]**
For workshops without a local network or for direct machine control. `btleplug` on desktop; native bridges CoreBluetooth (iOS/macOS) and Android BLE.

Custom GATT profile (proprietary 128-bit UUIDs to be generated; **do not** use the reserved 16-bit SIG space):
- **Metadata** (Read): project ID, the results of the project (identifier, number of nails of its layout, number of steps, spool colors).
- **Sequence transfer** (Write / Notify): the `light` profile of the chosen result or results, as blocks fragmented according to the negotiated MTU, **CRC32 per block**, resume on missing block.
- **Tracking / control** (Read / Write / Notify): project and result identifiers, current step, play/pause, winding direction, confirmation acknowledgments. The step is application data (§10.2), not written into the project.

---

## 10. Weaving workshop (Player)

### 10.1 Display and scrolling modes **[P1]**
Full-screen interface for smartphone / tablet, large type: current nail, next nail, winding direction (projections of §6.3), active spool, progress.

Three scrolling modes:
1. **Manual**: touch, keyboard keys (arrows, space), Android / iOS volume buttons.
2. **Automatic**: adjustable pace (e.g. 1 instruction every 3 to 10 s) with an optional pace beep.
3. **Voice (hands-free)**: keywords "Next", "Repeat", "Previous"; speech synthesis (TTS) in the application language ("Nail 84 on the right"). Recognition and synthesis go **exclusively through the system APIs** (no embedded engine). If the API is absent or unavailable offline, recognition is disabled and the application falls back to manual / automatic; synthesis alone may remain.

**Step back** in case of a handling error, in all modes.

### 10.2 Strict progress persistence **[P0]**
- **Synchronous and transactional** write (SQLite via rusqlite) at each nail crossing.
- Data: project ID, **result ID**, index of the active instruction, current spool, string length consumed, bookmarks, scrolling mode and display preferences. The progress is **application data**: it lives in the application's database, keyed by the project identifier (`project_id`) and the result identifier of the manifest, and is **never written into the `.loomora` file** (§9.1); opening a project and weaving never changes it. Each result of a project has its own progress.
- Identifiers of deleted results are never reused (§9.1): the progress of a result that no longer exists in the file can be detected and never attaches itself to another result.
- Abrupt shutdown, memory purge, battery shutdown: reopening on the last validated step, without loss.
- The image computation is not saved (§2.4).

### 10.3 Resynchronization / unweaving **[P1]**
When the user no longer knows where they are:

1. They activate "I am lost". The application asks them to **undo the string of the last nail touched** and to enter its number.
2. If this nail appears only once in the recent window of instructions, the position is realigned.
3. Otherwise (frequent ambiguity in dense areas), it asks them to undo a 2nd nail, then a 3rd if necessary.
4. Search for the **reverse** subsequence `(… Z → Y → X)` in the instruction list to find the step index unambiguously; the CW / CCW direction may be requested to resolve a residual ambiguity.
5. Safe resume at step N−k.

**Multi-spool:**
- Sequential mode: the search is restricted to the instruction range of the current color.
- Interleaved mode: the user indicates the color of the string being undone; the algorithm filters the corresponding subsequence and **warns if another spool was crossed or blocked on top**.

---

## 11. Internationalization **[P0 for the architecture]**

The application must be translatable into **any language**.
- Decoupled format: Project Fluent (`fluent-rs`) or JSON / gettext PO; complex plurals and genders handled.
- Language files (`fr.ftl`, `en.ftl`, `es.ftl`, `de.ftl`, `zh.ftl`, `ar.ftl`…) in the assets, extensible by the community **without recompilation**.
- **Import of a custom language pack** from the settings.
- Languages shipped at a minimum: French, English.
- **Single source of texts**: the strings come from the core (`loomora-i18n`) for all native UIs, with no translation resources specific to each platform, so that language packs (community or imported) work everywhere without recompilation. Only the labels required by the stores and the systems (application name, permission texts) are translated in the native resources.
- **RTL**: dynamic layout mirroring (Arabic, Hebrew, Farsi).
- **Fonts** with full Unicode coverage (Noto Sans): Latin, Cyrillic, Greek, Arabic, Devanagari, CJK, technical symbols.
- **Multilingual TTS**: synchronizes with the chosen language and selects the matching system voice.
- The direction instructions (§6.3) are localized (dial, right/left).

---

## 12. Interface and user flow

```
1. Preparation          2. Computation        3. Fabrication         4. Tracking
 - Image + cropping →    - CPU/GPU engine  →   - PDF/CNC export  →   - Player
 - Shape / exclusions    - Algorithm           - Printing             - Persistence
 - Nails (auto/import)   - Live preview        - BLE/QR transfer      - Unweaving
```

Main editing screen ("split screen"):
- **Left**: source image and processed target image.
- **Center**: dynamic real-time visualization of the path, layer-by-layer rendering simulation, residual contrast gauge.
- **Right**: parameter sliders (number of nails, number of lines, string transparency / diameter, contrast…).

On small screens, the panels become tabs. Accessibility: dynamic font sizes, native screen readers of each platform, themes (§12.1).

### 12.1 Display themes **[P1]**

All UIs offer three themes: **light**, **dark** and **high contrast**, along with the "follow the system" option (default).
- Semantic **design tokens** (colors, spacing, radii, font sizes) defined once in a source file, then generated for each platform (Compose, SwiftUI, XAML, GTK CSS, web CSS).
- **Light and dark**: text contrast ≥ 4.5:1 and UI elements ≥ 3:1 (WCAG AA).
- **High contrast**: text ≥ 7:1 (AAA), UI elements and focus indicators ≥ 4.5:1, crisp borders, no information conveyed by transparency or gradient. Automatic activation if the system requests high contrast (Windows contrast themes, iOS / macOS "Increase Contrast", Android high-contrast text, GTK HighContrast theme), with manual choice possible.
- **Never color alone**: CW / CCW direction, spools and states are also indicated by a pictogram or text.
- **Theme-independent preview**: the support background and the string colors are those of the project (§3.5, §7); only the frame around the preview follows the theme.
- **Player**: optional pure black background in dark mode (OLED screens, dimly lit workshop), large fonts.
- **PDF and printing**: always black on white, independent of the theme.
- The choice is remembered. CI: automatic verification of the contrast ratios on the tokens and screenshots of each screen in the three themes.

---

## 13. Non-functional requirements

- **100% offline** by default: no image or data is sent to a third party; GDPR compliance. Network exchanges (§9) are local or to a server chosen by the user.
- **Performance**: §5.3; UI fluidity (computation off the UI thread).
- **Memory**: §2.3 and §6.1.
- **Reliability**: weaving persistence (§10.2), atomic writing of projects.
- **Portability**: §2.1; equivalent results across platforms (non-regression tests, fixed seed). Bit-for-bit identity is **not** guaranteed (FMA, SIMD, order of floating-point operations); this is why the path is stored in the file and not recomputed (§9.1).
- **Technical validation**: each choice is validated by the phase 0 PoC (§15.1); the PoC is kept as a non-regression test on the old platforms.
- **Quality**: unit tests (geometry, tangents, resynchronization), CPU/GPU comparison, CLI tool for batch processing and tests.
- **Licenses**: compliance with the licenses of the dependencies and fonts.

---

## 14. Software architecture (modules)

There is no Cargo workspace (ADR 0001, ADR 0010). The backend is one C++11 library (`core/`, the static library `libloomora.a`, built with CMake) behind a handwritten C API; each UI is a separate project under `ui/legacy/<platform>` or `ui/modern/<platform>` and holds no logic; glue without logic (JNI, Emscripten exports) is in `bridge/`, toolchain files in `cmake/toolchains/`, host tools in `tools/`. The tree below is the functional breakdown still wanted for the backend: each `loomora-*` entry becomes a module of `core/`, or a library it links, when its feature is built. Today the backend only computes the probe (greeting, end-of-life test, probe record).

```
loomora/
├── crates/
│   ├── loomora-core/         # Geometry (2D and 3D: §3.6), shapes, masks, CW/CCW tangents, data models (no_std friendly)
│   ├── loomora-mesh/         # Meshes: import (STL, OBJ…), checking, repair, BVH, interior / exterior, primitives, extrusion, revolution, booleans
│   ├── loomora-render3d/     # 3D view (wgpu) and software fallback
│   ├── loomora-engine-cpu/   # CPU algorithms (rayon, SIMD): greedy, LSQR/NNLS, metaheuristics, fills
│   ├── loomora-engine-gpu/   # Compute shaders (wgpu, WGSL, naga)
│   ├── loomora-machine/      # G-code, SVG, DXF, machine JSON/CSV, nail export
│   ├── loomora-pdf/          # 1:1 PDF templates, tiling, data sheet, printing
│   ├── loomora-net/          # HTTP/WebSocket server (axum, tokio), QR, BLE (btleplug)
│   ├── loomora-storage/      # Transactional persistence (SQLite via rusqlite)
│   ├── loomora-app/          # States, use cases, view-models, UI-independent; system service interfaces
│   ├── loomora-ffi/          # Glue without logic: JNI (Android), `LibraryImport` declarations (C#); today `bridge/` and the UI projects
│   ├── loomora-capi/         # Handwritten stable C API `loomora.h` (C89), the only boundary; today `core/include/`
│   ├── loomora-wasm/         # WebAssembly and JavaScript exports for the PWA (Emscripten); today `bridge/wasm/`
│   └── loomora-i18n/         # fluent, unic-langid, language packs
├── ui/
│   ├── legacy/               # windows-win32, linux-xlib, android-views, web (macOS, iOS: Objective-C, not built)
│   └── modern/               # windows-winui3, linux-gtk3 (gtkmm), android-compose, web-ts (macOS, iOS: SwiftUI, not built)
└── cli/                      # Batch computation, headless server, tests (not built)
```

Indicative libraries (Rust crate names, historic: see the reading note on the backend language): `nalgebra` or `glam`, `geo`, `lyon_geom`, `image`, `rayon`, `wgpu`, `naga`, `axum`, `tokio`, `btleplug`, `rusqlite`, `fluent`, `uniffi`, `wasm-bindgen`, `gtk4`, `parry3d` (BVH, intersections), `stl_io`, `tobj`, `ply-rs`, `gltf`.

---

## 15. Planning and provisional milestones

0. **Phase 0 — Technical validation and PoC [blocking]**: verify each technical choice against the support requirements for old systems, through an end-to-end PoC on all targets (§15.1). No subsequent phase starts before milestone G0.
1. **Phase 1 — Proof of concept**: greedy algorithm in CLI on a 200-nail circle; performance measurements; geometry types and 3D-ready interfaces (§3.6.1).
2. **Phase 2 — MVP**: interface on the pilot platforms (§16.2), live preview, parameters, list export, 1:1 PDF template, tangents and winding direction.
3. **Phase 3 — Workshop**: manual / automatic / voice player, persistence, unweaving; deployment of the UIs on all target platforms; light / dark / high-contrast themes.
4. **Phase 4 — Advanced geometries**: predefined shapes, nested shapes, interior nails and automatic placement, nail import, vector mode.
5. **Phase 5 — Advanced engines**: GPU backend, LSQR/NNLS, metaheuristics.
6. **Phase 6 — Exchange and machines**: server, QR, BLE, CNC / G-code exports, PDF tiling.
7. **Phase 7 — Multi-color, full i18n, PWA**: multiple spools, language packs, RTL; PWA / WebAssembly delivery; tier B and C architectures; Win32 Legacy UI (Windows 2000 and later).
8. **Phase 8 — Non-flat supports (3D)**: mesh import and primitives, checking and repair; 3D nails; string domain and admissibility (BVH); 3D tangents and pass levels; multi-view projection; 3D view; 3D player; exports and templates.

### 15.1 Phase 0 — Technical validation and PoC

**Objective**: confirm, through experiment, that the technical choices (Rust, FFI, native UIs, GPU, SQLite, server, Bluetooth, voice, WebAssembly, 3D…) hold up on the **oldest targeted systems**, and decide, before any substantial development, what is kept, restricted or abandoned.

#### 15.1.1 Method
1. **Inventory**: each technical choice in the document is a hypothesis to validate, listed in the matrix of §15.1.4 with a measurable success criterion and a fallback.
2. **Test bench** (§15.1.2): reference environments, old and recent.
3. **Single end-to-end PoC** (§15.1.3), run identically on each target.
4. **Measurements**: build, execution, time, memory, results compared with reference data.
5. **Decision**: each hypothesis is classified **OK**, **OK with restrictions**, **KO**; the KOs trigger the planned fallback or a revision of the document.
6. **Traceability**: a decision log (ADR) and a validation report are versioned; the requirements document is updated (tiers A / B / C of §2.1.1, decisions of §16, disabled features of §2.3).

#### 15.1.2 Reference test bench

| Family | Environments |
|---|---|
| Android | API 21 emulator limited to 1 GB of RAM (armv7 and x86); one old, low-end real device; devices of the intermediate APIs (26 to 28, 34) and of the most recent level |
| iOS | One real device on iOS 15; one recent device |
| Windows | Virtual machine and real machine on **Windows XP SP3** (x86); Windows 10 x64; Windows 11 ARM64 |
| macOS | macOS 11 (Intel) and a recent Apple Silicon Mac |
| Linux | Ubuntu 20.04 and Debian 11 (x64); ARM 32-bit ARMv6 and ARMv7 (Raspberry Pi type); AArch64; PowerPC 32-bit and 64-bit big-endian (QEMU emulation, real hardware if available) |
| Web | Recent and old Chrome, Firefox and Safari; iOS Safari |
| Peripherals | A real printer (1:1 scale, tiling); a Bluetooth Low Energy device; a local network |

#### 15.1.3 "Hello Loomora" PoC (identical on all targets)
The PoC exercises, on each target, the whole technical chain with fixed reference data:
1. Load an embedded test image; compute with the **greedy** algorithm a 1,000-line path on 200 nails (circle), **CPU** engine; on the targets that allow it, repeat with the **GPU** engine and compare.
2. Write then read back a **`.loomora`** file (§9.1); compare byte by byte with the reference file produced on another architecture, including a **big-endian** one.
3. Save the progress in **SQLite** at each step, then kill the process abruptly and verify the resume.
4. Display the path in the platform's **native view** (5,000 segments) with the three themes (light, dark, high contrast).
5. Speak "Nail 84, clockwise" with the **system speech synthesis**; attempt recognition of "Next" (or cleanly note its absence).
6. Generate a **1:1 PDF template** and print it; measure the scale with calipers.
7. Transfer the file between two devices by **local HTTP** (QR) and by **Bluetooth Low Energy** where available.
8. For 3D: load a 100,000-triangle mesh, display the 3D view, compute the admissibility matrix for 500 nails.
9. Display Arabic, Hindi and Chinese text, and switch to RTL.

Each step produces a success or failure result and measurements (time, peak memory, binary size).

**Phase 0 as run (specification 001, 2026-11-12 to 2026-11-15).** The PoC was reduced to a deliberately minimal probe: a text field and the greeting `Bonjour ` + text computed by the backend. Steps 1 to 9 above were not run, and the hypotheses that need them stay "not covered". Scope rule (owner decision 2026-11-15): the Legacy UI is validated on the lowest testable system of each family, the Modern UI on one system and one 64-bit processor; success there stands for the range, rows not run stay `not_tested` and are not failures. macOS and iOS are pending (no trial). The test bench of §15.1.2 was reduced accordingly: a Windows 2000 build 2195 virtual machine (instead of XP SP3), Windows 11 x64, Android API 16 (Legacy) and API 24 (Modern) emulators, the CentOS 5.11 userland (manylinux1 image) and Ubuntu 24.04, Firefox 10.0 on Windows 2000 and Chrome 157.

#### 15.1.4 Matrix of hypotheses to validate

| # | Choice to validate | Old targets concerned | Success criterion | Fallback on failure |
|---|---|---|---|---|
| V1 | **Rust toolchain** | Windows XP, Android API 21 (ARMv7, x86), Linux ARMv6 / ARMv7, PowerPC 32 / 64-bit, macOS 11, iOS 15 | Build and execution of the PoC and of the core tests | XP: version abandoned or thin client; others: downgrade to tier C or target withdrawal |
| V2 | **CPU engine performance** | API 21 (1 GB), ARM 32-bit, PowerPC, XP | §5.3 target reached on a modern machine; time measured and no memory overrun on the old ones | Resolution at 0.5 px, fewer lines, offloaded computation (server or file) |
| V3 | **Memory on 32-bit** | XP, ARM 32-bit, API 21 | Peak under the budget (e.g. 512 MB out of 1 GB) at standard and reduced resolution | Reduced resolution, tile processing |
| V4 | **Cross-architecture consistency** | All, including big-endian | `.loomora` read back identically; equivalent paths within the documented tolerance | The file is authoritative; wider tolerance |
| V5 | **GPU backend (`wgpu`)** | API 21–23 (OpenGL ES), API ≥ 24 (Vulkan), Windows ARM64 (DirectX 12), macOS / iOS (Metal), Linux | Reliable detection; compute shader executed; result equivalent to the CPU | CPU only on the target, automatic disabling |
| V6 | **FFI bridges** | Kotlin (UniFFI, API 21), Swift (iOS 15), C# (WinUI 3 x86 / x64 / ARM64), C (`cbindgen`, MFC / XP), Rust (GTK), WebAssembly | Round-trip calls with large volumes, callbacks, errors and threads; acceptable binary size | Hand-written JNI and C API; UniFFI abandoned on the target |
| V7 | **Android API 21 UI** | Jetpack Compose or Views | Smooth 5,000-segment path on 1 GB; themes; RTL | Views; simplified rendering |
| V8 | **iOS 15 UI** | SwiftUI | Same; light, dark and contrast themes | Occasional UIKit |
| V9 | **Windows UI** | WinUI 3 (minimum Windows version to be set); MFC on XP (`v141_xp`) | Preview screen, list, dialog boxes, printing; themes drawn on XP | MFC only, or abandon XP |
| V10 | **Linux UI** | GTK 4 versus Slint on Ubuntu 20.04, Debian 11, ARM 32-bit; Flatpak and AppImage | Starts; Arabic, CJK and RTL correct; high contrast | Slint, or tier C (web UI) |
| V11 | **SQLite persistence** | All, including XP, ARM 32-bit and big-endian | Synchronous write at each nail; survives abrupt shutdown and power cut; acceptable latency on slow storage | Home-made append-only journal |
| V12 | **Embedded server and QR** | XP (without `tokio`), Android client (cleartext HTTP, API ≥ 28), iOS (local network) | PC → phone transfer with resume (`Range`) | File transfer only |
| V13 | **Bluetooth Low Energy** | Android API 21, iOS 15, Windows 10, macOS 11, Linux (BlueZ); XP excluded | Transfer of 15 to 40 KB with CRC, negotiated MTU and resume | BLE removed; file and HTTP |
| V14 | **System voice** | Android API 21, iOS 15, Windows (SAPI from XP to 11), macOS, Linux | Multilingual synthesis and recognition of 3 words; absence handled by clean disabling | Synthesis only |
| V15 | **i18n, RTL and fonts** | All native UIs, XP | Arabic, Hindi and Chinese text rendered correctly; RTL; language pack imported without recompiling; acceptable application size | System fonts; per-platform packs |
| V16 | **Printing and PDF** | XP (GDI), Android Print, AirPrint, CUPS | 1:1 template at the measured scale; correct tiling | PDF export only |
| V17 | **PWA / WebAssembly** | Recent and old browsers, iOS Safari | Working Wasm engine (threads, SIMD); acceptable performance; OPFS storage; memory limits respected | Single-threaded Wasm; offloaded computation |
| V18 | **3D** | BVH on 32-bit, API 21, XP; `wgpu` surfaces in `SurfaceView`, `SwapChainPanel`, `GLArea` | 100,000 triangles and 500 nails: admissibility matrix in acceptable time; smooth 3D view | Heavy simplification; software wireframe view |
| V19 | **Themes and contrast** | All | Contrast ratios verified automatically; system high contrast detected | Manual choice |
| V20 | **Distribution and signing** | Play (16 KB pages, `targetSdk`), App Store, macOS notarization, Flatpak / AppImage, XP installer and accepted signature | One installable package per platform | Packaging adaptation |

Reading after the probe (rows keep their numbers; the text as worded is also in `validation/hypotheses.json`): V6, V7, V9, V10, V15, V17 and V19 are *OK with restrictions* (partly covered) for the part the probe exercised, the other thirteen are *not covered by this probe*. V1 as worded is void (Rust is abandoned, ADR 0001): it is read as the C++11 toolchain (spikes S1 and S6). In V6 the bridges are the handwritten C API (header, JNI, `LibraryImport`, Emscripten exports), not UniFFI or `cbindgen`; in V9 the Windows legacy UI is Win32, not MFC; in V10 the Linux UIs are gtkmm on GTK 3.10 (GTK 4 an option) and Xlib, not GTK 4 against Slint; "XP" in the Windows entries is read as Windows 2000, the floor validated (XP is covered by inference).

#### 15.1.5 Milestone G0 and deliverables
- **Deliverables**: validation report (matrix of §15.1.4 filled in: OK, OK with restrictions, KO), decision log (ADR), versioned reference dataset, reproducible test bench, PoC kept as a non-regression baseline.
- **Update of the requirements document**: tiers A / B / C (§2.1.1) reclassified, decisions of §16 confirmed or revised, features disabled per platform (§2.3) specified.
- **Milestone G0 (move to phase 1)**: all hypotheses for the tier A platforms are validated, and the go / no-go decision is made for **Windows XP (V1, V9)**. The tier B and C targets can continue in parallel, their decision having to be made before phase 7.
- **Status of G0 (2026-11-15)**: each of the 20 hypotheses is now covered, partly covered, or explicitly marked not covered by the owner. Partly covered (OK with restrictions): V6, V7, V9, V10, V15, V17, V19. Not covered because the feature is not implemented yet in the probe, to be validated when it is built: V2, V3, V4, V5, V11, V12, V13, V14, V16, V18, V20. Not applicable: V1 (Rust dropped, ADR 0001; the C++11 toolchain is covered by V6, V9, V10 and the spikes). Not covered, pending Apple hardware: V8. G0 itself is not declared reached: that stays the owner's decision.
- **Go / no-go for the old Windows systems** (2026-11-15): Windows 2000 is *go* (OK with restrictions, verdict `windows-2000-x86-legacy`); Windows XP is *go by inference*, no XP system was run (spec Clarifications 2026-11-15).

---

## 16. Decisions

### 16.1 Settled

These decisions remain **provisional** until they are validated by the phase 0 PoC (§15.1).

| # | Item | Decision |
|---|---|---|
| 1 | UI | **Native UI per platform**, two per platform (Legacy and Modern, ADR 0008): Kotlin and Java views (Android), SwiftUI and Objective-C (iOS, iPadOS, macOS; not built), PWA / WebAssembly (web); Windows: WinUI 3 + Win32 (no. 8); Linux: see no. 9. **Tauri ruled out** (§2.2) |
| 2 | Minimum iOS | **iOS 15.0**, with possible functional restrictions on missing APIs (the spec 001 target iOS 5 is untried until a trial; owner decision 2026-11-15) |
| 3 | Persistence | **SQLite (`rusqlite`, embedded SQLite)** — chosen by default, the user having no preference: transactional, portable across 32 / 64-bit and little / big-endian, mature tooling. `redb` ruled out (less proven on exotic architectures) |
| 4 | Speech recognition | **System API only**; feature disabled if it is absent (§10.1) |
| 5 | `.loomora` format | Validated; specified in §9.1 (rewritten 2026-11-17 from spec 002): a **project** of several frames (2D and 3D), layouts, images, spools and results; **no weaving progress in the file** (application data, §10.2); **no hard limit** on elements or size (warnings above thresholds, refusal only for an abnormal decompression ratio or a failed allocation or write); `light` and `full` profiles; export of a subset of results |
| 6 | PWA / WebAssembly | **In scope** (P2) |
| 7 | Architectures | **As many as possible**, including Linux ARM 32-bit and PowerPC; tiers A / B / C (§2.1.1). PowerPC, MIPS, Itanium, Linux AArch64, ARM 32-bit and RISC-V are outside the probe scope (not built or tested by the phase 0 probe) |
| 8 | Windows UI | **Two UIs**: **WinUI 3** (Windows 10 version 1809+, x86 / x64 / ARM64) and plain **Win32** in C++11 (Windows 2000 and later, x86 / x64); **MFC dropped** (ADR 0009); §2.1.2 |
| 9 | Linux desktop UI | **Two UIs**: Modern = GTK 3 with gtkmm (C++, CMake), GTK 3.10 API as the floor, GTK 4 with gtkmm-4.0 as a build option, each newer feature behind a compile-time macro; Legacy = Xlib with Xft (C++11, CMake). Plain executables built on the machine that runs them, no Flatpak, no AppImage, no libadwaita (owner decisions 2026-11-12 and 2026-11-14; ADR 0005, 0006, 0007, 0010). Slint was not tried |
| 13 | Android UI | **Two UIs**: Legacy in Java views (API 9 to 23), Modern in Kotlin and Jetpack Compose (API 24+, the pinned Compose and AndroidX libraries need API 23, spike S8); ADR 0008, ADR 0016 |
| 14 | Rust toolchain for Windows XP | **Moot**: Rust is abandoned (ADR 0001). A C++11 Win32 executable built with MinGW-w64 runs on Windows 2000 build 2195 (spike S1; SP4 not run, XP by inference) |

### 16.2 Still open

Items 9, 13 and 14 were settled on 2026-11-15 and moved to §16.1 (Annex B); numbers are not reused.

| # | Item | Remark |
|---|---|---|
| 10 | Winding direction displayed by default | Relative, dial or CW / CCW |
| 11 | License and business model | Open source, free, paid; affects the stores, the dependencies and third-party code |
| 12 | Pilot platforms of the MVP | Suggestion: Android + WinUI 3 |

---

## 17. Planned evolutions (out of current scope)

| Evolution | Detail | Priority |
|---|---|---|
| String stuck to the surface (geodesic) | On convex bodies, the string follows the surface instead of forming chords | P3 |
| 4- to 5-axis machine exports | G-code with head orientation, for inclined nails | P3 |
| 3D-printable drilling template | STL guide with the holes at the positions and inclinations of the nails | P3 |
| Relief from an image | Height map; UV coordinates of an imported mesh | P3 |
| Reconstruction from a point cloud | Without a provided mesh | P3 |
| Vector modes (§5.1) on developable surfaces | After unfolding the surface | P3 |
| Direct CAD import (STEP, IGES) | Today: prior conversion to a mesh | P3 |

---

## Annex A — Request traceability

| User request | Sections |
|---|---|
| Initial requirements document: greedy algorithm, player, exports, planning | §5, §10, §8, §15 |
| Windows, Linux, macOS, Android, iPhone, old phones supported by the stores | §2.1, §2.3 |
| More shapes, import of an x/y table of nails | §3.2, §3.3 |
| Automatic nail placement, including inside | §3.4 |
| Several path computation modes | §5 |
| Solid black/white shapes: choice of the fill pattern | §5.1 |
| Photos: greedy or equivalent; least squares; generic algorithms | §5.2 |
| Right / left for each nail | §6.3 |
| Account for the string size and the nail position and size | §6.2 |
| Resolution according to the string size, recommendation in pixels | §6.1 |
| PDF of the nail positions, enlargement factor, direct printing | §8.1 |
| Multi-string: sequential vs. optimal color at each step | §7.2 |
| API 21; disable functions if needed | §2.1, §2.3 |
| CPU and GPU version; per-platform reimplementation possible | §2.2, §2.4 |
| Export the nail list for digital machines | §8.3 |
| Knowing where one is (undoing strings, deducing the position); multi-spool | §10.3 |
| No saving of the computation; manual / auto / voice scrolling; systematic saving | §2.4, §10.1, §10.2 |
| Windows ARM64; Rust and not C++ | §2.1, §2.2, §14 |
| Shape within a shape, interior free of string | §3.1 |
| Original default shapes (heart, drop, star…) | §3.2 |
| Import a path computed elsewhere, web server included, Bluetooth | §9 |
| Passing direction beyond right/left (up/down) | §6.3 |
| Translation into any language | §11 |
| Native interface per platform (Kotlin, SwiftUI, PWA / WebAssembly, desktop) | §2.2, §14, §16 |
| Light, dark, high-contrast themes | §12.1 |
| Non-flat supports: concave and convex shapes, 3D nails (interior / exterior), interior / exterior string, mesh input | §3.6, §9.1, §17 |
| Windows MFC version compatible with XP + WinUI 3 version | §2.1.2, §2.2.1, §16 |
| Verify the technical choices and build a PoC against the requirements of old systems | §15.1, §16 |

Note (2026-11-15): the requests "Rust and not C++" and "Windows MFC version compatible with XP" were superseded by the owner decisions of the spec Clarifications of 2026-11-12 (C++11 backend, plain Win32 instead of MFC); the rows keep tracing the original requests.

---

## Annex B — Revision history

| Date | Version | Change | Source |
|---|---|---|---|
| 2026-11-15 | V6.5 | Backend language: Rust, Cargo, `cbindgen` and UniFFI abandoned, one C++11 backend behind a handwritten C API; reading note, §2.1 column "Backend toolchain", §2.1.1 first sentence and 32-bit constraint, §2.2, §2.2.1, §14, §15.1.4 reading | Spec Clarifications 2026-11-12 (second session); ADR 0001, ADR 0011; spike S6 |
| 2026-11-15 | V6.5 | Windows legacy: Windows XP SP3 and MFC replaced by Windows 2000 (validated build 2195, no service pack) and plain Win32 built with MinGW-w64; XP by inference; end-of-life warning; §2.1, §2.1.2, §2.2.1, §3.6.9, §14, §15 phase 7, §15.1.5, §16.1 no. 1, 8 and 14 | Spec Clarifications 2026-11-12 (third session) and 2026-11-15; ADR 0009, ADR 0012; verdict `windows-2000-x86-legacy`; spike S1; report section 2 |
| 2026-11-15 | V6.5 | Windows modern: Windows 10 raised to version 1809, framework-dependent runtime requirement; §2.1, §2.2.1, §16.1 no. 8 | Research R6; deliverable `windows-11-x64-modern`; verdict `windows-11-x64-modern`; spike S8 |
| 2026-11-15 | V6.5 | Linux: GTK 4 with `gtk-rs`, AppImage and Flatpak replaced by gtkmm on GTK 3.10 (Modern) and Xlib (Legacy), binaries built on the target machine, lowest userland tried CentOS 5.11; §2.1, §2.2.1, §16.1 no. 9 | Spec Clarifications 2026-11-12 and 2026-11-14; ADR 0005, 0006, 0007, 0010; verdicts `linux-centos-5.11-x86_64-legacy`, `linux-ubuntu-24.04-x86_64-modern` |
| 2026-11-15 | V6.5 | Android: Legacy UI (API 9 to 23, NDK r15c, run on API 16) and Modern UI (API 24+, Kotlin and Compose) instead of minSdk 21; Compose or Views settled; §2.1, §2.2.1, §16.1 no. 13 | ADR 0008, ADR 0016; spikes S3 and S8; verdicts `android-4.1.2-x86-legacy`, `android-7.0-x86_64-modern` |
| 2026-11-15 | V6.5 | Web: WebAssembly path and JavaScript path (Emscripten 1.39.20 fastcomp), Firefox 10.0 on Windows 2000 as the Legacy floor, Internet Explorer up to 9 dropped, Safari covered by inference, Modern UI validated on one current browser; §2.1, §2.2.1 | Spec Clarifications 2026-11-12; ADR 0017; spike S5; verdicts `web-firefox-10-windows-2000-x86-legacy`, `web-chrome-157-windows-11-x64-modern` |
| 2026-11-15 | V6.5 | Tiers: no longer defined by Rust target availability, but by a C++11 compiler and the platform libraries; Rust triples removed from §2.1.1 | Spec Clarifications 2026-11-12 (second session); report section 10 |
| 2026-11-15 | V6.5 | CI on GitHub-hosted runners; the targets they cannot run are local runs; §2.1.1 | ADR 0018 |
| 2026-11-15 | V6.5 | Disabled features recorded by the trials added to §2.3 (Windows 2000: DPI awareness, dark theme; Linux Legacy: missing glyph, text shaping and bidi, glyph substitution; Android Modern: `uimode-contrast`; Web Legacy: peak memory) | Verdicts `windows-2000-x86-legacy`, `linux-*-legacy`, `android-7.0-x86_64-modern`, `web-firefox-10-windows-2000-x86-legacy`; report section 10 |
| 2026-11-15 | V6.5 | Phase 0 as run (minimal probe, PoC scope rule, reduced test bench), reading of the hypotheses matrix, go / no-go for Windows 2000 and XP; §15.1.3, §15.1.4, §15.1.5 | Spec Clarifications 2026-11-15 (fourth session) and Assumptions; report sections 2 and 9 |
| 2026-11-15 | V6.5 | Decisions of §16: no. 1 and 8 updated; no. 9, 13 and 14 settled and moved from §16.2 to §16.1; requirement identifiers and rows V1 to V20 unchanged | As the rows above |
| 2026-11-15 | V6.5 | macOS 11 and iOS 15 kept as the floors until a trial; the spec 001 targets macOS 10.6 and iOS 5 noted as untried (§2.1, §16.1 no. 2) | Owner decision 2026-11-15 |
| 2026-11-15 | V6.5 | PowerPC, MIPS, Itanium, Linux AArch64, ARM 32-bit and RISC-V kept and marked "outside the probe scope" (§2.1, §2.1.1, §16.1 no. 7) | Owner decision 2026-11-15 |
| 2026-11-15 | V6.5 | Tier A noted as not confirmed for any platform as a packaged application (§2.1.1) | Owner decision 2026-11-15; V20, validation report section 10 |
| 2026-11-15 | V6.5 | Win32 Legacy path drawing: plain GDI required, GDI+ optional and skipped where absent (§2.1.2) | Owner decision 2026-11-15 |
| 2026-11-15 | V6.5 | Milestone G0 left pending the owner's decision on each of the 15 uncovered hypotheses (§15.1.5); Rust-named library roles left to the reading note | Owner decision 2026-11-15 |
| 2026-11-15 | V6.5 | Status of G0 and the reading of the hypotheses matrix updated: V7 and V17 partly covered; V2 to V5, V11 to V14, V16, V18 and V20 not covered because not implemented yet; V1 not applicable; V8 pending Apple hardware (§15.1.4, §15.1.5) | Owner clarification 2026-11-15; `validation/hypotheses.json`; verdicts `android-4.1.2-x86-legacy`, `android-7.0-x86_64-modern`, `web-chrome-157-windows-11-x64-modern`, `web-firefox-10-windows-2000-x86-legacy` |
| 2026-11-17 | V6.5 | macOS Legacy floor moved from 10.6 to **10.7 Lion**: C++11 needs libc++, which the clang of Xcode 4.6.3 refuses below 10.7; Xcode 5.1.1 compiles the backend for 10.7 (macOS 10.6 stays untried, §2.1, §16.1 no. 2) | Owner decision 2026-11-17; `docs/implementation/validation/procedures/macos.md` M.11 | Compiled, not run |
| 2026-11-17 | V6.5 | iOS 5.0 Legacy target compiled with Xcode 5.1.1 (iOS 7.1 SDK, deployment 5.0, armv7 and armv7s); still not run on an iOS 5 device (§2.1, §16.1 no. 2) | Owner request 2026-11-17; procedure M.11 | Compiled, not run |
| 2026-11-17 | V6.6 | `.loomora` format: a project holds several frames, nail layouts, images, spools and results (one folder per item) instead of one support, one nail list and one sequence; `progress.json` and its merge rules removed, progress becomes application data keyed by project and result identifiers; hard limits (256 MB, 100,000 nails, 5,000,000 steps and triangles) replaced by warning thresholds and a decompression-ratio guard; `light` profile rules and result subsets; `GET export.loomora` selects results, no progress route; BLE metadata and transfer per result; §9.1, §9.2, §9.3, §10.2, §16.1 no. 5 | Spec 002 Clarifications 2026-11-17; `docs/implementation/specs/002-loomora-file-format/contracts/project-format.md` |
