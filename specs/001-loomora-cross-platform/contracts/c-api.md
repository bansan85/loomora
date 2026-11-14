# Contract: C++11 backend API exposed to the native UIs

The backend is written **only in C++11** (library `libloomora`, namespace `loomora`, no later standard required). This **C API** is the **only** boundary with the native UIs, Legacy and Modern alike. The header `core/include/loomora.h` is **handwritten**, valid as C89 and C++98 so that the oldest UI toolchains (old Xcode GCC, old MinGW) can include it. The same functions are exported depending on the target: C symbols in a static library (Windows Legacy, macOS, iOS, Linux Legacy), a DLL (Windows Modern), JNI glue (Android), Emscripten exports (Web).

## Header portability

- Types: the header includes `<stddef.h>` and, when `<stdint.h>` exists, `<stdint.h>`; for compilers without it (`_MSC_VER < 1600`) it defines `int32_t`, `uint32_t`, `uint8_t` and `int64_t` itself (`__int64` for the last one).
- Linkage: every function is `extern "C"` under C++ and uses the default C calling convention (`cdecl`); on Windows DLL builds `LOOMORA_API` expands to `__declspec(dllexport/dllimport)`, otherwise to `__attribute__((visibility("default")))` or nothing.
- No C++ type, template or exception appears in the header. The implementation behind it is free to use the C++ standard library.

## Functions (C form)

```c
#define LOOMORA_ABI_VERSION 1

uint32_t    loomora_abi_version(void);          /* must equal LOOMORA_ABI_VERSION */
/* Greeting function: writes "Bonjour " + input (UTF-8, no NUL required) into out[0..cap).
   Returns the byte length written (>= 8), or a negative error:
   -1 = input longer than 4096 bytes, -2 = cap too small, -3 = invalid pointer or invalid UTF-8, -4 = internal error.
   The caller owns both buffers; no NUL is written. -4 = unexpected internal failure (e.g. out of memory). */
int32_t     loomora_greet(const char *input, size_t input_len, uint8_t *out, size_t cap);

/* Families */
#define LOOMORA_FAMILY_WINDOWS 1
#define LOOMORA_FAMILY_MACOS   2
#define LOOMORA_FAMILY_LINUX   3
#define LOOMORA_FAMILY_ANDROID 4
#define LOOMORA_FAMILY_IOS     5
#define LOOMORA_FAMILY_WEB     6

int32_t     loomora_is_eol(int32_t family, int32_t major, int32_t minor); /* 1 = end of life, 0 otherwise */
const char *loomora_eol_warning(void);          /* UTF-8, static, never NULL */

/* UI variants */
#define LOOMORA_UI_LEGACY 1
#define LOOMORA_UI_MODERN 2

/* How the backend runs (the backend cannot tell a WebAssembly build from its JavaScript build) */
#define LOOMORA_CORE_NATIVE 1   /* "cpp"      */
#define LOOMORA_CORE_WASM   2   /* "cpp-wasm" */
#define LOOMORA_CORE_JS     3   /* "cpp-js"   */

/* Compiler identification string embedded at build time (e.g. "gcc 8.3.0"), static, never NULL. */
const char *loomora_build_compiler(void);

/* Builds the ProbeResult JSON line (see probe-result.schema.json).
   Writes at most `cap` bytes into `buf` (no NUL required) and returns the total length needed.
   `rss_kb` < 0 = not measured. `disabled_features` is a comma-separated list, "" if none.
   `timestamp_utc` is "YYYY-MM-DDTHH:MM:SSZ", supplied by the UI (the backend has no clock).
   The `compiler` field comes from loomora_build_compiler(). The function also computes
   `greeting_matches` by comparing `displayed_text` with the greeting of `input`. */
size_t      loomora_probe_record(const char *platform_id, int32_t ui_variant, int32_t core_kind,
                                const char *input, const char *displayed_text,
                                int32_t displayed, uint32_t display_ms, int64_t rss_kb,
                                int32_t eol_warning_shown, const char *disabled_features,
                                const char *timestamp_utc,
                                uint8_t *buf, size_t cap);
```

## Equivalents per target

| Target | Form | Note |
|---|---|---|
| Windows Legacy (Win32, C++), macOS / iOS (Objective-C), Linux Legacy (Xlib, C++), Linux Modern (GTK 3 with gtkmm, C++) | C symbols, static library `libloomora.a` | the native UI includes `loomora.h` |
| Windows Modern (WinUI 3) | same C symbols in `loomora.dll` (x64, ARM64) | called from C# with `LibraryImport` |
| macOS / iOS Modern (SwiftUI) | same C symbols in a static library | imported by Swift through a module map |
| Android | `Java_fr_loomora_probe_Native_*` in `bridge/jni/loomora_jni.cpp` | `Native.java` declares the `native` methods; strings returned as `String`; glue only converts `jstring` ↔ UTF-8 |
| Web | Emscripten exports (`EXPORTED_FUNCTIONS` from `bridge/wasm/exports.txt`) | called with `ccall`/`cwrap`; same API in WebAssembly and in JavaScript |

## Guarantees

- Memory crosses the boundary only in caller-supplied buffers (no function returns memory to free); returned strings are static; no mutable global state; callable from any thread.
- Every exported function catches all exceptions (`catch (...)`) and returns an error code: no exception ever crosses the C boundary. The shared build exports only `loomora_*` C symbols (checked by `tools/check_std.py`), and the C++ runtime is linked statically where the platform lacks one.
- Strings are UTF-8; conversion to the native encoding (UTF-16, `NSString`, `jstring`) happens in the UI.
- Independent of byte order and native integer size (32 or 64 bits).
- `loomora_is_eol`: negative values → 0.
- Compiles as strict C++11; uses of more recent compiler features are isolated behind a feature macro with a fallback (FR-021, FR-022).
- Both UI variants of a platform call exactly the same API on the same backend build family.

## Conformance tests

Golden vectors in `core/tests/vectors.json` (inputs `family/major/minor` → expected outputs; for `loomora_greet`, the exact bytes of `"Bonjour " + input` and the error codes; shape of the probe JSON). They run as Catch2 tests on the host (GCC and Clang, `-std=c++11`), as `loomora_selftest` on every target, and through each UI in probe mode.

## Evolution

Adding a function = increment of `LOOMORA_ABI_VERSION`. UIs check `loomora_abi_version()` at startup and show a readable error on mismatch.
