#include "format/json/json_in.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

#include <rapidjson/error/en.h>
#include <rapidjson/memorystream.h>

#include "loomora.h"

namespace loomora {
namespace format {
namespace json {

std::string dec(std::uint64_t value) {
    char buf[24];
    int n = 0;
    do {
        buf[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    std::string s;
    while (n > 0)
        s.push_back(buf[--n]);
    return s;
}

void fail(const std::string &entry, const char *code, const std::string &message) {
    Issue issue;
    issue.severity = kError;
    issue.code = code;
    issue.entry = entry;
    issue.message = message;
    throw FormatError(LOOMORA_ERR_CORRUPT, issue);
}

namespace {

bool looks_like_non_finite(const char *text, std::size_t len, std::size_t offset) {
    // The parser reports "invalid value" near the start of NaN, Infinity or inf: look at the
    // few characters around the offset.
    std::string win;
    const std::size_t from = offset > 2 ? offset - 2 : 0;
    for (std::size_t i = from; i < len && i < offset + 9; ++i)
        win.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[i]))));
    return win.find("nan") != std::string::npos || win.find("inf") != std::string::npos;
}

// The nesting depth and NUL byte check, before any DOM is built.
void scan_text(const char *data, std::size_t len, const std::string &entry) {
    const std::uint32_t max_depth = limits().json_depth;
    std::uint32_t depth = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < len; ++i) {
        const char c = data[i];
        if (c == '\0')
            fail(entry, "schema.syntax", "JSON text contains a NUL byte at offset " + dec(i));
        if (in_string) {
            if (c == '\\')
                ++i; // the escaped character (a NUL after a backslash is caught below by the parser)
            else if (c == '"')
                in_string = false;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max_depth)
                fail(entry, "schema.depth", "JSON nesting deeper than " + dec(max_depth));
        } else if ((c == '}' || c == ']') && depth > 0) {
            --depth;
        }
    }
}

// Rejects an object with the same key twice. The depth is already bounded, so recursion is safe.
void check_duplicates(const Value &v, const std::string &entry, const std::string &path) {
    if (v.IsArray()) {
        for (rapidjson::SizeType i = 0; i < v.Size(); ++i)
            check_duplicates(v[i], entry, path + "[" + dec(i) + "]");
    } else if (v.IsObject()) {
        const std::size_t n = v.MemberCount();
        if (n <= 24) {
            for (Value::ConstMemberIterator a = v.MemberBegin(); a != v.MemberEnd(); ++a)
                for (Value::ConstMemberIterator b = a + 1; b != v.MemberEnd(); ++b)
                    if (a->name == b->name)
                        fail(entry, "schema.duplicate_key",
                             "duplicate key \"" + std::string(a->name.GetString(), a->name.GetStringLength()) +
                                 "\" in " + (path.empty() ? "the root object" : path));
        } else { // large object: sort the members by name
            std::vector<const Value::Member *> m;
            m.reserve(n);
            for (Value::ConstMemberIterator a = v.MemberBegin(); a != v.MemberEnd(); ++a)
                m.push_back(&*a);
            struct ByName {
                bool operator()(const Value::Member *x, const Value::Member *y) const {
                    const std::size_t lx = x->name.GetStringLength(), ly = y->name.GetStringLength();
                    const int c = std::memcmp(x->name.GetString(), y->name.GetString(), lx < ly ? lx : ly);
                    return c != 0 ? c < 0 : lx < ly;
                }
            };
            std::sort(m.begin(), m.end(), ByName());
            for (std::size_t i = 1; i < m.size(); ++i)
                if (m[i - 1]->name == m[i]->name)
                    fail(entry, "schema.duplicate_key",
                         "duplicate key \"" + std::string(m[i]->name.GetString(), m[i]->name.GetStringLength()) +
                             "\" in " + (path.empty() ? "the root object" : path));
        }
        for (Value::ConstMemberIterator a = v.MemberBegin(); a != v.MemberEnd(); ++a)
            check_duplicates(a->value, entry, path + "." + std::string(a->name.GetString(), a->name.GetStringLength()));
    }
}

} // namespace

void throw_parse_error(const rapidjson::ParseResult &result, const char *text, std::size_t len,
                       const std::string &entry) {
    const std::string where = " at byte " + dec(result.Offset());
    if (result.Code() == rapidjson::kParseErrorNumberTooBig)
        fail(entry, "num.not_finite", "number out of range for a double" + where);
    if (text != nullptr && result.Code() == rapidjson::kParseErrorValueInvalid &&
        looks_like_non_finite(text, len, result.Offset()))
        fail(entry, "num.not_finite", "NaN and Infinity are not allowed" + where);
    fail(entry, "schema.syntax", std::string("invalid JSON: ") + rapidjson::GetParseError_En(result.Code()) + where);
}

void parse_document(const char *data, std::size_t len, Document &doc, const std::string &entry) {
    if (len > limits().json_entry_max_bytes)
        fail(entry, "schema.size", "JSON entry larger than the limit");
    scan_text(data, len, entry);
    rapidjson::MemoryStream stream(data, len);
    doc.ParseStream<kParseFlags>(stream);
    if (doc.HasParseError()) {
        rapidjson::ParseResult r(doc.GetParseError(), doc.GetErrorOffset());
        throw_parse_error(r, data, len, entry);
    }
    check_duplicates(doc, entry, std::string());
}

// ---- byte readers and stream ---------------------------------------------------------------

std::size_t MemoryByteReader::read(char *buf, std::size_t cap) {
    const std::size_t n = (len_ - pos_ < cap) ? len_ - pos_ : cap;
    if (n != 0)
        std::memcpy(buf, data_ + pos_, n);
    pos_ += n;
    return n;
}

std::size_t SourceByteReader::read(char *buf, std::size_t cap) {
    if (pos_ >= end_)
        return 0;
    const std::uint64_t left = end_ - pos_;
    const std::size_t want = left < cap ? static_cast<std::size_t>(left) : cap;
    const std::size_t n = source_.read_at(pos_, buf, want);
    pos_ += n;
    if (n == 0)
        pos_ = end_; // the source ended before the announced length
    return n;
}

ReaderStream::ReaderStream(ByteReader &in, std::size_t chunk)
    : in_(in), buf_(chunk == 0 ? 1 : chunk, '\0'), cur_(nullptr), end_(nullptr), consumed_(0),
      saw_nul_(false) {
    refill();
}

void ReaderStream::refill() {
    if (saw_nul_) { // the text ended at a NUL byte: read no further
        cur_ = end_;
        return;
    }
    const std::size_t n = in_.read(&buf_[0], buf_.size());
    cur_ = buf_.data();
    end_ = cur_ + n;
    if (n != 0 && std::memchr(cur_, 0, n) != nullptr) {
        saw_nul_ = true;
        end_ = static_cast<const char *>(std::memchr(cur_, 0, n)); // the text ends at the NUL
    }
}

// ---- typed access -----------------------------------------------------------------------------

namespace {

const char *type_name(const Value &v) {
    switch (v.GetType()) {
    case rapidjson::kNullType: return "null";
    case rapidjson::kFalseType:
    case rapidjson::kTrueType: return "a boolean";
    case rapidjson::kObjectType: return "an object";
    case rapidjson::kArrayType: return "an array";
    case rapidjson::kStringType: return "a string";
    default: return "a number";
    }
}

std::string show(const std::string &path) { return path.empty() ? std::string("the value") : path; }

[[noreturn]] void wrong_type(const Value &v, const std::string &entry, const std::string &path, const char *want) {
    fail(entry, "schema.type", show(path) + " must be " + want + ", found " + type_name(v));
}

const double kTwo53 = 9007199254740992.0;

} // namespace

std::uint32_t as_u32(const Value &v, const std::string &entry, const std::string &path) {
    if (v.IsUint())
        return v.GetUint();
    if (!v.IsNumber())
        wrong_type(v, entry, path, "an unsigned 32-bit integer");
    if (v.IsDouble()) {
        const double d = v.GetDouble();
        if (d >= 0 && d <= 4294967295.0 && d == std::floor(d))
            return static_cast<std::uint32_t>(d);
    }
    fail(entry, "schema.value", show(path) + " must be an integer from 0 to 4294967295");
}

std::uint64_t as_u64(const Value &v, const std::string &entry, const std::string &path) {
    if (!v.IsNumber())
        wrong_type(v, entry, path, "a non-negative integer");
    if (v.IsUint64()) {
        if (v.GetUint64() <= (1ull << 53))
            return v.GetUint64();
    } else if (v.IsDouble()) {
        const double d = v.GetDouble();
        if (d >= 0 && d <= kTwo53 && d == std::floor(d))
            return static_cast<std::uint64_t>(d);
    }
    fail(entry, "schema.value", show(path) + " must be an integer from 0 to 2^53");
}

double as_number(const Value &v, const std::string &entry, const std::string &path) {
    if (!v.IsNumber())
        wrong_type(v, entry, path, "a number");
    const double d = v.GetDouble();
    if (!(d - d == 0.0)) // not finite (cannot come from the parser; defensive)
        fail(entry, "num.not_finite", show(path) + " is not finite");
    return d;
}

bool as_bool(const Value &v, const std::string &entry, const std::string &path) {
    if (!v.IsBool())
        wrong_type(v, entry, path, "a boolean");
    return v.GetBool();
}

std::string as_string(const Value &v, const std::string &entry, const std::string &path, std::size_t max_bytes) {
    if (!v.IsString())
        wrong_type(v, entry, path, "a string");
    if (v.GetStringLength() > max_bytes)
        fail(entry, "schema.value", show(path) + " is longer than " + dec(max_bytes) + " bytes");
    return std::string(v.GetString(), v.GetStringLength());
}

Fields::Fields(const Value &object, const std::string &entry, const std::string &path)
    : object_(object), entry_(entry), path_(path) {
    if (!object.IsObject())
        wrong_type(object, entry, path, "an object");
}

std::string Fields::path_of(const char *name) const { return path_.empty() ? std::string(name) : path_ + "." + name; }

const Value *Fields::find(const char *name) const {
    Value::ConstMemberIterator it = object_.FindMember(name);
    return it == object_.MemberEnd() ? nullptr : &it->value;
}

const Value &Fields::get(const char *name) const {
    const Value *v = find(name);
    if (v == nullptr)
        fail(entry_, "schema.missing", "required member " + path_of(name) + " is missing");
    return *v;
}

std::uint32_t Fields::u32(const char *name) const { return as_u32(get(name), entry_, path_of(name)); }
std::uint64_t Fields::u64(const char *name) const { return as_u64(get(name), entry_, path_of(name)); }
double Fields::number(const char *name) const { return as_number(get(name), entry_, path_of(name)); }
bool Fields::boolean(const char *name) const { return as_bool(get(name), entry_, path_of(name)); }

std::string Fields::string(const char *name, std::size_t max_bytes) const {
    return as_string(get(name), entry_, path_of(name), max_bytes);
}

std::size_t Fields::one_of(const char *name, const char *const *allowed, std::size_t count) const {
    const std::string s = as_string(get(name), entry_, path_of(name), 64);
    for (std::size_t i = 0; i < count; ++i)
        if (s == allowed[i])
            return i;
    std::string list;
    for (std::size_t i = 0; i < count; ++i)
        list += (i ? ", " : "") + std::string(allowed[i]);
    fail(entry_, "schema.value", path_of(name) + " must be one of " + list + ", found \"" + s + "\"");
}

Fields Fields::object(const char *name) const { return Fields(get(name), entry_, path_of(name)); }

const Value &Fields::array(const char *name) const {
    const Value &v = get(name);
    if (!v.IsArray())
        wrong_type(v, entry_, path_of(name), "an array");
    return v;
}

} // namespace json
} // namespace format
} // namespace loomora
