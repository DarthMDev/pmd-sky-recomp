#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace skyrecomp::gpu2d {

inline constexpr int kVanillaWidth = 256;
inline constexpr int kVanillaHeight = 192;
inline constexpr int kMaxWidescreenWidth = 448;

/// Supported native widescreen aspect ratios for the 2D PPU rasterizer.
enum class AspectRatioPreset : uint8_t {
    Vanilla_4_3 = 0,   // 256x192 (4:3)
    SteamDeck_16_10 = 1, // 308x192 (~16:10)
    Widescreen_16_9 = 2, // 342x192 (16:9)
    Ultrawide_21_9 = 3,  // 448x192 (21:9)
};

/// HUD layout mode for widescreen presentation.
enum class HudLayoutMode : uint8_t {
    DualScreenAnchored = 0, // Classic dual-screen with HUD elements anchored to true widescreen edges
    SingleScreenModern = 1, // Single widescreen gameplay viewport with translucent HUD & minimap overlay
};

/// Horizontal anchor rule for 2D OAM sprites and Window/BG HUD regions.
enum class HudAnchor : uint8_t {
    WorldSpace = 0,  // Shifts with camera center (+left_margin) and supports negative/right-margin world coords
    LeftEdge = 1,    // Anchored to true left edge of widescreen frame (x' = x)
    Center = 2,      // Centered in widescreen frame (x' = x + left_margin)
    RightEdge = 3,   // Anchored to true right edge of widescreen frame (x' = x + (width - 256))
};

/// Geometry metrics for a configured aspect ratio preset.
struct ViewportMetrics {
    AspectRatioPreset preset = AspectRatioPreset::Widescreen_16_9;
    int width = 342;
    int height = kVanillaHeight;
    int left_margin = 43;  // (width - 256) / 2
    int right_margin = 43; // (width - 256) - left_margin
    int extra_columns_per_side = 2; // Extra 24x24 dungeon tile columns needed per side
};

/// Describes a 2D BG layer's role for widescreen scanline rasterization.
enum class BgLayerRole : uint8_t {
    WorldTilemap = 0,   // Scrollable 512x256 / 512x512 dungeon or ground background
    HudStatusOverlay = 1, // Top-edge status bar (HP / Level / Floor / Belly)
    DialogueWindow = 2, // Centered message log / dialogue box / menu window
};

/// Parameters for a single 2D BG layer scanline.
struct BgLayerConfig {
    bool enabled = true;
    uint8_t priority = 0; // 0 (highest) .. 3 (lowest)
    BgLayerRole role = BgLayerRole::WorldTilemap;
    int virtual_width = 512;  // 256 or 512 px
    int virtual_height = 256; // 256 or 512 px
    int hofs = 0;             // Hardware BGxHOFS (integer part)
    int vofs = 0;             // Hardware BGxVOFS (integer part)
    int subpixel_hofs_q8 = 0; // Optional 24.8 sub-pixel camera offset from Task 3.2 interpolator
    int subpixel_vofs_q8 = 0;
};

/// Represents an OAM sprite entry extended for widescreen & sub-pixel interpolation.
struct WidescreenOamSprite {
    int16_t vanilla_x = 0; // Signed world/screen X relative to vanilla 256px viewport (-128..383)
    int16_t vanilla_y = 0; // Signed screen Y (-64..255)
    uint8_t width = 16;
    uint8_t height = 16;
    uint8_t priority = 2;
    HudAnchor anchor = HudAnchor::WorldSpace;
    int16_t subpixel_dx_q8 = 0; // 24.8 fixed-point sub-pixel offset X
    int16_t subpixel_dy_q8 = 0; // 24.8 fixed-point sub-pixel offset Y
    const uint32_t* argb_pixels = nullptr; // Optional sprite bitmap (width * height ARGB8888)
};

/// Hardware Window register (WIN0H / WIN1H) remapped to widescreen coordinates.
struct WindowHorizontalBounds {
    uint16_t x1 = 0; // Left inclusive bound in [0, width]
    uint16_t x2 = 0; // Right exclusive bound in [0, width]
};

/// 3D UI / darkness quad (`struct render_3d_element_64`) expanded for widescreen margins.
/// Ported from widescreen research in Asphaltian/pmd-space (DungeonDarkness.cs & FullScreenRectangles.cs, GPL-3.0).
struct Render3dElement64Quad {
    uint8_t type = 0; // 2 = RENDER64_RECTANGLE, 5 = RENDER64_TILING
    int16_t tex_w = 0;
    int16_t tex_h = 0;
    int16_t tex_u = 0;
    int16_t tex_v = 0;
    // For RENDER64_RECTANGLE: x0=translation.x, y0=translation.y, x1=size.w, y1=size.h
    // For RENDER64_TILING: 4 vertex X/Y pairs (top-left, top-right, bottom-left, bottom-right)
    std::array<int16_t, 4> vx{0, 0, 0, 0};
    std::array<int16_t, 4> vy{0, 0, 0, 0};
};

/// Room pixel bounds (`struct room_data` +0x0C) for generating widescreen darkness margin pieces.
struct RoomPixelBounds {
    int16_t left = 0;
    int16_t top = 0;
    int16_t right = 0;
    int16_t bottom = 0;
};

/// Summary of Dungeon HUD state for Single-Screen Modern Layout overlay synthesis.
struct ModernHudState {
    int16_t floor_number = 1;
    int16_t leader_level = 1;
    int16_t leader_hp = 100;
    int16_t leader_max_hp = 100;
    int16_t belly = 100;
    int16_t max_belly = 100;
    bool message_log_open = false;
    bool minimap_enabled = true;
    // Optional 56x32 compact terrain/visibility grid (0=unrevealed, 1=floor, 2=water, 3=leader, 4=stairs, 5=enemy)
    std::array<uint8_t, 56 * 32> minimap_cells{};
};

/// True 2D Widescreen PPU Rasterizer & HUD Anchor Engine (Task 3.1).
class WidescreenPpu {
public:
    /// Constructs a widescreen PPU rasterizer for the specified aspect ratio and HUD layout mode.
    explicit WidescreenPpu(
        AspectRatioPreset preset = AspectRatioPreset::Widescreen_16_9,
        HudLayoutMode hud_mode = HudLayoutMode::DualScreenAnchored) noexcept;

    /// Computes the horizontal viewport width, margins, and tile column radius for `preset`.
    [[nodiscard]] static constexpr ViewportMetrics ComputeMetrics(AspectRatioPreset preset) noexcept {
        switch (preset) {
            case AspectRatioPreset::Vanilla_4_3:
                return {preset, 256, 192, 0, 0, 0};
            case AspectRatioPreset::SteamDeck_16_10:
                return {preset, 308, 192, 26, 26, 2};
            case AspectRatioPreset::Widescreen_16_9:
                return {preset, 342, 192, 43, 43, 2};
            case AspectRatioPreset::Ultrawide_21_9:
                return {preset, 448, 192, 96, 96, 4};
        }
        return {AspectRatioPreset::Widescreen_16_9, 342, 192, 43, 43, 2};
    }

    /// Sets the active widescreen aspect ratio preset and updates viewport metrics.
    void SetAspectRatio(AspectRatioPreset preset) noexcept;
    /// Returns the currently configured widescreen aspect ratio preset.
    [[nodiscard]] AspectRatioPreset GetAspectRatio() const noexcept { return metrics_.preset; }
    /// Returns the computed viewport geometry metrics for the active aspect ratio.
    [[nodiscard]] const ViewportMetrics& GetMetrics() const noexcept { return metrics_; }

    /// Sets the active HUD presentation mode (DualScreenAnchored or SingleScreenModern).
    void SetHudLayoutMode(HudLayoutMode mode) noexcept { hud_mode_ = mode; }
    /// Returns the active HUD presentation mode.
    [[nodiscard]] HudLayoutMode GetHudLayoutMode() const noexcept { return hud_mode_; }

    /// Classifies an OAM sprite's anchor based on its screen coordinates, priority, and UI state.
    [[nodiscard]] HudAnchor ClassifyOamHudAnchor(
        int vanilla_x,
        int vanilla_y,
        uint8_t bg_priority,
        bool is_hud_element,
        bool is_message_window_element = false) const noexcept;

    /// Remaps an OAM sprite's X coordinate (plus 24.8 sub-pixel delta) into the active widescreen viewport.
    [[nodiscard]] int RemapSpriteX(const WidescreenOamSprite& sprite) const noexcept;

    /// Remaps an OAM sprite's Y coordinate (plus 24.8 sub-pixel delta).
    [[nodiscard]] int RemapSpriteY(const WidescreenOamSprite& sprite) const noexcept;

    /// Remaps NDS hardware horizontal window bounds (WIN0H / WIN1H) for dialogue boxes & HUD masks.
    [[nodiscard]] WindowHorizontalBounds RemapWindowHorizontal(
        uint8_t vanilla_x1,
        uint8_t vanilla_x2,
        HudAnchor anchor) const noexcept;

    /// Computes the virtual BG source X coordinate for a widescreen output pixel `screen_x` in [0, width).
    /// Returns -1 if `screen_x` falls outside a non-wrapping HUD/dialogue layer's active region.
    [[nodiscard]] int SampleBgSourceX(const BgLayerConfig& layer, int screen_x) const noexcept;

    /// Rasterizes a single widescreen scanline (`metrics_.width` pixels) from a virtual BG pixel buffer
    /// (`layer.virtual_width * layer.virtual_height` ARGB8888 pixels).
    void RasterizeBgScanline(
        const BgLayerConfig& layer,
        std::span<const uint32_t> virtual_bg_argb,
        int scanline_y,
        std::span<uint32_t> out_scanline_argb) const noexcept;

    /// Composites a list of OAM sprites onto a widescreen ARGB8888 framebuffer (`width * 192`).
    void CompositeOamSprites(
        std::span<const WidescreenOamSprite> sprites,
        std::span<uint32_t> framebuffer_argb) const noexcept;

    /// Renders the Single-Screen Modern Layout translucent HUD (HP bar, Belly bar, Floor badge,
    /// and widescreen corner mini-map) onto the widescreen ARGB8888 framebuffer.
    void RenderModernSingleScreenHud(
        const ModernHudState& state,
        std::span<uint32_t> framebuffer_argb) const noexcept;

    /// Expands a 3D-rendered full-screen rectangle (`RENDER64_RECTANGLE` at 0,0 256x192) or
    /// dungeon darkness band (`RENDER64_TILING` 4x4 band) across the widescreen margins.
    /// Adapted from Asphaltian/pmd-space (`FullScreenRectangles.cs` & `DungeonDarkness.cs`, GPL-3.0).
    bool ExpandRender3dElementForWidescreen(
        Render3dElement64Quad& elem,
        bool is_building_dungeon_darkness = false) const noexcept;

    /// Generates additional 3D darkness band & 32x32 corner quads needed when a partially lit dungeon
    /// room's left/right border or rounded corners fall inside the widescreen margins.
    /// Adapted from Asphaltian/pmd-space (`DungeonDarkness.cs`, GPL-3.0).
    [[nodiscard]] std::vector<Render3dElement64Quad> BuildDungeonDarknessMarginQuads(
        const RoomPixelBounds& room_bounds,
        int16_t camera_pixel_x,
        int16_t camera_pixel_y) const;

private:
    ViewportMetrics metrics_;
    HudLayoutMode hud_mode_ = HudLayoutMode::DualScreenAnchored;
};

} // namespace skyrecomp::gpu2d
