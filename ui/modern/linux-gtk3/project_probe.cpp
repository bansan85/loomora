#include "project_probe.hpp"

#include <stddef.h>
#include <stdint.h>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "loomora.h"

namespace project_probe {

namespace {

// The timestamp of the copy: the round trip checks the structure of the copy, not a date, and the UI
// reads no clock and parses no part of the project (FR-027). One fixed valid value.
const char kCopyModifiedAt[] = "1970-01-01T00:00:00Z";

std::string Number(long long value) {
    const bool negative = value < 0;
    unsigned long long magnitude = negative ? 0ULL - static_cast<unsigned long long>(value)
                                            : static_cast<unsigned long long>(value);
    std::string digits;
    do {
        digits.insert(digits.begin(), static_cast<char>('0' + magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0);
    return negative ? "-" + digits : digits;
}

std::string Plural(long long n, const char *noun) {
    return Number(n) + " " + noun + (n == 1 ? "" : "s");
}

// A failure that leaves nothing to read (I/O, memory, bad argument): exit status 2, as the command.
bool Unreadable(int32_t rc) {
    return rc == LOOMORA_ERR_IO || rc == LOOMORA_ERR_NO_MEMORY || rc == LOOMORA_ERR_INVALID ||
           rc == LOOMORA_ERR_INTERNAL || rc == LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM;
}

// The summary line of `loomora validate --quiet` for a file: "OK <file>: 0 errors, 0 warnings",
// "FAIL <file>: N errors, M warnings" or "FAIL <file>: error <code>" (the failure left no error in
// the report); *status is 0, 1 or 2 as the command's exit status. The counts come from the report.
std::string SummaryLine(const std::string &name, int32_t rc, const loomora_report *report,
                        int *status) {
    const long long total = report != NULL ? static_cast<long long>(loomora_report_count(report)) : 0;
    const long long errors =
        report != NULL ? static_cast<long long>(loomora_report_error_count(report)) : 0;
    const long long warnings = total - errors;
    const bool codeOnly = rc != 0 && errors <= 0;
    if (rc != 0 && Unreadable(rc))
        *status = 2;
    else if (rc != 0 || errors > 0)
        *status = 1;
    else
        *status = 0;
    std::string line = std::string(*status == 0 ? "OK " : "FAIL ") + name + ": ";
    line += codeOnly ? "error " + Number(rc) : Plural(errors, "error") + ", " + Plural(warnings, "warning");
    return line;
}

#if defined(LOOMORA_HAVE_PROJECT_EXPORT)
struct Copy {
    std::vector<uint8_t> bytes;
    bool failed;
};

// The sink of the export: appends each chunk to the memory buffer.
int32_t AppendChunk(void *user, const uint8_t *data, size_t length) {
    Copy *copy = static_cast<Copy *>(user);
    try {
        copy->bytes.insert(copy->bytes.end(), data, data + length);
    } catch (const std::bad_alloc &) {
        copy->failed = true;
        return 1; // stops the export
    }
    return 0;
}

int RoundTrip(const std::string &path, const loomora_project *project, std::string *line) {
    Copy copy;
    copy.failed = false;
    loomora_report *exportReport = NULL;
    const int32_t profile =
        loomora_project_is_light(project) == 1 ? LOOMORA_PROFILE_LIGHT : LOOMORA_PROFILE_FULL;
    int32_t rc = loomora_project_export_stream(project, profile, NULL, 0, kCopyModifiedAt, 0,
                                               AppendChunk, &copy, &exportReport);
    if (exportReport != NULL)
        loomora_report_free(exportReport);
    if (rc == 0 && copy.failed)
        rc = LOOMORA_ERR_NO_MEMORY;
    if (rc != 0) {
        *line = "roundtrip FAIL export error " + Number(rc);
        return 1;
    }
    loomora_report *copyReport = NULL;
    const int32_t validated = loomora_validate_memory(copy.bytes.empty() ? NULL : &copy.bytes[0],
                                                      copy.bytes.size(), NULL, NULL, &copyReport);
    int status = 0;
    const std::string summary = SummaryLine(path, validated, copyReport, &status);
    if (copyReport != NULL)
        loomora_report_free(copyReport);
    *line = std::string(status == 0 ? "roundtrip ok " : "roundtrip FAIL ") + summary;
    return status == 0 ? 0 : 1;
}
#endif

} // namespace

Request ParseArgs(int argc, char **argv) {
    Request request;
    int projectOptions = 0;
    int probeOptions = 0; // --probe and --probe-input
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--validate-project") == 0 ||
            std::strcmp(argv[i], "--roundtrip-project") == 0) {
            projectOptions++;
            request.present = true;
            request.mode = std::strcmp(argv[i], "--validate-project") == 0 ? kValidate : kRoundtrip;
            if (i + 1 < argc)
                request.path = argv[++i];
            else
                request.badUsage = true;
        } else if (std::strcmp(argv[i], "--probe") == 0 || std::strcmp(argv[i], "--probe-input") == 0) {
            probeOptions++;
        }
    }
    if (projectOptions > 1 || (projectOptions == 1 && probeOptions > 0))
        request.badUsage = true;
    return request;
}

int Run(Mode mode, const std::string &path, std::string *line, std::string *diagnostic) {
    line->clear();
    diagnostic->clear();
    loomora_project *project = NULL;
    loomora_report *report = NULL;
    const int32_t rc = loomora_project_open_path(path.c_str(), NULL, NULL, &project, &report);
    int status = 0;
    const std::string summary = SummaryLine(path, rc, report, &status);
    if (report != NULL)
        loomora_report_free(report);

    if (rc != 0 || mode == kValidate) {
        if (project != NULL)
            loomora_project_close(project);
        *line = summary;
        return status;
    }
#if defined(LOOMORA_HAVE_PROJECT_EXPORT)
    status = RoundTrip(path, project, line);
    loomora_project_close(project);
    return status;
#else
    loomora_project_close(project);
    *diagnostic = "the round trip needs loomora_project_export_stream, which this build of the backend "
                  "does not have";
    return 2;
#endif
}

int RunRequest(const Request &request, const char *program) {
    if (request.badUsage) {
        std::fprintf(stderr,
                     "usage: %s [--probe [--probe-input <utf8>] | --validate-project <path> | "
                     "--roundtrip-project <path>]\n",
                     program);
        return 2;
    }
    std::string line;
    std::string diagnostic;
    int status = 2;
    try {
        status = Run(request.mode, request.path, &line, &diagnostic);
    } catch (const std::bad_alloc &) {
        std::fprintf(stderr, "loomora: out of memory\n");
        return 2;
    }
    if (!diagnostic.empty())
        std::fprintf(stderr, "loomora: %s\n", diagnostic.c_str());
    if (!line.empty()) {
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    return status;
}

} // namespace project_probe
