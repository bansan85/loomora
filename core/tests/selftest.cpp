// Self-test of the C API, driven the way a UI calls it. Plain C++11 `main`:
// no test framework, so it can be cross-compiled and run on every target.
// The greet vectors come from vectors.inc, generated from core/tests/vectors.json.
// (loomora_is_eol and loomora_probe_record are covered here once they are implemented.)
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "../include/loomora.h"
#include "vectors.inc"

namespace {

class Checker {
  public:
    int failures() const { return failures_; }

    void check(bool condition, const std::string &what) {
        if (condition)
            return;
        std::cout << "FAIL: " << what << '\n';
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
        std::cout << t.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "all passed\n";
    return 0;
}
