using System;
using System.Collections.Generic;

namespace SkyRecompiled.Launcher.Models;

/// <summary>Result of validating a Nintendo DS Explorers of Sky cartridge dump.</summary>
public sealed record RomInfo(string Path, string Region, string Sha1, bool IsValid, long SizeBytes = 0);

/// <summary>
/// User-configurable graphics, audio, and input settings synchronized with
/// <c>sky_settings.toml</c> and <c>runtime/ui/settings_overlay.cpp</c>.
/// </summary>
public sealed class SkyRuntimeSettings
{
    public int AspectRatio { get; set; } = 2; // 0=4:3, 1=16:10, 2=16:9, 3=21:9
    public int HudLayout { get; set; } = 0; // 0=Dual-Screen, 1=Hybrid
    public int RefreshRate { get; set; } = 1; // 0=60Hz, 1=120Hz, 2=144Hz, 3=240Hz, 4=Uncapped VRR
    public int ShaderFilter { get; set; } = 0; // 0=Nearest, 1=Sharp Bilinear, 2=Pixellate, 3=NDS LCD Grid, 4=CRT Scanline
    public bool WeatherAmbientBloom { get; set; } = true;
    public bool ShowFpsMeter { get; set; } = false;
    public int AudioVolumePercent { get; set; } = 100;
    public bool AudioMuted { get; set; } = false;
    public int ControllerPreset { get; set; } = 0; // 0=Standard DS, 1=Modern Dungeon, 2=Swapped B/A, 3=Custom
    public string ControllerBindings { get; set; } = "12,13,14,15,1,2,3,4,10,11,7,5";
    public string KeyboardBindings { get; set; } = "82,81,80,79,29,27,22,4,20,26,40,42";

    public SkyRuntimeSettings Clone() => new()
    {
        AspectRatio = AspectRatio,
        HudLayout = HudLayout,
        RefreshRate = RefreshRate,
        ShaderFilter = ShaderFilter,
        WeatherAmbientBloom = WeatherAmbientBloom,
        ShowFpsMeter = ShowFpsMeter,
        AudioVolumePercent = AudioVolumePercent,
        AudioMuted = AudioMuted,
        ControllerPreset = ControllerPreset,
        ControllerBindings = ControllerBindings,
        KeyboardBindings = KeyboardBindings,
    };
}

/// <summary>Launcher preferences stored in <c>settings.toml</c> alongside <c>rom_path</c>.</summary>
public sealed class LauncherPreferences
{
    public const string DefaultRescueEndpoint = "https://sky-rescue-worker.darthanonymous1.workers.dev";

    public string RescueEndpoint { get; set; } = DefaultRescueEndpoint;
    public bool RescueOfflineMode { get; set; } = false;

    public static string NormalizeEndpoint(string? endpoint)
    {
        var trimmed = (endpoint ?? string.Empty).Trim().TrimEnd('/');
        return trimmed.Length == 0 ? DefaultRescueEndpoint : trimmed;
    }
}

/// <summary>Metadata and persisted enable/priority state for one discovered mod layer.</summary>
public sealed record ModEntry(
    string Id,
    string Name,
    string Author,
    string Version,
    int Priority,
    int DefaultPriority,
    string Description,
    bool Enabled,
    bool DefaultEnabled,
    string FolderPath,
    int FileCount,
    IReadOnlyList<string> VirtualPaths);

/// <summary>Conflict entry identifying all enabled mods touching a NitroFS path and the winner.</summary>
public sealed record ModConflict(
    string VirtualPath,
    IReadOnlyList<string> ModChain,
    string WinnerModId);

/// <summary>Validation problem discovered while scanning a mod folder or metadata file.</summary>
public sealed record ModValidationIssue(
    string FolderName,
    string Message);

/// <summary>Persisted per-mod enable and priority overrides stored in <c>mods_state.json</c>.</summary>
public sealed record ModStateOverride(bool Enabled, int Priority);

/// <summary>Aggregated result of scanning and ordering <c>mods/</c>.</summary>
public sealed record ModScanResult(
    IReadOnlyList<ModEntry> Mods,
    IReadOnlyList<ModConflict> Conflicts,
    IReadOnlyList<ModValidationIssue> Issues);

/// <summary>Inspection summary of the active 128 KiB EEPROM save file and savestates.</summary>
public sealed record SaveFileInfo(
    string Path,
    bool Exists,
    long SizeBytes,
    bool IsValidEepromSize,
    DateTimeOffset? LastModifiedUtc,
    string Sha256Short,
    int SavestateCount);

/// <summary>Metadata for a timestamped save backup stored under <c>saves/backups/</c>.</summary>
public sealed record SaveBackupInfo(
    string FileName,
    string FullPath,
    long SizeBytes,
    bool IsValidEepromSize,
    DateTimeOffset CreatedUtc);
