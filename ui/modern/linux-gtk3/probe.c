/* Probe mode of the Linux Modern UI: see probe.h. */
#include "probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#include "loomora.h"

#define PROBE_EXIT_DELAY_MS 2000

static gint64 g_launch_us;
static gint64 g_displayed_us; /* 0 until the first draw */

typedef struct {
    GtkEntry *entry;
    GtkLabel *label;
    gboolean eol_warning_shown;
    int *exit_status;
} ProbeContext;

void probe_parse_args(int argc, char **argv, ProbeOptions *options) {
    int i;
    options->enabled = FALSE;
    options->input = NULL;
    options->bad_usage = FALSE;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0) {
            options->enabled = TRUE;
        } else if (strcmp(argv[i], "--probe-input") == 0) {
            if (i + 1 < argc) {
                options->input = argv[++i];
            } else {
                options->bad_usage = TRUE;
            }
        }
    }
    /* The input override is only meaningful in probe mode. */
    if (options->input != NULL && !options->enabled)
        options->bad_usage = TRUE;
}

void probe_start(void) { g_launch_us = g_get_monotonic_time(); }

void probe_mark_displayed(void) {
    if (g_displayed_us == 0)
        g_displayed_us = g_get_monotonic_time();
}

/* Appends `s` as a platform-id component: lowercase, anything outside [a-z0-9._-] becomes '-'. */
static void append_id_part(GString *id, const char *s) {
    for (; *s != '\0'; s++) {
        char c = g_ascii_tolower(*s);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            c = '-';
        g_string_append_c(id, c);
    }
}

/* Value of KEY in the contents of /etc/os-release (quotes removed), or NULL. */
static char *os_release_value(const char *contents, const char *key) {
    char **lines = g_strsplit(contents, "\n", -1);
    size_t key_len = strlen(key);
    char *value = NULL;
    int i;
    for (i = 0; lines[i] != NULL && value == NULL; i++) {
        if (strncmp(lines[i], key, key_len) == 0 && lines[i][key_len] == '=')
            value = g_strdup(lines[i] + key_len + 1);
    }
    g_strfreev(lines);
    if (value != NULL) {
        g_strdelimit(value, "\"'", ' ');
        g_strstrip(value);
    }
    return value;
}

char *probe_platform_id(void) {
    GString *id = g_string_new("linux");
    char *contents = NULL;
    char *os_id = NULL;
    char *os_version = NULL;
    struct utsname un;

    if (g_file_get_contents("/etc/os-release", &contents, NULL, NULL) ||
        g_file_get_contents("/usr/lib/os-release", &contents, NULL, NULL)) {
        os_id = os_release_value(contents, "ID");
        os_version = os_release_value(contents, "VERSION_ID");
    }
    if (os_id != NULL && os_id[0] != '\0') {
        g_string_append_c(id, '-');
        append_id_part(id, os_id);
    }
    if (os_version != NULL && os_version[0] != '\0') {
        g_string_append_c(id, '-');
        append_id_part(id, os_version);
    }
    if (uname(&un) == 0 && un.machine[0] != '\0') {
        g_string_append_c(id, '-');
        append_id_part(id, un.machine);
    }
    g_free(contents);
    g_free(os_id);
    g_free(os_version);
    return g_string_free(id, FALSE);
}

/* Peak resident memory in kB (VmHWM of /proc/self/status), or -1 if it cannot be read. */
static gint64 peak_rss_kb(void) {
    char *contents = NULL;
    gint64 value = -1;
    if (g_file_get_contents("/proc/self/status", &contents, NULL, NULL)) {
        const char *p = strstr(contents, "VmHWM:");
        if (p != NULL)
            value = g_ascii_strtoll(p + strlen("VmHWM:"), NULL, 10);
        g_free(contents);
    }
    return value;
}

static gboolean on_probe_timeout(gpointer data) {
    ProbeContext *ctx = (ProbeContext *)data;
    char *platform_id = probe_platform_id();
    GDateTime *now = g_date_time_new_now_utc();
    char *timestamp = g_date_time_format(now, "%Y-%m-%dT%H:%M:%SZ");
    gboolean displayed = g_displayed_us != 0;
    gint64 elapsed_ms = displayed ? (g_displayed_us - g_launch_us) / 1000 : 0;
    size_t needed;
    size_t written;
    uint8_t *buf;
    const char *input = loomora_entry_get_text(ctx->entry);
    const char *message = gtk_label_get_text(ctx->label);

    /* First call with no room measures, second one fills the buffer (the line is ASCII). */
    needed = loomora_probe_record(platform_id, LOOMORA_UI_MODERN, LOOMORA_CORE_NATIVE, input,
                                  message, displayed, (uint32_t)elapsed_ms, peak_rss_kb(),
                                  ctx->eol_warning_shown, "", timestamp, NULL, 0);
    buf = needed > 0 ? (uint8_t *)g_malloc(needed + 1) : NULL;
    written = 0;
    if (buf != NULL) {
        written = loomora_probe_record(platform_id, LOOMORA_UI_MODERN, LOOMORA_CORE_NATIVE, input,
                                       message, displayed, (uint32_t)elapsed_ms, peak_rss_kb(),
                                       ctx->eol_warning_shown, "", timestamp, buf, needed);
    }
    if (written == needed && needed > 0) {
        fwrite(buf, 1, needed, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    } else {
        fprintf(stderr, "loomora: the backend could not build the probe record\n");
        *ctx->exit_status = 1;
    }
    g_free(buf);
    g_free(timestamp);
    g_date_time_unref(now);
    g_free(platform_id);
    g_free(ctx);
    loomora_main_loop_quit();
    return G_SOURCE_REMOVE;
}

void probe_schedule_exit(GtkEntry *entry, GtkLabel *label, gboolean eol_warning_shown,
                         int *exit_status) {
    ProbeContext *ctx = g_new0(ProbeContext, 1);
    ctx->entry = entry;
    ctx->label = label;
    ctx->eol_warning_shown = eol_warning_shown;
    ctx->exit_status = exit_status;
    g_timeout_add(PROBE_EXIT_DELAY_MS, on_probe_timeout, ctx);
}
