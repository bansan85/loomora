package fr.bansan.loomora.probe

import android.content.Intent
import android.util.Log

/**
 * Project probe modes of the Android UI (specs/002-loomora-file-format/contracts/ui-validate-project.md):
 * the intent extras "validate_project" and "roundtrip_project", each a path readable by the app (the
 * launcher pushes the file first). No application logic: the file is opened, validated and (round
 * trip) exported and validated again by the backend through Native; this object only builds the one
 * result line from the counts the report gives. The line is the summary line of
 * `loomora validate --quiet`, byte for byte; it is logged with the tag LOOMORA_VALIDATE and nothing
 * else is logged to that tag. The same logic as ProjectProbe.java of the Legacy UI and the
 * project_probe.cpp of the desktop UIs.
 */
internal object ProjectProbe {
    const val TAG = "LOOMORA_VALIDATE"
    const val EXTRA_VALIDATE = "validate_project"
    const val EXTRA_ROUNDTRIP = "roundtrip_project"

    /** What the launch intent asks for. */
    enum class Mode { NONE, VALIDATE, ROUNDTRIP, BAD_USAGE }

    /** The result of a run: the line to log (empty when there is nothing to log) and the status 0, 1 or 2. */
    class Result(val line: String, val status: Int)

    // LOOMORA_ERR_* of loomora.h and loomora_project.h, LOOMORA_PROFILE_* of loomora_project.h.
    private const val ERR_INVALID = -3
    private const val ERR_INTERNAL = -4
    private const val ERR_IO = -10
    private const val ERR_NO_MEMORY = -16
    private const val ERR_TOO_LARGE_FOR_PLATFORM = -22
    private const val PROFILE_FULL = 1
    private const val PROFILE_LIGHT = 2

    // The timestamp of the copy: the round trip checks the structure of the copy, not a date, and the
    // UI reads no clock and parses no part of the project (FR-027). One fixed valid value.
    private const val COPY_MODIFIED_AT = "1970-01-01T00:00:00Z"

    /** The mode of the launch intent: a usage error is two modes, a mode next to "probe", or no path. */
    fun mode(intent: Intent?): Mode {
        if (intent == null) return Mode.NONE
        val validate = intent.hasExtra(EXTRA_VALIDATE)
        val roundtrip = intent.hasExtra(EXTRA_ROUNDTRIP)
        if (!validate && !roundtrip) return Mode.NONE
        if ((validate && roundtrip) || Probe.enabled(intent)) return Mode.BAD_USAGE
        // an extra that is not a string: no path
        if (intent.getStringExtra(if (validate) EXTRA_VALIDATE else EXTRA_ROUNDTRIP) == null) return Mode.BAD_USAGE
        return if (validate) Mode.VALIDATE else Mode.ROUNDTRIP
    }

    /** The path of the mode, as it was given, or null. */
    fun path(intent: Intent?, mode: Mode): String? = when (mode) {
        Mode.VALIDATE -> intent?.getStringExtra(EXTRA_VALIDATE)
        Mode.ROUNDTRIP -> intent?.getStringExtra(EXTRA_ROUNDTRIP)
        else -> null
    }

    /** Runs the mode on `path` and logs the line (tag LOOMORA_VALIDATE); returns the status. */
    fun runAndLog(mode: Mode, path: String): Int {
        val result = run(mode, path)
        if (result.line.isNotEmpty()) Log.i(TAG, result.line)
        return result.status
    }

    /**
     * Runs `mode` on the file `path`. The status is the exit status of the command line: 0 the file is
     * valid (warnings allowed), 1 it has an error or the round trip failed, 2 the file cannot be read
     * at all.
     */
    fun run(mode: Mode, path: String): Result {
        val opened = Native.projectOpen(path)
        val rc = if (opened == null) ERR_INTERNAL else opened[0].toInt()
        val project = if (opened == null) 0L else opened[1]
        val report = if (opened == null) 0L else opened[2]
        val summary = summaryLine(path, rc, report)
        if (report != 0L) Native.reportFree(report)
        if (rc != 0 || mode == Mode.VALIDATE) {
            if (project != 0L) Native.projectClose(project)
            return summary
        }
        val result = roundTrip(path, project)
        Native.projectClose(project)
        return result
    }

    /** A failure that leaves nothing to read (I/O, memory, bad argument): status 2, as the command. */
    private fun unreadable(rc: Int): Boolean =
        rc == ERR_IO || rc == ERR_NO_MEMORY || rc == ERR_INVALID || rc == ERR_INTERNAL ||
            rc == ERR_TOO_LARGE_FOR_PLATFORM

    private fun plural(n: Long, noun: String): String = "$n $noun" + (if (n == 1L) "" else "s")

    /**
     * The summary line of `loomora validate --quiet` for a file: "OK name: 0 errors, 0 warnings",
     * "FAIL name: N errors, M warnings" or "FAIL name: error code" (the failure left no error in the
     * report); the status is 0, 1 or 2 as the command's exit status. The counts come from the report
     * (handle 0: none).
     */
    private fun summaryLine(name: String, rc: Int, report: Long): Result {
        val total = if (report != 0L) Native.reportCount(report) else 0L
        val errors = if (report != 0L) Native.reportErrorCount(report) else 0L
        val warnings = total - errors
        val codeOnly = rc != 0 && errors <= 0
        val status = when {
            rc != 0 && unreadable(rc) -> 2
            rc != 0 || errors > 0 -> 1
            else -> 0
        }
        val counts = if (codeOnly) "error $rc" else plural(errors, "error") + ", " + plural(warnings, "warning")
        return Result((if (status == 0) "OK " else "FAIL ") + name + ": " + counts, status)
    }

    /** The round trip of an open project. */
    private fun roundTrip(path: String, project: Long): Result {
        val code = IntArray(1)
        val profile = if (Native.projectIsLight(project) == 1) PROFILE_LIGHT else PROFILE_FULL
        val copy = Native.projectExport(project, profile, COPY_MODIFIED_AT, code)
            ?: return Result("roundtrip FAIL export error " + (if (code[0] != 0) code[0] else ERR_INTERNAL), 1)
        val validated = Native.validateMemory(copy)
        val rc = if (validated == null) ERR_INTERNAL else validated[0].toInt()
        val report = if (validated == null) 0L else validated[2]
        val summary = summaryLine(path, rc, report)
        if (report != 0L) Native.reportFree(report)
        val ok = summary.status == 0
        return Result((if (ok) "roundtrip ok " else "roundtrip FAIL ") + summary.line, if (ok) 0 else 1)
    }
}
