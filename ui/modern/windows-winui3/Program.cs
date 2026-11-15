// Entry point of the Windows Modern UI. The application is unpackaged and framework-dependent, so
// the Windows App Runtime is found at start-up through the bootstrapper API (the documented way
// for an unpackaged app that does not carry the runtime). The ABI of the backend is checked first,
// as ui-contract.md asks. The XAML generated entry point (XamlGeneratedMain) starts the
// application once the runtime is loaded.
using System;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
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
        if (!Probe.Enabled)
            MessageBox(0, message, "Loomora", 0x10 /* MB_OK | MB_ICONERROR */);
    }

    [STAThread]
    private static int Main(string[] args)
    {
        Probe.Start();
        Probe.ParseCommandLine(args);
        if (Probe.BadUsage)
        {
            Console.Error.WriteLine("usage: Loomora.WinUI.exe [--probe [--probe-input <utf8>]]");
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
