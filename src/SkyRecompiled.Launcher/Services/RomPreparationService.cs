using System;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace SkyRecompiled.Launcher.Services;

public record RomInfo(string Path, string Region, string Sha1, bool IsValid);

public class RomPreparationService
{
    public const string CleanUsaSha1 = "5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152";
    public const string CleanEuSha1  = "c838a5adf1ed32d2da8454976e5b1a1aa189c139";

    public static string GetUserStorageDir()
    {
        if (OperatingSystem.IsMacOS())
        {
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            return Path.Combine(home, "Library", "Application Support", "SkyRecompiled");
        }
        else if (OperatingSystem.IsWindows())
        {
            var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            return Path.Combine(appData, "SkyRecompiled");
        }
        else // Linux
        {
            var xdg = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
            var baseDir = !string.IsNullOrEmpty(xdg)
                ? xdg
                : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local", "share");
            return Path.Combine(baseDir, "SkyRecompiled");
        }
    }

    public static string GetSettingsPath() => Path.Combine(GetUserStorageDir(), "settings.toml");
    public static string GetSavesPath() => Path.Combine(GetUserStorageDir(), "saves", "pmdsky.sav");
    public static string GetConsoleDir() => Path.Combine(GetUserStorageDir(), "console");

    public static string? LoadSavedRomPath()
    {
        try
        {
            var settingsFile = GetSettingsPath();
            if (!File.Exists(settingsFile)) return null;

            foreach (var line in File.ReadAllLines(settingsFile))
            {
                var trimmed = line.Trim();
                if (trimmed.StartsWith("rom_path") && trimmed.Contains('='))
                {
                    var parts = trimmed.Split('=', 2);
                    if (parts.Length == 2)
                    {
                        var val = parts[1].Trim().Trim('"', '\'');
                        if (File.Exists(val)) return val;
                    }
                }
            }
        }
        catch
        {
            // Ignore any permission or read issues
        }
        return null;
    }

    public static void SaveRomPath(string romPath)
    {
        try
        {
            var dir = GetUserStorageDir();
            Directory.CreateDirectory(dir);
            var settingsFile = GetSettingsPath();
            File.WriteAllText(settingsFile, $"# SkyRecompiled Configuration\nrom_path = \"{romPath}\"\n");
        }
        catch
        {
            // Ignore write errors
        }
    }

    public static async Task<RomInfo> VerifyRomAsync(string path, IProgress<(double Progress, string Status)>? progress = null, CancellationToken ct = default)
    {
        if (!File.Exists(path))
            return new RomInfo(path, "Unknown", "", false);

        var fileInfo = new FileInfo(path);
        long totalBytes = fileInfo.Length;
        if (totalBytes == 0)
            return new RomInfo(path, "Unknown", "", false);

        using var sha1 = SHA1.Create();
        await using var stream = File.OpenRead(path);
        byte[] buffer = new byte[1024 * 1024]; // 1MB chunks
        long readBytes = 0;
        int bytesRead;

        while ((bytesRead = await stream.ReadAsync(buffer.AsMemory(0, buffer.Length), ct)) > 0)
        {
            sha1.TransformBlock(buffer, 0, bytesRead, null, 0);
            readBytes += bytesRead;
            var pct = ((double)readBytes / totalBytes) * 100.0;
            progress?.Report((pct, $"Verifying cartridge ROM... {pct:0}%"));
        }

        sha1.TransformFinalBlock(buffer, 0, 0);
        var hashHex = Convert.ToHexString(sha1.Hash ?? Array.Empty<byte>()).ToLowerInvariant();

        if (hashHex == CleanUsaSha1)
            return new RomInfo(path, "USA (NA)", hashHex, true);
        if (hashHex == CleanEuSha1)
            return new RomInfo(path, "Europe (EU)", hashHex, true);

        return new RomInfo(path, "Unsupported / Modified", hashHex, false);
    }

    public static (string RunnerPath, string ConfigPath) LocateEngineBinaries()
    {
        var exeName = OperatingSystem.IsWindows() ? "nds_runner.exe" : "nds_runner";
        var appDir = AppDomain.CurrentDomain.BaseDirectory;

        // Try standard layout candidates
        string[] runnerCandidates =
        {
            Path.Combine(appDir, exeName),
            Path.Combine(appDir, "..", "MacOS", exeName),
            Path.Combine(appDir, "bin", exeName),
            Path.Combine(appDir, "..", "..", "..", "build", "runner", exeName),
            Path.Combine(appDir, "..", "..", "..", "..", "build", "runner", exeName)
        };

        string? foundRunner = null;
        foreach (var c in runnerCandidates)
        {
            var full = Path.GetFullPath(c);
            if (File.Exists(full)) { foundRunner = full; break; }
        }

        string[] configCandidates =
        {
            Path.Combine(appDir, "game.toml"),
            Path.Combine(appDir, "Resources", "game.toml"),
            Path.Combine(appDir, "..", "Resources", "game.toml"),
            Path.Combine(appDir, "config", "game.toml"),
            Path.Combine(appDir, "..", "..", "..", "config", "game.toml"),
            Path.Combine(appDir, "..", "..", "..", "..", "config", "game.toml")
        };

        string? foundConfig = null;
        foreach (var c in configCandidates)
        {
            var full = Path.GetFullPath(c);
            if (File.Exists(full)) { foundConfig = full; break; }
        }

        if (foundRunner == null)
            throw new FileNotFoundException($"Could not locate {exeName} engine executable.");
        if (foundConfig == null)
            throw new FileNotFoundException("Could not locate game.toml cartridge configuration.");

        return (foundRunner, foundConfig);
    }

    public static Process LaunchGame(string romPath)
    {
        var (runnerBin, configPath) = LocateEngineBinaries();
        var savePath = GetSavesPath();
        var consoleDir = GetConsoleDir();
        var userDir = GetUserStorageDir();

        Directory.CreateDirectory(Path.GetDirectoryName(savePath)!);
        Directory.CreateDirectory(consoleDir);
        Directory.CreateDirectory(Path.Combine(userDir, "savestates"));

        var skySettingsPath = Path.Combine(userDir, "sky_settings.toml");
        var psi = new ProcessStartInfo
        {
            FileName = runnerBin,
            Arguments = $"\"{consoleDir}\" --interactive --config \"{configPath}\" --rom \"{romPath}\" --save-path \"{savePath}\" --savestate-dir \"{Path.Combine(userDir, "savestates")}\" --runtime-settings-path \"{skySettingsPath}\" --rtc-host",
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(runnerBin)!
        };

        var proc = Process.Start(psi);
        if (proc == null)
            throw new InvalidOperationException("Failed to launch nds_runner process.");

        return proc;
    }
}
