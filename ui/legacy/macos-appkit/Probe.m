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
// TASK_BASIC_INFO_64 exists on 10.6; MACH_TASK_BASIC_INFO (10.8 and later) is only the fallback, and
// it is a plain constant, so naming it creates no link-time dependency (the #ifdef covers an SDK
// that does not define it). Each call is checked: a return code other than KERN_SUCCESS, a count
// shorter than the structure, or a resident size of zero (no real process has none) makes the
// answer unusable and the next source is tried, then -1. This is written for x86_64 under Rosetta
// as well, but it has never run there: no Mac was available.
static int64_t ResidentSizeKb(void) {
    task_basic_info_64_data_t info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_64_COUNT;
    memset(&info, 0, sizeof(info));
    if (task_info(mach_task_self(), TASK_BASIC_INFO_64, (task_info_t)&info, &count) ==
            KERN_SUCCESS &&
        count >= TASK_BASIC_INFO_64_COUNT && info.resident_size > 0)
        return (int64_t)(info.resident_size / 1024);
#ifdef MACH_TASK_BASIC_INFO
    mach_task_basic_info_data_t basic;
    count = MACH_TASK_BASIC_INFO_COUNT;
    memset(&basic, 0, sizeof(basic));
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&basic, &count) ==
            KERN_SUCCESS &&
        count >= MACH_TASK_BASIC_INFO_COUNT && basic.resident_size > 0)
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
    options.projectMode = LMProjectModeNone;
    options.projectPath = NULL;
    options.badUsage = NO;
    int projectOptions = 0; // --validate-project and --roundtrip-project given, with their repeats
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0) {
            options.enabled = YES;
        } else if (strcmp(argv[i], "--validate-project") == 0 ||
                   strcmp(argv[i], "--roundtrip-project") == 0) {
            projectOptions++;
            options.projectMode = strcmp(argv[i], "--validate-project") == 0
                                      ? LMProjectModeValidate
                                      : LMProjectModeRoundtrip;
            if (i + 1 < argc && IsValidUtf8(argv[i + 1]))
                options.projectPath = argv[++i];
            else
                options.badUsage = YES;
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
    // The three modes exclude each other.
    if (projectOptions > 1 || (projectOptions == 1 && (options.enabled || options.input != NULL)))
        options.badUsage = YES;
    return options;
}

void LMProbeMarkDisplayed(void) {
    if (!gDisplayed) {
        gDisplayedTicks = mach_absolute_time();
        gDisplayed = YES;
    }
}

// "major.minor[.patch]" to major and minor (minor 0 when absent). NO when it is not a version.
static BOOL ParseVersion(const char *text, int32_t *major, int32_t *minor) {
    int parsedMajor = 0;
    int parsedMinor = 0;
    if (text == NULL)
        return NO;
    int fields = sscanf(text, "%d.%d", &parsedMajor, &parsedMinor);
    if (fields < 1 || parsedMajor <= 0)
        return NO;
    if (fields < 2)
        parsedMinor = 0;
    // 10.16 is what a program linked with an SDK older than macOS 11 is told on macOS 11 and later;
    // no real macOS 10.16 exists, so it can only be macOS 11.
    if (parsedMajor == 10 && parsedMinor == 16) {
        parsedMajor = 11;
        parsedMinor = 0;
    }
    *major = parsedMajor;
    *minor = parsedMinor;
    return YES;
}

// Version of the running system, without Gestalt (deprecated) or NSProcessInfo (10.10 and later).
// First choice: sysctlbyname("kern.osproductversion") (macOS 10.13.4 and later). It is looked up by
// name at run time, so there is no link-time dependency: on 10.6 to 10.13.3 the call fails and the
// next source is used. SystemVersion.plist is the fallback; it can say 10.16 instead of the real
// version to a program linked with an SDK older than macOS 11 (the macos-26 build links the macOS 26
// SDK, which is told the truth, but the plist is only a fallback). Neither path has run on a Mac.
BOOL LMSystemVersion(int32_t *major, int32_t *minor) {
    char buffer[64];
    size_t size = sizeof(buffer) - 1;
    memset(buffer, 0, sizeof(buffer));
    if (sysctlbyname("kern.osproductversion", buffer, &size, NULL, 0) == 0 &&
        ParseVersion(buffer, major, minor))
        return YES;

    NSDictionary *plist = [NSDictionary
        dictionaryWithContentsOfFile:@"/System/Library/CoreServices/SystemVersion.plist"];
    NSString *version = [plist objectForKey:@"ProductVersion"];
    if (![version isKindOfClass:[NSString class]])
        return NO;
    return ParseVersion([version UTF8String], major, minor);
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

#pragma mark Project probe modes

// The timestamp of the copy: the round trip checks the structure of the copy, not a date, and the UI
// reads no clock and parses no part of the project (FR-027). One fixed valid value.
static const char kCopyModifiedAt[] = "1970-01-01T00:00:00Z";

// A failure that leaves nothing to read (I/O, memory, bad argument): exit status 2, as the command.
static BOOL ProjectUnreadable(int32_t rc) {
    return rc == LOOMORA_ERR_IO || rc == LOOMORA_ERR_NO_MEMORY || rc == LOOMORA_ERR_INVALID ||
           rc == LOOMORA_ERR_INTERNAL || rc == LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM;
}

static NSString *ProjectPlural(long long n, const char *noun) {
    return [NSString stringWithFormat:@"%lld %s%s", n, noun, n == 1 ? "" : "s"];
}

// The summary line of `loomora validate --quiet` for a file: "OK <file>: 0 errors, 0 warnings",
// "FAIL <file>: N errors, M warnings" or "FAIL <file>: error <code>" (the failure left no error in
// the report). *status is 0, 1 or 2 as the command's exit status. The counts come from the report.
static NSString *ProjectSummaryLine(const char *name, int32_t rc, const loomora_report *report,
                                    int *status) {
    long long total = report != NULL ? (long long)loomora_report_count(report) : 0;
    long long errors = report != NULL ? (long long)loomora_report_error_count(report) : 0;
    long long warnings = total - errors;
    BOOL codeOnly = rc != 0 && errors <= 0;
    if (rc != 0 && ProjectUnreadable(rc))
        *status = 2;
    else if (rc != 0 || errors > 0)
        *status = 1;
    else
        *status = 0;
    NSString *detail = codeOnly ? [NSString stringWithFormat:@"error %d", (int)rc]
                                : [NSString stringWithFormat:@"%@, %@", ProjectPlural(errors, "error"),
                                                             ProjectPlural(warnings, "warning")];
    return [NSString stringWithFormat:@"%s %s: %@", *status == 0 ? "OK" : "FAIL", name, detail];
}

// The sink of the export: appends each chunk to the memory buffer (`user` is an NSMutableData).
static int32_t ProjectAppendChunk(void *user, const uint8_t *data, size_t length) {
    [(NSMutableData *)user appendBytes:data length:length];
    return 0;
}

// Round trip of an open project: a full copy of every result into memory, validated again. Returns
// the line to print and sets *status.
static NSString *ProjectRoundTrip(const char *path, const loomora_project *project, int *status) {
    NSMutableData *copy = [NSMutableData data];
    loomora_report *exportReport = NULL;
    int32_t profile =
        loomora_project_is_light(project) == 1 ? LOOMORA_PROFILE_LIGHT : LOOMORA_PROFILE_FULL;
    int32_t rc = loomora_project_export_stream(project, profile, NULL, 0, kCopyModifiedAt, 0,
                                               ProjectAppendChunk, (void *)copy, &exportReport);
    if (exportReport != NULL)
        loomora_report_free(exportReport);
    if (rc != 0) {
        *status = 1;
        return [NSString stringWithFormat:@"roundtrip FAIL export error %d", (int)rc];
    }
    loomora_report *copyReport = NULL;
    int32_t validated = loomora_validate_memory([copy length] > 0 ? (const uint8_t *)[copy bytes] : NULL,
                                                [copy length], NULL, NULL, &copyReport);
    NSString *summary = ProjectSummaryLine(path, validated, copyReport, status);
    if (copyReport != NULL)
        loomora_report_free(copyReport);
    NSString *line = [NSString stringWithFormat:@"%s %@",
                                                *status == 0 ? "roundtrip ok" : "roundtrip FAIL", summary];
    if (*status != 0)
        *status = 1;
    return line;
}

int LMProjectProbeRun(int mode, const char *path) {
    int status = 0;
    NSString *line = nil;
    loomora_project *project = NULL;
    loomora_report *report = NULL;
    int32_t rc = loomora_project_open_path(path, NULL, NULL, &project, &report);
    NSString *summary = ProjectSummaryLine(path, rc, report, &status);
    if (report != NULL)
        loomora_report_free(report);
    if (rc != 0 || mode == LMProjectModeValidate)
        line = summary;
    else
        line = ProjectRoundTrip(path, project, &status);
    if (project != NULL)
        loomora_project_close(project);
    const char *utf8 = [line UTF8String];
    if (utf8 != NULL) {
        fputs(utf8, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    return status;
}
