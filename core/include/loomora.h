/* Loomora backend: public C API (valid as C89 and C++98). */
#ifndef LOOMORA_H
#define LOOMORA_H

#include <stddef.h>
#if defined(_MSC_VER) && _MSC_VER < 1600
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;
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

/* Compiler identification string, static, never NULL. */
const char *loomora_build_compiler(void);

#ifdef __cplusplus
}
#endif

#endif
