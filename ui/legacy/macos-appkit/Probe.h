// Probe mode of the macOS Legacy UI (ui-contract.md): --probe and --probe-input <utf8>.
//
// This file only reads the command line, measures (launch to first display, resident size) and
// reads the system. The JSON line itself is built by the backend (loomora_probe_record).
// Objective-C with manual retain/release, macOS 10.6 API only.
#ifndef LOOMORA_MACOS_PROBE_H
#define LOOMORA_MACOS_PROBE_H

#import <Foundation/Foundation.h>

#include <stdint.h>

typedef struct {
    BOOL enabled;        // --probe was given
    const char *input;   // value of --probe-input (points into argv, valid UTF-8), or NULL
    BOOL badUsage;       // missing value, input without --probe, or input that is not UTF-8
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

#endif
