// Project probe modes of the Windows Modern UI (docs/implementation/specs/002-loomora-file-format/contracts/
// ui-validate-project.md): --validate-project <path> and --roundtrip-project <path>. No application
// logic: the file is opened, validated and (round trip) exported and validated again by the backend
// through the C API (Native.cs); this file only builds the one result line from the counts the
// report gives. The line is the summary line of `loomora validate --quiet`, byte for byte. It is
// the C# counterpart of project_probe.cpp of the Windows Legacy UI, with the same statuses.
using System;
using System.Collections.Generic;
using System.Globalization;

namespace Loomora.WinUI;

internal static unsafe class ProjectProbe
{
    internal const int Validate = 1; // --validate-project
    internal const int Roundtrip = 2; // --roundtrip-project

    // The timestamp of the copy: the round trip checks the structure of the copy, not a date, and
    // the UI reads no clock and parses no part of the project (FR-027). One fixed valid value.
    private const string CopyModifiedAt = "1970-01-01T00:00:00Z";

    private static string Plural(long n, string noun) =>
        n.ToString(CultureInfo.InvariantCulture) + " " + noun + (n == 1 ? "" : "s");

    // A failure that leaves nothing to read (I/O, memory, bad argument): exit status 2, as the command.
    private static bool Unreadable(int rc) =>
        rc == Native.ErrIo || rc == Native.ErrNoMemory || rc == Native.ErrInvalid ||
        rc == Native.ErrInternal || rc == Native.ErrTooLargeForPlatform;

    // The summary line of `loomora validate --quiet` for a file: "OK <file>: 0 errors, 0 warnings",
    // "FAIL <file>: N errors, M warnings" or "FAIL <file>: error <code>" (the failure left no error
    // in the report); `status` is 0, 1 or 2 as the command's exit status. The counts come from the report.
    private static string SummaryLine(string name, int rc, nint report, out int status)
    {
        long total = report != 0 ? Native.ReportCount(report) : 0;
        long errors = report != 0 ? Native.ReportErrorCount(report) : 0;
        long warnings = total - errors;
        bool codeOnly = rc != 0 && errors <= 0;
        if (rc != 0 && Unreadable(rc))
            status = 2;
        else if (rc != 0 || errors > 0)
            status = 1;
        else
            status = 0;
        string line = (status == 0 ? "OK " : "FAIL ") + name + ": ";
        line += codeOnly ? "error " + rc.ToString(CultureInfo.InvariantCulture)
                         : Plural(errors, "error") + ", " + Plural(warnings, "warning");
        return line;
    }

    // Runs `mode` on the file `path` (exactly as it was given on the command line). `line` is what
    // to print (without the end of line); the result is the exit status of the program: 0 the file
    // is valid (warnings allowed), 1 it has an error or the round trip failed, 2 the file cannot be
    // read at all or the backend cannot be called.
    internal static int Run(int mode, string path, out string line)
    {
        try
        {
            return RunUnsafe(mode, path, out line);
        }
        catch (Exception e) when (e is DllNotFoundException || e is BadImageFormatException ||
                                  e is EntryPointNotFoundException || e is OutOfMemoryException)
        {
            line = string.Empty;
            Console.Error.WriteLine("loomora: " + e.Message);
            return 2;
        }
    }

    private static int RunUnsafe(int mode, string path, out string line)
    {
        int rc = Native.ProjectOpenPath(path, 0, 0, out nint project, out nint report);
        string summary = SummaryLine(path, rc, report, out int status);
        if (report != 0)
            Native.ReportFree(report);
        if (rc != 0 || mode == Validate)
        {
            if (project != 0)
                Native.ProjectClose(project);
            line = summary;
            return status;
        }
        status = RoundTrip(path, project, out line);
        Native.ProjectClose(project);
        return status;
    }

    private static int RoundTrip(string path, nint project, out string line)
    {
        List<byte[]> chunks = new List<byte[]>();
        long length = 0;
        bool failed = false;
        // The sink of the export: keeps each chunk (it never lets an exception reach native code).
        Native.SinkCallback sink = (user, data, size) =>
        {
            try
            {
                byte[] chunk = new byte[(int)size];
                if (size != 0)
                    new ReadOnlySpan<byte>(data, (int)size).CopyTo(chunk);
                chunks.Add(chunk);
                length += chunk.Length;
                return 0;
            }
            catch (Exception)
            {
                failed = true;
                return 1; // stops the export
            }
        };
        int profile = Native.ProjectIsLight(project) == 1 ? Native.ProfileLight : Native.ProfileFull;
        int rc = Native.ProjectExportStream(project, profile, CopyModifiedAt, sink, out nint exportReport);
        if (exportReport != 0)
            Native.ReportFree(exportReport);
        if (rc == 0 && failed)
            rc = Native.ErrNoMemory;
        if (rc != 0)
        {
            line = "roundtrip FAIL export error " + rc.ToString(CultureInfo.InvariantCulture);
            return 1;
        }

        byte[] copy = new byte[length];
        long offset = 0;
        foreach (byte[] chunk in chunks)
        {
            Buffer.BlockCopy(chunk, 0, copy, (int)offset, chunk.Length);
            offset += chunk.Length;
        }
        int validated;
        nint copyReport;
        fixed (byte* copyPointer = copy.Length > 0 ? copy : new byte[1]) // never a null pointer
        {
            validated = Native.ValidateMemory(copy.Length > 0 ? copyPointer : null, (nuint)copy.Length,
                                              0, 0, out copyReport);
        }
        string summary = SummaryLine(path, validated, copyReport, out int status);
        if (copyReport != 0)
            Native.ReportFree(copyReport);
        line = (status == 0 ? "roundtrip ok " : "roundtrip FAIL ") + summary;
        return status == 0 ? 0 : 1;
    }
}
