// The nails.json codec: SAX parse into compact arrays with a range-read index, writer in the key
// order of the schema, nail_count check, 500,000 nails.
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "format/codec/nails_codec.hpp"
#include "format/json/json_in.hpp"
#include "loomora.h"

using namespace loomora::format;
using namespace loomora::format::codec;
using namespace loomora::format::model;

namespace {

class TrickleReader : public json::ByteReader {
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

NailTable parse(const std::string &text, bool is_3d, std::uint64_t expected = kAnyNailCount) {
    json::MemoryByteReader in(text.data(), text.size());
    NailTable t(is_3d);
    read_nails(in, is_3d, t, "frames/1/layouts/1/nails.json", expected);
    return t;
}

// The issue code of a failed parse, "" when it parses.
std::string parse_code(const std::string &text, bool is_3d, std::uint64_t expected = kAnyNailCount) {
    try {
        parse(text, is_3d, expected);
    } catch (const FormatError &e) {
        CHECK(e.code() == LOOMORA_ERR_CORRUPT);
        REQUIRE(e.has_issue());
        CHECK(e.issue().entry == "frames/1/layouts/1/nails.json");
        return e.issue().code;
    }
    return "";
}

std::string written(const NailTable &t) {
    std::string s;
    write_nails(t, s);
    return s;
}

Nail nail2d(std::uint32_t id, double x, double y) {
    Nail n;
    n.id = id;
    n.x = x;
    n.y = y;
    n.diameter = 2;
    n.kind = kNailBoundary;
    return n;
}

Nail nail3d(std::uint32_t id) {
    Nail n = nail2d(id, 1.5, -2.25);
    n.z = 3;
    n.axis[0] = 0;
    n.axis[1] = 0.6;
    n.axis[2] = 0.8;
    n.length = 4.5;
    n.side = kSideInterior;
    n.triangle = 17;
    n.u = 0.25;
    n.v = 0.5;
    n.kind = kNailInterior;
    return n;
}

bool same_nail(const Nail &a, const Nail &b, bool is_3d) {
    if (a.id != b.id || a.x != b.x || a.y != b.y || a.diameter != b.diameter || a.kind != b.kind ||
        a.has_label != b.has_label || a.label != b.label)
        return false;
    if (!is_3d)
        return true;
    return a.z == b.z && a.axis[0] == b.axis[0] && a.axis[1] == b.axis[1] && a.axis[2] == b.axis[2] &&
           a.length == b.length && a.side == b.side && a.triangle == b.triangle && a.u == b.u && a.v == b.v;
}

bool same_table(const NailTable &a, const NailTable &b) {
    if (a.size() != b.size() || a.is_3d() != b.is_3d())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same_nail(a.get(i), b.get(i), a.is_3d()))
            return false;
    return true;
}

NailTable make_2d(std::size_t count, std::uint32_t seed) {
    std::mt19937 rng(seed);
    NailTable t(false);
    t.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Nail n = nail2d(static_cast<std::uint32_t>(i * 2 + 1), static_cast<double>(rng() % 100000) / 100.0 - 500.0,
                        static_cast<double>(rng() % 100000) / 7.0);
        n.diameter = 1.0 + static_cast<double>(rng() % 50) / 10.0;
        n.kind = static_cast<NailKind>(rng() % 3);
        if (rng() % 1000 == 0) {
            n.has_label = true;
            n.label = "L" + std::to_string(i);
        }
        t.add(n);
    }
    return t;
}

} // namespace

TEST_CASE("nails codec: 2D nails are written in the schema key order", "[nails]") {
    NailTable t(false);
    t.add(nail2d(0, 250, 0));
    Nail n = nail2d(1, 0.5, -3.75);
    n.has_label = true;
    n.label = "A\"1";
    n.kind = kNailInterior;
    n.diameter = 2.5;
    t.add(n);
    CHECK(written(t) ==
          "{\"nails\":[{\"id\":0,\"x\":250,\"y\":0,\"diameter\":2,\"kind\":\"boundary\"},"
          "{\"id\":1,\"x\":0.5,\"y\":-3.75,\"diameter\":2.5,\"kind\":\"interior\",\"label\":\"A\\\"1\"}]}");
    CHECK(written(NailTable(false)) == "{\"nails\":[]}");
}

TEST_CASE("nails codec: 3D nails are written in the schema key order", "[nails]") {
    NailTable t(true);
    t.add(nail3d(9));
    Nail n = nail3d(10);
    n.has_label = true;
    n.label = "Z";
    n.side = kSideExterior;
    t.add(n);
    CHECK(written(t) ==
          "{\"nails\":[{\"id\":9,\"x\":1.5,\"y\":-2.25,\"z\":3,\"diameter\":2,\"kind\":\"interior\",\"axis\":[0,0.6,0.8],"
          "\"length\":4.5,\"side\":\"interior\",\"face\":{\"triangle\":17,\"u\":0.25,\"v\":0.5}},"
          "{\"id\":10,\"x\":1.5,\"y\":-2.25,\"z\":3,\"diameter\":2,\"kind\":\"interior\",\"label\":\"Z\",\"axis\":[0,0.6,0.8],"
          "\"length\":4.5,\"side\":\"exterior\",\"face\":{\"triangle\":17,\"u\":0.25,\"v\":0.5}}]}");
}

TEST_CASE("nails codec: write then read gives the same nails", "[nails]") {
    NailTable t2 = make_2d(2500, 1);
    const std::string text = written(t2);
    NailTable back = parse(text, false, 2500);
    CHECK(same_table(t2, back));
    CHECK(back.label_count() == t2.label_count());
    CHECK(written(back) == text);

    NailTable t3(true);
    for (std::uint32_t i = 0; i < 1500; ++i) {
        Nail n = nail3d(i * 3);
        n.x = i * 0.1;
        n.u = 0.001 * i / 1500;
        if (i % 100 == 0) {
            n.has_label = true;
            n.label = "n" + std::to_string(i);
        }
        t3.add(n);
    }
    const std::string text3 = written(t3);
    NailTable back3 = parse(text3, true, 1500);
    CHECK(same_table(t3, back3));
    CHECK(written(back3) == text3);
}

TEST_CASE("nails codec: any key order, whitespace and unknown keys are accepted", "[nails]") {
    const std::string text =
        " { \"extra\": {\"a\":[1,2,{\"b\":null}]}, \"nails\" : [ \n"
        "  { \"kind\":\"interior\", \"future\":[[1],[2]], \"diameter\": 2.5e0, \"y\":-1, \"label\":\"q\", \"x\":1e1, \"id\":7 },\n"
        "  { \"id\":8,\"x\":0,\"y\":0,\"diameter\":1,\"kind\":\"boundary\",\"z\":5,\"axis\":[1,0,0] } ],\n"
        "  \"after\": true } ";
    NailTable t = parse(text, false, 2);
    REQUIRE(t.size() == 2);
    Nail a = t.get(0);
    CHECK(a.id == 7);
    CHECK(a.x == 10.0);
    CHECK(a.y == -1.0);
    CHECK(a.diameter == 2.5);
    CHECK(a.kind == kNailInterior);
    CHECK(a.has_label);
    CHECK(a.label == "q");
    CHECK(t.get(1).id == 8);
    CHECK_FALSE(t.get(1).has_label);
    CHECK(parse("{\"nails\":[]}", false, 0).size() == 0);
}

TEST_CASE("nails codec: a 2D table ignores 3D fields and a 3D table requires them", "[nails]") {
    const std::string one3d =
        "{\"nails\":[{\"id\":1,\"x\":1,\"y\":2,\"z\":3,\"diameter\":2,\"kind\":\"boundary\",\"axis\":[0,0,1],"
        "\"length\":2,\"side\":\"exterior\",\"face\":{\"triangle\":4,\"u\":0.1,\"v\":0.2}}]}";
    CHECK(parse_code(one3d, false).empty());
    CHECK(parse_code(one3d, true).empty());
    NailTable t = parse(one3d, true);
    CHECK(t.get(0).z == 3);
    CHECK(t.get(0).triangle == 4);
    CHECK(t.get(0).v == 0.2);
    const std::string only2d = "{\"nails\":[{\"id\":1,\"x\":1,\"y\":2,\"diameter\":2,\"kind\":\"boundary\"}]}";
    CHECK(parse_code(only2d, true) == "schema.missing");
    // Each required 3D member
    const char *without[] = {"\"z\":3,", "\"axis\":[0,0,1],", "\"length\":2,", "\"side\":\"exterior\",",
                             ",\"face\":{\"triangle\":4,\"u\":0.1,\"v\":0.2}"};
    for (std::size_t i = 0; i < sizeof without / sizeof without[0]; ++i) {
        std::string s = one3d;
        s.erase(s.find(without[i]), std::strlen(without[i]));
        INFO(s);
        CHECK(parse_code(s, true) == "schema.missing");
    }
    std::string noface_member = one3d;
    noface_member.erase(noface_member.find("\"u\":0.1,"), 8);
    CHECK(parse_code(noface_member, true) == "schema.missing");
}

TEST_CASE("nails codec: malformed nails are rejected with a code", "[nails]") {
    const std::string head = "{\"nails\":[{\"id\":1,\"x\":1,\"y\":2,\"diameter\":2,\"kind\":\"boundary\"";
    CHECK(parse_code(head + "}]}", false).empty());
    CHECK(parse_code("{\"nails\":[{\"id\":1,\"x\":1,\"y\":2,\"kind\":\"boundary\"}]}", false) == "schema.missing"); // diameter
    CHECK(parse_code("{\"nails\":[{\"x\":1,\"y\":2,\"diameter\":2,\"kind\":\"boundary\"}]}", false) == "schema.missing");
    CHECK(parse_code(head + ",\"x\":3}]}", false) == "schema.duplicate_key");
    CHECK(parse_code("{\"nails\":[],\"nails\":[]}", false) == "schema.duplicate_key");
    CHECK(parse_code("{\"nails\":[{\"id\":4294967296,\"x\":1,\"y\":2,\"diameter\":2,\"kind\":\"boundary\"}]}", false) == "schema.value");
    CHECK(parse_code("{\"nails\":[{\"id\":-1,\"x\":1,\"y\":2,\"diameter\":2,\"kind\":\"boundary\"}]}", false) == "schema.value");
    CHECK(parse_code("{\"nails\":[{\"id\":1.5,\"x\":1,\"y\":2,\"diameter\":2,\"kind\":\"boundary\"}]}", false) == "schema.value");
    CHECK(parse_code("{\"nails\":[{\"id\":9007199254740993,\"x\":1,\"y\":2,\"diameter\":2,\"kind\":\"boundary\"}]}", false) == "schema.value");
    CHECK(parse_code("{\"nails\":[{\"id\":\"1\",\"x\":1,\"y\":2,\"diameter\":2,\"kind\":\"boundary\"}]}", false) == "schema.type");
    CHECK(parse_code("{\"nails\":[{\"id\":1,\"x\":\"1\",\"y\":2,\"diameter\":2,\"kind\":\"boundary\"}]}", false) == "schema.type");
    CHECK(parse_code("{\"nails\":[{\"id\":1,\"x\":1,\"y\":2,\"diameter\":2,\"kind\":5}]}", false) == "schema.type");
    CHECK(parse_code("{\"nails\":[{\"id\":1,\"x\":1,\"y\":2,\"diameter\":2,\"kind\":\"round\"}]}", false) == "schema.value");
    CHECK(parse_code(head + ",\"label\":5}]}", false) == "schema.type");
    CHECK(parse_code(head + ",\"x\":null}]}", false) == "schema.duplicate_key");
    CHECK(parse_code("{\"nails\":[{\"id\":1,\"x\":1,\"y\":2,\"diameter\":null,\"kind\":\"boundary\"}]}", false) == "schema.type");
    CHECK(parse_code("{\"nails\":[1]}", false) == "schema.type");
    CHECK(parse_code("{\"nails\":{}}", false) == "schema.type");
    CHECK(parse_code("{\"nails\":5}", false) == "schema.type");
    CHECK(parse_code("{}", false) == "schema.missing");
    CHECK(parse_code("[]", false) == "schema.type");
    CHECK(parse_code("7", false) == "schema.type");
    CHECK(parse_code("", false) == "schema.syntax");
    CHECK(parse_code("{\"nails\":[{\"id\":1,\"x\":nan,\"y\":2,\"diameter\":2,\"kind\":\"boundary\"}]}", false) == "schema.syntax");
    CHECK(parse_code("{\"nails\":[{\"id\":1,\"x\":1e999,\"y\":2,\"diameter\":2,\"kind\":\"boundary\"}]}", false) == "num.not_finite");
    CHECK(parse_code(head + "}]} x", false) == "schema.syntax");
    CHECK(parse_code(head + "},]}", false) == "schema.syntax");
    // axis and face shape
    const std::string h3 = "{\"nails\":[{\"id\":1,\"x\":1,\"y\":2,\"z\":0,\"diameter\":2,\"kind\":\"boundary\",\"length\":1,\"side\":\"exterior\",";
    CHECK(parse_code(h3 + "\"axis\":[0,0],\"face\":{\"triangle\":0,\"u\":0,\"v\":0}}]}", true) == "schema.value");
    CHECK(parse_code(h3 + "\"axis\":[0,0,1,1],\"face\":{\"triangle\":0,\"u\":0,\"v\":0}}]}", true) == "schema.value");
    CHECK(parse_code(h3 + "\"axis\":[0,\"a\",1],\"face\":{\"triangle\":0,\"u\":0,\"v\":0}}]}", true) == "schema.type");
    CHECK(parse_code(h3 + "\"axis\":5,\"face\":{\"triangle\":0,\"u\":0,\"v\":0}}]}", true) == "schema.type");
    CHECK(parse_code(h3 + "\"axis\":[0,0,1],\"face\":[1]}]}", true) == "schema.type");
    CHECK(parse_code(h3 + "\"axis\":[0,0,1],\"face\":{\"triangle\":0,\"u\":0,\"u\":1,\"v\":0}}]}", true) == "schema.duplicate_key");
    CHECK(parse_code(h3 + "\"axis\":[0,0,1],\"face\":{\"triangle\":-1,\"u\":0,\"v\":0}}]}", true) == "schema.value");
    CHECK(parse_code(h3 + "\"axis\":[0,0,1],\"face\":{\"triangle\":0,\"u\":0,\"v\":0}}]}", true).empty());
    CHECK(parse_code(h3 + "\"axis\":[0,0,1],\"face\":{\"triangle\":0,\"u\":0,\"v\":0}}]}", true, 2) == "schema.nail_count");
    CHECK(parse_code(h3 + "\"side\":\"top\"}]}", true) == "schema.duplicate_key");
}

TEST_CASE("nails codec: nail_count is checked", "[nails]") {
    const std::string two =
        "{\"nails\":[{\"id\":1,\"x\":1,\"y\":2,\"diameter\":2,\"kind\":\"boundary\"},"
        "{\"id\":2,\"x\":1,\"y\":2,\"diameter\":2,\"kind\":\"boundary\"}]}";
    CHECK(parse_code(two, false, 2).empty());
    CHECK(parse_code(two, false, 3) == "schema.nail_count");
    CHECK(parse_code(two, false, 1) == "schema.nail_count");
    CHECK(parse_code(two, false, 0) == "schema.nail_count");
    CHECK(parse_code(two, false).empty());
    try {
        parse(two, false, 3);
    } catch (const FormatError &e) {
        CHECK(e.issue().message.find("3") != std::string::npos);
        CHECK(e.issue().message.find("2") != std::string::npos);
    }
}

TEST_CASE("nails codec: depth over 64 is rejected", "[nails]") {
    std::string deep = "{\"nails\":[{\"id\":1,\"x\":1,\"y\":2,\"diameter\":2,\"kind\":\"boundary\",\"future\":";
    deep += std::string(70, '[') + std::string(70, ']') + "}]}";
    CHECK(parse_code(deep, false) == "schema.depth");
}

TEST_CASE("nails codec: the parse is the same for any chunking", "[nails]") {
    NailTable t = make_2d(300, 3);
    const std::string text = written(t);
    const std::size_t steps[] = {1, 2, 5, 13, 1000};
    for (std::size_t i = 0; i < sizeof steps / sizeof steps[0]; ++i) {
        TrickleReader in(text, steps[i]);
        NailTable back(false);
        read_nails(in, false, back, "n.json", 300);
        CHECK(same_table(t, back));
    }
}

TEST_CASE("nails codec: the index has one offset per 1,024 nails", "[nails]") {
    NailTable t = make_2d(3000, 4);
    const std::string text = written(t);
    NailTable back = parse(text, false, 3000);
    REQUIRE(back.offsets.size() == 3);
    for (std::size_t k = 0; k < back.offsets.size(); ++k) {
        REQUIRE(back.offsets[k] < text.size());
        CHECK(text[static_cast<std::size_t>(back.offsets[k])] == '{');
        // The nail at that offset is nail k * 1024.
        json::MemoryByteReader in(text.data() + back.offsets[k], text.size() - static_cast<std::size_t>(back.offsets[k]));
        NailTable range(false);
        read_nail_range(in, false, 1, range, "n.json");
        REQUIRE(range.size() == 1);
        CHECK(same_nail(range.get(0), t.get(k * 1024), false));
    }
    CHECK(back.offsets[0] == std::strlen("{\"nails\":["));
}

TEST_CASE("nails codec: a range read returns the nails after an offset", "[nails]") {
    NailTable t = make_2d(3000, 5);
    const std::string text = written(t);
    NailTable full = parse(text, false);
    const std::size_t off = static_cast<std::size_t>(full.offsets[1]);

    json::MemoryByteReader in(text.data() + off, text.size() - off);
    NailTable range(false);
    read_nail_range(in, false, 10, range, "n.json");
    REQUIRE(range.size() == 10);
    for (std::size_t i = 0; i < 10; ++i)
        CHECK(same_nail(range.get(i), t.get(1024 + i), false));

    // Across more than one index block.
    json::MemoryByteReader in2(text.data() + off, text.size() - off);
    NailTable range2(false);
    read_nail_range(in2, false, 1500, range2, "n.json");
    REQUIRE(range2.size() == 1500);
    CHECK(same_nail(range2.get(1499), t.get(1024 + 1499), false));

    // Up to the end of the array: fewer nails than asked.
    const std::size_t off2 = static_cast<std::size_t>(full.offsets[2]);
    json::MemoryByteReader in3(text.data() + off2, text.size() - off2);
    NailTable range3(false);
    read_nail_range(in3, false, 5000, range3, "n.json");
    CHECK(range3.size() == 3000 - 2048);
    CHECK(same_nail(range3.get(range3.size() - 1), t.get(2999), false));

    // Nothing asked, nothing read; a malformed fragment is an error.
    json::MemoryByteReader in4(text.data() + off, text.size() - off);
    NailTable range4(false);
    read_nail_range(in4, false, 0, range4, "n.json");
    CHECK(range4.size() == 0);
    const std::string bad = "{\"id\":1,\"x\":1}";
    json::MemoryByteReader in5(bad.data(), bad.size());
    NailTable range5(false);
    CHECK_THROWS_AS(read_nail_range(in5, false, 1, range5, "n.json"), FormatError);
}

TEST_CASE("nails codec: 3D range read", "[nails]") {
    NailTable t(true);
    for (std::uint32_t i = 0; i < 2100; ++i) {
        Nail n = nail3d(i);
        n.triangle = i;
        t.add(n);
    }
    const std::string text = written(t);
    NailTable full = parse(text, true, 2100);
    REQUIRE(full.offsets.size() == 3);
    const std::size_t off = static_cast<std::size_t>(full.offsets[2]);
    json::MemoryByteReader in(text.data() + off, text.size() - off);
    NailTable range(true);
    read_nail_range(in, true, 100, range, "n.json");
    REQUIRE(range.size() == 52);
    CHECK(range.get(0).triangle == 2048);
    CHECK(range.get(51).id == 2099);
}

TEST_CASE("nails codec: coordinates round trip exactly", "[nails]") {
    std::mt19937_64 rng(77);
    NailTable t(false);
    for (std::uint32_t i = 0; i < 5000; ++i) {
        std::uint64_t bx = rng(), by = rng();
        bx = (bx & 0x800FFFFFFFFFFFFFull) | (static_cast<std::uint64_t>(1000 + rng() % 47) << 52);
        by = (by & 0x800FFFFFFFFFFFFFull) | (static_cast<std::uint64_t>(1015 + rng() % 16) << 52);
        Nail n = nail2d(i, 0, 0);
        std::memcpy(&n.x, &bx, 8);
        std::memcpy(&n.y, &by, 8);
        n.diameter = 5e-324 * static_cast<double>(1 + i);
        t.add(n);
    }
    NailTable back = parse(written(t), false, 5000);
    CHECK(same_table(t, back));
}

TEST_CASE("nails codec: a table with an invalid kind or a NaN is refused on write", "[nails]") {
    NailTable t(false);
    Nail n = nail2d(1, 0, 0);
    n.x = std::numeric_limits<double>::quiet_NaN();
    t.add(n);
    std::string out;
    CHECK_THROWS_AS(write_nails(t, out), FormatError);
    NailTable k(false);
    k.add(nail2d(1, 0, 0));
    k.kind[0] = 9;
    std::string out2;
    CHECK_THROWS_AS(write_nails(k, out2), FormatError);
}

TEST_CASE("nails codec: 500,000 nails", "[nails][large]") {
    const std::size_t count = 500000;
    NailTable t = make_2d(count, 99);
    std::string text;
    write_nails(t, text);
    CHECK(text.size() > count * 40);
    // Parse through a chunked reader as the entry reader will do.
    json::MemoryByteReader in(text.data(), text.size());
    NailTable back(false);
    read_nails(in, false, back, "nails.json", count);
    REQUIRE(back.size() == count);
    CHECK(back.offsets.size() == (count + kNailsPerIndexEntry - 1) / kNailsPerIndexEntry);
    CHECK(back.id == t.id);
    CHECK(back.x == t.x);
    CHECK(back.y == t.y);
    CHECK(back.diameter == t.diameter);
    CHECK(back.kind == t.kind);
    CHECK(back.label_index == t.label_index);
    CHECK(back.label_text == t.label_text);
    // Range read near the end through the index.
    const std::size_t last_block = back.offsets.size() - 1;
    const std::size_t off = static_cast<std::size_t>(back.offsets[last_block]);
    json::MemoryByteReader in2(text.data() + off, text.size() - off);
    NailTable range(false);
    read_nail_range(in2, false, kNailsPerIndexEntry, range, "nails.json");
    CHECK(range.size() == count - last_block * kNailsPerIndexEntry);
    CHECK(range.get(0).id == t.id[last_block * kNailsPerIndexEntry]);
    CHECK(written(back).size() == text.size());
}
