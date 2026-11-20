using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
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

    public static Dictionary<string, string> LoadSettings(string? settingsFile = null)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        var path = settingsFile ?? GetSettingsPath();
        if (!File.Exists(path)) return result;

        foreach (var line in File.ReadAllLines(path))
        {
            var trimmed = line.Trim();
            if (string.IsNullOrEmpty(trimmed) || trimmed.StartsWith('#') || !trimmed.Contains('='))
                continue;
            var parts = trimmed.Split('=', 2);
            if (parts.Length == 2)
            {
                var key = parts[0].Trim();
                var val = parts[1].Trim().Trim('"', '\'');
                if (!string.IsNullOrEmpty(key))
                    result[key] = val;
            }
        }
        return result;
    }

    public static void SaveSettingsAtomic(Dictionary<string, string> settings, string? settingsFile = null)
    {
        var targetPath = settingsFile ?? GetSettingsPath();
        var dir = Path.GetDirectoryName(targetPath);
        if (!string.IsNullOrEmpty(dir))
            Directory.CreateDirectory(dir);

        var lines = new List<string> { "# SkyRecompiled Configuration" };
        foreach (var kv in settings.OrderBy(p => p.Key, StringComparer.Ordinal))
        {
            lines.Add($"{kv.Key} = \"{kv.Value}\"");
        }
        var content = string.Join(Environment.NewLine, lines) + Environment.NewLine;
        var tmpPath = Path.Combine(dir ?? ".", $".{Path.GetFileName(targetPath)}.{Guid.NewGuid():N}.tmp");
        try
        {
            File.WriteAllText(tmpPath, content);
            File.Move(tmpPath, targetPath, overwrite: true);
        }
        finally
        {
            if (File.Exists(tmpPath))
                File.Delete(tmpPath);
        }
    }

    public static string? LoadSavedRomPath()
    {
        try
        {
            var settings = LoadSettings();
            if (settings.TryGetValue("rom_path", out var val) && File.Exists(val))
                return val;
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
            var settings = LoadSettings();
            settings["rom_path"] = romPath;
            SaveSettingsAtomic(settings);
        }
        catch
        {
            // Ignore write errors
        }
    }

    public static void ClearSavedRomPath(string? settingsFile = null)
    {
        try
        {
            var settings = LoadSettings(settingsFile);
            if (settings.Remove("rom_path"))
                SaveSettingsAtomic(settings, settingsFile);
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
        var savestateDir = Path.Combine(userDir, "savestates");

        Directory.CreateDirectory(Path.GetDirectoryName(savePath)!);
        Directory.CreateDirectory(consoleDir);
        Directory.CreateDirectory(savestateDir);

        var skySettingsPath = Path.Combine(userDir, "sky_settings.toml");
        var logPath = Path.Combine(userDir, "nds_runner.log");
        var psi = new ProcessStartInfo
        {
            FileName = runnerBin,
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(runnerBin)!,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        psi.ArgumentList.Add(consoleDir);
        psi.ArgumentList.Add("--interactive");
        psi.ArgumentList.Add("--config");
        psi.ArgumentList.Add(configPath);
        psi.ArgumentList.Add("--rom");
        psi.ArgumentList.Add(romPath);
        psi.ArgumentList.Add("--save-path");
        psi.ArgumentList.Add(savePath);
        psi.ArgumentList.Add("--savestate-dir");
        psi.ArgumentList.Add(savestateDir);
        psi.ArgumentList.Add("--runtime-settings-path");
        psi.ArgumentList.Add(skySettingsPath);
        psi.ArgumentList.Add("--rtc-host");

        try
        {
            File.WriteAllText(logPath, $"$ {psi.FileName} {string.Join(" ", psi.ArgumentList)}\n");
        }
        catch
        {
            // Diagnostic logging must never prevent the game from launching.
        }

        var proc = Process.Start(psi);
        if (proc == null)
            throw new InvalidOperationException("Failed to launch nds_runner process.");

        void AppendRunnerLog(string? line)
        {
            if (line is null) return;
            try { File.AppendAllText(logPath, line + Environment.NewLine); }
            catch { }
        }
        proc.OutputDataReceived += (_, e) => AppendRunnerLog(e.Data);
        proc.ErrorDataReceived += (_, e) => AppendRunnerLog(e.Data);
        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();

        return proc;
    }
}
