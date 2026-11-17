#include "format/json/json_out.hpp"

#include <cstring>

#include <rapidjson/internal/dtoa.h>

#include "format/report.hpp"
#include "loomora.h"

namespace loomora {
namespace format {
namespace json {

namespace {

[[noreturn]] void refuse(const char *code, const char *message) {
    Issue issue;
    issue.severity = kError;
    issue.code = code;
    issue.message = message;
    throw FormatError(LOOMORA_ERR_INVALID_ITEM, issue);
}

} // namespace

std::size_t format_double_to(double value, char *buf) {
    if (!(value - value == 0.0))
        refuse("num.not_finite", "a number to write is NaN or Infinity");
    if (value == 0.0) { // also -0
        buf[0] = '0';
        return 1;
    }
    char *end = rapidjson::internal::dtoa(value, buf, 324);
    std::size_t n = static_cast<std::size_t>(end - buf);
    if (n >= 2 && buf[n - 2] == '.' && buf[n - 1] == '0')
        n -= 2; // a whole value: no ".0"
    return n;
}

std::string format_double(double value) {
    char buf[40];
    const std::size_t n = format_double_to(value, buf);
    return std::string(buf, n);
}

JsonWriter::JsonWriter(std::string &out) : sink_(out), writer_(sink_) {}

void JsonWriter::begin_object() { writer_.StartObject(); }
void JsonWriter::end_object() { writer_.EndObject(); }
void JsonWriter::begin_array() { writer_.StartArray(); }
void JsonWriter::end_array() { writer_.EndArray(); }

void JsonWriter::key(const char *name, std::size_t len) {
    if (!writer_.Key(name, static_cast<rapidjson::SizeType>(len)))
        refuse("schema.value", "a key is not valid UTF-8");
}
void JsonWriter::key(const char *name) { key(name, std::strlen(name)); }

void JsonWriter::null_value() { writer_.Null(); }
void JsonWriter::boolean(bool value) { writer_.Bool(value); }
void JsonWriter::uint(std::uint64_t value) { writer_.Uint64(value); }
void JsonWriter::integer(std::int64_t value) { writer_.Int64(value); }

void JsonWriter::number(double value) {
    char buf[40];
    const std::size_t n = format_double_to(value, buf);
    writer_.RawValue(buf, n, rapidjson::kNumberType);
}

void JsonWriter::string(const char *text, std::size_t len) {
    if (!writer_.String(text, static_cast<rapidjson::SizeType>(len)))
        refuse("schema.value", "a string is not valid UTF-8");
}

void JsonWriter::raw(const std::string &json_text) {
    rapidjson::Type type = rapidjson::kNumberType;
    if (!json_text.empty()) {
        if (json_text[0] == '{')
            type = rapidjson::kObjectType;
        else if (json_text[0] == '[')
            type = rapidjson::kArrayType;
        else if (json_text[0] == '"')
            type = rapidjson::kStringType;
    }
    writer_.RawValue(json_text.data(), json_text.size(), type);
}

void JsonWriter::value(const rapidjson::Value &v) {
    switch (v.GetType()) {
    case rapidjson::kNullType:
        null_value();
        break;
    case rapidjson::kFalseType:
    case rapidjson::kTrueType:
        boolean(v.GetBool());
        break;
    case rapidjson::kObjectType:
        begin_object();
        for (rapidjson::Value::ConstMemberIterator it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
            key(it->name.GetString(), it->name.GetStringLength());
            value(it->value);
        }
        end_object();
        break;
    case rapidjson::kArrayType:
        begin_array();
        for (rapidjson::SizeType i = 0; i < v.Size(); ++i)
            value(v[i]);
        end_array();
        break;
    case rapidjson::kStringType:
        string(v.GetString(), v.GetStringLength());
        break;
    default: // number
        if (v.IsUint64())
            uint(v.GetUint64());
        else if (v.IsInt64())
            integer(v.GetInt64());
        else
            number(v.GetDouble());
        break;
    }
}

} // namespace json
} // namespace format
} // namespace loomora
