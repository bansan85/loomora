// Test-side ZIP builder: assembles archives byte by byte, with every field of every header under
// the control of the test, so each rule of the reader has a file that breaks it. Independent of
// the ZipWriter (it shares nothing with it but zlib for the deflate streams). Header-only, C++11.
//
// Typical use:
//   ziptest::Entry e = ziptest::file("a.json", "{}"); e.method = 8;
//   ziptest::Archive a = ziptest::build(ziptest::with_mimetype(e));
//   MemorySource src(a.bytes.data(), a.bytes.size());
// and, to break something, patch `a.bytes` at the offsets of `a.at[i]` (see the field offsets below).
#ifndef LOOMORA_TESTS_FORMAT_ZIP_BUILDER_HPP
#define LOOMORA_TESTS_FORMAT_ZIP_BUILDER_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "zlib.h"

namespace ziptest {

typedef std::vector<std::uint8_t> Bytes;

// Field offsets inside a local header and a central directory header.
enum LocalField { L_SIG = 0, L_NEEDED = 4, L_FLAGS = 6, L_METHOD = 8, L_CRC = 14, L_CSIZE = 18,
                  L_USIZE = 22, L_NAMELEN = 26, L_EXTRALEN = 28, L_NAME = 30 };
enum CentralField { C_SIG = 0, C_MADEBY = 4, C_NEEDED = 6, C_FLAGS = 8, C_METHOD = 10, C_CRC = 16,
                    C_CSIZE = 20, C_USIZE = 24, C_NAMELEN = 28, C_EXTRALEN = 30, C_COMMENTLEN = 32,
                    C_DISK = 34, C_EATTR = 38, C_OFFSET = 42, C_NAME = 46 };
// Field offsets inside the classic end record.
enum EndField { E_DISK = 4, E_CDDISK = 6, E_ENTRIES_DISK = 8, E_ENTRIES = 10, E_CDSIZE = 12,
                E_CDOFFSET = 16, E_COMMENTLEN = 20 };

inline void put16(Bytes &v, unsigned x) {
    v.push_back(static_cast<std::uint8_t>(x));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}
inline void put32(Bytes &v, std::uint32_t x) {
    put16(v, x & 0xFFFFu);
    put16(v, x >> 16);
}
inline void put64(Bytes &v, std::uint64_t x) {
    put32(v, static_cast<std::uint32_t>(x));
    put32(v, static_cast<std::uint32_t>(x >> 32));
}
inline void put_at16(Bytes &v, std::size_t pos, unsigned x) {
    v[pos] = static_cast<std::uint8_t>(x);
    v[pos + 1] = static_cast<std::uint8_t>(x >> 8);
}
inline void put_at32(Bytes &v, std::size_t pos, std::uint32_t x) {
    put_at16(v, pos, x & 0xFFFFu);
    put_at16(v, pos + 2, x >> 16);
}
inline unsigned get16(const Bytes &v, std::size_t pos) {
    return static_cast<unsigned>(v[pos]) | (static_cast<unsigned>(v[pos + 1]) << 8);
}
inline std::uint32_t get32(const Bytes &v, std::size_t pos) {
    return get16(v, pos) | (static_cast<std::uint32_t>(get16(v, pos + 2)) << 16);
}

inline Bytes to_bytes(const std::string &s) { return Bytes(s.begin(), s.end()); }

inline std::uint32_t crc_of(const Bytes &d) {
    return static_cast<std::uint32_t>(
        ::crc32(0L, d.empty() ? Z_NULL : &d[0], static_cast<uInt>(d.size())));
}

// A raw deflate stream (level 6) of `d`.
inline Bytes deflate_raw(const Bytes &d) {
    z_stream z;
    std::memset(&z, 0, sizeof z);
    deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    Bytes out(deflateBound(&z, static_cast<uLong>(d.size())) + 16);
    z.next_in = d.empty() ? Z_NULL : const_cast<Bytef *>(&d[0]);
    z.avail_in = static_cast<uInt>(d.size());
    z.next_out = &out[0];
    z.avail_out = static_cast<uInt>(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

struct Entry {
    std::string name;
    Bytes data;                 // uncompressed content
    int method;                 // 0 stored, 8 deflate (any other value is written as is)
    bool has_payload;           // use `payload` as the compressed bytes instead of computing them
    Bytes payload;
    bool has_crc;               // declare `crc` in both headers instead of the real one
    std::uint32_t crc;
    bool has_declared_csize;    // declare these sizes in both headers instead of the real ones
    std::uint64_t declared_csize;
    bool has_declared_usize;
    std::uint64_t declared_usize;
    unsigned flags;             // general purpose flags of both headers (bit 3 is set by `descriptor`)
    unsigned made_by;           // version made by (central); 0x0314 = Unix
    std::uint32_t ext_attr;     // external attributes (central)
    Bytes local_extra;          // extra field of the local header (after the ZIP64 one, if any)
    Bytes central_extra;
    bool zip64;                 // sizes saturated in both headers, real values in the ZIP64 extra
    bool zip64_offset;          // central offset saturated, real value in the ZIP64 extra
    bool descriptor;            // flag bit 3: zeros in the local header, a descriptor after the data

    Entry() : method(0), has_payload(false), has_crc(false), crc(0), has_declared_csize(false),
              declared_csize(0), has_declared_usize(false), declared_usize(0), flags(0),
              made_by(0x0314), ext_attr(0x81A40000u), zip64(false), zip64_offset(false),
              descriptor(false) {}
};

inline Entry file(const std::string &name, const std::string &content, int method = 0) {
    Entry e;
    e.name = name;
    e.data = to_bytes(content);
    e.method = method;
    return e;
}

inline Entry mimetype_entry() { return file("mimetype", "application/vnd.loomora+zip", 0); }

struct Options {
    std::string comment;        // archive comment
    bool zip64_end;             // write the ZIP64 end record and locator
    bool saturate_end;          // classic end fields set to the ZIP64 markers (with zip64_end)
    unsigned disk_number;       // number of this disk in the end record
    Options() : zip64_end(false), saturate_end(true), disk_number(0) {}
};

struct Placement {
    std::size_t local_off;
    std::size_t data_off;
    std::size_t central_off;
    std::size_t csize;
};

struct Archive {
    Bytes bytes;
    std::vector<Placement> at;
    std::size_t cd_offset;
    std::size_t cd_size;
    std::size_t end_offset;     // classic end record
};

inline Archive build(const std::vector<Entry> &entries, const Options &opt = Options()) {
    Archive a;
    Bytes &b = a.bytes;
    const std::uint32_t kSat = 0xFFFFFFFFu;
    // Local records.
    std::vector<std::uint32_t> crcs;
    std::vector<std::uint64_t> usizes, csizes;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry &e = entries[i];
        Bytes comp;
        if (e.has_payload)
            comp = e.payload;
        else if (e.method == 8)
            comp = deflate_raw(e.data);
        else
            comp = e.data;
        const std::uint32_t crc = e.has_crc ? e.crc : crc_of(e.data);
        const std::uint64_t cs = e.has_declared_csize ? e.declared_csize : comp.size();
        const std::uint64_t us = e.has_declared_usize ? e.declared_usize : e.data.size();
        crcs.push_back(crc);
        csizes.push_back(cs);
        usizes.push_back(us);

        Placement p;
        p.local_off = b.size();
        const unsigned flags = e.flags | (e.descriptor ? 8u : 0u);
        Bytes extra;
        if (e.zip64) {
            put16(extra, 1);
            put16(extra, 16);
            put64(extra, us);
            put64(extra, cs);
        }
        extra.insert(extra.end(), e.local_extra.begin(), e.local_extra.end());
        put32(b, 0x04034b50u);
        put16(b, e.zip64 ? 45 : 20);
        put16(b, flags);
        put16(b, static_cast<unsigned>(e.method));
        put16(b, 0);
        put16(b, 0x21);
        if (e.descriptor) {
            put32(b, 0);
            put32(b, 0);
            put32(b, 0);
        } else {
            put32(b, crc);
            put32(b, e.zip64 ? kSat : static_cast<std::uint32_t>(cs));
            put32(b, e.zip64 ? kSat : static_cast<std::uint32_t>(us));
        }
        put16(b, static_cast<unsigned>(e.name.size()));
        put16(b, static_cast<unsigned>(extra.size()));
        b.insert(b.end(), e.name.begin(), e.name.end());
        b.insert(b.end(), extra.begin(), extra.end());
        p.data_off = b.size();
        p.csize = comp.size();
        b.insert(b.end(), comp.begin(), comp.end());
        if (e.descriptor) {
            put32(b, 0x08074b50u);
            put32(b, crc);
            if (e.zip64) {
                put64(b, cs);
                put64(b, us);
            } else {
                put32(b, static_cast<std::uint32_t>(cs));
                put32(b, static_cast<std::uint32_t>(us));
            }
        }
        p.central_off = 0;
        a.at.push_back(p);
    }
    // Central directory.
    a.cd_offset = b.size();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry &e = entries[i];
        a.at[i].central_off = b.size();
        Bytes extra;
        if (e.zip64 || e.zip64_offset) {
            put16(extra, 1);
            put16(extra, static_cast<unsigned>((e.zip64 ? 16 : 0) + (e.zip64_offset ? 8 : 0)));
            if (e.zip64) {
                put64(extra, usizes[i]);
                put64(extra, csizes[i]);
            }
            if (e.zip64_offset)
                put64(extra, a.at[i].local_off);
        }
        extra.insert(extra.end(), e.central_extra.begin(), e.central_extra.end());
        put32(b, 0x02014b50u);
        put16(b, e.made_by);
        put16(b, (e.zip64 || e.zip64_offset) ? 45 : 20);
        put16(b, e.flags | (e.descriptor ? 8u : 0u));
        put16(b, static_cast<unsigned>(e.method));
        put16(b, 0);
        put16(b, 0x21);
        put32(b, crcs[i]);
        put32(b, e.zip64 ? kSat : static_cast<std::uint32_t>(csizes[i]));
        put32(b, e.zip64 ? kSat : static_cast<std::uint32_t>(usizes[i]));
        put16(b, static_cast<unsigned>(e.name.size()));
        put16(b, static_cast<unsigned>(extra.size()));
        put16(b, 0);
        put16(b, 0);
        put16(b, 0);
        put32(b, e.ext_attr);
        put32(b, e.zip64_offset ? kSat : static_cast<std::uint32_t>(a.at[i].local_off));
        b.insert(b.end(), e.name.begin(), e.name.end());
        b.insert(b.end(), extra.begin(), extra.end());
    }
    a.cd_size = b.size() - a.cd_offset;
    // End records.
    const std::size_t n = entries.size();
    if (opt.zip64_end) {
        const std::size_t z64_off = b.size();
        put32(b, 0x06064b50u);
        put64(b, 44);
        put16(b, 0x0314);
        put16(b, 45);
        put32(b, 0);
        put32(b, 0);
        put64(b, n);
        put64(b, n);
        put64(b, a.cd_size);
        put64(b, a.cd_offset);
        put32(b, 0x07064b50u);
        put32(b, 0);
        put64(b, z64_off);
        put32(b, 1);
    }
    a.end_offset = b.size();
    const bool sat = opt.zip64_end && opt.saturate_end;
    put32(b, 0x06054b50u);
    put16(b, opt.disk_number);
    put16(b, 0);
    put16(b, sat ? 0xFFFFu : static_cast<unsigned>(n));
    put16(b, sat ? 0xFFFFu : static_cast<unsigned>(n));
    put32(b, sat ? kSat : static_cast<std::uint32_t>(a.cd_size));
    put32(b, sat ? kSat : static_cast<std::uint32_t>(a.cd_offset));
    put16(b, static_cast<unsigned>(opt.comment.size()));
    b.insert(b.end(), opt.comment.begin(), opt.comment.end());
    return a;
}

// `rest` preceded by the mimetype entry.
inline std::vector<Entry> with_mimetype(const std::vector<Entry> &rest) {
    std::vector<Entry> v;
    v.push_back(mimetype_entry());
    v.insert(v.end(), rest.begin(), rest.end());
    return v;
}
inline std::vector<Entry> with_mimetype(const Entry &one) {
    return with_mimetype(std::vector<Entry>(1, one));
}

} // namespace ziptest

#endif
