// Entry point of the Loomora Legacy UI for macOS. The command line is read first, so that a wrong
// usage or a backend ABI mismatch in probe mode ends the program before any window exists.
#import <Cocoa/Cocoa.h>

#include <stdio.h>

#include "AppDelegate.h"
#include "Probe.h"

int main(int argc, char **argv) {
    LMProbeStart();
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int status = 0;

    LMProbeOptions options = LMProbeParseArgs(argc, argv);
    NSString *abiError = [AppDelegate abiMismatchMessage];
    if (options.badUsage) {
        fputs("usage: Loomora [--probe [--probe-input <utf8>] | --validate-project <path> |"
              " --roundtrip-project <path>]\n",
              stderr);
        status = 2;
    } else if ((options.enabled || options.projectMode != LMProjectModeNone) && abiError != nil) {
        fprintf(stderr, "loomora: %s\n", [abiError UTF8String]);
        status = 1;
    } else if (options.projectMode != LMProjectModeNone) {
        // --validate-project / --roundtrip-project: no window, the line goes to stdout.
        status = LMProjectProbeRun(options.projectMode, options.projectPath);
    } else {
        NSApplication *application = [NSApplication sharedApplication];
        AppDelegate *delegate = [[AppDelegate alloc] initWithInitialInput:options.input
                                                                    probe:options.enabled];
        [application setDelegate:delegate];
        [application run];
        [delegate release];
    }

    [pool drain];
    return status;
}
