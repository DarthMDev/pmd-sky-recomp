#include "dungeon_camera.hpp"

#include <algorithm>
#include <cstdlib>

namespace skyrecomp::camera {

/// Constructs an Overlay 29 dungeon camera and widescreen frustum hook.
DungeonCameraHook::DungeonCameraHook(
    pmdsky::Region region,
    gpu2d::AspectRatioPreset aspect) noexcept
    : region_(region),
      aspect_(aspect),
      metrics_(gpu2d::WidescreenPpu::ComputeMetrics(aspect)) {}

/// Sets the active widescreen aspect ratio preset and updates frustum metrics.
void DungeonCameraHook::SetAspectRatio(gpu2d::AspectRatioPreset aspect) noexcept {
    aspect_ = aspect;
    metrics_ = gpu2d::WidescreenPpu::ComputeMetrics(aspect);
}

/// Reads the current `display_data` camera & visibility snapshot from guest Main RAM.
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

/// Computes the tile-column streaming window for the current camera position and aspect ratio.
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

/// Checks whether a dungeon tile is within the widescreen frustum and visible under fog-of-war rules.
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

/// Streams all visible dungeon tiles within the widescreen frustum from `dungeon->tile_ptrs[32][56]`.
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

/// Builds a `ModernHudState` (including the 56x32 revealed minimap grid) from guest RAM.
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

/// Computes the temporary camera X shift needed so margin sprites avoid vanilla 256px culling.
int DungeonCameraHook::ComputeMarginCameraShift(
    int vanilla_screen_x,
    MarginSpriteKind kind) const noexcept {
    const int margin = metrics_.left_margin;
    if (margin <= 0) {
        return 0;
    }

    // Exact vanilla reach bounds from Asphaltian/pmd-space MarginSprites.cs (GPL-3.0).
    int reach_left = -64;
    int reach_right = 319;
    switch (kind) {
        case MarginSpriteKind::Item:
            reach_left = -32;
            reach_right = 287;
            break;
        case MarginSpriteKind::ThrownItemShadow:
            reach_left = -16;
            reach_right = 270;
            break;
        case MarginSpriteKind::Monster:
            reach_left = -64;
            reach_right = 319;
            break;
        case MarginSpriteKind::OamPiece:
            reach_left = -64;
            reach_right = gpu2d::kVanillaWidth;
            break;
    }

    if (vanilla_screen_x < reach_left - margin || vanilla_screen_x > reach_right + margin) {
        return 0;
    }
    if (vanilla_screen_x < reach_left) {
        return -margin;
    }
    if (vanilla_screen_x > reach_right) {
        return margin;
    }
    return 0;
}

/// Widescreen replacement for `sub_0201B9B0` (`isPieceOffScreen`).
bool DungeonCameraHook::IsOamPieceOffWidescreen(int vanilla_piece_x) const noexcept {
    // Ported from Asphaltian/pmd-space MarginSprites.isPieceOffScreen (GPL-3.0).
    constexpr int kPieceReachLeft = -64;
    constexpr int kLowestObjectX = -128;
    const int margin = metrics_.left_margin;
    const int min_x = std::max(kPieceReachLeft - margin, kLowestObjectX);
    const int max_x = gpu2d::kVanillaWidth + metrics_.right_margin;
    return vanilla_piece_x < min_x || vanilla_piece_x >= max_x;
}

/// Samples a single 8x8 tilemap entry from an `overlay11` 3x3 chunk `map_render` structure.
uint16_t DungeonCameraHook::SampleGroundMapTileEntry(
    GuestMemoryView mem,
    uint32_t map_render_addr,
    int bg_index,
    int tile_col,
    int tile_row) noexcept {
    if (!mem.in_range(map_render_addr, layout::ground::kMapRenderSize) || bg_index < 0 || bg_index >= 2) {
        return 0u;
    }

    // Ported from Asphaltian/pmd-space MapRender.Entry (GroundBg.cs, GPL-3.0).
    const int16_t chunk_dims =
        mem.read_or<int16_t>(map_render_addr + layout::ground::kMapRenderChunkDimensionsOffset, 0);
    if (chunk_dims != layout::ground::kMapRenderChunks3x3) {
        return 0u;
    }

    auto div_floor_chunk = [](int tiles, int& out_chunk, int& out_tile) noexcept {
        constexpr int kChunk = layout::ground::kChunkTileSize;
        int q = tiles / kChunk;
        int r = tiles % kChunk;
        if (r < 0) {
            --q;
            r += kChunk;
        }
        out_chunk = q;
        out_tile = r;
    };

    int chunk_col = 0;
    int sub_col = 0;
    int chunk_row = 0;
    int sub_row = 0;
    div_floor_chunk(tile_col, chunk_col, sub_col);
    div_floor_chunk(tile_row, chunk_row, sub_row);

    const uint8_t wrap_around =
        mem.read_or<uint8_t>(map_render_addr + layout::ground::kMapRenderWrapAroundOffset, 0u);
    const int width_chunks =
        mem.read_or<int32_t>(map_render_addr + layout::ground::kMapRenderWidthChunksOffset, 0);
    const int height_chunks =
        mem.read_or<int32_t>(map_render_addr + layout::ground::kMapRenderHeightChunksOffset, 0);
    if (width_chunks <= 0 || height_chunks <= 0) {
        return 0u;
    }

    if (wrap_around != 0u) {
        chunk_col = ((chunk_col % width_chunks) + width_chunks) % width_chunks;
        chunk_row = ((chunk_row % height_chunks) + height_chunks) % height_chunks;
    } else if (chunk_col < 0 || chunk_col >= width_chunks || chunk_row < 0 || chunk_row >= height_chunks) {
        return 0u;
    }

    const uint32_t chunks_ptr = mem.read_or<uint32_t>(
        map_render_addr + layout::ground::kMapRenderChunkMapPtrOffset + static_cast<uint32_t>(bg_index * 4),
        0u);
    const uint32_t chunk_tiles_ptr = mem.read_or<uint32_t>(
        map_render_addr + layout::ground::kMapRenderChunkTilesPtrOffset + static_cast<uint32_t>(bg_index * 4),
        0u);

    const uint32_t chunk_idx_addr =
        chunks_ptr + static_cast<uint32_t>((chunk_row * layout::ground::kChunkMapStride + chunk_col) * 2);
    const auto chunk_id = mem.read<uint16_t>(chunk_idx_addr);
    if (!chunk_id.has_value()) {
        return 0u;
    }

    constexpr int kChunkArea = layout::ground::kChunkTileSize * layout::ground::kChunkTileSize;
    const uint32_t tile_entry_addr =
        chunk_tiles_ptr +
        static_cast<uint32_t>((static_cast<int>(*chunk_id) * kChunkArea +
                               sub_row * layout::ground::kChunkTileSize + sub_col) * 2);
    return mem.read_or<uint16_t>(tile_entry_addr, 0u);
}

/// Detects whether a Ground Mode `ground_bg` represents a static 256x192 full-screen illustration.
bool DungeonCameraHook::IsGroundMapStaticFullScreenPicture(
    GuestMemoryView mem,
    uint32_t ground_bg_addr) const noexcept {
    if (!mem.in_range(ground_bg_addr, layout::ground::kGroundBgSize)) {
        return false;
    }

    // Dialogue scenes can have active performers over a static full-screen
    // backdrop. Decide from the background map bounds, not actor presence.
    const int cam_count = std::clamp(
        mem.read_or<int32_t>(ground_bg_addr + layout::ground::kGroundBgCameraCountOffset, 0),
        0,
        2);
    if (cam_count <= 0) {
        return false;
    }

    constexpr int kVanillaColumns = gpu2d::kVanillaWidth / 8; // 32
    constexpr int kVanillaRows = gpu2d::kVanillaHeight / 8;   // 24

    for (int idx = 0; idx < cam_count; ++idx) {
        const uint32_t mr_addr =
            ground_bg_addr + layout::ground::kGroundBgMapRenderOffset +
            static_cast<uint32_t>(idx) * layout::ground::kMapRenderSize;
        const int16_t chunk_dims =
            mem.read_or<int16_t>(mr_addr + layout::ground::kMapRenderChunkDimensionsOffset, 0);
        if (chunk_dims != layout::ground::kMapRenderChunks3x3) {
            return false;
        }

        const int num_bgs = std::clamp<int>(
            mem.read_or<int16_t>(mr_addr + layout::ground::kMapRenderNumBgsOffset, 0),
            0,
            2);
        constexpr int kMaxChunks = 192 / layout::ground::kChunkTileSize;
        const int total_cols = std::clamp<int32_t>(
            mem.read_or<int32_t>(mr_addr + layout::ground::kMapRenderWidthChunksOffset, 0),
            0, kMaxChunks) * layout::ground::kChunkTileSize;
        const int total_rows = std::clamp<int32_t>(
            mem.read_or<int32_t>(mr_addr + layout::ground::kMapRenderHeightChunksOffset, 0),
            0, kMaxChunks) * layout::ground::kChunkTileSize;

        int first_col = total_cols;
        int last_col = -1;
        int first_row = total_rows;
        int last_row = -1;

        for (int bg = 0; bg < num_bgs; ++bg) {
            for (int r = 0; r < total_rows; ++r) {
                for (int c = 0; c < total_cols; ++c) {
                    if (SampleGroundMapTileEntry(mem, mr_addr, bg, c, r) != 0u) {
                        first_col = std::min(first_col, c);
                        last_col = std::max(last_col, c);
                        first_row = std::min(first_row, r);
                        last_row = std::max(last_row, r);
                        if (last_col - first_col >= kVanillaColumns ||
                            last_row - first_row >= kVanillaRows) {
                            return false;
                        }
                    }
                }
            }
        }

        if (last_col < first_col || last_row < first_row) {
            return false;
        }
    }

    return true;
}

/// Computes the widescreen margin shift for an off-screen waiting or walking cutscene `live_actor`.
OffScreenActorAdjustment DungeonCameraHook::ComputeOffScreenActorAdjustment(
    GuestMemoryView mem,
    uint32_t actor_addr,
    int camera_pixel_x) const noexcept {
    OffScreenActorAdjustment out{};
    const int margin = metrics_.left_margin;
    if (margin <= 0 || !mem.in_range(actor_addr, layout::ground::kLiveActorSize)) {
        return out;
    }

    // Ported from Asphaltian/pmd-space OffScreenActors.MoveOut (GPL-3.0).
    const uint8_t enabled = mem.read_or<uint8_t>(actor_addr + layout::ground::kLiveActorEnabledOffset, 0u);
    const int16_t kind = mem.read_or<int16_t>(actor_addr + layout::ground::kLiveActorKindOffset, -1);
    if (enabled == 0u || kind == -1) {
        return out;
    }

    const int32_t screen_left_q8 = camera_pixel_x * 256;
    const int32_t screen_right_q8 = (camera_pixel_x + gpu2d::kVanillaWidth) * 256;
    const int32_t shift_q8 = margin * 256;

    auto classify_side = [screen_left_q8, screen_right_q8](int32_t min_x_q8, int32_t max_x_q8) noexcept -> int {
        if (max_x_q8 <= screen_left_q8) {
            return -1;
        }
        if (min_x_q8 >= screen_right_q8) {
            return +1;
        }
        return 0;
    };

    const int32_t half_w_q8 = mem.read_or<int32_t>(actor_addr + layout::ground::kLiveActorHalfSizeOffset, 0);
    const int16_t is_moving = mem.read_or<int16_t>(actor_addr + layout::ground::kLiveActorIsMovingOffset, 0);

    if (is_moving != 0) {
        const int32_t start_x_q8 = mem.read_or<int32_t>(actor_addr + layout::ground::kLiveActorMoveStartOffset, 0);
        const int32_t target_x_q8 = mem.read_or<int32_t>(actor_addr + layout::ground::kLiveActorMoveTargetOffset, 0);
        const int target_side = classify_side(target_x_q8 - half_w_q8, target_x_q8 + half_w_q8);
        const int start_side = classify_side(start_x_q8 - half_w_q8, start_x_q8 + half_w_q8);
        if (target_side != 0 && start_side == 0) {
            out.should_shift = true;
            out.adjusts_move_target = true;
            out.side = target_side;
            out.delta_x_q8 = target_side * shift_q8;
        }
        return out;
    }

    const int32_t min_x_q8 = mem.read_or<int32_t>(actor_addr + layout::ground::kLiveActorCoordMinOffset, 0);
    const int32_t max_x_q8 = mem.read_or<int32_t>(actor_addr + layout::ground::kLiveActorCoordMaxOffset, 0);
    const int32_t init_x_q8 = mem.read_or<int32_t>(actor_addr + layout::ground::kLiveActorInitialPosOffset, 0);
    const int waiting_side = classify_side(min_x_q8, max_x_q8);
    if (waiting_side != 0 && (min_x_q8 + half_w_q8) == init_x_q8) {
        out.should_shift = true;
        out.adjusts_move_target = false;
        out.side = waiting_side;
        out.delta_x_q8 = waiting_side * shift_q8;
    }

    return out;
}

} // namespace skyrecomp::camera
