namespace Loomora.Maui;

public partial class App : Application
{
	// Size of the window in device-independent units: the same as the WinUI 3 Modern UI at 96 DPI.
	private const double WindowWidth = 560;
	private const double WindowHeight = 360;

	public App()
	{
		InitializeComponent();
	}

	protected override Window CreateWindow(IActivationState? activationState)
	{
		return new Window(new MainPage())
		{
			Title = "Loomora",
			Width = WindowWidth,
			Height = WindowHeight,
			MinimumWidth = 320,
			MinimumHeight = 240,
		};
	}
}
