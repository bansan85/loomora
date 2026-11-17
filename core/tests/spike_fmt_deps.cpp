// Spike S-F1 of spec 002 (T009): do the two vendored dependencies of the .loomora format build
// and behave identically on a toolchain? Built as the target loomora_spike_fmt_deps (not part of
// the default build, not a ctest test), run by hand on each toolchain; the output is compared.
//
//   zlib      deflate a 1 MiB buffer (level 6 and level 1, raw deflate as ZIP stores it), inflate
//             it back, compare byte for byte.
//   RapidJSON write a document with a table of doubles, parse it with kParseFullPrecisionFlag and
//             kParseIterativeFlag, compare every double bit for bit and the re-written text.
//   SHA-256   of the deflated streams and of the JSON text, printed: two toolchains that print
//             the same lines produce the same bytes. (A private SHA-256 below: the product's
//             sha256.cpp is another task.)
//
// Plain C++11, <cstdio> only (no <iostream>, so it also builds for the toolchains that avoid it).
// Exit status 0 when every check passed.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// ---- SHA-256 (FIPS 180-4), just enough for this program ----

struct Sha256 {
    std::uint32_t h[8];
    std::uint8_t block[64];
    std::size_t fill;
    std::uint64_t total;

    Sha256() : fill(0), total(0) {
        static const std::uint32_t init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        std::memcpy(h, init, sizeof h);
    }

    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress(const std::uint8_t *p) {
        static const std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
            0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
            0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
            0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
            0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
            0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
            0xc67178f2u};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(p[4 * i]) << 24) | (static_cast<std::uint32_t>(p[4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(p[4 * i + 2]) << 8) | static_cast<std::uint32_t>(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    void update(const void *data, std::size_t len) {
        const std::uint8_t *p = static_cast<const std::uint8_t *>(data);
        total += len;
        while (len > 0) {
            const std::size_t n = len < 64 - fill ? len : 64 - fill;
            std::memcpy(block + fill, p, n);
            fill += n;
            p += n;
            len -= n;
            if (fill == 64) {
                compress(block);
                fill = 0;
            }
        }
    }

    std::string hex() {
        const std::uint64_t bits = total * 8;
        const std::uint8_t one = 0x80;
        const std::uint8_t zero = 0;
        update(&one, 1);
        while (fill != 56)
            update(&zero, 1);
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i)
            len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        update(len, 8);
        char out[65];
        for (int i = 0; i < 8; ++i)
            snprintf(out + 8 * i, 9, "%08x", static_cast<unsigned>(h[i]));
        return std::string(out, 64);
    }
};

std::string sha256_hex(const void *data, std::size_t len) {
    Sha256 s;
    s.update(data, len);
    return s.hex();
}

// ---- zlib ----

// Deterministic, compressible content: pseudo-text with repeated words and some noise.
std::vector<unsigned char> make_buffer(std::size_t n) {
    static const char *const words[] = {"frame ", "layout ", "image ", "spool ", "result ", "nail ", "{\"id\":", "0.5,", "\n"};
    std::vector<unsigned char> b;
    b.reserve(n);
    std::uint32_t x = 12345;
    while (b.size() < n) {
        x = x * 1664525u + 1013904223u;
        const char *w = words[(x >> 16) % 9];
        for (; *w != '\0' && b.size() < n; ++w)
            b.push_back(static_cast<unsigned char>(*w));
        if ((x & 0x1F) == 0 && b.size() < n)
            b.push_back(static_cast<unsigned char>(x >> 24));
    }
    return b;
}

void zlib_round_trip(int level) {
    const std::vector<unsigned char> in = make_buffer(1u << 20);
    z_stream d;
    std::memset(&d, 0, sizeof d);
    // Raw deflate (negative window bits), as a ZIP entry stores it.
    check(deflateInit2(&d, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK, "deflateInit2");
    std::vector<unsigned char> packed(deflateBound(&d, static_cast<uLong>(in.size())) + 16);
    d.next_in = const_cast<Bytef *>(&in[0]);
    d.avail_in = static_cast<uInt>(in.size());
    d.next_out = &packed[0];
    d.avail_out = static_cast<uInt>(packed.size());
    check(deflate(&d, Z_FINISH) == Z_STREAM_END, "deflate finishes in one call");
    const std::size_t packed_size = packed.size() - d.avail_out;
    deflateEnd(&d);

    z_stream z;
    std::memset(&z, 0, sizeof z);
    check(inflateInit2(&z, -15) == Z_OK, "inflateInit2");
    std::vector<unsigned char> out(in.size() + 16);
    z.next_in = &packed[0];
    z.avail_in = static_cast<uInt>(packed_size);
    z.next_out = &out[0];
    z.avail_out = static_cast<uInt>(out.size());
    check(inflate(&z, Z_FINISH) == Z_STREAM_END, "inflate finishes");
    const std::size_t out_size = out.size() - z.avail_out;
    inflateEnd(&z);

    check(out_size == in.size() && std::memcmp(&out[0], &in[0], in.size()) == 0, "inflate gives back the input");
    const uLong crc = crc32(crc32(0L, Z_NULL, 0), &in[0], static_cast<uInt>(in.size()));
    std::printf("zlib %s level %d: in=%lu deflated=%lu crc32=%08lx sha256(deflated)=%s\n", ZLIB_VERSION, level,
                static_cast<unsigned long>(in.size()), static_cast<unsigned long>(packed_size),
                static_cast<unsigned long>(crc), sha256_hex(&packed[0], packed_size).c_str());
}

// ---- RapidJSON ----

std::vector<double> make_doubles() {
    std::vector<double> v;
    const double fixed[] = {0.0, 0.1, 0.2, 0.3, 1.0 / 3.0, 2.0 / 3.0, 1e-7, 1e21, 1e22, 123456789.123456789,
                            4.35, 2.5e-308, 5e-324, 1.7976931348623157e308, 9007199254740992.0,
                            9007199254740993.0, 0.30000000000000004, -0.5, -1e-300, 3.141592653589793,
                            2.718281828459045, 1.4142135623730951, 8.5, 1e23, 8.41e21, 5.0e-5};
    for (std::size_t i = 0; i < sizeof fixed / sizeof fixed[0]; ++i)
        v.push_back(fixed[i]);
    // Doubles from pseudo-random bit patterns (finite ones only): the hard cases of shortest
    // round-trip printing and of full-precision parsing.
    std::uint64_t x = 88172645463325252ull;
    while (v.size() < 2000) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        double d;
        std::memcpy(&d, &x, sizeof d);
        if (d == d && d - d == 0.0) // not NaN, not infinite
            v.push_back(d);
    }
    return v;
}

void json_round_trip() {
    const std::vector<double> table = make_doubles();

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("format");
    w.String("loomora-spike");
    w.Key("table");
    w.StartArray();
    for (std::size_t i = 0; i < table.size(); ++i)
        w.Double(table[i]);
    w.EndArray();
    w.Key("unicode");
    w.String("\xC3\xA9t\xC3\xA9 \xE6\x97\xA5 \xF0\x9F\x98\x80");
    w.EndObject();
    const std::string text(sb.GetString(), sb.GetSize());

    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseFullPrecisionFlag | rapidjson::kParseIterativeFlag>(text.c_str());
    check(!doc.HasParseError(), "the document parses");
    if (doc.HasParseError())
        return;
    check(doc.IsObject() && doc.HasMember("table") && doc["table"].IsArray() && doc["table"].Size() == table.size(),
          "the table has the written size");
    bool exact = true;
    if (doc.IsObject() && doc.HasMember("table") && doc["table"].IsArray() && doc["table"].Size() == table.size()) {
        for (std::size_t i = 0; i < table.size(); ++i) {
            const rapidjson::Value &v = doc["table"][static_cast<rapidjson::SizeType>(i)];
            const double got = v.GetDouble();
            if (std::memcmp(&got, &table[i], sizeof got) != 0 && !(got == 0.0 && table[i] == 0.0)) {
                exact = false;
                std::printf("FAIL: double %lu does not round-trip\n", static_cast<unsigned long>(i));
                break;
            }
        }
    }
    check(exact, "every double round-trips bit for bit");

    rapidjson::StringBuffer sb2;
    rapidjson::Writer<rapidjson::StringBuffer> w2(sb2);
    doc.Accept(w2);
    check(std::string(sb2.GetString(), sb2.GetSize()) == text, "re-writing the parsed document gives the same text");

    std::printf("rapidjson: %lu doubles, %lu bytes, sha256(json)=%s\n", static_cast<unsigned long>(table.size()),
                static_cast<unsigned long>(text.size()), sha256_hex(text.data(), text.size()).c_str());
}

} // namespace

int main() {
    std::printf("S-F1 sizeof(void*)=%u sizeof(long)=%u\n", static_cast<unsigned>(sizeof(void *)),
                static_cast<unsigned>(sizeof(long)));
    zlib_round_trip(6);
    zlib_round_trip(1);
    json_round_trip();
    std::printf(g_failures == 0 ? "S-F1: ok\n" : "S-F1: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
