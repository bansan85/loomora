/* Probe mode of the Linux Legacy UI (ui-contract.md): --probe, --probe-input <utf8>.
   Only converts and measures: the JSON line itself comes from loomora_probe_record().
   Plain C, valid as C89; no function newer than glibc 2.12 (gettimeofday, not clock_gettime,
   which lives in librt before glibc 2.17). */
#ifndef LOOMORA_XLIB_PROBE_H
#define LOOMORA_XLIB_PROBE_H

typedef struct {
    int enabled;        /* --probe was given */
    const char *input;  /* --probe-input value, NULL when not given (points into argv) */
    int bad_usage;      /* an option was malformed (e.g. --probe-input without a value) */
} ProbeOptions;

/* Reads --probe and --probe-input <utf8> from argv. Unknown arguments are left alone. */
void probe_parse_args(int argc, char **argv, ProbeOptions *options);

/* Records the launch time; call it first thing in main(). */
void probe_start(void);

/* Records the first display (the first Expose of the window drawn and flushed). Further calls
   are ignored. */
void probe_mark_displayed(void);

/* Arms the exit timer: 2 seconds from now. */
void probe_arm(void);

/* Milliseconds left before the probe must be emitted (0 when it is due), or -1 when the probe
   is not armed. */
long probe_ms_left(void);

/* Platform id of the running system, "linux-<os-release ID>-<VERSION_ID>-<machine>" (for
   example "linux-ubuntu-24.04-x86_64"); matches ^[a-z0-9][a-z0-9._-]*$. Written into `buf`
   (always NUL terminated). */
void probe_platform_id(char *buf, unsigned long cap);

/* Writes the probe line to stdout (one line, flushed). `input` and `message` are the content of
   the field and the greeting displayed at that moment (UTF-8); `disabled_features` is a
   comma-separated list ("" if none). Returns 0 on success, 1 when the backend could not build
   the record. */
int probe_emit(const char *input, const char *message, int eol_warning_shown,
               const char *disabled_features);

#endif
