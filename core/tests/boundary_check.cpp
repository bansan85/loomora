// C boundary check (spike S6): no exception may escape any loomora_* function, even when an
// allocation fails at any point. Built and run by tools/check_std.py together with the
// backend sources (not with the test framework). Strict C++11, STL allowed.
//
// The global operator new is replaced: while `g_armed` is set, the allocation number
// `g_fail_at` (counting from 0) throws std::bad_alloc. Every function is called once per
// failure point, from 0 up to the number of allocations a successful call performs, plus a
// margin, so that every allocation site of the call is made to fail once.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "../include/loomora.h"

namespace {

bool g_armed = false;
long g_counter = 0;
long g_fail_at = -1; // -1: never fail
bool g_failed_once = false;

} // namespace

void *operator new(std::size_t size) {
    if (g_armed) {
        const long n = g_counter++;
        if (n == g_fail_at) {
            g_failed_once = true;
            throw std::bad_alloc();
        }
    }
    void *p = std::malloc(size ? size : 1);
    if (!p)
        throw std::bad_alloc();
    return p;
}

void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

namespace {

struct Outcome {
    bool escaped;     // an exception left the function
    long value;       // returned value
    long allocations; // allocations seen while armed
    bool injected;    // the injected failure was actually thrown
};

typedef long (*Call)();

Outcome run(Call call, long fail_at) {
    Outcome o;
    o.escaped = false;
    o.value = 0;
    g_counter = 0;
    g_fail_at = fail_at;
    g_failed_once = false;
    g_armed = true;
    try {
        o.value = call();
    } catch (...) {
        o.escaped = true;
    }
    g_armed = false;
    o.allocations = g_counter;
    o.injected = g_failed_once;
    return o;
}

long call_greet() {
    unsigned char out[64];
    return loomora_greet("Coucou", 6, out, sizeof out);
}

long call_greet_long() {
    // Long enough to defeat the small-string optimisation of any standard library.
    static const char text[] = "0123456789012345678901234567890123456789";
    unsigned char out[128];
    return loomora_greet(text, sizeof text - 1, out, sizeof out);
}

long call_is_eol() { return loomora_is_eol(LOOMORA_FAMILY_WINDOWS, 6, 1); }

long call_probe() {
    unsigned char buf[1024];
    return static_cast<long>(loomora_probe_record(
        "windows-11-x64", LOOMORA_UI_MODERN, LOOMORA_CORE_NATIVE, "Coucou", "Bonjour Coucou", 1, 12,
        1024, 0, "a,b", "2026-11-12T10:00:00Z", buf, sizeof buf));
}

long call_probe_sizing() {
    return static_cast<long>(loomora_probe_record("windows-11-x64", LOOMORA_UI_LEGACY,
                                                  LOOMORA_CORE_NATIVE, "x", "Bonjour x", 1, 5, -1,
                                                  1, "", "2026-11-12T10:00:00Z", 0, 0));
}

long call_static_strings() {
    const char *a = loomora_eol_warning();
    const char *b = loomora_build_compiler();
    return (a && b && a[0] && b[0]) ? 1 : 0;
}

long call_abi() { return static_cast<long>(loomora_abi_version()); }

struct Case {
    const char *name;
    Call call;
};

} // namespace

int main() {
    const Case cases[] = {{"loomora_greet", call_greet},
                          {"loomora_greet (long input)", call_greet_long},
                          {"loomora_is_eol", call_is_eol},
                          {"loomora_probe_record", call_probe},
                          {"loomora_probe_record (sizing call)", call_probe_sizing},
                          {"loomora_eol_warning / loomora_build_compiler", call_static_strings},
                          {"loomora_abi_version", call_abi}};
    int failures = 0;
    long total_injected = 0;

    for (std::size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const Outcome base = run(cases[i].call, -1);
        if (base.escaped) {
            std::printf("FAIL %s: exception escaped without any injected failure\n", cases[i].name);
            ++failures;
            continue;
        }
        long injected = 0;
        for (long k = 0; k < base.allocations + 4; ++k) {
            const Outcome o = run(cases[i].call, k);
            if (o.escaped) {
                std::printf("FAIL %s: exception escaped when allocation %ld failed\n",
                            cases[i].name, k);
                ++failures;
            }
            if (o.injected)
                ++injected;
        }
        std::printf("ok   %s: %ld allocation(s) per call, %ld failure(s) injected, none escaped\n",
                    cases[i].name, base.allocations, injected);
        total_injected += injected;
    }

    // The check is meaningless if no allocation was ever made to fail.
    if (total_injected == 0) {
        std::printf("FAIL: no allocation failure could be injected\n");
        ++failures;
    }
    std::printf("%s (%ld injected failures)\n",
                failures ? "BOUNDARY CHECK FAILED" : "BOUNDARY CHECK OK", total_injected);
    return failures ? 1 : 0;
}
