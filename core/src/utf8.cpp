#include "utf8.hpp"

#include "errors.hpp"

namespace loomora {

namespace {

constexpr char32_t kMaxCodePoint = 0x10FFFF;

constexpr bool is_continuation(unsigned char b) { return (b & 0xC0) == 0x80; }
constexpr bool is_surrogate(char32_t cp) { return cp >= 0xD800 && cp <= 0xDFFF; }

/* What a lead byte announces. `length == 0` means it cannot start a sequence. */
struct Lead {
    std::size_t length;
    char32_t payload; // bits carried by the lead byte itself
    char32_t min_cp;  // smallest code point this length may encode (overlong check)
};

constexpr Lead classify(unsigned char b) {
    return b < 0x80   ? Lead{1, b, 0}
           : b < 0xC2 ? Lead{0, 0, 0} // continuation byte or overlong 2-byte lead
           : b < 0xE0 ? Lead{2, char32_t(b & 0x1F), 0x80}
           : b < 0xF0 ? Lead{3, char32_t(b & 0x0F), 0x800}
           : b < 0xF5 ? Lead{4, char32_t(b & 0x07), 0x10000}
                      : Lead{0, 0, 0};
}

} // namespace

std::u32string decode_utf8(const std::string &bytes) {
    std::u32string code_points;
    code_points.reserve(bytes.size());

    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const Lead lead = classify(static_cast<unsigned char>(bytes[pos]));
        if (lead.length == 0 || bytes.size() - pos < lead.length)
            throw InvalidUtf8("invalid or truncated UTF-8 sequence");

        char32_t value = lead.payload;
        for (std::size_t k = 1; k < lead.length; ++k) {
            const unsigned char b = static_cast<unsigned char>(bytes[pos + k]);
            if (!is_continuation(b))
                throw InvalidUtf8("invalid UTF-8 continuation byte");
            value = (value << 6) | (b & 0x3F);
        }
        if (value < lead.min_cp || value > kMaxCodePoint || is_surrogate(value))
            throw InvalidUtf8("overlong, surrogate or out-of-range code point");

        code_points.push_back(value);
        pos += lead.length;
    }
    return code_points;
}

} // namespace loomora
