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

/* ---- Fallback switches of the .loomora format (spec 002, FR-021) ----
   Each switch is DEFAULT OFF: the modern code path is used everywhere, and a switch is turned on
   only for the one target that needs it, by its toolchain file (-D on the compiler command line,
   cmake/toolchains/) or by the platform macro tested below. Never turn one on for everybody.
   Each one is declared, with the platforms that take it, in
   docs/implementation/validation/fallbacks.json (the id is given in its comment). The io layer
   of src/format/io/ is their only user. */

/* LOOMORA_FMT_WIN2000_SEEK (fallbacks.json id: fmt-win2000-seek). Windows 2000 floor: a 64-bit
   file offset is set with SetFilePointer and its high-word argument, never SetFilePointerEx.
   On when the headers are limited to Windows 2000 (_WIN32_WINNT=0x0500, set by the Windows 2000
   toolchain file); every other Windows target takes the modern path. */
#ifndef LOOMORA_FMT_WIN2000_SEEK
#if defined(_WIN32) && defined(_WIN32_WINNT) && _WIN32_WINNT <= 0x0500
#define LOOMORA_FMT_WIN2000_SEEK 1
#else
#define LOOMORA_FMT_WIN2000_SEEK 0
#endif
#endif

/* LOOMORA_FMT_LSEEK64 (fallbacks.json id: fmt-lseek64). Android API 9 to 20 have no 64-bit
   lseek / pread behind _FILE_OFFSET_BITS: lseek64 / pread64 are called explicitly. On for those
   API levels only (__ANDROID__ and __ANDROID_API__ are predefined by the NDK compilers). */
#ifndef LOOMORA_FMT_LSEEK64
#if defined(__ANDROID__) && defined(__ANDROID_API__) && __ANDROID_API__ < 21
#define LOOMORA_FMT_LSEEK64 1
#else
#define LOOMORA_FMT_LSEEK64 0
#endif
#endif

/* LOOMORA_FMT_NO_LARGE_FILES (fallbacks.json id: fmt-no-large-files). A target whose file offsets
   cannot exceed 32 bits signed: a file over 2 GiB is refused with
   LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM. Off everywhere; a toolchain file of such a target defines it. */
#ifndef LOOMORA_FMT_NO_LARGE_FILES
#define LOOMORA_FMT_NO_LARGE_FILES 0
#endif

#endif
