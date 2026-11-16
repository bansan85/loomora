// Probe mode of the iOS Legacy UI: see Probe.h.
#import "Probe.h"

#import <TargetConditionals.h>
#import <UIKit/UIKit.h>

#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "loomora.h"

const NSUInteger kLoomoraMaxInputScalars = 1000;

static const NSTimeInterval kExitDelaySeconds = 2.0;

// Owned for the whole life of the process.
static NSDate *sLaunchDate = nil;
static NSDate *sDisplayedDate = nil;
static BOOL sStarted = NO;

// Start time of this process (sysctl), nil if it cannot be read.
static NSDate *ProcessStartDate(void) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid()};
    struct kinfo_proc info;
    size_t size = sizeof(info);
    struct timeval created;
    memset(&info, 0, sizeof(info));
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0) {
        return nil;
    }
    created = info.kp_proc.p_un.__p_starttime;
    return [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)created.tv_sec +
                                                 (NSTimeInterval)created.tv_usec / 1000000.0];
}

// Resident size in kB (task_info), -1 if it cannot be read. MACH_TASK_BASIC_INFO is not known to
// iOS 5 (the call then fails at run time and the older TASK_BASIC_INFO is used).
static int64_t ResidentKb(void) {
#ifdef MACH_TASK_BASIC_INFO
    mach_task_basic_info_data_t basic;
    mach_msg_type_number_t basicCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&basic, &basicCount) ==
        KERN_SUCCESS) {
        return (int64_t)(basic.resident_size / 1024);
    }
#endif
    {
        task_basic_info_data_t legacy;
        mach_msg_type_number_t legacyCount = TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&legacy, &legacyCount) ==
            KERN_SUCCESS) {
            return (int64_t)(legacy.resident_size / 1024);
        }
    }
    return -1;
}

// "ios-<major>.<minor>-<arch>", with "-simulator" on the simulator.
static NSString *PlatformId(void) {
    int major = 0;
    int minor = 0;
    NSMutableString *identifier;
    [Probe getSystemMajor:&major minor:&minor];
    identifier = [NSMutableString stringWithFormat:@"ios-%d.%d", major, minor];
#if defined(__arm64__)
    [identifier appendString:@"-arm64"];
#elif defined(__ARM_ARCH_7S__)
    [identifier appendString:@"-armv7s"];
#elif defined(__arm__)
    [identifier appendString:@"-armv7"];
#elif defined(__x86_64__)
    [identifier appendString:@"-x86_64"];
#elif defined(__i386__)
    [identifier appendString:@"-i386"];
#else
    [identifier appendString:@"-unknown"];
#endif
#if TARGET_IPHONE_SIMULATOR
    [identifier appendString:@"-simulator"];
#endif
    return identifier;
}

// "YYYY-MM-DDTHH:MM:SSZ" in UTC, whatever the locale and the calendar of the user.
static NSString *TimestampUtc(void) {
    NSDateFormatter *formatter = [[[NSDateFormatter alloc] init] autorelease];
    NSLocale *locale = [[[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"] autorelease];
    [formatter setLocale:locale];
    [formatter setTimeZone:[NSTimeZone timeZoneForSecondsFromGMT:0]];
    [formatter setDateFormat:@"yyyy-MM-dd'T'HH:mm:ss'Z'"];
    return [formatter stringFromDate:[NSDate date]];
}

static uint32_t ElapsedMs(void) {
    NSTimeInterval ms;
    if (sDisplayedDate == nil || sLaunchDate == nil) {
        return 0;
    }
    ms = [sDisplayedDate timeIntervalSinceDate:sLaunchDate] * 1000.0;
    return ms > 0 ? (uint32_t)ms : 0;
}

@implementation Probe

+ (void)start {
    NSDate *date = ProcessStartDate();
    if (date == nil) {
        date = [NSDate date];
    }
    [sLaunchDate release];
    sLaunchDate = [date retain];
}

+ (BOOL)enabled {
    return [[[NSProcessInfo processInfo] arguments] containsObject:@"-probe"];
}

// The argument list is read directly: NSUserDefaults would parse a value such as "(a, b)" as a
// property list.
+ (NSString *)inputOverride {
    NSArray *arguments;
    NSUInteger index;
    if (![self enabled]) {
        return nil;
    }
    arguments = [[NSProcessInfo processInfo] arguments];
    index = [arguments indexOfObject:@"-probe_input"];
    if (index != NSNotFound) {
        return index + 1 < [arguments count] ? [arguments objectAtIndex:index + 1] : @"";
    }
    return nil;
}

+ (NSString *)limited:(NSString *)text {
    NSUInteger length = [text length];
    NSUInteger index = 0;
    NSUInteger scalars = 0;
    while (index < length && scalars < kLoomoraMaxInputScalars) {
        unichar unit = [text characterAtIndex:index];
        if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < length) {
            unichar next = [text characterAtIndex:index + 1];
            if (next >= 0xDC00 && next <= 0xDFFF) {
                index++;
            }
        }
        index++;
        scalars++;
    }
    if (index >= length) {
        return text;
    }
    return [text substringToIndex:index];
}

+ (void)getSystemMajor:(int *)major minor:(int *)minor {
    int parsedMajor = 0;
    int parsedMinor = 0;
    const char *version = [[[UIDevice currentDevice] systemVersion] UTF8String];
    if (version != NULL) {
        if (sscanf(version, "%d.%d", &parsedMajor, &parsedMinor) < 1) {
            parsedMajor = 0;
            parsedMinor = 0;
        }
    }
    *major = parsedMajor;
    *minor = parsedMinor;
}

+ (void)markDisplayed {
    if (sDisplayedDate == nil) {
        sDisplayedDate = [[NSDate alloc] init];
    }
}

+ (void)beginWithTarget:(id)target action:(SEL)action {
    [self markDisplayed];
    if (![self enabled] || sStarted) {
        return;
    }
    sStarted = YES;
    [target performSelector:action withObject:nil afterDelay:kExitDelaySeconds];
}

+ (int)emitInput:(NSString *)input message:(NSString *)message eolShown:(BOOL)eolShown {
    const char *inputUtf8 = [input UTF8String];
    const char *messageUtf8 = [message UTF8String];
    const char *platformUtf8 = [PlatformId() UTF8String];
    const char *timestampUtf8 = [TimestampUtc() UTF8String];
    uint32_t displayMs = ElapsedMs();
    int64_t rssKb = ResidentKb();
    int32_t displayed = sDisplayedDate != nil ? 1 : 0;
    int32_t eol = eolShown ? 1 : 0;
    size_t needed;
    size_t written;
    NSMutableData *buffer;
    NSString *line;

    if (inputUtf8 == NULL || messageUtf8 == NULL) {
        NSLog(@"loomora: the text cannot be converted to UTF-8");
        return 1;
    }
    // First call with no room measures, second one fills the buffer (the line is ASCII).
    needed = loomora_probe_record(platformUtf8, LOOMORA_UI_LEGACY, LOOMORA_CORE_NATIVE, inputUtf8,
                                  messageUtf8, displayed, displayMs, rssKb, eol, "", timestampUtf8,
                                  NULL, 0);
    if (needed == 0) {
        NSLog(@"loomora: the backend could not build the probe record");
        return 1;
    }
    buffer = [NSMutableData dataWithLength:needed];
    written = loomora_probe_record(platformUtf8, LOOMORA_UI_LEGACY, LOOMORA_CORE_NATIVE, inputUtf8,
                                   messageUtf8, displayed, displayMs, rssKb, eol, "",
                                   timestampUtf8, (uint8_t *)[buffer mutableBytes], needed);
    if (written != needed) {
        NSLog(@"loomora: the backend could not build the probe record");
        return 1;
    }
    line = [[[NSString alloc] initWithBytes:[buffer bytes]
                                     length:needed
                                   encoding:NSASCIIStringEncoding] autorelease];
    if (line == nil) {
        NSLog(@"loomora: the probe record is not ASCII");
        return 1;
    }
    NSLog(@"%@", line);
    return 0;
}

@end
