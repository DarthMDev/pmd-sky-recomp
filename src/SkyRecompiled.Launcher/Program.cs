using System;
using System.Linq;
using Avalonia;
using SkyRecompiled.Launcher.Services;

namespace SkyRecompiled.Launcher;

sealed class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Contains("--version", StringComparer.OrdinalIgnoreCase))
        {
            Console.WriteLine("SkyRecompiled Launcher 0.2.3");
            return 0;
        }

        if (args.Contains("--check-runner", StringComparer.OrdinalIgnoreCase))
        {
            return RomPreparationService.CheckRunner();
        }

        return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    public static AppBuilder BuildAvaloniaApp()
        => AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace();
}
