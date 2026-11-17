// sequence.bin (contracts/project-format.md 5.1): record layout, rejections, the lossless rebuild of
// the machine step list (requirements 8.3) and the winding convention of FR-015.
#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "zlib.h"
#include "format/codec/sequence_codec.hpp"
#include "format/endian.hpp"
#include "format/io/memory_io.hpp"
#include "format/report.hpp"

using namespace loomora::format;
typedef std::vector<std::uint8_t> Bytes;

namespace {

SequenceStep step(std::uint32_t nail, bool ccw, StepType type, std::uint32_t spool, std::uint8_t level = 0) {
    SequenceStep s;
    s.nail_id = nail;
    s.ccw = ccw;
    s.type = type;
    s.spool_index = spool;
    s.level = level;
    return s;
}

Bytes encode(const std::vector<SequenceStep> &steps, bool levels = false) {
    MemorySink sink;
    encode_sequence(sink, steps, levels);
    sink.commit();
    return sink.take();
}

// Appends the CRC-32 of every byte so far.
void seal(Bytes &b) {
    const std::uint32_t crc = static_cast<std::uint32_t>(crc32(0, b.empty() ? nullptr : &b[0],
                                                               static_cast<unsigned>(b.size())));
    unsigned char t[4];
    write_u32(t, crc);
    b.insert(b.end(), t, t + 4);
}

Bytes raw(const std::string &s) { return Bytes(s.begin(), s.end()); }

// Header "LMSQ", version 1, header flags, then `rest` (count varint and records), then the CRC.
Bytes file(std::uint8_t header_flags, const Bytes &rest) {
    Bytes b = raw("LMSQ");
    b.push_back(1);
    b.push_back(header_flags);
    b.insert(b.end(), rest.begin(), rest.end());
    seal(b);
    return b;
}

Bytes bytes(std::initializer_list<int> l) {
    Bytes b;
    for (int v : l)
        b.push_back(static_cast<std::uint8_t>(v));
    return b;
}

// The code of the issue carried by the FormatError thrown by `fn` ("" when none, "no throw" when
// nothing is thrown).
template <class F> std::string issue_code(F fn, int *err = nullptr) {
    try {
        fn();
    } catch (const FormatError &e) {
        if (err != nullptr)
            *err = e.code();
        return e.has_issue() ? e.issue().code : std::string();
    }
    return "no throw";
}

std::vector<SequenceStep> decode(const Bytes &b) {
    MemorySource src(b.empty() ? nullptr : &b[0], b.size());
    std::vector<SequenceStep> out;
    decode_sequence(src, &out);
    return out;
}

std::string decode_code(const Bytes &b) {
    return issue_code([&b] { decode(b); });
}

} // namespace

TEST_CASE("sequence.bin layout is byte exact", "[sequence]") {
    std::vector<SequenceStep> steps;
    steps.push_back(step(12, false, kStepStart, 0));
    steps.push_back(step(87, true, kStepSegment, 0));
    steps.push_back(step(87, false, kStepEnd, 0));
    const Bytes got = encode(steps);

    // "LMSQ" 1 0 count=3 | 0C 02 | 57 01 | 57 04 | CRC32
    Bytes want = raw("LMSQ");
    const Bytes tail = bytes({1, 0, 3, 0x0C, 0x02, 0x57, 0x01, 0x57, 0x04});
    want.insert(want.end(), tail.begin(), tail.end());
    seal(want);
    CHECK(got == want);
    CHECK(got.size() == 4 + 1 + 1 + 1 + 6 + 4);
}

TEST_CASE("flags: rotation bit 0, type bits 1-2, spool bits 3-7", "[sequence]") {
    std::vector<SequenceStep> steps;
    steps.push_back(step(0, false, kStepSegment, 0)); // flags 0x00
    steps.push_back(step(0, true, kStepSegment, 0));  // 0x01
    steps.push_back(step(0, false, kStepStart, 0));   // 0x02
    steps.push_back(step(0, false, kStepEnd, 0));     // 0x04
    steps.push_back(step(0, false, kStepSegment, 30)); // 30 << 3 = 0xF0
    steps.push_back(step(0, true, kStepEnd, 5));      // 0x28 | 0x04 | 0x01 = 0x2D
    const Bytes got = encode(steps);
    const std::size_t first = 4 + 1 + 1 + 1;
    const int expected[6] = {0x00, 0x01, 0x02, 0x04, 0xF0, 0x2D};
    for (std::size_t i = 0; i < 6; ++i)
        CHECK(got[first + 2 * i + 1] == expected[i]);
}

TEST_CASE("winding convention FR-015: rotation bit 0 is CW, 1 is CCW, nothing is mirrored", "[sequence]") {
    std::vector<SequenceStep> steps;
    steps.push_back(step(3, false, kStepStart, 0));
    steps.push_back(step(4, true, kStepSegment, 0));
    const Bytes got = encode(steps);
    CHECK((got[8] & 1) == 0); // record 1, flags: CW
    CHECK((got[10] & 1) == 1); // record 2, flags: CCW
    const std::vector<SequenceStep> back = decode(got);
    REQUIRE(back.size() == 2);
    CHECK(back[0].ccw == false);
    CHECK(back[1].ccw == true);
    // The machine list keeps the sides as they are: from_side is the rotation of the previous
    // record of the spool, to_side that of the segment's record.
    std::vector<std::uint32_t> ids(1, 7);
    std::vector<MachineStep> m;
    rebuild_machine_steps(back, ids, SegmentLength(), &m);
    REQUIRE(m.size() == 1);
    CHECK(m[0].from_ccw == false);
    CHECK(m[0].to_ccw == true);
}

TEST_CASE("encode and decode round trip, escape spool index and 5-byte nail ids", "[sequence]") {
    std::vector<SequenceStep> steps;
    steps.push_back(step(0, false, kStepStart, 0));
    steps.push_back(step(127, true, kStepSegment, 30));
    steps.push_back(step(128, false, kStepSegment, 31));       // first escaped index
    steps.push_back(step(16383, true, kStepSegment, 32));
    steps.push_back(step(0xFFFFFFFFu, false, kStepSegment, 1000000));
    steps.push_back(step(5, true, kStepEnd, 0xFFFFFFFFu));
    const Bytes b = encode(steps);
    const std::vector<SequenceStep> back = decode(b);
    REQUIRE(back.size() == steps.size());
    for (std::size_t i = 0; i < steps.size(); ++i) {
        CHECK(back[i].nail_id == steps[i].nail_id);
        CHECK(back[i].ccw == steps[i].ccw);
        CHECK(back[i].type == steps[i].type);
        CHECK(back[i].spool_index == steps[i].spool_index);
        CHECK(back[i].level == 0);
    }
}

TEST_CASE("level byte is present only with header bit 0", "[sequence]") {
    std::vector<SequenceStep> steps;
    steps.push_back(step(1, false, kStepStart, 0, 0));
    steps.push_back(step(2, true, kStepSegment, 0, 7));
    steps.push_back(step(3, false, kStepSegment, 0, 255));
    const Bytes with = encode(steps, true);
    CHECK(with[5] == 1); // header flags
    CHECK(with.size() == 4 + 1 + 1 + 1 + 3 * 3 + 4);
    const std::vector<SequenceStep> back = decode(with);
    REQUIRE(back.size() == 3);
    CHECK(back[1].level == 7);
    CHECK(back[2].level == 255);
    // A level without the header bit cannot be encoded silently.
    int err = 0;
    CHECK(issue_code([&steps] { encode(steps, false); }, &err) == "");
    CHECK(err == LOOMORA_ERR_INVALID_ITEM);
}

TEST_CASE("an empty sequence is valid", "[sequence]") {
    const Bytes b = encode(std::vector<SequenceStep>());
    CHECK(b.size() == 4 + 1 + 1 + 1 + 4);
    CHECK(decode(b).empty());
}

TEST_CASE("about 3 bytes per step, 5000 steps under 20 KB (SC-006)", "[sequence]") {
    std::vector<SequenceStep> steps;
    std::uint32_t seed = 12345;
    for (int i = 0; i < 5000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        steps.push_back(step((seed >> 8) % 300, ((seed >> 4) & 1) != 0,
                             i == 0 ? kStepStart : kStepSegment, (seed >> 20) % 4));
    }
    const Bytes b = encode(steps);
    CHECK(b.size() < 20 * 1024);
    CHECK(b.size() <= 5000u * 3u + 16u);
    CHECK(decode(b).size() == 5000);
}

TEST_CASE("a sequence larger than the read buffer decodes in order", "[sequence]") {
    std::vector<SequenceStep> steps;
    std::uint32_t seed = 99;
    for (int i = 0; i < 90000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        steps.push_back(step((seed >> 8) % 100000, (seed & 1) != 0, kStepSegment, (seed >> 24) % 40));
    }
    const Bytes b = encode(steps);
    REQUIRE(b.size() > 200000);
    const std::vector<SequenceStep> back = decode(b);
    REQUIRE(back.size() == steps.size());
    CHECK(back[0].nail_id == steps[0].nail_id);
    CHECK(back[44444].nail_id == steps[44444].nail_id);
    CHECK(back[44444].spool_index == steps[44444].spool_index);
    CHECK(back.back().nail_id == steps.back().nail_id);
}

TEST_CASE("decode_sequence returns a range and still verifies the CRC", "[sequence]") {
    std::vector<SequenceStep> steps;
    for (int i = 0; i < 100; ++i)
        steps.push_back(step(static_cast<std::uint32_t>(i), false, kStepSegment, 0));
    Bytes b = encode(steps);
    MemorySource src(&b[0], b.size());
    std::vector<SequenceStep> out;
    std::uint32_t total = 0;
    bool levels = true;
    decode_sequence(src, &out, 10, 5, &total, &levels);
    CHECK(total == 100);
    CHECK(levels == false);
    REQUIRE(out.size() == 5);
    CHECK(out[0].nail_id == 10);
    CHECK(out[4].nail_id == 14);
    b[20] ^= 0x40; // damage a record outside the range
    MemorySource bad(&b[0], b.size());
    CHECK(issue_code([&] { decode_sequence(bad, &out, 10, 5); }) != "no throw");
}

TEST_CASE("sequence rejections carry their issue codes", "[sequence]") {
    int err = 0;

    SECTION("magic, version, reserved header flags, too short") {
        Bytes b = encode(std::vector<SequenceStep>(1, step(1, false, kStepStart, 0)));
        Bytes m = b;
        m[0] = 'X';
        CHECK(decode_code(m) == "seq.header");
        Bytes v = b;
        v[4] = 2;
        CHECK(decode_code(v) == "seq.header");
        CHECK(decode_code(file(0x02, bytes({0}))) == "seq.header"); // reserved flag bit
        CHECK(decode_code(Bytes()) == "seq.header");
        CHECK(decode_code(raw("LMSQ")) == "seq.header");
        CHECK(issue_code([&m] { decode(m); }, &err) == "seq.header");
        CHECK(err == LOOMORA_ERR_CORRUPT);
    }

    SECTION("reserved record type 3") {
        // flags = type 3 << 1 = 0x06
        CHECK(decode_code(file(0, bytes({1, 0x05, 0x06}))) == "seq.header");
    }

    SECTION("non-minimal varint (count, nail id, escaped spool)") {
        CHECK(decode_code(file(0, bytes({0x81, 0x00, 0x05, 0x02}))) == "seq.header"); // count 1 as 81 00
        CHECK(decode_code(file(0, bytes({1, 0x85, 0x00, 0x02}))) == "seq.header");    // nail 5 as 85 00
        CHECK(decode_code(file(0, bytes({1, 0x05, 0xFA, 0x85, 0x00}))) == "seq.header"); // spool 5 as 85 00
        CHECK(decode_code(file(0, bytes({1, 0x05, 0xFA, 0x05}))) == "seq.header");     // escape below 31
    }

    SECTION("varint longer than 5 bytes or above 2^32 - 1") {
        CHECK(decode_code(file(0, bytes({1, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01, 0x02}))) == "seq.header");
        CHECK(decode_code(file(0, bytes({1, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x02}))) == "seq.header");
    }

    SECTION("count different from the number of records") {
        // count 2 with 1 record, count 1 with 2 records, both with a valid CRC.
        CHECK(decode_code(file(0, bytes({2, 0x05, 0x02}))) == "seq.count");
        CHECK(decode_code(file(0, bytes({1, 0x05, 0x02, 0x05, 0x02}))) == "seq.count");
        CHECK(issue_code([] { decode(file(0, bytes({2, 0x05, 0x02}))); }, &err) == "seq.count");
        CHECK(err == LOOMORA_ERR_CORRUPT);
        // a huge count does not allocate and ends as a count error
        CHECK(decode_code(file(0, bytes({0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x05, 0x02}))) == "seq.count");
        // a record cut in the middle of its varint
        CHECK(decode_code(file(0, bytes({1, 0x85}))) == "seq.count");
    }

    SECTION("CRC mismatch") {
        std::vector<SequenceStep> steps;
        for (int i = 0; i < 10; ++i)
            steps.push_back(step(static_cast<std::uint32_t>(i), false, kStepSegment, 0));
        Bytes b = encode(steps);
        Bytes flipped = b;
        flipped[10] ^= 0x01; // a rotation bit changes, still structurally valid
        CHECK(decode_code(flipped) == "seq.crc");
        Bytes crc = b;
        crc[crc.size() - 1] ^= 0x80;
        CHECK(decode_code(crc) == "seq.crc");
        CHECK(issue_code([&crc] { decode(crc); }, &err) == "seq.crc");
        CHECK(err == LOOMORA_ERR_CORRUPT);
    }
}

TEST_CASE("every truncation of a sequence is rejected", "[sequence]") {
    std::vector<SequenceStep> steps;
    for (int i = 0; i < 20; ++i)
        steps.push_back(step(static_cast<std::uint32_t>(i * 50), (i & 1) != 0, kStepSegment, i % 3));
    const Bytes b = encode(steps, false);
    for (std::size_t n = 0; n < b.size(); ++n) {
        Bytes cut(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n));
        const std::string code = decode_code(cut);
        CHECK(code.compare(0, 4, "seq.") == 0);
    }
}

TEST_CASE("rebuild_machine_steps on the example of requirements 8.3", "[sequence]") {
    // spool selection: index 0 -> project spool 0, index 1 -> project spool 9
    std::vector<std::uint32_t> ids;
    ids.push_back(0);
    ids.push_back(9);
    std::vector<SequenceStep> steps;
    steps.push_back(step(12, false, kStepStart, 0));      // spool 0 starts at nail 12, CW
    steps.push_back(step(87, true, kStepSegment, 0));     // step 1: 12 CW -> 87 CCW
    steps.push_back(step(40, false, kStepStart, 1));      // another spool starts, interleaved
    steps.push_back(step(3, false, kStepSegment, 1));     // step 2: 40 CW -> 3 CW, spool 9
    steps.push_back(step(12, false, kStepSegment, 0));    // step 3: 87 CCW -> 12 CW, spool 0
    steps.push_back(step(12, false, kStepEnd, 0));        // cut: no machine step
    steps.push_back(step(77, true, kStepSegment, 1));     // step 4: 3 CW -> 77 CCW, spool 9

    struct Len {
        static double f(std::uint32_t a, bool, std::uint32_t b, bool) { return a == 12 && b == 87 ? 412.35 : 1.5; }
    };
    std::vector<MachineStep> m;
    rebuild_machine_steps(steps, ids, Len::f, &m);
    REQUIRE(m.size() == 4);

    CHECK(m[0].step == 1);
    CHECK(m[0].from_nail == 12);
    CHECK(m[0].from_ccw == false);
    CHECK(m[0].to_nail == 87);
    CHECK(m[0].to_ccw == true);
    CHECK(m[0].spool_id == 0);
    CHECK(m[0].length_mm == Approx(412.35));

    CHECK(m[1].step == 2);
    CHECK(m[1].from_nail == 40);
    CHECK(m[1].to_nail == 3);
    CHECK(m[1].spool_id == 9);
    CHECK(m[1].length_mm == Approx(1.5));

    CHECK(m[2].from_nail == 87);
    CHECK(m[2].from_ccw == true);
    CHECK(m[2].to_nail == 12);
    CHECK(m[2].to_ccw == false);
    CHECK(m[2].spool_id == 0);

    CHECK(m[3].step == 4);
    CHECK(m[3].from_nail == 3);
    CHECK(m[3].from_ccw == false);
    CHECK(m[3].to_nail == 77);
    CHECK(m[3].to_ccw == true);
    CHECK(m[3].spool_id == 9);
}

TEST_CASE("rebuild is lossless: the machine list lets the records be recovered", "[sequence]") {
    std::vector<std::uint32_t> ids(1, 4);
    std::vector<SequenceStep> steps;
    steps.push_back(step(1, true, kStepStart, 0));
    for (std::uint32_t i = 2; i < 50; ++i)
        steps.push_back(step(i, (i % 3) == 0, kStepSegment, 0));
    std::vector<MachineStep> m;
    rebuild_machine_steps(steps, ids, SegmentLength(), &m);
    REQUIRE(m.size() == 48);
    CHECK(m[0].from_nail == 1);
    CHECK(m[0].from_ccw == true);
    for (std::size_t i = 0; i < m.size(); ++i) {
        CHECK(m[i].to_nail == steps[i + 1].nail_id);
        CHECK(m[i].to_ccw == steps[i + 1].ccw);
        CHECK(m[i].length_mm == 0.0); // no length function: 0
        if (i > 0) {
            CHECK(m[i].from_nail == m[i - 1].to_nail);
            CHECK(m[i].from_ccw == m[i - 1].to_ccw);
        }
    }
}

TEST_CASE("rebuild rejects a segment without a started spool and a bad spool index", "[sequence]") {
    std::vector<std::uint32_t> ids(1, 0);
    std::vector<MachineStep> m;
    std::vector<SequenceStep> s1(1, step(1, false, kStepSegment, 0));
    CHECK(issue_code([&] { rebuild_machine_steps(s1, ids, SegmentLength(), &m); }) == "seq.order");
    std::vector<SequenceStep> s2;
    s2.push_back(step(1, false, kStepStart, 0));
    s2.push_back(step(2, false, kStepEnd, 0));
    s2.push_back(step(3, false, kStepSegment, 0)); // string was cut
    CHECK(issue_code([&] { rebuild_machine_steps(s2, ids, SegmentLength(), &m); }) == "seq.order");
    std::vector<SequenceStep> s3(1, step(1, false, kStepStart, 1));
    CHECK(issue_code([&] { rebuild_machine_steps(s3, ids, SegmentLength(), &m); }) == "seq.spool");
}
