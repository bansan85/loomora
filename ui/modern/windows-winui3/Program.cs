// Entry point of the Windows Modern UI. The application is unpackaged and framework-dependent, so
// the Windows App Runtime is found at start-up through the bootstrapper API (the documented way
// for an unpackaged app that does not carry the runtime). The ABI of the backend is checked first,
// as ui-contract.md asks. The XAML generated entry point (XamlGeneratedMain) starts the
// application once the runtime is loaded.
using System;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Windows.ApplicationModel.DynamicDependency;

namespace Loomora.WinUI;

internal static partial class Program
{
    // Windows App SDK 2.6: major 2 in the high 16 bits, minor 6 in the low 16 bits. Any 2.6.x
    // runtime (package family Microsoft.WindowsAppRuntime.2) satisfies it.
    private const uint WindowsAppSdkMajorMinor = 0x00020006;

    [LibraryImport("user32.dll", EntryPoint = "MessageBoxW", StringMarshalling = StringMarshalling.Utf16)]
    private static partial int MessageBox(nint window, string text, string caption, uint type);

    // A GUI executable has no console: the message goes to standard error (for a parent that
    // redirected it) and, outside probe mode, to a message box.
    private static void Fail(string message)
    {
        try
        {
            Console.Error.WriteLine("loomora: " + message);
        }
        catch (IOException)
        {
            // no standard error
        }
        if (!Probe.Enabled && Probe.ProjectMode == 0)
            MessageBox(0, message, "Loomora", 0x10 /* MB_OK | MB_ICONERROR */);
    }

    // The project probe modes (contracts/ui-validate-project.md): the backend only, so neither the
    // Windows App Runtime nor a window is needed. The line goes to standard output, in UTF-8.
    private static int RunProject()
    {
        int status = ProjectProbe.Run(Probe.ProjectMode, Probe.ProjectPath, out string line);
        if (line.Length != 0)
            Probe.WriteToStandardOutput(line + '\n', new UTF8Encoding(false));
        return status;
    }

    [STAThread]
    private static int Main(string[] args)
    {
        Probe.Start();
        Probe.ParseCommandLine(args);
        if (Probe.BadUsage)
        {
            Console.Error.WriteLine("usage: Loomora.WinUI.exe [--probe [--probe-input <utf8>] | " +
                                    "--validate-project <path> | --roundtrip-project <path>]");
            return 2;
        }

        try
        {
            if (Native.AbiVersion() != Native.ExpectedAbiVersion)
            {
                Fail("ABI version mismatch between the interface and the engine.");
                return 1;
            }
        }
        catch (Exception e) when (e is DllNotFoundException || e is BadImageFormatException || e is EntryPointNotFoundException)
        {
            Fail("The engine (loomora.dll, same architecture as this program) cannot be loaded: " + e.Message);
            return 1;
        }

        if (Probe.ProjectMode != 0) // --validate-project / --roundtrip-project: no window, no runtime
            return RunProject();

        if (!Bootstrap.TryInitialize(WindowsAppSdkMajorMinor, out int result))
        {
            Fail("The Windows App Runtime 2.6 for this architecture is not installed (HRESULT 0x" +
                 result.ToString("X8", CultureInfo.InvariantCulture) +
                 "). Install it, then start the program again.");
            return 1;
        }
        try
        {
            XamlGeneratedProgram.XamlGeneratedMain();
        }
        finally
        {
            Bootstrap.Shutdown();
        }
        return Probe.ExitStatus;
    }
}
