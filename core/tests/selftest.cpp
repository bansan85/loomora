// Self-test of the C API, driven the way a UI calls it. Plain C++11 `main`:
// no test framework, so it can be cross-compiled and run on every target.
// The greet vectors come from vectors.inc, generated from core/tests/vectors.json.
// (loomora_is_eol and loomora_probe_record are covered here once they are implemented.)
//
// Output goes through <iostream>, except where LOOMORA_AVOID_IOSTREAM is defined: the MinGW-w64
// toolchains of the Windows floors older than Vista (cmake/toolchains/mingw-w64-win*.cmake) define
// it, because the iostream initialisation of a static libstdc++ pulls the thread-support code of
// libgcc (gthr-win32-cond.o) into the program, which imports Vista-only KERNEL32 functions and
// keeps it from loading on Windows 2000 (spike S1, docs/implementation/adr/0002 and 0004). There the output uses
// std::fputs of <cstdio>, and this file must stay free of <iostream>, <thread>, <mutex>,
// <condition_variable>, <locale> and std::call_once. std::printf is avoided too: with MinGW-w64
// it becomes __mingw_vfprintf, which locks the stream through msvcrt's _lock / _unlock, two
// internal exports that tools/win2000_imports.json does not list.
#include <cstdint>
#include <string>
#include <vector>

#ifdef LOOMORA_AVOID_IOSTREAM
#include <cstdio>
#else
#include <iostream>
#endif

#include "../include/loomora.h"
#include "vectors.inc"

namespace {

void print(const std::string &line) {
#ifdef LOOMORA_AVOID_IOSTREAM
    std::fputs(line.c_str(), stdout);
#else
    std::cout << line;
#endif
}

class Checker {
  public:
    int failures() const { return failures_; }

    void check(bool condition, const std::string &what) {
        if (condition)
            return;
        std::fputs(("FAIL: " + what + "\n").c_str(), stdout);
        ++failures_;
    }

    void greet(const vectors::GreetVector &v) {
        // Sentinel-filled buffer: the bytes after the result must stay untouched.
        std::vector<std::uint8_t> buffer(v.capacity + 16, 0xA5);
        const std::int32_t status = loomora_greet(v.null_input ? nullptr : v.input.data(),
                                                  v.input_len, buffer.data(), v.capacity);
        const std::string what = std::string("greet vector ") + v.id;
        if (status != v.expected_status) {
            check(false, what + " (got " + std::to_string(status) + ", want " +
                             std::to_string(v.expected_status) + ")");
            return;
        }
        if (status < 0)
            return;
        const std::size_t length = static_cast<std::size_t>(status);
        check(std::string(buffer.begin(), buffer.begin() + length) == v.expected_output,
              what + ": output bytes");
        bool untouched = true;
        for (std::size_t i = length; i < buffer.size(); ++i)
            untouched = untouched && buffer[i] == 0xA5;
        check(untouched, what + ": bytes after the result were modified");
    }

  private:
    int failures_ = 0;
};

} // namespace

int main() {
    Checker t;

    t.check(loomora_abi_version() == LOOMORA_ABI_VERSION, "abi version");
    t.check(loomora_build_compiler() != nullptr, "compiler string");

    for (std::size_t i = 0; i < vectors::kGreetVectorsCount; ++i)
        t.greet(vectors::kGreetVectors[i]);

    if (t.failures() != 0) {
        std::fputs((std::to_string(t.failures()) + " failure(s)\n").c_str(), stdout);
        return 1;
    }
    std::fputs("all passed\n", stdout);
    return 0;
}
