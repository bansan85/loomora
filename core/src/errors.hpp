#ifndef LOOMORA_ERRORS_HPP
#define LOOMORA_ERRORS_HPP

#include <stdexcept>

namespace loomora {

/* The input is longer than the 4,096 bytes the greeting accepts. */
class InputTooLong : public std::length_error {
  public:
    using std::length_error::length_error;
};

/* The input is not strictly valid UTF-8. */
class InvalidUtf8 : public std::invalid_argument {
  public:
    using std::invalid_argument::invalid_argument;
};

} // namespace loomora

#endif
