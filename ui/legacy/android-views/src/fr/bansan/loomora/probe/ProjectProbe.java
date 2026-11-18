package fr.bansan.loomora.probe;

import android.content.Intent;
import android.util.Log;

/**
 * Project probe modes of the Android UI (specs/002-loomora-file-format/contracts/ui-validate-project.md):
 * the intent extras "validate_project" and "roundtrip_project", each a path readable by the app (the
 * launcher pushes the file first). No application logic: the file is opened, validated and (round
 * trip) exported and validated again by the backend through Native; this class only builds the one
 * result line from the counts the report gives. The line is the summary line of
 * `loomora validate --quiet`, byte for byte; it is logged with the tag LOOMORA_VALIDATE and nothing
 * else is logged to that tag. The same logic as the project_probe.cpp of the desktop UIs.
 *
 * Java 6 level (Android 2.3, API 9): no lambda, no stream.
 */
final class ProjectProbe {
    static final String TAG = "LOOMORA_VALIDATE";
    static final String EXTRA_VALIDATE = "validate_project";
    static final String EXTRA_ROUNDTRIP = "roundtrip_project";

    /** No project mode was asked for. */
    static final int NONE = 0;
    /** The extra "validate_project". */
    static final int VALIDATE = 1;
    /** The extra "roundtrip_project". */
    static final int ROUNDTRIP = 2;
    /** Usage error (status 2): two modes, a project mode next to "probe", or a missing path. */
    static final int BAD_USAGE = 3;

    // LOOMORA_ERR_* of loomora.h and loomora_project.h, LOOMORA_PROFILE_* of loomora_project.h.
    private static final int ERR_INVALID = -3;
    private static final int ERR_INTERNAL = -4;
    private static final int ERR_IO = -10;
    private static final int ERR_NO_MEMORY = -16;
    private static final int ERR_TOO_LARGE_FOR_PLATFORM = -22;
    private static final int PROFILE_FULL = 1;
    private static final int PROFILE_LIGHT = 2;

    // The timestamp of the copy: the round trip checks the structure of the copy, not a date, and the
    // UI reads no clock and parses no part of the project (FR-027). One fixed valid value.
    private static final String COPY_MODIFIED_AT = "1970-01-01T00:00:00Z";

    private ProjectProbe() {
    }

    /** The result of a run: the line to log (empty when there is nothing to log) and the status 0, 1 or 2. */
    static final class Result {
        String line = "";
        int status = 2;
    }

    /** NONE, VALIDATE, ROUNDTRIP or BAD_USAGE for the launch intent. */
    static int mode(Intent intent) {
        if (intent == null) {
            return NONE;
        }
        boolean validate = intent.hasExtra(EXTRA_VALIDATE);
        boolean roundtrip = intent.hasExtra(EXTRA_ROUNDTRIP);
        if (!validate && !roundtrip) {
            return NONE;
        }
        if ((validate && roundtrip) || Probe.enabled(intent)) {
            return BAD_USAGE;
        }
        String path = intent.getStringExtra(validate ? EXTRA_VALIDATE : EXTRA_ROUNDTRIP);
        if (path == null) {
            return BAD_USAGE; // an extra that is not a string: no path
        }
        return validate ? VALIDATE : ROUNDTRIP;
    }

    /** The path of the mode (as it was given), or null. */
    static String path(Intent intent, int mode) {
        if (intent == null) {
            return null;
        }
        if (mode == VALIDATE) {
            return intent.getStringExtra(EXTRA_VALIDATE);
        }
        if (mode == ROUNDTRIP) {
            return intent.getStringExtra(EXTRA_ROUNDTRIP);
        }
        return null;
    }

    /** Runs the mode on `path` and logs the line (tag LOOMORA_VALIDATE); returns the status. */
    static int runAndLog(int mode, String path) {
        Result result = run(mode, path);
        if (result.line.length() > 0) {
            Log.i(TAG, result.line);
        }
        return result.status;
    }

    /**
     * Runs `mode` on the file `path`. The status is the exit status of the command line: 0 the file
     * is valid (warnings allowed), 1 it has an error or the round trip failed, 2 the file cannot be
     * read at all.
     */
    static Result run(int mode, String path) {
        Result result = new Result();
        long[] opened = Native.projectOpen(path);
        int rc = opened == null ? ERR_INTERNAL : (int) opened[0];
        long project = opened == null ? 0L : opened[1];
        long report = opened == null ? 0L : opened[2];
        int[] status = new int[1];
        String summary = summaryLine(path, rc, report, status);
        if (report != 0L) {
            Native.reportFree(report);
        }
        if (rc != 0 || mode == VALIDATE) {
            if (project != 0L) {
                Native.projectClose(project);
            }
            result.line = summary;
            result.status = status[0];
            return result;
        }
        result.status = roundTrip(path, project, result);
        Native.projectClose(project);
        return result;
    }

    /** A failure that leaves nothing to read (I/O, memory, bad argument): status 2, as the command. */
    private static boolean unreadable(int rc) {
        return rc == ERR_IO || rc == ERR_NO_MEMORY || rc == ERR_INVALID || rc == ERR_INTERNAL
                || rc == ERR_TOO_LARGE_FOR_PLATFORM;
    }

    private static String plural(long n, String noun) {
        return Long.toString(n) + " " + noun + (n == 1 ? "" : "s");
    }

    /**
     * The summary line of `loomora validate --quiet` for a file: "OK name: 0 errors, 0 warnings",
     * "FAIL name: N errors, M warnings" or "FAIL name: error code" (the failure left no error in the
     * report); status[0] is 0, 1 or 2 as the command's exit status. The counts come from the report
     * (handle 0: none).
     */
    private static String summaryLine(String name, int rc, long report, int[] status) {
        long total = report != 0L ? Native.reportCount(report) : 0L;
        long errors = report != 0L ? Native.reportErrorCount(report) : 0L;
        long warnings = total - errors;
        boolean codeOnly = rc != 0 && errors <= 0;
        if (rc != 0 && unreadable(rc)) {
            status[0] = 2;
        } else if (rc != 0 || errors > 0) {
            status[0] = 1;
        } else {
            status[0] = 0;
        }
        return (status[0] == 0 ? "OK " : "FAIL ") + name + ": "
                + (codeOnly ? "error " + rc : plural(errors, "error") + ", " + plural(warnings, "warning"));
    }

    /** The round trip of an open project; sets result.line and returns the status (0 or 1). */
    private static int roundTrip(String path, long project, Result result) {
        int[] code = new int[1];
        int profile = Native.projectIsLight(project) == 1 ? PROFILE_LIGHT : PROFILE_FULL;
        byte[] copy = Native.projectExport(project, profile, COPY_MODIFIED_AT, code);
        if (copy == null) {
            result.line = "roundtrip FAIL export error " + (code[0] != 0 ? code[0] : ERR_INTERNAL);
            return 1;
        }
        long[] validated = Native.validateMemory(copy);
        int rc = validated == null ? ERR_INTERNAL : (int) validated[0];
        long report = validated == null ? 0L : validated[2];
        int[] status = new int[1];
        String summary = summaryLine(path, rc, report, status);
        if (report != 0L) {
            Native.reportFree(report);
        }
        result.line = (status[0] == 0 ? "roundtrip ok " : "roundtrip FAIL ") + summary;
        return status[0] == 0 ? 0 : 1;
    }
}
