// The ZIP writer (format/zip/zip_writer): fixed header values, mimetype first, ZIP64 only when
// needed, round trip with the reader, copy-through, determinism.
#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../../include/loomora.h"
#include "format/io/memory_io.hpp"
#include "format/limits.hpp"
#include "format/testing.hpp"
#include "format/zip/zip_reader.hpp"
#include "format/zip/zip_writer.hpp"
#include "zip_builder.hpp"

using namespace loomora::format;
using ziptest::Bytes;
using ziptest::get16;
using ziptest::get32;
using ziptest::to_bytes;

namespace {

// ---- a small SHA-256 (the golden hash below must not depend on the code under test) ----------
std::string sha256_hex(const Bytes &data) {
    static const std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    Bytes m = data;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56)
        m.push_back(0);
    for (int i = 7; i >= 0; --i)
        m.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(m[off + 4 * i]) << 24) | (static_cast<std::uint32_t>(m[off + 4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(m[off + 4 * i + 2]) << 8) | m[off + 4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = ((w[i - 15] >> 7) | (w[i - 15] << 25)) ^ ((w[i - 15] >> 18) | (w[i - 15] << 14)) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = ((w[i - 2] >> 17) | (w[i - 2] << 15)) ^ ((w[i - 2] >> 19) | (w[i - 2] << 13)) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
            const std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    static const char *hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 8; ++i)
        for (int j = 28; j >= 0; j -= 4)
            out += hex[(h[i] >> j) & 0xF];
    return out;
}

// ---- helpers ----------------------------------------------------------------------------------
Bytes pattern(std::size_t n, unsigned seed) {
    Bytes b(n);
    std::uint32_t x = seed * 2654435761u + 1;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        b[i] = static_cast<std::uint8_t>(x >> 24);
    }
    return b;
}

Bytes text(std::size_t n) {
    Bytes b;
    const std::string unit = "{\"nail\":123,\"x\":4.5,\"y\":-6.25},";
    while (b.size() < n)
        b.insert(b.end(), unit.begin(), unit.end());
    b.resize(n);
    return b;
}

template <class F> int code_thrown_by(F fn) {
    try {
        fn();
    } catch (...) {
        return code_of_current_exception();
    }
    return 0;
}

// A small archive in the canonical shape: mimetype, a deflated JSON, a stored binary.
Bytes small_archive() {
    MemorySink sink;
    ZipWriter w(sink);
    w.add_mimetype();
    const std::string manifest = "{\"format\":\"loomora\",\"version\":\"1.0\"}";
    w.add_deflate("manifest.json", manifest.data(), manifest.size());
    const Bytes bin = pattern(100, 7);
    w.add_stored("frames/1/mesh.bin", &bin[0], bin.size());
    w.finish();
    sink.commit();
    return sink.take();
}

struct Central {
    std::string name;
    unsigned made_by, needed, flags, method, time, date, extra_len, comment_len, disk;
    std::uint32_t crc, csize, usize, eattr, offset;
};

// The central directory records of `a`, read with the test's own parser.
std::vector<Central> central_of(const Bytes &a) {
    MemorySource src(&a[0], a.size());
    ZipReader r(src);
    r.open();
    std::vector<Central> v;
    std::size_t pos = static_cast<std::size_t>(r.central_directory_offset());
    while (pos + 46 <= a.size() && get32(a, pos) == 0x02014b50u) {
        Central c;
        c.made_by = get16(a, pos + 4);
        c.needed = get16(a, pos + 6);
        c.flags = get16(a, pos + 8);
        c.method = get16(a, pos + 10);
        c.time = get16(a, pos + 12);
        c.date = get16(a, pos + 14);
        c.crc = get32(a, pos + 16);
        c.csize = get32(a, pos + 20);
        c.usize = get32(a, pos + 24);
        const unsigned nl = get16(a, pos + 28);
        c.extra_len = get16(a, pos + 30);
        c.comment_len = get16(a, pos + 32);
        c.disk = get16(a, pos + 34);
        c.eattr = get32(a, pos + 38);
        c.offset = get32(a, pos + 42);
        c.name.assign(a.begin() + pos + 46, a.begin() + pos + 46 + nl);
        v.push_back(c);
        pos += 46 + nl + c.extra_len + c.comment_len;
    }
    return v;
}

} // namespace

TEST_CASE("mimetype is the first entry, stored, at offset 0, with no extra field", "[zipw]") {
    const Bytes a = small_archive();
    REQUIRE(a.size() > 100);
    CHECK(get32(a, 0) == 0x04034b50u);
    CHECK(get16(a, 8) == 0);                       // stored
    CHECK(get16(a, 26) == 8);                      // name length
    CHECK(get16(a, 28) == 0);                      // no extra field
    CHECK(std::string(a.begin() + 30, a.begin() + 38) == "mimetype");
    CHECK(std::string(a.begin() + 38, a.begin() + 66) == "application/vnd.loomora+zip");
}

TEST_CASE("every header field has its fixed value", "[zipw]") {
    const Bytes a = small_archive();
    const std::vector<Central> c = central_of(a);
    REQUIRE(c.size() == 3);
    for (std::size_t i = 0; i < c.size(); ++i) {
        INFO(c[i].name);
        CHECK(c[i].made_by == 0x0314);
        CHECK(c[i].needed == 20);
        CHECK(c[i].flags == 0);
        CHECK(c[i].time == 0x0000);
        CHECK(c[i].date == 0x0021);
        CHECK(c[i].extra_len == 0);
        CHECK(c[i].comment_len == 0);
        CHECK(c[i].disk == 0);
        CHECK(c[i].eattr == 0x81A40000u);
    }
    CHECK(c[0].name == "mimetype");
    CHECK(c[1].method == 8);
    CHECK(c[2].method == 0);
    // The local headers carry the same fixed values.
    for (std::size_t i = 0; i < c.size(); ++i) {
        const std::size_t lo = c[i].offset;
        CHECK(get16(a, lo + 4) == 20);
        CHECK(get16(a, lo + 6) == 0);
        CHECK(get16(a, lo + 10) == 0x0000);
        CHECK(get16(a, lo + 12) == 0x0021);
        CHECK(get16(a, lo + 28) == 0); // no extra field
    }
    // End record: this disk, no comment, entries in the order written.
    const std::size_t end = a.size() - 22;
    CHECK(get32(a, end) == 0x06054b50u);
    CHECK(get16(a, end + 10) == 3);
    CHECK(get16(a, end + 20) == 0);
    MemorySource src(&a[0], a.size());
    ZipReader r(src);
    r.open();
    CHECK(get32(a, end + 16) == r.central_directory_offset());
}

TEST_CASE("the flag bit 11 is set only for a name that is not ASCII", "[zipw]") {
    MemorySink sink;
    ZipWriter w(sink);
    w.add_mimetype();
    w.add_stored("x-ns/plain.bin", "a", 1);
    w.add_stored("x-ns/\xC3\xA9t\xC3\xA9.bin", "b", 1);
    w.finish();
    const Bytes a = sink.bytes();
    const std::vector<Central> c = central_of(a);
    REQUIRE(c.size() == 3);
    CHECK(c[1].flags == 0);
    CHECK(c[2].flags == 0x0800);
    CHECK(get16(a, c[2].offset + 6) == 0x0800);
}

TEST_CASE("stored and deflate entries round trip through the reader", "[zipw]") {
    MemorySink sink;
    ZipWriter w(sink);
    w.add_mimetype();
    const Bytes json = text(300000);
    const Bytes bin = pattern(70000, 3);
    w.add_deflate("a/data.json", &json[0], json.size());
    w.add_stored("a/data.bin", &bin[0], bin.size());
    w.add_deflate("empty-deflate", nullptr, 0);
    w.add_stored("empty-stored", nullptr, 0);
    w.add_deflate("x-ns/random.bin", &bin[0], bin.size());
    w.finish();
    CHECK(w.entry_count() == 6);
    const Bytes a = sink.bytes();
    MemorySource src(&a[0], a.size());
    ZipReader r(src);
    r.open();
    REQUIRE(r.entries().size() == 6);
    CHECK(r.find("a/data.json")->method == 8);
    CHECK(r.find("a/data.json")->compressed_size < json.size() / 4);
    CHECK(r.find("a/data.bin")->method == 0);
    CHECK(r.read_all(*r.find("a/data.json"), 1u << 30) == json);
    CHECK(r.read_all(*r.find("a/data.bin"), 1u << 30) == bin);
    CHECK(r.read_all(*r.find("x-ns/random.bin"), 1u << 30) == bin);
    CHECK(r.read_all(*r.find("empty-deflate"), 10).empty());
    CHECK(r.read_all(*r.find("empty-stored"), 10).empty());
}

TEST_CASE("an entry may be written from a Source or in chunks, the bytes do not change", "[zipw]") {
    const Bytes json = text(200000);
    MemorySink one;
    {
        ZipWriter w(one);
        w.add_mimetype();
        w.add_deflate("a.json", &json[0], json.size());
        w.add_stored("a.bin", &json[0], json.size());
        w.finish();
    }
    MemorySink src_sink;
    {
        MemorySource src(&json[0], json.size());
        ZipWriter w(src_sink);
        w.add_mimetype();
        w.add_deflate("a.json", src);
        w.add_stored("a.bin", src);
        w.finish();
    }
    MemorySink chunked;
    {
        ZipWriter w(chunked);
        w.add_mimetype();
        const std::size_t sizes[] = {1, 7, 4096, 33, 65536, 5};
        w.begin_entry("a.json", 8, json.size());
        std::size_t pos = 0, k = 0;
        while (pos < json.size()) {
            const std::size_t n = std::min(sizes[k++ % 6], json.size() - pos);
            w.write(&json[pos], n);
            pos += n;
        }
        w.end_entry();
        w.begin_entry("a.bin", 0, json.size());
        w.write(&json[0], 10);
        w.write(&json[10], json.size() - 10);
        w.end_entry();
        w.finish();
    }
    CHECK(one.bytes() == src_sink.bytes());
    CHECK(one.bytes() == chunked.bytes());
}

TEST_CASE("the deflate stream is zlib level 6, window 15, memLevel 8, default strategy", "[zipw]") {
    const Bytes json = text(50000);
    MemorySink sink;
    ZipWriter w(sink);
    w.add_mimetype();
    w.add_deflate("a.json", &json[0], json.size());
    w.finish();
    const Bytes a = sink.bytes();
    MemorySource src(&a[0], a.size());
    ZipReader r(src);
    r.open();
    const ZipEntry &e = *r.find("a.json");
    Bytes raw(e.compressed_size);
    REQUIRE(r.read_raw(e, 0, &raw[0], raw.size()) == raw.size());
    CHECK(raw == ziptest::deflate_raw(json)); // the builder uses the same fixed parameters
}

TEST_CASE("no ZIP64 structure is written when nothing needs it", "[zipw][zip64]") {
    const Bytes a = small_archive();
    const std::vector<Central> c = central_of(a);
    for (std::size_t i = 0; i < c.size(); ++i) {
        CHECK(c[i].extra_len == 0);
        CHECK(c[i].needed == 20);
    }
    CHECK(get32(a, a.size() - 22) == 0x06054b50u);
    CHECK(get32(a, a.size() - 22 - 20) != 0x07064b50u);
}

TEST_CASE("ZIP64 is written when a size, an offset or the count reaches its threshold", "[zipw][zip64]") {
    ScopedLimits guard;
    limits().zip64_size_threshold = 100;
    limits().zip64_offset_threshold = 400;
    limits().zip64_count_threshold = 6;
    const Bytes big = pattern(150, 1);       // size over the threshold
    const Bytes small = to_bytes("{\"k\":1}"); // far below, even as a worst case deflate
    const Bytes edge = text(90);              // 90 + worst-case growth reaches 100: ZIP64 for deflate
    MemorySink sink;
    ZipWriter w(sink);
    w.add_mimetype();
    w.add_stored("big.bin", &big[0], big.size());
    w.add_deflate("small.json", &small[0], small.size());
    w.add_deflate("edge.json", &edge[0], edge.size());
    w.add_stored("late1.bin", &small[0], small.size());
    w.add_stored("late2.bin", &small[0], small.size());
    w.finish();
    const Bytes a = sink.bytes();
    MemorySource src(&a[0], a.size());
    ZipReader r(src);
    r.open();
    REQUIRE(r.entries().size() == 6);
    CHECK(r.read_all(*r.find("big.bin"), 1000) == big);
    CHECK(r.read_all(*r.find("small.json"), 1000) == small);
    CHECK(r.read_all(*r.find("edge.json"), 1000) == edge);
    CHECK(r.read_all(*r.find("late2.bin"), 1000) == small);

    const std::vector<Central> c = central_of(a);
    REQUIRE(c.size() == 6);
    // mimetype: nothing ZIP64.
    CHECK(c[0].extra_len == 0);
    CHECK(c[0].needed == 20);
    // big.bin: sizes saturated, a 4 + 16 byte extra field, version needed 45, local header too.
    CHECK(c[1].csize == 0xFFFFFFFFu);
    CHECK(c[1].usize == 0xFFFFFFFFu);
    CHECK(c[1].needed == 45);
    CHECK(c[1].extra_len == 20);
    CHECK(get16(a, c[1].offset + 4) == 45);
    CHECK(get32(a, c[1].offset + 18) == 0xFFFFFFFFu);
    CHECK(get16(a, c[1].offset + 28) == 20);
    // small.json: classic.
    CHECK(c[2].csize != 0xFFFFFFFFu);
    CHECK(c[2].needed == 20);
    CHECK(c[2].extra_len == 0);
    // edge.json: ZIP64 sizes because the deflate worst case reaches the threshold.
    CHECK(c[3].usize == 0xFFFFFFFFu);
    // an entry whose offset is past the offset threshold: only the offset is ZIP64.
    bool offset_case = false;
    for (std::size_t i = 4; i < c.size(); ++i) {
        if (c[i].offset == 0xFFFFFFFFu) {
            offset_case = true;
            CHECK(c[i].csize != 0xFFFFFFFFu);
            CHECK(c[i].extra_len == 12); // 4 + 8
            CHECK(c[i].needed == 45);
        }
    }
    CHECK(offset_case);
    // six entries reach the count threshold: ZIP64 end record and locator, saturated count.
    const std::size_t end = a.size() - 22;
    CHECK(get32(a, end) == 0x06054b50u);
    CHECK(get16(a, end + 10) == 0xFFFFu);
    CHECK(get32(a, end - 20) == 0x07064b50u);
    CHECK(get32(a, end - 20 - 56) == 0x06064b50u);
}

TEST_CASE("ZIP64 from the count alone", "[zipw][zip64]") {
    ScopedLimits guard;
    limits().zip64_count_threshold = 3;
    MemorySink sink;
    ZipWriter w(sink);
    w.add_mimetype();
    w.add_stored("a", "1", 1);
    w.add_stored("b", "2", 1);
    w.finish();
    const Bytes a = sink.bytes();
    const std::size_t end = a.size() - 22;
    CHECK(get32(a, end - 20) == 0x07064b50u);
    const std::vector<Central> c = central_of(a);
    for (std::size_t i = 0; i < c.size(); ++i)
        CHECK(c[i].extra_len == 0);
    MemorySource src(&a[0], a.size());
    ZipReader r(src);
    r.open();
    CHECK(r.entries().size() == 3);
}

TEST_CASE("a deflate entry written with ZIP64 sizes patches the local header", "[zipw][zip64]") {
    ScopedLimits guard;
    limits().zip64_size_threshold = 1000;
    const Bytes json = text(20000); // compresses far below the threshold, still ZIP64: size 20000
    MemorySink sink;
    ZipWriter w(sink);
    w.add_mimetype();
    w.add_deflate("a.json", &json[0], json.size());
    w.finish();
    const Bytes a = sink.bytes();
    MemorySource src(&a[0], a.size());
    ZipReader r(src);
    r.open();
    const ZipEntry &e = *r.find("a.json");
    CHECK(e.size == 20000);
    CHECK(e.compressed_size < 1000);
    CHECK(r.read_all(e, 1u << 20) == json);
    // local header: saturated sizes, real ones in the extra field
    CHECK(get32(a, e.local_offset + 22) == 0xFFFFFFFFu);
    CHECK(get32(a, e.local_offset + 18) == 0xFFFFFFFFu);
}

TEST_CASE("the same project written twice gives the same bytes (golden SHA-256)", "[zipw][golden]") {
    const Bytes first = small_archive();
    const Bytes second = small_archive();
    CHECK(first == second);
    // The hash covers the zlib stream of the vendored zlib 1.3.1 and every fixed header value; a
    // change of either is a visible change of this constant.
    CHECK(sha256_hex(first) == "0000000000000000000000000000000000000000000000000000000000000000");
}

TEST_CASE("copy-through writes the compressed bytes as they are", "[zipw][raw]") {
    // An archive from another "writer": the builder, with a deflate stream that differs from ours.
    std::vector<ziptest::Entry> v;
    ziptest::Entry e = ziptest::file("x-ns/data.json", "{\"a\":[1,2,3,4,5,6,7,8,9,10]}", 8);
    e.has_payload = true;
    // The same content as a stored deflate block (valid, and not what zlib would produce).
    const std::string content = "{\"a\":[1,2,3,4,5,6,7,8,9,10]}";
    e.payload.push_back(0x01);
    ziptest::put16(e.payload, static_cast<unsigned>(content.size()));
    ziptest::put16(e.payload, static_cast<unsigned>(~content.size() & 0xFFFF));
    e.payload.insert(e.payload.end(), content.begin(), content.end());
    v.push_back(e);
    v.push_back(ziptest::file("x-ns/blob.bin", "stored bytes", 0));
    const ziptest::Archive in = ziptest::build(ziptest::with_mimetype(v));

    MemorySource insrc(&in.bytes[0], in.bytes.size());
    ZipReader reader(insrc);
    reader.open();

    MemorySink sink;
    ZipWriter w(sink);
    w.add_mimetype();
    w.add_raw("x-ns/data.json", reader, *reader.find("x-ns/data.json"));
    w.add_raw("x-ns/blob.bin", reader, *reader.find("x-ns/blob.bin"));
    w.finish();
    const Bytes out = sink.bytes();
    MemorySource outsrc(&out[0], out.size());
    ZipReader back(outsrc);
    back.open();
    const ZipEntry &d = *back.find("x-ns/data.json");
    const ZipEntry &d0 = *reader.find("x-ns/data.json");
    CHECK(d.method == 8);
    CHECK(d.crc32 == d0.crc32);
    CHECK(d.compressed_size == d0.compressed_size);
    Bytes a(d.compressed_size), b(d0.compressed_size);
    back.read_raw(d, 0, &a[0], a.size());
    reader.read_raw(d0, 0, &b[0], b.size());
    CHECK(a == b);
    CHECK(back.read_all(d, 1000) == to_bytes(content));
    CHECK(back.read_all(*back.find("x-ns/blob.bin"), 100) == to_bytes("stored bytes"));
    // the generic form takes any source and offset
    MemorySink sink2;
    ZipWriter w2(sink2);
    w2.add_mimetype();
    w2.add_raw("copy.json", 8, d0.crc32, d0.compressed_size, d0.size, insrc, d0.data_offset);
    w2.finish();
    const Bytes out2 = sink2.bytes();
    MemorySource s2(&out2[0], out2.size());
    ZipReader r2(s2);
    r2.open();
    CHECK(r2.read_all(*r2.find("copy.json"), 1000) == to_bytes(content));
}

TEST_CASE("a bad use of the writer is an internal error", "[zipw][misuse]") {
    const int internal = LOOMORA_ERR_INTERNAL;
    SECTION("first entry is not the mimetype") {
        MemorySink sink;
        ZipWriter w(sink);
        CHECK(code_thrown_by([&] { w.add_stored("manifest.json", "{}", 2); }) == internal);
    }
    SECTION("mimetype added twice or late") {
        MemorySink sink;
        ZipWriter w(sink);
        w.add_mimetype();
        CHECK(code_thrown_by([&] { w.add_mimetype(); }) == internal);
        CHECK(code_thrown_by([&] { w.add_stored("mimetype", "x", 1); }) == internal);
    }
    SECTION("finish without mimetype") {
        MemorySink sink;
        ZipWriter w(sink);
        CHECK(code_thrown_by([&] { w.finish(); }) == internal);
    }
    SECTION("bad and duplicate names") {
        MemorySink sink;
        ZipWriter w(sink);
        w.add_mimetype();
        CHECK(code_thrown_by([&] { w.add_stored("../x", "x", 1); }) == internal);
        CHECK(code_thrown_by([&] { w.add_stored("dir/", "", 0); }) == internal);
        CHECK(code_thrown_by([&] { w.add_stored("", "x", 1); }) == internal);
        w.add_stored("A.bin", "x", 1);
        CHECK(code_thrown_by([&] { w.add_stored("a.BIN", "x", 1); }) == internal);
    }
    SECTION("a declared size that is not the size written") {
        MemorySink sink;
        ZipWriter w(sink);
        w.add_mimetype();
        w.begin_entry("a.bin", 0, 10);
        w.write("12345", 5);
        CHECK(code_thrown_by([&] { w.end_entry(); }) == internal);
        MemorySink sink2;
        ZipWriter w2(sink2);
        w2.add_mimetype();
        w2.begin_entry("a.bin", 8, 3);
        CHECK(code_thrown_by([&] { w2.write("12345", 5); }) == internal);
    }
    SECTION("an unknown method") {
        MemorySink sink;
        ZipWriter w(sink);
        w.add_mimetype();
        CHECK(code_thrown_by([&] { w.begin_entry("a.bin", 9, 1); }) == internal);
    }
    SECTION("calls after finish, entry within an entry") {
        MemorySink sink;
        ZipWriter w(sink);
        w.add_mimetype();
        w.begin_entry("a.bin", 0, 1);
        CHECK(code_thrown_by([&] { w.begin_entry("b.bin", 0, 1); }) == internal);
        CHECK(code_thrown_by([&] { w.finish(); }) == internal);
        w.write("x", 1);
        w.end_entry();
        w.finish();
        CHECK(code_thrown_by([&] { w.add_stored("c.bin", "x", 1); }) == internal);
        CHECK(code_thrown_by([&] { w.finish(); }) == internal);
    }
}

TEST_CASE("a failing sink stops the writer with the sink's error", "[zipw][io]") {
    struct Hook {
        static bool fail_second(void *user, std::uint64_t, std::size_t) {
            return ++*static_cast<int *>(user) == 2;
        }
    };
    ScopedHooks guard;
    int count = 0;
    hooks().user = &count;
    hooks().fail_write = &Hook::fail_second;
    MemorySink sink;
    ZipWriter w(sink);
    CHECK(code_thrown_by([&] {
              w.add_mimetype();
              w.add_stored("a.bin", "x", 1);
          }) == LOOMORA_ERR_IO);
}

TEST_CASE("the progress callback cancels a write", "[zipw][progress]") {
    struct Cb {
        static int stop(void *, std::uint64_t, std::uint64_t) { return 1; }
    };
    ScopedLimits guard;
    limits().progress_interval_bytes = 1;
    MemorySink sink;
    Progress p(&Cb::stop, nullptr, 0);
    ZipWriter w(sink, &p);
    const Bytes data = pattern(1000, 5);
    CHECK(code_thrown_by([&] {
              w.add_mimetype();
              w.add_deflate("a.bin", &data[0], data.size());
          }) == LOOMORA_ERR_CANCELLED);
}

TEST_CASE("an archive of many entries written and read back", "[zipw]") {
    MemorySink sink;
    ZipWriter w(sink);
    w.add_mimetype();
    for (int i = 0; i < 300; ++i) {
        const std::string name = "frames/" + std::to_string(i) + "/frame.json";
        const std::string body = "{\"id\":" + std::to_string(i) + "}";
        w.add_deflate(name, body.data(), body.size());
    }
    w.finish();
    const Bytes a = sink.bytes();
    MemorySource src(&a[0], a.size());
    ZipReader r(src);
    r.open();
    REQUIRE(r.entries().size() == 301);
    CHECK(r.read_all(*r.find("frames/299/frame.json"), 100) == to_bytes("{\"id\":299}"));
}
