// Probe mode of the Linux Legacy UI: see probe.hpp.
#include "probe.hpp"

#include <sys/utsname.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <vector>

#include "loomora.h"

namespace probe {

namespace {

const long kExitDelayMs = 2000L;

typedef std::chrono::steady_clock Clock;

Clock::time_point g_launch;
Clock::time_point g_displayed;
bool g_displayedSet = false;
Clock::time_point g_deadline;
bool g_armed = false;

// a - b in milliseconds.
long DiffMs(const Clock::time_point &a, const Clock::time_point &b) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(a - b).count());
}

// Appends `s` to `id` as a platform-id component: lowercase, anything outside [a-z0-9._-]
// becomes '-'.
void AppendIdPart(std::string &id, const std::string &s) {
    for (std::string::size_type i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            c = '-';
        id += c;
    }
}

// Value of KEY in the file `path` (a /etc/os-release style file, quotes removed) into `value`;
// returns true when found. At most 63 characters are kept.
bool OsReleaseValue(const char *path, const char *key, std::string &value) {
    std::FILE *f = std::fopen(path, "r");
    char line[256];
    const std::size_t keyLen = std::strlen(key);
    bool found = false;
    if (f == nullptr)
        return false;
    while (!found && std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, key, keyLen) == 0 && line[keyLen] == '=') {
            value.clear();
            for (const char *p = line + keyLen + 1;
                 *p != '\0' && *p != '\n' && value.size() < 63; p++) {
                if (*p != '"' && *p != '\'' && *p != ' ')
                    value += *p;
            }
            found = true;
        }
    }
    std::fclose(f);
    return found;
}

// Peak resident memory in kB (VmHWM of /proc/self/status), or -1 if it cannot be read.
long PeakRssKb() {
    std::FILE *f = std::fopen("/proc/self/status", "r");
    char line[256];
    long value = -1;
    if (f == nullptr)
        return -1;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "VmHWM:", 6) == 0) {
            char *end = nullptr;
            const long kb = std::strtol(line + 6, &end, 10);
            if (end != line + 6)
                value = kb;
            break;
        }
    }
    std::fclose(f);
    return value;
}

} // namespace

Options ParseArgs(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--probe") == 0) {
            options.enabled = true;
        } else if (std::strcmp(argv[i], "--probe-input") == 0) {
            if (i + 1 < argc)
                options.input = argv[++i];
            else
                options.badUsage = true;
        }
    }
    // The input override is only meaningful in probe mode.
    if (options.input != nullptr && !options.enabled)
        options.badUsage = true;
    return options;
}

void Start() { g_launch = Clock::now(); }

void MarkDisplayed() {
    if (!g_displayedSet) {
        g_displayed = Clock::now();
        g_displayedSet = true;
    }
}

void Arm() {
    g_deadline = Clock::now() + std::chrono::milliseconds(kExitDelayMs);
    g_armed = true;
}

long MsLeft() {
    if (!g_armed)
        return -1;
    const long left = DiffMs(g_deadline, Clock::now());
    return left > 0 ? left : 0;
}

std::string PlatformId() {
    std::string id;
    std::string osId;
    std::string osVersion;
    bool haveId = false;
    bool haveVersion = false;
    AppendIdPart(id, "linux");
    if (OsReleaseValue("/etc/os-release", "ID", osId) ||
        OsReleaseValue("/usr/lib/os-release", "ID", osId)) {
        haveId = !osId.empty();
        if (OsReleaseValue("/etc/os-release", "VERSION_ID", osVersion) ||
            OsReleaseValue("/usr/lib/os-release", "VERSION_ID", osVersion))
            haveVersion = !osVersion.empty();
    }
    if (haveId) {
        id += '-';
        AppendIdPart(id, osId);
    }
    if (haveVersion) {
        id += '-';
        AppendIdPart(id, osVersion);
    }
    utsname un;
    if (uname(&un) == 0 && un.machine[0] != '\0') {
        id += '-';
        AppendIdPart(id, un.machine);
    }
    return id;
}

int Emit(const std::string &input, const std::string &message, bool eolWarningShown,
         const char *disabledFeatures) {
    const std::string platformId = PlatformId();
    char timestamp[32];
    const std::time_t now = std::time(nullptr);
    const std::tm *utc = std::gmtime(&now);
    const bool displayed = g_displayedSet;
    long elapsedMs = displayed ? DiffMs(g_displayed, g_launch) : 0;
    const long rss = PeakRssKb();

    timestamp[0] = '\0';
    if (utc != nullptr)
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", utc);
    if (elapsedMs < 0)
        elapsedMs = 0;

    // First call with no room measures, second one fills the buffer (the line is ASCII).
    const std::size_t needed = loomora_probe_record(
        platformId.c_str(), LOOMORA_UI_LEGACY, LOOMORA_CORE_NATIVE, input.c_str(),
        message.c_str(), displayed, static_cast<uint32_t>(elapsedMs), static_cast<int64_t>(rss),
        eolWarningShown, disabledFeatures, timestamp, nullptr, 0);
    std::size_t written = 0;
    std::vector<uint8_t> buf;
    if (needed > 0) {
        try {
            buf.resize(needed);
        } catch (const std::bad_alloc &) {
            buf.clear();
        }
    }
    if (buf.size() == needed && needed > 0) {
        written = loomora_probe_record(
            platformId.c_str(), LOOMORA_UI_LEGACY, LOOMORA_CORE_NATIVE, input.c_str(),
            message.c_str(), displayed, static_cast<uint32_t>(elapsedMs),
            static_cast<int64_t>(rss), eolWarningShown, disabledFeatures, timestamp, &buf[0],
            needed);
    }
    if (written == needed && needed > 0) {
        std::fwrite(&buf[0], 1, needed, stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        return 0;
    }
    std::fprintf(stderr, "loomora: the backend could not build the probe record\n");
    return 1;
}

} // namespace probe
