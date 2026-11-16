// Spike S11 (T132): Objective-C side of the probe. Built for x86_64 with -mmacosx-version-min=10.6
// (manual reference counting, no ARC, no @available).
//
// With -DPROBE_NEW_API it also calls APIs that are newer than 10.6, on purpose: the compile is
// expected to FAIL under -Werror=partial-availability, and the diagnostics are what the workflow
// prints. Without it, the file only uses what 10.6 has and must compile under that flag.
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

#include "probe_cpp.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    char buffer[64];
    int sum = probe_cpp_run(buffer, sizeof buffer);
    // Names a class of AppKit (so that the link needs the framework) without opening a window.
    NSString *message = [NSString stringWithFormat:@"objc+%s (%@)", buffer, NSStringFromClass([NSApplication class])];
    NSLog(@"%@", message);
#ifdef PROBE_NEW_API
    // Foundation, 10.7: NSJSONSerialization.
    NSData *json = [NSJSONSerialization dataWithJSONObject:[NSArray arrayWithObject:message] options:0 error:NULL];
    NSLog(@"json: %lu bytes", (unsigned long)[json length]);
    // AppKit, 10.7: -[NSWindow toggleFullScreen:].
    NSWindow *window = nil;
    [window toggleFullScreen:nil];
    // Foundation, 10.8: NSByteCountFormatter.
    NSLog(@"%@", [NSByteCountFormatter stringFromByteCount:1024 countStyle:NSByteCountFormatterCountStyleFile]);
#endif
    [pool drain];
    return sum == 20 ? 0 : 1;
}
