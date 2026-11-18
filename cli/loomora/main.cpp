// loomora: the host command of the Loomora backend (spec 002, User Story 6).
//
//   loomora validate [--json] [--quiet] [--max-issues N] [--progress] <file> [<file> ...]
//   loomora --version
//
// Validates .loomora files with the full validation of the backend and prints one line per issue and
// one summary line per file. It has no logic of its own (FR-030): it calls loomora_validate_path and
// reads the report through loomora_report_*, nothing else of the library. The library opens the files
// read-only. Contract: docs/implementation/specs/002-loomora-file-format/contracts/cli-validate.md.
//
// Exit status: 0 every file valid (warnings allowed), 1 at least one file has an error, 2 usage
// error or a file that could not be read at all; with several files, the highest.
//
// The only thing taken from the library's internals is the hidden, test-only option
// `--set-limit name=value`, which lowers a warning threshold so that the large-file corpus can be
// small. It calls loomora::format::set_limit_by_name (core/src/format/limits.hpp) through the static
// library `loomora` this program links: no new symbol is exported from the shared library. The
// option is not listed in the usage text and nothing is read from the environment.
#include "loomora.h"

#include "format/limits.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

const int kOk = 0;
const int kInvalid = 1;
const int kUsage = 2;

struct Options {
    bool json;
    bool quiet;
    bool progress;
    bool max_given;
    unsigned long long max_issues;
    std::vector<std::string> files;
    Options() : json(false), quiet(false), progress(false), max_given(false), max_issues(0) {}
};

void usage(std::FILE *to) {
    std::fputs("usage: loomora validate [--json] [--quiet] [--max-issues N] [--progress] <file> [<file> ...]\n"
               "       loomora --version\n",
               to);
}

// ---- text helpers --------------------------------------------------------------------------

// Replaces what could break the one-line-per-issue output: control characters and bytes that are not
// well-formed UTF-8 become \xNN (a hostile entry name can hold both).
std::string printable(const std::string &s) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    const std::size_t n = s.size();
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        if (c >= 0x20 && c < 0x7f) {
            len = 1;
        } else if (c >= 0xc2 && c <= 0xdf) {
            len = 2;
        } else if (c >= 0xe0 && c <= 0xef) {
            len = 3;
        } else if (c >= 0xf0 && c <= 0xf4) {
            len = 4;
        }
        bool good = len != 0 && i + len <= n;
        for (std::size_t k = 1; good && k < len; ++k) {
            good = (static_cast<unsigned char>(s[i + k]) & 0xc0) == 0x80;
        }
        if (good && len >= 3) { // overlong forms, surrogates, above U+10FFFF
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xe0 && c1 < 0xa0) || (c == 0xed && c1 > 0x9f) || (c == 0xf0 && c1 < 0x90) ||
                (c == 0xf4 && c1 > 0x8f)) {
                good = false;
            }
        }
        if (good) {
            out.append(s, i, len);
            i += len;
        } else {
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
            ++i;
        }
    }
    return out;
}

std::string json_string(const std::string &raw) {
    const std::string s = printable(raw);
    std::string out = "\"";
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
    return out;
}

std::string number(long long v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld", v);
    return buf;
}

std::string plural(long long n, const char *noun) {
    return number(n) + " " + noun + (n == 1 ? "" : "s");
}

std::string pad(const std::string &s, std::size_t width) {
    return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
}

const char *kind_name(int32_t kind) {
    switch (kind) {
    case LOOMORA_KIND_FRAME: return "frame";
    case LOOMORA_KIND_LAYOUT: return "layout";
    case LOOMORA_KIND_IMAGE: return "image";
    case LOOMORA_KIND_SPOOL: return "spool";
    case LOOMORA_KIND_RESULT: return "result";
    default: return "item";
    }
}

// ---- standard output (UTF-8, bytes as they are on every host) ------------------------------

#ifdef _WIN32
UINT g_console_cp = 0;

void init_console() {
    _setmode(_fileno(stdout), _O_BINARY); // "\n" stays "\n", as on the other hosts
    _setmode(_fileno(stderr), _O_BINARY);
    g_console_cp = GetConsoleOutputCP(); // 0 when there is no console
    if (g_console_cp != 0) {
        SetConsoleOutputCP(CP_UTF8);
    }
}

void restore_console() {
    if (g_console_cp != 0) {
        SetConsoleOutputCP(g_console_cp);
    }
}

// The command line as UTF-8: the narrow argv follows the ANSI code page and loses characters.
std::vector<std::string> command_line(int, char **) {
    std::vector<std::string> args;
    int argc = 0;
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (wide == NULL) {
        return args;
    }
    for (int i = 1; i < argc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        std::string utf8(n > 1 ? static_cast<std::size_t>(n - 1) : 0, '\0');
        if (n > 1) {
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, &utf8[0], n, NULL, NULL);
        }
        args.push_back(utf8);
    }
    LocalFree(wide);
    return args;
}
#else
void init_console() {}
void restore_console() {}

std::vector<std::string> command_line(int argc, char **argv) {
    return std::vector<std::string>(argv + 1, argv + argc);
}
#endif

void emit(const std::string &text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
}

// ---- the report ----------------------------------------------------------------------------

std::string report_text(const loomora_report *r, std::size_t index, int32_t field) {
    const int64_t need = loomora_report_text(r, index, field, NULL, 0);
    if (need <= 0) {
        return std::string();
    }
    std::vector<uint8_t> buf(static_cast<std::size_t>(need));
    const int64_t got = loomora_report_text(r, index, field, &buf[0], buf.size());
    if (got != need) {
        return std::string();
    }
    return std::string(reinterpret_cast<const char *>(&buf[0]), buf.size());
}

struct Issue {
    int32_t severity; // 1 warning, 2 error
    int32_t kind;
    uint32_t id;
    std::string code, entry, message;
};

Issue read_issue(const loomora_report *r, std::size_t index) {
    Issue issue;
    issue.severity = 0;
    issue.kind = 0;
    issue.id = 0;
    if (loomora_report_issue(r, index, &issue.severity, &issue.kind, &issue.id) != 0) {
        issue.severity = 2;
    }
    issue.code = report_text(r, index, 1);
    issue.entry = report_text(r, index, 2);
    issue.message = report_text(r, index, 3);
    return issue;
}

std::string issue_line(const Issue &issue) {
    std::string line = pad(issue.severity == 1 ? "warning" : "error", 7) + " " + pad(printable(issue.code), 19) + " ";
    std::string fields;
    if (!issue.entry.empty()) {
        fields = "entry=" + printable(issue.entry);
    }
    if (issue.kind != 0 || issue.id != 0) {
        fields += std::string(fields.empty() ? "" : "  ") + kind_name(issue.kind) + "=" + number(issue.id);
    }
    if (!issue.message.empty()) {
        fields += std::string(fields.empty() ? "" : "  ") + printable(issue.message);
    }
    return line + fields + "\n";
}

std::string issue_json(const Issue &issue) {
    std::string item = "null";
    if (issue.kind != 0 || issue.id != 0) {
        item = std::string("{\"kind\":\"") + kind_name(issue.kind) + "\",\"id\":" + number(issue.id) + "}";
    }
    return std::string("{\"severity\":\"") + (issue.severity == 1 ? "warning" : "error") +
           "\",\"code\":" + json_string(issue.code) + ",\"entry\":" + json_string(issue.entry) +
           ",\"item\":" + item + ",\"message\":" + json_string(issue.message) + "}";
}

struct ProgressState {
    const std::string *file;
};

int32_t on_progress(void *user, uint64_t done, uint64_t total) {
    const ProgressState *state = static_cast<const ProgressState *>(user);
    std::fprintf(stderr, "progress %s: %llu of %llu bytes\n", state->file->c_str(),
                 static_cast<unsigned long long>(done), static_cast<unsigned long long>(total));
    return 0; // never cancels
}

// A failure that leaves nothing to read (I/O, memory, bad argument): status 2.
bool unreadable(int32_t rc) {
    return rc == LOOMORA_ERR_IO || rc == LOOMORA_ERR_NO_MEMORY || rc == LOOMORA_ERR_INVALID ||
           rc == LOOMORA_ERR_INTERNAL || rc == LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM;
}

int validate_file(const std::string &file, const Options &opt) {
    ProgressState state;
    state.file = &file;
    loomora_report *report = NULL;
    const int32_t rc = loomora_validate_path(file.c_str(), opt.progress ? on_progress : NULL, &state, &report);

    const std::size_t total = report != NULL ? static_cast<std::size_t>(loomora_report_count(report)) : 0;
    long long errors = report != NULL ? static_cast<long long>(loomora_report_error_count(report)) : 0;
    const long long warnings = static_cast<long long>(total) - errors;
    // `error <code>`: the library failed without an error in the report (no report at all, or a code
    // the report does not carry); the issue counts would say nothing.
    const bool code_only = rc != 0 && errors <= 0;

    int status = kOk;
    if (rc != 0 && unreadable(rc)) {
        status = kUsage;
    } else if (rc != 0 || errors > 0) {
        status = kInvalid;
    }
    if (status == kUsage) {
        std::fprintf(stderr, "loomora: cannot read %s (error %d)\n", file.c_str(), static_cast<int>(rc));
    }

    std::string out;
    if (opt.json) {
        out = "{\"file\":" + json_string(file) + ",\"valid\":" + (status == kOk ? "true" : "false") +
              ",\"errors\":" + number(errors) + ",\"warnings\":" + number(warnings) + ",\"issues\":[";
        if (!opt.quiet) {
            unsigned long long shown = 0;
            for (std::size_t i = 0; i < total && (!opt.max_given || shown < opt.max_issues); ++i, ++shown) {
                out += (i ? "," : "") + issue_json(read_issue(report, i));
            }
        }
        out += "]";
        if (code_only) {
            out += ",\"error\":" + number(rc);
        }
        out += "}\n";
    } else {
        if (!opt.quiet) {
            unsigned long long shown = 0;
            for (std::size_t i = 0; i < total && (!opt.max_given || shown < opt.max_issues); ++i, ++shown) {
                out += issue_line(read_issue(report, i));
            }
        }
        out += std::string(status == kOk ? "OK " : "FAIL ") + file + ": ";
        out += code_only ? "error " + number(rc) : plural(errors, "error") + ", " + plural(warnings, "warning");
        out += "\n";
    }
    emit(out);
    if (report != NULL) {
        loomora_report_free(report);
    }
    return status;
}

// "--name value" or "--name=value": returns false when the value is missing.
bool option_value(const std::vector<std::string> &args, std::size_t &i, const char *name, std::string &value) {
    const std::string &arg = args[i];
    const std::size_t len = std::string(name).size();
    if (arg.size() > len && arg[len] == '=') {
        value = arg.substr(len + 1);
        return true;
    }
    if (i + 1 >= args.size()) {
        return false;
    }
    value = args[++i];
    return true;
}

bool is_option(const std::string &arg, const char *name) {
    const std::string n(name);
    return arg == n || (arg.size() > n.size() && arg.compare(0, n.size(), n) == 0 && arg[n.size()] == '=');
}

bool parse_number(const std::string &text, unsigned long long &out) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    out = std::strtoull(text.c_str(), NULL, 10);
    return true;
}

int run(const std::vector<std::string> &args) {
    Options opt;
    std::size_t i = 0;
    if (!args.empty() && args[0] == "validate") {
        i = 1;
    }
    bool only_files = false;
    for (; i < args.size(); ++i) {
        const std::string &arg = args[i];
        std::string value;
        if (only_files || arg.empty() || arg[0] != '-' || arg == "-") {
            opt.files.push_back(arg);
        } else if (arg == "--") {
            only_files = true;
        } else if (arg == "--version") {
            uint32_t present = 0;
            int32_t major = 0, minor = 0;
            present = loomora_format_version(&major, &minor);
            std::printf("loomora (project format %d.%d, ABI %u)%s\n", static_cast<int>(major),
                        static_cast<int>(minor), static_cast<unsigned>(loomora_abi_version()),
                        present ? "" : " without project API");
            return kOk;
        } else if (arg == "--help" || arg == "-h") {
            usage(stdout);
            return kOk;
        } else if (arg == "--json") {
            opt.json = true;
        } else if (arg == "--quiet") {
            opt.quiet = true;
        } else if (arg == "--progress") {
            opt.progress = true;
        } else if (is_option(arg, "--max-issues")) {
            if (!option_value(args, i, "--max-issues", value) || !parse_number(value, opt.max_issues)) {
                std::fputs("loomora: --max-issues needs a number\n", stderr);
                return kUsage;
            }
            opt.max_given = true;
        } else if (is_option(arg, "--set-limit")) { // hidden, test only
            unsigned long long limit = 0;
            if (!option_value(args, i, "--set-limit", value)) {
                std::fputs("loomora: --set-limit needs name=value\n", stderr);
                return kUsage;
            }
            const std::size_t eq = value.find('=');
            if (eq == std::string::npos || !parse_number(value.substr(eq + 1), limit) ||
                !loomora::format::set_limit_by_name(value.substr(0, eq).c_str(), limit)) {
                std::fprintf(stderr, "loomora: bad --set-limit %s\n", value.c_str());
                return kUsage;
            }
        } else {
            std::fprintf(stderr, "loomora: unknown option %s\n", arg.c_str());
            usage(stderr);
            return kUsage;
        }
    }
    if (opt.files.empty()) {
        usage(stderr);
        return kUsage;
    }
    int status = kOk;
    for (std::size_t f = 0; f < opt.files.size(); ++f) {
        const int one = validate_file(opt.files[f], opt);
        if (one > status) {
            status = one;
        }
    }
    return status;
}

} // namespace

int main(int argc, char **argv) {
    init_console();
    const int status = run(command_line(argc, argv));
    std::fflush(stdout);
    restore_console();
    return status;
}
