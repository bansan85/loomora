// Probe mode of the macOS Legacy UI: see Probe.h.
#import "Probe.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

#include "loomora.h"

static uint64_t gLaunchTicks = 0;
static uint64_t gDisplayedTicks = 0;
static BOOL gDisplayed = NO;

// Strict UTF-8 check: no overlong form, no surrogate, nothing above U+10FFFF, no truncated tail.
static BOOL IsValidUtf8(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p != 0) {
        unsigned char c = *p;
        int extra;
        unsigned long code;
        if (c < 0x80) {
            p++;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
            code = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            code = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            code = c & 0x07;
        } else {
            return NO;
        }
        p++;
        for (int i = 0; i < extra; i++, p++) {
            if ((*p & 0xC0) != 0x80)
                return NO;
            code = (code << 6) | (*p & 0x3F);
        }
        if ((extra == 2 && code < 0x800) || (extra == 3 && code < 0x10000) ||
            (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
            return NO;
    }
    return YES;
}

// Milliseconds between two mach_absolute_time values.
static uint32_t MillisecondsBetween(uint64_t from, uint64_t to) {
    mach_timebase_info_data_t base;
    if (to < from || mach_timebase_info(&base) != KERN_SUCCESS || base.denom == 0)
        return 0;
    // Nanoseconds as a double: no overflow of ticks * numer for a few seconds of uptime.
    double nanoseconds = (double)(to - from) * (double)base.numer / (double)base.denom;
    double ms = nanoseconds / 1000000.0;
    return ms >= 4294967295.0 ? 4294967295U : (uint32_t)ms;
}

// Resident size of the process in kB from task_info, or -1 when it cannot be read.
// TASK_BASIC_INFO_64 exists on 10.6; MACH_TASK_BASIC_INFO (10.8 and later) is only the fallback.
static int64_t ResidentSizeKb(void) {
    task_basic_info_64_data_t info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_64_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO_64, (task_info_t)&info, &count) ==
        KERN_SUCCESS)
        return (int64_t)(info.resident_size / 1024);
#ifdef MACH_TASK_BASIC_INFO
    mach_task_basic_info_data_t basic;
    count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&basic, &count) ==
        KERN_SUCCESS)
        return (int64_t)(basic.resident_size / 1024);
#endif
    return -1;
}

// Architecture for the platform id. A machine that reports arm64 runs this x86_64 slice under
// Rosetta, as for the Modern UI. Otherwise it is the slice itself: i386 for the 32-bit slice
// (like the x86 builds of the other Legacy UIs), x86_64 for the 64-bit one.
static const char *ArchitectureName(void) {
    int isArm64 = 0;
    size_t size = sizeof(isArm64);
    if (sysctlbyname("hw.optional.arm64", &isArm64, &size, NULL, 0) == 0 && isArm64 == 1)
        return "arm64";
#if defined(__i386__)
    return "i386";
#else
    return "x86_64";
#endif
}

void LMProbeStart(void) { gLaunchTicks = mach_absolute_time(); }

LMProbeOptions LMProbeParseArgs(int argc, char **argv) {
    LMProbeOptions options;
    options.enabled = NO;
    options.input = NULL;
    options.badUsage = NO;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0) {
            options.enabled = YES;
        } else if (strcmp(argv[i], "--probe-input") == 0) {
            if (i + 1 < argc && IsValidUtf8(argv[i + 1]))
                options.input = argv[++i];
            else
                options.badUsage = YES;
        }
    }
    // The input override is only meaningful in probe mode.
    if (options.input != NULL && !options.enabled)
        options.badUsage = YES;
    return options;
}

void LMProbeMarkDisplayed(void) {
    if (!gDisplayed) {
        gDisplayedTicks = mach_absolute_time();
        gDisplayed = YES;
    }
}

// SystemVersion.plist is read instead of Gestalt (deprecated) or NSProcessInfo (10.10 and later).
// Real version only when the program is linked with the macOS 11 SDK or later (the macos-26
// build is); older SDKs are told 10.16.
BOOL LMSystemVersion(int32_t *major, int32_t *minor) {
    NSDictionary *plist = [NSDictionary
        dictionaryWithContentsOfFile:@"/System/Library/CoreServices/SystemVersion.plist"];
    NSString *version = [plist objectForKey:@"ProductVersion"];
    int parsedMajor = 0;
    int parsedMinor = 0;
    if (![version isKindOfClass:[NSString class]])
        return NO;
    int fields = sscanf([version UTF8String], "%d.%d", &parsedMajor, &parsedMinor);
    if (fields < 1)
        return NO;
    *major = parsedMajor;
    *minor = fields >= 2 ? parsedMinor : 0;
    return YES;
}

// "macos-<version>-<arch>", for example macos-10.6-x86_64; matches ^[a-z0-9][a-z0-9._-]*$.
static void PlatformId(char *out, size_t size) {
    int32_t major = 0;
    int32_t minor = 0;
    const char *arch = ArchitectureName();
    if (!LMSystemVersion(&major, &minor))
        snprintf(out, size, "macos-unknown-%s", arch);
    else if (major >= 11)
        snprintf(out, size, "macos-%d-%s", (int)major, arch);
    else
        snprintf(out, size, "macos-%d.%d-%s", (int)major, (int)minor, arch);
}

// "YYYY-MM-DDTHH:MM:SSZ", the current time in UTC.
static void TimestampUtc(char *out, size_t size) {
    time_t now = time(NULL);
    struct tm utc;
    out[0] = '\0';
    if (gmtime_r(&now, &utc) != NULL)
        strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

int LMProbeEmit(const char *input, const char *message, BOOL eolWarningShown) {
    char platformId[64];
    char timestamp[32];
    PlatformId(platformId, sizeof(platformId));
    TimestampUtc(timestamp, sizeof(timestamp));
    uint32_t displayMs = gDisplayed ? MillisecondsBetween(gLaunchTicks, gDisplayedTicks) : 0;
    int64_t rssKb = ResidentSizeKb();

    // First call with no room measures, second one fills the buffer (the line is ASCII).
    size_t needed = loomora_probe_record(platformId, LOOMORA_UI_LEGACY, LOOMORA_CORE_NATIVE, input,
                                         message, gDisplayed ? 1 : 0, displayMs, rssKb,
                                         eolWarningShown ? 1 : 0, "", timestamp, NULL, 0);
    size_t written = 0;
    NSMutableData *line = nil;
    if (needed > 0) {
        line = [NSMutableData dataWithLength:needed];
        written = loomora_probe_record(platformId, LOOMORA_UI_LEGACY, LOOMORA_CORE_NATIVE, input,
                                       message, gDisplayed ? 1 : 0, displayMs, rssKb,
                                       eolWarningShown ? 1 : 0, "", timestamp,
                                       (uint8_t *)[line mutableBytes], needed);
    }
    if (needed > 0 && written == needed) {
        fwrite([line bytes], 1, needed, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        return 0;
    }
    fprintf(stderr, "loomora: the backend could not build the probe record\n");
    return 1;
}
