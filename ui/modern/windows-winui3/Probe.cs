// Probe mode of the Windows Modern UI (ui-contract.md): --probe and --probe-input <utf8>.
//
// The probe line is built by the backend (loomora_probe_record); this file only measures, converts
// and writes it: to standard output (attached to the console of the parent when the process has
// none, it is a GUI executable) and to loomora-probe.json next to the executable.
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Loomora.WinUI;

internal static partial class Probe
{
    internal const int ExitDelayMs = 2000;
    private const string ProbeFileName = "loomora-probe.json";

    private static DateTime _launchTime = DateTime.Now;
    private static long _launchTicks = Stopwatch.GetTimestamp();
    private static bool _launchFromProcess;
    private static DateTime _displayedTime;
    private static bool _displayed;

    internal static bool Enabled { get; private set; }
    internal static bool HasInput { get; private set; }
    internal static string Input { get; private set; } = string.Empty;
    internal static bool BadUsage { get; private set; }
    internal static int ExitStatus { get; set; }

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool AttachConsole(uint processId);

    [LibraryImport("kernel32.dll")]
    private static partial nint GetStdHandle(int standardHandle);

    // The launch is the creation of the process, so the delay includes the start of the .NET
    // runtime and of the Windows App Runtime, not only the time spent in this code.
    internal static void Start()
    {
        _launchTicks = Stopwatch.GetTimestamp();
        try
        {
            using Process self = Process.GetCurrentProcess();
            _launchTime = self.StartTime;
            _launchFromProcess = true;
        }
        catch (Exception)
        {
            _launchTime = DateTime.Now; // fallback: the first instruction of Main
        }
    }

    internal static void ParseCommandLine(string[] args)
    {
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--probe")
            {
                Enabled = true;
            }
            else if (args[i] == "--probe-input")
            {
                if (i + 1 < args.Length)
                {
                    HasInput = true;
                    Input = args[++i];
                }
                else
                {
                    BadUsage = true;
                }
            }
        }
        // The input override is only meaningful in probe mode, and must be convertible to UTF-8.
        if (HasInput && (!Enabled || !IsWellFormedUtf16(Input)))
            BadUsage = true;
    }

    // True if every UTF-16 unit belongs to a well-formed sequence: no unpaired surrogate.
    private static bool IsWellFormedUtf16(string text)
    {
        for (int i = 0; i < text.Length; i++)
        {
            if (char.IsHighSurrogate(text[i]))
            {
                if (i + 1 >= text.Length || !char.IsLowSurrogate(text[i + 1]))
                    return false;
                i++;
            }
            else if (char.IsLowSurrogate(text[i]))
            {
                return false;
            }
        }
        return true;
    }

    // Records the first drawn frame; further calls are ignored.
    internal static void MarkDisplayed()
    {
        if (_displayed)
            return;
        _displayed = true;
        _displayedTime = DateTime.Now;
    }

    private static uint ElapsedMs()
    {
        if (!_displayed)
            return 0;
        double ms = _launchFromProcess
            ? (_displayedTime - _launchTime).TotalMilliseconds
            : Stopwatch.GetElapsedTime(_launchTicks).TotalMilliseconds;
        return ms > 0 ? (uint)ms : 0;
    }

    private static string TimestampUtc() =>
        DateTime.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);

    private static void WriteToStandardOutput(string line)
    {
        try
        {
            nint handle = GetStdHandle(-11); // STD_OUTPUT_HANDLE
            Stream? stream = null;
            if (handle != 0 && handle != -1)
            {
                stream = Console.OpenStandardOutput(); // redirected to a pipe or a file
            }
            else if (AttachConsole(uint.MaxValue)) // ATTACH_PARENT_PROCESS
            {
                SafeFileHandle console =
                    File.OpenHandle("CONOUT$", FileMode.Open, FileAccess.Write, FileShare.ReadWrite);
                stream = new FileStream(console, FileAccess.Write);
            }
            if (stream == null)
                return; // no console: the file is the output
            using (stream)
            {
                byte[] bytes = Encoding.ASCII.GetBytes(line);
                stream.Write(bytes, 0, bytes.Length);
                stream.Flush();
            }
        }
        catch (Exception)
        {
            // standard output is best effort, the file next to the executable is the record
        }
    }

    // Builds the probe line with the backend and writes it; `input` is what the field holds,
    // `message` what the greeting label displays.
    internal static void Emit(string input, string message, bool eolWarningShown)
    {
        try
        {
            long rssKb = -1;
            try
            {
                using Process self = Process.GetCurrentProcess();
                rssKb = self.PeakWorkingSet64 / 1024;
            }
            catch (Exception)
            {
                // not measurable: null in the record
            }
            string? line = Native.ProbeRecord(SystemInfo.PlatformId(), input, message, _displayed,
                                              ElapsedMs(), rssKb, eolWarningShown, "", TimestampUtc());
            if (line == null)
            {
                ExitStatus = 1;
                return;
            }
            line += "\n";
            WriteToStandardOutput(line);
            File.WriteAllText(Path.Combine(AppContext.BaseDirectory, ProbeFileName), line,
                              new UTF8Encoding(false));
        }
        catch (Exception)
        {
            ExitStatus = 1;
        }
    }
}
