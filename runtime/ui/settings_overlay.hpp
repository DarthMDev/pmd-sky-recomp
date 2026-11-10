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

/// Interactive In-Game Settings Overlay powered by `third_party/recomp-ui` (Task 3.4).
class SkySettingsOverlay {
public:
    explicit SkySettingsOverlay(const SkySettingsState& initial_state = {}) noexcept;
    ~SkySettingsOverlay() noexcept;

    SkySettingsOverlay(const SkySettingsOverlay&) = delete;
    SkySettingsOverlay& operator=(const SkySettingsOverlay&) = delete;

    [[nodiscard]] bool IsValid() const noexcept { return ui_ != nullptr; }
    [[nodiscard]] bool IsVisible() const noexcept;
    void SetVisible(bool visible) noexcept;

    [[nodiscard]] const SkySettingsState& GetState() const noexcept { return state_; }
    void SetState(const SkySettingsState& state) noexcept;

    [[nodiscard]] size_t GetSelectedIndex() const noexcept;

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

    /// Resolves the DS keypad bitmask mapping for the active controller preset.
    [[nodiscard]] DsButtonMapping GetActiveButtonMapping() const noexcept;

    /// Renders the `recomp-ui` settings overlay onto `framebuffer_argb` (`width * height` ARGB8888).
    bool RenderArgb8888(std::span<uint32_t> framebuffer_argb, int width, int height) noexcept;

    /// Serializes `SkySettingsState` to a human-readable TOML string.
    [[nodiscard]] std::string SerializeToToml() const;

    /// Parses `SkySettingsState` from a TOML string.
    bool LoadFromToml(std::string_view toml_text) noexcept;

private:
    static int CallbackGetValue(void* context, const RecompRuntimeUiItem* item, int* value_out) noexcept;
    static int CallbackSetValue(void* context, const RecompRuntimeUiItem* item, int value) noexcept;
    static int CallbackRunAction(void* context, const RecompRuntimeUiItem* item) noexcept;
    static int CallbackIsEnabled(void* context, const RecompRuntimeUiItem* item) noexcept;

    SkySettingsState state_{};
    RecompRuntimeUi* ui_ = nullptr;
    bool prev_chord_held_ = false;
};

} // namespace skyrecomp::ui
