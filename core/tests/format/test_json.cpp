// The JSON layer: strict reading (depth, duplicate keys, NaN, numbers), compact writing, SAX.
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "format/json/json_in.hpp"
#include "format/json/json_out.hpp"
#include "loomora.h"

using namespace loomora::format;
using namespace loomora::format::json;

namespace {

// Parses `text`; returns the issue code of the failure, "" when it parses.
std::string parse_code(const std::string &text) {
    Document doc;
    try {
        parse_document(text.data(), text.size(), doc, "test.json");
    } catch (const FormatError &e) {
        CHECK(e.code() == LOOMORA_ERR_CORRUPT);
        REQUIRE(e.has_issue());
        CHECK(e.issue().entry == "test.json");
        CHECK(e.issue().severity == kError);
        return e.issue().code;
    }
    return "";
}

std::string nested(int depth, char open, char close) {
    return std::string(static_cast<std::size_t>(depth), open) + std::string(static_cast<std::size_t>(depth), close);
}

std::string write_double(double d) { return format_double(d); }

std::uint64_t bits_of(double d) {
    std::uint64_t b;
    std::memcpy(&b, &d, sizeof b);
    return b;
}

// A ByteReader that gives at most `step` bytes per call, to cross every chunk boundary.
class TrickleReader : public ByteReader {
  public:
    TrickleReader(const std::string &s, std::size_t step) : s_(s), pos_(0), step_(step) {}
    std::size_t read(char *buf, std::size_t cap) override {
        std::size_t n = s_.size() - pos_;
        if (n > step_)
            n = step_;
        if (n > cap)
            n = cap;
        std::memcpy(buf, s_.data() + pos_, n);
        pos_ += n;
        return n;
    }

  private:
    const std::string &s_;
    std::size_t pos_;
    std::size_t step_;
};

// A SAX handler that records a trace of the events.
struct TraceHandler {
    std::string trace;
    std::size_t stop_after_objects;
    std::size_t objects;
    TraceHandler() : stop_after_objects(0), objects(0) {}
    bool Null() { trace += "n;"; return true; }
    bool Bool(bool b) { trace += b ? "T;" : "F;"; return true; }
    bool Int(int i) { trace += "i" + std::string(i < 0 ? "-" : "") + dec(static_cast<std::uint64_t>(i < 0 ? -i : i)) + ";"; return true; }
    bool Uint(unsigned u) { trace += "u" + dec(u) + ";"; return true; }
    bool Int64(std::int64_t) { trace += "I;"; return true; }
    bool Uint64(std::uint64_t u) { trace += "U" + dec(u) + ";"; return true; }
    bool Double(double) { trace += "d;"; return true; }
    bool RawNumber(const char *, rapidjson::SizeType, bool) { return true; }
    bool String(const char *s, rapidjson::SizeType n, bool) { trace += "s" + std::string(s, n) + ";"; return true; }
    bool Key(const char *s, rapidjson::SizeType n, bool) { trace += "k" + std::string(s, n) + ";"; return true; }
    bool StartObject() { trace += "{;"; return true; }
    bool EndObject(rapidjson::SizeType) {
        trace += "};";
        ++objects;
        return stop_after_objects == 0 || objects < stop_after_objects;
    }
    bool StartArray() { trace += "[;"; return true; }
    bool EndArray(rapidjson::SizeType) { trace += "];"; return true; }
};

} // namespace

TEST_CASE("json: a document parses whatever the whitespace and key order", "[json]") {
    const std::string text = " {\n \"b\" : [1, 2.5, \"x\", null, true],\t\"a\": {\"k\": -3} } ";
    Document doc;
    parse_document(text.data(), text.size(), doc, "t.json");
    Fields f(doc, "t.json", "");
    CHECK(f.array("b").Size() == 5);
    CHECK(f.object("a").number("k") == -3.0);
    CHECK(f.has("a"));
    CHECK_FALSE(f.has("zz"));
}

TEST_CASE("json: depth 64 is accepted and depth 65 is rejected", "[json]") {
    CHECK(parse_code(nested(64, '[', ']')).empty());
    CHECK(parse_code(nested(65, '[', ']')) == "schema.depth");
    std::string obj;
    for (int i = 0; i < 65; ++i)
        obj += "{\"a\":";
    obj += "1" + std::string(65, '}');
    CHECK(parse_code(obj) == "schema.depth");
    // Brackets inside strings do not count.
    CHECK(parse_code("[\"" + std::string(100, '[') + "\"]").empty());
    // A very deep document is refused without exhausting the stack.
    CHECK(parse_code(nested(200000, '[', ']')) == "schema.depth");
}

TEST_CASE("json: a duplicate key is rejected", "[json]") {
    CHECK(parse_code("{\"a\":1,\"a\":2}") == "schema.duplicate_key");
    CHECK(parse_code("{\"x\":{\"a\":1,\"b\":2,\"a\":3}}") == "schema.duplicate_key");
    CHECK(parse_code("[{\"a\":1},{\"a\":1,\"a\":1}]") == "schema.duplicate_key");
    // The same key at different levels or in different objects is fine.
    CHECK(parse_code("{\"a\":{\"a\":1},\"b\":[{\"a\":1},{\"a\":2}]}").empty());
    // A large object (sorted comparison).
    std::string big = "{";
    for (int i = 0; i < 100; ++i)
        big += "\"k" + dec(static_cast<std::uint64_t>(i)) + "\":0,";
    CHECK(parse_code(big + "\"k50\":1}") == "schema.duplicate_key");
    CHECK(parse_code(big + "\"z\":1}").empty());
}

TEST_CASE("json: NaN, Infinity and out-of-range numbers are rejected", "[json]") {
    CHECK(parse_code("[nan]") == "num.not_finite");
    CHECK(parse_code("[NaN]") == "num.not_finite");
    CHECK(parse_code("[Infinity]") == "num.not_finite");
    CHECK(parse_code("[-Infinity]") == "num.not_finite");
    CHECK(parse_code("[inf]") == "num.not_finite");
    CHECK(parse_code("[1e999]") == "num.not_finite");
    CHECK(parse_code("[-1e999]") == "num.not_finite");
    CHECK(parse_code("{\"x\":nan}") == "num.not_finite");
    CHECK(parse_code("[1e-999]").empty()); // underflows to 0
}

TEST_CASE("json: malformed text is rejected with schema.syntax", "[json]") {
    CHECK(parse_code("") == "schema.syntax");
    CHECK(parse_code("   ") == "schema.syntax");
    CHECK(parse_code("{") == "schema.syntax");
    CHECK(parse_code("[1,]") == "schema.syntax");
    CHECK(parse_code("{\"a\":1,}") == "schema.syntax");
    CHECK(parse_code("{} {}") == "schema.syntax");
    CHECK(parse_code("[01]") == "schema.syntax");
    CHECK(parse_code("[1.]") == "schema.syntax");
    CHECK(parse_code("{'a':1}") == "schema.syntax");
    CHECK(parse_code("[\"a\nb\"]") == "schema.syntax");                   // raw control character
    CHECK(parse_code("\xEF\xBB\xBF{}") == "schema.syntax");               // BOM
    CHECK(parse_code(std::string("{}\0{}", 5)) == "schema.syntax");       // NUL
    CHECK(parse_code(std::string("{}\0", 3)) == "schema.syntax");
    CHECK(parse_code("[\"\xC3\x28\"]") == "schema.syntax");               // invalid UTF-8
    CHECK(parse_code("[\"\xED\xA0\x80\"]") == "schema.syntax");           // lone surrogate in UTF-8
    CHECK(parse_code("[\"\\ud800\"]") == "schema.syntax");                // lone surrogate escape
    CHECK(parse_code("[\"caf\xC3\xA9 \xF0\x9F\x98\x80\"]").empty());
}

TEST_CASE("json: numbers parse to the exact nearest double", "[json]") {
    Document doc;
    const std::string text =
        "[0.1, 1e22, 1e-7, 5e-324, 2.2250738585072014e-308, 1.7976931348623157e308, 123456789012345678, "
        "0.30000000000000004, 4503599627370497.5, -0]";
    parse_document(text.data(), text.size(), doc, "t.json");
    CHECK(doc[0].GetDouble() == 0.1);
    CHECK(doc[1].GetDouble() == 1e22);
    CHECK(doc[2].GetDouble() == 1e-7);
    CHECK(doc[3].GetDouble() == 5e-324);
    CHECK(doc[4].GetDouble() == 2.2250738585072014e-308);
    CHECK(doc[5].GetDouble() == 1.7976931348623157e308);
    CHECK(doc[6].GetUint64() == 123456789012345678ull);
    CHECK(doc[7].GetDouble() == 0.30000000000000004);
    CHECK(doc[8].GetDouble() == 4503599627370497.5);
}

TEST_CASE("json: identifiers are u32 and integers above 2^53 are rejected", "[json]") {
    Document doc;
    const std::string text =
        "{\"ok\":4294967295,\"zero\":0,\"big\":4294967296,\"neg\":-1,\"frac\":1.5,\"whole\":3.0,\"p53\":9007199254740993,"
        "\"huge\":18446744073709551615,\"exp\":1e3,\"s\":\"7\",\"max53\":9007199254740992}";
    parse_document(text.data(), text.size(), doc, "t.json");
    Fields f(doc, "t.json", "");
    CHECK(f.u32("ok") == 4294967295u);
    CHECK(f.u32("zero") == 0u);
    CHECK(f.u32("whole") == 3u);
    CHECK(f.u32("exp") == 1000u);
    CHECK(f.u64("max53") == 9007199254740992ull);
    const char *bad[] = {"big", "neg", "frac", "p53", "huge", "s"};
    for (std::size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        INFO(bad[i]);
        CHECK_THROWS_AS(f.u32(bad[i]), FormatError);
    }
    CHECK_THROWS_AS(f.u64("p53"), FormatError);
    CHECK_THROWS_AS(f.u64("huge"), FormatError);
    CHECK_THROWS_AS(f.u64("neg"), FormatError);
    CHECK_THROWS_AS(f.u64("frac"), FormatError);
    try {
        f.u32("p53");
    } catch (const FormatError &e) {
        CHECK(e.issue().code == "schema.value");
        CHECK(e.issue().entry == "t.json");
    }
    try {
        f.u32("s");
    } catch (const FormatError &e) {
        CHECK(e.issue().code == "schema.type");
    }
    try {
        f.u32("absent");
    } catch (const FormatError &e) {
        CHECK(e.issue().code == "schema.missing");
    }
}

TEST_CASE("json: typed access reports the entry and the path", "[json]") {
    Document doc;
    const std::string text = "{\"o\":{\"name\":\"abcd\",\"kind\":\"b\",\"flag\":true,\"list\":[1,2]},\"n\":5}";
    parse_document(text.data(), text.size(), doc, "frames/1/frame.json");
    Fields f(doc, "frames/1/frame.json", "");
    Fields o = f.object("o");
    CHECK(o.string("name", 4) == "abcd");
    CHECK(o.boolean("flag"));
    CHECK(o.array("list").Size() == 2);
    static const char *const kinds[] = {"a", "b", "c"};
    CHECK(o.one_of("kind", kinds, 3) == 1);
    try {
        o.string("name", 3);
        FAIL("too long");
    } catch (const FormatError &e) {
        CHECK(e.issue().code == "schema.value");
        CHECK(e.issue().entry == "frames/1/frame.json");
        CHECK(e.issue().message.find("o.name") != std::string::npos);
    }
    CHECK_THROWS_AS(f.object("n"), FormatError);
    CHECK_THROWS_AS(o.boolean("name"), FormatError);
    CHECK_THROWS_AS(Fields(doc["n"], "x", "n"), FormatError);
    static const char *const other[] = {"x", "y"};
    CHECK_THROWS_AS(o.one_of("kind", other, 2), FormatError);
}

// ---- writer ----------------------------------------------------------------------------------

TEST_CASE("json writer: doubles use the shortest text, whole values have no .0", "[json]") {
    CHECK(write_double(500.0) == "500");
    CHECK(write_double(0.3) == "0.3");
    CHECK(write_double(1.0) == "1");
    CHECK(write_double(-250.0) == "-250");
    CHECK(write_double(0.001) == "0.001");
    CHECK(write_double(1e-7) == "1e-7");
    CHECK(write_double(1e22) == "1e22");
    CHECK(write_double(1.5e22) == "1.5e22");
    CHECK(write_double(123456.789) == "123456.789");
    CHECK(write_double(0.1 + 0.2) == "0.30000000000000004");
    CHECK(write_double(5e-324) == "5e-324");
    CHECK(write_double(1.7976931348623157e308) == "1.7976931348623157e308");
}

TEST_CASE("json writer: negative zero is written as 0", "[json]") {
    CHECK(write_double(-0.0) == "0");
    CHECK(write_double(0.0) == "0");
    std::string out;
    JsonWriter w(out);
    w.begin_array();
    w.number(-0.0);
    w.end_array();
    CHECK(out == "[0]");
}

TEST_CASE("json writer: NaN and Infinity are refused", "[json]") {
    const double zero = 0.0;
    CHECK_THROWS_AS(write_double(zero / zero), FormatError);
    CHECK_THROWS_AS(write_double(1.0 / zero), FormatError);
    CHECK_THROWS_AS(write_double(-1.0 / zero), FormatError);
    std::string out;
    JsonWriter w(out);
    try {
        w.number(1.0 / zero);
        FAIL("must throw");
    } catch (const FormatError &e) {
        CHECK(e.code() == LOOMORA_ERR_INVALID_ITEM);
        CHECK(e.issue().code == "num.not_finite");
    }
}

TEST_CASE("json writer: compact output with keys in the caller's order", "[json]") {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field_uint("z", 7);
    w.field_string("a", "x\"y\n\x01 \xC3\xA9");
    w.key("list");
    w.begin_array();
    w.uint(1);
    w.integer(-2);
    w.number(2.5);
    w.boolean(false);
    w.null_value();
    w.begin_object();
    w.end_object();
    w.end_array();
    w.field_raw("params", "{\"r\":5}");
    w.field_bool("ok", true);
    w.end_object();
    CHECK(out == "{\"z\":7,\"a\":\"x\\\"y\\n\\u0001 \xC3\xA9\",\"list\":[1,-2,2.5,false,null,{}],\"params\":{\"r\":5},\"ok\":true}");
}

TEST_CASE("json writer: an invalid UTF-8 string is refused", "[json]") {
    std::string out;
    JsonWriter w(out);
    w.begin_array();
    CHECK_THROWS_AS(w.string(std::string("\xC3\x28")), FormatError);
}

TEST_CASE("json writer: a parsed value is written back compact", "[json]") {
    const std::string text = " { \"b\" : [ 1 , 2.50 , \"s\" , null ] , \"a\" : { \"x\" : 1e2 , \"neg\":-0 } } ";
    Document doc;
    parse_document(text.data(), text.size(), doc, "t.json");
    std::string out;
    JsonWriter w(out);
    w.value(doc);
    CHECK(out == "{\"b\":[1,2.5,\"s\",null],\"a\":{\"x\":100,\"neg\":0}}");
}

TEST_CASE("json: 100,000 random doubles round trip bit for bit", "[json]") {
    std::mt19937_64 rng(20261117);
    std::vector<double> values;
    values.reserve(100000);
    // Specials first: denormals, the extremes, the named cases of the task.
    const double specials[] = {1e-7, 1e22, 5e-324, 2.2250738585072014e-308, 2.225073858507201e-308, 1.7976931348623157e308,
                               0.1, 0.3, 1.0 / 3.0, 9007199254740993.0, 1e21, 1e23, 123456789.123456789, 4.35, 0.0};
    for (std::size_t i = 0; i < sizeof specials / sizeof specials[0]; ++i) {
        values.push_back(specials[i]);
        values.push_back(-specials[i]);
    }
    while (values.size() < 100000) {
        std::uint64_t b = rng();
        const unsigned kind = static_cast<unsigned>(rng() % 4);
        if (kind == 0)
            b &= 0x800FFFFFFFFFFFFFull; // denormal or zero (exponent bits cleared)
        else if (kind == 1)
            b = (b & 0x800FFFFFFFFFFFFFull) | (static_cast<std::uint64_t>(1000 + rng() % 46) << 52); // small magnitudes
        double d;
        std::memcpy(&d, &b, sizeof d);
        if (!(d - d == 0.0))
            continue; // NaN or Infinity
        values.push_back(d);
    }
    std::size_t checked = 0;
    for (std::size_t start = 0; start < values.size(); start += 1000) {
        std::string out;
        JsonWriter w(out);
        w.begin_array();
        for (std::size_t i = start; i < start + 1000; ++i)
            w.number(values[i]);
        w.end_array();
        Document doc;
        parse_document(out.data(), out.size(), doc, "t.json");
        REQUIRE(doc.Size() == 1000);
        for (rapidjson::SizeType i = 0; i < 1000; ++i) {
            const double want = values[start + i];
            const double got = as_number(doc[i], "t.json", "x");
            const bool same = (want == 0.0) ? (got == 0.0) : bits_of(got) == bits_of(want);
            if (!same)
                FAIL("round trip differs for " << write_double(want));
            ++checked;
        }
    }
    CHECK(checked == 100000);
}

// ---- SAX -------------------------------------------------------------------------------------

TEST_CASE("json sax: events are the same for any chunking", "[json]") {
    const std::string text = "{\"a\":[1,-2,3.5,true,null,\"\xC3\xA9\\n\"],\"b\":{\"c\":18446744073709551615}}";
    std::string expected;
    {
        MemoryByteReader in(text.data(), text.size());
        TraceHandler h;
        CHECK(parse_sax(in, h, "t.json"));
        expected = h.trace;
        CHECK(expected == "{;ka;[;u1;i-2;d;T;n;s\xC3\xA9\n;];kb;{;kc;U18446744073709551615;};};");
    }
    const std::size_t steps[] = {1, 2, 3, 7, 64};
    for (std::size_t i = 0; i < sizeof steps / sizeof steps[0]; ++i) {
        TrickleReader in(text, steps[i]);
        TraceHandler h;
        CHECK(parse_sax(in, h, "t.json"));
        CHECK(h.trace == expected);
    }
}

TEST_CASE("json sax: depth, NUL, syntax and number errors", "[json]") {
    TraceHandler h;
    std::string deep = nested(65, '[', ']');
    TrickleReader in1(deep, 5);
    try {
        parse_sax(in1, h, "n.json");
        FAIL("depth");
    } catch (const FormatError &e) {
        CHECK(e.issue().code == "schema.depth");
        CHECK(e.issue().entry == "n.json");
    }
    std::string ok = nested(64, '[', ']');
    TrickleReader in2(ok, 5);
    TraceHandler h2;
    CHECK(parse_sax(in2, h2, "n.json"));

    const char *texts[] = {"{\"a\":}", "[1,2", "[1e999]"};
    const char *codes[] = {"schema.syntax", "schema.syntax", "num.not_finite"};
    for (int i = 0; i < 3; ++i) {
        std::string t = texts[i];
        TrickleReader in(t, 2);
        TraceHandler hh;
        try {
            parse_sax(in, hh, "n.json");
            FAIL("must throw");
        } catch (const FormatError &e) {
            CHECK(e.issue().code == codes[i]);
        }
    }
    std::string nul("{}\0x", 4);
    TrickleReader in3(nul, 3);
    TraceHandler h3;
    try {
        parse_sax(in3, h3, "n.json");
        FAIL("nul");
    } catch (const FormatError &e) {
        CHECK(e.issue().code == "schema.syntax");
    }
}

TEST_CASE("json sax: a handler can end the parse early", "[json]") {
    const std::string text = "[{\"a\":1},{\"a\":2},{\"a\":3},{\"a\":4}]";
    MemoryByteReader in(text.data(), text.size());
    TraceHandler h;
    h.stop_after_objects = 2;
    CHECK_FALSE(parse_sax(in, h, "t.json"));
    CHECK(h.objects == 2);
}
