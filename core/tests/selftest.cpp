// Self-test of the C API, driven the way a UI calls it. Plain C++11 `main`:
// no test framework, so it can be cross-compiled and run on every target.
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "../include/loomora.h"

namespace {

struct Greeting {
    std::string input;
    std::string expected;
};

struct Failure {
    std::string what;
    const char *input; // nullptr exercises the null-pointer check
    std::size_t length;
    std::size_t capacity;
    std::int32_t expected;
};

constexpr std::size_t kCapacity = 5000;

/* Calls loomora_greet and returns the status; `out` receives the bytes written. */
std::int32_t call(const char *input, std::size_t length, std::size_t capacity, std::string &out) {
    std::vector<std::uint8_t> buffer(kCapacity);
    const std::int32_t status = loomora_greet(input, length, buffer.data(), capacity);
    out.assign(buffer.begin(), buffer.begin() + (status < 0 ? 0 : status));
    return status;
}

class Checker {
  public:
    int failures() const { return failures_; }

    void check(bool condition, const std::string &what) {
        if (condition)
            return;
        std::cout << "FAIL: " << what << '\n';
        ++failures_;
    }

    void greeting(const Greeting &g) {
        std::string out;
        const std::int32_t status = call(g.input.data(), g.input.size(), kCapacity, out);
        check(status >= 0 && out == g.expected, "greeting of \"" + g.input + "\"");
    }

    void failure(const Failure &f) {
        std::string out;
        const std::int32_t status = call(f.input, f.length, f.capacity, out);
        check(status == f.expected, f.what + " (got " + std::to_string(status) + ", want " +
                                        std::to_string(f.expected) + ")");
    }

  private:
    int failures_ = 0;
};

} // namespace

int main() {
    Checker t;

    t.check(loomora_abi_version() == LOOMORA_ABI_VERSION, "abi version");
    t.check(loomora_build_compiler() != nullptr, "compiler string");

    const std::vector<Greeting> greetings = {
        {"", "Bonjour "},
        {"Coucou", "Bonjour Coucou"},
        {"H\xC3\xA9l\xC3\xA8ne", "Bonjour H\xC3\xA9l\xC3\xA8ne"},
        {"\xE6\x97\xA5\xE6\x9C\xAC", "Bonjour \xE6\x97\xA5\xE6\x9C\xAC"},
        {"\xF0\x9F\x98\x80", "Bonjour \xF0\x9F\x98\x80"},
        {std::string(4096, 'a'), "Bonjour " + std::string(4096, 'a')}, // limit accepted
    };
    for (const Greeting &g : greetings)
        t.greeting(g);

    const std::string too_long(4097, 'a');
    const std::vector<Failure> failures = {
        {"input too long", too_long.data(), too_long.size(), kCapacity, LOOMORA_ERR_TOO_LONG},
        {"capacity too small", "abc", 3, 5, LOOMORA_ERR_CAPACITY},
        {"overlong encoding", "\xC0\x80", 2, 100, LOOMORA_ERR_INVALID},
        {"surrogate", "\xED\xA0\x80", 3, 100, LOOMORA_ERR_INVALID},
        {"invalid byte", "\xFF", 1, 100, LOOMORA_ERR_INVALID},
        {"null input", nullptr, 3, 100, LOOMORA_ERR_INVALID},
        {"truncated sequence", "abc\xC3", 4, 100, LOOMORA_ERR_INVALID},
        {"above U+10FFFF", "\xF4\x90\x80\x80", 4, 100, LOOMORA_ERR_INVALID},
    };
    for (const Failure &f : failures)
        t.failure(f);

    if (t.failures() != 0) {
        std::cout << t.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "all passed\n";
    return 0;
}
