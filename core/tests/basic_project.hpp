// The reference projects of spec 002 built through the PUBLIC C API only (loomora.h): `basic` (one 2D
// frame, one layout of 200 nails, one image with a PNG, 3 spools, one result of 5,000 steps), `empty`
// (no item) and `escape` (a result selecting 40 spools, steps using the indexes 31 to 39). Header
// only, plain C++11, no framework: used by core/tests/refgen.cpp and by the Catch2 tests.
//
// Everything is deterministic and platform independent: the project identifier, the times, the
// coordinates (rounded to 0.001 mm), the pictures (PNG with stored deflate blocks, written here) and
// the steps are fixed, so the archive has the same bytes on every host.
#ifndef LOOMORA_TESTS_BASIC_PROJECT_HPP
#define LOOMORA_TESTS_BASIC_PROJECT_HPP

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../include/loomora.h"

namespace basic_project {

typedef std::vector<std::uint8_t> Bytes;

static const char kCreatedAt[] = "2026-11-17T09:00:00Z";
static const char kResultCreatedAt[] = "2026-11-17T09:30:00Z";
static const char kModifiedAt[] = "2026-11-17T10:00:00Z";
static const std::uint32_t kNailCount = 200;
static const std::uint32_t kStepCount = 5000;

inline std::uint32_t crc32_of(const std::uint8_t *d, std::size_t n, std::uint32_t crc = 0) {
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline void put_be32(Bytes &b, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        b.push_back(static_cast<std::uint8_t>(v >> s));
}

inline void png_chunk(Bytes &png, const char *type, const Bytes &data) {
    put_be32(png, static_cast<std::uint32_t>(data.size()));
    Bytes body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    put_be32(png, crc32_of(body.data(), body.size()));
}

// A valid 8-bit grayscale PNG of `w` x `h` pixels (zlib stream of stored blocks), `gray` + a gradient.
inline Bytes make_png(std::uint32_t w, std::uint32_t h, std::uint8_t gray) {
    Bytes raw;
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0); // filter none
        for (std::uint32_t x = 0; x < w; ++x)
            raw.push_back(static_cast<std::uint8_t>(gray + x + y));
    }
    Bytes z;
    z.push_back(0x78);
    z.push_back(0x01);
    std::size_t pos = 0;
    do {
        const std::size_t n = raw.size() - pos > 65535 ? 65535 : raw.size() - pos;
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n & 0xFF));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
        z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < raw.size());
    std::uint32_t a = 1, b = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    put_be32(z, (b << 16) | a);

    Bytes png;
    static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), sig, sig + 8);
    Bytes ihdr;
    put_be32(ihdr, w);
    put_be32(ihdr, h);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(0); // grayscale
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    png_chunk(png, "IHDR", ihdr);
    png_chunk(png, "IDAT", z);
    png_chunk(png, "IEND", Bytes());
    return png;
}

inline double round3(double v) { return std::floor(v * 1000.0 + 0.5) / 1000.0; }

inline std::string num(unsigned long v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%lu", v);
    return buf;
}

inline const std::uint8_t *project_random() {
    static const std::uint8_t r[16] = {0x9b, 0x1d, 0xeb, 0x4d, 0x3b, 0x7d, 0x4b, 0xad,
                                       0x9b, 0xdd, 0x2b, 0x0d, 0x7b, 0x3d, 0xcb, 0x6d};
    return r;
}

// Creates an empty project (the `empty` reference project) with the fixed identifier and times.
inline int32_t create(loomora_editor **e, const char *title = "Living room") {
    return loomora_editor_create(project_random(), title, kCreatedAt, e);
}

inline int32_t add_spools(loomora_editor *e, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        const std::string json = "{\"name\":\"Spool " + num(i + 1) + "\",\"color\":\"#" +
                                 std::string(i % 3 == 0 ? "000000" : i % 3 == 1 ? "ff0000" : "0000ff") +
                                 "\",\"opacity\":1,\"diameter_mm\":0.3,\"available_length_mm\":200000," +
                                 "\"material\":\"cotton\",\"domain\":\"exterior\"}";
        uint32_t id = 0;
        const int32_t rc = loomora_editor_add_spool(e, json.c_str(), &id);
        if (rc != 0)
            return rc;
    }
    return 0;
}

// One 2D frame (a 500 x 500 mm square) and one layout of 200 boundary nails on a circle of 200 mm.
inline int32_t add_frame_and_layout(loomora_editor *e, uint32_t *frame_id, uint32_t *layout_id) {
    const char *frame =
        "{\"name\":\"Square 50\",\"kind\":\"2d\",\"dimensions\":{\"width_mm\":500,\"height_mm\":500},"
        "\"background\":\"#ffffff\",\"outline\":{\"type\":\"rectangle\",\"params\":{\"width_mm\":500,"
        "\"height_mm\":500},\"polyline\":[[-250,-250],[250,-250],[250,250],[-250,250]]},"
        "\"exclusions\":[],\"surface\":\"plane\"}";
    int32_t rc = loomora_editor_add_frame(e, frame, frame_id);
    if (rc != 0)
        return rc;
    std::vector<loomora_nail> nails(kNailCount);
    for (std::uint32_t i = 0; i < kNailCount; ++i) {
        const double a = 6.283185307179586 * i / kNailCount;
        loomora_nail n = loomora_nail();
        n.id = i;
        n.kind = 1;
        n.x = round3(200.0 * std::cos(a));
        n.y = round3(200.0 * std::sin(a));
        n.diameter = 2;
        nails[i] = n;
    }
    rc = loomora_editor_add_layout(e, *frame_id, "{\"name\":\"Circle 200\"}", &nails[0], nails.size(), layout_id);
    if (rc != 0)
        return rc;
    return loomora_editor_layout_set_label(e, *layout_id, 0, "first");
}

inline int32_t add_image(loomora_editor *e, uint32_t *image_id) {
    const Bytes source = make_png(8, 8, 40);
    const Bytes target = make_png(8, 8, 90);
    const Bytes thumb = make_png(4, 4, 10);
    const char *json = "{\"name\":\"Portrait\",\"color_mode\":\"bw\",\"shape\":\"circle\",\"width_px\":8,"
                       "\"height_px\":8}";
    return loomora_editor_add_image(e, json, &source[0], source.size(), &target[0], target.size(), &thumb[0],
                                    thumb.size(), image_id);
}

// A result on `frame` / `layout` / `image` selecting spools 1..spool_count (identifiers) with `steps`
// steps: every selected spool starts at a nail, the segments go round the layout, every spool ends.
// `first_spool` is the identifier of the first selected spool.
inline int32_t add_result(loomora_editor *e, uint32_t frame, uint32_t layout, uint32_t image, unsigned spool_count,
                          unsigned first_spool, std::uint32_t steps, uint32_t *result_id) {
    std::string spools;
    for (unsigned i = 0; i < spool_count; ++i)
        spools += std::string(i ? "," : "") + "{\"spool\":" + num(first_spool + i) + ",\"length_mm\":" +
                  num(1000 + 10 * i) + "}";
    const std::string json = "{\"name\":\"Portrait on circle\",\"created_at\":\"" + std::string(kResultCreatedAt) +
                             "\",\"frame\":" + num(frame) + ",\"layout\":" + num(layout) +
                             ",\"images\":[{\"image\":" + num(image) + "}],\"spools\":[" + spools + "]}";
    std::vector<loomora_step> list(steps);
    for (std::uint32_t i = 0; i < steps; ++i) {
        loomora_step s = loomora_step();
        const bool first = i < spool_count;
        const bool last = i >= steps - spool_count;
        s.spool_index = first ? i : last ? i - (steps - spool_count) : (i * 5u + 1u) % spool_count;
        s.nail = (i * 7u + s.spool_index * 13u) % kNailCount;
        s.rotation = static_cast<std::uint8_t>(i & 1u);
        s.type = first ? 1 : last ? 2 : 0;
        list[i] = s;
    }
    const Bytes thumb = make_png(4, 4, 200);
    return loomora_editor_add_result(e, json.c_str(), &list[0], list.size(), &thumb[0], thumb.size(),
                                     "{\"algorithm\":\"greedy\",\"seed\":1}", result_id);
}

// The `basic` reference project.
inline int32_t build_basic(loomora_editor **e) {
    int32_t rc = create(e);
    if (rc != 0)
        return rc;
    uint32_t frame = 0, layout = 0, image = 0, result = 0;
    if ((rc = add_spools(*e, 3)) != 0 || (rc = add_frame_and_layout(*e, &frame, &layout)) != 0 ||
        (rc = add_image(*e, &image)) != 0 || (rc = add_result(*e, frame, layout, image, 3, 1, kStepCount, &result)) != 0) {
        loomora_editor_close(*e);
        *e = 0;
    }
    return rc;
}

// A result selecting 40 spools, steps using the indexes 31 to 39 (the escape path of sequence.bin).
inline int32_t build_escape(loomora_editor **e) {
    int32_t rc = create(e, "Forty spools");
    if (rc != 0)
        return rc;
    uint32_t frame = 0, layout = 0, image = 0, result = 0;
    if ((rc = add_spools(*e, 40)) != 0 || (rc = add_frame_and_layout(*e, &frame, &layout)) != 0 ||
        (rc = add_image(*e, &image)) != 0 || (rc = add_result(*e, frame, layout, image, 40, 1, 400, &result)) != 0) {
        loomora_editor_close(*e);
        *e = 0;
    }
    return rc;
}

} // namespace basic_project

#endif
