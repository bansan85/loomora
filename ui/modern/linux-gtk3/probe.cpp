// Probe mode of the Linux Modern UI: see probe.hpp.
#include "probe.hpp"

#include <sys/utsname.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

#include "loomora.h"

namespace loomora {

ProbeOptions parse_probe_args(int argc, char** argv) {
    ProbeOptions options;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--probe") == 0) {
            options.enabled = true;
        } else if (std::strcmp(argv[i], "--probe-input") == 0) {
            if (i + 1 < argc) {
                options.has_input = true;
                options.input = argv[++i];
            } else {
                options.bad_usage = true;
            }
        }
    }
    // The input override is only meaningful in probe mode.
    if (options.has_input && !options.enabled)
        options.bad_usage = true;
    return options;
}

namespace {

// Appends `s` as a platform-id component: lowercase, anything outside [a-z0-9._-] becomes '-'.
void append_id_part(std::string& id, const std::string& s) {
    for (std::string::size_type i = 0; i < s.size(); i++) {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            c = '-';
        id += c;
    }
}

std::string trim(const std::string& s) {
    const char* blanks = " \t\r\n";
    std::string::size_type first = s.find_first_not_of(blanks);
    if (first == std::string::npos)
        return std::string();
    return s.substr(first, s.find_last_not_of(blanks) - first + 1);
}

// Value of KEY in the contents of /etc/os-release (quotes removed), empty when absent.
std::string os_release_value(const std::string& contents, const std::string& key) {
    std::istringstream lines(contents);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.compare(0, key.size() + 1, key + "=") == 0) {
            std::string value = line.substr(key.size() + 1);
            for (std::string::size_type i = 0; i < value.size(); i++) {
                if (value[i] == '"' || value[i] == '\'')
                    value[i] = ' ';
            }
            return trim(value);
        }
    }
    return std::string();
}

bool read_file(const char* path, std::string& contents) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in)
        return false;
    contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Peak resident memory in kB (VmHWM of /proc/self/status), or -1 if it cannot be read.
std::int64_t peak_rss_kb() {
    std::string contents;
    if (!read_file("/proc/self/status", contents))
        return -1;
    const std::string key = "VmHWM:";
    std::string::size_type at = contents.find(key);
    if (at == std::string::npos)
        return -1;
    // Not strtoll: with glibc 2.38 and GCC 13 it binds to __isoc23_strtoll (GLIBC_2.38), which
    // would raise the glibc version the executable asks for for no reason.
    std::istringstream value(contents.substr(at + key.size()));
    std::int64_t kb = -1;
    if (!(value >> kb))
        return -1;
    return kb;
}

// "YYYY-MM-DDTHH:MM:SSZ", the current time in UTC.
std::string timestamp_utc() {
    std::time_t now = std::time(nullptr);
    std::tm utc;
    char text[32];
    gmtime_r(&now, &utc);
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

}  // namespace

std::string platform_id() {
    std::string id = "linux";
    std::string contents;
    std::string os_id;
    std::string os_version;
    if (read_file("/etc/os-release", contents) || read_file("/usr/lib/os-release", contents)) {
        os_id = os_release_value(contents, "ID");
        os_version = os_release_value(contents, "VERSION_ID");
    }
    if (!os_id.empty()) {
        id += '-';
        append_id_part(id, os_id);
    }
    if (!os_version.empty()) {
        id += '-';
        append_id_part(id, os_version);
    }
    struct utsname un;
    if (uname(&un) == 0 && un.machine[0] != '\0') {
        id += '-';
        append_id_part(id, un.machine);
    }
    return id;
}

Probe::Probe() : launch_(std::chrono::steady_clock::now()) {}

void Probe::mark_displayed() {
    if (!is_displayed_) {
        displayed_ = std::chrono::steady_clock::now();
        is_displayed_ = true;
    }
}

bool Probe::write_record(const std::string& input, const std::string& message,
                         bool eol_warning_shown) const {
    const std::string id = platform_id();
    const std::string timestamp = timestamp_utc();
    const std::uint32_t elapsed_ms =
        is_displayed_ ? static_cast<std::uint32_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(displayed_ - launch_)
                                .count())
                      : 0;
    const std::int64_t rss_kb = peak_rss_kb();

    // First call with no room measures, second one fills the buffer (the line is ASCII).
    const std::size_t needed = loomora_probe_record(
        id.c_str(), LOOMORA_UI_MODERN, LOOMORA_CORE_NATIVE, input.c_str(), message.c_str(),
        is_displayed_, elapsed_ms, rss_kb, eol_warning_shown, "", timestamp.c_str(), nullptr, 0);
    std::vector<std::uint8_t> line(needed);
    std::size_t written = 0;
    if (needed > 0) {
        written = loomora_probe_record(id.c_str(), LOOMORA_UI_MODERN, LOOMORA_CORE_NATIVE,
                                       input.c_str(), message.c_str(), is_displayed_, elapsed_ms,
                                       rss_kb, eol_warning_shown, "", timestamp.c_str(),
                                       line.data(), line.size());
    }
    if (written != needed || needed == 0) {
        std::fprintf(stderr, "loomora: the backend could not build the probe record\n");
        return false;
    }
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return true;
}

}  // namespace loomora
