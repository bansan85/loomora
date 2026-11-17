// Entry point of the iOS Legacy UI (UIKit, Objective-C, manual retain/release).
#import <UIKit/UIKit.h>

#import "AppDelegate.h"
#import "Probe.h"

int main(int argc, char *argv[]) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int status;
    [Probe start];
    if ([Probe badProjectUsage]) {
        // Two modes at once, or a project mode with -probe or without its path: nothing is written.
        status = 2;
    } else if ([Probe projectMode] != kProjectModeNone) {
        // -validate_project / -roundtrip_project: no UI, the line goes to NSLog.
        status = [Probe runProject];
    } else {
        status = UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
    [pool release];
    return status;
}
