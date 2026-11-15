// Application object of the Windows Modern UI: creates the main window, marks the first drawn
// frame and, in probe mode, ends the program 2 seconds after the window is shown.
using System;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace Loomora.WinUI;

public partial class App : Application
{
    private MainWindow? _window;
    private DispatcherQueueTimer? _exitTimer;

    public App()
    {
        InitializeComponent();
        UnhandledException += (sender, e) =>
        {
            Console.Error.WriteLine("loomora: unhandled exception: " + e.Exception);
            Probe.ExitStatus = 1;
        };
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        _window = new MainWindow(Probe.HasInput ? Probe.Input : null);
        CompositionTarget.Rendering += OnFirstFrame;
        _window.Activate();
        if (Probe.Enabled)
        {
            _exitTimer = _window.DispatcherQueue.CreateTimer();
            _exitTimer.Interval = TimeSpan.FromMilliseconds(Probe.ExitDelayMs);
            _exitTimer.IsRepeating = false;
            _exitTimer.Tick += (sender, e) => FinishProbe();
            _exitTimer.Start();
        }
    }

    // The first frame drawn after the window was activated.
    private static void OnFirstFrame(object? sender, object e)
    {
        CompositionTarget.Rendering -= OnFirstFrame;
        Probe.MarkDisplayed();
    }

    // Probe mode ends here: the line is built from what the controls hold now.
    private void FinishProbe()
    {
        if (_window != null)
            Probe.Emit(_window.InputText, _window.GreetingTextValue, _window.EolWarningShown);
        _window?.Close();
        Exit();
    }
}
