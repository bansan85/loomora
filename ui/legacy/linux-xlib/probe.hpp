// Probe mode of the Linux Legacy UI (ui-contract.md): --probe, --probe-input <utf8>.
// Only converts and measures: the JSON line itself comes from loomora_probe_record().
// C++11; no function newer than glibc 2.12 (gettimeofday, not std::chrono::steady_clock or
// clock_gettime, which live in librt before glibc 2.17).
#ifndef LOOMORA_XLIB_PROBE_HPP
#define LOOMORA_XLIB_PROBE_HPP

#include <string>

namespace probe {

struct Options {
    bool enabled = false;         // --probe was given
    const char *input = nullptr;  // --probe-input value, nullptr when not given (points into argv)
    bool badUsage = false;        // an option was malformed (e.g. --probe-input without a value)
};

// Reads --probe and --probe-input <utf8> from argv. Unknown arguments are left alone.
Options ParseArgs(int argc, char **argv);

// Records the launch time; call it first thing in main().
void Start();

// Records the first display (the first Expose of the window drawn and flushed). Further calls are
// ignored.
void MarkDisplayed();

// Arms the exit timer: 2 seconds from now.
void Arm();

// Milliseconds left before the probe must be emitted (0 when it is due), or -1 when the probe is
// not armed.
long MsLeft();

// Platform id of the running system, "linux-<os-release ID>-<VERSION_ID>-<machine>" (for example
// "linux-ubuntu-24.04-x86_64"); matches ^[a-z0-9][a-z0-9._-]*$.
std::string PlatformId();

// Writes the probe line to stdout (one line, flushed). `input` and `message` are the content of
// the field and the greeting displayed at that moment (UTF-8); `disabledFeatures` is a
// comma-separated list ("" if none). Returns 0 on success, 1 when the backend could not build
// the record.
int Emit(const std::string &input, const std::string &message, bool eolWarningShown,
         const char *disabledFeatures);

} // namespace probe

#endif
