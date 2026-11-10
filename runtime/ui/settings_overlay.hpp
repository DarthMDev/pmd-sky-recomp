#pragma once

#include "../camera/dungeon_camera.hpp"
#include "../camera/subpixel_interpolator.hpp"
#include "../gpu2d/widescreen_ppu.hpp"
#include "../shaders/weather_shader_pipeline.hpp"

extern "C" {
#include "recomp_runtime_ui.h"
}

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace skyrecomp::ui {

/// Controller button preset for PMD: Explorers of Sky (Task 3.4).
enum class ControllerRemapPreset : uint8_t {
    StandardNintendoDs = 0, // A=Confirm/Attack, B=Cancel/Menu, X=Menu, Y=Sort/Diagonal, L/R=Moves/Throw
    ModernDungeon = 1,      // South=Confirm, East=Cancel, LB=Diagonal Lock, RB=Set Move, LT=Look Around
    SwappedConfirmCancel = 2, // Swaps A <-> B for Western Xbox/PC controller muscle memory
};

/// Resolved DS keypad bitmask remap table for a controller preset.
struct DsButtonMapping {
    uint16_t confirm_bit = (1u << 0);    // KEY_A
    uint16_t cancel_bit = (1u << 1);     // KEY_B
    uint16_t menu_bit = (1u << 10);      // KEY_X
    uint16_t sort_bit = (1u << 11);      // KEY_Y
    uint16_t shoulder_l_bit = (1u << 9); // KEY_L
    uint16_t shoulder_r_bit = (1u << 8); // KEY_R
};

/// Complete user-configurable settings state managed by the in-game `recomp-ui` overlay.
struct SkySettingsState {
    gpu2d::AspectRatioPreset aspect_ratio = gpu2d::AspectRatioPreset::Widescreen_16_9;
    gpu2d::HudLayoutMode hud_layout = gpu2d::HudLayoutMode::DualScreenAnchored;
    camera::RefreshRateMode refresh_rate = camera::RefreshRateMode::ProMotion_120Hz;
    shaders::ShaderFilterPreset shader_filter = shaders::ShaderFilterPreset::SharpBilinear;
    bool weather_ambient_bloom = true;
    int audio_volume_percent = 100; // 0 .. 100 (step 10)
    bool audio_muted = false;
    ControllerRemapPreset controller_preset = ControllerRemapPreset::StandardNintendoDs;
};

/// Host input snapshot for driving the overlay toggle (`F1` / `Guide` / `Select+Start`) and navigation.
struct OverlayInputState {
    bool f1_pressed = false;
    bool gamepad_guide_pressed = false;
    bool gamepad_select_held = false;
    bool gamepad_start_held = false;
    std::optional<RecompRuntimeUiInput> nav_input = std::nullopt;
};

/// Menu item indices inside the SkyRecompiled settings overlay.
enum class SettingsItemIndex : size_t {
    AspectRatio = 0,
    HudLayout = 1,
    FrameInterpolation = 2,
    ShaderFilter = 3,
    WeatherBloom = 4,
    AudioVolume = 5,
    ControllerMapping = 6,
    ResumeGame = 7,
    Count = 8,
};

/// Live runtime consumers bound to `SkySettingsOverlay` so changes made in the overlay or loaded
/// from disk automatically propagate to graphics subsystems, audio output, input remapping, and disk.
struct RuntimeConsumerBindings {
    gpu2d::WidescreenPpu* ppu = nullptr;
    camera::DungeonCameraHook* camera_hook = nullptr;
    camera::SubpixelInterpolator* interpolator = nullptr;
    shaders::WeatherShaderPipeline* shader_pipeline = nullptr;
    std::function<void(int effective_volume_percent, bool muted)> on_audio_settings_changed{};
    std::function<void(const DsButtonMapping& mapping)> on_controller_mapping_changed{};
    std::filesystem::path config_file_path{};
};

/// Interactive In-Game Settings Overlay powered by `third_party/recomp-ui` (Task 3.4).
class SkySettingsOverlay {
public:
    /// Constructs the in-game settings overlay with the given initial state.
    explicit SkySettingsOverlay(const SkySettingsState& initial_state = {}) noexcept;

    /// Destroys the underlying `RecompRuntimeUi` instance.
    ~SkySettingsOverlay() noexcept;

    SkySettingsOverlay(const SkySettingsOverlay&) = delete;
    SkySettingsOverlay& operator=(const SkySettingsOverlay&) = delete;

    /// Returns true if the underlying `RecompRuntimeUi` context was initialized.
    [[nodiscard]] bool IsValid() const noexcept { return ui_ != nullptr; }

    /// Returns true when the settings overlay menu is currently open on screen.
    [[nodiscard]] bool IsVisible() const noexcept;

    /// Opens or closes the settings overlay menu.
    void SetVisible(bool visible) noexcept;

    /// Returns the current settings state.
    [[nodiscard]] const SkySettingsState& GetState() const noexcept { return state_; }

    /// Updates the settings state and immediately synchronizes any bound runtime consumers.
    void SetState(const SkySettingsState& state) noexcept;

    /// Returns the index of the currently highlighted row in the settings overlay.
    [[nodiscard]] size_t GetSelectedIndex() const noexcept;

    /// Binds live graphics subsystems, audio/controller callbacks, and an optional config file path,
    /// immediately applying the current settings state to all bound consumers.
    void BindRuntimeConsumers(RuntimeConsumerBindings bindings) noexcept;

    /// Processes host keyboard (`F1`) and gamepad (`Guide` or `Select+Start`) toggle inputs
    /// plus `recomp-ui` navigation/adjustment actions.
    /// Returns true if the overlay is currently visible (meaning game input should be paused/consumed).
    bool HandleInput(const OverlayInputState& input) noexcept;

    /// Synchronizes the current `SkySettingsState` into live Milestone 3 runtime subsystems.
    void ApplyToSubsystems(
        gpu2d::WidescreenPpu& ppu,
        camera::DungeonCameraHook& camera_hook,
        camera::SubpixelInterpolator& interpolator,
        shaders::WeatherShaderPipeline& shader_pipeline) const noexcept;

    /// Returns the effective output volume percentage in [0, 100] (0 when muted).
    [[nodiscard]] int GetEffectiveAudioVolumePercent() const noexcept {
        return state_.audio_muted ? 0 : state_.audio_volume_percent;
    }

    /// Scales a buffer of signed 16-bit PCM audio samples in-place using the active volume and mute state.
    void ScaleAudioSamplesInt16(std::span<int16_t> pcm_samples) const noexcept;

    /// Resolves the DS keypad bitmask mapping for the active controller preset.
    [[nodiscard]] DsButtonMapping GetActiveButtonMapping() const noexcept;

    /// Remaps a logical DS pressed-button bitmask (where bit 0=A, 1=B, 8=R, 9=L, 10=X, 11=Y)
    /// into the effective hardware keypad mask according to `GetActiveButtonMapping()`.
    [[nodiscard]] uint16_t RemapDsKeyinputMask(uint16_t logical_pressed_mask) const noexcept;

    /// Renders the `recomp-ui` settings overlay onto `framebuffer_argb` (`width * height` ARGB8888).
    bool RenderArgb8888(std::span<uint32_t> framebuffer_argb, int width, int height) noexcept;

    /// Serializes `SkySettingsState` to a human-readable TOML string.
    [[nodiscard]] std::string SerializeToToml() const;

    /// Parses `SkySettingsState` from a TOML string and applies the loaded settings to bound consumers.
    bool LoadFromToml(std::string_view toml_text) noexcept;

    /// Saves the current `SkySettingsState` as TOML to the specified file path.
    bool SaveToFile(const std::filesystem::path& path) const noexcept;

    /// Loads `SkySettingsState` from a TOML file on disk and applies it to all bound consumers.
    bool LoadFromFile(const std::filesystem::path& path) noexcept;

private:
    /// Synchronizes current graphics, audio, and controller settings to all bound consumers.
    void NotifyBoundConsumers() noexcept;

    /// `recomp-ui` callback that reads the current integer value for a menu item.
    static int CallbackGetValue(void* context, const RecompRuntimeUiItem* item, int* value_out) noexcept;
    /// `recomp-ui` callback that updates the integer value for a menu item.
    static int CallbackSetValue(void* context, const RecompRuntimeUiItem* item, int value) noexcept;
    /// `recomp-ui` callback that executes a button action item (e.g., Resume Game).
    static int CallbackRunAction(void* context, const RecompRuntimeUiItem* item) noexcept;
    /// `recomp-ui` callback that reports whether a menu item is currently enabled.
    static int CallbackIsEnabled(void* context, const RecompRuntimeUiItem* item) noexcept;
    /// `recomp-ui` callback invoked after any value change to persist settings to disk.
    static void CallbackSave(void* context) noexcept;

    SkySettingsState state_{};
    RuntimeConsumerBindings bindings_{};
    RecompRuntimeUi* ui_ = nullptr;
    bool prev_chord_held_ = false;
};

} // namespace skyrecomp::ui
