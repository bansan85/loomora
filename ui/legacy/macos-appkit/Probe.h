// Probe mode of the macOS Legacy UI (ui-contract.md): --probe and --probe-input <utf8>, and the
// project modes --validate-project <path> and --roundtrip-project <path> (LMProjectProbeRun).
//
// This file only reads the command line, measures (launch to first display, resident size) and
// reads the system. The JSON line itself is built by the backend (loomora_probe_record).
// Objective-C with manual retain/release, macOS 10.6 API only.
#ifndef LOOMORA_MACOS_PROBE_H
#define LOOMORA_MACOS_PROBE_H

#import <Foundation/Foundation.h>

#include <stdint.h>

// Project probe modes (specs/002-loomora-file-format/contracts/ui-validate-project.md).
enum {
    LMProjectModeNone = 0,
    LMProjectModeValidate = 1,  // --validate-project <path>
    LMProjectModeRoundtrip = 2  // --roundtrip-project <path>
};

typedef struct {
    BOOL enabled;        // --probe was given
    const char *input;   // value of --probe-input (points into argv, valid UTF-8), or NULL
    int projectMode;     // LMProjectModeNone, or the project mode given
    const char *projectPath; // its path (points into argv, valid UTF-8), or NULL
    BOOL badUsage;       // missing value, input without --probe, input or path that is not UTF-8,
                         // or more than one of --probe and the project modes
} LMProbeOptions;

// Records the launch time; call it first thing in main.
void LMProbeStart(void);

// Reads the raw argv (not NSProcessInfo, which repairs invalid UTF-8): the program refuses it, as
// the other UIs do. Unknown arguments (including those added by Finder or Cocoa) are ignored.
LMProbeOptions LMProbeParseArgs(int argc, char **argv);

// Records the first display; call it after the window was drawn. Later calls are ignored.
void LMProbeMarkDisplayed(void);

// Version of the running system (the end-of-life test). Returns NO when it cannot be read.
BOOL LMSystemVersion(int32_t *major, int32_t *minor);

// Writes the probe line to stdout. Returns 0 on success, 1 (and a message on stderr) when the
// backend could not build the record. `input` and `message` are UTF-8.
int LMProbeEmit(const char *input, const char *message, BOOL eolWarningShown);

// Project probe mode: opens the file `path` with the backend, validates it (and, for the round trip,
// exports a copy into memory and validates it) and writes the one result line to stdout: the line of
// `loomora validate --quiet`, built from the counts of the backend's report. No application logic.
// Returns the exit status: 0 valid (warnings allowed), 1 an error or a failed round trip, 2 the file
// cannot be read at all.
int LMProjectProbeRun(int mode, const char *path);

#endif
