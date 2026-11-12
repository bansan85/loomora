#ifndef LOOMORA_INTERNAL_CONFIG_HPP
#define LOOMORA_INTERNAL_CONFIG_HPP

/* Compiler and feature detection (FR-021, FR-022). Recent compiler features are used
   only behind the macros below, each with a fallback that keeps the code strict C++11.
   Every fallback is declared in validation/fallbacks.json.

   This header includes no C++ standard header: it can be included anywhere. */

/* ---- Compiler detection (clang and clang-cl also define __GNUC__ / _MSC_VER) ---- */
#if defined(__clang__)
#define LOOMORA_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define LOOMORA_COMPILER_GCC 1
#elif defined(_MSC_VER)
#define LOOMORA_COMPILER_MSVC 1
#endif

/* ---- LOOMORA_HAS_BUILTIN_EXPECT: __builtin_expect (GCC 3.0+, Clang) ----
   Fallback: LOOMORA_LIKELY(x) and LOOMORA_UNLIKELY(x) evaluate to plain (x). */
#if defined(__GNUC__) || defined(__clang__)
#define LOOMORA_HAS_BUILTIN_EXPECT 1
#define LOOMORA_LIKELY(x) __builtin_expect(!!(x), 1)
#define LOOMORA_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LOOMORA_HAS_BUILTIN_EXPECT 0
#define LOOMORA_LIKELY(x) (x)
#define LOOMORA_UNLIKELY(x) (x)
#endif

/* ---- LOOMORA_HAS_CXX17_NODISCARD: [[nodiscard]] (C++17) ----
   MSVC keeps __cplusplus at 199711L unless /Zc:__cplusplus is given, so _MSVC_LANG is
   consulted too. Fallback: LOOMORA_NODISCARD expands to nothing. */
#if (defined(__cplusplus) && __cplusplus >= 201703L) ||                                            \
    (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
#define LOOMORA_HAS_CXX17_NODISCARD 1
#define LOOMORA_NODISCARD [[nodiscard]]
#else
#define LOOMORA_HAS_CXX17_NODISCARD 0
#define LOOMORA_NODISCARD
#endif

#endif
