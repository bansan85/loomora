#include "format/zip/zip_writer.hpp"

#include <algorithm>
#include <cstring>
#include <set>

#include "zlib.h"

#include "format/endian.hpp"
#include "format/limits.hpp"
#include "format/report.hpp"
#include "format/testing.hpp"
#include "loomora.h"

namespace loomora {
namespace format {

namespace {

const std::size_t kBuffer = 16 * 1024;     // output of deflate: fixed, so chunking cannot change the stream
const std::size_t kCopyBuffer = 32 * 1024; // copy-through and Source reads
const std::size_t kMaxZlibChunk = 1u << 30;

const std::uint32_t kSat32 = 0xFFFFFFFFu;
const unsigned kSat16 = 0xFFFFu;

[[noreturn]] void misuse(const std::string &message) {
    throw FormatError(LOOMORA_ERR_INTERNAL, message);
}

void check_alloc(std::size_t bytes) {
    if (should_fail_alloc(bytes))
        throw FormatError(LOOMORA_ERR_NO_MEMORY, "memory allocation failed");
}

// The thresholds, clamped to what the classic fields can hold.
std::uint64_t size_threshold() { return std::min<std::uint64_t>(limits().zip64_size_threshold, kSat32); }
std::uint64_t offset_threshold() { return std::min<std::uint64_t>(limits().zip64_offset_threshold, kSat32); }
std::uint64_t count_threshold() { return std::min<std::uint64_t>(limits().zip64_count_threshold, kSat16); }

// An upper bound of the deflate output for `n` input bytes (zlib's deflateBound for these
// parameters, with 6 bytes of margin), computed here so it does not depend on the width of uLong.
std::uint64_t deflate_bound(std::uint64_t n) { return n + (n >> 12) + (n >> 14) + (n >> 25) + 13; }

bool is_ascii(const std::string &s) {
    for (std::size_t i = 0; i < s.size(); ++i)
        if (static_cast<unsigned char>(s[i]) >= 0x80)
            return false;
    return true;
}

void put16(std::vector<unsigned char> &v, unsigned x) {
    unsigned char b[2];
    write_u16(b, static_cast<std::uint16_t>(x));
    v.insert(v.end(), b, b + 2);
}
void put32(std::vector<unsigned char> &v, std::uint32_t x) {
    unsigned char b[4];
    write_u32(b, x);
    v.insert(v.end(), b, b + 4);
}
void put64(std::vector<unsigned char> &v, std::uint64_t x) {
    unsigned char b[8];
    write_u64(b, x);
    v.insert(v.end(), b, b + 8);
}

struct Record {
    std::string name;
    std::uint16_t method;
    std::uint32_t crc;
    std::uint64_t csize;
    std::uint64_t usize;
    std::uint64_t offset;
    bool zip64_sizes;
    bool zip64_offset;

    Record() : method(0), crc(0), csize(0), usize(0), offset(0), zip64_sizes(false), zip64_offset(false) {}
};

} // namespace

struct ZipWriter::Impl {
    Sink &sink;
    Progress *progress;
    std::vector<Record> records;
    std::set<std::string> folded;
    bool finished;
    bool mimetype_ok; // add_mimetype is running

    // The entry being written.
    bool in_entry;
    Record cur;
    std::uint64_t declared;
    std::uint64_t written;
    std::uint32_t crc;
    std::uint64_t extra_pos; // absolute offset of the ZIP64 extra field data in the local header (0: none)
    z_stream zs;
    bool zinit;
    std::vector<unsigned char> out;

    Impl(Sink &s, Progress *p)
        : sink(s), progress(p), finished(false), mimetype_ok(false), in_entry(false), declared(0),
          written(0), crc(0), extra_pos(0), zinit(false) {
        std::memset(&zs, 0, sizeof zs);
    }
    ~Impl() {
        if (zinit)
            deflateEnd(&zs);
    }

    void write_sink(const void *data, std::size_t len) {
        if (len != 0)
            sink.write(data, len);
    }

    void begin(const std::string &name, std::uint16_t method, std::uint64_t usize, bool known_csize,
               std::uint64_t csize, std::uint32_t known_crc);
    void feed(const void *data, std::size_t len);
    void end();
    void write_central_directory();
    void flush_deflate(int flush);
};

ZipWriter::ZipWriter(Sink &sink, Progress *progress) : impl_(new Impl(sink, progress)), count_(0) {}

ZipWriter::~ZipWriter() {}

// Writes the local header and prepares the entry. For a deflate entry the compressed size is not
// known yet: its field holds 0 (or the ZIP64 marker) and end() patches it.
void ZipWriter::Impl::begin(const std::string &name, std::uint16_t method, std::uint64_t usize,
                            bool known_csize, std::uint64_t csize, std::uint32_t known_crc) {
    if (finished)
        misuse("the archive is finished");
    if (in_entry)
        misuse("an entry is already open");
    if (method != 0 && method != 8)
        misuse("only methods 0 and 8 can be written");
    const bool is_mimetype = name == kMimetypeName;
    if (records.empty() && !mimetype_ok)
        misuse("the first entry must be the mimetype (add_mimetype)");
    if (is_mimetype && !mimetype_ok)
        misuse("the mimetype entry is written by add_mimetype");
    std::string why;
    if (!is_valid_entry_name(name, &why))
        misuse("invalid entry name '" + name + "': " + why);
    if (name[name.size() - 1] == '/')
        misuse("a directory entry is never written: '" + name + "'");
    const std::string key = fold_entry_name(name);
    if (folded.count(key) != 0)
        misuse("duplicate entry name '" + name + "'");

    const std::uint64_t offset = sink.position();
    Record r;
    r.name = name;
    r.method = method;
    r.crc = known_crc;
    r.csize = known_csize ? csize : (method == 0 ? usize : 0);
    r.usize = usize;
    r.offset = offset;
    const std::uint64_t t = size_threshold();
    const bool csize_known = known_csize || method == 0;
    r.zip64_sizes = usize >= t || (csize_known ? r.csize >= t : deflate_bound(usize) >= t);
    r.zip64_offset = offset >= offset_threshold();

    std::vector<unsigned char> h;
    put32(h, 0x04034b50u);
    put16(h, r.zip64_sizes ? 45 : 20);
    put16(h, is_ascii(name) ? 0 : 0x0800);
    put16(h, method);
    put16(h, 0x0000);
    put16(h, 0x0021);
    put32(h, r.crc);
    put32(h, r.zip64_sizes ? kSat32 : static_cast<std::uint32_t>(r.csize));
    put32(h, r.zip64_sizes ? kSat32 : static_cast<std::uint32_t>(usize));
    put16(h, static_cast<unsigned>(name.size()));
    put16(h, r.zip64_sizes ? 20 : 0);
    h.insert(h.end(), name.begin(), name.end());
    extra_pos = 0;
    if (r.zip64_sizes) {
        put16(h, 1);
        put16(h, 16);
        extra_pos = offset + h.size();
        put64(h, usize);
        put64(h, r.csize);
    }
    write_sink(&h[0], h.size());
    cur = r;
    declared = usize;
    written = 0;
    crc = 0;
    in_entry = true;
    folded.insert(key);

    if (method == 8 && !known_csize) {
        check_alloc(kBuffer);
        out.resize(kBuffer);
        if (deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            throw FormatError(LOOMORA_ERR_NO_MEMORY, "memory allocation failed");
        zinit = true;
    }
}

void ZipWriter::Impl::flush_deflate(int flush) {
    for (;;) {
        zs.next_out = &out[0];
        zs.avail_out = static_cast<uInt>(out.size());
        const int ret = deflate(&zs, flush);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
            throw FormatError(ret == Z_MEM_ERROR ? LOOMORA_ERR_NO_MEMORY : LOOMORA_ERR_INTERNAL,
                              "deflate failed");
        const std::size_t n = out.size() - zs.avail_out;
        write_sink(&out[0], n);
        cur.csize += n;
        if (ret == Z_STREAM_END)
            return;
        if (flush == Z_NO_FLUSH && zs.avail_in == 0 && zs.avail_out != 0)
            return;
    }
}

void ZipWriter::Impl::feed(const void *data, std::size_t len) {
    if (!in_entry)
        misuse("no entry is open");
    if (len > declared - written)
        misuse("more data than the declared size of the entry");
    const unsigned char *p = static_cast<const unsigned char *>(data);
    std::size_t left = len;
    while (left != 0) {
        const std::size_t n = std::min(left, kMaxZlibChunk);
        crc = static_cast<std::uint32_t>(::crc32(crc, p, static_cast<uInt>(n)));
        if (cur.method == 0) {
            write_sink(p, n);
        } else {
            zs.next_in = const_cast<Bytef *>(p);
            zs.avail_in = static_cast<uInt>(n);
            flush_deflate(Z_NO_FLUSH);
        }
        if (progress != nullptr)
            progress->advance(n);
        p += n;
        left -= n;
        written += n;
    }
}

void ZipWriter::Impl::end() {
    if (!in_entry)
        misuse("no entry is open");
    if (written != declared)
        misuse("the data written is not the declared size of the entry");
    if (zinit) {
        zs.next_in = Z_NULL;
        zs.avail_in = 0;
        flush_deflate(Z_FINISH);
        deflateEnd(&zs);
        zinit = false;
        std::memset(&zs, 0, sizeof zs);
    }
    cur.crc = crc;
    if (!cur.zip64_sizes && cur.csize >= size_threshold())
        misuse("compressed size over the threshold without ZIP64");
    // Patch the local header: CRC-32 and, for a deflate entry, the compressed size.
    unsigned char b[8];
    write_u32(b, crc);
    sink.patch_at(cur.offset + 14, b, 4);
    if (cur.method == 8) {
        if (cur.zip64_sizes) {
            write_u64(b, cur.csize);
            sink.patch_at(extra_pos + 8, b, 8);
        } else {
            write_u32(b, static_cast<std::uint32_t>(cur.csize));
            sink.patch_at(cur.offset + 18, b, 4);
        }
    }
    records.push_back(cur);
    in_entry = false;
}

void ZipWriter::Impl::write_central_directory() {
    const std::uint64_t cd_offset = sink.position();
    for (std::size_t i = 0; i < records.size(); ++i) {
        const Record &r = records[i];
        std::vector<unsigned char> h;
        std::vector<unsigned char> extra;
        if (r.zip64_sizes || r.zip64_offset) {
            put16(extra, 1);
            put16(extra, static_cast<unsigned>((r.zip64_sizes ? 16 : 0) + (r.zip64_offset ? 8 : 0)));
            if (r.zip64_sizes) {
                put64(extra, r.usize);
                put64(extra, r.csize);
            }
            if (r.zip64_offset)
                put64(extra, r.offset);
        }
        put32(h, 0x02014b50u);
        put16(h, 0x0314);
        put16(h, (r.zip64_sizes || r.zip64_offset) ? 45 : 20);
        put16(h, is_ascii(r.name) ? 0 : 0x0800);
        put16(h, r.method);
        put16(h, 0x0000);
        put16(h, 0x0021);
        put32(h, r.crc);
        put32(h, r.zip64_sizes ? kSat32 : static_cast<std::uint32_t>(r.csize));
        put32(h, r.zip64_sizes ? kSat32 : static_cast<std::uint32_t>(r.usize));
        put16(h, static_cast<unsigned>(r.name.size()));
        put16(h, static_cast<unsigned>(extra.size()));
        put16(h, 0);
        put16(h, 0);
        put16(h, 0);
        put32(h, 0x81A40000u);
        put32(h, r.zip64_offset ? kSat32 : static_cast<std::uint32_t>(r.offset));
        h.insert(h.end(), r.name.begin(), r.name.end());
        h.insert(h.end(), extra.begin(), extra.end());
        write_sink(&h[0], h.size());
    }
    const std::uint64_t cd_size = sink.position() - cd_offset;
    const std::uint64_t n = records.size();
    const bool big_count = n >= count_threshold();
    const bool big_size = cd_size >= size_threshold();
    const bool big_offset = cd_offset >= offset_threshold();
    std::vector<unsigned char> e;
    if (big_count || big_size || big_offset) {
        const std::uint64_t z_off = sink.position();
        put32(e, 0x06064b50u);
        put64(e, 44);
        put16(e, 0x0314);
        put16(e, 45);
        put32(e, 0);
        put32(e, 0);
        put64(e, n);
        put64(e, n);
        put64(e, cd_size);
        put64(e, cd_offset);
        put32(e, 0x07064b50u);
        put32(e, 0);
        put64(e, z_off);
        put32(e, 1);
    }
    put32(e, 0x06054b50u);
    put16(e, 0);
    put16(e, 0);
    put16(e, big_count ? kSat16 : static_cast<unsigned>(n));
    put16(e, big_count ? kSat16 : static_cast<unsigned>(n));
    put32(e, big_size ? kSat32 : static_cast<std::uint32_t>(cd_size));
    put32(e, big_offset ? kSat32 : static_cast<std::uint32_t>(cd_offset));
    put16(e, 0);
    write_sink(&e[0], e.size());
}

void ZipWriter::add_mimetype() {
    if (impl_->finished || impl_->in_entry || !impl_->records.empty())
        misuse("the mimetype must be the first entry");
    impl_->mimetype_ok = true;
    try {
        begin_entry(kMimetypeName, 0, kMimetypeSize);
        write(kMimetypeContent, kMimetypeSize);
        end_entry();
    } catch (...) {
        impl_->mimetype_ok = false;
        throw;
    }
    impl_->mimetype_ok = false;
}

void ZipWriter::begin_entry(const std::string &name, std::uint16_t method, std::uint64_t uncompressed_size) {
    impl_->begin(name, method, uncompressed_size, false, 0, 0);
}

void ZipWriter::write(const void *data, std::size_t len) { impl_->feed(data, len); }

void ZipWriter::end_entry() {
    impl_->end();
    count_ = impl_->records.size();
}

void ZipWriter::add_stored(const std::string &name, const void *data, std::size_t len) {
    begin_entry(name, 0, len);
    write(data, len);
    end_entry();
}

void ZipWriter::add_deflate(const std::string &name, const void *data, std::size_t len) {
    begin_entry(name, 8, len);
    write(data, len);
    end_entry();
}

namespace {
void copy_source(ZipWriter &w, const Source &source) {
    check_alloc(kCopyBuffer);
    std::vector<unsigned char> buf(kCopyBuffer);
    std::uint64_t pos = 0;
    const std::uint64_t size = source.size();
    while (pos < size) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size - pos, buf.size()));
        const std::size_t got = source.read_at(pos, &buf[0], want);
        if (got == 0)
            throw FormatError(LOOMORA_ERR_IO, "the source ended before its size");
        w.write(&buf[0], got);
        pos += got;
    }
}
} // namespace

void ZipWriter::add_stored(const std::string &name, const Source &source) {
    begin_entry(name, 0, source.size());
    copy_source(*this, source);
    end_entry();
}

void ZipWriter::add_deflate(const std::string &name, const Source &source) {
    begin_entry(name, 8, source.size());
    copy_source(*this, source);
    end_entry();
}

// The copy-through trusts nothing it did not just read (FR-023): another process may have rewritten
// the old file in place after it was opened and its SHA-256 checked, and the new manifest would then
// carry the old sum for different bytes. The CRC-32 recorded for the entry is of the uncompressed
// data, so a stored entry is checked on the bytes being copied (one CRC-32 pass, no extra read), and
// a deflated one is inflated into a scratch buffer while its compressed bytes go to the sink. That
// costs one inflate of each kept JSON entry (small: bulk data is stored), and no hash: CRC-32 over
// the same pass catches an accidental or ordinary rewrite, which is what FR-023 asks for, at a
// fraction of the cost of a SHA-256 over every kept byte. A mismatch is LOOMORA_ERR_CORRUPT naming
// the entry; the caller aborts the sink, so the previous file stays and no temporary file is left.
namespace {
class RawVerifier {
  public:
    RawVerifier(const std::string &name, std::uint16_t method, std::uint32_t crc, std::uint64_t size)
        : name_(name), method_(method), want_crc_(crc), size_(size), crc_(0), out_(0), ended_(false),
          zinit_(false) {
        std::memset(&zs_, 0, sizeof zs_);
        if (method_ == 8) {
            check_alloc(kCopyBuffer);
            scratch_.resize(kCopyBuffer);
            if (inflateInit2(&zs_, -15) != Z_OK)
                throw FormatError(LOOMORA_ERR_NO_MEMORY, "memory allocation failed");
            zinit_ = true;
        }
    }
    ~RawVerifier() {
        if (zinit_)
            inflateEnd(&zs_);
    }

    void feed(const unsigned char *data, std::size_t len) {
        if (method_ == 0) {
            add(data, len);
            return;
        }
        zs_.next_in = const_cast<unsigned char *>(data);
        zs_.avail_in = static_cast<uInt>(len);
        for (;;) {
            if (ended_) {
                if (zs_.avail_in != 0)
                    bad("the compressed data is longer than its deflate stream");
                return;
            }
            zs_.next_out = &scratch_[0];
            zs_.avail_out = static_cast<uInt>(scratch_.size());
            const int ret = inflate(&zs_, Z_NO_FLUSH);
            if (ret == Z_MEM_ERROR)
                throw FormatError(LOOMORA_ERR_NO_MEMORY, "memory allocation failed");
            if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
                bad("the compressed data is not a valid deflate stream");
            add(&scratch_[0], scratch_.size() - zs_.avail_out);
            if (ret == Z_STREAM_END) {
                ended_ = true;
                continue;
            }
            if (zs_.avail_out == 0)
                continue; // the scratch buffer is full: output may still be pending
            if (zs_.avail_in != 0)
                bad("the compressed data is not a valid deflate stream"); // stuck: no progress
            return;
        }
    }

    void finish() {
        if (method_ == 8 && !ended_)
            bad("the compressed data ends before its deflate stream");
        if (out_ != size_)
            bad("the size of the entry changed since the archive was opened");
        if (crc_ != want_crc_)
            bad("the CRC-32 of the entry changed since the archive was opened");
    }

  private:
    void add(const unsigned char *p, std::size_t n) {
        if (n == 0)
            return;
        crc_ = static_cast<std::uint32_t>(::crc32(crc_, p, static_cast<uInt>(n)));
        out_ += n;
    }
    [[noreturn]] void bad(const char *what) {
        Issue issue;
        issue.severity = kError;
        issue.code = "entry.crc";
        issue.entry = name_;
        issue.message = "'" + name_ + "' cannot be copied into the new file: " + what +
                        " (was the file modified by another program?)";
        throw FormatError(LOOMORA_ERR_CORRUPT, issue);
    }

    std::string name_;
    std::uint16_t method_;
    std::uint32_t want_crc_;
    std::uint64_t size_;
    std::uint32_t crc_;
    std::uint64_t out_;
    bool ended_;
    bool zinit_;
    z_stream zs_;
    std::vector<unsigned char> scratch_;
};
} // namespace

void ZipWriter::add_raw(const std::string &name, std::uint16_t method, std::uint32_t crc32,
                        std::uint64_t compressed_size, std::uint64_t size, const Source &source,
                        std::uint64_t offset) {
    if (method == 0 && compressed_size != size)
        misuse("a stored entry has equal compressed and uncompressed sizes");
    if (offset > source.size() || compressed_size > source.size() - offset)
        misuse("the raw bytes lie outside the source");
    Impl &s = *impl_;
    s.begin(name, method, size, true, compressed_size, crc32);
    check_alloc(kCopyBuffer);
    std::vector<unsigned char> buf(kCopyBuffer);
    RawVerifier verify(name, method, crc32, size);
    std::uint64_t done = 0;
    while (done < compressed_size) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(compressed_size - done, buf.size()));
        if (!read_exact(source, offset + done, &buf[0], want))
            throw FormatError(LOOMORA_ERR_IO, "the source ended before the raw entry");
        verify.feed(&buf[0], want);
        s.write_sink(&buf[0], want);
        if (s.progress != nullptr)
            s.progress->advance(want);
        done += want;
    }
    verify.finish();
    // Everything is known: no patch, the header already holds the final values.
    s.records.push_back(s.cur);
    s.in_entry = false;
    count_ = s.records.size();
}

void ZipWriter::add_raw(const std::string &name, const ZipReader &reader, const ZipEntry &entry) {
    add_raw(name, entry.method, entry.crc32, entry.compressed_size, entry.size, reader.source(),
            entry.data_offset);
}

void ZipWriter::finish() {
    Impl &s = *impl_;
    if (s.finished)
        misuse("the archive is already finished");
    if (s.in_entry)
        misuse("an entry is still open");
    if (s.records.empty())
        misuse("the archive has no mimetype entry");
    s.write_central_directory();
    s.finished = true;
}

} // namespace format
} // namespace loomora
