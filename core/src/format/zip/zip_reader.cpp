#include "format/zip/zip_reader.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

#include "zlib.h"

#include "errors.hpp"
#include "format/endian.hpp"
#include "format/limits.hpp"
#include "format/testing.hpp"
#include "loomora.h"
#include "utf8.hpp"

namespace loomora {
namespace format {

const char kMimetypeName[] = "mimetype";
const char kMimetypeContent[] = "application/vnd.loomora+zip";
const std::size_t kMimetypeSize = sizeof(kMimetypeContent) - 1;

namespace {

const std::uint32_t kLocalSig = 0x04034b50u;
const std::uint32_t kCentralSig = 0x02014b50u;
const std::uint32_t kEndSig = 0x06054b50u;
const std::uint32_t kZip64EndSig = 0x06064b50u;
const std::uint32_t kZip64LocatorSig = 0x07064b50u;

const std::size_t kLocalHeaderSize = 30;
const std::size_t kCentralHeaderSize = 46;
const std::size_t kEndRecordSize = 22;
const std::size_t kZip64LocatorSize = 20;
const std::size_t kZip64EndSize = 56;
const std::size_t kStreamBuffer = 32 * 1024; // compressed bytes read at a time

const std::uint32_t kSat32 = 0xFFFFFFFFu;
const unsigned kSat16 = 0xFFFFu;

const unsigned kFlagEncrypted = 0x0001;
const unsigned kFlagDescriptor = 0x0008;
const unsigned kFlagStrong = 0x0040;
const unsigned kFlagMasked = 0x2000;

[[noreturn]] void fail(int code, const char *issue_code, const std::string &entry,
                       const std::string &message) {
    Issue issue;
    issue.severity = kError;
    issue.code = issue_code;
    issue.entry = entry;
    issue.message = message;
    throw FormatError(code, issue);
}

[[noreturn]] void corrupt(const char *issue_code, const std::string &entry, const std::string &message) {
    fail(LOOMORA_ERR_CORRUPT, issue_code, entry, message);
}

// Every allocation of this file goes through here: the test hook can refuse it, and a real
// failure is a std::bad_alloc, which code_for_exception maps to LOOMORA_ERR_NO_MEMORY as well.
void check_alloc(std::size_t bytes) {
    if (should_fail_alloc(bytes))
        throw FormatError(LOOMORA_ERR_NO_MEMORY, "memory allocation failed");
}

bool exact(const Source &s, std::uint64_t offset, void *buf, std::size_t len) {
    return read_exact(s, offset, buf, len);
}

std::uint64_t mul_saturated(std::uint64_t a, std::uint64_t b) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
        return std::numeric_limits<std::uint64_t>::max();
    return a * b;
}

bool bomb(std::uint64_t size, std::uint64_t compressed) {
    const Limits &l = limits();
    return size > l.bomb_min_bytes && size > mul_saturated(compressed, l.bomb_ratio);
}

bool is_unix_host(unsigned made_by) {
    const unsigned host = made_by >> 8;
    return host == 3 || host == 19; // Unix, Mac OS X
}

// The ZIP64 extended information of an extra field: its data, or none.
struct Zip64Extra {
    const unsigned char *data;
    std::size_t len;
    Zip64Extra() : data(nullptr), len(0) {}
};

bool find_zip64(const unsigned char *extra, std::size_t len, Zip64Extra *out) {
    std::size_t p = 0;
    while (p + 4 <= len) {
        const unsigned id = read_u16(extra + p);
        const std::size_t sz = read_u16(extra + p + 2);
        if (p + 4 + sz > len)
            return false; // malformed
        if (id == 1 && out->data == nullptr) {
            out->data = extra + p + 4;
            out->len = sz;
        }
        p += 4 + sz;
    }
    return p == len;
}

} // namespace

bool is_valid_entry_name(const std::string &name, std::string *why) {
    const char *reason = nullptr;
    if (name.empty()) {
        reason = "empty name";
    } else if (name.size() > limits().entry_name_max_bytes) {
        reason = "name longer than the limit";
    } else if (name[0] == '/') {
        reason = "absolute path";
    } else if (name.find('\\') != std::string::npos) {
        reason = "backslash in the name";
    } else {
        try {
            const std::u32string cps = decode_utf8(name);
            for (std::size_t i = 0; i < cps.size() && reason == nullptr; ++i) {
                const char32_t c = cps[i];
                if (c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F))
                    reason = "control character in the name";
            }
        } catch (const InvalidUtf8 &) {
            reason = "name is not valid UTF-8";
        }
    }
    if (reason == nullptr) {
        std::string n = name;
        if (n[n.size() - 1] == '/')
            n.erase(n.size() - 1); // a directory entry
        std::size_t start = 0;
        bool first = true;
        for (;;) {
            const std::size_t slash = n.find('/', start);
            const std::string comp = n.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (comp.empty()) {
                reason = "empty path component";
            } else if (comp == "." || comp == "..") {
                reason = "'.' or '..' path component";
            } else if (first && comp.find(':') != std::string::npos) {
                reason = "':' in the first path component";
            }
            if (reason != nullptr || slash == std::string::npos)
                break;
            start = slash + 1;
            first = false;
        }
    }
    if (reason != nullptr && why != nullptr)
        *why = reason;
    return reason == nullptr;
}

std::string fold_entry_name(const std::string &name) {
    std::string out = name;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = static_cast<char>(out[i] + ('a' - 'A'));
    return out;
}

// ---------------------------------------------------------------------------------------------
// Opening

void ZipReader::read_end_record(std::uint64_t *cd_offset, std::uint64_t *cd_size, std::uint64_t *count) {
    const std::uint64_t size = source_.size();
    if (size < kEndRecordSize)
        fail(LOOMORA_ERR_NOT_A_PROJECT, "zip.not_zip", "", "not a ZIP archive: too short");
    const std::size_t tail = static_cast<std::size_t>(std::min<std::uint64_t>(size, kEndRecordSize + 65535u));
    check_alloc(tail);
    std::vector<unsigned char> buf(tail);
    if (!exact(source_, size - tail, &buf[0], tail))
        corrupt("zip.central", "", "the archive could not be read");

    std::size_t found = tail; // index of the end record in buf
    for (std::size_t i = tail - kEndRecordSize + 1; i-- > 0;) {
        if (read_u32(&buf[i]) != kEndSig)
            continue;
        const std::size_t comment = read_u16(&buf[i + 20]);
        if (i + kEndRecordSize + comment == tail) {
            found = i;
            break;
        }
    }
    if (found == tail)
        fail(LOOMORA_ERR_NOT_A_PROJECT, "zip.not_zip", "", "not a ZIP archive: no end of central directory record");
    const unsigned char *e = &buf[found];
    const std::uint64_t end_pos = size - tail + found;

    if (read_u16(e + 20) != 0)
        corrupt("zip.comment", "", "the archive has a comment");
    if (read_u16(e + 4) != 0 || read_u16(e + 6) != 0 || read_u16(e + 8) != read_u16(e + 10))
        corrupt("zip.multidisk", "", "multi-disk archives are not supported");
    const unsigned entries16 = read_u16(e + 10);
    const std::uint32_t size32 = read_u32(e + 12);
    const std::uint32_t offset32 = read_u32(e + 16);
    *count = entries16;
    *cd_size = size32;
    *cd_offset = offset32;
    std::uint64_t cd_limit = end_pos; // the central directory must end before this

    bool locator = false;
    if (end_pos >= kZip64LocatorSize) {
        unsigned char loc[kZip64LocatorSize];
        if (!exact(source_, end_pos - kZip64LocatorSize, loc, sizeof loc))
            corrupt("zip.central", "", "the archive could not be read");
        locator = read_u32(loc) == kZip64LocatorSig;
        if (locator) {
            if (read_u32(loc + 4) != 0 || read_u32(loc + 16) != 1)
                corrupt("zip.multidisk", "", "multi-disk archives are not supported");
            const std::uint64_t z64_off = read_u64(loc + 8);
            if (z64_off > end_pos - kZip64LocatorSize || end_pos - kZip64LocatorSize - z64_off < kZip64EndSize)
                corrupt("zip.zip64", "", "the ZIP64 end record lies outside the archive");
            unsigned char z[kZip64EndSize];
            if (!exact(source_, z64_off, z, sizeof z))
                corrupt("zip.central", "", "the archive could not be read");
            if (read_u32(z) != kZip64EndSig || read_u64(z + 4) < 44)
                corrupt("zip.zip64", "", "malformed ZIP64 end record");
            if (read_u32(z + 16) != 0 || read_u32(z + 20) != 0)
                corrupt("zip.multidisk", "", "multi-disk archives are not supported");
            const std::uint64_t z_disk = read_u64(z + 24);
            const std::uint64_t z_count = read_u64(z + 32);
            const std::uint64_t z_size = read_u64(z + 40);
            const std::uint64_t z_offset = read_u64(z + 48);
            if (z_disk != z_count)
                corrupt("zip.multidisk", "", "multi-disk archives are not supported");
            // A classic field that is not saturated must agree with the ZIP64 record.
            if ((entries16 != kSat16 && entries16 != z_count) || (size32 != kSat32 && size32 != z_size) ||
                (offset32 != kSat32 && offset32 != z_offset))
                corrupt("zip.zip64", "", "the ZIP64 end record disagrees with the end record");
            *count = z_count;
            *cd_size = z_size;
            *cd_offset = z_offset;
            cd_limit = z64_off;
        }
    }
    if (!locator && (entries16 == kSat16 || size32 == kSat32 || offset32 == kSat32))
        corrupt("zip.zip64", "", "the end record needs ZIP64 but there is no ZIP64 end record");

    if (*cd_offset > cd_limit || *cd_size > cd_limit - *cd_offset)
        corrupt("zip.central", "", "the central directory lies outside the archive");
    if (*count > *cd_size / kCentralHeaderSize)
        corrupt("zip.central", "", "the entry count does not fit the central directory");
}

void ZipReader::read_central_directory(std::uint64_t cd_offset, std::uint64_t cd_size,
                                       std::uint64_t count, Progress *progress) {
    const std::uint64_t cd_end = cd_offset + cd_size;
    std::uint64_t pos = cd_offset;
    std::set<std::string> seen;
    std::vector<unsigned char> extra;
    bool first_is_file = false;
    for (std::uint64_t i = 0; i < count; ++i) {
        unsigned char h[kCentralHeaderSize];
        if (cd_end - pos < kCentralHeaderSize || !exact(source_, pos, h, sizeof h))
            corrupt("zip.central", "", "the central directory is truncated");
        if (read_u32(h) != kCentralSig)
            corrupt("zip.central", "", "bad central directory header signature");
        const unsigned made_by = read_u16(h + 4);
        const unsigned flags = read_u16(h + 8);
        const unsigned method = read_u16(h + 10);
        const std::uint32_t crc = read_u32(h + 16);
        const std::uint32_t csize32 = read_u32(h + 20);
        const std::uint32_t usize32 = read_u32(h + 24);
        const std::size_t name_len = read_u16(h + 28);
        const std::size_t extra_len = read_u16(h + 30);
        const std::size_t comment_len = read_u16(h + 32);
        const unsigned disk = read_u16(h + 34);
        const std::uint32_t attrs = read_u32(h + 38);
        const std::uint32_t offset32 = read_u32(h + 42);
        const std::uint64_t record = kCentralHeaderSize + name_len + extra_len + comment_len;
        if (record > cd_end - pos)
            corrupt("zip.central", "", "a central directory record runs past the directory");
        if (progress != nullptr)
            progress->advance(record);
        if (comment_len != 0)
            corrupt("zip.comment", "", "an entry has a comment");
        if (disk != 0 && disk != kSat16)
            corrupt("zip.multidisk", "", "multi-disk archives are not supported");
        if (name_len > limits().entry_name_max_bytes)
            corrupt("zip.bad_name", "", "an entry name is longer than the limit");

        check_alloc(name_len + extra_len);
        std::string name(name_len, '\0');
        if (name_len != 0 && !exact(source_, pos + kCentralHeaderSize, &name[0], name_len))
            corrupt("zip.central", "", "the central directory is truncated");
        extra.resize(extra_len);
        if (extra_len != 0 && !exact(source_, pos + kCentralHeaderSize + name_len, &extra[0], extra_len))
            corrupt("zip.central", "", "the central directory is truncated");

        if ((flags & (kFlagEncrypted | kFlagStrong | kFlagMasked)) != 0)
            corrupt("zip.encrypted", name, "encrypted entries are not supported");
        if (method != 0 && method != 8)
            corrupt("zip.method", name, "unsupported compression method");
        std::string why;
        if (!is_valid_entry_name(name, &why))
            corrupt("zip.bad_name", name, "invalid entry name: " + why);
        const std::string folded = fold_entry_name(name);
        if (!seen.insert(folded).second)
            corrupt("zip.duplicate_name", name, "two entries have the same name (ASCII case folded)");
        if (is_unix_host(made_by)) {
            const unsigned type = (attrs >> 16) & 0170000u;
            if (type != 0 && type != 0100000u && type != 0040000u)
                corrupt("zip.symlink", name, "a symbolic link, device, socket or pipe entry");
        }

        ZipEntry e;
        e.name = name;
        e.method = static_cast<std::uint16_t>(method);
        e.crc32 = crc;
        e.compressed_size = csize32;
        e.size = usize32;
        e.local_offset = offset32;
        e.has_descriptor = (flags & kFlagDescriptor) != 0;
        if (usize32 == kSat32 || csize32 == kSat32 || offset32 == kSat32) {
            Zip64Extra z;
            if (!find_zip64(extra.empty() ? nullptr : &extra[0], extra.size(), &z))
                corrupt("zip.central", name, "malformed extra field");
            std::size_t p = 0;
            std::uint64_t *targets[3] = {&e.size, &e.compressed_size, &e.local_offset};
            const bool wanted[3] = {usize32 == kSat32, csize32 == kSat32, offset32 == kSat32};
            for (int k = 0; k < 3; ++k) {
                if (!wanted[k])
                    continue;
                if (z.data == nullptr || z.len - p < 8)
                    corrupt("zip.zip64", name, "the entry needs a ZIP64 extra field that is missing or too short");
                *targets[k] = read_u64(z.data + p);
                p += 8;
            }
        }

        const bool is_dir = name[name.size() - 1] == '/';
        if (i == 0)
            first_is_file = !is_dir;
        if (!is_dir) {
            if (e.method == 0 && e.size != e.compressed_size)
                corrupt("entry.size", name, "a stored entry has different compressed and uncompressed sizes");
            if (e.method == 8 && bomb(e.size, e.compressed_size))
                fail(LOOMORA_ERR_BOMB, "entry.bomb", name,
                     "the decompression ratio is above the limit (decompression bomb)");
        }
        // The local header must fit before the central directory.
        if (e.local_offset > cd_offset || cd_offset - e.local_offset < kLocalHeaderSize)
            corrupt("zip.bounds", name, "the local header lies outside the entries area");
        check_alloc(sizeof(ZipEntry) + name.size());
        (is_dir ? dirs_ : entries_).push_back(e);
        pos += record;
    }
    first_is_file_ = first_is_file;
}

void ZipReader::check_local_headers_and_layout(Progress *progress) {
    struct Extent {
        std::uint64_t start;
        std::uint64_t end;
        const std::string *name;
    };
    std::vector<Extent> extents;
    check_alloc((entries_.size() + dirs_.size()) * sizeof(Extent));
    extents.reserve(entries_.size() + dirs_.size());
    std::vector<unsigned char> extra;
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<ZipEntry> &list = pass == 0 ? entries_ : dirs_;
        for (std::size_t i = 0; i < list.size(); ++i) {
            ZipEntry &e = list[i];
            unsigned char h[kLocalHeaderSize];
            if (!exact(source_, e.local_offset, h, sizeof h))
                corrupt("zip.bounds", e.name, "the local header could not be read");
            if (read_u32(h) != kLocalSig)
                corrupt("zip.local_header", e.name, "bad local header signature");
            const unsigned flags = read_u16(h + 6);
            const unsigned method = read_u16(h + 8);
            const std::uint32_t crc = read_u32(h + 14);
            const std::uint32_t csize32 = read_u32(h + 18);
            const std::uint32_t usize32 = read_u32(h + 22);
            const std::size_t name_len = read_u16(h + 26);
            const std::size_t extra_len = read_u16(h + 28);
            if (progress != nullptr)
                progress->advance(kLocalHeaderSize + name_len + extra_len);
            if ((flags & (kFlagEncrypted | kFlagStrong | kFlagMasked)) != 0)
                corrupt("zip.encrypted", e.name, "encrypted entries are not supported");
            if (name_len != e.name.size() || method != e.method)
                corrupt("zip.local_header", e.name, "the local header disagrees with the central directory");
            e.data_offset = e.local_offset + kLocalHeaderSize + name_len + extra_len;
            if (e.data_offset > cd_offset_ || e.compressed_size > cd_offset_ - e.data_offset)
                corrupt("zip.bounds", e.name, "the entry lies outside the entries area");
            std::string local_name(name_len, '\0');
            if (name_len != 0 &&
                !exact(source_, e.local_offset + kLocalHeaderSize, &local_name[0], name_len))
                corrupt("zip.bounds", e.name, "the local header could not be read");
            if (local_name != e.name)
                corrupt("zip.local_header", e.name, "the local header disagrees with the central directory");
            const bool descriptor = (flags & kFlagDescriptor) != 0 || e.has_descriptor;
            if (!descriptor) {
                std::uint64_t usize = usize32, csize = csize32;
                if (usize32 == kSat32 || csize32 == kSat32) {
                    check_alloc(extra_len);
                    extra.resize(extra_len);
                    if (extra_len != 0 &&
                        !exact(source_, e.local_offset + kLocalHeaderSize + name_len, &extra[0], extra_len))
                        corrupt("zip.bounds", e.name, "the local header could not be read");
                    Zip64Extra z;
                    if (!find_zip64(extra.empty() ? nullptr : &extra[0], extra.size(), &z))
                        corrupt("zip.local_header", e.name, "malformed extra field");
                    std::size_t p = 0;
                    if (usize32 == kSat32) {
                        if (z.data == nullptr || z.len - p < 8)
                            corrupt("zip.local_header", e.name, "the local ZIP64 extra field is missing or too short");
                        usize = read_u64(z.data + p);
                        p += 8;
                    }
                    if (csize32 == kSat32) {
                        if (z.data == nullptr || z.len - p < 8)
                            corrupt("zip.local_header", e.name, "the local ZIP64 extra field is missing or too short");
                        csize = read_u64(z.data + p);
                    }
                }
                if (crc != e.crc32 || usize != e.size || csize != e.compressed_size)
                    corrupt("zip.local_header", e.name, "the local header disagrees with the central directory");
            }
            if (pass == 0 && i == 0)
                first_extra_len_ = extra_len;
            Extent x;
            x.start = e.local_offset;
            x.end = e.data_offset + e.compressed_size;
            x.name = &e.name;
            extents.push_back(x);
        }
    }
    std::sort(extents.begin(), extents.end(),
              [](const Extent &a, const Extent &b) { return a.start < b.start; });
    for (std::size_t i = 1; i < extents.size(); ++i)
        if (extents[i].start < extents[i - 1].end)
            corrupt("zip.overlap", *extents[i].name, "entries overlap");
}

void ZipReader::check_mimetype() {
    const int np = LOOMORA_ERR_NOT_A_PROJECT;
    if (entries_.empty() || !first_is_file_)
        fail(np, "zip.mimetype", kMimetypeName, "the first entry is not 'mimetype'");
    const ZipEntry &m = entries_[0];
    if (m.name != kMimetypeName)
        fail(np, "zip.mimetype", kMimetypeName, "the first entry is not 'mimetype'");
    if (m.local_offset != 0)
        fail(np, "zip.mimetype", kMimetypeName, "'mimetype' is not at offset 0");
    if (m.method != 0)
        fail(np, "zip.mimetype", kMimetypeName, "'mimetype' is not stored");
    if (first_extra_len_ != 0)
        fail(np, "zip.mimetype", kMimetypeName, "'mimetype' has an extra field");
    if (m.size != kMimetypeSize)
        fail(np, "zip.mimetype", kMimetypeName, "'mimetype' has the wrong content");
    char buf[sizeof(kMimetypeContent)];
    if (!exact(source_, m.data_offset, buf, kMimetypeSize) || std::memcmp(buf, kMimetypeContent, kMimetypeSize) != 0)
        fail(np, "zip.mimetype", kMimetypeName, "'mimetype' has the wrong content");
}

void ZipReader::open(Progress *progress) {
    entries_.clear();
    dirs_.clear();
    opened_ = false;
    first_is_file_ = false;
    first_extra_len_ = 0;
    std::uint64_t cd_offset = 0, cd_size = 0, count = 0;
    read_end_record(&cd_offset, &cd_size, &count);
    cd_offset_ = cd_offset;
    read_central_directory(cd_offset, cd_size, count, progress);
    check_local_headers_and_layout(progress);
    check_mimetype();
    opened_ = true;
}

const ZipEntry *ZipReader::find(const std::string &name) const {
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].name == name)
            return &entries_[i];
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Access to the content

std::size_t ZipReader::read_raw(const ZipEntry &entry, std::uint64_t offset, void *buf, std::size_t len) const {
    if (offset >= entry.compressed_size || len == 0)
        return 0;
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(len, entry.compressed_size - offset));
    if (!exact(source_, entry.data_offset + offset, buf, n))
        corrupt("zip.bounds", entry.name, "the entry data could not be read");
    return n;
}

std::size_t ZipReader::read_range(const ZipEntry &entry, std::uint64_t offset, void *buf, std::size_t len) const {
    if (entry.method != 0)
        throw FormatError(LOOMORA_ERR_INTERNAL, "read_range needs a stored entry");
    return read_raw(entry, offset, buf, len);
}

struct ZipEntryStream::Impl {
    const ZipReader &reader;
    ZipEntry entry;
    Progress *progress;
    std::vector<unsigned char> in;
    std::size_t in_pos;
    std::size_t in_len;
    std::uint64_t consumed; // compressed bytes read from the source
    z_stream zs;
    bool zinit;
    bool zend;
    std::uint32_t crc;
    bool finished;

    Impl(const ZipReader &r, const ZipEntry &e, Progress *p)
        : reader(r), entry(e), progress(p), in_pos(0), in_len(0), consumed(0), zinit(false), zend(false),
          crc(0), finished(false) {
        std::memset(&zs, 0, sizeof zs);
        check_alloc(kStreamBuffer);
        in.resize(kStreamBuffer);
        if (entry.method == 8) {
            if (inflateInit2(&zs, -15) != Z_OK)
                throw FormatError(LOOMORA_ERR_NO_MEMORY, "memory allocation failed");
            zinit = true;
        }
    }
    ~Impl() {
        if (zinit)
            inflateEnd(&zs);
    }

    // Refills the input buffer; false when the compressed data is exhausted.
    bool refill() {
        const std::uint64_t left = entry.compressed_size - consumed;
        if (left == 0)
            return false;
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(left, in.size()));
        const std::size_t got = reader.read_raw(entry, consumed, &in[0], want);
        if (got == 0)
            return false;
        consumed += got;
        in_pos = 0;
        in_len = got;
        return true;
    }

    void update_crc(const unsigned char *p, std::size_t n) {
        crc = static_cast<std::uint32_t>(::crc32(crc, p, static_cast<uInt>(n)));
    }

    [[noreturn]] void size_error(const char *what) { corrupt("entry.size", entry.name, what); }

    void inflate_error(int ret) {
        if (ret == Z_MEM_ERROR)
            throw FormatError(LOOMORA_ERR_NO_MEMORY, "memory allocation failed");
        corrupt("zip.inflate", entry.name, "the compressed data is not a valid deflate stream");
    }

    // Inflates into [out, out + len) until it is full or the stream ends; returns the bytes made.
    std::size_t inflate_some(unsigned char *out, std::size_t len) {
        zs.next_out = out;
        zs.avail_out = static_cast<uInt>(len);
        while (zs.avail_out != 0 && !zend) {
            if (zs.avail_in == 0) {
                if (!refill())
                    size_error("the compressed data ends before the end of the deflate stream");
                zs.next_in = &in[0];
                zs.avail_in = static_cast<uInt>(in_len);
            }
            const int ret = inflate(&zs, Z_NO_FLUSH);
            if (ret == Z_STREAM_END)
                zend = true;
            else if (ret != Z_OK && ret != Z_BUF_ERROR)
                inflate_error(ret);
        }
        return len - zs.avail_out;
    }

    void finish() {
        if (entry.method == 8) {
            if (!zend) {
                unsigned char probe;
                if (inflate_some(&probe, 1) != 0)
                    size_error("the entry is longer than its declared size");
            }
            // Everything that inflate did not consume is still in the buffer or in the source.
            if (zs.avail_in != 0 || consumed != entry.compressed_size)
                size_error("the compressed data is longer than the deflate stream");
        }
        if (crc != entry.crc32)
            corrupt("entry.crc", entry.name, "the CRC-32 of the entry does not match");
        finished = true;
    }
};

ZipEntryStream::ZipEntryStream(const ZipReader &reader, const ZipEntry &entry, Progress *progress)
    : impl_(new Impl(reader, entry, progress)), produced_(0) {}

ZipEntryStream::~ZipEntryStream() {}

std::size_t ZipEntryStream::read(void *buf, std::size_t len) {
    Impl &s = *impl_;
    if (s.finished || len == 0)
        return 0;
    const std::uint64_t remaining = s.entry.size - produced_;
    if (remaining == 0) {
        s.finish();
        return 0;
    }
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(len, remaining));
    unsigned char *out = static_cast<unsigned char *>(buf);
    std::size_t n = 0;
    if (s.entry.method == 0) {
        n = s.reader.read_raw(s.entry, s.consumed, out, want);
        if (n == 0)
            s.size_error("the entry data ends before its declared size");
        s.consumed += n;
    } else {
        n = s.inflate_some(out, want);
        if (n == 0)
            s.size_error("the entry is shorter than its declared size");
    }
    s.update_crc(out, n);
    produced_ += n;
    if (s.progress != nullptr)
        s.progress->advance(n);
    if (produced_ == s.entry.size)
        s.finish();
    return n;
}

std::unique_ptr<ZipEntryStream> ZipReader::open_entry(const ZipEntry &entry, Progress *progress) const {
    return std::unique_ptr<ZipEntryStream>(new ZipEntryStream(*this, entry, progress));
}

std::vector<std::uint8_t> ZipReader::read_all(const ZipEntry &entry, std::uint64_t max_bytes,
                                              Progress *progress) const {
    if (entry.size > max_bytes)
        corrupt("entry.size", entry.name, "the entry is larger than the limit");
    std::unique_ptr<ZipEntryStream> s = open_entry(entry, progress);
    std::vector<std::uint8_t> out;
    unsigned char tmp[16 * 1024];
    for (;;) {
        const std::size_t n = s->read(tmp, sizeof tmp);
        if (n == 0)
            break;
        check_alloc(out.size() + n);
        out.insert(out.end(), tmp, tmp + n);
    }
    return out;
}

} // namespace format
} // namespace loomora
