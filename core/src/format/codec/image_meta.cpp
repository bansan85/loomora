// Container-level image checks and metadata strip (image_meta.hpp).
#include "format/codec/image_meta.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include "zlib.h"
#include "format/codec/codec_error.hpp"
#include "format/endian.hpp"
#include "format/limits.hpp"

namespace loomora {
namespace format {

namespace {

typedef std::vector<std::uint8_t> Bytes;

const unsigned char kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
const std::uint32_t kPngMaxDim = 0x7FFFFFFFu;

// Decimal text of a number (std::to_string and std::snprintf are missing from the gnustl of the Android NDK).
std::string dec(unsigned long long value) {
    char buf[32];
    snprintf(buf, sizeof buf, "%llu", value);
    return std::string(buf);
}

[[noreturn]] void bad(const std::string &what) {
    throw FormatError(LOOMORA_ERR_INVALID_ITEM, what);
}

std::uint32_t read_be32(const unsigned char *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

unsigned long crc_of(const unsigned char *p, std::size_t len) {
    unsigned long crc = crc32(0L, Z_NULL, 0);
    while (len > 0) {
        const std::size_t n = len > 0x40000000u ? 0x40000000u : len;
        crc = crc32(crc, p, static_cast<uInt>(n));
        p += n;
        len -= n;
    }
    return crc;
}

bool tag_is(const unsigned char *p, const char *tag) { return std::memcmp(p, tag, 4) == 0; }

// ---- PNG -----------------------------------------------------------------------------------------

bool png_keeps(const unsigned char *type) {
    if ((type[0] & 0x20) == 0)
        return true; // critical chunk
    static const char *const kAncillary[] = {"tRNS", "gAMA", "cHRM", "sRGB", "iCCP", "sBIT", "cICP",
                                             "mDCV", "cLLI", "pHYs", "bKGD", "hIST", "sPLT", "acTL",
                                             "fcTL", "fdAT"};
    for (std::size_t i = 0; i < sizeof kAncillary / sizeof kAncillary[0]; ++i)
        if (tag_is(type, kAncillary[i]))
            return true;
    return false;
}

Bytes strip_png(const std::uint8_t *data, std::size_t len) {
    if (len < 8 || std::memcmp(data, kPngSignature, 8) != 0)
        bad("not a PNG: bad signature");
    Bytes out;
    out.reserve(len);
    out.insert(out.end(), data, data + 8);
    std::size_t pos = 8;
    bool first = true;
    for (;;) {
        if (len - pos < 12)
            bad("PNG ends before IEND (truncated chunk header)");
        const std::uint32_t clen = read_be32(data + pos);
        const unsigned char *type = data + pos + 4;
        if (clen > kPngMaxDim || static_cast<std::uint64_t>(len - pos - 12) < clen)
            bad("PNG chunk longer than the data");
        const std::size_t total = 12 + static_cast<std::size_t>(clen);
        if (crc_of(type, 4 + static_cast<std::size_t>(clen)) != read_be32(data + pos + 8 + clen))
            bad("PNG chunk CRC does not match");
        if (first) {
            if (!tag_is(type, "IHDR") || clen != 13)
                bad("PNG does not start with a 13-byte IHDR chunk");
            first = false;
        } else if (tag_is(type, "IHDR")) {
            bad("PNG has a second IHDR chunk");
        }
        if (png_keeps(type))
            out.insert(out.end(), data + pos, data + pos + total);
        pos += total;
        if (tag_is(type, "IEND")) {
            if (clen != 0)
                bad("PNG IEND chunk is not empty");
            return out; // anything after IEND is dropped
        }
    }
}

// Returns null when the header is readable, else what is wrong.
const char *parse_png_header(const std::uint8_t *data, std::size_t len, PngHeader *h) {
    if (len < 8 + 25)
        return "PNG is too short for its IHDR chunk";
    if (std::memcmp(data, kPngSignature, 8) != 0)
        return "not a PNG: bad signature";
    if (read_be32(data + 8) != 13 || !tag_is(data + 12, "IHDR"))
        return "PNG does not start with a 13-byte IHDR chunk";
    if (crc_of(data + 12, 4 + 13) != read_be32(data + 8 + 8 + 13))
        return "PNG IHDR CRC does not match";
    const std::uint32_t w = read_be32(data + 16);
    const std::uint32_t ht = read_be32(data + 20);
    if (w == 0 || ht == 0 || w > kPngMaxDim || ht > kPngMaxDim)
        return "PNG width or height is zero or above 2^31 - 1";
    h->width = w;
    h->height = ht;
    h->bit_depth = data[24];
    h->color_type = data[25];
    return nullptr;
}

// ---- JPEG ----------------------------------------------------------------------------------------

bool jpeg_keeps(unsigned marker, const unsigned char *payload, std::size_t plen) {
    if (marker == 0xFE)
        return false; // COM
    if (marker < 0xE0 || marker > 0xEF)
        return true; // not an APPn
    struct Prefix {
        unsigned marker;
        const char *text;
        std::size_t len;
    };
    static const Prefix kKept[] = {{0xE0, "JFIF\0", 5}, {0xE2, "ICC_PROFILE\0", 12}, {0xEE, "Adobe", 5}};
    for (std::size_t i = 0; i < sizeof kKept / sizeof kKept[0]; ++i)
        if (kKept[i].marker == marker && plen >= kKept[i].len &&
            std::memcmp(payload, kKept[i].text, kKept[i].len) == 0)
            return true;
    return false;
}

Bytes strip_jpeg(const std::uint8_t *data, std::size_t len) {
    if (len < 3 || data[0] != 0xFF || data[1] != 0xD8)
        bad("not a JPEG: bad signature");
    Bytes out;
    out.reserve(len);
    out.push_back(0xFF);
    out.push_back(0xD8);
    std::size_t pos = 2;
    for (;;) {
        if (pos >= len)
            bad("JPEG ends before EOI");
        if (data[pos] != 0xFF)
            bad("JPEG has data where a marker is expected");
        while (pos < len && data[pos] == 0xFF)
            ++pos; // fill bytes
        if (pos >= len)
            bad("JPEG ends inside a marker");
        const unsigned marker = data[pos++];
        if (marker == 0x00 || marker == 0xD8)
            bad("JPEG has an unexpected marker");
        if (marker == 0xD9) {
            out.push_back(0xFF);
            out.push_back(0xD9);
            return out; // anything after EOI is dropped
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            out.push_back(0xFF);
            out.push_back(static_cast<std::uint8_t>(marker));
            continue;
        }
        if (len - pos < 2)
            bad("JPEG ends inside a segment length");
        const std::size_t seglen = (static_cast<std::size_t>(data[pos]) << 8) | data[pos + 1];
        if (seglen < 2 || len - pos < seglen)
            bad("JPEG segment length is invalid or runs past the end");
        if (jpeg_keeps(marker, data + pos + 2, seglen - 2)) {
            out.push_back(0xFF);
            out.push_back(static_cast<std::uint8_t>(marker));
            out.insert(out.end(), data + pos, data + pos + seglen);
        }
        pos += seglen;
        if (marker == 0xDA) {
            // entropy-coded data: up to the next marker that is not a stuffed 0xFF00 or RSTn
            std::size_t i = pos;
            for (;;) {
                while (i < len && data[i] != 0xFF)
                    ++i;
                if (i >= len)
                    bad("JPEG ends inside a scan");
                std::size_t j = i + 1;
                while (j < len && data[j] == 0xFF)
                    ++j;
                if (j >= len)
                    bad("JPEG ends inside a scan");
                if (data[j] == 0x00 || (data[j] >= 0xD0 && data[j] <= 0xD7)) {
                    i = j + 1;
                    continue;
                }
                break; // a real marker starts at i (after optional fill bytes)
            }
            out.insert(out.end(), data + pos, data + i);
            pos = i;
        }
    }
}

// ---- WebP ----------------------------------------------------------------------------------------

bool webp_keeps(const unsigned char *fourcc) {
    static const char *const kKept[] = {"VP8 ", "VP8L", "VP8X", "ALPH", "ANIM", "ANMF", "ICCP"};
    for (std::size_t i = 0; i < sizeof kKept / sizeof kKept[0]; ++i)
        if (tag_is(fourcc, kKept[i]))
            return true;
    return false;
}

Bytes strip_webp(const std::uint8_t *data, std::size_t len) {
    if (len < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WEBP", 4) != 0)
        bad("not a WebP: bad signature");
    const std::uint64_t riff_size = read_u32(data + 4);
    if (riff_size < 4 || riff_size + 8 > len)
        bad("WebP RIFF size is invalid or larger than the data");
    const std::size_t end = static_cast<std::size_t>(riff_size) + 8;

    Bytes out(data, data + 12);
    std::size_t pos = 12;
    while (pos < end) {
        if (end - pos < 8)
            bad("WebP chunk header runs past the RIFF size");
        const std::uint32_t csize = read_u32(data + pos + 4);
        if (csize > end - pos - 8)
            bad("WebP chunk is larger than the RIFF");
        const std::size_t payload_end = pos + 8 + static_cast<std::size_t>(csize);
        std::size_t next = payload_end + (csize & 1u);
        if (next > end)
            next = end; // a last odd chunk without its padding byte
        if (webp_keeps(data + pos)) {
            const std::size_t at = out.size();
            out.insert(out.end(), data + pos, data + next);
            if ((csize & 1u) != 0 && next == payload_end)
                out.push_back(0);
            if (tag_is(data + pos, "VP8X")) {
                if (csize < 10)
                    bad("WebP VP8X chunk is shorter than 10 bytes");
                out[at + 8] = static_cast<std::uint8_t>(out[at + 8] & ~0x0Cu); // Exif and XMP flags
            }
        }
        pos = next;
    }
    if (out.size() - 8 > 0xFFFFFFFFull)
        bad("WebP is too large");
    write_u32(&out[4], static_cast<std::uint32_t>(out.size() - 8));
    return out;
}

} // namespace

ImageFormat detect_image_format(const std::uint8_t *data, std::size_t len) {
    if (len >= 8 && std::memcmp(data, kPngSignature, 8) == 0)
        return kImagePng;
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return kImageJpeg;
    if (len >= 12 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBP", 4) == 0)
        return kImageWebp;
    return kImageUnknown;
}

const char *image_format_name(ImageFormat format) {
    switch (format) {
    case kImagePng:
        return "png";
    case kImageJpeg:
        return "jpeg";
    case kImageWebp:
        return "webp";
    case kImageUnknown:
        break;
    }
    return "";
}

std::vector<std::uint8_t> strip_image_metadata(const std::uint8_t *data, std::size_t len,
                                               ImageFormat format, std::size_t *removed_bytes) {
    const ImageFormat found = detect_image_format(data, len);
    if (found == kImageUnknown)
        bad("not a PNG, JPEG or WebP image: unknown signature");
    if (format != kImageUnknown && format != found)
        bad(std::string("the image is not a ") + image_format_name(format) + " file");
    Bytes out;
    switch (found) {
    case kImagePng:
        out = strip_png(data, len);
        break;
    case kImageJpeg:
        out = strip_jpeg(data, len);
        break;
    case kImageWebp:
        out = strip_webp(data, len);
        break;
    case kImageUnknown:
        break;
    }
    if (removed_bytes != nullptr)
        *removed_bytes = len - out.size();
    return out;
}

PngHeader read_png_header(const std::uint8_t *data, std::size_t len) {
    PngHeader h;
    const char *problem = parse_png_header(data, len, &h);
    if (problem != nullptr)
        bad(problem);
    return h;
}

bool thumbnail_within_limits(const std::uint8_t *data, std::size_t len) {
    PngHeader h;
    if (parse_png_header(data, len, &h) != nullptr)
        return false;
    const Limits &l = limits();
    return len <= l.thumb_max_bytes && h.width <= l.thumb_max_side && h.height <= l.thumb_max_side;
}

void require_thumbnail(const std::uint8_t *data, std::size_t len) {
    const Limits &l = limits();
    PngHeader h;
    const char *problem = parse_png_header(data, len, &h);
    if (problem != nullptr)
        bad(std::string("thumbnail: ") + problem);
    if (h.width > l.thumb_max_side || h.height > l.thumb_max_side)
        bad("thumbnail is " + dec(h.width) + " x " + dec(h.height) +
            " pixels, above the maximum of " + dec(l.thumb_max_side));
    if (len > l.thumb_max_bytes)
        bad("thumbnail is " + dec(len) + " bytes, above the maximum of " +
            dec(l.thumb_max_bytes));
}

} // namespace format
} // namespace loomora
