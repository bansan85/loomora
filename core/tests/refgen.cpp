// loomora_refgen [--scale N] DIR: writes the reference projects of spec 002 into DIR, using only the
// public C API of loomora_project.h: DIR/basic.loomora (one 2D frame, one layout of 200 nails, one
// image with a PNG, 3 spools, one result of 5,000 steps), DIR/empty.loomora (no item),
// DIR/escape40.loomora (a result selecting 40 spools), DIR/multi.loomora (3 frames incl. a 3D cube,
// 5 layouts, 4 images, 6 spools, 5 results; core/tests/multi_project.hpp), DIR/multi-light.loomora (the
// light export of multi, every item) and DIR/one.loomora (the light export of ONE result of multi, the
// Cube result: its frame, layout, two images and two spools; T084). The light files are made by
// loomora_project_export_path from the saved multi.loomora. The files are checked by
// tools/check_loomora.py and their SHA-256 are the golden values of the determinism test.
// With `--scale N` (N >= 2) only DIR/multi-xN.loomora is written: the `multi` project with the nails of
// each layout and the steps of each result multiplied by N, for the large-project measurement (SC-005).
// Host program: not built for the cross targets or the self-test builds.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../include/loomora.h"
#include "basic_project.hpp"
#include "multi_project.hpp"

namespace {

int write_project(loomora_editor *e, int32_t rc, const std::string &path) {
    if (rc != 0) {
        std::fprintf(stderr, "loomora_refgen: building %s failed (%d)\n", path.c_str(), static_cast<int>(rc));
        return 1;
    }
    rc = loomora_editor_save_path(e, path.c_str(), basic_project::kModifiedAt, 0, nullptr, nullptr, nullptr);
    loomora_editor_close(e);
    if (rc != 0) {
        std::fprintf(stderr, "loomora_refgen: saving %s failed (%d)\n", path.c_str(), static_cast<int>(rc));
        return 1;
    }
    std::printf("wrote %s\n", path.c_str());
    return 0;
}

// Exports `src` (a saved project) as a light copy, every item or only `result` when not 0.
int export_light(const std::string &src, const std::string &path, std::uint32_t result) {
    loomora_project *p = nullptr;
    loomora_report *report = nullptr;
    int32_t rc = loomora_project_open_path(src.c_str(), nullptr, nullptr, &p, &report);
    if (report != nullptr)
        loomora_report_free(report);
    if (rc != 0 || p == nullptr) {
        std::fprintf(stderr, "loomora_refgen: opening %s failed (%d)\n", src.c_str(), static_cast<int>(rc));
        return 1;
    }
    rc = loomora_project_export_path(p, path.c_str(), LOOMORA_PROFILE_LIGHT, result != 0 ? &result : nullptr,
                                     result != 0 ? 1u : 0u, basic_project::kModifiedAt, 0, nullptr, nullptr, nullptr);
    loomora_project_close(p);
    if (rc != 0) {
        std::fprintf(stderr, "loomora_refgen: exporting %s failed (%d)\n", path.c_str(), static_cast<int>(rc));
        return 1;
    }
    std::printf("wrote %s\n", path.c_str());
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    unsigned scale = 0;
    int first = 1;
    if (argc >= 3 && std::strcmp(argv[1], "--scale") == 0) {
        const long n = std::strtol(argv[2], nullptr, 10);
        if (n < 1 || n > 100000) {
            std::fputs("loomora_refgen: --scale needs a number from 1 to 100000\n", stderr);
            return 2;
        }
        scale = static_cast<unsigned>(n);
        first = 3;
    }
    if (argc != first + 1) {
        std::fputs("usage: loomora_refgen [--scale N] DIR\n", stderr);
        return 2;
    }
    const std::string dir = argv[first];
    if (scale != 0) {
        loomora_editor *e = nullptr;
        const int32_t rc = multi_project::build_multi(&e, nullptr, scale);
        return write_project(e, rc, dir + "/multi-x" + basic_project::num(scale) + ".loomora");
    }
    for (int which = 0; which < 4; ++which) {
        loomora_editor *e = nullptr;
        const char *name = which == 0 ? "basic" : which == 1 ? "empty" : which == 2 ? "escape40" : "multi";
        multi_project::Ids ids;
        const int32_t rc = which == 0 ? basic_project::build_basic(&e)
                           : which == 1 ? basic_project::create(&e, "")
                           : which == 2 ? basic_project::build_escape(&e)
                                        : multi_project::build_multi(&e, &ids);
        if (write_project(e, rc, dir + "/" + name + ".loomora") != 0)
            return 1;
        if (which == 3 && (export_light(dir + "/multi.loomora", dir + "/multi-light.loomora", 0) != 0 ||
                           export_light(dir + "/multi.loomora", dir + "/one.loomora", ids.result_3d) != 0))
            return 1;
    }
    return 0;
}
