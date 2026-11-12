#ifndef LOOMORA_GREET_HPP
#define LOOMORA_GREET_HPP

#include <cstddef>
#include <string>

namespace loomora {

constexpr std::size_t kMaxInputSize = 4096;

/* Returns "Bonjour " followed by `input` (UTF-8).
   Throws InputTooLong if input exceeds kMaxInputSize bytes, InvalidUtf8 if it
   is not valid UTF-8. */
std::string greet(const std::string &input);

} // namespace loomora

#endif
