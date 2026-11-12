/* Loomora backend: public C API (valid as C89 and C++98). */
#ifndef LOOMORA_H
#define LOOMORA_H

#include <stddef.h>
#if defined(_MSC_VER) && _MSC_VER < 1600
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;
typedef __int64 int64_t;
#else
#include <stdint.h>
#endif

#define LOOMORA_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

uint32_t loomora_abi_version(void);

/* Negative codes returned by loomora_greet. */
#define LOOMORA_ERR_TOO_LONG -1 /* input longer than 4096 bytes */
#define LOOMORA_ERR_CAPACITY -2 /* cap too small for the result */
#define LOOMORA_ERR_INVALID -3  /* null pointer or invalid UTF-8 */
#define LOOMORA_ERR_INTERNAL -4 /* unexpected failure (e.g. out of memory) */

/* Writes "Bonjour " + input (UTF-8) into out[0..cap). Returns the byte length
   written or one of the LOOMORA_ERR_* codes. The caller owns both buffers; no
   NUL is written and no exception ever crosses this function. */
int32_t loomora_greet(const char *input, size_t input_len, uint8_t *out, size_t cap);

/* Families */
#define LOOMORA_FAMILY_WINDOWS 1
#define LOOMORA_FAMILY_MACOS 2
#define LOOMORA_FAMILY_LINUX 3
#define LOOMORA_FAMILY_ANDROID 4
#define LOOMORA_FAMILY_IOS 5
#define LOOMORA_FAMILY_WEB 6

/* 1 = the vendor no longer maintains this system version, 0 otherwise (negative
   arguments, unknown family and the Web family always give 0). */
int32_t loomora_is_eol(int32_t family, int32_t major, int32_t minor);

/* End-of-life warning text, UTF-8, static, never NULL. */
const char *loomora_eol_warning(void);

/* UI variants */
#define LOOMORA_UI_LEGACY 1
#define LOOMORA_UI_MODERN 2

/* How the backend runs (it cannot tell a WebAssembly build from its JavaScript build). */
#define LOOMORA_CORE_NATIVE 1 /* "cpp" */
#define LOOMORA_CORE_WASM 2   /* "cpp-wasm" */
#define LOOMORA_CORE_JS 3     /* "cpp-js" */

/* Compiler identification string, static, never NULL. */
const char *loomora_build_compiler(void);

/* Builds the ProbeResult JSON line (see probe-result.schema.json). Writes at most
   `cap` bytes into `buf` (no NUL) and returns the total length needed. `rss_kb` < 0
   means not measured; `disabled_features` is a comma-separated list ("" if none);
   `timestamp_utc` is "YYYY-MM-DDTHH:MM:SSZ" supplied by the UI. The `compiler` field
   comes from loomora_build_compiler(); `greeting_matches` compares `displayed_text`
   with the greeting of `input` (false when the greeting fails). Returns 0, writing
   nothing, for an unknown `ui_variant` or `core_kind` or on internal failure. */
size_t loomora_probe_record(const char *platform_id, int32_t ui_variant, int32_t core_kind,
                            const char *input, const char *displayed_text, int32_t displayed,
                            uint32_t display_ms, int64_t rss_kb, int32_t eol_warning_shown,
                            const char *disabled_features, const char *timestamp_utc, uint8_t *buf,
                            size_t cap);

#ifdef __cplusplus
}
#endif

#endif
