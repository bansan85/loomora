// Writing the JSON entries of a .loomora file (project-format.md section 4 and 6).
//
// Compact text (no whitespace outside strings), UTF-8, object keys in the order the caller writes
// them (the caller follows the `properties` order of the schema). Numbers:
//   - unsigned and signed integers are written as plain decimal integers;
//   - a double is written with RapidJSON's shortest round-trip conversion (Grisu2, the same text on
//     every platform and compiler), in RapidJSON's layout (`0.001`, `1e-7`, `1.5e22`) except that a
//     whole value has no `.0` (`500`, not `500.0`), as in the examples of the contract. `-0` is
//     written as `0`. The text parses back to exactly the same double (tests: 100,000 random
//     doubles, denormals, 1e-7, 1e22).
//   - NaN and Infinity cannot be written: FormatError(LOOMORA_ERR_INVALID_ITEM), code num.not_finite.
// A string that is not valid UTF-8 is refused the same way (code schema.value).
// Structure errors (a value where a key is expected, an unclosed object) are programming errors,
// not checked at run time in a release build.
#ifndef LOOMORA_FORMAT_JSON_JSON_OUT_HPP
#define LOOMORA_FORMAT_JSON_JSON_OUT_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>

namespace loomora {
namespace format {
namespace json {

// format_double into `buf` (at least 40 bytes, not NUL-terminated); returns the length.
std::size_t format_double_to(double value, char *buf);
// The text of a double as the writer writes it (no NaN or Infinity: throws like number()).
std::string format_double(double value);

namespace detail {
struct StringSink {
    typedef char Ch;
    explicit StringSink(std::string &s) : s_(&s) {}
    void Put(char c) { s_->push_back(c); }
    void Flush() {}
    std::string *s_;
};
} // namespace detail

class JsonWriter {
  public:
    // Appends to `out`; the string must stay alive and untouched while the writer is used.
    explicit JsonWriter(std::string &out);

    void begin_object();
    void end_object();
    void begin_array();
    void end_array();
    // The name of the next member of an object.
    void key(const char *name);
    void key(const std::string &name) { key(name.c_str(), name.size()); }
    void key(const char *name, std::size_t len);

    void null_value();
    void boolean(bool value);
    void uint(std::uint64_t value);
    void integer(std::int64_t value);
    void number(double value);
    void string(const char *text, std::size_t len);
    void string(const std::string &text) { string(text.data(), text.size()); }
    // An already compact JSON text (a free object kept as text, such as `params`, `construction`
    // or `extensions`). It must be one valid JSON value; it is copied as it is.
    void raw(const std::string &json_text);
    // A parsed value, written compact with the number rules above.
    void value(const rapidjson::Value &v);

    // `key(name)` then the value.
    void field_uint(const char *name, std::uint64_t v) { key(name); uint(v); }
    void field_number(const char *name, double v) { key(name); number(v); }
    void field_bool(const char *name, bool v) { key(name); boolean(v); }
    void field_string(const char *name, const std::string &v) { key(name); string(v); }
    void field_raw(const char *name, const std::string &json_text) { key(name); raw(json_text); }

  private:
    typedef rapidjson::Writer<detail::StringSink, rapidjson::UTF8<>, rapidjson::UTF8<>,
                              rapidjson::CrtAllocator, rapidjson::kWriteValidateEncodingFlag>
        Impl;
    detail::StringSink sink_;
    Impl writer_;
};

} // namespace json
} // namespace format
} // namespace loomora

#endif
