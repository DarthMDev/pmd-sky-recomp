using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;
using SkyRecompiled.Launcher.Models;

namespace SkyRecompiled.Launcher.Services;

/// <summary>
/// Manages cartridge SHA-1 verification, cross-platform user storage paths, atomic
/// launcher settings persistence, engine binary discovery, and launching <c>nds_runner</c>.
/// </summary>
public class RomPreparationService
{
    public const string CleanUsaSha1 = "5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152";
    public const string CleanEuSha1 = "c838a5adf1ed32d2da8454976e5b1a1aa189c139";

    public static readonly IReadOnlySet<string> DefaultValidSha1s = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
    {
        CleanUsaSha1,
        CleanEuSha1,
    };

    private static readonly object LogLock = new();

    public static string GetUserStorageDir()
    {
        if (OperatingSystem.IsMacOS())
        {
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            return Path.Combine(home, "Library", "Application Support", "SkyRecompiled");
        }
        if (OperatingSystem.IsWindows())
        {
            var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            return Path.Combine(appData, "SkyRecompiled");
        }

        var xdg = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
        var baseDir = !string.IsNullOrEmpty(xdg)
            ? xdg
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local", "share");
        return Path.Combine(baseDir, "SkyRecompiled");
    }

    public static string GetSettingsPath() => Path.Combine(GetUserStorageDir(), "settings.toml");
    public static string GetSkySettingsPath() => Path.Combine(GetUserStorageDir(), "sky_settings.toml");
    public static string GetSavesDir() => Path.Combine(GetUserStorageDir(), "saves");
    public static string GetSavesPath() => Path.Combine(GetSavesDir(), "pmdsky.sav");
    public static string GetSavestatesDir() => Path.Combine(GetUserStorageDir(), "savestates");
    public static string GetConsoleDir() => Path.Combine(GetUserStorageDir(), "console");
    public static string GetModsDir() => Path.Combine(GetUserStorageDir(), "mods");
    public static string GetModStatePath() => Path.Combine(GetUserStorageDir(), "mods_state.json");
    public static string GetActiveModsManifestPath() => Path.Combine(GetUserStorageDir(), "active_mods.json");
    public static string GetLogPath() => Path.Combine(GetUserStorageDir(), "nds_runner.log");

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
            var sanitizedKey = kv.Key.Replace("\r", "").Replace("\n", "").Trim();
            if (string.IsNullOrEmpty(sanitizedKey))
                continue;
            var sanitizedVal = (kv.Value ?? string.Empty).Replace("\r", "").Replace("\n", "");
            lines.Add($"{sanitizedKey} = \"{sanitizedVal}\"");
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

    public static void UpdateSettings(IDictionary<string, string> updates, string? settingsFile = null)
    {
        var current = LoadSettings(settingsFile);
        foreach (var kv in updates)
        {
            current[kv.Key] = kv.Value;
        }
        SaveSettingsAtomic(current, settingsFile);
    }

    public static LauncherPreferences LoadPreferences(string? settingsFile = null)
    {
        var raw = LoadSettings(settingsFile);
        var prefs = new LauncherPreferences();
        if (raw.TryGetValue("rescue_endpoint", out var ep))
            prefs.RescueEndpoint = LauncherPreferences.NormalizeEndpoint(ep);
        if (raw.TryGetValue("rescue_offline", out var offline))
            prefs.RescueOfflineMode = string.Equals(offline, "true", StringComparison.OrdinalIgnoreCase) || offline == "1";
        return prefs;
    }

    public static void SavePreferences(LauncherPreferences prefs, string? settingsFile = null)
    {
        UpdateSettings(new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["rescue_endpoint"] = LauncherPreferences.NormalizeEndpoint(prefs.RescueEndpoint),
            ["rescue_offline"] = prefs.RescueOfflineMode ? "true" : "false",
        }, settingsFile);
    }

    public static string? LoadSavedRomPath(string? settingsFile = null)
    {
        try
        {
            var settings = LoadSettings(settingsFile);
            if (settings.TryGetValue("rom_path", out var val) && File.Exists(val))
                return val;
        }
        catch
        {
            // Ignore any permission or read issues
        }
        return null;
    }

    public static void SaveRomPath(string romPath, string? settingsFile = null)
    {
        try
        {
            var settings = LoadSettings(settingsFile);
            settings["rom_path"] = romPath;
            SaveSettingsAtomic(settings, settingsFile);
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

    public static Task<RomInfo> VerifyRomAsync(
        string path,
        IProgress<(double Progress, string Status)>? progress = null,
        CancellationToken ct = default)
        => VerifyRomAsync(path, DefaultValidSha1s, progress, ct);

    public static async Task<RomInfo> VerifyRomAsync(
        string path,
        IReadOnlySet<string> acceptedSha1s,
        IProgress<(double Progress, string Status)>? progress = null,
        CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
            return new RomInfo(path, "Unknown", "", false, 0);

        var fileInfo = new FileInfo(path);
        long totalBytes = fileInfo.Length;
        if (totalBytes == 0)
            return new RomInfo(path, "Unknown", "", false, 0);

        using var sha1 = SHA1.Create();
        await using var stream = File.OpenRead(path);
        byte[] buffer = new byte[1024 * 1024]; // 1MB chunks
        long readBytes = 0;
        int bytesRead;

        while ((bytesRead = await stream.ReadAsync(buffer.AsMemory(0, buffer.Length), ct)) > 0)
        {
            ct.ThrowIfCancellationRequested();
            sha1.TransformBlock(buffer, 0, bytesRead, null, 0);
            readBytes += bytesRead;
            var pct = ((double)readBytes / totalBytes) * 100.0;
            progress?.Report((pct, $"Verifying cartridge ROM... {pct:0}%"));
        }

        sha1.TransformFinalBlock(buffer, 0, 0);
        var hashHex = Convert.ToHexString(sha1.Hash ?? Array.Empty<byte>()).ToLowerInvariant();

        if (hashHex == CleanUsaSha1)
            return new RomInfo(path, "USA (NA)", hashHex, true, totalBytes);
        if (hashHex == CleanEuSha1)
            return new RomInfo(path, "Europe (EU)", hashHex, true, totalBytes);
        if (acceptedSha1s.Contains(hashHex))
            return new RomInfo(path, "Verified Custom", hashHex, true, totalBytes);

        return new RomInfo(path, "Unsupported / Modified", hashHex, false, totalBytes);
    }

    public static (string RunnerPath, string ConfigPath) LocateEngineBinaries(string? baseDirOverride = null)
    {
        var exeName = OperatingSystem.IsWindows() ? "nds_runner.exe" : "nds_runner";
        var appDir = string.IsNullOrWhiteSpace(baseDirOverride) ? AppContext.BaseDirectory : baseDirOverride;

        var ancestors = new List<string>();
        var parent = new DirectoryInfo(Path.GetFullPath(appDir)).Parent;
        for (int depth = 0; depth < 8 && parent != null; depth++)
        {
            ancestors.Add(parent.FullName);
            parent = parent.Parent;
        }

        var runnerCandidates = new List<string>
        {
            Path.Combine(appDir, exeName),
            Path.Combine(appDir, "bin", exeName),
            Path.Combine(appDir, "..", "MacOS", exeName),
        };
        foreach (var anc in ancestors)
            runnerCandidates.Add(Path.Combine(anc, "build", "runner", exeName));
        foreach (var anc in ancestors)
            runnerCandidates.Add(Path.Combine(anc, "build", "runner-fallback", exeName));

        string? foundRunner = null;
        foreach (var c in runnerCandidates)
        {
            var full = Path.GetFullPath(c);
            if (File.Exists(full)) { foundRunner = full; break; }
        }

        var configCandidates = new List<string>
        {
            Path.Combine(appDir, "game.toml"),
            Path.Combine(appDir, "Resources", "game.toml"),
            Path.Combine(appDir, "..", "Resources", "game.toml"),
            Path.Combine(appDir, "config", "game.toml"),
            Path.Combine(appDir, "..", "config", "game.toml"),
        };
        foreach (var anc in ancestors)
            configCandidates.Add(Path.Combine(anc, "config", "game.toml"));

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

    public static bool TryLocateEngineBinaries(
        out string? runnerPath,
        out string? configPath,
        out string? error,
        string? baseDirOverride = null)
    {
        try
        {
            var located = LocateEngineBinaries(baseDirOverride);
            runnerPath = located.RunnerPath;
            configPath = located.ConfigPath;
            error = null;
            return true;
        }
        catch (Exception ex)
        {
            runnerPath = null;
            configPath = null;
            error = ex.Message;
            return false;
        }
    }

    /// <summary>
    /// Headless runner verification for CI and CLI smoke checks (<c>--check-runner</c>).
    /// </summary>
    public static int CheckRunner(string? baseDirOverride = null)
    {
        if (!TryLocateEngineBinaries(out var runnerPath, out var configPath, out var error, baseDirOverride) ||
            runnerPath == null || configPath == null)
        {
            Console.Error.WriteLine($"[SkyRecompiled] {error}");
            return 1;
        }

        var psi = new ProcessStartInfo
        {
            FileName = runnerPath,
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(runnerPath)!,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        psi.ArgumentList.Add("--help");

        try
        {
            using var proc = Process.Start(psi);
            if (proc == null)
            {
                Console.Error.WriteLine("[SkyRecompiled] Failed to spawn nds_runner.");
                return 1;
            }
            var stdout = proc.StandardOutput.ReadToEnd();
            var stderr = proc.StandardError.ReadToEnd();
            proc.WaitForExit();
            if (!string.IsNullOrEmpty(stdout))
                Console.Out.Write(stdout);
            if (!string.IsNullOrEmpty(stderr))
                Console.Error.Write(stderr);
            return proc.ExitCode;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[SkyRecompiled] Runner check failed: {ex.Message}");
            return 1;
        }
    }

    public static Process LaunchGame(string romPath, Action<string>? onLogLine = null)
    {
        var (runnerBin, configPath) = LocateEngineBinaries();
        var savePath = GetSavesPath();
        var consoleDir = GetConsoleDir();
        var userDir = GetUserStorageDir();
        var savestateDir = GetSavestatesDir();

        Directory.CreateDirectory(Path.GetDirectoryName(savePath)!);
        Directory.CreateDirectory(consoleDir);
        Directory.CreateDirectory(savestateDir);

        var skySettingsPath = GetSkySettingsPath();
        var logPath = GetLogPath();
        var psi = new ProcessStartInfo
        {
            FileName = runnerBin,
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(runnerBin)!,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
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

        var cmdBanner = $"$ {psi.FileName} {string.Join(" ", psi.ArgumentList)}";
        try
        {
            lock (LogLock)
            {
                File.WriteAllText(logPath, cmdBanner + Environment.NewLine);
            }
        }
        catch
        {
            // Diagnostic logging must never prevent the game from launching.
        }
        onLogLine?.Invoke(cmdBanner);

        var proc = Process.Start(psi);
        if (proc == null)
            throw new InvalidOperationException("Failed to launch nds_runner process.");

        void AppendRunnerLog(string? line)
        {
            if (line is null) return;
            try
            {
                lock (LogLock)
                {
                    File.AppendAllText(logPath, line + Environment.NewLine);
                }
            }
            catch
            {
                // Ignore disk log append failures
            }
            onLogLine?.Invoke(line);
        }

        proc.OutputDataReceived += (_, e) => AppendRunnerLog(e.Data);
        proc.ErrorDataReceived += (_, e) => AppendRunnerLog(e.Data);
        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();

        return proc;
    }

    public static void OpenPathInOs(string targetPath, bool createDirectory = true)
    {
        if (createDirectory)
            Directory.CreateDirectory(targetPath);

        var fileName = OperatingSystem.IsMacOS() ? "open"
            : OperatingSystem.IsWindows() ? "explorer.exe"
            : "xdg-open";
        var psi = new ProcessStartInfo
        {
            FileName = fileName,
            UseShellExecute = false,
        };
        psi.ArgumentList.Add(targetPath);
        Process.Start(psi);
    }
}
