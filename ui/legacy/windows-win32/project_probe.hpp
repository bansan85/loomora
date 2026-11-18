// Project probe modes of the UI (specs/002-loomora-file-format/contracts/ui-validate-project.md):
// --validate-project <path> and --roundtrip-project <path>. No application logic: the file is
// opened, validated and (round trip) exported and validated again by the backend through the C API
// (loomora.h); this file only builds the one result line from the counts the report gives. The line
// is the summary line of `loomora validate --quiet`, byte for byte.
//
// Platform neutral C++11 (no UI toolkit, no operating system call): the same file is in
// ui/legacy/windows-win32/, ui/legacy/linux-xlib/ and ui/modern/linux-gtk3/, and each UI does the
// parsing of its own command line and the writing of the line.
#ifndef LOOMORA_PROJECT_PROBE_HPP
#define LOOMORA_PROJECT_PROBE_HPP

#include <string>

namespace project_probe {

enum Mode {
    kValidate = 1, // --validate-project
    kRoundtrip = 2 // --roundtrip-project
};

// Runs `mode` on the file `path` (UTF-8, exactly as it was given on the command line). Sets *line to
// the line to print (without the end of line; empty when there is nothing to print) and returns the
// exit status of the program: 0 the file is valid (warnings allowed), 1 it has an error or the round
// trip failed, 2 the file cannot be read at all or the mode is not available in this build.
// *diagnostic receives a one-line explanation for the error stream when the status is 2 and *line is
// empty (the round trip is compiled only when the backend has the export, LOOMORA_HAVE_PROJECT_EXPORT).
int Run(Mode mode, const std::string &path, std::string *line, std::string *diagnostic);

} // namespace project_probe

#endif
