// Loomora Legacy UI for macOS: Objective-C and AppKit, macOS 10.6 API only (manual retain/release,
// no ARC, no Auto Layout, no literals, no blocks). No application logic: the greeting, the
// end-of-life test and its warning text and the probe line all come from the backend.
#import <Cocoa/Cocoa.h>

@class LMBannerView;

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTextFieldDelegate> {
  @private
    NSWindow *window_;
    NSTextField *field_;
    NSTextField *greetingLabel_;
    LMBannerView *banner_;
    NSTextField *bannerLabel_;
    NSButton *bannerButton_;
    NSTimer *probeTimer_;
    NSString *input_;        // the text of the field, limited to 1,000 characters
    NSString *greetingText_; // the greeting as displayed: the backend's, or a readable error
    NSString *eolWarning_;
    NSString *abiError_;
    BOOL eolWarningVisible_;
    BOOL probe_;
}

// nil when the backend ABI matches the version this application was built for, otherwise the
// message to show.
+ (NSString *)abiMismatchMessage;

// `initialInput` is UTF-8 (--probe-input) or NULL for the default "Coucou". `probe` is true when
// --probe was given: the application then exits by itself 2 seconds after the first display.
- (id)initWithInitialInput:(const char *)initialInput probe:(BOOL)probe;

@end
