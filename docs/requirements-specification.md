# Loomora — Functional and Technical Requirements Document (V6, consolidated)

Universal String Art software (the art of stretched string).

| | |
|---|---|
| Version | V6.4 — consolidation of versions V1 to V5 (Gemini export "String Art Requirements Document") + the decisions of §16.1; native interface per platform, themes, non-flat supports (3D, meshes) |
| Date | 2026-11-11 |
| Status | Draft to be validated; **all technical choices are provisional until the phase 0 PoC (§15.1)**; points still open in §16.2 |
| Convention | **[P0]** essential · **[P1]** major · **[P2]** optional / V2 of the product. Unmarked: P1. |

Reading notes:
- The words "must" / "may" are normative: *must* = requirement, *may* = option.
- Items marked **[Proposal]** were not explicitly requested; they fill a gap noticed during the consolidation and are to be validated.
- Annex A traces each user request to its section.

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

| Platform | Architecture | Rust target | Minimum / constraints |
|---|---|---|---|
| Windows | x86_64 | `x86_64-pc-windows-msvc` | Windows 10+; WinUI 3 UI |
| Windows | **ARM64** | `aarch64-pc-windows-msvc` | Full native support (Surface, Copilot+, Snapdragon X); WinUI 3 UI |
| Windows **legacy** | x86 (x64 optional) | `i686-pc-windows-msvc` or `-gnu`, XP-adapted toolchain (unofficial, §2.1.2) | **Windows XP SP3 minimum**; **MFC** UI; restricted features (§2.1.2) |
| macOS | ARM64 + x86_64 | `aarch64-apple-darwin`, `x86_64-apple-darwin` | macOS 11+, Universal 2 binary |
| Linux | x86_64 + AArch64 | `x86_64-unknown-linux-gnu`, `aarch64-unknown-linux-gnu` | Ubuntu 20.04+ / Debian 11+; AppImage and Flatpak, no exotic dynamic dependency |
| Android | ARMv7, ARM64, x86, x86_64 | `armv7-linux-androideabi`, `aarch64-linux-android`, `i686-linux-android`, `x86_64-linux-android` | **minSdk = API 21 (Android 5.0)**; targetSdk according to the Google Play requirement at the time of publication; native Kotlin UI (§2.2) |
| iOS / iPadOS | ARM64 | `aarch64-apple-ios` | **iOS 15.0 minimum**, with functional restrictions if an API is missing; native SwiftUI UI (§2.2) |
| Web (PWA) **[P2, in scope]** | WebAssembly | `wasm32-unknown-unknown` | Chrome, Safari, Firefox; dedicated TypeScript web UI, bound to the WebAssembly engine; WebGPU if available, otherwise CPU engine; engine in Web Workers; OPFS / IndexedDB storage; no embedded server and no BLE on Safari (the browser is a client of a Loomora server) |

Guiding principle: target the oldest devices accepted by the stores. **It is acceptable to disable features** on those devices (§2.3) rather than raise the floor.

#### 2.1.1 Extended architectures ("as many architectures as possible")
Support is organized in tiers, according to the availability of the Rust toolchain (tier 2 minimum) and of the platform's native UI toolkit. The list must be revalidated with each Rust release.

| Tier | Deliverable | Architectures |
|---|---|---|
| **A** | Complete packaged application (native UI) | The table in §2.1 |
| **B** | Complete application if the UI toolkit exists on the target (WinUI 3, GTK 4), otherwise tier C | Windows x86 32-bit (`i686-pc-windows-msvc`; WinUI 3 or MFC UI); Linux x86 32-bit (`i686-unknown-linux-gnu`); **Linux ARM 32-bit** (`armv7-unknown-linux-gnueabihf`, `arm-unknown-linux-gnueabihf` for ARMv6); Linux RISC-V 64 (`riscv64gc-unknown-linux-gnu`); **Linux PowerPC 64 little-endian** (`powerpc64le-unknown-linux-gnu`) |
| **C** | CPU engine + CLI + embedded server; the UI is the PWA served to a local or remote browser (§9.2) | **Linux PowerPC 32-bit and 64-bit big-endian** (`powerpc-unknown-linux-gnu`, `powerpc64-unknown-linux-gnu`); s390x; LoongArch64; FreeBSD; other Rust tier 2 targets |

Cross-cutting constraints:
- **32-bit**: never assume `usize` = 64 bits; addressable memory budget of 2 to 4 GB; 64-bit atomics through a portable layer.
- **Big-endian**: all serialization (files, BLE, database) is explicitly little-endian; CI tests under emulation (QEMU).
- **SIMD**: a portable scalar path is mandatory; NEON, SSE / AVX, AltiVec / VSX are optional optimizations.
- **GPU**: absent or unsupported on these targets → CPU engine.
- "Best-effort" support outside tier A, with CI building and testing the engine on every declared target.

#### 2.1.2 Windows legacy (Windows XP) — MFC UI

Windows has **two UIs**: **WinUI 3** (modern) and **MFC** (Windows XP-compatible). They rely on the same core and the same `.loomora` files.

- **MFC scope**: Windows XP SP3 and later, x86 (x64 optional, XP x64). The same MFC executable also runs on Windows 10 and 11; the user can choose WinUI 3 or MFC there.
- **Exception to the "no C++" rule**: MFC requires C++ for this one UI layer, which is thin and free of business logic. The core stays in Rust, called through a stable C API generated with `cbindgen` (crate `loomora-capi`). C++ toolchain: Visual Studio with the `v141_xp` toolset (a component deprecated by Microsoft).
- **Rust toolchain for XP: major risk.** Rust does not officially support Windows XP (the official Windows targets require recent versions of Windows). An unofficial toolchain (adapted standard library, compatibility layer) or an adapted GNU/MinGW target is needed. To be validated in phase 0 (§16.2). Without a viable solution, the XP version is abandoned or redesigned.
- **Features explicitly disabled on XP** (§2.3):
  - GPU (CPU engine only); 2 GB of addressable memory, hence reduced resolution (§6.1).
  - Bluetooth Low Energy (absent from XP): transfer by file or local HTTP.
  - Modern HTTPS / TLS (TLS 1.2 absent by default): the embedded server stays on HTTP on the local network; no mDNS, pairing by QR or IP address.
  - Simplified embedded server (`std::net` + threads): the modern asynchronous I/O used by `tokio` does not exist on XP.
  - Voice: SAPI 5 if installed, otherwise the feature is disabled (decision 16.1 no. 4).
  - No PWA and no embedded webview.
- **Drawing and themes**: GDI+ for path drawing; light / dark themes drawn by the application (MFC in *owner-draw*); high contrast follows the Windows contrast scheme.
- **Security**: XP is no longer maintained by Microsoft. The application displays a warning and exposes nothing outside the local network.

### 2.2 Technical stack
- **Core entirely in Rust** (memory safety, no GC, no C++ runtime). C++ is excluded from the core; the only exception is the MFC UI layer (§2.1.2).
- A feature **may be reimplemented several times depending on the platform** (audio, Bluetooth, printing, speech recognition…), but universal methods should be preferred when they exist.
- **User interface: native on each platform** (decision 16.1 no. 1). Tauri and webviews are ruled out for installed applications; only the PWA is a web UI.

#### 2.2.1 UI per platform

| Platform | UI technology | Remarks |
|---|---|---|
| Android | Kotlin, Jetpack Compose (Views if Compose causes problems on API 21) | |
| iOS / iPadOS | SwiftUI (Swift) | iOS 15; occasional UIKit if a SwiftUI API is missing |
| macOS | SwiftUI, module shared with iOS / iPadOS | Occasional AppKit |
| Windows (x86, x64, ARM64) | WinUI 3 (C#, Windows App SDK) | Modern UI; Windows 10 to be confirmed depending on the SDK version |
| Windows XP and later (x86) | MFC (C++, `v141_xp` toolset) | Legacy UI (§2.1.2); thin layer over the core's C API |
| Linux | GTK 4 + libadwaita via `gtk-rs` (Rust), delivered as Flatpak and AppImage | **Provisional** (§16.2) |
| Web (PWA) | TypeScript + lightweight web framework, engine in WebAssembly | Same application model as the other UIs |
| Tier C (§2.1.1) | CLI + web UI served by the embedded server (§9.2) | |

Principles:
- **Thin UIs, zero business logic.** States, use cases and view-models live in `loomora-app` (Rust), exposed by `loomora-ffi`: UniFFI to Kotlin and Swift, `uniffi-bindgen-cs` to C#, C API (`cbindgen`, crate `loomora-capi`) to MFC, direct Rust calls for GTK, `wasm-bindgen` for the web.
- **Functional parity.** This document is the single specification. Common acceptance scenarios are run on every UI. A function missing from a platform is disabled explicitly (§2.3), never silently.
- **Path drawing.** The core provides primitives (polylines, nails, colors, opacity) that each UI draws with its native canvas (Compose Canvas, SwiftUI Canvas / Metal, Direct2D / Win2D, Cairo / GL, Canvas 2D / WebGL). A shared `wgpu` rendering on a native surface (`loomora-render3d`) is adopted for the 3D view (§3.6.7) **[P2]**.
- **System services** (Bluetooth, voice / TTS, printing, sharing, files, notifications): implemented natively by each UI behind interfaces defined in `loomora-app` (UniFFI callback interfaces). On desktop, Bluetooth may go through `btleplug`.
- **Texts**: provided by the core (§11), to keep a single translation source.

### 2.3 Graceful degradation (old devices: API 21–25, 32-bit, 1–2 GB of RAM)
- GPU acceleration disabled automatically; strict fallback to the CPU engine.
- Simulation canvas resolution reduced (§6.1).
- Advanced offline speech recognition disabled, with fallback to the manual and automatic (timer) modes. Speech synthesis remains in use if the system provides it.
- Upfront memory allocation to avoid fragmentation; buffers that can be shrunk dynamically (tiled cache or temporary downsampling during computation).
- No instruction newer than ARMv7 is required in the 32-bit fallback binary.

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
- **Windows XP (MFC)**: CPU computation and export, 100,000-triangle cap, basic software wireframe 3D view.

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

### 9.1 `.loomora` project and path format **[P1]** (validated, decision 16.5)

**Principles**
- A single format for the file (import / export / sharing), the network (§9.2), Bluetooth (§9.3) and the PWA.
- Self-describing, versioned, verifiable, readable without Loomora (JSON in a ZIP archive).
- Architecture-independent: UTF-8 text, **explicit little-endian** binary (no "native" serialization: cf. big-endian PowerPC, §2.1.1).
- The path is **stored, not recomputed** on import: the results of a computation may differ slightly from one architecture to another (§13).

**Container**
- ZIP archive (*stored* or *deflate* entries), extension `.loomora`, MIME type `application/vnd.loomora+zip`. First entry `mimetype`, uncompressed.
- File association in each OS (double-click, "Open with", Android / iOS share sheet, drag and drop, PWA *File Handling*).

**Geometric convention (common to the whole file)**
- Unit: millimeter. Origin: center of the support's bounding box. X to the right, Y up; Z toward the viewer (right-handed coordinate system); z = 0 for a planar support.
- **Front** view (the viewer's side of the artwork). **CW / CCW** (§6.3) are defined in this coordinate system, seen from the nail tip (in 2D, the nail axis points toward the viewer: front view). A "mirror" export is an explicit processing step, never implicit.

**Archive entries**

| Entry | Req. | Content |
|---|---|---|
| `mimetype` | yes | `application/vnd.loomora+zip` |
| `manifest.json` | yes | See below |
| `support.json` | yes | Outer contour, exclusion zones, dimensions, background / support color |
| `nails.json` | yes | List of nails |
| `spools.json` | yes | List of spools (strings) |
| `mesh.json`, `mesh.bin` | if meshed surface | Support mesh (§3.6) |
| `views.json` | if 3D target | Projection cameras and associated target images (§3.6.5) |
| `sequence.bin` | yes | Weaving sequence, compact binary encoding |
| `settings.json` | no | Computation parameters for reproducibility (algorithm, seed, resolution, preprocessing, engine versions) |
| `progress.json` | no | Weaving progress (PC ↔ phone synchronization) |
| `media/source.*`, `media/target.png`, `media/preview.png` | no | Source image, processed target image, thumbnail |

**`manifest.json`**
- `format`: `{ "name": "loomora", "major": 1, "minor": 0 }`; `min_reader`: minimum reader version.
- `project_id` (UUID v4, stable across exports), `title`, `created_at`, `modified_at` (UTC, ISO 8601).
- `generator`: application, version, platform, architecture.
- `profile`: `full` | `light` (see below).
- `counts`: number of nails, steps, spools.
- `entries`: for each entry, `size` and `sha256`.
- `required_features`: list of features the reader must handle (empty for a planar project; e.g. `surface_mesh`, `string_levels`, §3.6); a reader that does not handle one rejects the file.
- `extensions`: free object with keys prefixed by a namespace (ignored by readers that do not know it).

**`support.json`** — fields `surface` (`plane` by default; `mesh`, `primitive`, `extrusion`, `revolution`, `csg`, §3.6.2) and `body` (`solid` | `shell` | `guide`, §3.6.1); outer shape and exclusion shapes, each with its type (circle, ellipse, rectangle, heart, star, SVG…), its parameters **and** its resolved polyline (the reader does not need to know the shape to display it); board dimensions; background color.

**`nails.json`** — array of `{ id, x_mm, y_mm, z_mm?, axis?, diameter_mm, length_mm?, side?, face?, kind, label? }` (`z_mm`, `axis`, `side`, `face` for 3D, §3.6.3):
- `id`: unique u32, not necessarily contiguous;
- `kind`: `boundary` | `exclusion_boundary` | `interior`;
- `label`: number displayed on the template and in the player, if it differs from `id`.
- `axis`: unit vector base → tip (default: toward the viewer); `length_mm`: length protruding from the surface; `side`: `exterior` | `interior`; `face`: attachment triangle and barycentric coordinates.

**`spools.json`** — array of `{ id, name, color_hex, opacity, diameter_mm, length_available_m?, material?, domain? }` (`domain`: `exterior` | `interior` | `both`, §3.6.4).

**`mesh.json` / `mesh.bin`** (non-flat supports, §3.6) — `mesh.json`: units, applied transformation, `body`, flags (watertight, manifold, number of vertices and triangles), name and fingerprint of the original file, optional parametric construction (primitive, extrusion, revolution, booleans) so that it can be edited. `mesh.bin` (little-endian): positions (f32, mm), triangle indices (u32), optional normals and UVs; the original file may be attached in `media/`.

**`views.json`** — projection cameras (position, orientation, orthographic or perspective, size), associated target image (`media/`) and weight (§3.6.5).

**`sequence.bin`** (little-endian)
```
"LMSQ" | version u8 | header flags u8 (bit 0: levels present) | count (varint LEB128) | records… | CRC32 u32
record: nail_id (varint) | flags u8 | level u8 (if header bit 0; pass height, 0 = base, §3.6.4)
flags: bit 0      = rotation at this nail (0 = CW, 1 = CCW)
       bits 1–2   = type (0 segment, 1 start, 2 end, 3 reserved)
       bits 3–7   = spool_id (0–30; 31 = escape, followed by a varint)
```
Semantics: the records are in weaving order.
- `start`: the spool's string is attached to the nail (knot).
- `segment`: the spool's string goes from the nail of its previous record to `nail_id`, with the winding direction indicated at `nail_id`; the rotation at the starting nail is that of the previous record of this spool. The tangent type (CW→CW, CCW→CCW, CW→CCW, CCW→CW) is deduced from it.
- `end`: the string is cut / fixed at this nail.

The step list of the machine JSON (§8.3) can be reconstructed losslessly from this encoding. About 3 bytes per step (≈ 15 KB for 5,000 steps), which suits Bluetooth.

**`progress.json`** — `{ project_id, revision, updated_at, device_id, index, consumed_mm_per_spool, bookmarks }`.
- `revision`: monotonic counter per device.
- An import **never overwrites** local progress without confirmation. If the two progress states have diverged, the user chooses (keep local, take remote, keep the furthest along).

**Export profiles**
- `full`: everything, including the source image and the parameters.
- `light`: without `media/` or `settings.json`; used for Bluetooth and small terminals.
- The EXIF / GPS metadata of embedded images is **always stripped**.

**Versioning**
- *Minor* (added fields): a reader ignores unknown fields and, if possible, keeps unknown entries when rewriting.
- *Major* (breaking change): an old reader rejects the file with an explicit message (`min_reader`). A recent reader can read all previous majors.

**Mandatory validation on import**
- Verification of `sha256` / CRC32; unique nail identifiers; valid references between entries; finite numbers; nails within the support (tolerance); warning if the minimum nail-to-nail distance is not respected.
- Limits: maximum decompressed size 256 MB, 100,000 nails, 5,000,000 steps, 5,000,000 triangles; rejection of absolute paths, `..` and symbolic links (*zip-slip*); guard against decompression bombs; no executable content.
- `loomora validate <file>` command (CLI).

**Mapping to transports**
- **File**: system save / open / share; PWA: *File System Access*, download, *Web Share Target*.
- **HTTP** (§9.2): `GET /project/<token>/export.loomora?profile=light|full` (resumable via `Range`); `PUT /project/<token>/progress` for `progress.json`.
- **BLE** (§9.3): `light` profile sent as a series of entries (name, size, content) with a CRC32 per block, then verification of the manifest `sha256`; the receiver rebuilds a local `.loomora`.
- **QR code**: contains only the pairing link, never the project.

**Tests**: reference files produced on each architecture and read back on the others (including big-endian and 32-bit), lossless round trip, reader fuzzing.

### 9.2 Embedded web server **[P1]**
- The software includes a micro HTTP / WebSocket server (Rust: `axum`/`tokio`), with no external dependency; its implementation is part of Loomora.
- It can also run **headless on a server** (CLI binary) to compute remotely.
- **QR code pairing**: URL `http://<IP>:<PORT>/project/<TOKEN>`; the phone scans it and loads the full project into its persistent local cache. mDNS / Zeroconf discovery.
- **Bidirectional** synchronization: weaving progress is reported in real time from the phone to the PC (WebSocket).
- Security: limited-use access token, listening on the local network by default, optional TLS for remote access.
- **Privacy**: remote computation is an explicit option. The image is sent only to a server chosen and operated by the user, with their consent, never to a third-party service by default.

### 9.3 Bluetooth Low Energy **[P1]**
For workshops without a local network or for direct machine control. `btleplug` on desktop; native bridges CoreBluetooth (iOS/macOS) and Android BLE.

Custom GATT profile (proprietary 128-bit UUIDs to be generated; **do not** use the reserved 16-bit SIG space):
- **Metadata** (Read): project ID, number of nails, number of steps, spool colors.
- **Sequence transfer** (Write / Notify): blocks fragmented according to the negotiated MTU, **CRC32 per block**, resume on missing block.
- **Tracking / control** (Read / Write / Notify): current step, play/pause, winding direction, confirmation acknowledgments.

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
- Data: project ID, index of the active instruction, current spool, string length consumed, scrolling mode and display preferences.
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

## 14. Software architecture (Cargo workspace)

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
│   ├── loomora-ffi/          # UniFFI bridges (Kotlin, Swift, C#)
│   ├── loomora-capi/         # Stable C API (cbindgen) for the MFC UI
│   ├── loomora-wasm/         # WebAssembly binding for the PWA (wasm-bindgen)
│   └── loomora-i18n/         # fluent, unic-langid, language packs
└── apps/
    ├── android/              # Kotlin, Jetpack Compose
    ├── apple/                # SwiftUI: iOS, iPadOS, macOS (shared module)
    ├── windows-winui/        # WinUI 3 (C#)
    ├── windows-mfc/          # MFC (C++), Windows XP and later
    ├── linux/                # GTK 4 (gtk-rs)
    ├── web/                  # PWA (TypeScript)
    └── cli/                  # Batch computation, headless server, tests
```

Indicative libraries: `nalgebra` or `glam`, `geo`, `lyon_geom`, `image`, `rayon`, `wgpu`, `naga`, `axum`, `tokio`, `btleplug`, `rusqlite`, `fluent`, `uniffi`, `wasm-bindgen`, `gtk4`, `parry3d` (BVH, intersections), `stl_io`, `tobj`, `ply-rs`, `gltf`.

---

## 15. Planning and provisional milestones

0. **Phase 0 — Technical validation and PoC [blocking]**: verify each technical choice against the support requirements for old systems, through an end-to-end PoC on all targets (§15.1). No subsequent phase starts before milestone G0.
1. **Phase 1 — Proof of concept**: greedy algorithm in CLI on a 200-nail circle; performance measurements; geometry types and 3D-ready interfaces (§3.6.1).
2. **Phase 2 — MVP**: interface on the pilot platforms (§16.2), live preview, parameters, list export, 1:1 PDF template, tangents and winding direction.
3. **Phase 3 — Workshop**: manual / automatic / voice player, persistence, unweaving; deployment of the UIs on all target platforms; light / dark / high-contrast themes.
4. **Phase 4 — Advanced geometries**: predefined shapes, nested shapes, interior nails and automatic placement, nail import, vector mode.
5. **Phase 5 — Advanced engines**: GPU backend, LSQR/NNLS, metaheuristics.
6. **Phase 6 — Exchange and machines**: server, QR, BLE, CNC / G-code exports, PDF tiling.
7. **Phase 7 — Multi-color, full i18n, PWA**: multiple spools, language packs, RTL; PWA / WebAssembly delivery; tier B and C architectures; MFC UI (Windows XP).
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

#### 15.1.5 Milestone G0 and deliverables
- **Deliverables**: validation report (matrix of §15.1.4 filled in: OK, OK with restrictions, KO), decision log (ADR), versioned reference dataset, reproducible test bench, PoC kept as a non-regression baseline.
- **Update of the requirements document**: tiers A / B / C (§2.1.1) reclassified, decisions of §16 confirmed or revised, features disabled per platform (§2.3) specified.
- **Milestone G0 (move to phase 1)**: all hypotheses for the tier A platforms are validated, and the go / no-go decision is made for **Windows XP (V1, V9)**. The tier B and C targets can continue in parallel, their decision having to be made before phase 7.

---

## 16. Decisions

### 16.1 Settled

These decisions remain **provisional** until they are validated by the phase 0 PoC (§15.1).

| # | Item | Decision |
|---|---|---|
| 1 | UI | **Native UI per platform**: Kotlin (Android), SwiftUI (iOS, iPadOS, macOS), PWA / WebAssembly (web); Windows: WinUI 3 + MFC (no. 8); Linux: see no. 9. **Tauri ruled out** (§2.2) |
| 2 | Minimum iOS | **iOS 15.0**, with possible functional restrictions on missing APIs |
| 3 | Persistence | **SQLite (`rusqlite`, embedded SQLite)** — chosen by default, the user having no preference: transactional, portable across 32 / 64-bit and little / big-endian, mature tooling. `redb` ruled out (less proven on exotic architectures) |
| 4 | Speech recognition | **System API only**; feature disabled if it is absent (§10.1) |
| 5 | `.loomora` format | Validated; specified in §9.1 |
| 6 | PWA / WebAssembly | **In scope** (P2) |
| 7 | Architectures | **As many as possible**, including Linux ARM 32-bit and PowerPC; tiers A / B / C (§2.1.1) |
| 8 | Windows UI | **Two UIs**: **WinUI 3** (Windows 10+, x86 / x64 / ARM64) and **MFC** (Windows XP SP3 and later, x86); §2.1.2 |

### 16.2 Still open

| # | Item | Remark |
|---|---|---|
| 9 | **Linux desktop UI** | Provisionally chosen: GTK 4 (Rust). Alternative: a single Rust toolkit (Slint), to be validated for complex text and RTL (§11) |
| 10 | Winding direction displayed by default | Relative, dial or CW / CCW |
| 11 | License and business model | Open source, free, paid; affects the stores, the dependencies and third-party code |
| 12 | Pilot platforms of the MVP | Suggestion: Android + WinUI 3 |
| 13 | Jetpack Compose or Views on Android API 21 | To be decided after the phase 0 spike |
| 14 | **Rust toolchain for Windows XP** | Major risk (§2.1.2): unofficial toolchain, server without `tokio`, TLS absent. Spike in phase 0; otherwise the MFC XP version is abandoned or redesigned |

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
