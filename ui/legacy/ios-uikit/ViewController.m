// The only screen of the iOS Legacy UI. No application logic: the greeting, the end-of-life
// verdict and its text come from the C API of the backend (core/include/loomora.h); this file
// wires controls and converts text. Manual retain/release (no ARC), iOS 5 APIs only: no Auto
// Layout, no block, no ARC weak reference; the frames are computed in viewDidLayoutSubviews.
#import "ViewController.h"

#import "Probe.h"

#include <stdlib.h>
#include <string.h>

#include "loomora.h"

static NSString *const kDefaultInput = @"Coucou";
static NSString *const kGreetingError = @"(error)";
static NSString *const kAbiMismatch = @"The interface and the engine do not match (ABI version).";

static const CGFloat kMargin = 16.0;
static const CGFloat kFieldHeight = 31.0;
static const CGFloat kBannerPadding = 8.0;

// "Bonjour " + input, computed by the backend; kGreetingError when the backend reports an error.
static NSString *GreetingForInput(NSString *input) {
    const char *utf8 = [input UTF8String];
    NSUInteger length = [input lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    NSMutableData *buffer;
    int32_t written;
    NSString *greeting;
    if (utf8 == NULL) {
        return kGreetingError;
    }
    buffer = [NSMutableData dataWithLength:length + 64]; // "Bonjour " + input
    written = loomora_greet(utf8, length, (uint8_t *)[buffer mutableBytes], [buffer length]);
    if (written < 0) {
        return kGreetingError;
    }
    greeting = [[[NSString alloc] initWithBytes:[buffer bytes]
                                         length:(NSUInteger)written
                                       encoding:NSUTF8StringEncoding] autorelease];
    return greeting != nil ? greeting : kGreetingError;
}

@implementation ViewController

- (void)loadView {
    UIView *root = [[UIView alloc] initWithFrame:[[UIScreen mainScreen] applicationFrame]];
    root.backgroundColor = [UIColor whiteColor];
    root.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.view = root;
    [root release];
}

- (void)viewDidLoad {
    int major = 0;
    int minor = 0;
    NSString *override;
    NSString *initial;
    [super viewDidLoad];

    abiMatches = (loomora_abi_version() == LOOMORA_ABI_VERSION);
    [Probe getSystemMajor:&major minor:&minor];

    // End-of-life banner: black on amber, shown non-blockingly above the other controls.
    if (abiMatches && loomora_is_eol(LOOMORA_FAMILY_IOS, major, minor) == 1) {
        eolLabel = [[UILabel alloc] initWithFrame:CGRectZero];
        eolLabel.text = [NSString stringWithUTF8String:loomora_eol_warning()];
        eolLabel.font = [UIFont systemFontOfSize:16.0];
        eolLabel.textColor = [UIColor blackColor];
        eolLabel.backgroundColor = [UIColor colorWithRed:1.0 green:0.85 blue:0.3 alpha:1.0];
        eolLabel.textAlignment = NSTextAlignmentCenter;
        eolLabel.numberOfLines = 0;
        eolLabel.accessibilityIdentifier = @"eol-warning";
        [self.view addSubview:eolLabel];
    }

    inputField = [[UITextField alloc] initWithFrame:CGRectZero];
    override = [Probe inputOverride];
    initial = [Probe limited:(override != nil ? override : kDefaultInput)];
    inputField.text = initial;
    inputField.borderStyle = UITextBorderStyleRoundedRect;
    inputField.font = [UIFont systemFontOfSize:17.0];
    inputField.textColor = [UIColor blackColor];
    inputField.autocorrectionType = UITextAutocorrectionTypeNo;
    inputField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    inputField.returnKeyType = UIReturnKeyDone;
    inputField.clearButtonMode = UITextFieldViewModeWhileEditing;
    inputField.delegate = self;
    inputField.accessibilityIdentifier = @"input";
    [inputField addTarget:self
                   action:@selector(inputChanged:)
         forControlEvents:UIControlEventEditingChanged];
    [self.view addSubview:inputField];

    // The greeting can be long (1,000 characters): it wraps and the scroll view lets the user
    // reach all of it instead of overflowing the screen.
    greetingScroll = [[UIScrollView alloc] initWithFrame:CGRectZero];
    greetingScroll.backgroundColor = [UIColor whiteColor];
    greetingLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    greetingLabel.font = [UIFont systemFontOfSize:24.0];
    greetingLabel.textColor = [UIColor blackColor];
    greetingLabel.backgroundColor = [UIColor whiteColor];
    greetingLabel.textAlignment = NSTextAlignmentCenter;
    greetingLabel.numberOfLines = 0;
    greetingLabel.accessibilityIdentifier = @"greeting";
    [greetingScroll addSubview:greetingLabel];
    [self.view addSubview:greetingScroll];

    [self refreshGreeting];
}

// iOS 5 releases the views of a controller that is not on screen when memory is low; viewDidLoad
// builds them again.
- (void)viewDidUnload {
    inputField.delegate = nil;
    [inputField release];
    inputField = nil;
    [greetingLabel release];
    greetingLabel = nil;
    [greetingScroll release];
    greetingScroll = nil;
    [eolLabel release];
    eolLabel = nil;
    [super viewDidUnload];
}

- (void)dealloc {
    inputField.delegate = nil;
    [inputField release];
    [greetingLabel release];
    [greetingScroll release];
    [eolLabel release];
    [super dealloc];
}

- (BOOL)shouldAutorotateToInterfaceOrientation:(UIInterfaceOrientation)orientation {
    return YES;
}

// The greeting is asked to the backend at each change of the field.
- (void)refreshGreeting {
    NSString *input = inputField.text != nil ? inputField.text : @"";
    greetingLabel.text = abiMatches ? GreetingForInput(input) : kAbiMismatch;
    [self.view setNeedsLayout];
}

// Truncates a text pasted beyond the limit, without error, then updates the greeting.
- (void)inputChanged:(id)sender {
    NSString *text = inputField.text != nil ? inputField.text : @"";
    NSString *limited = [Probe limited:text];
    if (![limited isEqualToString:text]) {
        inputField.text = limited;
    }
    [self refreshGreeting];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    [textField resignFirstResponder];
    return YES;
}

// iOS 7 and later let the root view extend under the status bar: the layout guide, which the
// earlier systems do not have, then gives the space to leave free (0 below iOS 7).
- (CGFloat)topInset {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if ([self respondsToSelector:@selector(topLayoutGuide)]) {
        return [[self topLayoutGuide] length];
    }
#pragma clang diagnostic pop
    return 0.0;
}

// From the top: the end-of-life banner (when shown), the field, then the greeting centred in the
// rest of the screen.
- (void)viewDidLayoutSubviews {
    CGRect bounds = self.view.bounds;
    CGFloat width = bounds.size.width - 2.0 * kMargin;
    CGFloat top = [self topInset];
    CGFloat greetingTop;
    CGFloat greetingHeight;
    CGSize textSize;
    CGFloat contentHeight;
    [super viewDidLayoutSubviews];

    if (eolLabel != nil) {
        CGSize bannerSize =
            [eolLabel sizeThatFits:CGSizeMake(bounds.size.width - 2.0 * kBannerPadding, CGFLOAT_MAX)];
        CGFloat bannerHeight = bannerSize.height + 2.0 * kBannerPadding;
        eolLabel.frame = CGRectMake(0.0, top, bounds.size.width, bannerHeight);
        top += bannerHeight;
    }
    top += kMargin;
    inputField.frame = CGRectMake(kMargin, top, width, kFieldHeight);
    greetingTop = top + kFieldHeight + kMargin;
    greetingHeight = bounds.size.height - greetingTop;
    if (greetingHeight < 0.0) {
        greetingHeight = 0.0;
    }
    greetingScroll.frame = CGRectMake(kMargin, greetingTop, width, greetingHeight);
    textSize = [greetingLabel sizeThatFits:CGSizeMake(width, CGFLOAT_MAX)];
    contentHeight = textSize.height > greetingHeight ? textSize.height : greetingHeight;
    greetingLabel.frame = CGRectMake(0.0, (contentHeight - textSize.height) / 2.0, width,
                                     textSize.height);
    greetingScroll.contentSize = CGSizeMake(width, contentHeight);
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [Probe beginWithTarget:self action:@selector(probeFire)];
}

// Probe mode, 2 seconds after the first display: emits the line built from what the screen holds
// now and exits.
- (void)probeFire {
    int status = [Probe emitInput:(inputField.text != nil ? inputField.text : @"")
                          message:(greetingLabel.text != nil ? greetingLabel.text : @"")
                         eolShown:(eolLabel != nil)];
    exit(status);
}

@end
