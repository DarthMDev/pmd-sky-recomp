using System;
using System.Collections.Generic;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using SkyRecompiled.Launcher.Models;
using SkyRecompiled.Launcher.Services;

namespace SkyRecompiled.Launcher.ViewModels;

public sealed partial class SettingsViewModel : ViewModelBase
{
    private readonly string _skySettingsPath;
    private readonly string _launcherSettingsPath;

    public SettingsViewModel()
        : this(
            RomPreparationService.GetSkySettingsPath(),
            RomPreparationService.GetSettingsPath())
    {
    }

    public SettingsViewModel(string skySettingsPath, string launcherSettingsPath)
    {
        _skySettingsPath = skySettingsPath;
        _launcherSettingsPath = launcherSettingsPath;
        Reload();
    }

    public Action? OnSettingsSaved { get; set; }

    public IReadOnlyList<string> AspectRatioChoices => SkySettingsService.AspectRatioLabels;
    public IReadOnlyList<string> HudLayoutChoices => SkySettingsService.HudLayoutLabels;
    public IReadOnlyList<string> RefreshRateChoices => SkySettingsService.RefreshRateLabels;
    public IReadOnlyList<string> ShaderFilterChoices => SkySettingsService.ShaderFilterLabels;
    public IReadOnlyList<string> ControllerPresetChoices => SkySettingsService.ControllerPresetLabels;

    [ObservableProperty]
    private int _aspectRatioIndex = 2;

    [ObservableProperty]
    private int _hudLayoutIndex = 0;

    [ObservableProperty]
    private int _refreshRateIndex = 1;

    [ObservableProperty]
    private int _shaderFilterIndex = 0;

    [ObservableProperty]
    private bool _weatherAmbientBloom = true;

    [ObservableProperty]
    private bool _showFpsMeter = false;

    [ObservableProperty]
    private int _audioVolumePercent = 100;

    [ObservableProperty]
    private bool _audioMuted = false;

    [ObservableProperty]
    private int _controllerPresetIndex = 0;

    [ObservableProperty]
    private string _rescueEndpoint = LauncherPreferences.DefaultRescueEndpoint;

    [ObservableProperty]
    private bool _rescueOfflineMode = false;

    [ObservableProperty]
    private string _statusMessage = "Changes are saved atomically to sky_settings.toml and settings.toml.";

    private string _controllerBindings = new SkyRuntimeSettings().ControllerBindings;
    private string _keyboardBindings = new SkyRuntimeSettings().KeyboardBindings;

    public string UserDataDir => RomPreparationService.GetUserStorageDir();
    public string SkySettingsFileDisplay => _skySettingsPath;
    public string LauncherSettingsFileDisplay => _launcherSettingsPath;

    public SkyRuntimeSettings BuildRuntimeSettings() => new()
    {
        AspectRatio = AspectRatioIndex,
        HudLayout = HudLayoutIndex,
        RefreshRate = RefreshRateIndex,
        ShaderFilter = ShaderFilterIndex,
        WeatherAmbientBloom = WeatherAmbientBloom,
        ShowFpsMeter = ShowFpsMeter,
        AudioVolumePercent = AudioVolumePercent,
        AudioMuted = AudioMuted,
        ControllerPreset = ControllerPresetIndex,
        ControllerBindings = _controllerBindings,
        KeyboardBindings = _keyboardBindings,
    };

    public LauncherPreferences BuildPreferences() => new()
    {
        RescueEndpoint = LauncherPreferences.NormalizeEndpoint(RescueEndpoint),
        RescueOfflineMode = RescueOfflineMode,
    };

    [RelayCommand]
    public void Reload()
    {
        var rt = SkySettingsService.Load(_skySettingsPath);
        AspectRatioIndex = rt.AspectRatio;
        HudLayoutIndex = rt.HudLayout;
        RefreshRateIndex = rt.RefreshRate;
        ShaderFilterIndex = rt.ShaderFilter;
        WeatherAmbientBloom = rt.WeatherAmbientBloom;
        ShowFpsMeter = rt.ShowFpsMeter;
        AudioVolumePercent = rt.AudioVolumePercent;
        AudioMuted = rt.AudioMuted;
        ControllerPresetIndex = rt.ControllerPreset;
        _controllerBindings = rt.ControllerBindings;
        _keyboardBindings = rt.KeyboardBindings;

        var prefs = RomPreparationService.LoadPreferences(_launcherSettingsPath);
        RescueEndpoint = prefs.RescueEndpoint;
        RescueOfflineMode = prefs.RescueOfflineMode;
        StatusMessage = "Loaded current configuration from disk.";
    }

    [RelayCommand]
    public void SaveSettings()
    {
        try
        {
            var rt = BuildRuntimeSettings();
            var prefs = BuildPreferences();
            SkySettingsService.SaveAtomic(rt, _skySettingsPath);
            RomPreparationService.SavePreferences(prefs, _launcherSettingsPath);
            RescueEndpoint = prefs.RescueEndpoint;
            StatusMessage = $"Settings saved ({DateTime.Now:HH:mm:ss}).";
            OnSettingsSaved?.Invoke();
        }
        catch (Exception ex)
        {
            StatusMessage = $"Failed to save settings: {ex.Message}";
        }
    }

    [RelayCommand]
    public void RestoreDefaults()
    {
        var defaults = new SkyRuntimeSettings();
        AspectRatioIndex = defaults.AspectRatio;
        HudLayoutIndex = defaults.HudLayout;
        RefreshRateIndex = defaults.RefreshRate;
        ShaderFilterIndex = defaults.ShaderFilter;
        WeatherAmbientBloom = defaults.WeatherAmbientBloom;
        ShowFpsMeter = defaults.ShowFpsMeter;
        AudioVolumePercent = defaults.AudioVolumePercent;
        AudioMuted = defaults.AudioMuted;
        ControllerPresetIndex = defaults.ControllerPreset;
        _controllerBindings = defaults.ControllerBindings;
        _keyboardBindings = defaults.KeyboardBindings;

        RescueEndpoint = LauncherPreferences.DefaultRescueEndpoint;
        RescueOfflineMode = false;

        SaveSettings();
        StatusMessage = "Restored default graphics, audio, input, and rescue settings.";
    }
}
