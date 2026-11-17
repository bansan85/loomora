// loomora_refgen DIR: writes DIR/basic.loomora, the `basic` reference project of spec 002 (one 2D
// frame, one layout of 200 nails, one image with a PNG, 3 spools, one result of 5,000 steps), using
// only the public C API of loomora_project.h. The file is checked by tools/check_loomora.py and its
// SHA-256 is the golden value of the determinism test. It also writes DIR/empty.loomora (no item)
// and DIR/escape40.loomora (a result selecting 40 spools) for the same checker. Host program: not built for the cross
// targets or the self-test builds.
#include <cstdio>
#include <string>

#include "../include/loomora.h"
#include "basic_project.hpp"

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fputs("usage: loomora_refgen DIR\n", stderr);
        return 2;
    }
    const std::string dir = argv[1];
    for (int which = 0; which < 3; ++which) {
        loomora_editor *e = nullptr;
        const char *name = which == 0 ? "basic" : which == 1 ? "empty" : "escape40";
        int32_t rc = which == 0 ? basic_project::build_basic(&e)
                                : which == 1 ? basic_project::create(&e, "") : basic_project::build_escape(&e);
        if (rc != 0) {
            std::fprintf(stderr, "loomora_refgen: building %s failed (%d)
", name, static_cast<int>(rc));
            return 1;
        }
        const std::string path = dir + "/" + name + ".loomora";
        rc = loomora_editor_save_path(e, path.c_str(), basic_project::kModifiedAt, 0, nullptr, nullptr, nullptr);
        loomora_editor_close(e);
        if (rc != 0) {
            std::fprintf(stderr, "loomora_refgen: saving %s failed (%d)
", path.c_str(), static_cast<int>(rc));
            return 1;
        }
        std::printf("wrote %s
", path.c_str());
    }
    return 0;
}
