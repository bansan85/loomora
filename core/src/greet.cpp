#include "greet.hpp"

#include "errors.hpp"
#include "utf8.hpp"

namespace loomora {

std::string greet(const std::string &input) {
    if (input.size() > kMaxInputSize)
        throw InputTooLong("input exceeds 4096 bytes");
    decode_utf8(input); // validation only: throws InvalidUtf8
    return "Bonjour " + input;
}

} // namespace loomora
