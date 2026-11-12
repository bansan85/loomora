#ifndef LOOMORA_UTF8_HPP
#define LOOMORA_UTF8_HPP

#include <string>

namespace loomora {

/* Strict UTF-8 decoding: rejects overlong forms, surrogates, code points above
   U+10FFFF, stray continuation bytes and truncated sequences.
   Throws InvalidUtf8 on malformed input. */
std::u32string decode_utf8(const std::string &bytes);

} // namespace loomora

#endif
