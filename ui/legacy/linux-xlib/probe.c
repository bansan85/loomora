/* Probe mode of the Linux Legacy UI: see probe.h. */
#include "probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>

#include "loomora.h"

#define PROBE_EXIT_DELAY_MS 2000L

static struct timeval g_launch;
static struct timeval g_displayed;
static int g_displayed_set = 0;
static struct timeval g_deadline;
static int g_armed = 0;

void probe_parse_args(int argc, char **argv, ProbeOptions *options) {
    int i;
    options->enabled = 0;
    options->input = NULL;
    options->bad_usage = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0) {
            options->enabled = 1;
        } else if (strcmp(argv[i], "--probe-input") == 0) {
            if (i + 1 < argc) {
                options->input = argv[++i];
            } else {
                options->bad_usage = 1;
            }
        }
    }
    /* The input override is only meaningful in probe mode. */
    if (options->input != NULL && !options->enabled)
        options->bad_usage = 1;
}

void probe_start(void) { gettimeofday(&g_launch, NULL); }

void probe_mark_displayed(void) {
    if (!g_displayed_set) {
        gettimeofday(&g_displayed, NULL);
        g_displayed_set = 1;
    }
}

/* a - b in milliseconds. */
static long diff_ms(const struct timeval *a, const struct timeval *b) {
    return (long)(a->tv_sec - b->tv_sec) * 1000L + (long)(a->tv_usec - b->tv_usec) / 1000L;
}

void probe_arm(void) {
    gettimeofday(&g_deadline, NULL);
    g_deadline.tv_sec += PROBE_EXIT_DELAY_MS / 1000L;
    g_deadline.tv_usec += (PROBE_EXIT_DELAY_MS % 1000L) * 1000L;
    if (g_deadline.tv_usec >= 1000000L) {
        g_deadline.tv_sec += 1;
        g_deadline.tv_usec -= 1000000L;
    }
    g_armed = 1;
}

long probe_ms_left(void) {
    struct timeval now;
    long left;
    if (!g_armed)
        return -1;
    gettimeofday(&now, NULL);
    left = diff_ms(&g_deadline, &now);
    return left > 0 ? left : 0;
}

/* Appends `s` (at most `n` bytes) to `id` as a platform-id component: lowercase, anything
   outside [a-z0-9._-] becomes '-'. */
static void append_id_part(char *id, unsigned long cap, const char *s) {
    unsigned long len = strlen(id);
    for (; *s != '\0' && len + 1 < cap; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            c = '-';
        id[len++] = c;
    }
    id[len] = '\0';
}

/* Value of KEY in the file `path` (a /etc/os-release style file, quotes removed) into `value`;
   returns 1 when found. */
static int os_release_value(const char *path, const char *key, char *value, unsigned long cap) {
    FILE *f = fopen(path, "r");
    char line[256];
    unsigned long key_len = strlen(key);
    int found = 0;
    if (f == NULL)
        return 0;
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            const char *p = line + key_len + 1;
            unsigned long n = 0;
            while (*p != '\0' && *p != '\n' && n + 1 < cap) {
                if (*p != '"' && *p != '\'' && *p != ' ')
                    value[n++] = *p;
                p++;
            }
            value[n] = '\0';
            found = 1;
        }
    }
    fclose(f);
    return found;
}

void probe_platform_id(char *buf, unsigned long cap) {
    char os_id[64];
    char os_version[64];
    struct utsname un;
    int have_id = 0;
    int have_version = 0;
    if (cap == 0)
        return;
    buf[0] = '\0';
    append_id_part(buf, cap, "linux");
    if (os_release_value("/etc/os-release", "ID", os_id, sizeof(os_id)) ||
        os_release_value("/usr/lib/os-release", "ID", os_id, sizeof(os_id))) {
        have_id = os_id[0] != '\0';
        if (os_release_value("/etc/os-release", "VERSION_ID", os_version, sizeof(os_version)) ||
            os_release_value("/usr/lib/os-release", "VERSION_ID", os_version,
                             sizeof(os_version)))
            have_version = os_version[0] != '\0';
    }
    if (have_id) {
        append_id_part(buf, cap, "-");
        append_id_part(buf, cap, os_id);
    }
    if (have_version) {
        append_id_part(buf, cap, "-");
        append_id_part(buf, cap, os_version);
    }
    if (uname(&un) == 0 && un.machine[0] != '\0') {
        append_id_part(buf, cap, "-");
        append_id_part(buf, cap, un.machine);
    }
}

/* Peak resident memory in kB (VmHWM of /proc/self/status), or -1 if it cannot be read. */
static long peak_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long value = -1;
    if (f == NULL)
        return -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            value = strtol(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return value;
}

int probe_emit(const char *input, const char *message, int eol_warning_shown,
               const char *disabled_features) {
    char platform_id[160];
    char timestamp[32];
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    int displayed = g_displayed_set;
    long elapsed_ms = displayed ? diff_ms(&g_displayed, &g_launch) : 0;
    long rss = peak_rss_kb();
    size_t needed;
    size_t written = 0;
    uint8_t *buf;
    int status = 1;

    probe_platform_id(platform_id, sizeof(platform_id));
    timestamp[0] = '\0';
    if (utc != NULL)
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", utc);
    if (elapsed_ms < 0)
        elapsed_ms = 0;

    /* First call with no room measures, second one fills the buffer (the line is ASCII). */
    needed = loomora_probe_record(platform_id, LOOMORA_UI_LEGACY, LOOMORA_CORE_NATIVE, input,
                                  message, displayed, (uint32_t)elapsed_ms, (int64_t)rss,
                                  eol_warning_shown, disabled_features, timestamp, NULL, 0);
    buf = needed > 0 ? (uint8_t *)malloc(needed + 1) : NULL;
    if (buf != NULL) {
        written = loomora_probe_record(platform_id, LOOMORA_UI_LEGACY, LOOMORA_CORE_NATIVE,
                                       input, message, displayed, (uint32_t)elapsed_ms,
                                       (int64_t)rss, eol_warning_shown, disabled_features,
                                       timestamp, buf, needed);
    }
    if (buf != NULL && written == needed) {
        fwrite(buf, 1, needed, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        status = 0;
    } else {
        fprintf(stderr, "loomora: the backend could not build the probe record\n");
    }
    free(buf);
    return status;
}
