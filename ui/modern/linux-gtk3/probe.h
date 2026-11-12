/* Probe mode of the Linux Modern UI (ui-contract.md): --probe, --probe-input <utf8>.
   Only converts and measures: the JSON line itself comes from loomora_probe_record(). */
#ifndef LOOMORA_GTK3_PROBE_H
#define LOOMORA_GTK3_PROBE_H

#include <gtk/gtk.h>

typedef struct {
    gboolean enabled;   /* --probe was given */
    const char *input;  /* --probe-input value, NULL when not given (points into argv) */
    gboolean bad_usage; /* an option was malformed (e.g. --probe-input without a value) */
} ProbeOptions;

/* Reads --probe and --probe-input <utf8> from argv (GTK options are already removed by
   gtk_init_check). Unknown arguments are left alone. */
void probe_parse_args(int argc, char **argv, ProbeOptions *options);

/* Records the launch time; call it first thing in main(). */
void probe_start(void);

/* Records the first display; call it from the first "draw" of the main window. Further calls
   are ignored. */
void probe_mark_displayed(void);

/* Platform id of the running system, "linux-<os-release ID>-<VERSION_ID>-<machine>" (for
   example "linux-ubuntu-24.04-x86_64"); matches ^[a-z0-9][a-z0-9._-]*$. Never NULL; the
   returned string is owned by the caller (g_free). */
char *probe_platform_id(void);

/* Arms the probe: after 2 seconds (g_timeout_add) the probe line is written to stdout and the
   main loop is left. `entry` and `label` give the input and the displayed greeting at that
   moment. The exit status of the program is stored in *exit_status (0 on success). */
void probe_schedule_exit(GtkEntry *entry, GtkLabel *label, gboolean eol_warning_shown,
                         int *exit_status);

#endif
