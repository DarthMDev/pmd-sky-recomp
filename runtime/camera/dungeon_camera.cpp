#include "dungeon_camera.hpp"

#include <algorithm>
#include <cstdlib>

namespace skyrecomp::camera {

DungeonCameraHook::DungeonCameraHook(
    pmdsky::Region region,
    gpu2d::AspectRatioPreset aspect) noexcept
    : region_(region),
      aspect_(aspect),
      metrics_(gpu2d::WidescreenPpu::ComputeMetrics(aspect)) {}

void DungeonCameraHook::SetAspectRatio(gpu2d::AspectRatioPreset aspect) noexcept {
    aspect_ = aspect;
    metrics_ = gpu2d::WidescreenPpu::ComputeMetrics(aspect);
}

DungeonDisplaySnapshot DungeonCameraHook::ReadDisplaySnapshot(GuestMemoryView mem) const noexcept {
    DungeonDisplaySnapshot snap{};
    const uint32_t dungeon_ptr = mem.resolve_dungeon_ptr(region_);
    if (dungeon_ptr == 0u) {
        return snap;
    }

    const uint32_t disp_base = dungeon_ptr + layout::dungeon::kDisplayDataOffset;
    snap.valid = true;
    snap.camera_tile_x = mem.read_or<int16_t>(disp_base + layout::display_data::kCameraPosOffset + 0u, 0);
    snap.camera_tile_y = mem.read_or<int16_t>(disp_base + layout::display_data::kCameraPosOffset + 2u, 0);
    snap.camera_pixel_x = mem.read_or<int16_t>(disp_base + layout::display_data::kCameraPixelPosOffset + 0u, 0);
    snap.camera_pixel_y = mem.read_or<int16_t>(disp_base + layout::display_data::kCameraPixelPosOffset + 2u, 0);
    snap.camera_target_ptr = mem.read_or<uint32_t>(disp_base + layout::display_data::kCameraTargetPtrOffset, 0u);
    snap.visibility_range = mem.read_or<uint8_t>(disp_base + layout::display_data::kVisibilityRangeOffset, 2u);
    snap.blinded = mem.read_or<uint8_t>(disp_base + layout::display_data::kBlindedOffset, 0u) != 0u;
    snap.luminous = mem.read_or<uint8_t>(disp_base + layout::display_data::kLuminousOffset, 0u) != 0u;
    snap.natural_lighting = mem.read_or<uint8_t>(disp_base + layout::display_data::kNaturalLightingOffset, 1u) != 0u;
    snap.map_surveyor = mem.read_or<uint8_t>(disp_base + layout::display_data::kMapSurveyorOffset, 0u) != 0u;
    snap.can_see_traps = mem.read_or<uint8_t>(disp_base + layout::display_data::kCanSeeTrapsOffset, 0u) != 0u;
    snap.darkness = mem.read_or<uint8_t>(disp_base + layout::display_data::kDarknessOffset, 0u) != 0u;
    snap.dropeye = mem.read_or<uint8_t>(disp_base + layout::display_data::kDropeyeOffset, 0u) != 0u;
    snap.floor_number = mem.read_or<int16_t>(disp_base + layout::display_data::kFloorTouchScreenOffset, 1);
    snap.leader_level = mem.read_or<int16_t>(disp_base + layout::display_data::kLeaderLevelTouchScreenOffset, 1);
    snap.leader_hp = mem.read_or<int16_t>(disp_base + layout::display_data::kLeaderHpTouchScreenOffset, 100);
    snap.leader_max_hp = mem.read_or<int16_t>(disp_base + layout::display_data::kLeaderMaxHpTouchScreenOffset, 100);

    // Read camera target entity's room_idx if camera_target_ptr is valid
    if (mem.in_range(snap.camera_target_ptr, layout::entity::kSize)) {
        snap.target_room_idx = mem.read_or<uint8_t>(
            snap.camera_target_ptr + layout::entity::kRoomIdxOffset,
            0xFFu);
    }

    return snap;
}

TileStreamingWindow DungeonCameraHook::ComputeStreamingWindow(
    const DungeonDisplaySnapshot& snapshot) const noexcept {
    TileStreamingWindow win{};
    win.center_tile_x = snapshot.camera_tile_x;
    win.center_tile_y = snapshot.camera_tile_y;
    win.horizontal_tile_radius = ComputeHorizontalTileRadius(metrics_.width);
    win.vertical_tile_radius = kVanillaVerticalTileRadius;

    const int raw_min_x = win.center_tile_x - win.horizontal_tile_radius;
    const int raw_max_x = win.center_tile_x + win.horizontal_tile_radius;
    const int vanilla_min_x = std::clamp(win.center_tile_x - kVanillaHorizontalTileRadius, 0, layout::dungeon::kMaxGridX - 1);
    const int vanilla_max_x = std::clamp(win.center_tile_x + kVanillaHorizontalTileRadius, 0, layout::dungeon::kMaxGridX - 1);

    win.min_tile_x = std::clamp(raw_min_x, 0, layout::dungeon::kMaxGridX - 1);
    win.max_tile_x = std::clamp(raw_max_x, 0, layout::dungeon::kMaxGridX - 1);
    win.min_tile_y = std::clamp(win.center_tile_y - win.vertical_tile_radius, 0, layout::dungeon::kMaxGridY - 1);
    win.max_tile_y = std::clamp(win.center_tile_y + win.vertical_tile_radius, 0, layout::dungeon::kMaxGridY - 1);

    win.extra_margin_columns_left = std::max(0, vanilla_min_x - win.min_tile_x);
    win.extra_margin_columns_right = std::max(0, win.max_tile_x - vanilla_max_x);
    return win;
}

bool DungeonCameraHook::IsPositionVisibleAndOnWidescreen(
    const DungeonDisplaySnapshot& snapshot,
    int tile_x,
    int tile_y,
    uint8_t tile_room_idx,
    bool respect_fog_of_war) const noexcept {
    if (!snapshot.valid) {
        return false;
    }
    if (tile_x < 0 || tile_x >= layout::dungeon::kMaxGridX ||
        tile_y < 0 || tile_y >= layout::dungeon::kMaxGridY) {
        return false;
    }

    const int dx = std::abs(tile_x - static_cast<int>(snapshot.camera_tile_x));
    const int dy = std::abs(tile_y - static_cast<int>(snapshot.camera_tile_y));
    const int max_rx = ComputeHorizontalTileRadius(metrics_.width);

    // 1. Check expanded widescreen screen frustum
    if (dx > max_rx || dy > kVanillaVerticalTileRadius) {
        return false;
    }

    if (!respect_fog_of_war || snapshot.luminous) {
        return true;
    }

    // 2. Enforce PMD Sky fog-of-war / status visibility rules (GetVisibilityRange)
    if (snapshot.blinded || snapshot.dropeye) {
        return dx <= 1 && dy <= 1;
    }

    const bool in_hallway = (snapshot.target_room_idx >= 0xFEu);
    if (!in_hallway && snapshot.natural_lighting && !snapshot.darkness) {
        // Inside a lit room: any tile in the same room (or immediate 1-tile corridor doorway) is visible
        if (tile_room_idx == snapshot.target_room_idx) {
            return true;
        }
        const int hall_range = std::max<int>(2, snapshot.visibility_range);
        return dx <= hall_range && dy <= hall_range;
    }

    // In hallway or dark floor: Chebyshev distance bounded by visibility_range (default 2 tiles)
    const int vis_range = (snapshot.visibility_range > 0) ? static_cast<int>(snapshot.visibility_range) : 2;
    return dx <= vis_range && dy <= vis_range;
}

std::vector<StreamedDungeonTile> DungeonCameraHook::StreamVisibleTiles(
    GuestMemoryView mem,
    const DungeonDisplaySnapshot& snapshot) const {
    std::vector<StreamedDungeonTile> tiles;
    if (!snapshot.valid) {
        return tiles;
    }
    const uint32_t dungeon_ptr = mem.resolve_dungeon_ptr(region_);
    if (dungeon_ptr == 0u) {
        return tiles;
    }

    const TileStreamingWindow win = ComputeStreamingWindow(snapshot);
    const size_t reserve_count =
        static_cast<size_t>(win.max_tile_x - win.min_tile_x + 1) *
        static_cast<size_t>(win.max_tile_y - win.min_tile_y + 1);
    tiles.reserve(reserve_count);

    const uint32_t tile_ptrs_base = dungeon_ptr + layout::dungeon::kTilePtrsOffset;

    for (int ty = win.min_tile_y; ty <= win.max_tile_y; ++ty) {
        for (int tx = win.min_tile_x; tx <= win.max_tile_x; ++tx) {
            const uint32_t ptr_index =
                static_cast<uint32_t>(ty * layout::dungeon::kMaxGridX + tx) * sizeof(uint32_t);
            const uint32_t tile_ptr = mem.read_or<uint32_t>(tile_ptrs_base + ptr_index, 0u);
            if (!mem.in_range(tile_ptr, layout::tile::kSize)) {
                continue;
            }

            const uint16_t terrain_flags = mem.read_or<uint16_t>(tile_ptr + layout::tile::kTerrainFlagsOffset, 0u);
            const uint16_t vis_flags = mem.read_or<uint16_t>(tile_ptr + layout::tile::kVisibilityFlagsOffset, 0u);
            const uint16_t tex_id = mem.read_or<uint16_t>(tile_ptr + layout::tile::kTextureIdOffset, 0u);
            const uint8_t room_idx = mem.read_or<uint8_t>(tile_ptr + layout::tile::kRoomIndexOffset, 0xFFu);

            const int world_px = tx * kDungeonTilePixels;
            const int world_py = ty * kDungeonTilePixels;
            const int vanilla_screen_x = world_px - static_cast<int>(snapshot.camera_pixel_x);
            const int vanilla_screen_y = world_py - static_cast<int>(snapshot.camera_pixel_y);
            const int wide_screen_x = vanilla_screen_x + metrics_.left_margin;

            const int dx = std::abs(tx - static_cast<int>(snapshot.camera_tile_x));
            const bool in_margin = (dx > kVanillaHorizontalTileRadius);
            const bool illuminated = IsPositionVisibleAndOnWidescreen(snapshot, tx, ty, room_idx, true);

            tiles.push_back(StreamedDungeonTile{
                .tile_x = static_cast<int16_t>(tx),
                .tile_y = static_cast<int16_t>(ty),
                .screen_pixel_x = static_cast<int16_t>(wide_screen_x),
                .screen_pixel_y = static_cast<int16_t>(vanilla_screen_y),
                .terrain_flags = terrain_flags,
                .visibility_flags = vis_flags,
                .texture_id = tex_id,
                .room_idx = room_idx,
                .in_widescreen_margin = in_margin,
                .illuminated_by_fov = illuminated,
            });
        }
    }

    return tiles;
}

gpu2d::ModernHudState DungeonCameraHook::BuildModernHudState(
    GuestMemoryView mem,
    const DungeonDisplaySnapshot& snapshot) const noexcept {
    gpu2d::ModernHudState hud{};
    if (!snapshot.valid) {
        return hud;
    }

    hud.floor_number = snapshot.floor_number;
    hud.leader_level = snapshot.leader_level;
    hud.leader_hp = snapshot.leader_hp;
    hud.leader_max_hp = snapshot.leader_max_hp;

    const uint32_t dungeon_ptr = mem.resolve_dungeon_ptr(region_);
    if (dungeon_ptr == 0u) {
        return hud;
    }

    const uint32_t tile_ptrs_base = dungeon_ptr + layout::dungeon::kTilePtrsOffset;
    for (int ty = 0; ty < layout::dungeon::kMaxGridY; ++ty) {
        for (int tx = 0; tx < layout::dungeon::kMaxGridX; ++tx) {
            const uint32_t ptr_index =
                static_cast<uint32_t>(ty * layout::dungeon::kMaxGridX + tx) * sizeof(uint32_t);
            const uint32_t tile_ptr = mem.read_or<uint32_t>(tile_ptrs_base + ptr_index, 0u);
            if (!mem.in_range(tile_ptr, layout::tile::kSize)) {
                continue;
            }
            const uint16_t terrain_flags = mem.read_or<uint16_t>(tile_ptr + layout::tile::kTerrainFlagsOffset, 0u);
            const uint16_t vis_flags = mem.read_or<uint16_t>(tile_ptr + layout::tile::kVisibilityFlagsOffset, 0u);
            const bool revealed = snapshot.luminous || ((vis_flags & 0x01u) != 0u);
            if (!revealed) {
                continue;
            }

            const uint8_t terrain_type = static_cast<uint8_t>(terrain_flags & 0x03u);
            uint8_t cell = 0u;
            if (terrain_type == 1u) {
                cell = 1u; // Normal floor
            } else if (terrain_type == 2u) {
                cell = 2u; // Water / lava
            }

            const uint32_t monster_ptr = mem.read_or<uint32_t>(tile_ptr + layout::tile::kMonsterPtrOffset, 0u);
            if (monster_ptr != 0u) {
                cell = (monster_ptr == snapshot.camera_target_ptr) ? 3u : 5u;
            }
            hud.minimap_cells[static_cast<size_t>(ty * layout::dungeon::kMaxGridX + tx)] = cell;
        }
    }

    return hud;
}

} // namespace skyrecomp::camera
