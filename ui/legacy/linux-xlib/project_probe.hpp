// Project probe modes of the UI (specs/002-loomora-file-format/contracts/ui-validate-project.md):
// --validate-project <path> and --roundtrip-project <path>. No application logic: the file is
// opened, validated and (round trip) exported and validated again by the backend through the C API
// (loomora.h); this file only builds the one result line from the counts the report gives. The line
// is the summary line of `loomora validate --quiet`, byte for byte.
//
// Platform neutral C++11 (no UI toolkit, no operating system call): Run() is the same code in
// ui/legacy/windows-win32/, ui/legacy/linux-xlib/ and ui/modern/linux-gtk3/; each UI parses its own
// command line (ParseArgs here) and writes the line (standard output on Linux).
#ifndef LOOMORA_PROJECT_PROBE_HPP
#define LOOMORA_PROJECT_PROBE_HPP

#include <string>

namespace project_probe {

enum Mode {
    kValidate = 1, // --validate-project
    kRoundtrip = 2 // --roundtrip-project
};

// What the command line asks for.
struct Request {
    bool present = false;  // --validate-project or --roundtrip-project was given
    Mode mode = kValidate;
    std::string path;      // its value, as given (UTF-8 bytes)
    bool badUsage = false; // a missing value, or two of --probe, --validate-project, --roundtrip-project,
                           // or --probe-input next to a project mode: the program exits with 2
};

// Reads argv[1..argc) for the two project options. Unknown arguments (among them the GTK ones) are
// left alone.
Request ParseArgs(int argc, char **argv);

// The whole project mode of a Linux UI: a usage error (status 2, nothing on standard output), or Run()
// with the line written to standard output (one line, flushed) and the diagnostic to the error
// stream. Returns the exit status of the program. No window, no display is opened.
int RunRequest(const Request &request, const char *program);

// Runs `mode` on the file `path` (UTF-8, exactly as it was given on the command line). Sets *line to
// the line to print (without the end of line; empty when there is nothing to print) and returns the
// exit status of the program: 0 the file is valid (warnings allowed), 1 it has an error or the round
// trip failed, 2 the file cannot be read at all or the mode is not available in this build.
// *diagnostic receives a one-line explanation for the error stream when the status is 2 and *line is
// empty (the round trip is compiled only when the backend has the export, LOOMORA_HAVE_PROJECT_EXPORT).
int Run(Mode mode, const std::string &path, std::string *line, std::string *diagnostic);

} // namespace project_probe

#endif
