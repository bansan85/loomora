// Image container handling (format/codec/image_meta): metadata strip for PNG, JPEG and WebP built
// byte by byte here, structure checks, PNG IHDR size and the thumbnail limits (SC-009, FR-011).
#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "zlib.h"
#include "../../include/loomora.h"
#include "../../include/loomora_project.h"
#include "format/codec/image_meta.hpp"
#include "format/limits.hpp"
#include "format/report.hpp"

using namespace loomora::format;
typedef std::vector<std::uint8_t> Bytes;

namespace {

void put(Bytes &b, const std::string &s) { b.insert(b.end(), s.begin(), s.end()); }
void put(Bytes &b, const Bytes &s) { b.insert(b.end(), s.begin(), s.end()); }
void be32(Bytes &b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 24));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void le32(Bytes &b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 24));
}
Bytes str(const std::string &s) { return Bytes(s.begin(), s.end()); }
bool contains(const Bytes &b, const std::string &needle) {
    return std::search(b.begin(), b.end(), needle.begin(), needle.end()) != b.end();
}

// ---- PNG ----
Bytes png_chunk(const std::string &type, const Bytes &data) {
    Bytes b;
    be32(b, static_cast<std::uint32_t>(data.size()));
    Bytes td = str(type);
    put(td, data);
    put(b, td);
    be32(b, static_cast<std::uint32_t>(crc32(0, &td[0], static_cast<unsigned>(td.size()))));
    return b;
}
Bytes png_ihdr(std::uint32_t w, std::uint32_t h) {
    Bytes d;
    be32(d, w);
    be32(d, h);
    d.push_back(8); // bit depth
    d.push_back(2); // RGB
    d.push_back(0);
    d.push_back(0);
    d.push_back(0);
    return png_chunk("IHDR", d);
}
const unsigned char kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
Bytes png_sig() { return Bytes(kPngSig, kPngSig + 8); }
Bytes idat(std::size_t n, std::uint8_t seed) {
    Bytes d(n);
    for (std::size_t i = 0; i < n; ++i)
        d[i] = static_cast<std::uint8_t>(seed + i * 7);
    return png_chunk("IDAT", d);
}

// ---- JPEG ----
Bytes jseg(int marker, const Bytes &payload) {
    Bytes b;
    b.push_back(0xFF);
    b.push_back(static_cast<std::uint8_t>(marker));
    const std::size_t len = payload.size() + 2;
    b.push_back(static_cast<std::uint8_t>(len >> 8));
    b.push_back(static_cast<std::uint8_t>(len));
    put(b, payload);
    return b;
}
Bytes jmarker(int marker) {
    Bytes b;
    b.push_back(0xFF);
    b.push_back(static_cast<std::uint8_t>(marker));
    return b;
}
Bytes cstr(const std::string &s) { // NUL-terminated text
    Bytes b = str(s);
    b.push_back(0);
    return b;
}
Bytes cat(const Bytes &a, const Bytes &b) {
    Bytes r = a;
    put(r, b);
    return r;
}

// ---- WebP ----
Bytes riff_chunk(const std::string &fourcc, const Bytes &payload) {
    Bytes b = str(fourcc);
    le32(b, static_cast<std::uint32_t>(payload.size()));
    put(b, payload);
    if (payload.size() & 1)
        b.push_back(0);
    return b;
}
Bytes riff(const Bytes &chunks) {
    Bytes b = str("RIFF");
    le32(b, static_cast<std::uint32_t>(4 + chunks.size()));
    put(b, "WEBP");
    put(b, chunks);
    return b;
}
Bytes vp8x(std::uint8_t flags) {
    Bytes p(10, 0);
    p[0] = flags;
    p[4] = 15; // canvas width - 1 (24 bits)
    p[7] = 15;
    return riff_chunk("VP8X", p);
}

template <class F> int code_of(F fn) {
    try {
        fn();
    } catch (const FormatError &e) {
        return e.code();
    }
    return 0;
}

Bytes strip(const Bytes &b, ImageFormat f = kImageUnknown) {
    return strip_image_metadata(b.empty() ? nullptr : &b[0], b.size(), f);
}

} // namespace

TEST_CASE("format detection by signature", "[image]") {
    CHECK(detect_image_format(&png_sig()[0], 8) == kImagePng);
    const std::uint8_t jpg[3] = {0xFF, 0xD8, 0xFF};
    CHECK(detect_image_format(jpg, 3) == kImageJpeg);
    const Bytes w = riff(riff_chunk("VP8L", Bytes(5, 1)));
    CHECK(detect_image_format(&w[0], w.size()) == kImageWebp);
    const std::uint8_t junk[4] = {1, 2, 3, 4};
    CHECK(detect_image_format(junk, 4) == kImageUnknown);
    CHECK(detect_image_format(junk, 0) == kImageUnknown);
    CHECK(std::string(image_format_name(kImagePng)) == "png");
    CHECK(std::string(image_format_name(kImageJpeg)) == "jpeg");
    CHECK(std::string(image_format_name(kImageWebp)) == "webp");
}

TEST_CASE("PNG: metadata chunks are dropped, pixel chunks stay byte for byte", "[image]") {
    const Bytes keep_head = cat(png_sig(), png_ihdr(4, 3));
    Bytes gama;
    be32(gama, 45455);
    const Bytes kept = cat(png_chunk("gAMA", gama), cat(idat(40, 1), idat(33, 9)));
    const Bytes iend = png_chunk("IEND", Bytes());

    Bytes in = keep_head;
    put(in, png_chunk("eXIf", cat(str("MM\0*"), cat(str("GPSLatitude"), str("Canon EOS")))));
    put(in, png_chunk("tEXt", cat(cstr("Comment"), str("taken at home"))));
    put(in, png_chunk("gAMA", gama));
    put(in, png_chunk("iTXt", cat(cstr("XML:com.adobe.xmp"), str("<x:xmpmeta>GPSLongitude</x:xmpmeta>"))));
    put(in, png_chunk("zTXt", cat(cstr("Raw profile type exif"), str("xxxx"))));
    put(in, png_chunk("tIME", Bytes(7, 1)));
    put(in, idat(40, 1));
    put(in, png_chunk("vpAg", Bytes(9, 2))); // unknown ancillary chunk
    put(in, idat(33, 9));
    put(in, iend);
    put(in, str("TRAILING GPS DATA"));

    Bytes want = keep_head;
    put(want, kept);
    put(want, iend);

    std::size_t removed = 0;
    const Bytes got = strip_image_metadata(&in[0], in.size(), kImageUnknown, &removed);
    CHECK(got == want);
    CHECK(removed == in.size() - want.size());
    CHECK(!contains(got, "GPS"));
    CHECK(!contains(got, "Canon"));
    CHECK(!contains(got, "xmp"));
    CHECK(!contains(got, "eXIf"));
    CHECK(!contains(got, "tEXt"));

    // stripping again changes nothing; naming the format is accepted
    CHECK(strip(got) == got);
    CHECK(strip(in, kImagePng) == want);
}

TEST_CASE("PNG: structure errors are rejected", "[image]") {
    Bytes ok = cat(png_sig(), png_ihdr(2, 2));
    put(ok, idat(20, 3));
    put(ok, png_chunk("IEND", Bytes()));
    REQUIRE(strip(ok).size() == ok.size());

    SECTION("not an image / wrong declared format") {
        CHECK(code_of([&] { strip(str("hello world, not an image")); }) == LOOMORA_ERR_INVALID_ITEM);
        CHECK(code_of([&] { strip(ok, kImageJpeg); }) == LOOMORA_ERR_INVALID_ITEM);
        CHECK(code_of([&] { strip(Bytes()); }) == LOOMORA_ERR_INVALID_ITEM);
    }
    SECTION("chunk CRC mismatch") {
        Bytes b = ok;
        b[b.size() - 20] ^= 0x10; // inside the IDAT data
        CHECK(code_of([&] { strip(b); }) == LOOMORA_ERR_INVALID_ITEM);
    }
    SECTION("IHDR not first, missing IEND, truncated, chunk length past the end") {
        Bytes b = png_sig();
        put(b, idat(5, 1));
        put(b, png_ihdr(2, 2));
        put(b, png_chunk("IEND", Bytes()));
        CHECK(code_of([&] { strip(b); }) == LOOMORA_ERR_INVALID_ITEM);

        Bytes noend(ok.begin(), ok.end() - 12);
        CHECK(code_of([&] { strip(noend); }) == LOOMORA_ERR_INVALID_ITEM);
        for (std::size_t n = 0; n < ok.size(); n += 3) {
            Bytes cut(ok.begin(), ok.begin() + static_cast<std::ptrdiff_t>(n));
            CHECK(code_of([&] { strip(cut); }) == LOOMORA_ERR_INVALID_ITEM);
        }
        Bytes huge = ok;
        huge[8 + 25 + 0] = 0x7F; // IDAT length becomes enormous
        CHECK(code_of([&] { strip(huge); }) == LOOMORA_ERR_INVALID_ITEM);
    }
}

TEST_CASE("PNG IHDR size", "[image]") {
    const Bytes p = cat(png_sig(), png_ihdr(1200, 800));
    const PngHeader h = read_png_header(&p[0], p.size());
    CHECK(h.width == 1200);
    CHECK(h.height == 800);
    CHECK(h.bit_depth == 8);
    CHECK(h.color_type == 2);
    const Bytes zero = cat(png_sig(), png_ihdr(0, 5));
    CHECK(code_of([&] { read_png_header(&zero[0], zero.size()); }) == LOOMORA_ERR_INVALID_ITEM);
    const Bytes big = cat(png_sig(), png_ihdr(0x80000000u, 5));
    CHECK(code_of([&] { read_png_header(&big[0], big.size()); }) == LOOMORA_ERR_INVALID_ITEM);
    Bytes crc = p;
    crc[16] ^= 1; // width
    CHECK(code_of([&] { read_png_header(&crc[0], crc.size()); }) == LOOMORA_ERR_INVALID_ITEM);
    CHECK(code_of([&] { read_png_header(&p[0], 20); }) == LOOMORA_ERR_INVALID_ITEM);
    const Bytes notpng = str("GIF89a....................");
    CHECK(code_of([&] { read_png_header(&notpng[0], notpng.size()); }) == LOOMORA_ERR_INVALID_ITEM);
}

TEST_CASE("JPEG: APP1, APP13, COM and foreign APPn are dropped, the scan is copied", "[image]") {
    const Bytes soi = jmarker(0xD8);
    const Bytes jfif = jseg(0xE0, cat(cstr("JFIF"), Bytes(9, 1)));
    const Bytes icc = jseg(0xE2, cat(cstr("ICC_PROFILE"), Bytes(20, 5)));
    const Bytes adobe = jseg(0xEE, cat(str("Adobe"), Bytes(7, 0)));
    const Bytes dqt = jseg(0xDB, Bytes(65, 3));
    const Bytes sof = jseg(0xC0, Bytes(15, 2));
    const Bytes dht = jseg(0xC4, Bytes(30, 4));
    const Bytes sos = jseg(0xDA, Bytes(10, 1));
    Bytes scan;
    for (int i = 0; i < 50; ++i)
        scan.push_back(static_cast<std::uint8_t>(i * 3 + 1 > 0xFE ? 0x11 : i * 3 + 1));
    scan.push_back(0xFF);
    scan.push_back(0x00); // stuffed byte
    scan.push_back(0xFF);
    scan.push_back(0xD0); // RST0
    scan.push_back(0x42);
    scan.push_back(0xFF); // fill byte
    scan.push_back(0xFF);
    scan.push_back(0xD1); // RST1
    scan.push_back(0x07);
    const Bytes eoi = jmarker(0xD9);

    const Bytes exif = jseg(0xE1, cat(cstr("Exif"), str(" MM GPSLatitude N 48.85 Canon EOS 5D")));
    const Bytes xmp = jseg(0xE1, cat(cstr("http://ns.adobe.com/xap/1.0/"), str("<x:xmpmeta>GPS</x:xmpmeta>")));
    const Bytes ps = jseg(0xED, cat(cstr("Photoshop 3.0"), str("8BIM IPTC John Doe")));
    const Bytes com = jseg(0xFE, str("shot with a phone"));
    const Bytes mpf = jseg(0xE2, cat(cstr("MPF"), Bytes(12, 9)));
    const Bytes app5 = jseg(0xE5, str("vendor data"));

    Bytes want = soi;
    put(want, jfif);
    put(want, icc);
    put(want, adobe);
    put(want, dqt);
    put(want, sof);
    put(want, dht);
    put(want, sos);
    put(want, scan);
    put(want, eoi);

    Bytes in = soi;
    put(in, jfif);
    put(in, exif);
    put(in, xmp);
    put(in, icc);
    put(in, ps);
    put(in, com);
    put(in, mpf);
    put(in, app5);
    put(in, adobe);
    put(in, dqt);
    put(in, sof);
    put(in, dht);
    put(in, sos);
    put(in, scan);
    put(in, eoi);
    put(in, str("GPS trailer after EOI"));

    const Bytes got = strip(in);
    CHECK(got == want);
    CHECK(!contains(got, "GPS"));
    CHECK(!contains(got, "Exif"));
    CHECK(!contains(got, "Canon"));
    CHECK(!contains(got, "Photoshop"));
    CHECK(!contains(got, "IPTC"));
    CHECK(strip(got) == got);
    CHECK(strip(in, kImageJpeg) == want);
}

TEST_CASE("JPEG: a progressive file with several scans keeps them all", "[image]") {
    Bytes in = jmarker(0xD8);
    put(in, jseg(0xE1, cat(cstr("Exif"), str("GPS"))));
    put(in, jseg(0xC2, Bytes(15, 2)));
    put(in, jseg(0xDA, Bytes(8, 1)));
    put(in, Bytes(20, 0x55));
    put(in, jseg(0xC4, Bytes(12, 4)));
    put(in, jseg(0xDA, Bytes(8, 2)));
    put(in, Bytes(10, 0x66));
    put(in, jmarker(0xD9));
    Bytes want = jmarker(0xD8);
    put(want, jseg(0xC2, Bytes(15, 2)));
    put(want, jseg(0xDA, Bytes(8, 1)));
    put(want, Bytes(20, 0x55));
    put(want, jseg(0xC4, Bytes(12, 4)));
    put(want, jseg(0xDA, Bytes(8, 2)));
    put(want, Bytes(10, 0x66));
    put(want, jmarker(0xD9));
    CHECK(strip(in) == want);
}

TEST_CASE("JPEG: structure errors are rejected", "[image]") {
    Bytes ok = jmarker(0xD8);
    put(ok, jseg(0xDB, Bytes(65, 3)));
    put(ok, jseg(0xDA, Bytes(8, 1)));
    put(ok, Bytes(16, 0x33));
    put(ok, jmarker(0xD9));
    REQUIRE(strip(ok) == ok);

    Bytes noeoi(ok.begin(), ok.end() - 2);
    CHECK(code_of([&] { strip(noeoi); }) == LOOMORA_ERR_INVALID_ITEM);

    Bytes badlen = jmarker(0xD8);
    put(badlen, jseg(0xE1, Bytes(10, 1)));
    badlen[5] = 0xFF; // segment length runs past the end
    put(badlen, jmarker(0xD9));
    CHECK(code_of([&] { strip(badlen); }) == LOOMORA_ERR_INVALID_ITEM);

    Bytes shortlen = jmarker(0xD8);
    put(shortlen, jseg(0xE1, Bytes(10, 1)));
    shortlen[4] = 0;
    shortlen[5] = 1; // length below 2
    put(shortlen, jmarker(0xD9));
    CHECK(code_of([&] { strip(shortlen); }) == LOOMORA_ERR_INVALID_ITEM);

    Bytes nomarker = jmarker(0xD8);
    put(nomarker, Bytes(6, 0x12)); // data where a marker must be
    CHECK(code_of([&] { strip(nomarker); }) == LOOMORA_ERR_INVALID_ITEM);

    for (std::size_t n = 0; n < ok.size(); ++n) {
        Bytes cut(ok.begin(), ok.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK(code_of([&] { strip(cut); }) == LOOMORA_ERR_INVALID_ITEM);
    }
}

TEST_CASE("WebP: EXIF and XMP chunks are dropped, flags and RIFF size follow", "[image]") {
    const Bytes vp8l = riff_chunk("VP8L", Bytes(11, 0x2F)); // odd size: one padding byte
    const Bytes alph = riff_chunk("ALPH", Bytes(6, 1));
    const Bytes exif = riff_chunk("EXIF", cat(str("Exif  II* GPSLatitude Canon"), Bytes(3, 0)));
    const Bytes xmp = riff_chunk("XMP ", str("<x:xmpmeta> GPS </x:xmpmeta>"));
    const Bytes junk = riff_chunk("abcd", Bytes(4, 4));

    // flags: alpha 0x10 | EXIF 0x08 | XMP 0x04 | ICC 0x20
    Bytes in_chunks = vp8x(0x3C);
    put(in_chunks, exif);
    put(in_chunks, alph);
    put(in_chunks, vp8l);
    put(in_chunks, xmp);
    put(in_chunks, junk);
    Bytes in = riff(in_chunks);
    put(in, str("GPS trailing bytes")); // outside the RIFF size

    Bytes want_chunks = vp8x(0x30);
    put(want_chunks, alph);
    put(want_chunks, vp8l);
    const Bytes want = riff(want_chunks);

    const Bytes got = strip(in);
    CHECK(got == want);
    CHECK(!contains(got, "GPS"));
    CHECK(!contains(got, "Canon"));
    CHECK(!contains(got, "EXIF"));
    CHECK(!contains(got, "XMP "));
    CHECK(strip(got) == got);
    CHECK(strip(in, kImageWebp) == want);

    // simple lossy/lossless files without VP8X are copied as they are
    const Bytes plain = riff(vp8l);
    CHECK(strip(plain) == plain);
}

TEST_CASE("WebP: structure errors are rejected", "[image]") {
    const Bytes ok = riff(riff_chunk("VP8L", Bytes(11, 0x2F)));
    REQUIRE(strip(ok) == ok);

    Bytes big = ok;
    big[4] = 0xFF; // RIFF size beyond the data
    CHECK(code_of([&] { strip(big); }) == LOOMORA_ERR_INVALID_ITEM);

    Bytes chunk_past = riff(riff_chunk("VP8L", Bytes(11, 1)));
    chunk_past[16] = 0xFF; // chunk size beyond the RIFF
    CHECK(code_of([&] { strip(chunk_past); }) == LOOMORA_ERR_INVALID_ITEM);

    Bytes odd(ok.begin(), ok.begin() + 15); // header cut inside a chunk header
    odd[4] = 7;
    CHECK(code_of([&] { strip(odd); }) == LOOMORA_ERR_INVALID_ITEM);

    Bytes nowebp = ok;
    nowebp[8] = 'X';
    CHECK(code_of([&] { strip(nowebp); }) == LOOMORA_ERR_INVALID_ITEM);

    for (std::size_t n = 0; n < ok.size(); ++n) {
        Bytes cut(ok.begin(), ok.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK(code_of([&] { strip(cut); }) == LOOMORA_ERR_INVALID_ITEM);
    }
}

TEST_CASE("thumbnail limits: 256 px and 64 KiB", "[image]") {
    Bytes ok = cat(png_sig(), png_ihdr(256, 256));
    put(ok, idat(100, 1));
    put(ok, png_chunk("IEND", Bytes()));
    CHECK(thumbnail_within_limits(&ok[0], ok.size()));
    require_thumbnail(&ok[0], ok.size());

    Bytes wide = cat(png_sig(), png_ihdr(257, 100));
    put(wide, idat(100, 1));
    put(wide, png_chunk("IEND", Bytes()));
    CHECK(!thumbnail_within_limits(&wide[0], wide.size()));
    CHECK(code_of([&] { require_thumbnail(&wide[0], wide.size()); }) == LOOMORA_ERR_INVALID_ITEM);

    Bytes tall = cat(png_sig(), png_ihdr(100, 257));
    CHECK(!thumbnail_within_limits(&tall[0], tall.size()));
    CHECK(code_of([&] { require_thumbnail(&tall[0], tall.size()); }) == LOOMORA_ERR_INVALID_ITEM);

    // exactly 64 KiB passes, one byte more does not
    Bytes edge = cat(png_sig(), png_ihdr(64, 64));
    const Bytes end = png_chunk("IEND", Bytes());
    // chunk overhead is 12 bytes, so the IDAT payload fills the rest
    const std::size_t room = 64 * 1024 - edge.size() - end.size() - 12;
    put(edge, idat(room, 1));
    put(edge, end);
    REQUIRE(edge.size() == 64 * 1024);
    CHECK(thumbnail_within_limits(&edge[0], edge.size()));
    Bytes over = cat(png_sig(), png_ihdr(64, 64));
    put(over, idat(room + 1, 1));
    put(over, end);
    CHECK(over.size() == 64 * 1024 + 1);
    CHECK(!thumbnail_within_limits(&over[0], over.size()));
    CHECK(code_of([&] { require_thumbnail(&over[0], over.size()); }) == LOOMORA_ERR_INVALID_ITEM);

    // not a PNG
    const Bytes jpg = jmarker(0xD8);
    CHECK(!thumbnail_within_limits(&jpg[0], jpg.size()));
    CHECK(code_of([&] { require_thumbnail(&jpg[0], jpg.size()); }) == LOOMORA_ERR_INVALID_ITEM);

    // the limits come from limits()
    ScopedLimits scoped;
    limits().thumb_max_side = 64;
    CHECK(!thumbnail_within_limits(&ok[0], ok.size()));
    limits().thumb_max_side = 256;
    limits().thumb_max_bytes = 100;
    CHECK(!thumbnail_within_limits(&ok[0], ok.size()));
}
