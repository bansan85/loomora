// Entry point of the iOS Legacy UI (UIKit, Objective-C, manual retain/release).
#import <UIKit/UIKit.h>

#import "AppDelegate.h"
#import "Probe.h"

int main(int argc, char *argv[]) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int status;
    [Probe start];
    status = UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    [pool release];
    return status;
}
