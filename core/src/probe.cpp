#include "probe.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

#include "errors.hpp"
#include "greet.hpp"
#include "utf8.hpp"
#include "version.hpp"

namespace loomora {

namespace {

const char kHexDigits[] = "0123456789abcdef";

void append_unicode_escape(std::string &out, unsigned unit) {
    out += "\\u";
    for (int shift = 12; shift >= 0; shift -= 4)
        out += kHexDigits[(unit >> shift) & 0xF];
}

/* Appends `text` (UTF-8, possibly malformed) as a JSON string literal made of printable
   ASCII only: quotes, backslashes and control characters are escaped, other non-ASCII
   characters become \uXXXX (surrogate pairs above U+FFFF), malformed bytes become U+FFFD. */
void append_string(std::string &out, const std::string &text) {
    const std::u32string code_points = decode_utf8_lossy(text);
    out += '"';
    for (std::size_t i = 0; i < code_points.size(); ++i) {
        const char32_t cp = code_points[i];
        switch (cp) {
        case U'"':
            out += "\\\"";
            break;
        case U'\\':
            out += "\\\\";
            break;
        case U'\b':
            out += "\\b";
            break;
        case U'\f':
            out += "\\f";
            break;
        case U'\n':
            out += "\\n";
            break;
        case U'\r':
            out += "\\r";
            break;
        case U'\t':
            out += "\\t";
            break;
        default:
            if (cp >= 0x20 && cp < 0x7F) {
                out += static_cast<char>(cp);
            } else if (cp > 0xFFFF) {
                const char32_t v = cp - 0x10000;
                append_unicode_escape(out, static_cast<unsigned>(0xD800 + (v >> 10)));
                append_unicode_escape(out, static_cast<unsigned>(0xDC00 + (v & 0x3FF)));
            } else { // other control characters (including DEL) and non-ASCII
                append_unicode_escape(out, static_cast<unsigned>(cp));
            }
        }
    }
    out += '"';
}

/* Decimal digits of `value` (std::to_string is missing on some old MinGW runtimes). */
void append_unsigned(std::string &out, unsigned long long value) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (n > 0)
        out += digits[--n];
}

void append_bool(std::string &out, bool value) { out += value ? "true" : "false"; }

/* The comma-separated list as a JSON array of strings; empty items are skipped. */
void append_string_array(std::string &out, const std::string &list) {
    out += '[';
    bool first = true;
    std::size_t start = 0;
    while (start <= list.size()) {
        std::size_t end = list.find(',', start);
        if (end == std::string::npos)
            end = list.size();
        if (end > start) {
            if (!first)
                out += ',';
            append_string(out, list.substr(start, end - start));
            first = false;
        }
        start = end + 1;
    }
    out += ']';
}

/* The greeting the backend computes for `input`; false when greet fails (too long or
   invalid UTF-8), so that such a record never claims a match. */
bool greeting_matches(const std::string &input, const std::string &displayed_text) {
    try {
        return greet(input) == displayed_text;
    } catch (const InputTooLong &) {
        return false;
    } catch (const InvalidUtf8 &) {
        return false;
    }
}

const char *ui_variant_name(std::int32_t ui_variant) {
    switch (ui_variant) {
    case 1:
        return "legacy";
    case 2:
        return "modern";
    default:
        throw std::invalid_argument("unknown ui_variant");
    }
}

const char *core_name(std::int32_t core_kind) {
    switch (core_kind) {
    case 1:
        return "cpp";
    case 2:
        return "cpp-wasm";
    case 3:
        return "cpp-js";
    default:
        throw std::invalid_argument("unknown core_kind");
    }
}

std::string text(const char *s) { return s == nullptr ? std::string() : std::string(s); }

} // namespace

std::size_t probe_record(const char *platform_id, std::int32_t ui_variant, std::int32_t core_kind,
                         const char *input, const char *displayed_text, std::int32_t displayed,
                         std::uint32_t display_ms, std::int64_t rss_kb,
                         std::int32_t eol_warning_shown, const char *disabled_features,
                         const char *timestamp_utc, std::uint8_t *buf, std::size_t cap) {
    const std::string in = text(input);
    const std::string shown = text(displayed_text);

    // Fields in the order of probe-result.schema.json.
    std::string json = "{\"schema\":1,\"platform_id\":";
    append_string(json, text(platform_id));
    json += ",\"ui_variant\":";
    append_string(json, ui_variant_name(ui_variant));
    json += ",\"input\":";
    append_string(json, in);
    json += ",\"message\":";
    append_string(json, shown);
    json += ",\"greeting_matches\":";
    append_bool(json, greeting_matches(in, shown));
    json += ",\"core\":";
    append_string(json, core_name(core_kind));
    json += ",\"compiler\":";
    append_string(json, build_compiler());
    json += ",\"displayed\":";
    append_bool(json, displayed != 0);
    json += ",\"display_ms\":";
    append_unsigned(json, display_ms);
    json += ",\"peak_rss_kb\":";
    if (rss_kb < 0)
        json += "null"; // not measured
    else
        append_unsigned(json, static_cast<unsigned long long>(rss_kb));
    json += ",\"eol_warning_shown\":";
    append_bool(json, eol_warning_shown != 0);
    json += ",\"disabled_features\":";
    append_string_array(json, text(disabled_features));
    json += ",\"timestamp_utc\":";
    append_string(json, text(timestamp_utc));
    json += '}';

    if (buf != nullptr && cap != 0)
        std::memcpy(buf, json.data(), json.size() < cap ? json.size() : cap);
    return json.size();
}

} // namespace loomora
