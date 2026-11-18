// The ZIP reader (format/zip/zip_reader): each rule of contracts/project-format.md section 1 has
// an archive built byte by byte (zip_builder.hpp) that breaks it.
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../../include/loomora.h"
#include "format/io/memory_io.hpp"
#include "format/limits.hpp"
#include "format/testing.hpp"
#include "format/zip/zip_reader.hpp"
#include "zip_builder.hpp"

using namespace loomora::format;
using namespace ziptest;

namespace {

struct Outcome {
    int code;
    std::string issue;
    std::string entry;
    Outcome() : code(0) {}
};

Outcome try_open(const Bytes &b) {
    Outcome o;
    MemorySource src(b.empty() ? nullptr : &b[0], b.size());
    ZipReader r(src);
    try {
        r.open();
    } catch (const FormatError &e) {
        o.code = e.code();
        if (e.has_issue()) {
            o.issue = e.issue().code;
            o.entry = e.issue().entry;
        }
    }
    return o;
}

std::vector<Entry> two_files() {
    std::vector<Entry> v;
    v.push_back(file("a.json", "{\"a\":1}", 8));
    v.push_back(file("b.bin", "0123456789", 0));
    return with_mimetype(v);
}

Archive standard() { return build(two_files()); }

void require_rejected(const Bytes &b, const char *issue, int code = LOOMORA_ERR_CORRUPT) {
    const Outcome o = try_open(b);
    INFO("expected " << issue);
    CHECK(o.issue == issue);
    CHECK(o.code == code);
}

Bytes read_whole(const ZipReader &r, const ZipEntry &e) { return r.read_all(e, 1u << 30); }

} // namespace

TEST_CASE("a valid archive lists its entries with their sizes, methods and CRC", "[zip]") {
    const Archive a = standard();
    MemorySource src(&a.bytes[0], a.bytes.size());
    ZipReader r(src);
    r.open();
    REQUIRE(r.entries().size() == 3);
    CHECK(r.entries()[0].name == "mimetype");
    CHECK(r.entries()[0].local_offset == 0);
    CHECK(r.entries()[0].method == 0);
    CHECK(r.entries()[0].size == kMimetypeSize);
    const ZipEntry *j = r.find("a.json");
    REQUIRE(j != nullptr);
    CHECK(j->method == 8);
    CHECK(j->size == 7);
    CHECK(j->crc32 == crc_of(to_bytes("{\"a\":1}")));
    CHECK(read_whole(r, *j) == to_bytes("{\"a\":1}"));
    const ZipEntry *bin = r.find("b.bin");
    REQUIRE(bin != nullptr);
    CHECK(bin->method == 0);
    CHECK(bin->compressed_size == 10);
    CHECK(read_whole(r, *bin) == to_bytes("0123456789"));
    CHECK(r.find("nope") == nullptr);
    CHECK(r.archive_size() == a.bytes.size());
    CHECK(r.central_directory_offset() == a.cd_offset);
}

TEST_CASE("raw access returns the compressed bytes and ranges of a stored entry", "[zip]") {
    const Archive a = standard();
    MemorySource src(&a.bytes[0], a.bytes.size());
    ZipReader r(src);
    r.open();
    const ZipEntry &bin = *r.find("b.bin");
    char buf[16];
    CHECK(r.read_raw(bin, 0, buf, sizeof buf) == 10);
    CHECK(std::memcmp(buf, "0123456789", 10) == 0);
    CHECK(r.read_raw(bin, 4, buf, 3) == 3);
    CHECK(std::memcmp(buf, "456", 3) == 0);
    CHECK(r.read_raw(bin, 10, buf, 3) == 0);
    CHECK(r.read_range(bin, 7, buf, 16) == 3);
    CHECK(std::memcmp(buf, "789", 3) == 0);
    const ZipEntry &j = *r.find("a.json");
    const Bytes comp = deflate_raw(to_bytes("{\"a\":1}"));
    REQUIRE(j.compressed_size == comp.size());
    Bytes got(comp.size());
    CHECK(r.read_raw(j, 0, &got[0], got.size()) == comp.size());
    CHECK(got == comp);
    CHECK_THROWS_AS(r.read_range(j, 0, buf, 1), FormatError);
}

TEST_CASE("an entry is streamed through small buffers", "[zip]") {
    Bytes big(100000);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<std::uint8_t>((i * 7) ^ (i >> 5));
    Entry e;
    e.name = "big.bin";
    e.data = big;
    e.method = 8;
    const Archive a = build(with_mimetype(e));
    MemorySource src(&a.bytes[0], a.bytes.size());
    ZipReader r(src);
    r.open();
    std::unique_ptr<ZipEntryStream> s = r.open_entry(*r.find("big.bin"));
    Bytes out;
    std::uint8_t buf[777];
    for (;;) {
        const std::size_t n = s->read(buf, sizeof buf);
        if (n == 0)
            break;
        out.insert(out.end(), buf, buf + n);
    }
    CHECK(out == big);
    CHECK(s->produced() == big.size());
    CHECK(s->read(buf, sizeof buf) == 0); // stays at the end
}

TEST_CASE("an empty entry reads as empty, stored and deflate", "[zip]") {
    std::vector<Entry> v;
    v.push_back(file("e0", "", 0));
    v.push_back(file("e8", "", 8));
    const Archive a = build(with_mimetype(v));
    MemorySource src(&a.bytes[0], a.bytes.size());
    ZipReader r(src);
    r.open();
    CHECK(read_whole(r, *r.find("e0")).empty());
    CHECK(read_whole(r, *r.find("e8")).empty());
}

TEST_CASE("directory entries are accepted and left out", "[zip]") {
    std::vector<Entry> v;
    Entry d = file("frames/", "");
    d.ext_attr = 0x41ED0000u;
    v.push_back(d);
    v.push_back(file("frames/1/frame.json", "{}"));
    const Archive a = build(with_mimetype(v));
    MemorySource src(&a.bytes[0], a.bytes.size());
    ZipReader r(src);
    r.open();
    CHECK(r.entries().size() == 2);
    CHECK(r.find("frames/") == nullptr);
    CHECK(r.find("frames/1/frame.json") != nullptr);
}

TEST_CASE("ZIP64 fields of an entry and of the end record are read", "[zip][zip64]") {
    std::vector<Entry> v;
    Entry e = file("z.json", "{\"z\":true}", 8);
    e.zip64 = true;
    Entry f = file("o.bin", "offset", 0);
    f.zip64_offset = true;
    v.push_back(e);
    v.push_back(f);
    Options o;
    o.zip64_end = true;
    const Archive a = build(with_mimetype(v), o);
    MemorySource src(&a.bytes[0], a.bytes.size());
    ZipReader r(src);
    r.open();
    REQUIRE(r.entries().size() == 3);
    CHECK(read_whole(r, *r.find("z.json")) == to_bytes("{\"z\":true}"));
    CHECK(read_whole(r, *r.find("o.bin")) == to_bytes("offset"));
}

TEST_CASE("a ZIP64 end record whose classic fields are not saturated is accepted if equal", "[zip][zip64]") {
    Options o;
    o.zip64_end = true;
    o.saturate_end = false;
    const Archive a = build(two_files(), o);
    CHECK(try_open(a.bytes).code == 0);
}

TEST_CASE("inconsistent ZIP64 structures are rejected", "[zip][zip64]") {
    SECTION("saturated entry size without a ZIP64 extra field") {
        Archive a = standard();
        put_at32(a.bytes, a.at[2].central_off + C_USIZE, 0xFFFFFFFFu);
        put_at32(a.bytes, a.at[2].local_off + L_USIZE, 0xFFFFFFFFu);
        require_rejected(a.bytes, "zip.zip64");
    }
    SECTION("saturated offset without a ZIP64 extra field") {
        Archive a = standard();
        put_at32(a.bytes, a.at[1].central_off + C_OFFSET, 0xFFFFFFFFu);
        require_rejected(a.bytes, "zip.zip64");
    }
    SECTION("saturated end record without the ZIP64 locator") {
        Archive a = standard();
        put_at32(a.bytes, a.end_offset + E_CDOFFSET, 0xFFFFFFFFu);
        require_rejected(a.bytes, "zip.zip64");
    }
    SECTION("ZIP64 end record that disagrees with the classic one") {
        Options o;
        o.zip64_end = true;
        o.saturate_end = false;
        Archive a = build(two_files(), o);
        put_at16(a.bytes, a.end_offset + E_ENTRIES, 2); // classic says 2, ZIP64 says 3
        put_at16(a.bytes, a.end_offset + E_ENTRIES_DISK, 2);
        require_rejected(a.bytes, "zip.zip64");
    }
    SECTION("ZIP64 locator that points outside the file") {
        Options o;
        o.zip64_end = true;
        Archive a = build(two_files(), o);
        put_at32(a.bytes, a.end_offset - 20 + 8, 0x7FFFFFFFu);
        require_rejected(a.bytes, "zip.zip64");
    }
}

TEST_CASE("a data descriptor is accepted", "[zip]") {
    std::vector<Entry> v;
    Entry s = file("s.bin", "stored with descriptor", 0);
    s.descriptor = true;
    Entry d = file("d.json", "{\"deflate\":\"with descriptor\"}", 8);
    d.descriptor = true;
    Entry z = file("z.bin", "zip64 descriptor", 0);
    z.descriptor = true;
    z.zip64 = true;
    v.push_back(s);
    v.push_back(d);
    v.push_back(z);
    const Archive a = build(with_mimetype(v));
    MemorySource src(&a.bytes[0], a.bytes.size());
    ZipReader r(src);
    r.open();
    CHECK(read_whole(r, *r.find("s.bin")) == to_bytes("stored with descriptor"));
    CHECK(read_whole(r, *r.find("d.json")) == to_bytes("{\"deflate\":\"with descriptor\"}"));
    CHECK(read_whole(r, *r.find("z.bin")) == to_bytes("zip64 descriptor"));
    CHECK(r.find("d.json")->has_descriptor);
}

TEST_CASE("the central directory is authoritative", "[zip]") {
    SECTION("a longer extra field in the local header only moves the data") {
        std::vector<Entry> v;
        Entry e = file("x.bin", "payload", 0);
        e.local_extra = to_bytes("1234567890");
        v.push_back(e);
        const Archive a = build(with_mimetype(v));
        MemorySource src(&a.bytes[0], a.bytes.size());
        ZipReader r(src);
        r.open();
        CHECK(read_whole(r, *r.find("x.bin")) == to_bytes("payload"));
    }
    SECTION("bytes between entries that no entry owns are ignored") {
        Archive a = standard();
        // A local-header-looking record of an entry that the central directory does not list.
        Bytes junk;
        put32(junk, 0x04034b50u);
        junk.insert(junk.end(), 40, 0x55);
        a.bytes.insert(a.bytes.begin() + a.cd_offset, junk.begin(), junk.end());
        // Shift the central directory offset and the offsets of the central records after it.
        const std::size_t shift = junk.size();
        put_at32(a.bytes, a.end_offset + shift + E_CDOFFSET, static_cast<std::uint32_t>(a.cd_offset + shift));
        CHECK(try_open(a.bytes).code == 0);
    }
}

TEST_CASE("valid names are accepted", "[zip][names]") {
    const char *good[] = {"a", "manifest.json", "frames/1/layouts/2/nails.json", "x-ns/a b.bin",
                          "a.b/c", "..a", "a..", "\xC3\xA9t\xC3\xA9.bin", "frames/1/"};
    for (std::size_t i = 0; i < sizeof good / sizeof good[0]; ++i) {
        INFO(good[i]);
        std::string why;
        CHECK(is_valid_entry_name(good[i], &why));
    }
}

TEST_CASE("invalid names are rejected", "[zip][names]") {
    std::vector<std::string> bad;
    bad.push_back("");
    bad.push_back("/abs");
    bad.push_back("a//b");
    bad.push_back("./a");
    bad.push_back("a/./b");
    bad.push_back("a/..");
    bad.push_back("../a");
    bad.push_back("a/../b");
    bad.push_back(".");
    bad.push_back("..");
    bad.push_back("a\\b");
    bad.push_back("C:file");
    bad.push_back("C:/file");
    bad.push_back(std::string("a\0b", 3));
    bad.push_back("a\x01" "b");
    bad.push_back("a\x1F");
    bad.push_back("a\x7F");
    bad.push_back("\xC2\x85");        // U+0085, a C1 control
    bad.push_back("\xC3\x28");        // malformed UTF-8
    bad.push_back("\xED\xA0\x80");    // a surrogate
    bad.push_back(std::string(513, 'a'));
    bad.push_back("/");
    for (std::size_t i = 0; i < bad.size(); ++i) {
        INFO("name #" << i);
        CHECK_FALSE(is_valid_entry_name(bad[i]));
    }
    CHECK(is_valid_entry_name(std::string(512, 'a')));
    CHECK(is_valid_entry_name("dir/a:b")); // ':' only matters in the first component
}

TEST_CASE("the name length limit follows limits()", "[zip][names]") {
    ScopedLimits guard;
    limits().entry_name_max_bytes = 10;
    CHECK(is_valid_entry_name("0123456789"));
    CHECK_FALSE(is_valid_entry_name("01234567890"));
}

TEST_CASE("an archive with a bad name is rejected with zip.bad_name", "[zip][names]") {
    const char *bad[] = {"/abs.json", "a/../b.json", "a\\b.json", "C:x.json", "a//b.json"};
    for (std::size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        std::vector<Entry> v;
        v.push_back(file(bad[i], "{}"));
        const Archive a = build(with_mimetype(v));
        INFO(bad[i]);
        const Outcome o = try_open(a.bytes);
        CHECK(o.issue == "zip.bad_name");
        CHECK(o.entry == bad[i]);
        CHECK(o.code == LOOMORA_ERR_CORRUPT);
    }
}

TEST_CASE("names equal after ASCII case folding are rejected", "[zip][names]") {
    std::vector<Entry> v;
    v.push_back(file("Frames/1/Frame.json", "{}"));
    v.push_back(file("frames/1/frame.JSON", "{}"));
    require_rejected(build(with_mimetype(v)).bytes, "zip.duplicate_name");
    std::vector<Entry> w;
    w.push_back(file("same.json", "{}"));
    w.push_back(file("same.json", "{}"));
    require_rejected(build(with_mimetype(w)).bytes, "zip.duplicate_name");
    CHECK(fold_entry_name("AbC/\xC3\x89") == "abc/\xC3\x89"); // only ASCII letters fold
}

TEST_CASE("symbolic links, devices and sockets are rejected", "[zip][mode]") {
    const std::uint32_t modes[] = {0xA1FF0000u, 0x61A40000u, 0x21A40000u, 0xC1A40000u, 0x11A40000u};
    for (std::size_t i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        std::vector<Entry> v;
        Entry e = file("link", "target");
        e.ext_attr = modes[i];
        v.push_back(e);
        INFO(std::hex << modes[i]);
        require_rejected(build(with_mimetype(v)).bytes, "zip.symlink");
    }
    SECTION("a regular file or a mode written by a non-Unix tool is fine") {
        std::vector<Entry> v;
        Entry e = file("dos.bin", "x");
        e.made_by = 0x0014; // MS-DOS host: the high attribute bits mean nothing
        e.ext_attr = 0xA1FF0020u;
        v.push_back(e);
        CHECK(try_open(build(with_mimetype(v)).bytes).code == 0);
    }
}

TEST_CASE("overlapping entries and entries out of bounds are rejected", "[zip][layout]") {
    SECTION("an entry that runs into the next one") {
        Archive a;
        std::vector<Entry> v;
        Entry first = file("a.bin", "0123456789");
        first.has_declared_csize = true;
        first.declared_csize = 40;
        first.has_declared_usize = true;
        first.declared_usize = 40;
        v.push_back(first);
        v.push_back(file("b.bin", "more data here, long enough to be hit by the first"));
        a = build(with_mimetype(v));
        require_rejected(a.bytes, "zip.overlap");
    }
    SECTION("an entry that reaches into the central directory") {
        std::vector<Entry> v;
        Entry last = file("a.bin", "0123456789");
        last.has_declared_csize = true;
        last.declared_csize = 10000;
        last.has_declared_usize = true;
        last.declared_usize = 10000;
        v.push_back(last);
        require_rejected(build(with_mimetype(v)).bytes, "zip.bounds");
    }
    SECTION("an entry whose local header lies after the central directory") {
        Archive a = standard();
        put_at32(a.bytes, a.at[2].central_off + C_OFFSET, static_cast<std::uint32_t>(a.cd_offset + 4));
        require_rejected(a.bytes, "zip.bounds");
    }
    SECTION("an entry whose local header lies beyond the end of the file") {
        Archive a = standard();
        put_at32(a.bytes, a.at[2].central_off + C_OFFSET, 0x00FFFFFFu);
        require_rejected(a.bytes, "zip.bounds");
    }
    SECTION("two central records pointing at the same local header") {
        Archive a = standard();
        // b.bin's central record gets a.json's name and offset: a duplicate of the same bytes.
        // Same length names are needed to keep the record sizes: rename "b.bin" -> "A.JSON".
        put_at32(a.bytes, a.at[2].central_off + C_OFFSET, static_cast<std::uint32_t>(a.at[1].local_off));
        const Outcome o = try_open(a.bytes);
        CHECK(o.code != 0);
    }
}

TEST_CASE("unknown methods and encryption are rejected", "[zip][method]") {
    const unsigned methods[] = {1, 6, 9, 12, 14, 93, 99};
    for (std::size_t i = 0; i < sizeof methods / sizeof methods[0]; ++i) {
        std::vector<Entry> v;
        Entry e = file("m.bin", "x");
        e.method = static_cast<int>(methods[i]);
        v.push_back(e);
        INFO(methods[i]);
        require_rejected(build(with_mimetype(v)).bytes, "zip.method");
    }
    const unsigned flags[] = {0x0001, 0x0040, 0x2000, 0x0001 | 0x0800};
    for (std::size_t i = 0; i < sizeof flags / sizeof flags[0]; ++i) {
        std::vector<Entry> v;
        Entry e = file("enc.bin", "x");
        e.flags = flags[i];
        v.push_back(e);
        INFO(flags[i]);
        require_rejected(build(with_mimetype(v)).bytes, "zip.encrypted");
    }
}

TEST_CASE("the end record is checked", "[zip][end]") {
    SECTION("an archive comment") {
        Options o;
        o.comment = "hello";
        require_rejected(build(two_files(), o).bytes, "zip.comment");
    }
    SECTION("a multi-disk archive") {
        Options o;
        o.disk_number = 1;
        require_rejected(build(two_files(), o).bytes, "zip.multidisk");
    }
    SECTION("an entry count that does not match the central directory") {
        Archive a = standard();
        put_at16(a.bytes, a.end_offset + E_ENTRIES, 4);
        put_at16(a.bytes, a.end_offset + E_ENTRIES_DISK, 4);
        require_rejected(a.bytes, "zip.central");
    }
    SECTION("a central directory size larger than the archive") {
        Archive a = standard();
        put_at32(a.bytes, a.end_offset + E_CDSIZE, 0x7FFFFFFFu);
        require_rejected(a.bytes, "zip.central");
    }
    SECTION("a central directory offset that does not point at a central header") {
        Archive a = standard();
        put_at32(a.bytes, a.end_offset + E_CDOFFSET, 3);
        require_rejected(a.bytes, "zip.central");
    }
    SECTION("a huge entry count with a tiny directory") {
        Archive a = standard();
        put_at16(a.bytes, a.end_offset + E_ENTRIES, 65000);
        put_at16(a.bytes, a.end_offset + E_ENTRIES_DISK, 65000);
        require_rejected(a.bytes, "zip.central");
    }
    SECTION("a central header whose names run past the directory") {
        Archive a = standard();
        put_at16(a.bytes, a.at[2].central_off + C_NAMELEN, 60000);
        require_rejected(a.bytes, "zip.central");
    }
    SECTION("a record comment in a central header") {
        Archive a = standard();
        put_at16(a.bytes, a.at[1].central_off + C_COMMENTLEN, 3);
        require_rejected(a.bytes, "zip.comment");
    }
}

TEST_CASE("data that is not a ZIP archive is rejected as not a project", "[zip]") {
    require_rejected(Bytes(), "zip.not_zip", LOOMORA_ERR_NOT_A_PROJECT);
    require_rejected(Bytes(21, 0), "zip.not_zip", LOOMORA_ERR_NOT_A_PROJECT);
    require_rejected(to_bytes("this is certainly not a zip archive at all, just text"), "zip.not_zip",
                     LOOMORA_ERR_NOT_A_PROJECT);
    Bytes png(100000, 0x89);
    require_rejected(png, "zip.not_zip", LOOMORA_ERR_NOT_A_PROJECT);
}

TEST_CASE("a local header that disagrees with the central directory is rejected", "[zip][local]") {
    Archive base = standard();
    const std::size_t lo = base.at[2].local_off;
    SECTION("name") {
        base.bytes[lo + L_NAME] ^= 0x01;
        require_rejected(base.bytes, "zip.local_header");
    }
    SECTION("name length") {
        put_at16(base.bytes, lo + L_NAMELEN, 4);
        require_rejected(base.bytes, "zip.local_header");
    }
    SECTION("method") {
        put_at16(base.bytes, lo + L_METHOD, 8);
        require_rejected(base.bytes, "zip.local_header");
    }
    SECTION("CRC-32") {
        base.bytes[lo + L_CRC] ^= 0xFF;
        require_rejected(base.bytes, "zip.local_header");
    }
    SECTION("compressed size") {
        put_at32(base.bytes, lo + L_CSIZE, 9);
        require_rejected(base.bytes, "zip.local_header");
    }
    SECTION("uncompressed size") {
        put_at32(base.bytes, lo + L_USIZE, 11);
        require_rejected(base.bytes, "zip.local_header");
    }
    SECTION("signature") {
        base.bytes[lo] = 'X';
        require_rejected(base.bytes, "zip.local_header");
    }
    SECTION("a local header cut by the central directory") {
        put_at16(base.bytes, lo + L_EXTRALEN, 60000);
        require_rejected(base.bytes, "zip.bounds");
    }
    SECTION("with a data descriptor the sizes and CRC of the local header are not compared") {
        std::vector<Entry> v;
        Entry e = file("d.bin", "data");
        e.descriptor = true;
        v.push_back(e);
        Archive a = build(with_mimetype(v));
        put_at32(a.bytes, a.at[1].local_off + L_CRC, 12345);
        put_at32(a.bytes, a.at[1].local_off + L_USIZE, 99);
        CHECK(try_open(a.bytes).code == 0);
    }
}

TEST_CASE("the mimetype entry is checked", "[zip][mimetype]") {
    const int np = LOOMORA_ERR_NOT_A_PROJECT;
    SECTION("missing: the first entry is something else") {
        std::vector<Entry> v;
        v.push_back(file("manifest.json", "{}"));
        require_rejected(build(v).bytes, "zip.mimetype", np);
    }
    SECTION("an empty archive") {
        require_rejected(build(std::vector<Entry>()).bytes, "zip.mimetype", np);
    }
    SECTION("not first") {
        std::vector<Entry> v;
        v.push_back(file("manifest.json", "{}"));
        v.push_back(mimetype_entry());
        require_rejected(build(v).bytes, "zip.mimetype", np);
    }
    SECTION("deflated") {
        std::vector<Entry> v;
        v.push_back(file("mimetype", "application/vnd.loomora+zip", 8));
        require_rejected(build(v).bytes, "zip.mimetype", np);
    }
    SECTION("wrong content") {
        std::vector<Entry> v;
        v.push_back(file("mimetype", "application/vnd.loomora+zip\n"));
        require_rejected(build(v).bytes, "zip.mimetype", np);
        std::vector<Entry> w;
        w.push_back(file("mimetype", "application/vnd.loomora+zio"));
        require_rejected(build(w).bytes, "zip.mimetype", np);
        std::vector<Entry> x;
        x.push_back(file("mimetype", "application/zip"));
        require_rejected(build(x).bytes, "zip.mimetype", np);
    }
    SECTION("an extra field in the local header") {
        Entry m = mimetype_entry();
        m.local_extra = to_bytes("\x55\x54\x05\x00\x01\x00\x00\x00\x00");
        std::vector<Entry> v(1, m);
        require_rejected(build(v).bytes, "zip.mimetype", np);
    }
    SECTION("a different case") {
        std::vector<Entry> v;
        v.push_back(file("MimeType", "application/vnd.loomora+zip"));
        require_rejected(build(v).bytes, "zip.mimetype", np);
    }
    SECTION("not at offset 0") {
        Archive a = standard();
        // Prepend one byte: every offset of the central directory is now wrong in the same way,
        // so shift them all to keep the archive otherwise valid.
        a.bytes.insert(a.bytes.begin(), 0x00);
        for (std::size_t i = 0; i < a.at.size(); ++i)
            put_at32(a.bytes, a.at[i].central_off + 1 + C_OFFSET,
                     static_cast<std::uint32_t>(a.at[i].local_off + 1));
        put_at32(a.bytes, a.end_offset + 1 + E_CDOFFSET, static_cast<std::uint32_t>(a.cd_offset + 1));
        require_rejected(a.bytes, "zip.mimetype", np);
    }
}

TEST_CASE("a truncated archive never opens", "[zip]") {
    std::vector<Entry> v;
    v.push_back(file("a.json", "{\"a\":1}", 8));
    v.push_back(file("b.bin", "0123456789"));
    v.push_back(file("c/d.bin", "abc"));
    const Archive a = build(with_mimetype(v));
    for (std::size_t n = 0; n < a.bytes.size(); ++n) {
        const Bytes cut(a.bytes.begin(), a.bytes.begin() + n);
        const Outcome o = try_open(cut);
        INFO("truncated at " << n);
        REQUIRE((o.code == LOOMORA_ERR_NOT_A_PROJECT || o.code == LOOMORA_ERR_CORRUPT));
        CHECK_FALSE(o.issue.empty());
    }
    CHECK(try_open(a.bytes).code == 0);
}

TEST_CASE("every byte of a valid archive can be flipped without a crash", "[zip]") {
    const Archive a = standard();
    for (std::size_t i = 0; i < a.bytes.size(); ++i) {
        Bytes b = a.bytes;
        b[i] ^= 0x5A;
        try_open(b); // any outcome but a crash or an unexpected exception type
    }
}

TEST_CASE("the content of an entry is checked while streaming", "[zip][entry]") {
    SECTION("CRC-32 mismatch, stored") {
        std::vector<Entry> v;
        Entry e = file("c.bin", "some data");
        e.has_crc = true;
        e.crc = 0xDEADBEEFu;
        v.push_back(e);
        const Archive a = build(with_mimetype(v));
        MemorySource src(&a.bytes[0], a.bytes.size());
        ZipReader r(src);
        r.open();
        try {
            r.read_all(*r.find("c.bin"), 1000);
            FAIL("expected entry.crc");
        } catch (const FormatError &e2) {
            CHECK(e2.issue().code == "entry.crc");
            CHECK(e2.issue().entry == "c.bin");
            CHECK(e2.code() == LOOMORA_ERR_CORRUPT);
        }
    }
    SECTION("CRC-32 mismatch, deflate") {
        std::vector<Entry> v;
        Entry e = file("c.json", "{\"x\":[1,2,3]}", 8);
        e.has_crc = true;
        e.crc = 1;
        v.push_back(e);
        const Archive a = build(with_mimetype(v));
        MemorySource src(&a.bytes[0], a.bytes.size());
        ZipReader r(src);
        r.open();
        CHECK_THROWS_AS(r.read_all(*r.find("c.json"), 1000), FormatError);
    }
    SECTION("declared size smaller than the data") {
        std::vector<Entry> v;
        Entry e = file("s.json", "0123456789abcdef", 8);
        e.has_declared_usize = true;
        e.declared_usize = 10;
        v.push_back(e);
        const Archive a = build(with_mimetype(v));
        MemorySource src(&a.bytes[0], a.bytes.size());
        ZipReader r(src);
        r.open();
        try {
            r.read_all(*r.find("s.json"), 1000);
            FAIL("expected entry.size");
        } catch (const FormatError &e2) {
            CHECK(e2.issue().code == "entry.size");
        }
    }
    SECTION("declared size larger than the data") {
        std::vector<Entry> v;
        Entry e = file("s.json", "0123456789", 8);
        e.has_declared_usize = true;
        e.declared_usize = 25;
        v.push_back(e);
        const Archive a = build(with_mimetype(v));
        MemorySource src(&a.bytes[0], a.bytes.size());
        ZipReader r(src);
        r.open();
        try {
            r.read_all(*r.find("s.json"), 1000);
            FAIL("expected entry.size");
        } catch (const FormatError &e2) {
            CHECK(e2.issue().code == "entry.size");
        }
    }
    SECTION("a stored entry whose two sizes differ") {
        std::vector<Entry> v;
        Entry e = file("s.bin", "0123456789");
        e.has_declared_usize = true;
        e.declared_usize = 8;
        v.push_back(e);
        require_rejected(build(with_mimetype(v)).bytes, "entry.size");
    }
    SECTION("compressed bytes that are not a deflate stream") {
        std::vector<Entry> v;
        Entry e = file("bad.json", "0123456789", 8);
        e.has_payload = true;
        e.payload = to_bytes("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF");
        v.push_back(e);
        const Archive a = build(with_mimetype(v));
        MemorySource src(&a.bytes[0], a.bytes.size());
        ZipReader r(src);
        r.open();
        try {
            r.read_all(*r.find("bad.json"), 1000);
            FAIL("expected zip.inflate");
        } catch (const FormatError &e2) {
            CHECK(e2.issue().code == "zip.inflate");
        }
    }
    SECTION("trailing compressed bytes after the end of the deflate stream") {
        std::vector<Entry> v;
        Entry e = file("t.json", "0123456789", 8);
        e.has_payload = true;
        e.payload = deflate_raw(to_bytes("0123456789"));
        e.payload.push_back(0);
        e.payload.push_back(0);
        v.push_back(e);
        const Archive a = build(with_mimetype(v));
        MemorySource src(&a.bytes[0], a.bytes.size());
        ZipReader r(src);
        r.open();
        CHECK_THROWS_AS(r.read_all(*r.find("t.json"), 1000), FormatError);
    }
    SECTION("an entry larger than the caller's limit") {
        const Archive a = standard();
        MemorySource src(&a.bytes[0], a.bytes.size());
        ZipReader r(src);
        r.open();
        CHECK_THROWS_AS(r.read_all(*r.find("b.bin"), 9), FormatError);
        CHECK(r.read_all(*r.find("b.bin"), 10).size() == 10);
    }
}

TEST_CASE("the ratio guard rejects decompression bombs", "[zip][bomb]") {
    ScopedLimits guard;
    SECTION("2 MiB of zeros in a few kilobytes is a bomb") {
        std::vector<Entry> v;
        Entry e;
        e.name = "bomb.json";
        e.data = Bytes(2u * 1024 * 1024, 0);
        e.method = 8;
        v.push_back(e);
        const Archive a = build(with_mimetype(v));
        REQUIRE(a.bytes.size() < 20000);
        const Outcome o = try_open(a.bytes);
        CHECK(o.issue == "entry.bomb");
        CHECK(o.entry == "bomb.json");
        CHECK(o.code == LOOMORA_ERR_BOMB);
    }
    SECTION("at or below 1 MiB the ratio is not checked") {
        std::vector<Entry> v;
        Entry e;
        e.name = "dense.json";
        e.data = Bytes(1024 * 1024, 0);
        e.method = 8;
        v.push_back(e);
        CHECK(try_open(build(with_mimetype(v)).bytes).code == 0);
    }
    SECTION("the ratio follows limits().bomb_ratio") {
        std::vector<Entry> v;
        Entry e;
        e.name = "x.json";
        e.data = Bytes(1500000, 'a');
        e.method = 8;
        v.push_back(e);
        const Archive a = build(with_mimetype(v));
        limits().bomb_ratio = 100000000;
        CHECK(try_open(a.bytes).code == 0);
        limits().bomb_ratio = 250;
        CHECK(try_open(a.bytes).issue == "entry.bomb");
    }
    SECTION("a bomb that lies about its size is stopped at the declared size") {
        std::vector<Entry> v;
        Entry e;
        e.name = "lie.json";
        e.data = Bytes(3u * 1024 * 1024, 0);
        e.method = 8;
        e.has_declared_usize = true;
        e.declared_usize = 5000; // honest-looking ratio
        v.push_back(e);
        const Archive a = build(with_mimetype(v));
        MemorySource src(&a.bytes[0], a.bytes.size());
        ZipReader r(src);
        r.open();
        try {
            r.read_all(*r.find("lie.json"), 1u << 30);
            FAIL("expected entry.size");
        } catch (const FormatError &e2) {
            CHECK(e2.issue().code == "entry.size");
        }
    }
}

TEST_CASE("the progress callback cancels a read", "[zip][progress]") {
    struct Cb {
        static int stop(void *user, std::uint64_t, std::uint64_t) {
            ++*static_cast<int *>(user);
            return 1;
        }
    };
    ScopedLimits guard;
    limits().progress_interval_bytes = 1;
    Bytes big(50000, 'x');
    Entry e;
    e.name = "big.bin";
    e.data = big;
    const Archive a = build(with_mimetype(e));
    MemorySource src(&a.bytes[0], a.bytes.size());
    SECTION("while streaming an entry") {
        ZipReader r(src);
        r.open();
        int calls = 0;
        Progress p(&Cb::stop, &calls, 0);
        try {
            r.read_all(*r.find("big.bin"), 1u << 20, &p);
            FAIL("expected cancellation");
        } catch (const FormatError &e2) {
            CHECK(e2.code() == LOOMORA_ERR_CANCELLED);
        }
        CHECK(calls == 1);
    }
    SECTION("while opening") {
        ZipReader r(src);
        int calls = 0;
        Progress p(&Cb::stop, &calls, 0);
        try {
            r.open(&p);
            FAIL("expected cancellation");
        } catch (const FormatError &e2) {
            CHECK(e2.code() == LOOMORA_ERR_CANCELLED);
        }
    }
}

namespace {
bool fail_always(void *, std::size_t) { return true; }
} // namespace

TEST_CASE("a failed allocation is LOOMORA_ERR_NO_MEMORY", "[zip][memory]") {
    const Archive a = standard();
    MemorySource src(&a.bytes[0], a.bytes.size());
    ScopedHooks hooks_guard;
    hooks().fail_alloc = &fail_always;
    ZipReader r(src);
    try {
        r.open();
        FAIL("expected NO_MEMORY");
    } catch (const FormatError &e) {
        CHECK(e.code() == LOOMORA_ERR_NO_MEMORY);
    }
    reset_hooks();
    r.open();
    hooks().fail_alloc = &fail_always;
    try {
        r.open_entry(*r.find("a.json"));
        FAIL("expected NO_MEMORY");
    } catch (const FormatError &e) {
        CHECK(e.code() == LOOMORA_ERR_NO_MEMORY);
    }
}

TEST_CASE("a read error of the source is LOOMORA_ERR_IO", "[zip]") {
    struct Failing : public Source {
        const Source &inner;
        mutable int budget;
        Failing(const Source &s, int n) : inner(s), budget(n) {}
        std::uint64_t size() const override { return inner.size(); }
        std::size_t read_at(std::uint64_t off, void *buf, std::size_t len) const override {
            if (--budget < 0)
                throw FormatError(LOOMORA_ERR_IO, "read failed");
            return inner.read_at(off, buf, len);
        }
    };
    const Archive a = standard();
    MemorySource mem(&a.bytes[0], a.bytes.size());
    for (int n = 0; n < 12; ++n) {
        Failing f(mem, n);
        ZipReader r(f);
        try {
            r.open();
        } catch (const FormatError &e) {
            CHECK(e.code() == LOOMORA_ERR_IO);
        }
    }
}

// ---- T104: zlib counts in 32 bits ----------------------------------------------------------------

TEST_CASE("inflate_chunk_len never gives zlib more than 1 GiB, so a length of 2^32 never becomes 0", "[zip][inflate]") {
    ScopedHooks scoped;
    const std::uint64_t gib = 1ull << 30;
    CHECK(inflate_chunk_len(0) == 0u);
    CHECK(inflate_chunk_len(1) == 1u);
    CHECK(inflate_chunk_len(gib) == gib);
    CHECK(inflate_chunk_len(gib + 1) == gib);
    // The lengths that the 32-bit cast of zlib turned into 0 (or into a small wrong value).
    CHECK(inflate_chunk_len(1ull << 32) == gib);
    CHECK(inflate_chunk_len(2ull << 32) == gib);
    CHECK(inflate_chunk_len((1ull << 32) + 5) == gib);
    CHECK(inflate_chunk_len(~0ull) == gib);
    // A test can lower the cap, never raise it above 1 GiB.
    hooks().inflate_chunk_max = 7;
    CHECK(inflate_chunk_len(1ull << 32) == 7u);
    CHECK(inflate_chunk_len(3) == 3u);
    hooks().inflate_chunk_max = static_cast<std::size_t>(gib) * 2;
    CHECK(inflate_chunk_len(1ull << 32) == gib);
}

TEST_CASE("a deflated entry is inflated in chunks when one read asks for more than the chunk", "[zip][inflate]") {
    std::string content;
    for (int i = 0; i < 3000; ++i)
        content += static_cast<char>('a' + (i * 7 + i / 13) % 26);
    std::vector<Entry> v;
    v.push_back(file("d.txt", content, 8));
    v.push_back(file("s.txt", content, 0));
    const Archive a = build(with_mimetype(v));
    MemorySource src(&a.bytes[0], a.bytes.size());
    ZipReader r(src);
    r.open();

    ScopedHooks scoped;
    hooks().inflate_chunk_max = 7; // 3,000 bytes in one read are 429 calls of zlib
    std::uint64_t inflated = 0;
    hooks().inflated_bytes = &inflated;
    const char *names[2] = {"d.txt", "s.txt"};
    for (int k = 0; k < 2; ++k) {
        inflated = 0;
        std::unique_ptr<ZipEntryStream> s = r.open_entry(*r.find(names[k]));
        std::vector<char> out(content.size());
        // One read of the whole entry: every byte must come back from this single call.
        REQUIRE(s->read(&out[0], out.size()) == content.size());
        CHECK(std::string(out.begin(), out.end()) == content);
        CHECK(s->read(&out[0], out.size()) == 0u); // the end: size, length and CRC-32 verified
        CHECK(inflated == (k == 0 ? content.size() : 0u));
    }
    // read_all goes through the same path, with the CRC-32 computed in chunks too.
    const Bytes all = r.read_all(*r.find("d.txt"), 1u << 20);
    CHECK(std::string(all.begin(), all.end()) == content);
}

TEST_CASE("a corrupt CRC-32 is still caught when the CRC is computed in chunks", "[zip][inflate]") {
    std::vector<Entry> v;
    Entry e = file("d.txt", std::string(500, 'x'), 8);
    e.has_crc = true;
    e.crc = 1234;
    v.push_back(e);
    const Archive a = build(with_mimetype(v));
    MemorySource src(&a.bytes[0], a.bytes.size());
    ZipReader r(src);
    r.open();
    ScopedHooks scoped;
    hooks().inflate_chunk_max = 3;
    CHECK_THROWS_AS(r.read_all(*r.find("d.txt"), 1000), FormatError);
}

// ---- T106: the name index of ZipReader::find ---------------------------------------------------

TEST_CASE("find answers from the name index: exact names only, files only, the right entry", "[zip][index]") {
    std::vector<Entry> v;
    for (int i = 0; i < 300; ++i)
        v.push_back(file("items/" + std::to_string(i) + "/item.json", "{\"i\":" + std::to_string(i) + "}", i % 2 ? 8 : 0));
    Entry dir;
    dir.name = "items/";
    dir.method = 0;
    dir.ext_attr = 0x41ED0000u;
    v.push_back(dir);
    const Archive a = build(with_mimetype(v));
    MemorySource src(&a.bytes[0], a.bytes.size());
    ZipReader r(src);
    r.open();
    for (int i = 0; i < 300; ++i) {
        const ZipEntry *e = r.find("items/" + std::to_string(i) + "/item.json");
        REQUIRE(e != nullptr);
        CHECK(e == &r.entries()[static_cast<std::size_t>(i) + 1]);
        const Bytes b = read_whole(r, *e);
        CHECK(std::string(b.begin(), b.end()) == "{\"i\":" + std::to_string(i) + "}");
    }
    CHECK(r.find("mimetype") == &r.entries()[0]);
    CHECK(r.find("items/") == nullptr);                  // a directory entry is not a file
    CHECK(r.find("Items/0/item.json") == nullptr);       // exact match, no case folding
    CHECK(r.find("items/300/item.json") == nullptr);
    CHECK(r.find("") == nullptr);
    // A second open() rebuilds the index.
    r.open();
    CHECK(r.find("items/7/item.json") == &r.entries()[8]);
}

TEST_CASE("two names equal after ASCII case folding are still rejected with the index", "[zip][index]") {
    std::vector<Entry> v;
    v.push_back(file("a/B.json", "{}", 0));
    v.push_back(file("a/b.json", "{}", 0));
    require_rejected(build(with_mimetype(v)).bytes, "zip.duplicate_name");
}
