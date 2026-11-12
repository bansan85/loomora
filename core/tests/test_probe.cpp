// loomora_probe_record: the produced JSON line is parsed (with a tiny strict parser) and
// compared field by field with the shape and the golden vectors of core/tests/vectors.json.
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "../include/loomora.h"
#include "vectors.inc"

namespace {

// ---- minimal JSON model and parser (objects of strings, integers, booleans, null, arrays) ----

struct Json {
    enum Kind { Null, Bool, Int, Str, Arr } kind = Null;
    bool boolean = false;
    long long integer = 0;
    std::string text;
    std::vector<Json> items;
};

class Parser {
  public:
    explicit Parser(const std::string &text) : t_(text) {}

    // Parses one top-level object with unique keys; throws std::runtime_error otherwise.
    std::map<std::string, Json> object() {
        std::map<std::string, Json> result;
        skip();
        expect('{');
        skip();
        if (peek() == '}') {
            ++p_;
        } else {
            for (;;) {
                skip();
                const std::string key = string();
                skip();
                expect(':');
                Json value = value_();
                if (!result.insert(std::make_pair(key, value)).second)
                    fail("duplicate key " + key);
                skip();
                if (peek() == ',') {
                    ++p_;
                    continue;
                }
                expect('}');
                break;
            }
        }
        skip();
        if (p_ != t_.size())
            fail("trailing characters");
        return result;
    }

  private:
    const std::string &t_;
    std::size_t p_ = 0;

    [[noreturn]] void fail(const std::string &why) const {
        throw std::runtime_error(why + " at offset " + std::to_string(p_));
    }
    char peek() const { return p_ < t_.size() ? t_[p_] : '\0'; }
    void skip() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t'))
            ++p_;
    }
    void expect(char c) {
        if (peek() != c)
            fail(std::string("expected '") + c + "'");
        ++p_;
    }

    Json value_() {
        skip();
        Json v;
        const char c = peek();
        if (c == '"') {
            v.kind = Json::Str;
            v.text = string();
        } else if (c == '[') {
            ++p_;
            v.kind = Json::Arr;
            skip();
            if (peek() == ']') {
                ++p_;
            } else {
                for (;;) {
                    v.items.push_back(value_());
                    skip();
                    if (peek() == ',') {
                        ++p_;
                        continue;
                    }
                    expect(']');
                    break;
                }
            }
        } else if (t_.compare(p_, 4, "true") == 0) {
            p_ += 4;
            v.kind = Json::Bool;
            v.boolean = true;
        } else if (t_.compare(p_, 5, "false") == 0) {
            p_ += 5;
            v.kind = Json::Bool;
        } else if (t_.compare(p_, 4, "null") == 0) {
            p_ += 4;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const std::size_t start = p_;
            if (c == '-')
                ++p_;
            const std::size_t digits = p_;
            while (peek() >= '0' && peek() <= '9')
                ++p_;
            if (p_ == digits || (t_[digits] == '0' && p_ - digits > 1))
                fail("malformed integer");
            v.kind = Json::Int;
            v.integer = std::strtoll(t_.substr(start, p_ - start).c_str(), nullptr, 10);
        } else {
            fail("unexpected character");
        }
        return v;
    }

    unsigned hex4() {
        if (p_ + 4 > t_.size())
            fail("short \\u escape");
        unsigned code = 0;
        for (int i = 0; i < 4; ++i, ++p_) {
            const char h = t_[p_];
            code <<= 4;
            if (h >= '0' && h <= '9')
                code |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f')
                code |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
                code |= static_cast<unsigned>(h - 'A' + 10);
            else
                fail("bad hex digit");
        }
        return code;
    }

    static void utf8(std::string &out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::string string() {
        expect('"');
        std::string out;
        for (;;) {
            if (p_ >= t_.size())
                fail("unterminated string");
            const char c = t_[p_++];
            if (c == '"')
                return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            const char e = p_ < t_.size() ? t_[p_++] : '\0';
            switch (e) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                unsigned cp = hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF) { // high surrogate: a low one must follow
                    if (t_.compare(p_, 2, "\\u") != 0)
                        fail("lone high surrogate");
                    p_ += 2;
                    const unsigned low = hex4();
                    if (low < 0xDC00 || low > 0xDFFF)
                        fail("bad low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    fail("lone low surrogate");
                }
                utf8(out, cp);
                break;
            }
            default:
                fail("bad escape");
            }
        }
    }
};

// ---- helpers ----

const std::uint8_t kSentinel = 0xA5;

/* Calls loomora_probe_record with the vector's arguments; returns the needed length. */
std::size_t call(const vectors::ProbeVector &v, std::uint8_t *buf, std::size_t cap) {
    return loomora_probe_record(v.platform_id.c_str(), v.ui_variant, v.core_kind, v.input.c_str(),
                                v.displayed_text.c_str(), v.displayed, v.display_ms, v.rss_kb,
                                v.eol_warning_shown, v.disabled_features.c_str(),
                                v.timestamp_utc.c_str(), buf, cap);
}

/* The complete record of a vector. */
std::string record(const vectors::ProbeVector &v) {
    std::vector<std::uint8_t> probe(1);
    const std::size_t needed = call(v, probe.data(), 0);
    std::vector<std::uint8_t> buffer(needed + 16, kSentinel);
    REQUIRE(call(v, buffer.data(), needed) == needed);
    for (std::size_t k = needed; k < buffer.size(); ++k)
        REQUIRE(buffer[k] == kSentinel); // never writes past `needed`
    return std::string(buffer.begin(), buffer.begin() + needed);
}

const vectors::ProbeVector &find(const std::string &id) {
    for (std::size_t i = 0; i < vectors::kProbeVectorsCount; ++i)
        if (id == vectors::kProbeVectors[i].id)
            return vectors::kProbeVectors[i];
    FAIL("no probe vector " + id);
    return vectors::kProbeVectors[0]; // not reached
}

/* One line, ASCII only (non-ASCII characters must be \u escapes), no raw control character.
   One trailing newline is tolerated and stripped. */
std::string single_ascii_line(const std::string &raw) {
    std::string line = raw;
    if (!line.empty() && line[line.size() - 1] == '\n')
        line.erase(line.size() - 1);
    for (std::size_t i = 0; i < line.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        INFO("byte " << i << " = " << static_cast<unsigned>(c));
        REQUIRE(c >= 0x20);
        REQUIRE(c < 0x80);
    }
    return line;
}

bool type_ok(const std::string &type, const Json &v) {
    if (type == "int")
        return v.kind == Json::Int;
    if (type == "string")
        return v.kind == Json::Str;
    if (type == "bool")
        return v.kind == Json::Bool;
    if (type == "int_or_null")
        return v.kind == Json::Int || v.kind == Json::Null;
    if (type == "string_array") {
        if (v.kind != Json::Arr)
            return false;
        for (std::size_t i = 0; i < v.items.size(); ++i)
            if (v.items[i].kind != Json::Str)
                return false;
        return true;
    }
    return false;
}

} // namespace

TEST_CASE("probe: every vector yields a record of the expected shape and content", "[probe]") {
    for (std::size_t i = 0; i < vectors::kProbeVectorsCount; ++i) {
        const vectors::ProbeVector &v = vectors::kProbeVectors[i];
        INFO("vector: " << v.id);

        const std::string line = single_ascii_line(record(v));
        std::map<std::string, Json> obj;
        try {
            obj = Parser(line).object();
        } catch (const std::exception &e) {
            FAIL("invalid JSON (" << e.what() << "): " << line);
        }

        // Shape: exactly the declared fields, each with its declared type.
        REQUIRE(obj.size() == vectors::kProbeFieldsCount);
        for (std::size_t f = 0; f < vectors::kProbeFieldsCount; ++f) {
            INFO("field: " << vectors::kProbeFields[f].name);
            const std::map<std::string, Json>::const_iterator it =
                obj.find(vectors::kProbeFields[f].name);
            REQUIRE(it != obj.end());
            REQUIRE(type_ok(vectors::kProbeFields[f].type, it->second));
        }

        // Content.
        REQUIRE(obj["schema"].integer == 1);
        REQUIRE(obj["platform_id"].text == v.expect_platform_id);
        REQUIRE(obj["ui_variant"].text == v.expect_ui_variant);
        REQUIRE(obj["input"].text == v.expect_input);
        REQUIRE(obj["message"].text == v.expect_message);
        REQUIRE(obj["greeting_matches"].boolean == v.expect_greeting_matches);
        REQUIRE(obj["core"].text == v.expect_core);
        REQUIRE(obj["compiler"].text == loomora_build_compiler());
        REQUIRE(obj["displayed"].boolean == v.expect_displayed);
        REQUIRE(obj["display_ms"].integer >= 0);
        REQUIRE(static_cast<std::uint64_t>(obj["display_ms"].integer) == v.expect_display_ms);
        if (v.expect_peak_rss_null) {
            REQUIRE(obj["peak_rss_kb"].kind == Json::Null);
        } else {
            REQUIRE(obj["peak_rss_kb"].kind == Json::Int);
            REQUIRE(obj["peak_rss_kb"].integer == v.expect_peak_rss_kb);
        }
        REQUIRE(obj["eol_warning_shown"].boolean == v.expect_eol_warning_shown);
        REQUIRE(obj["timestamp_utc"].text == v.expect_timestamp_utc);

        const std::vector<Json> &features = obj["disabled_features"].items;
        REQUIRE(features.size() == v.expect_disabled_features.size());
        for (std::size_t k = 0; k < features.size(); ++k)
            REQUIRE(features[k].text == v.expect_disabled_features[k]);
    }
}

TEST_CASE("probe: greeting_matches is true and false", "[probe]") {
    const std::string match = record(find("native_legacy_match"));
    const std::string mismatch = record(find("mismatch_is_reported"));
    REQUIRE(Parser(single_ascii_line(match)).object()["greeting_matches"].boolean == true);
    REQUIRE(Parser(single_ascii_line(mismatch)).object()["greeting_matches"].boolean == false);
}

TEST_CASE("probe: truncation copies a prefix and returns the needed length", "[probe]") {
    const char *ids[] = {"native_legacy_match", "non_ascii_is_escaped_to_ascii",
                         "input_too_long_never_matches"};
    for (std::size_t n = 0; n < sizeof(ids) / sizeof(ids[0]); ++n) {
        const vectors::ProbeVector &v = find(ids[n]);
        INFO("vector: " << v.id);
        const std::string full = record(v);
        REQUIRE(full.size() > 100);

        const std::size_t caps[] = {
            0, 1, 10, full.size() / 2, full.size() - 1, full.size(), full.size() + 50};
        for (std::size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); ++c) {
            const std::size_t cap = caps[c];
            INFO("cap " << cap);
            std::vector<std::uint8_t> buffer(cap + 16, kSentinel);
            REQUIRE(call(v, buffer.data(), cap) == full.size()); // always the total length needed

            const std::size_t written = cap < full.size() ? cap : full.size();
            REQUIRE(std::string(buffer.begin(), buffer.begin() + written) ==
                    full.substr(0, written));
            for (std::size_t k = written; k < buffer.size(); ++k)
                REQUIRE(buffer[k] == kSentinel); // nothing beyond min(cap, needed)
        }
    }
}

TEST_CASE("probe: quotes, backslashes and control characters are escaped", "[probe]") {
    const vectors::ProbeVector &v = find("json_escaping_of_quotes_backslashes_and_controls");
    const std::string line = single_ascii_line(record(v));

    // Raw text of the input must not appear: the quote and the backslash are escaped,
    // and the newline and tab never appear as raw characters (checked by single_ascii_line).
    REQUIRE(line.find("say \"hi\"") == std::string::npos);
    REQUIRE(line.find("say \\\"hi\\\" \\\\ back") != std::string::npos);

    // The line still decodes to the original text.
    const std::map<std::string, Json> obj = Parser(line).object();
    REQUIRE(obj.find("input")->second.text == v.expect_input);
    REQUIRE(obj.find("message")->second.text == v.expect_message);
}

TEST_CASE("probe: non-ASCII text is written as \\u escapes", "[probe]") {
    std::string line = single_ascii_line(record(find("non_ascii_is_escaped_to_ascii")));
    for (std::size_t i = 0; i < line.size(); ++i) // hex digits may be written in upper case
        if (line[i] >= 'A' && line[i] <= 'Z')
            line[i] = static_cast<char>(line[i] - 'A' + 'a');
    REQUIRE(line.find("\\u00e9") != std::string::npos); // e acute
}
