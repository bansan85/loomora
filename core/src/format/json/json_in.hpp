// Reading the JSON entries of a .loomora file (project-format.md section 4 and 6).
//
// Two ways in, both strict (RFC 8259 text, UTF-8 checked, no BOM, no NaN, Infinity or number out
// of range, nesting depth limits().json_depth = 64, one root value, no NUL byte):
//   - parse_document: the whole text in memory, as a RapidJSON DOM; duplicate object keys are
//     rejected. For the small entries (manifest, frame, layout, image, result, spools).
//   - parse_sax: a stream of bytes (ByteReader, any chunk size) fed to a RapidJSON SAX handler
//     under a depth guard. For the large arrays (nails.json); the handler checks its own keys.
// Numbers are parsed with kParseFullPrecisionFlag (exact nearest double), the parser is iterative
// (kParseIterativeFlag: no recursion on the call stack, whatever the nesting).
//
// Every failure is a FormatError(LOOMORA_ERR_CORRUPT) carrying an Issue with one of these codes:
//   schema.syntax         not valid JSON (the message gives the byte offset)
//   schema.depth          nesting deeper than limits().json_depth
//   schema.duplicate_key  an object has the same key twice
//   schema.size           entry larger than limits().json_entry_max_bytes
//   schema.type           a value of the wrong JSON type
//   schema.missing        a required member is absent
//   schema.value          a value out of range or not in the allowed set
//   num.not_finite        NaN, Infinity or a number too large for a double
// The Issue carries the archive entry name given by the caller; item kind and id are not set
// (the caller knows them and may fill them in).
#ifndef LOOMORA_FORMAT_JSON_JSON_IN_HPP
#define LOOMORA_FORMAT_JSON_JSON_IN_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include <rapidjson/document.h>
#include <rapidjson/reader.h>

#include "format/io/source.hpp"
#include "format/limits.hpp"
#include "format/report.hpp"

namespace loomora {
namespace format {
namespace json {

typedef rapidjson::Document Document;
typedef rapidjson::Value Value;

// The RapidJSON parse flags of the format.
const unsigned kParseFlags = rapidjson::kParseFullPrecisionFlag | rapidjson::kParseIterativeFlag |
                             rapidjson::kParseValidateEncodingFlag;

// Decimal text of an unsigned number (std::to_string is missing on some old MinGW runtimes).
std::string dec(std::uint64_t value);

// Throws FormatError(LOOMORA_ERR_CORRUPT) with an error Issue of this code, entry and message.
[[noreturn]] void fail(const std::string &entry, const char *code, const std::string &message);

// ---- DOM -----------------------------------------------------------------------------------

// Parses `len` bytes at `data` into `doc`. Throws FormatError (codes above).
void parse_document(const char *data, std::size_t len, Document &doc, const std::string &entry);

// ---- SAX -----------------------------------------------------------------------------------

// A pull source of bytes: copies up to `cap` bytes into `buf` and returns how many, 0 at the end.
class ByteReader {
  public:
    virtual ~ByteReader() {}
    virtual std::size_t read(char *buf, std::size_t cap) = 0;
};

// A ByteReader over a caller buffer (no copy of the whole buffer).
class MemoryByteReader : public ByteReader {
  public:
    MemoryByteReader(const void *data, std::size_t len)
        : data_(static_cast<const char *>(data)), len_(len), pos_(0) {}
    std::size_t read(char *buf, std::size_t cap) override;

  private:
    const char *data_;
    std::size_t len_;
    std::size_t pos_;
};

// A ByteReader that gives one `prefix` character, then the bytes of `inner` (to parse a fragment
// of a document as if it were inside an array: prefix '[').
class PrefixByteReader : public ByteReader {
  public:
    PrefixByteReader(char prefix, ByteReader &inner) : prefix_(prefix), sent_(false), inner_(inner) {}
    std::size_t read(char *buf, std::size_t cap) override {
        if (cap == 0)
            return 0;
        if (!sent_) {
            sent_ = true;
            buf[0] = prefix_;
            return 1;
        }
        return inner_.read(buf, cap);
    }

  private:
    char prefix_;
    bool sent_;
    ByteReader &inner_;
};

// A ByteReader over [offset, offset + length) of a random-access Source.
class SourceByteReader : public ByteReader {
  public:
    SourceByteReader(const Source &source, std::uint64_t offset, std::uint64_t length)
        : source_(source), pos_(offset), end_(offset + length) {}
    std::size_t read(char *buf, std::size_t cap) override;

  private:
    const Source &source_;
    std::uint64_t pos_;
    std::uint64_t end_;
};

// A RapidJSON input stream over a ByteReader, buffered by chunks of `chunk` bytes.
class ReaderStream {
  public:
    typedef char Ch;
    explicit ReaderStream(ByteReader &in, std::size_t chunk = 64 * 1024);

    Ch Peek() const { return cur_ < end_ ? *cur_ : '\0'; }
    Ch Take() {
        if (cur_ >= end_)
            return '\0';
        const Ch c = *cur_++;
        ++consumed_;
        if (cur_ == end_)
            refill();
        return c;
    }
    std::size_t Tell() const { return consumed_; }
    // Not used: this stream is for parsing only (RapidJSON stream concept).
    Ch *PutBegin() { return nullptr; }
    void Put(Ch) {}
    void Flush() {}
    std::size_t PutEnd(Ch *) { return 0; }

    bool saw_nul() const { return saw_nul_; }

  private:
    void refill();

    ByteReader &in_;
    std::string buf_;
    const Ch *cur_;
    const Ch *end_;
    std::size_t consumed_;
    bool saw_nul_;
};

// Forwards the events to `handler`, refusing nesting deeper than `max_depth`.
template <typename Handler>
class DepthGuard {
  public:
    DepthGuard(Handler &h, std::uint32_t max_depth) : h_(h), max_(max_depth), depth_(0), too_deep_(false) {}
    bool too_deep() const { return too_deep_; }

    bool Null() { return h_.Null(); }
    bool Bool(bool b) { return h_.Bool(b); }
    bool Int(int i) { return h_.Int(i); }
    bool Uint(unsigned i) { return h_.Uint(i); }
    bool Int64(std::int64_t i) { return h_.Int64(i); }
    bool Uint64(std::uint64_t i) { return h_.Uint64(i); }
    bool Double(double d) { return h_.Double(d); }
    bool RawNumber(const char *s, rapidjson::SizeType n, bool c) { return h_.RawNumber(s, n, c); }
    bool String(const char *s, rapidjson::SizeType n, bool c) { return h_.String(s, n, c); }
    bool Key(const char *s, rapidjson::SizeType n, bool c) { return h_.Key(s, n, c); }
    bool StartObject() { return enter() && h_.StartObject(); }
    bool EndObject(rapidjson::SizeType n) { --depth_; return h_.EndObject(n); }
    bool StartArray() { return enter() && h_.StartArray(); }
    bool EndArray(rapidjson::SizeType n) { --depth_; return h_.EndArray(n); }

  private:
    bool enter() {
        if (++depth_ > max_) {
            too_deep_ = true;
            return false;
        }
        return true;
    }
    Handler &h_;
    std::uint32_t max_;
    std::uint32_t depth_;
    bool too_deep_;
};

// Throws FormatError for a failed RapidJSON parse (called by parse_sax; `text` is the whole
// input when it is in memory, to recognise NaN and Infinity, else null).
[[noreturn]] void throw_parse_error(const rapidjson::ParseResult &result, const char *text,
                                    std::size_t len, const std::string &entry);

// Parses `stream` into `handler` (RapidJSON SAX handler: Null, Bool, Int, Uint, Int64, Uint64,
// Double, String, StartObject, Key, EndObject, StartArray, EndArray; RawNumber is never called).
// Returns true when the whole document was parsed. When the handler returns false the parse stops
// and the function returns false (a deliberate early end, e.g. a range read); the handler must
// then report its own error if it was not deliberate. Throws FormatError for a syntax error, a
// depth overflow, a NUL byte or a document larger than the entry limit. The caller owns the
// stream, so a handler may ask it for the byte offset: with the iterative parser the
// StartObject and StartArray events come before the `{` or `[` is taken, so stream.Tell() is the
// offset of that character.
template <typename Handler>
bool parse_sax_stream(ReaderStream &stream, Handler &handler, const std::string &entry) {
    DepthGuard<Handler> guard(handler, limits().json_depth);
    rapidjson::Reader reader;
    const rapidjson::ParseResult result = reader.Parse<kParseFlags>(stream, guard);
    if (guard.too_deep())
        fail(entry, "schema.depth", "JSON nesting deeper than " + dec(limits().json_depth));
    if (stream.saw_nul())
        fail(entry, "schema.syntax", "JSON text contains a NUL byte");
    if (stream.Tell() > limits().json_entry_max_bytes)
        fail(entry, "schema.size", "JSON entry larger than the limit");
    if (result.IsError()) {
        if (result.Code() == rapidjson::kParseErrorTermination)
            return false;
        throw_parse_error(result, nullptr, 0, entry);
    }
    return true;
}

// parse_sax_stream over a new stream of `in`.
template <typename Handler>
bool parse_sax(ByteReader &in, Handler &handler, const std::string &entry) {
    ReaderStream stream(in);
    return parse_sax_stream(stream, handler, entry);
}

// ---- Typed access to a parsed DOM ------------------------------------------------------------
// Each function throws FormatError (schema.type, schema.missing or schema.value) with a message
// naming `path`, the dotted path of the value (e.g. "outline.polyline[3]").

// An unsigned 32-bit integer (an identifier): a JSON integer, or a double with an integral value
// (1.0), in 0..4294967295.
std::uint32_t as_u32(const Value &v, const std::string &entry, const std::string &path);
// An integer in 0..2^53 (counts and sizes; larger values are never written).
std::uint64_t as_u64(const Value &v, const std::string &entry, const std::string &path);
// Any JSON number as a double (finite by construction).
double as_number(const Value &v, const std::string &entry, const std::string &path);
bool as_bool(const Value &v, const std::string &entry, const std::string &path);
// A string of at most `max_bytes` bytes (UTF-8 length).
std::string as_string(const Value &v, const std::string &entry, const std::string &path,
                      std::size_t max_bytes);

// Members of one JSON object, with errors that name the entry and the path.
class Fields {
  public:
    // Throws schema.type when `object` is not an object.
    Fields(const Value &object, const std::string &entry, const std::string &path);

    const Value *find(const char *name) const; // null when absent
    bool has(const char *name) const { return find(name) != nullptr; }
    const Value &get(const char *name) const;  // schema.missing when absent

    std::uint32_t u32(const char *name) const;
    std::uint64_t u64(const char *name) const;
    double number(const char *name) const;
    bool boolean(const char *name) const;
    std::string string(const char *name, std::size_t max_bytes) const;
    // A string that must be one of `allowed` (schema.value otherwise); returns its index.
    std::size_t one_of(const char *name, const char *const *allowed, std::size_t count) const;
    Fields object(const char *name) const;
    const Value &array(const char *name) const; // schema.type when not an array

    const std::string &entry() const { return entry_; }
    std::string path_of(const char *name) const;

  private:
    const Value &object_;
    std::string entry_;
    std::string path_;
};

} // namespace json
} // namespace format
} // namespace loomora

#endif
