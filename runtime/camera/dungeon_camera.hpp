#pragma once

#include "skyrecomp/guest_mem.hpp"
#include "skyrecomp/pmdsky_addrs.hpp"
#include "../gpu2d/widescreen_ppu.hpp"

#include <cstdint>
#include <vector>

namespace skyrecomp::camera {

inline constexpr int kDungeonTilePixels = 24;
inline constexpr int kVanillaHorizontalTileRadius = 6;
inline constexpr int kVanillaVerticalTileRadius = 5;

/// Snapshot of `overlay29` `struct display_data` camera & lighting fields.
struct DungeonDisplaySnapshot {
    bool valid = false;
    int16_t camera_tile_x = 0;
    int16_t camera_tile_y = 0;
    int16_t camera_pixel_x = 0;
    int16_t camera_pixel_y = 0;
    uint32_t camera_target_ptr = 0;
    uint8_t target_room_idx = 0xFFu;
    uint8_t visibility_range = 2;
    bool blinded = false;
    bool luminous = false;
    bool natural_lighting = true;
    bool map_surveyor = false;
    bool can_see_traps = false;
    bool darkness = false;
    bool dropeye = false;
    int16_t floor_number = 1;
    int16_t leader_level = 1;
    int16_t leader_hp = 100;
    int16_t leader_max_hp = 100;
};

/// Describes the inclusive dungeon tile-column and tile-row streaming window for a widescreen frame.
struct TileStreamingWindow {
    int center_tile_x = 0;
    int center_tile_y = 0;
    int horizontal_tile_radius = kVanillaHorizontalTileRadius;
    int vertical_tile_radius = kVanillaVerticalTileRadius;
    int min_tile_x = 0; // Clamped to [0, 55]
    int max_tile_x = 0; // Clamped to [0, 55]
    int min_tile_y = 0; // Clamped to [0, 31]
    int max_tile_y = 0; // Clamped to [0, 31]
    int extra_margin_columns_left = 0;  // Columns streamed purely for the left widescreen margin
    int extra_margin_columns_right = 0; // Columns streamed purely for the right widescreen margin
};

/// Streamed descriptor for a single visible dungeon tile in the widescreen frustum.
struct StreamedDungeonTile {
    int16_t tile_x = 0;
    int16_t tile_y = 0;
    int16_t screen_pixel_x = 0; // Top-left pixel X in active widescreen viewport
    int16_t screen_pixel_y = 0; // Top-left pixel Y in active widescreen viewport
    uint16_t terrain_flags = 0;
    uint16_t visibility_flags = 0;
    uint16_t texture_id = 0;
    uint8_t room_idx = 0xFFu;
    bool in_widescreen_margin = false;
    bool illuminated_by_fov = true;
};

/// Sprite category for exact widescreen margin culling reaches.
/// Ported from Asphaltian/pmd-space (`MarginSprites.cs`, GPL-3.0).
enum class MarginSpriteKind : uint8_t {
    Item = 0,             // Vanilla reach: [-32, 287] (DisplayItem)
    ThrownItemShadow = 1, // Vanilla reach: [-16, 270] (DisplayThrownItemShadow)
    Monster = 2,          // Vanilla reach: [-64, 319] (ov29_022E273C ShouldDisplayMonster)
    OamPiece = 3,         // Vanilla reach: [-64, 256] (sub_0201B9B0 isPieceOffScreen)
};

/// Describes the widescreen wait/target coordinate adjustment for an off-screen cutscene actor.
/// Ported from Asphaltian/pmd-space (`OffScreenActors.cs`, GPL-3.0).
struct OffScreenActorAdjustment {
    bool should_shift = false;
    bool adjusts_move_target = false; // true = shift move_target (+0x90), false = shift coord_min/max (+0x15C/+0x164)
    int side = 0;                     // -1 = left margin, +1 = right margin
    int32_t delta_x_q8 = 0;           // 24.8 fixed-point horizontal offset to apply
};

/// Hook for `overlay29` camera, widescreen tile-column streaming, and `IsPositionVisibleAndOnScreen`
/// (`0x022E2CA0` NA / `0x022E3624` EU) entity culling (Task 3.1).
class DungeonCameraHook {
public:
    explicit DungeonCameraHook(
        pmdsky::Region region = pmdsky::Region::NA,
        gpu2d::AspectRatioPreset aspect = gpu2d::AspectRatioPreset::Widescreen_16_9) noexcept;

    void SetRegion(pmdsky::Region region) noexcept { region_ = region; }
    [[nodiscard]] pmdsky::Region GetRegion() const noexcept { return region_; }

    void SetAspectRatio(gpu2d::AspectRatioPreset aspect) noexcept;
    [[nodiscard]] gpu2d::AspectRatioPreset GetAspectRatio() const noexcept { return aspect_; }

    /// Computes the horizontal dungeon tile radius needed to fill `viewport_width` pixels without pop-in.
    /// Vanilla 256px -> 6 tiles; 308px (16:10) -> 8 tiles; 342px (16:9) -> 9 tiles; 448px (21:9) -> 11 tiles.
    [[nodiscard]] static constexpr int ComputeHorizontalTileRadius(int viewport_width) noexcept {
        if (viewport_width <= gpu2d::kVanillaWidth) {
            return kVanillaHorizontalTileRadius;
        }
        const int half_width = (viewport_width + 1) / 2;
        const int ceil_tiles = (half_width + kDungeonTilePixels - 1) / kDungeonTilePixels;
        return ceil_tiles + 1;
    }

    /// Reads the current `display_data` camera & visibility snapshot from guest Main RAM.
    [[nodiscard]] DungeonDisplaySnapshot ReadDisplaySnapshot(GuestMemoryView mem) const noexcept;

    /// Computes the tile-column streaming window for the current camera position and aspect ratio.
    [[nodiscard]] TileStreamingWindow ComputeStreamingWindow(
        const DungeonDisplaySnapshot& snapshot) const noexcept;

    /// Widescreen replacement hook for `IsPositionVisibleAndOnScreen` (`0x022E2CA0` NA / `0x022E3624` EU)
    /// and `ShouldDisplayEntity` (`0x022E2574` NA / `0x022E2EB4` EU).
    /// Expands the screen-space X culling bound to match the active widescreen aspect ratio while
    /// strictly enforcing hallway/blind/darkness fog-of-war rules (`GetVisibilityRange`).
    [[nodiscard]] bool IsPositionVisibleAndOnWidescreen(
        const DungeonDisplaySnapshot& snapshot,
        int tile_x,
        int tile_y,
        uint8_t tile_room_idx = 0xFFu,
        bool respect_fog_of_war = true) const noexcept;

    /// Streams all visible dungeon tiles within the widescreen frustum from `dungeon->tile_ptrs[32][56]`.
    [[nodiscard]] std::vector<StreamedDungeonTile> StreamVisibleTiles(
        GuestMemoryView mem,
        const DungeonDisplaySnapshot& snapshot) const;

    /// Builds a `ModernHudState` (including the 56x32 revealed minimap grid) from guest RAM.
    [[nodiscard]] gpu2d::ModernHudState BuildModernHudState(
        GuestMemoryView mem,
        const DungeonDisplaySnapshot& snapshot) const noexcept;

    /// Computes the temporary camera X shift (`-left_margin`, `0`, or `+right_margin`) needed so
    /// `DisplayItem`, `DisplayThrownItemShadow`, or `ShouldDisplayMonster` renders an entity in the
    /// widescreen margin without hitting vanilla 256px culling bounds.
    /// Adapted from Asphaltian/pmd-space (`MarginSprites.cs`, GPL-3.0).
    [[nodiscard]] int ComputeMarginCameraShift(int vanilla_screen_x, MarginSpriteKind kind) const noexcept;

    /// Widescreen replacement for `sub_0201B9B0` (`isPieceOffScreen`).
    /// Adapted from Asphaltian/pmd-space (`MarginSprites.cs`, GPL-3.0).
    [[nodiscard]] bool IsOamPieceOffWidescreen(int vanilla_piece_x) const noexcept;

    /// Samples a single 8x8 tilemap entry from an `overlay11` 3x3 chunk `map_render` structure
    /// (supports negative left-margin and >=32 right-margin columns plus `wrap_around`).
    /// Adapted from Asphaltian/pmd-space (`GroundBg.cs` & `GroundMargins.cs`, GPL-3.0).
    [[nodiscard]] static uint16_t SampleGroundMapTileEntry(
        GuestMemoryView mem,
        uint32_t map_render_addr,
        int bg_index,
        int tile_col,
        int tile_row) noexcept;

    /// Detects whether a Ground Mode `ground_bg` represents a static 256x192 full-screen chapter/cutscene
    /// illustration (whose non-zero tile bounds fit inside 32x24 tiles with no active actors), so margins
    /// can be cleanly pillarboxed instead of streaming empty/garbage tiles.
    /// Adapted from Asphaltian/pmd-space (`FullScreenPictures.cs`, GPL-3.0).
    [[nodiscard]] bool IsGroundMapStaticFullScreenPicture(
        GuestMemoryView mem,
        uint32_t ground_bg_addr) const noexcept;

    /// Computes the widescreen margin shift for an off-screen waiting or walking cutscene `live_actor`
    /// so NPCs parked just outside the vanilla [0, 256) frame do not appear prematurely in the margins.
    /// Adapted from Asphaltian/pmd-space (`OffScreenActors.cs`, GPL-3.0).
    [[nodiscard]] OffScreenActorAdjustment ComputeOffScreenActorAdjustment(
        GuestMemoryView mem,
        uint32_t actor_addr,
        int camera_pixel_x) const noexcept;

private:
    pmdsky::Region region_ = pmdsky::Region::NA;
    gpu2d::AspectRatioPreset aspect_ = gpu2d::AspectRatioPreset::Widescreen_16_9;
    gpu2d::ViewportMetrics metrics_;
};

} // namespace skyrecomp::camera
