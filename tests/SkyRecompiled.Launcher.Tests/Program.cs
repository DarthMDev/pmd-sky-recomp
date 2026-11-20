using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;
using SkyRecompiled.Launcher.Models;
using SkyRecompiled.Launcher.Services;

namespace SkyRecompiled.Launcher.Tests;

internal static class Program
{
    private static int _passed;
    private static int _failed;

    public static int Main(string[] args)
    {
        RunSync("RomPreparationService.VerifyAndSettings", TestRomPreparationServiceAsync().GetAwaiter().GetResult);
        RunSync("RomPreparationService.LocateEngineBinaries", TestLocateEngineBinaries);
        RunSync("SkySettingsService.RoundTripAndClamping", TestSkySettingsService);
        RunSync("ModManagerService.DiscoveryPrecedenceAndSafety", TestModManagerService);
        RunSync("SaveManagementService.InspectBackupAndRestore", TestSaveManagementService);

        Console.WriteLine($"[LauncherTests] Summary: {_passed} passed, {_failed} failed.");
        return _failed == 0 ? 0 : 1;
    }

    private static void RunSync(string name, Action test)
    {
        try
        {
            test();
            _passed++;
            Console.WriteLine($"PASS: {name}");
        }
        catch (Exception ex)
        {
            _failed++;
            Console.Error.WriteLine($"FAIL: {name}: {ex}");
        }
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }

    private static void AssertEqual<T>(T actual, T expected, string label)
    {
        if (!EqualityComparer<T>.Default.Equals(actual, expected))
            throw new InvalidOperationException($"{label}: expected '{expected}', got '{actual}'");
    }

    private static void AssertThrows<TEx>(Action action, string label) where TEx : Exception
    {
        try
        {
            action();
        }
        catch (TEx)
        {
            return;
        }
        catch (Exception ex)
        {
            throw new InvalidOperationException($"{label}: expected {typeof(TEx).Name}, got {ex.GetType().Name}: {ex.Message}");
        }
        throw new InvalidOperationException($"{label}: expected {typeof(TEx).Name}, but no exception was thrown");
    }

    private sealed class TempDir : IDisposable
    {
        public string Path { get; }

        public TempDir()
        {
            Path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"sky-launcher-test-{Guid.NewGuid():N}");
            Directory.CreateDirectory(Path);
        }

        public void Dispose()
        {
            try
            {
                if (Directory.Exists(Path))
                    Directory.Delete(Path, recursive: true);
            }
            catch
            {
                // Best effort cleanup
            }
        }
    }

    private static async Task TestRomPreparationServiceAsync()
    {
        using var tmp = new TempDir();

        // Missing and empty ROM
        var missing = await RomPreparationService.VerifyRomAsync(Path.Combine(tmp.Path, "missing.nds"));
        Assert(!missing.IsValid, "Missing file should be invalid");

        var emptyPath = Path.Combine(tmp.Path, "empty.nds");
        await File.WriteAllBytesAsync(emptyPath, Array.Empty<byte>());
        var empty = await RomPreparationService.VerifyRomAsync(emptyPath);
        Assert(!empty.IsValid, "Empty file should be invalid");

        // Custom sample with SHA-1 verification + cancellation
        var samplePath = Path.Combine(tmp.Path, "Pokémon Sky (Unicode & Spaces é).nds");
        var payload = Enumerable.Repeat((byte)0x5A, 256 * 1024).ToArray();
        await File.WriteAllBytesAsync(samplePath, payload);
        var expectedSha1 = Convert.ToHexString(SHA1.HashData(payload)).ToLowerInvariant();

        var rejected = await RomPreparationService.VerifyRomAsync(samplePath);
        Assert(!rejected.IsValid, "Unrecognized hash should be rejected");
        AssertEqual(rejected.Sha1, expectedSha1, "Computed SHA-1");

        var customAccepted = await RomPreparationService.VerifyRomAsync(
            samplePath,
            new HashSet<string>(StringComparer.OrdinalIgnoreCase) { expectedSha1 });
        Assert(customAccepted.IsValid, "Custom accepted SHA-1 should validate");
        AssertEqual(customAccepted.SizeBytes, (long)payload.Length, "ROM size");

        using var cts = new CancellationTokenSource();
        cts.Cancel();
        bool cancelled = false;
        try
        {
            await RomPreparationService.VerifyRomAsync(samplePath, null, cts.Token);
        }
        catch (OperationCanceledException)
        {
            cancelled = true;
        }
        Assert(cancelled, "VerifyRomAsync should throw OperationCanceledException when cancelled");

        // Atomic settings preservation & preferences
        var settingsPath = Path.Combine(tmp.Path, "nested", "settings.toml");
        Directory.CreateDirectory(Path.GetDirectoryName(settingsPath)!);
        await File.WriteAllTextAsync(settingsPath, "# existing\ncustom_flag = \"keep_me\"\n");

        RomPreparationService.UpdateSettings(new Dictionary<string, string>
        {
            ["rom_path"] = samplePath,
        }, settingsPath);

        var loaded = RomPreparationService.LoadSettings(settingsPath);
        AssertEqual(loaded["rom_path"], samplePath, "rom_path persisted");
        AssertEqual(loaded["custom_flag"], "keep_me", "unrelated key preserved");

        RomPreparationService.SavePreferences(new LauncherPreferences
        {
            RescueEndpoint = "https://board.example.org/",
            RescueOfflineMode = true,
        }, settingsPath);

        var prefs = RomPreparationService.LoadPreferences(settingsPath);
        AssertEqual(prefs.RescueEndpoint, "https://board.example.org", "normalized endpoint");
        Assert(prefs.RescueOfflineMode, "offline mode persisted");
        AssertEqual(RomPreparationService.LoadSettings(settingsPath)["custom_flag"], "keep_me", "custom_flag still preserved");

        RomPreparationService.ClearSavedRomPath(settingsPath);
        var afterClear = RomPreparationService.LoadSettings(settingsPath);
        Assert(!afterClear.ContainsKey("rom_path"), "rom_path removed");
        AssertEqual(afterClear["custom_flag"], "keep_me", "custom_flag kept after clear");
        AssertEqual(Directory.GetFiles(Path.GetDirectoryName(settingsPath)!).Length, 1, "no leftover temp files");

        if (!OperatingSystem.IsWindows())
        {
            var lockedSettings = Path.Combine(tmp.Path, "locked_settings.toml");
            await File.WriteAllTextAsync(lockedSettings, "rom_path = \"/tmp/sky.nds\"\n");
            File.SetUnixFileMode(lockedSettings, UnixFileMode.None);
            Assert(!RomPreparationService.TryLoadPreferences(out _, out _, lockedSettings), "Unreadable settings.toml detected");
            RomPreparationService.SavePreferences(new LauncherPreferences(), lockedSettings, mergeWithExisting: false);
            Assert(RomPreparationService.TryLoadPreferences(out var recovered, out _, lockedSettings), "Overwrite without merge recovers unreadable settings.toml");
            AssertEqual(recovered.RescueEndpoint, LauncherPreferences.DefaultRescueEndpoint, "Recovered default endpoint");
        }
    }

    private static void TestLocateEngineBinaries()
    {
        using var tmp = new TempDir();
        var exeName = OperatingSystem.IsWindows() ? "nds_runner.exe" : "nds_runner";

        // 1. macOS .app layout: Contents/MacOS/nds_runner + Contents/Resources/game.toml
        var macAppMacOs = Path.Combine(tmp.Path, "SkyRecompiled.app", "Contents", "MacOS");
        var macAppRes = Path.Combine(tmp.Path, "SkyRecompiled.app", "Contents", "Resources");
        Directory.CreateDirectory(macAppMacOs);
        Directory.CreateDirectory(macAppRes);
        File.WriteAllBytes(Path.Combine(macAppMacOs, exeName), new byte[] { 1 });
        File.WriteAllText(Path.Combine(macAppRes, "game.toml"), "[system]\n");
        var macLocated = RomPreparationService.LocateEngineBinaries(macAppMacOs);
        AssertEqual(macLocated.RunnerPath, Path.Combine(macAppMacOs, exeName), "macOS runner path");
        AssertEqual(macLocated.ConfigPath, Path.Combine(macAppRes, "game.toml"), "macOS config path");

        // 2. Linux layout: pkg/bin/nds_runner + pkg/config/game.toml
        var linuxBin = Path.Combine(tmp.Path, "linux-pkg", "bin");
        var linuxCfg = Path.Combine(tmp.Path, "linux-pkg", "config");
        Directory.CreateDirectory(linuxBin);
        Directory.CreateDirectory(linuxCfg);
        File.WriteAllBytes(Path.Combine(linuxBin, exeName), new byte[] { 1 });
        File.WriteAllText(Path.Combine(linuxCfg, "game.toml"), "[system]\n");
        var linuxLocated = RomPreparationService.LocateEngineBinaries(linuxBin);
        AssertEqual(linuxLocated.RunnerPath, Path.Combine(linuxBin, exeName), "Linux runner path");
        AssertEqual(linuxLocated.ConfigPath, Path.Combine(linuxCfg, "game.toml"), "Linux config path");

        // 3. Windows layout: pkg/SkyRecompiled.exe (baseDir=pkg/) -> pkg/bin/nds_runner + pkg/config/game.toml
        var winRoot = Path.Combine(tmp.Path, "win-pkg");
        Directory.CreateDirectory(Path.Combine(winRoot, "bin"));
        Directory.CreateDirectory(Path.Combine(winRoot, "config"));
        File.WriteAllBytes(Path.Combine(winRoot, "bin", exeName), new byte[] { 1 });
        File.WriteAllText(Path.Combine(winRoot, "config", "game.toml"), "[system]\n");
        var winLocated = RomPreparationService.LocateEngineBinaries(winRoot);
        AssertEqual(winLocated.RunnerPath, Path.Combine(winRoot, "bin", exeName), "Windows runner path");
        AssertEqual(winLocated.ConfigPath, Path.Combine(winRoot, "config", "game.toml"), "Windows config path");

        // 4. Deep repository publish layout (7 levels below repo root: src/SkyRecompiled.Launcher/bin/Release/net8.0/osx-arm64/publish/)
        var repoSim = Path.Combine(tmp.Path, "repo-sim");
        var publishBase = Path.Combine(repoSim, "src", "SkyRecompiled.Launcher", "bin", "Release", "net8.0", "osx-arm64", "publish") + Path.DirectorySeparatorChar;
        Directory.CreateDirectory(publishBase);
        Directory.CreateDirectory(Path.Combine(repoSim, "build", "runner"));
        Directory.CreateDirectory(Path.Combine(repoSim, "config"));
        File.WriteAllBytes(Path.Combine(repoSim, "build", "runner", exeName), new byte[] { 1 });
        File.WriteAllText(Path.Combine(repoSim, "config", "game.toml"), "[system]\n");
        var publishLocated = RomPreparationService.LocateEngineBinaries(publishBase);
        AssertEqual(publishLocated.RunnerPath, Path.Combine(repoSim, "build", "runner", exeName), "Deep publish runner path");
        AssertEqual(publishLocated.ConfigPath, Path.Combine(repoSim, "config", "game.toml"), "Deep publish config path");

        // 5. Missing runner gracefully reports false in TryLocateEngineBinaries
        var emptyDir = Path.Combine(tmp.Path, "empty-base");
        Directory.CreateDirectory(emptyDir);
        Assert(!RomPreparationService.TryLocateEngineBinaries(out _, out _, out var err, emptyDir), "Missing runner should return false");
        Assert(!string.IsNullOrEmpty(err), "Error message populated");
    }

    private static void TestSkySettingsService()
    {
        using var tmp = new TempDir();
        var path = Path.Combine(tmp.Path, "sky_settings.toml");

        var custom = new SkyRuntimeSettings
        {
            AspectRatio = 3,
            HudLayout = 1,
            RefreshRate = 4,
            ShaderFilter = 4,
            WeatherAmbientBloom = false,
            ShowFpsMeter = true,
            AudioVolumePercent = 70,
            AudioMuted = true,
            ControllerPreset = 2,
            ControllerBindings = "12,13,14,15,2,1,3,4,10,11,7,5",
            KeyboardBindings = "82,81,80,79,29,27,22,4,20,26,40,42",
        };
        SkySettingsService.SaveAtomic(custom, path);
        var loaded = SkySettingsService.Load(path);

        AssertEqual(loaded.AspectRatio, 3, "AspectRatio");
        AssertEqual(loaded.HudLayout, 1, "HudLayout");
        AssertEqual(loaded.RefreshRate, 4, "RefreshRate");
        AssertEqual(loaded.ShaderFilter, 4, "ShaderFilter");
        AssertEqual(loaded.WeatherAmbientBloom, false, "WeatherAmbientBloom");
        AssertEqual(loaded.ShowFpsMeter, true, "ShowFpsMeter");
        AssertEqual(loaded.AudioVolumePercent, 70, "AudioVolumePercent");
        AssertEqual(loaded.AudioMuted, true, "AudioMuted");
        AssertEqual(loaded.ControllerPreset, 2, "ControllerPreset");
        AssertEqual(loaded.ControllerBindings, "12,13,14,15,2,1,3,4,10,11,7,5", "ControllerBindings");

        // Out-of-range clamping, inline TOML comments, and invalid bindings preservation
        var clamped = SkySettingsService.Parse(
            "[graphics]\naspect_ratio = 99 # clamp\nhud_layout = -3\n[audio]\nvolume_percent = 500\nmuted = true # inline comment\n[input]\ncontroller_bindings = \"bad\"\n");
        AssertEqual(clamped.AspectRatio, 3, "Clamped AspectRatio");
        AssertEqual(clamped.HudLayout, 0, "Clamped HudLayout");
        AssertEqual(clamped.AudioVolumePercent, 100, "Clamped AudioVolumePercent");
        AssertEqual(clamped.AudioMuted, true, "Inline comment stripped from bool");
        AssertEqual(clamped.ControllerBindings, new SkyRuntimeSettings().ControllerBindings, "Invalid CSV falls back to default");

        // Named controller presets serialize canonical bindings while Custom preserves user bindings
        var modernToml = SkySettingsService.Serialize(new SkyRuntimeSettings
        {
            ControllerPreset = 1,
            ControllerBindings = "12,13,14,15,1,2,3,4,10,11,7,5",
        });
        Assert(modernToml.Contains("controller_bindings = \"12,13,14,15,1,2,3,10,4,11,7,5\"", StringComparison.Ordinal),
            "ModernDungeon preset writes canonical binding array");

        var customToml = SkySettingsService.Serialize(new SkyRuntimeSettings
        {
            ControllerPreset = 3,
            ControllerBindings = "12,13,14,15,4,3,2,1,10,11,7,5",
        });
        Assert(customToml.Contains("controller_bindings = \"12,13,14,15,4,3,2,1,10,11,7,5\"", StringComparison.Ordinal),
            "Custom preset preserves user binding array");

        // TryLoad succeeds cleanly on absent file and reports error on unreadable file
        var absentOk = SkySettingsService.TryLoad(out _, out var absentErr, Path.Combine(tmp.Path, "nonexistent.toml"));
        Assert(absentOk && absentErr == null, "Absent sky_settings.toml is not treated as read failure");
        if (!OperatingSystem.IsWindows())
        {
            var unreadablePath = Path.Combine(tmp.Path, "unreadable.toml");
            File.WriteAllText(unreadablePath, "[graphics]\naspect_ratio = 1\n");
            File.SetUnixFileMode(unreadablePath, UnixFileMode.None);
            try
            {
                var readOk = SkySettingsService.TryLoad(out _, out var readErr, unreadablePath);
                Assert(!readOk && !string.IsNullOrEmpty(readErr), "Unreadable sky_settings.toml reports failure");
            }
            finally
            {
                File.SetUnixFileMode(unreadablePath, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            }
        }
    }

    private static void TestModManagerService()
    {
        using var tmp = new TempDir();
        var modsDir = Path.Combine(tmp.Path, "mods");
        var statePath = Path.Combine(tmp.Path, "mods_state.json");
        var manifestPath = Path.Combine(tmp.Path, "active_mods.json");
        Directory.CreateDirectory(modsDir);

        // 1. Folder without romfs/ is ignored even with invalid mod.toml
        var nonMod = Path.Combine(modsDir, "notes");
        Directory.CreateDirectory(nonMod);
        File.WriteAllText(Path.Combine(nonMod, "mod.toml"), "id = \"bad id\"\n");

        // 2. Two enabled mods with tie priority=10 -> alphabetical ID sort ("a_mod" then "z_mod", so "z_mod" wins!)
        var modA = Path.Combine(modsDir, "mod_a");
        Directory.CreateDirectory(Path.Combine(modA, "romfs", "data"));
        File.WriteAllText(Path.Combine(modA, "mod.toml"), "id = \"a_mod\" # comment\nname = \"Mod A\"\npriority = 10\n");
        File.WriteAllBytes(Path.Combine(modA, "romfs", "data", "shared.bin"), new byte[] { 1 });
        File.WriteAllBytes(Path.Combine(modA, "romfs", "data", "only_a.bin"), new byte[] { 2 });

        var modZ = Path.Combine(modsDir, "mod_z");
        Directory.CreateDirectory(Path.Combine(modZ, "romfs", "data"));
        File.WriteAllText(Path.Combine(modZ, "mod.toml"), "id = \"z_mod\"\nname = \"Mod Z\"\npriority = 10\n");
        File.WriteAllBytes(Path.Combine(modZ, "romfs", "data", "shared.bin"), new byte[] { 3 });

        // 3. Invalid priority mod recorded as issue without failing scan
        var badMod = Path.Combine(modsDir, "bad_mod");
        Directory.CreateDirectory(Path.Combine(badMod, "romfs"));
        File.WriteAllText(Path.Combine(badMod, "mod.toml"), "id = \"bad_mod\"\npriority = \"oops\"\n");

        // 4. Symlink inside romfs rejected
        if (!OperatingSystem.IsWindows())
        {
            var outsideFile = Path.Combine(tmp.Path, "outside.bin");
            File.WriteAllBytes(outsideFile, new byte[] { 9 });
            var symMod = Path.Combine(modsDir, "sym_mod");
            Directory.CreateDirectory(Path.Combine(symMod, "romfs"));
            File.WriteAllText(Path.Combine(symMod, "mod.toml"), "id = \"sym_mod\"\n");
            File.CreateSymbolicLink(Path.Combine(symMod, "romfs", "escape.bin"), outsideFile);
        }

        // 5. Invalid-romfs folder does not reserve ID for a subsequent valid folder
        if (!OperatingSystem.IsWindows())
        {
            var invalidFirst = Path.Combine(modsDir, "00_broken_dup");
            Directory.CreateDirectory(Path.Combine(invalidFirst, "romfs", "data"));
            File.WriteAllText(Path.Combine(invalidFirst, "mod.toml"), "id = \"dup_ok\"\n");
            var ext = Path.Combine(tmp.Path, "outside2.bin");
            File.WriteAllBytes(ext, new byte[] { 1 });
            File.CreateSymbolicLink(Path.Combine(invalidFirst, "romfs", "data", "bad.bin"), ext);

            var validSecond = Path.Combine(modsDir, "01_valid_dup");
            Directory.CreateDirectory(Path.Combine(validSecond, "romfs", "data"));
            File.WriteAllText(Path.Combine(validSecond, "mod.toml"), "id = \"dup_ok\"\npriority = 30\n");
            File.WriteAllBytes(Path.Combine(validSecond, "romfs", "data", "good.bin"), new byte[] { 5 });
        }

        var scan = ModManagerService.ScanMods(modsDir, statePath);
        int expectedValid = OperatingSystem.IsWindows() ? 2 : 3;
        AssertEqual(scan.Mods.Count, expectedValid, "Valid mods count");
        AssertEqual(scan.Mods[0].Id, "a_mod", "First layer");
        AssertEqual(scan.Mods[1].Id, "z_mod", "Second layer");
        AssertEqual(scan.Conflicts.Count, 1, "One overlapping file conflict");
        AssertEqual(scan.Conflicts[0].VirtualPath, "data/shared.bin", "Conflict path");
        AssertEqual(scan.Conflicts[0].WinnerModId, "z_mod", "Tie-breaker winner");
        Assert(scan.Issues.Count >= 1, "Bad mod captured as validation issue");

        // Persisted priority override flips winner to a_mod
        ModManagerService.SaveStateAtomic(new Dictionary<string, ModStateOverride>
        {
            ["a_mod"] = new ModStateOverride(true, 50),
            ["z_mod"] = new ModStateOverride(true, 10),
        }, statePath);
        var rescanned = ModManagerService.ScanMods(modsDir, statePath);
        AssertEqual(rescanned.Mods[0].Id, "z_mod", "Lower priority first");
        AssertEqual(rescanned.Mods[^1].Id, "a_mod", "Highest priority last");
        AssertEqual(rescanned.Conflicts[0].WinnerModId, "a_mod", "Higher priority wins");

        ModManagerService.ExportActiveModsManifest(rescanned, manifestPath);
        Assert(File.ReadAllText(manifestPath).Contains("\"a_mod\"", StringComparison.Ordinal), "Manifest contains active mods");

        // Null override value in mods_state.json is ignored gracefully
        File.WriteAllText(statePath, "{\"a_mod\": null, \"z_mod\": {\"Enabled\": true, \"Priority\": 10}}\n");
        var afterNull = ModManagerService.ScanMods(modsDir, statePath);
        AssertEqual(afterNull.Mods.Count, expectedValid, "Null override does not crash scan");
        AssertEqual(afterNull.Mods.First(m => m.Id == "a_mod").Priority, 10, "a_mod falls back to metadata priority");

        // Path validation checks
        AssertThrows<ArgumentException>(() => ModManagerService.ValidateVirtualPath("../escape"), "Parent traversal");
        AssertThrows<ArgumentException>(() => ModManagerService.ValidateVirtualPath("C:/abs"), "Drive colon");

        // ImportModFolder success
        var importSrc = Path.Combine(tmp.Path, "import_pack");
        Directory.CreateDirectory(Path.Combine(importSrc, "romfs", "sound"));
        File.WriteAllText(Path.Combine(importSrc, "mod.toml"), "id = \"sound_mod\"\nname = \"Sound Pack\"\npriority = 75\n");
        File.WriteAllBytes(Path.Combine(importSrc, "romfs", "sound", "bgm.smd"), new byte[] { 7, 8 });
        var imported = ModManagerService.ImportModFolder(importSrc, modsDir, statePath);
        AssertEqual(imported.Id, "sound_mod", "Imported mod ID");
        AssertEqual(imported.FileCount, 1, "Imported file count");
    }

    private static void TestSaveManagementService()
    {
        using var tmp = new TempDir();
        var savesDir = Path.Combine(tmp.Path, "saves");
        var statesDir = Path.Combine(tmp.Path, "savestates");
        Directory.CreateDirectory(savesDir);
        Directory.CreateDirectory(statesDir);
        File.WriteAllBytes(Path.Combine(statesDir, "slot1.ss0"), new byte[] { 1, 2, 3 });

        var missingInfo = SaveManagementService.InspectSave(savesDir, statesDir);
        Assert(!missingInfo.Exists, "Missing save reported");
        AssertEqual(missingInfo.SavestateCount, 1, "Savestate count");

        // Create valid 128 KiB EEPROM save
        var savePath = SaveManagementService.GetSaveFilePath(savesDir);
        var originalSave = new byte[SaveManagementService.ExpectedEepromSaveBytes];
        Array.Fill(originalSave, (byte)0x42);
        File.WriteAllBytes(savePath, originalSave);

        var validInfo = SaveManagementService.InspectSave(savesDir, statesDir);
        Assert(validInfo.Exists && validInfo.IsValidEepromSize, "131072-byte EEPROM save is valid");
        AssertEqual(validInfo.Sha256Short.Length, 16, "SHA-256 prefix length");

        var backup = SaveManagementService.CreateBackup(savesDir);
        Assert(backup.IsValidEepromSize, "Backup has valid 128 KiB size");
        AssertEqual(SaveManagementService.ListBackups(savesDir).Count, 1, "One backup listed");

        // Modify active save, then restore -> safety pre-restore backup created + original bytes restored
        var modifiedSave = new byte[SaveManagementService.ExpectedEepromSaveBytes];
        Array.Fill(modifiedSave, (byte)0x99);
        File.WriteAllBytes(savePath, modifiedSave);

        SaveManagementService.RestoreBackup(backup.FullPath, savesDir);
        Assert(File.ReadAllBytes(savePath).SequenceEqual(originalSave), "Active save restored to original bytes");
        AssertEqual(SaveManagementService.ListBackups(savesDir).Count, 2, "Pre-restore safety backup created");

        // Reject invalid-size backup restore
        var corruptBackup = Path.Combine(SaveManagementService.GetBackupsDir(savesDir), "corrupt.sav");
        File.WriteAllBytes(corruptBackup, new byte[] { 1, 2, 3, 4 });
        AssertThrows<InvalidDataException>(
            () => SaveManagementService.RestoreBackup(corruptBackup, savesDir),
            "Refuse corrupted non-128KiB save restore");
    }
}
