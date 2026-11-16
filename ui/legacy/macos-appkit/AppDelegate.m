// The window: end-of-life banner (when needed), text field, greeting in the middle. Frames are
// computed by hand (no Auto Layout on 10.6) in -layoutContent, called at start, on resize and when
// the banner is dismissed. Colours are the system ones.
#import "AppDelegate.h"

#include <string.h>

#include "Probe.h"
#include "loomora.h"

// The limit counts characters (Unicode scalars), like the other UIs, not bytes or UTF-16 units.
static const NSUInteger kMaxInputCharacters = 1000;
// Largest greeting: 1,000 characters of at most 4 bytes each, plus "Bonjour ".
static const size_t kGreetingCapacity = 4096 + 64;
static const NSTimeInterval kProbeExitDelaySeconds = 2.0;

static const CGFloat kMargin = 12.0;
static const CGFloat kSpacing = 12.0;
static const CGFloat kFieldHeight = 24.0;
static const CGFloat kBannerPadding = 8.0;
static const CGFloat kBannerButtonWidth = 76.0;
static const CGFloat kBannerButtonHeight = 22.0;

// `text` cut after its first 1,000 Unicode scalars (a surrogate pair is never split); `text`
// itself when it is not longer.
static NSString *LimitedText(NSString *text) {
    NSUInteger length = [text length];
    NSUInteger index = 0;
    NSUInteger count = 0;
    while (index < length && count < kMaxInputCharacters) {
        unichar c = [text characterAtIndex:index];
        if (c >= 0xD800 && c <= 0xDBFF && index + 1 < length) {
            unichar next = [text characterAtIndex:index + 1];
            if (next >= 0xDC00 && next <= 0xDFFF)
                index++;
        }
        index++;
        count++;
    }
    return index >= length ? text : [text substringToIndex:index];
}

static NSString *GreetingError(int code) {
    return [NSString stringWithFormat:@"The greeting could not be computed (error %d).", code];
}

// "Bonjour " + input, computed by the backend: the input goes in as UTF-8 bytes. Returns the
// greeting or a readable error.
static NSString *GreetingFor(NSString *input) {
    const char *utf8 = [input UTF8String];
    if (utf8 == NULL)
        return GreetingError(LOOMORA_ERR_INVALID);
    NSMutableData *out = [NSMutableData dataWithLength:kGreetingCapacity];
    int32_t written =
        loomora_greet(utf8, strlen(utf8), (uint8_t *)[out mutableBytes], kGreetingCapacity);
    if (written < 0)
        return GreetingError((int)written);
    NSString *greeting = [[[NSString alloc] initWithBytes:[out bytes]
                                                   length:(NSUInteger)written
                                                 encoding:NSUTF8StringEncoding] autorelease];
    return greeting != nil ? greeting : GreetingError(LOOMORA_ERR_INVALID);
}

// A long greeting gets a smaller font so that it stays readable in the window.
static CGFloat GreetingFontSize(NSString *greeting) {
    NSUInteger length = [greeting length];
    if (length <= 60)
        return 28.0;
    return length <= 250 ? 20.0 : 13.0;
}

// A text field cell that centres its text vertically: the greeting is "in the middle".
@interface LMGreetingCell : NSTextFieldCell
@end

@implementation LMGreetingCell

- (NSRect)drawingRectForBounds:(NSRect)rect {
    NSRect drawing = [super drawingRectForBounds:rect];
    NSSize needed = [self cellSizeForBounds:rect];
    if (needed.height < drawing.size.height) {
        drawing.origin.y += (drawing.size.height - needed.height) / 2.0;
        drawing.size.height = needed.height;
    }
    return drawing;
}

@end

// The end-of-life banner: a rounded rectangle with a warning-colour border.
@interface LMBannerView : NSView
@end

@implementation LMBannerView

- (void)drawRect:(NSRect)dirty {
    NSRect box = NSInsetRect([self bounds], 0.5, 0.5);
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:box xRadius:6.0 yRadius:6.0];
    [[NSColor controlBackgroundColor] setFill];
    [path fill];
    [[NSColor orangeColor] setStroke];
    [path stroke];
}

@end

// A label (+1 retained): not editable, no border or background, wrapped. `cellClass` is the class
// of its cell, or Nil for the default one.
static NSTextField *NewLabel(Class cellClass) {
    NSTextField *label = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 100, 20)];
    if (cellClass != Nil)
        [label setCell:[[[cellClass alloc] initTextCell:@""] autorelease]];
    [label setEditable:NO];
    [label setSelectable:YES];
    [label setBezeled:NO];
    [label setBordered:NO];
    [label setDrawsBackground:NO];
    [label setTextColor:[NSColor controlTextColor]];
    [[label cell] setWraps:YES];
    [[label cell] setLineBreakMode:NSLineBreakByWordWrapping];
    return label;
}

@implementation AppDelegate

+ (NSString *)abiMismatchMessage {
    if (loomora_abi_version() == (uint32_t)LOOMORA_ABI_VERSION)
        return nil;
    return [NSString stringWithFormat:@"Backend ABI version %u does not match the version %u "
                                      @"this application expects.",
                                      (unsigned)loomora_abi_version(),
                                      (unsigned)LOOMORA_ABI_VERSION];
}

- (id)initWithInitialInput:(const char *)initialInput probe:(BOOL)probe {
    self = [super init];
    if (self == nil)
        return nil;
    NSString *text = initialInput != NULL ? [NSString stringWithUTF8String:initialInput] : nil;
    input_ = [LimitedText(text != nil ? text : @"Coucou") copy];
    probe_ = probe;
    abiError_ = [[AppDelegate abiMismatchMessage] retain];
    if (abiError_ == nil) {
        int32_t major = 0;
        int32_t minor = 0;
        eolWarning_ = [[NSString alloc] initWithUTF8String:loomora_eol_warning()];
        if (LMSystemVersion(&major, &minor))
            eolWarningVisible_ = loomora_is_eol(LOOMORA_FAMILY_MACOS, major, minor) == 1;
    }
    greetingText_ = [GreetingFor(input_) retain];
    return self;
}

- (void)dealloc {
    [probeTimer_ invalidate];
    [window_ release];
    [field_ release];
    [greetingLabel_ release];
    [banner_ release];
    [bannerLabel_ release];
    [bannerButton_ release];
    [input_ release];
    [greetingText_ release];
    [eolWarning_ release];
    [abiError_ release];
    [super dealloc];
}

#pragma mark Window

// Application menu (Quit) and Edit menu (cut, copy, paste, select all work in the field).
- (void)buildMenu {
    NSMenu *mainMenu = [[[NSMenu alloc] initWithTitle:@""] autorelease];

    NSMenuItem *appItem = [[[NSMenuItem alloc] initWithTitle:@"Loomora"
                                                      action:NULL
                                               keyEquivalent:@""] autorelease];
    NSMenu *appMenu = [[[NSMenu alloc] initWithTitle:@"Loomora"] autorelease];
    [appMenu addItem:[[[NSMenuItem alloc] initWithTitle:@"Quit Loomora"
                                                 action:@selector(terminate:)
                                          keyEquivalent:@"q"] autorelease]];
    [appItem setSubmenu:appMenu];
    [mainMenu addItem:appItem];

    NSMenuItem *editItem = [[[NSMenuItem alloc] initWithTitle:@"Edit"
                                                       action:NULL
                                                keyEquivalent:@""] autorelease];
    NSMenu *editMenu = [[[NSMenu alloc] initWithTitle:@"Edit"] autorelease];
    [editMenu addItem:[[[NSMenuItem alloc] initWithTitle:@"Cut"
                                                  action:@selector(cut:)
                                           keyEquivalent:@"x"] autorelease]];
    [editMenu addItem:[[[NSMenuItem alloc] initWithTitle:@"Copy"
                                                  action:@selector(copy:)
                                           keyEquivalent:@"c"] autorelease]];
    [editMenu addItem:[[[NSMenuItem alloc] initWithTitle:@"Paste"
                                                  action:@selector(paste:)
                                           keyEquivalent:@"v"] autorelease]];
    [editMenu addItem:[[[NSMenuItem alloc] initWithTitle:@"Select All"
                                                  action:@selector(selectAll:)
                                           keyEquivalent:@"a"] autorelease]];
    [editItem setSubmenu:editMenu];
    [mainMenu addItem:editItem];

    [NSApp setMainMenu:mainMenu];
}

- (void)buildWindow {
    NSUInteger style = NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask |
                       NSResizableWindowMask;
    window_ = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 240)
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [window_ setTitle:@"Loomora"];
    [window_ setMinSize:NSMakeSize(400, 200)];
    // The delegate owns the window (released in -dealloc), closing it must not release it again.
    [window_ setReleasedWhenClosed:NO];
    [window_ setDelegate:self];
    [window_ center];
    NSView *content = [window_ contentView];

    greetingLabel_ = NewLabel([LMGreetingCell class]);
    [greetingLabel_ setAlignment:NSCenterTextAlignment];
    [content addSubview:greetingLabel_];

    if (abiError_ != nil) {
        [greetingLabel_ setStringValue:abiError_];
        [greetingLabel_ setFont:[NSFont systemFontOfSize:13.0]];
        return;
    }

    field_ = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 100, kFieldHeight)];
    [[field_ cell] setPlaceholderString:@"Your text"];
    [[field_ cell] setScrollable:YES];
    [field_ setStringValue:input_];
    [field_ setDelegate:self];
    [content addSubview:field_];

    if (eolWarningVisible_) {
        banner_ = [[LMBannerView alloc] initWithFrame:NSMakeRect(0, 0, 100, 40)];
        bannerLabel_ = NewLabel(Nil);
        [bannerLabel_ setStringValue:eolWarning_];
        [bannerLabel_ setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
        [banner_ addSubview:bannerLabel_];
        bannerButton_ = [[NSButton alloc]
            initWithFrame:NSMakeRect(0, 0, kBannerButtonWidth, kBannerButtonHeight)];
        [bannerButton_ setTitle:@"Dismiss"];
        [bannerButton_ setBezelStyle:NSRoundedBezelStyle];
        [[bannerButton_ cell] setControlSize:NSSmallControlSize];
        [bannerButton_ setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
        [bannerButton_ setTarget:self];
        [bannerButton_ setAction:@selector(dismissBanner:)];
        [banner_ addSubview:bannerButton_];
        [content addSubview:banner_];
    }
    [self showGreeting];
}

// Puts the greeting in the label (the text is `greetingText_`).
- (void)showGreeting {
    [greetingLabel_ setFont:[NSFont systemFontOfSize:GreetingFontSize(greetingText_)]];
    [greetingLabel_ setStringValue:greetingText_];
}

// Banner (top), field, then the greeting in the rest of the window.
- (void)layoutContent {
    NSRect bounds = [[window_ contentView] bounds];
    CGFloat width = bounds.size.width - 2 * kMargin;
    CGFloat top = bounds.size.height - kMargin;
    if (width < 0)
        width = 0;
    if (abiError_ != nil) {
        [greetingLabel_ setFrame:NSMakeRect(kMargin, kMargin, width, top - kMargin)];
        return;
    }
    if (banner_ != nil && ![banner_ isHidden]) {
        CGFloat labelWidth = width - kBannerButtonWidth - 3 * kBannerPadding;
        if (labelWidth < 0)
            labelWidth = 0;
        CGFloat labelHeight =
            [[bannerLabel_ cell] cellSizeForBounds:NSMakeRect(0, 0, labelWidth, 100000)].height;
        CGFloat height = labelHeight + 2 * kBannerPadding;
        if (height < kBannerButtonHeight + 2 * kBannerPadding)
            height = kBannerButtonHeight + 2 * kBannerPadding;
        [banner_ setFrame:NSMakeRect(kMargin, top - height, width, height)];
        [bannerLabel_ setFrame:NSMakeRect(kBannerPadding, (height - labelHeight) / 2.0, labelWidth,
                                          labelHeight)];
        [bannerButton_ setFrame:NSMakeRect(width - kBannerPadding - kBannerButtonWidth,
                                           (height - kBannerButtonHeight) / 2.0,
                                           kBannerButtonWidth, kBannerButtonHeight)];
        top -= height + kSpacing;
    }
    [field_ setFrame:NSMakeRect(kMargin, top - kFieldHeight, width, kFieldHeight)];
    top -= kFieldHeight + kSpacing;
    CGFloat greetingHeight = top - kMargin;
    [greetingLabel_ setFrame:NSMakeRect(kMargin, kMargin, width,
                                        greetingHeight > 0 ? greetingHeight : 0)];
}

- (void)dismissBanner:(id)sender {
    eolWarningVisible_ = NO;
    [banner_ setHidden:YES];
    [self layoutContent];
}

#pragma mark Events

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [self buildMenu];
    [self buildWindow];
    [self layoutContent];
    [window_ makeKeyAndOrderFront:nil];
    if (field_ != nil) {
        [window_ makeFirstResponder:field_];
    }
    [NSApp activateIgnoringOtherApps:YES];
    // Forces the first draw: this is the first display.
    [window_ display];
    LMProbeMarkDisplayed();
    if (probe_ && probeTimer_ == nil) {
        probeTimer_ = [NSTimer timerWithTimeInterval:kProbeExitDelaySeconds
                                              target:self
                                            selector:@selector(probeFinish:)
                                            userInfo:nil
                                             repeats:NO];
        // Common modes: the countdown also runs while a menu is tracked or the window resized.
        [[NSRunLoop currentRunLoop] addTimer:probeTimer_ forMode:NSRunLoopCommonModes];
    }
}

// Closing the window ends the program with status 0.
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

- (void)windowDidResize:(NSNotification *)notification {
    [self layoutContent];
}

// Every change of the field: the limit first, then the greeting is computed again.
- (void)controlTextDidChange:(NSNotification *)notification {
    NSText *editor = [[notification userInfo] objectForKey:@"NSFieldEditor"];
    NSString *text = editor != nil ? [editor string] : [field_ stringValue];
    NSString *limited = LimitedText(text);
    if (limited != text) {
        if (editor != nil) {
            [editor setString:limited];
            [editor setSelectedRange:NSMakeRange([limited length], 0)];
        } else {
            [field_ setStringValue:limited];
        }
    }
    [input_ release];
    input_ = [limited copy];
    [greetingText_ release];
    greetingText_ = [GreetingFor(input_) retain];
    [self showGreeting];
}

// After 2 seconds: the probe line goes to stdout and the application terminates (status 0).
- (void)probeFinish:(NSTimer *)timer {
    probeTimer_ = nil;
    const char *input = [input_ UTF8String];
    const char *message = [greetingText_ UTF8String];
    if (LMProbeEmit(input != NULL ? input : "", message != NULL ? message : "",
                    eolWarningVisible_) != 0)
        exit(1);
    [NSApp terminate:nil];
}

@end
