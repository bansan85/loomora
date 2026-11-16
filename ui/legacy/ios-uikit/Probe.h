// Probe mode of the iOS Legacy UI (ui-contract.md): launch arguments -probe and
// -probe_input <utf8>. The probe line is built by the backend (loomora_probe_record); this class
// only measures, converts and writes it, through NSLog. The app exits 2 seconds after the first
// display. Manual retain/release (no ARC), iOS 5 APIs only.
#import <Foundation/Foundation.h>

// Limit of the input field, in Unicode scalars (at most 4 bytes each: under the 4,096 bytes of the
// backend).
extern const NSUInteger kLoomoraMaxInputScalars;

@interface Probe : NSObject

// Records the start of the launch (the creation of the process when sysctl can tell).
+ (void)start;

// YES when -probe is among the launch arguments.
+ (BOOL)enabled;

// The text given with -probe_input, only in probe mode; nil otherwise.
+ (NSString *)inputOverride;

// At most kLoomoraMaxInputScalars Unicode scalars, never cutting a surrogate pair in two.
+ (NSString *)limited:(NSString *)text;

// Major and minor number of the system version ("5.1.1" gives 5 and 1).
+ (void)getSystemMajor:(int *)major minor:(int *)minor;

// Records the first display; further calls are ignored.
+ (void)markDisplayed;

// Probe mode: once the screen is shown, calls [target action] after 2 seconds (the target then
// calls +emitInput:message:eolShown: and exits). Does nothing outside probe mode or when already
// started.
+ (void)beginWithTarget:(id)target action:(SEL)action;

// Writes the probe line with NSLog; returns the exit status.
+ (int)emitInput:(NSString *)input message:(NSString *)message eolShown:(BOOL)eolShown;

@end
