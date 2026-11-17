// Shared by the two fuzz drivers of the .loomora reader (T072): what one input does (exercise), and
// the helpers of the structure-aware mutation (a file made of stored entries is patched so that its ZIP
// CRC-32 and the SHA-256 of its manifest stay right after a mutation of one entry, which lets the
// mutated bytes reach the JSON, nails, sequence, mesh and semantic checks instead of stopping at the
// container pass). Header only; it uses the public C API plus the internal ZIP reader and writer.
#ifndef LOOMORA_FUZZ_COMMON_HPP
#define LOOMORA_FUZZ_COMMON_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../include/loomora.h"
#include "format/io/memory_io.hpp"
#include "format/sha256.hpp"
#include "format/zip/zip_reader.hpp"
#include "format/zip/zip_writer.hpp"

namespace fuzz {

typedef std::vector<std::uint8_t> Bytes;

// Reads every item of an opened project through the public API, bounded (a hostile file can declare
// billions of nails: only the first and the last chunks are read).
inline void read_everything(const loomora_project *p) {
    std::vector<std::uint8_t> buf(1 << 16);
    loomora_project_manifest(p, buf.data(), buf.size());
    loomora_project_is_light(p);
    const int32_t kinds[5] = {LOOMORA_KIND_FRAME, LOOMORA_KIND_LAYOUT, LOOMORA_KIND_IMAGE, LOOMORA_KIND_SPOOL,
                              LOOMORA_KIND_RESULT};
    for (int k = 0; k < 5; ++k) {
        const int64_t n = loomora_project_count(p, kinds[k]);
        if (n <= 0 || n > 5000)
            continue;
        std::vector<std::uint32_t> ids(static_cast<std::size_t>(n));
        if (loomora_project_ids(p, kinds[k], ids.data(), ids.size()) != n)
            continue;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const int64_t len = loomora_project_item_json(p, kinds[k], ids[i], nullptr, 0);
            if (len > 0 && len < (1 << 22)) {
                std::vector<std::uint8_t> json(static_cast<std::size_t>(len));
                loomora_project_item_json(p, kinds[k], ids[i], json.data(), json.size());
            }
            if (kinds[k] == LOOMORA_KIND_LAYOUT) {
                const int64_t nails = loomora_layout_nail_count(p, ids[i]);
                std::vector<loomora_nail> chunk(1024);
                if (nails > 0) {
                    loomora_layout_read_nails(p, ids[i], 0, chunk.size(), chunk.data());
                    loomora_layout_read_nails(p, ids[i], static_cast<std::uint64_t>(nails) - 1, chunk.size(), chunk.data());
                    loomora_layout_nail_label(p, ids[i], chunk[0].id, buf.data(), buf.size());
                }
            } else if (kinds[k] == LOOMORA_KIND_RESULT) {
                const int64_t steps = loomora_result_step_count(p, ids[i]);
                std::vector<loomora_step> chunk(1024);
                for (std::uint64_t at = 0; steps > 0 && at < static_cast<std::uint64_t>(steps) && at < 8192; at += 1024)
                    loomora_result_read_steps(p, ids[i], at, chunk.size(), chunk.data());
                char name[64];
                std::snprintf(name, sizeof name, "results/%u/thumb.png", static_cast<unsigned>(ids[i]));
                loomora_project_read_entry(p, name, 0, buf.data(), buf.size());
            } else if (kinds[k] == LOOMORA_KIND_FRAME) {
                loomora_mesh_info info;
                if (loomora_frame_mesh_info(p, ids[i], &info) == 0) {
                    std::vector<float> f(3 * 256);
                    std::vector<std::uint32_t> t(3 * 256);
                    loomora_frame_read_vertices(p, ids[i], 0, 256, f.data());
                    loomora_frame_read_triangles(p, ids[i], 0, 256, t.data());
                    loomora_frame_read_normals(p, ids[i], 0, 256, f.data());
                    loomora_frame_read_uvs(p, ids[i], 0, 256, f.data());
                    if (info.vertices > 256)
                        loomora_frame_read_vertices(p, ids[i], info.vertices - 100, 100, f.data());
                }
            } else if (kinds[k] == LOOMORA_KIND_IMAGE) {
                char name[64];
                std::snprintf(name, sizeof name, "images/%u/thumb.png", static_cast<unsigned>(ids[i]));
                loomora_project_entry_size(p, name);
                loomora_project_read_entry(p, name, 0, buf.data(), buf.size());
            }
        }
    }
}

inline void read_report(const loomora_report *r) {
    if (r == nullptr)
        return;
    const int64_t n = loomora_report_count(r);
    std::vector<std::uint8_t> buf(1024);
    for (int64_t i = 0; i < n && i < 2000; ++i) {
        int32_t sev, kind;
        std::uint32_t id;
        loomora_report_issue(r, static_cast<std::size_t>(i), &sev, &kind, &id);
        for (int field = 1; field <= 3; ++field)
            loomora_report_text(r, static_cast<std::size_t>(i), field, buf.data(), buf.size());
    }
    loomora_report_error_count(r);
}

// What one input does: validate it, open it, read the report and every item.
inline void exercise(const std::uint8_t *data, std::size_t size) {
    loomora_report *vr = nullptr;
    loomora_validate_memory(data, size, nullptr, nullptr, &vr);
    read_report(vr);
    loomora_report_free(vr);
    loomora_project *p = nullptr;
    loomora_report *r = nullptr;
    const int32_t rc = loomora_project_open_memory(data, size, nullptr, nullptr, &p, &r);
    read_report(r);
    if (rc == 0 && p != nullptr)
        read_everything(p);
    loomora_project_close(p);
    loomora_report_free(r);
}

// ---- structure-aware helpers ------------------------------------------------------------------------

inline std::uint32_t crc32_of(const std::uint8_t *d, std::size_t n) {
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i)
        crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline std::uint32_t rd32(const Bytes &b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
inline std::uint32_t rd16(const Bytes &b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8);
}
inline void wr32(Bytes &b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}

// A stored entry of a classic (not ZIP64) archive: where its bytes are and where its CRC-32 is written.
struct StoredEntry {
    std::string name;
    std::size_t data, size;      // the bytes
    std::size_t crc_local, crc_central;
};

// The stored entries of `a` (empty when the archive is not a classic ZIP that this can walk).
inline std::vector<StoredEntry> stored_entries(const Bytes &a) {
    std::vector<StoredEntry> out;
    if (a.size() < 22)
        return out;
    std::size_t eocd = a.size() - 22;
    for (std::size_t back = 0; back < 70000 && back <= a.size() - 22; ++back) {
        eocd = a.size() - 22 - back;
        if (rd32(a, eocd) == 0x06054b50u)
            break;
        if (back == a.size() - 22 || back == 69999)
            return out;
    }
    if (rd32(a, eocd) != 0x06054b50u)
        return out;
    const std::size_t count = rd16(a, eocd + 10);
    std::size_t at = rd32(a, eocd + 16);
    for (std::size_t i = 0; i < count; ++i) {
        if (at + 46 > a.size() || rd32(a, at) != 0x02014b50u)
            break;
        const std::size_t nlen = rd16(a, at + 28), elen = rd16(a, at + 30), clen = rd16(a, at + 32);
        const std::size_t local = rd32(a, at + 42), csize = rd32(a, at + 20), usize = rd32(a, at + 24);
        if (at + 46 + nlen + elen + clen > a.size())
            break;
        if (rd16(a, at + 10) == 0 && csize == usize && local + 30 <= a.size() && rd32(a, local) == 0x04034b50u) {
            StoredEntry e;
            e.name.assign(reinterpret_cast<const char *>(&a[at + 46]), nlen);
            const std::size_t data = local + 30 + rd16(a, local + 26) + rd16(a, local + 28);
            if (data <= a.size() && usize <= a.size() - data) {
                e.data = data;
                e.size = usize;
                e.crc_local = local + 14;
                e.crc_central = at + 16;
                out.push_back(e);
            }
        }
        at += 46 + nlen + elen + clen;
    }
    return out;
}

// Makes the ZIP CRC-32 of every stored entry right again, and the "sha256" of each stored entry in a
// stored manifest.json (same length, written in place). Entry sizes never change.
inline void fix_up(Bytes &a) {
    const std::vector<StoredEntry> es = stored_entries(a);
    std::size_t manifest = es.size();
    std::vector<std::string> digests(es.size());
    for (std::size_t i = 0; i < es.size(); ++i) {
        if (es[i].name == "manifest.json")
            manifest = i;
        loomora::format::Sha256 h;
        h.update(es[i].size == 0 ? static_cast<const void *>(&a) : static_cast<const void *>(&a[es[i].data]), es[i].size);
        digests[i] = h.hex_digest();
    }
    if (manifest < es.size()) {
        const std::string text(reinterpret_cast<const char *>(&a[es[manifest].data]), es[manifest].size);
        for (std::size_t i = 0; i < es.size(); ++i) {
            if (i == manifest)
                continue;
            const std::size_t key = text.find("\"" + es[i].name + "\"");
            if (key == std::string::npos)
                continue;
            const std::size_t sha = text.find("\"sha256\":\"", key);
            if (sha == std::string::npos || sha + 10 + 64 > text.size())
                continue;
            std::memcpy(&a[es[manifest].data + sha + 10], digests[i].data(), 64);
        }
    }
    for (std::size_t i = 0; i < es.size(); ++i) {
        const std::uint32_t crc = crc32_of(es[i].size == 0 ? &a[0] : &a[es[i].data], es[i].size);
        wr32(a, es[i].crc_local, crc);
        wr32(a, es[i].crc_central, crc);
    }
}

// The same archive with every entry stored (copy through the internal ZIP reader and writer), or an
// empty vector when `a` is not an archive the reader accepts. Gives the mutator structured seeds.
inline Bytes to_stored(const Bytes &a) {
    try {
        loomora::format::MemorySource src(a.data(), a.size());
        loomora::format::ZipReader zr(src);
        zr.open(nullptr);
        loomora::format::MemorySink sink;
        loomora::format::ZipWriter zw(sink);
        zw.add_mimetype();
        for (std::size_t i = 0; i < zr.entries().size(); ++i) {
            const loomora::format::ZipEntry &e = zr.entries()[i];
            if (e.name == "mimetype" || (!e.name.empty() && e.name[e.name.size() - 1] == '/'))
                continue;
            const Bytes content = zr.read_all(e, 64u << 20);
            zw.add_stored(e.name, content.empty() ? static_cast<const void *>(&a) : static_cast<const void *>(&content[0]),
                          content.size());
        }
        zw.finish();
        sink.commit();
        return sink.take();
    } catch (...) {
        return Bytes();
    }
}

} // namespace fuzz

#endif
