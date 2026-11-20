using System;
using System.Globalization;
using System.IO;
using System.Text;
using SkyRecompiled.Launcher.Models;

namespace SkyRecompiled.Launcher.Services;

/// <summary>
/// Reads and atomically writes <c>sky_settings.toml</c> in exact parity with
/// <c>runtime/ui/settings_overlay.cpp</c>.
/// </summary>
public static class SkySettingsService
{
    public static readonly string[] AspectRatioLabels =
    {
        "4:3 (256x192)",
        "16:10 (308x192)",
        "16:9 (342x192)",
        "21:9 (448x192)",
    };

    public static readonly string[] HudLayoutLabels =
    {
        "Dual-Screen (Default)",
        "Hybrid",
    };

    public static readonly string[] RefreshRateLabels =
    {
        "60Hz (Vanilla)",
        "120Hz (ProMotion)",
        "144Hz",
        "240Hz",
        "Uncapped VRR",
    };

    public static readonly string[] ShaderFilterLabels =
    {
        "Nearest",
        "Sharp Bilinear",
        "Pixellate",
        "NDS LCD Grid",
        "CRT Scanline",
    };

    public static readonly string[] ControllerPresetLabels =
    {
        "Standard DS",
        "Modern Dungeon",
        "Swapped B/A",
        "Custom",
    };

    public static SkyRuntimeSettings Load(string? path = null)
    {
        var targetPath = path ?? RomPreparationService.GetSkySettingsPath();
        var settings = new SkyRuntimeSettings();
        if (!File.Exists(targetPath))
            return settings;

        try
        {
            var text = File.ReadAllText(targetPath, Encoding.UTF8);
            ParseInto(text, settings);
        }
        catch
        {
            return new SkyRuntimeSettings();
        }
        return settings;
    }

    public static SkyRuntimeSettings Parse(string tomlText)
    {
        var settings = new SkyRuntimeSettings();
        ParseInto(tomlText, settings);
        return settings;
    }

    private static void ParseInto(string tomlText, SkyRuntimeSettings target)
    {
        foreach (var rawLine in tomlText.Split('\n'))
        {
            var line = rawLine.Trim();
            if (string.IsNullOrEmpty(line) || line.StartsWith('#') || line.StartsWith('['))
                continue;

            var eq = line.IndexOf('=');
            if (eq < 0)
                continue;

            var key = line[..eq].Trim();
            var val = line[(eq + 1)..].Trim();

            switch (key)
            {
                case "aspect_ratio":
                    if (int.TryParse(val, NumberStyles.Integer, CultureInfo.InvariantCulture, out var ar))
                        target.AspectRatio = Math.Clamp(ar, 0, 3);
                    break;
                case "hud_layout":
                    if (int.TryParse(val, NumberStyles.Integer, CultureInfo.InvariantCulture, out var hl))
                        target.HudLayout = Math.Clamp(hl, 0, 1);
                    break;
                case "refresh_rate":
                    if (int.TryParse(val, NumberStyles.Integer, CultureInfo.InvariantCulture, out var rr))
                        target.RefreshRate = Math.Clamp(rr, 0, 4);
                    break;
                case "shader_filter":
                    if (int.TryParse(val, NumberStyles.Integer, CultureInfo.InvariantCulture, out var sf))
                        target.ShaderFilter = Math.Clamp(sf, 0, 4);
                    break;
                case "weather_ambient_bloom":
                    target.WeatherAmbientBloom = val == "true" || val == "1";
                    break;
                case "show_fps_meter":
                    target.ShowFpsMeter = val == "true" || val == "1";
                    break;
                case "volume_percent":
                    if (int.TryParse(val, NumberStyles.Integer, CultureInfo.InvariantCulture, out var vp))
                        target.AudioVolumePercent = Math.Clamp(vp, 0, 100);
                    break;
                case "muted":
                    target.AudioMuted = val == "true" || val == "1";
                    break;
                case "controller_preset":
                    if (int.TryParse(val, NumberStyles.Integer, CultureInfo.InvariantCulture, out var cp))
                        target.ControllerPreset = Math.Clamp(cp, 0, 3);
                    break;
                case "controller_bindings":
                    var cb = StripQuotes(val);
                    if (IsValidBindingsCsv(cb))
                        target.ControllerBindings = cb;
                    break;
                case "keyboard_bindings":
                    var kb = StripQuotes(val);
                    if (IsValidBindingsCsv(kb))
                        target.KeyboardBindings = kb;
                    break;
            }
        }
    }

    private static string StripQuotes(string s)
    {
        if (s.Length >= 2 && s[0] == '"' && s[^1] == '"')
            return s[1..^1];
        return s;
    }

    private static bool IsValidBindingsCsv(string csv)
    {
        var parts = csv.Split(',');
        if (parts.Length != 12)
            return false;
        foreach (var p in parts)
        {
            if (!int.TryParse(p.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out _))
                return false;
        }
        return true;
    }

    public static string Serialize(SkyRuntimeSettings settings)
    {
        var aspect = Math.Clamp(settings.AspectRatio, 0, 3);
        var hud = Math.Clamp(settings.HudLayout, 0, 1);
        var refresh = Math.Clamp(settings.RefreshRate, 0, 4);
        var shader = Math.Clamp(settings.ShaderFilter, 0, 4);
        var volume = Math.Clamp(settings.AudioVolumePercent, 0, 100);
        var preset = Math.Clamp(settings.ControllerPreset, 0, 3);
        var ctrlBindings = IsValidBindingsCsv(settings.ControllerBindings)
            ? settings.ControllerBindings
            : new SkyRuntimeSettings().ControllerBindings;
        var keyBindings = IsValidBindingsCsv(settings.KeyboardBindings)
            ? settings.KeyboardBindings
            : new SkyRuntimeSettings().KeyboardBindings;

        var sb = new StringBuilder();
        sb.Append("[graphics]\n");
        sb.Append($"aspect_ratio = {aspect.ToString(CultureInfo.InvariantCulture)}\n");
        sb.Append($"hud_layout = {hud.ToString(CultureInfo.InvariantCulture)}\n");
        sb.Append($"refresh_rate = {refresh.ToString(CultureInfo.InvariantCulture)}\n");
        sb.Append($"shader_filter = {shader.ToString(CultureInfo.InvariantCulture)}\n");
        sb.Append($"weather_ambient_bloom = {(settings.WeatherAmbientBloom ? "true" : "false")}\n");
        sb.Append($"show_fps_meter = {(settings.ShowFpsMeter ? "true" : "false")}\n\n");
        sb.Append("[audio]\n");
        sb.Append($"volume_percent = {volume.ToString(CultureInfo.InvariantCulture)}\n");
        sb.Append($"muted = {(settings.AudioMuted ? "true" : "false")}\n\n");
        sb.Append("[input]\n");
        sb.Append($"controller_preset = {preset.ToString(CultureInfo.InvariantCulture)}\n");
        sb.Append($"controller_bindings = \"{ctrlBindings}\"\n");
        sb.Append($"keyboard_bindings = \"{keyBindings}\"\n");
        return sb.ToString();
    }

    public static void SaveAtomic(SkyRuntimeSettings settings, string? path = null)
    {
        var targetPath = path ?? RomPreparationService.GetSkySettingsPath();
        var dir = Path.GetDirectoryName(targetPath);
        if (!string.IsNullOrEmpty(dir))
            Directory.CreateDirectory(dir);

        var content = Serialize(settings);
        var tmpPath = Path.Combine(dir ?? ".", $".{Path.GetFileName(targetPath)}.{Guid.NewGuid():N}.tmp");
        try
        {
            File.WriteAllText(tmpPath, content, new UTF8Encoding(false));
            File.Move(tmpPath, targetPath, overwrite: true);
        }
        finally
        {
            if (File.Exists(tmpPath))
                File.Delete(tmpPath);
        }
    }

    public static string FormatSummary(SkyRuntimeSettings settings)
    {
        var ar = AspectRatioLabels[Math.Clamp(settings.AspectRatio, 0, AspectRatioLabels.Length - 1)];
        var hl = HudLayoutLabels[Math.Clamp(settings.HudLayout, 0, HudLayoutLabels.Length - 1)];
        var rr = RefreshRateLabels[Math.Clamp(settings.RefreshRate, 0, RefreshRateLabels.Length - 1)];
        var sf = ShaderFilterLabels[Math.Clamp(settings.ShaderFilter, 0, ShaderFilterLabels.Length - 1)];
        var audio = settings.AudioMuted ? "Muted" : $"{Math.Clamp(settings.AudioVolumePercent, 0, 100)}%";
        return $"{ar} • {hl} • {rr} • {sf} • Audio {audio}";
    }
}
