// Probe mode of the Linux Modern UI (ui-contract.md): --probe, --probe-input <utf8>.
// Only converts and measures: the JSON line itself comes from loomora_probe_record(). Nothing
// here depends on GTK; the timer that ends a probe run and the window are the application's.
#ifndef LOOMORA_GTK3_PROBE_HPP
#define LOOMORA_GTK3_PROBE_HPP

#include <chrono>
#include <string>

namespace loomora {

struct ProbeOptions {
    bool enabled = false;    // --probe was given
    bool has_input = false;  // --probe-input was given
    std::string input;       // its value, raw bytes (UTF-8 is checked by the caller)
    bool bad_usage = false;  // an option was malformed (e.g. --probe-input without a value)
};

// Reads --probe and --probe-input <utf8> from argv[1..argc). Unknown arguments, among them the
// GTK ones, are left alone.
ProbeOptions parse_probe_args(int argc, char** argv);

// Platform id of the running system, "linux-<os-release ID>-<VERSION_ID>-<machine>" (for
// example "linux-ubuntu-24.04-x86_64"); matches ^[a-z0-9][a-z0-9._-]*$. The same rule as
// tools/build.py linux_platform_id().
std::string platform_id();

class Probe {
public:
    // Records the launch time; construct it first thing in main().
    Probe();

    // Records the first display; call it from the first draw of the main window. Only the first
    // call counts.
    void mark_displayed();

    // Writes the probe line to stdout: `input` is the content of the field and `message` the
    // displayed greeting at this moment. Returns false, and writes an error to stderr, when the
    // backend could not build the record.
    bool write_record(const std::string& input, const std::string& message,
                      bool eol_warning_shown) const;

private:
    std::chrono::steady_clock::time_point launch_;
    std::chrono::steady_clock::time_point displayed_;
    bool is_displayed_ = false;
};

}  // namespace loomora

#endif
