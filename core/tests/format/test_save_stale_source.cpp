// FR-023: a save copies the compressed bytes of the untouched entries straight from the old file.
// If another process rewrites that file in place between the open and the save, the copy must not
// carry stale sums into the new manifest: the save fails with LOOMORA_ERR_CORRUPT naming the entry,
// the previous file is untouched and no temporary file stays.
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../../include/loomora.h"
#include "../multi_project.hpp"
#include "format/io/memory_io.hpp"
#include "format/zip/zip_reader.hpp"
#include "fs_test_util.hpp"

using namespace loomora::format;
using basic_project::Bytes;

namespace {

// Overwrites one byte of the file in place (a writable handle on the same file, not a replace).
bool flip_byte_in_place(const std::string &path, std::uint64_t offset) {
    FILE *f = std::fopen(path.c_str(), "r+b");
    if (f == nullptr)
        return false;
    bool ok = std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0;
    const int c = ok ? std::fgetc(f) : EOF;
    ok = ok && c != EOF && std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0 &&
         std::fputc(c ^ 0xFF, f) == (c ^ 0xFF);
    return std::fclose(f) == 0 && ok;
}

// The first entry of the given method (0 stored, 8 deflate) with at least 16 compressed bytes.
bool find_entry(const Bytes &file, std::uint16_t method, std::string *name, std::uint64_t *middle) {
    MemorySource src(&file[0], file.size());
    ZipReader zip(src);
    zip.open();
    const std::vector<ZipEntry> &all = zip.entries();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].name == "mimetype" || all[i].name == "manifest.json")
            continue;
        if (all[i].method == method && all[i].compressed_size >= 16) {
            *name = all[i].name;
            *middle = all[i].data_offset + all[i].compressed_size / 2;
            return true;
        }
    }
    return false;
}

void check_stale(std::uint16_t method) {
    fstest::ScratchDir dir("stale");
    const std::string path = dir.file("multi.loomora");
    {
        loomora_editor *e = nullptr;
        multi_project::Ids ids;
        REQUIRE(multi_project::build_small(&e, &ids) == 0);
        REQUIRE(loomora_editor_save_path(e, path.c_str(), basic_project::kModifiedAt, 0, nullptr, nullptr,
                                         nullptr) == 0);
        loomora_editor_close(e);
    }
    Bytes original;
    REQUIRE(fstest::read_file(path, original));
    std::string victim;
    std::uint64_t at = 0;
    REQUIRE(find_entry(original, method, &victim, &at));

    loomora_editor *e = nullptr;
    REQUIRE(loomora_editor_open_path(path.c_str(), nullptr, nullptr, &e, nullptr) == 0);
    REQUIRE(loomora_editor_set_title(e, "Edited") == 0); // the entry stays "kept"

    // Another process rewrites the old file in place: same size, other bytes in a kept entry.
    REQUIRE(flip_byte_in_place(path, at));
    Bytes tampered;
    REQUIRE(fstest::read_file(path, tampered));
    REQUIRE(tampered.size() == original.size());
    REQUIRE(tampered != original);

    loomora_report *report = nullptr;
    const std::string out = dir.file("saved.loomora");
    CHECK(loomora_editor_save_path(e, path.c_str(), "2026-11-18T08:00:00Z", 0, nullptr, nullptr, &report) ==
          LOOMORA_ERR_CORRUPT);
    // the failure names the entry
    bool named = false;
    for (int64_t i = 0; report != nullptr && i < loomora_report_count(report); ++i) {
        std::uint8_t text[256] = {0};
        const int64_t n = loomora_report_text(report, static_cast<std::size_t>(i), 2, text, sizeof text);
        if (n > 0 && victim == std::string(reinterpret_cast<const char *>(text)))
            named = true;
    }
    CHECK(named);
    loomora_report_free(report);

    // a save to another path fails the same way and leaves nothing
    CHECK(loomora_editor_save_path(e, out.c_str(), "2026-11-18T08:00:00Z", 0, nullptr, nullptr, nullptr) ==
          LOOMORA_ERR_CORRUPT);
    loomora_editor_close(e);

    Bytes after;
    REQUIRE(fstest::read_file(path, after));
    CHECK((after == tampered)); // the save did not touch the old file
    CHECK(dir.names().size() == 1); // no temporary file, no output file
}

} // namespace

TEST_CASE("a save refuses a kept stored entry rewritten after the open", "[format][stale]") {
    check_stale(0);
}

TEST_CASE("a save refuses a kept deflated entry rewritten after the open", "[format][stale]") {
    check_stale(8);
}
