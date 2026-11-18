# bridge/wasm

The Web module is `libloomora` (C++11) compiled by Emscripten. There is no glue code: the six
functions of `core/include/loomora.h` are exported by name and the pages call them through
`ccall` / `cwrap`. This folder holds the three lists the build reads and nothing else.

| File | Content |
|---|---|
| `exports.txt` | the exported symbols (`_loomora_*`, with the `_` prefix of C symbols), a JSON array, `-sEXPORTED_FUNCTIONS=@...` |
| `runtime-methods.txt` | the runtime helpers a page may use, a JSON array, `-sEXPORTED_RUNTIME_METHODS=@...` |
| `exports-selftest.txt` | `_main`, for `loomora_selftest` (the test run under node by `ctest`) |

`_malloc` and `_free` are not exported: a page uses the stack of the module (`stackSave` /
`stackAlloc` / `stackRestore`) for every small buffer it passes. The one exception is a whole
`.loomora` file (the project probe modes of spec 002, `loomora_project_open_memory`, which does not
copy the buffer): `module.cpp` holds three helpers for it, `loomora_web_alloc(n)` and
`loomora_web_free(p)` (a buffer of the module that the page fills with the file, released after
`loomora_project_close`) and `loomora_web_export(project, profile, modified_at, &data, &size)`
(`loomora_project_export_stream` into a memory buffer, since a page cannot give the C API a callback
in both builds). The project functions of `loomora_project.h` are exported by name as the others;
those that return an `int64_t` (`loomora_report_count`, ...) give a `BigInt` in the WebAssembly build,
and out-pointers (`loomora_project **`, `size_t *`) are 4-byte slots on the module's stack.

## Loading

`createLoomora(options)` is the factory function (`-sMODULARIZE=1 -sEXPORT_NAME=createLoomora`). With
Emscripten 7.0.0 it returns a `Promise` of the module `M`; the glue file `loomora.js` is a classic
script that defines the global `createLoomora`, and it reads `loomora.wasm` next to itself unless
`options.instantiateWasm(imports, done)` is given (the Modern page does, to use
`WebAssembly.instantiateStreaming`; `done(instance)` hands the instance back). The JavaScript-only
build (Emscripten 1.39.20) returns the module itself, which has a `then()` of its own.

## Signatures

Types: `i32` is a JavaScript number; a pointer or a `size_t` is an `i32` in WebAssembly (32-bit
memory) and a number in `ccall` / `cwrap`; `i64` is a `BigInt` in the WebAssembly build.

```js
// ABI version of the backend, 1 today; the page refuses any other value.
M.ccall("loomora_abi_version", "number", [], []);

// "Bonjour " + input as UTF-8 into out[0..cap): the byte length written (no NUL), or a negative
// code: -1 input longer than 4096 bytes, -2 cap too small, -3 invalid UTF-8, -4 internal failure.
const greet = M.cwrap("loomora_greet", "number", ["number", "number", "number", "number"]);
//    greet(inputPtr, inputLen, outPtr, outCap)

// 1 if the system version is end of life; always 0 for the Web family (6).
const isEol = M.cwrap("loomora_is_eol", "number", ["number", "number", "number"]);
//    isEol(family, major, minor)

// Static UTF-8 strings, NUL-terminated: read with M.UTF8ToString(pointer).
M.ccall("loomora_eol_warning", "number", [], []);
M.ccall("loomora_build_compiler", "number", [], []);

// The ProbeResult JSON line (ASCII) into buf[0..cap): returns the total length needed (call again
// with a larger buffer when it exceeds cap), 0 for an unknown ui_variant or core_kind.
const probe = M.cwrap("loomora_probe_record", "number",
  ["number", "number", "number", "number", "number", "number", "number",
   "number",                                  // rss_kb: int64_t, passed as a BigInt, -1 = not measured
   "number", "number", "number", "number", "number"]);
//    probe(platformIdPtr, uiVariant, coreKind, inputPtr, displayedTextPtr, displayed, displayMs,
//          BigInt(rssKb), eolWarningShown, disabledFeaturesPtr, timestampPtr, bufPtr, bufCap)
```

`loomora_probe_record` has 13 parameters in the WebAssembly build: `rss_kb` (`int64_t`) is one `BigInt`
(Emscripten 7 passes `i64` as `BigInt`; `cwrap` has no type name for it, `"number"` forwards the
value untouched, so the `BigInt` is the one thing to remember). The JavaScript-only build has
no `i64`: Emscripten splits it into two 32-bit numbers, low then high, which makes 14 parameters
(`-1` is `-1, -1`); `ui/legacy/web/app.js` tells the two apart by `_loomora_probe_record.length`.

Strings and buffers are passed by address: copy the UTF-8 bytes into `M.HEAPU8` at an address from
`M.stackAlloc(n)` (restore with `M.stackRestore(M.stackSave())` after the call), pass the byte length
as `input_len`, and read the result back from `M.HEAPU8`. Read `M.HEAPU8` again after the call: the
heap can move when the module grows its memory (`-sALLOW_MEMORY_GROWTH=1`).

Constants used by the pages (from `loomora.h`): family `WEB` = 6; `ui_variant` Legacy = 1, Modern = 2;
`core_kind` `cpp-wasm` = 2, `cpp-js` = 3.
