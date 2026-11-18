// SC-005 measurement (spec 002, T074): the time to validate and open a large `.loomora` project and the
// peak memory of the process. Host program, not a test: it is not built by default (EXCLUDE_FROM_ALL,
// target loomora_bench_format) and is run by hand.
//
//   loomora_bench_format gen NAILS STEPS FILE      build the project through the public C API and save it
//   loomora_bench_format open FILE [REPEATS]       time loomora_validate_path and loomora_project_open_path
//                                                  and read every item (nails and steps) like a UI would
//
// The project has one 2D frame (500 x 500 mm), one layout of NAILS nails on a square grid (diameter 0.5
// mm, so that no two nails touch), one image, 3 spools and one result of STEPS steps. Each command
// prints one line per measure and the peak memory of the process (peak working set on Windows, maximum
// resident set size on Linux).
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// psapi.h needs this before the include to resolve K32GetProcessMemoryInfo on every target.
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include "../include/loomora.h"
#include "basic_project.hpp"

namespace {

typedef std::chrono::steady_clock Clock;

double since(const Clock::time_point &t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

double peak_memory_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    std::memset(&pmc, 0, sizeof pmc);
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
    return -1;
#else
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return -1;
    return static_cast<double>(ru.ru_maxrss) / 1024.0; // kilobytes on Linux
#endif
}

int fail(const char *what, int32_t rc) {
    std::fprintf(stderr, "bench_format: %s failed (%d)\n", what, static_cast<int>(rc));
    return 1;
}

int generate(std::uint32_t nails, std::uint32_t steps, const char *path) {
    using namespace basic_project;
    const Clock::time_point t0 = Clock::now();
    loomora_editor *e = nullptr;
    int32_t rc = create(&e, "Large project");
    if (rc != 0)
        return fail("create", rc);
    if ((rc = add_spools(e, 3)) != 0)
        return fail("add_spools", rc);
    uint32_t frame = 0, layout = 0, image = 0, result = 0;
    const char *frame_json =
        "{\"name\":\"Square 500\",\"kind\":\"2d\",\"dimensions\":{\"width_mm\":500,\"height_mm\":500},"
        "\"background\":\"#ffffff\",\"outline\":{\"type\":\"rectangle\",\"params\":{\"width_mm\":500,"
        "\"height_mm\":500},\"polyline\":[[-250,-250],[250,-250],[250,250],[-250,250]]},"
        "\"exclusions\":[],\"surface\":\"plane\"}";
    if ((rc = loomora_editor_add_frame(e, frame_json, &frame)) != 0)
        return fail("add_frame", rc);
    {
        std::uint32_t side = 1;
        while (static_cast<std::uint64_t>(side) * side < nails)
            ++side;
        const double step = 496.0 / (side > 1 ? side - 1 : 1);
        std::vector<loomora_nail> list(nails);
        for (std::uint32_t i = 0; i < nails; ++i) {
            loomora_nail n = loomora_nail();
            n.id = i;
            n.kind = 3; // interior
            n.x = round3(-248.0 + step * (i % side));
            n.y = round3(-248.0 + step * (i / side));
            n.diameter = 0.5;
            list[i] = n;
        }
        if ((rc = loomora_editor_add_layout(e, frame, "{\"name\":\"Grid\"}", &list[0], list.size(), &layout)) != 0)
            return fail("add_layout", rc);
    }
    if ((rc = add_image(e, &image)) != 0)
        return fail("add_image", rc);
    {
        const unsigned spool_count = 3;
        std::string spools;
        for (unsigned i = 0; i < spool_count; ++i)
            spools += std::string(i ? "," : "") + "{\"spool\":" + num(1 + i) + ",\"length_mm\":" + num(1000 + 10 * i) + "}";
        const std::string json = "{\"name\":\"Large result\",\"created_at\":\"" + std::string(kResultCreatedAt) +
                                 "\",\"frame\":" + num(frame) + ",\"layout\":" + num(layout) +
                                 ",\"images\":[{\"image\":" + num(image) + "}],\"spools\":[" + spools + "]}";
        std::vector<loomora_step> list(steps);
        for (std::uint32_t i = 0; i < steps; ++i) {
            loomora_step s = loomora_step();
            const bool first = i < spool_count;
            const bool last = i >= steps - spool_count;
            s.spool_index = first ? i : last ? i - (steps - spool_count) : (i * 5u + 1u) % spool_count;
            s.nail = static_cast<std::uint32_t>((static_cast<std::uint64_t>(i) * 7u + s.spool_index * 13u) % nails);
            s.rotation = static_cast<std::uint8_t>(i & 1u);
            s.type = first ? 1 : last ? 2 : 0;
            list[i] = s;
        }
        const Bytes thumb = make_png(4, 4, 200);
        if ((rc = loomora_editor_add_result(e, json.c_str(), &list[0], list.size(), &thumb[0], thumb.size(),
                                            "{\"algorithm\":\"greedy\",\"seed\":1}", &result)) != 0)
            return fail("add_result", rc);
    }
    const double built = since(t0);
    const Clock::time_point t1 = Clock::now();
    rc = loomora_editor_save_path(e, path, kModifiedAt, 0, nullptr, nullptr, nullptr);
    loomora_editor_close(e);
    if (rc != 0)
        return fail("save", rc);
    std::printf("gen nails=%lu steps=%lu: build %.3f s, save %.3f s, peak memory %.0f MB\n",
                static_cast<unsigned long>(nails), static_cast<unsigned long>(steps), built, since(t1), peak_memory_mb());
    return 0;
}

int open_file(const char *path, int repeats) {
    for (int r = 0; r < repeats; ++r) {
        loomora_report *report = nullptr;
        Clock::time_point t0 = Clock::now();
        int32_t rc = loomora_validate_path(path, nullptr, nullptr, &report);
        const double validate = since(t0);
        const long warnings = report ? static_cast<long>(loomora_report_count(report)) : -1;
        const long errors = report ? static_cast<long>(loomora_report_error_count(report)) : -1;
        loomora_report_free(report);
        report = nullptr;

        loomora_project *p = nullptr;
        t0 = Clock::now();
        const int32_t orc = loomora_project_open_path(path, nullptr, nullptr, &p, &report);
        const double open = since(t0);
        const long owarn = report ? static_cast<long>(loomora_report_count(report)) : -1;
        loomora_report_free(report);
        if (orc != 0 || p == nullptr)
            return fail("open", orc);

        // What a UI does after opening: the first result's steps and the first layout's nails.
        t0 = Clock::now();
        long long steps = 0, nails = 0;
        uint32_t ids[16];
        const int64_t nres = loomora_project_ids(p, LOOMORA_KIND_RESULT, ids, 16);
        if (nres > 0) {
            const int64_t n = loomora_result_step_count(p, ids[0]);
            std::vector<loomora_step> buf(n > 0 ? static_cast<std::size_t>(n) : 1);
            if (n > 0)
                steps = loomora_result_read_steps(p, ids[0], 0, buf.size(), &buf[0]);
        }
        const int64_t nlay = loomora_project_ids(p, LOOMORA_KIND_LAYOUT, ids, 16);
        if (nlay > 0) {
            const int64_t n = loomora_layout_nail_count(p, ids[0]);
            std::vector<loomora_nail> buf(n > 0 ? static_cast<std::size_t>(n) : 1);
            if (n > 0)
                nails = loomora_layout_read_nails(p, ids[0], 0, buf.size(), &buf[0]);
        }
        const double read = since(t0);
        loomora_project_close(p);
        std::printf("open #%d: validate rc=%d %.3f s (%ld issue(s), %ld error(s)); open rc=%d %.3f s (%ld warning(s)); read %lld steps + %lld nails %.3f s; peak memory %.0f MB\n",
                    r + 1, static_cast<int>(rc), validate, warnings, errors, static_cast<int>(orc), open, owarn, steps, nails,
                    read, peak_memory_mb());
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 5 && std::strcmp(argv[1], "gen") == 0)
        return generate(static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)),
                        static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)), argv[4]);
    if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "open") == 0)
        return open_file(argv[2], argc == 4 ? std::atoi(argv[3]) : 3);
    std::fprintf(stderr, "usage: loomora_bench_format gen NAILS STEPS FILE | open FILE [REPEATS]\n");
    return 2;
}
