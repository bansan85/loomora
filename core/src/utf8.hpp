#ifndef LOOMORA_UTF8_HPP
#define LOOMORA_UTF8_HPP

#include <string>

namespace loomora {

/* U+FFFD, substituted for malformed bytes by decode_utf8_lossy. */
constexpr char32_t kReplacementCharacter = 0xFFFD;

/* Strict UTF-8 decoding: rejects overlong forms, surrogates, code points above
   U+10FFFF, stray continuation bytes and truncated sequences.
   Throws InvalidUtf8 on malformed input. */
std::u32string decode_utf8(const std::string &bytes);

/* Same rules, but never throws InvalidUtf8: each byte that cannot be decoded becomes
   one U+FFFD. Used only to report text that is not trusted (probe records). */
std::u32string decode_utf8_lossy(const std::string &bytes);

} // namespace loomora

#endif
