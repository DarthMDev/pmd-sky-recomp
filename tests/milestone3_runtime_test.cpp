#ifdef NDEBUG
#undef NDEBUG
#endif

#include "skyrecomp/guest_mem.hpp"
#include "skyrecomp/pmdsky_addrs.hpp"
#include "../runtime/camera/dungeon_camera.hpp"
#include "../runtime/camera/subpixel_interpolator.hpp"
#include "../runtime/gpu2d/widescreen_ppu.hpp"
#include "../runtime/shaders/weather_shader_pipeline.hpp"
#include "../runtime/ui/guest_ui_state.hpp"
#include "../runtime/ui/remaster_hud.hpp"
#include "../runtime/ui/settings_overlay.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace skyrecomp;

/// Writes a trivially copyable value into simulated guest Main RAM with bounds checking.
template <typename T>
void WriteGuest(std::vector<uint8_t>& ram, uint32_t guest_addr, T value) {
    assert(guest_addr >= layout::kMainRamBase);
    const size_t offset = static_cast<size_t>(guest_addr - layout::kMainRamBase);
    assert(offset + sizeof(T) <= ram.size());
    if (guest_addr < layout::kMainRamBase || offset + sizeof(T) > ram.size()) {
        std::abort();
    }
    std::memcpy(ram.data() + offset, &value, sizeof(T));
}

/// Tests widescreen PPU metrics, BG scanlines, OAM sprite remapping, and HUD anchoring.
void TestWidescreenPpuAndHudAnchoring() {
    using namespace skyrecomp::gpu2d;

    static_assert(WidescreenPpu::ComputeMetrics(AspectRatioPreset::Vanilla_4_3).width == 256);
    static_assert(WidescreenPpu::ComputeMetrics(AspectRatioPreset::SteamDeck_16_10).width == 308);
    static_assert(WidescreenPpu::ComputeMetrics(AspectRatioPreset::Widescreen_16_9).width == 342);
    static_assert(WidescreenPpu::ComputeMetrics(AspectRatioPreset::Ultrawide_21_9).width == 448);

    WidescreenPpu ppu(AspectRatioPreset::Widescreen_16_9, HudLayoutMode::DualScreenAnchored);
    assert(ppu.GetMetrics().left_margin == 43);
    assert(ppu.GetMetrics().right_margin == 43);

    // 1. Verify OAM HUD anchor classification & coordinate remapping
    WidescreenOamSprite left_hud_sprite{
        .vanilla_x = 12,
        .vanilla_y = 8,
        .anchor = ppu.ClassifyOamHudAnchor(12, 8, 0, true, false),
    };
    assert(left_hud_sprite.anchor == HudAnchor::LeftEdge);
    assert(ppu.RemapSpriteX(left_hud_sprite) == 12);

    WidescreenOamSprite right_hud_sprite{
        .vanilla_x = 220,
        .vanilla_y = 8,
        .anchor = ppu.ClassifyOamHudAnchor(220, 8, 0, true, false),
    };
    assert(right_hud_sprite.anchor == HudAnchor::RightEdge);
    assert(ppu.RemapSpriteX(right_hud_sprite) == 220 + (342 - 256));

    WidescreenOamSprite dialogue_sprite{
        .vanilla_x = 32,
        .vanilla_y = 144,
        .anchor = ppu.ClassifyOamHudAnchor(32, 144, 0, true, true),
    };
    assert(dialogue_sprite.anchor == HudAnchor::Center);
    assert(ppu.RemapSpriteX(dialogue_sprite) == 32 + 43);

    // World-space monster sprite with +1.5px (+384 in 24.8 fixed-point) sub-pixel offset
    WidescreenOamSprite monster_sprite{
        .vanilla_x = 128,
        .vanilla_y = 96,
        .anchor = HudAnchor::WorldSpace,
        .subpixel_dx_q8 = 384,
        .subpixel_dy_q8 = -256,
    };
    assert(ppu.RemapSpriteX(monster_sprite) == 128 + 43 + 2);
    assert(ppu.RemapSpriteY(monster_sprite) == 96 - 1);

    // 2. Verify hardware window (WIN0H) widescreen remapping
    const auto full_win = ppu.RemapWindowHorizontal(0, 255, HudAnchor::Center);
    assert(full_win.x1 == 0 && full_win.x2 == 342);

    const auto centered_win = ppu.RemapWindowHorizontal(16, 240, HudAnchor::Center);
    assert(centered_win.x1 == 16 + 43 && centered_win.x2 == 240 + 43);

    // 3. Verify 512x256 scrollable BG scanline vs non-repeating 256px dialogue BG layer
    BgLayerConfig world_bg{
        .enabled = true,
        .role = BgLayerRole::WorldTilemap,
        .virtual_width = 512,
        .virtual_height = 256,
        .hofs = 10,
        .vofs = 0,
    };
    // At widescreen left margin (screen_x == 0), world BG samples (10 - 43) mod 512 = 479
    assert(ppu.SampleBgSourceX(world_bg, 0) == 479);
    // At vanilla left edge (screen_x == 43), world BG samples 10
    assert(ppu.SampleBgSourceX(world_bg, 43) == 10);

    BgLayerConfig dlg_bg{
        .enabled = true,
        .role = BgLayerRole::DialogueWindow,
        .virtual_width = 256,
        .virtual_height = 256,
    };
    // Dialogue window must NOT repeat in the widescreen left/right margins
    assert(ppu.SampleBgSourceX(dlg_bg, 0) == -1);
    assert(ppu.SampleBgSourceX(dlg_bg, 42) == -1);
    assert(ppu.SampleBgSourceX(dlg_bg, 43) == 0);
    assert(ppu.SampleBgSourceX(dlg_bg, 43 + 255) == 255);
    assert(ppu.SampleBgSourceX(dlg_bg, 43 + 256) == -1);

    // 4. Verify Single-Screen Modern Layout HUD rendering
    ppu.SetHudLayoutMode(HudLayoutMode::SingleScreenModern);
    std::vector<uint32_t> fb(342 * 192, 0xFF000000u);
    ModernHudState hud{};
    hud.leader_hp = 80;
    hud.leader_max_hp = 100;
    hud.belly = 90;
    hud.max_belly = 100;
    hud.minimap_enabled = true;
    hud.minimap_cells[5 * 56 + 10] = 3u; // Leader marker
    ppu.RenderModernSingleScreenHud(hud, fb);
    // Top-left HP bar pixel should be modified from black
    assert(fb[10 * 342 + 20] != 0xFF000000u);
    // Top-right minimap leader dot should be modified
    const int map_x0 = 342 - 56 - 8;
    assert(fb[(6 + 5) * 342 + (map_x0 + 10)] != 0xFF000000u);
}

/// Verifies Overlay 29 dungeon camera visibility and widescreen tile streaming.
void TestOverlay29CameraAndTileStreaming() {
    using namespace skyrecomp::camera;
    using namespace skyrecomp::gpu2d;

    static_assert(DungeonCameraHook::ComputeHorizontalTileRadius(256) == 6);
    static_assert(DungeonCameraHook::ComputeHorizontalTileRadius(308) == 8);
    static_assert(DungeonCameraHook::ComputeHorizontalTileRadius(342) == 9);
    static_assert(DungeonCameraHook::ComputeHorizontalTileRadius(448) == 11);

    std::vector<uint8_t> ram(layout::kMainRamSize, 0u);
    constexpr uint32_t kDungeonAddr = 0x02100000u;
    constexpr uint32_t kLeaderEntityAddr =
        kDungeonAddr + layout::dungeon::kEntityTableOffset + layout::entity_table::kEntitiesArrayOffset;
    constexpr uint32_t kTilePoolAddr = 0x02200000u;

    WriteGuest<uint32_t>(ram, pmdsky::NA.DUNGEON_PTR_MASTER, kDungeonAddr);

    // Configure leader entity in room #2 at tile (20, 15)
    WriteGuest<int32_t>(ram, kLeaderEntityAddr + layout::entity::kTypeOffset, 1);
    WriteGuest<uint8_t>(ram, kLeaderEntityAddr + layout::entity::kRoomIdxOffset, 2u);

    // Configure display_data camera centered at tile (20, 15)
    const uint32_t disp_addr = kDungeonAddr + layout::dungeon::kDisplayDataOffset;
    WriteGuest<int16_t>(ram, disp_addr + layout::display_data::kCameraPosOffset + 0u, 20);
    WriteGuest<int16_t>(ram, disp_addr + layout::display_data::kCameraPosOffset + 2u, 15);
    WriteGuest<int16_t>(ram, disp_addr + layout::display_data::kCameraPixelPosOffset + 0u, 20 * 24 - 128);
    WriteGuest<int16_t>(ram, disp_addr + layout::display_data::kCameraPixelPosOffset + 2u, 15 * 24 - 96);
    WriteGuest<uint32_t>(ram, disp_addr + layout::display_data::kCameraTargetPtrOffset, kLeaderEntityAddr);
    WriteGuest<uint8_t>(ram, disp_addr + layout::display_data::kVisibilityRangeOffset, 2u);
    WriteGuest<uint8_t>(ram, disp_addr + layout::display_data::kNaturalLightingOffset, 1u);

    // Populate dungeon tile_ptrs[32][56]
    uint32_t next_tile_addr = kTilePoolAddr;
    for (int ty = 0; ty < layout::dungeon::kMaxGridY; ++ty) {
        for (int tx = 0; tx < layout::dungeon::kMaxGridX; ++tx) {
            const uint32_t ptr_slot =
                kDungeonAddr + layout::dungeon::kTilePtrsOffset +
                static_cast<uint32_t>((ty * layout::dungeon::kMaxGridX + tx) * sizeof(uint32_t));
            WriteGuest<uint32_t>(ram, ptr_slot, next_tile_addr);
            WriteGuest<uint16_t>(ram, next_tile_addr + layout::tile::kTerrainFlagsOffset, 1u); // floor
            WriteGuest<uint16_t>(ram, next_tile_addr + layout::tile::kVisibilityFlagsOffset, 1u); // revealed
            WriteGuest<uint8_t>(ram, next_tile_addr + layout::tile::kRoomIndexOffset, 2u); // room #2
            next_tile_addr += layout::tile::kSize;
        }
    }

    GuestMemoryView mem(ram.data(), ram.size());
    DungeonCameraHook cam_hook(pmdsky::Region::NA, AspectRatioPreset::Widescreen_16_9);
    const auto snap = cam_hook.ReadDisplaySnapshot(mem);
    assert(snap.valid);
    assert(snap.camera_tile_x == 20 && snap.camera_tile_y == 15);
    assert(snap.target_room_idx == 2u);

    const auto win = cam_hook.ComputeStreamingWindow(snap);
    assert(win.horizontal_tile_radius == 9);
    assert(win.min_tile_x == 11 && win.max_tile_x == 29);
    assert(win.extra_margin_columns_left == 3);
    assert(win.extra_margin_columns_right == 3);

    // Tile at dx = 8 (widescreen margin) in room #2 should be visible in 16:9!
    assert(cam_hook.IsPositionVisibleAndOnWidescreen(snap, 28, 15, 2u, true));
    // Tile at dx = 10 is outside 16:9 frustum (radius 9)
    assert(!cam_hook.IsPositionVisibleAndOnWidescreen(snap, 30, 15, 2u, true));

    // In 21:9 ultrawide (radius 11), tile at dx = 10 IS visible!
    cam_hook.SetAspectRatio(AspectRatioPreset::Ultrawide_21_9);
    assert(cam_hook.IsPositionVisibleAndOnWidescreen(snap, 30, 15, 2u, true));

    // In a hallway (target_room_idx = 0xFF), fog-of-war restricts visibility to visibility_range (2 tiles)
    DungeonDisplaySnapshot hall_snap = snap;
    hall_snap.target_room_idx = 0xFFu;
    assert(cam_hook.IsPositionVisibleAndOnWidescreen(hall_snap, 22, 15, 0xFFu, true));
    assert(!cam_hook.IsPositionVisibleAndOnWidescreen(hall_snap, 25, 15, 0xFFu, true));

    // Stream visible tiles and verify margin tiles are streamed seamlessly
    const auto streamed = cam_hook.StreamVisibleTiles(mem, snap);
    assert(streamed.size() == static_cast<size_t>((11 * 2 + 1) * (5 * 2 + 1)));
    bool saw_margin_tile = false;
    for (const auto& t : streamed) {
        if (t.in_widescreen_margin && t.illuminated_by_fov) {
            saw_margin_tile = true;
            break;
        }
    }
    assert(saw_margin_tile);
}

/// Verifies high-refresh-rate sub-pixel interpolation without mutating guest RAM.
void TestSubpixelInterpolationDeterminism() {
    using namespace skyrecomp::camera;

    std::vector<uint8_t> ram(layout::kMainRamSize, 0u);
    constexpr uint32_t kDungeonAddr = 0x02100000u;
    constexpr uint32_t kEntity0Addr =
        kDungeonAddr + layout::dungeon::kEntityTableOffset + layout::entity_table::kEntitiesArrayOffset;
    const uint32_t disp_addr = kDungeonAddr + layout::dungeon::kDisplayDataOffset;

    WriteGuest<uint32_t>(ram, pmdsky::NA.DUNGEON_PTR_MASTER, kDungeonAddr);
    WriteGuest<int16_t>(ram, disp_addr + layout::display_data::kFloorTouchScreenOffset, 5);

    // Tick 1: camera at (100, 80) px, entity 0 at (120.0, 96.0) px in 24.8 fixed-point
    WriteGuest<int16_t>(ram, disp_addr + layout::display_data::kCameraPixelPosOffset + 0u, 100);
    WriteGuest<int16_t>(ram, disp_addr + layout::display_data::kCameraPixelPosOffset + 2u, 80);
    WriteGuest<int32_t>(ram, kEntity0Addr + layout::entity::kTypeOffset, 1);
    WriteGuest<uint16_t>(ram, kEntity0Addr + layout::entity::kSpawnGenIdOffset, 42u);
    WriteGuest<int32_t>(ram, kEntity0Addr + layout::entity::kPixelPosOffset + 0u, 120 * 256);
    WriteGuest<int32_t>(ram, kEntity0Addr + layout::entity::kPixelPosOffset + 4u, 96 * 256);

    SubpixelInterpolator interp(pmdsky::Region::NA, RefreshRateMode::Esports_240Hz);
    interp.CaptureLogicTick(GuestMemoryView(ram.data(), ram.size()));

    // Tick 2: camera moves +4 px to (104, 80), entity 0 moves +4 px to (124.0, 96.0)
    WriteGuest<int16_t>(ram, disp_addr + layout::display_data::kCameraPixelPosOffset + 0u, 104);
    WriteGuest<int32_t>(ram, kEntity0Addr + layout::entity::kPixelPosOffset + 0u, 124 * 256);

    // Snapshot copy of guest RAM before CaptureLogicTick + EvaluateSubFrame to prove zero guest mutation
    const std::vector<uint8_t> ram_snapshot = ram;

    interp.CaptureLogicTick(GuestMemoryView(ram.data(), ram.size()));
    assert(interp.HasTwoSnapshots());

    // Evaluate 240Hz sub-frames (alpha = 0.25, 0.50, 0.75, 1.00)
    const auto alphas_240 = SubpixelInterpolator::ComputeSubFrameAlphas(240);
    assert(alphas_240.size() == 4);

    const auto sf_quarter = interp.EvaluateSubFrame(0.25f);
    assert(sf_quarter.camera_x_q8 == 101 * 256);
    assert(sf_quarter.dungeon_poses[0].active);
    assert(!sf_quarter.dungeon_poses[0].snapped_discontinuity);
    assert(sf_quarter.dungeon_poses[0].interp_x_q8 == 121 * 256);
    assert(sf_quarter.dungeon_poses[0].subpixel_dx_q8 == -3 * 256);

    const auto sf_half = interp.EvaluateSubFrame(0.50f);
    assert(sf_half.camera_x_q8 == 102 * 256);
    assert(sf_half.dungeon_poses[0].interp_x_q8 == 122 * 256);
    assert(sf_half.dungeon_poses[0].subpixel_dx_q8 == -2 * 256);

    // Verify guest RAM is 100% bit-identical (zero breakage of turn-based determinism)
    assert(ram == ram_snapshot);

    // Tick 3: Warp Seed teleport (+120 pixels > 24px threshold) must snap immediately without smear!
    WriteGuest<int32_t>(ram, kEntity0Addr + layout::entity::kPixelPosOffset + 0u, 244 * 256);
    interp.CaptureLogicTick(GuestMemoryView(ram.data(), ram.size()));
    const auto sf_warp = interp.EvaluateSubFrame(0.50f);
    assert(sf_warp.dungeon_poses[0].snapped_discontinuity);
    assert(sf_warp.dungeon_poses[0].interp_x_q8 == 244 * 256);
    assert(sf_warp.dungeon_poses[0].subpixel_dx_q8 == 0);

    // Also verify Overlay 11 Ground Mode interpolation
    WriteGuest<uint32_t>(ram, pmdsky::NA.DUNGEON_PTR_MASTER, 0u);
    constexpr uint32_t kGroundBgAddr = 0x02180000u;
    constexpr uint32_t kGroundActorsAddr = 0x02190000u;
    WriteGuest<uint32_t>(ram, pmdsky::NA.GROUND_STATE_MAP, kGroundBgAddr);
    WriteGuest<uint32_t>(ram, pmdsky::NA.GROUND_STATE_PTRS + layout::ground::kGroundActorsPtrOffset, kGroundActorsAddr);

    WriteGuest<int32_t>(ram, kGroundBgAddr + layout::ground::kGroundBgCameraPixelPosOffset + 0u, 50 * 256);
    WriteGuest<uint8_t>(ram, kGroundActorsAddr + layout::ground::kLiveActorEnabledOffset, 1u);
    WriteGuest<int16_t>(ram, kGroundActorsAddr + layout::ground::kLiveActorIdOffset, 7);
    WriteGuest<int32_t>(ram, kGroundActorsAddr + layout::ground::kLiveActorCoordMinOffset + 0u, 80 * 256);

    interp.Reset();
    interp.CaptureLogicTick(GuestMemoryView(ram.data(), ram.size()));
    WriteGuest<int32_t>(ram, kGroundBgAddr + layout::ground::kGroundBgCameraPixelPosOffset + 0u, 52 * 256);
    WriteGuest<int32_t>(ram, kGroundActorsAddr + layout::ground::kLiveActorCoordMinOffset + 0u, 84 * 256);
    interp.CaptureLogicTick(GuestMemoryView(ram.data(), ram.size()));

    const auto ground_sf = interp.EvaluateSubFrame(0.50f);
    assert(ground_sf.mode == ActiveGameMode::GroundOverlay11);
    assert(ground_sf.camera_x_q8 == 51 * 256);
    assert(ground_sf.ground_poses[0].active);
    assert(ground_sf.ground_poses[0].interp_x_q8 == 82 * 256);
}

/// Verifies the weather shader pipeline, uniform generation, and CPU post-process LUT path.
void TestWeatherShaderPipeline() {
    using namespace skyrecomp::shaders;

    WeatherShaderPipeline pipeline(
        pmdsky::Region::NA,
        ShaderFilterPreset::SharpBilinear,
        true);
    assert(pipeline.GetWeatherColorTableGuestAddress() == 0x022DE620u);
    assert(pipeline.GetMetalShaderSource().find("sky_hd_fragment_main") != std::string_view::npos);
    assert(pipeline.GetMetalShaderSource().find("sharp_bilinear_uv") != std::string_view::npos);

    constexpr uint32_t pixels[] = {0xFFFF0000u, 0xFF0000FFu};
    assert(WeatherShaderPipeline::SampleSharpBilinearArgb(
        pixels, 2, 1, 0.25f, 0.5f, 4.0f, ShaderFilterPreset::SharpBilinear) == pixels[0]);
    assert(WeatherShaderPipeline::SampleSharpBilinearArgb(
        pixels, 2, 1, 0.0f, 0.5f, 4.0f, ShaderFilterPreset::SharpBilinear) == pixels[0]);

    std::vector<uint8_t> ram(layout::kMainRamSize, 0u);
    constexpr uint32_t kDungeonAddr = 0x02100000u;
    WriteGuest<uint32_t>(ram, pmdsky::NA.DUNGEON_PTR_MASTER, kDungeonAddr);
    WriteGuest<uint8_t>(ram, kDungeonAddr + layout::dungeon::kWeatherOffset, static_cast<uint8_t>(DungeonWeatherId::Sunny));

    // Populate dungeon->color_table with a warm Sunny LUT
    for (int i = 0; i < 256; ++i) {
        const uint32_t r = std::min(255, i + 12);
        const uint32_t g = static_cast<uint32_t>(i);
        const uint32_t b = static_cast<uint32_t>(std::max(0, i - 12));
        const uint32_t rgba = r | (g << 8) | (b << 16) | 0xFF000000u;
        WriteGuest<uint32_t>(
            ram,
            kDungeonAddr + layout::dungeon::kColorTableOffset + static_cast<uint32_t>(i * 4),
            rgba);
    }

    GuestMemoryView mem(ram.data(), ram.size());
    const auto wstate = pipeline.ReadWeatherStateFromGuest(mem);
    assert(wstate.in_dungeon);
    assert(wstate.weather == DungeonWeatherId::Sunny);
    assert(wstate.has_custom_color_table);

    const auto uniforms = pipeline.BuildUniforms(342, 192, 1368, 768, wstate);
    assert(uniforms.weather_id == static_cast<uint32_t>(DungeonWeatherId::Sunny));
    assert(uniforms.bloom_enabled == 1u);
    assert(uniforms.weather_tint[0] > uniforms.weather_tint[2]); // Warm sunny red > blue

    // Apply post-processing to a mid-tone test frame and verify exact per-pixel LUT application
    // without double-applying the mid-tone LUT tint shift in BuildUniforms.
    std::vector<uint32_t> mid_fb(4, 0xFF808080u);
    pipeline.SetWeatherBloomEnabled(false);
    pipeline.ApplyPostProcessArgb8888(mid_fb, 2, 2, wstate);
    // LUT entry 0x80 has r=140, g=128, b=116; multiplied by unshifted Sunny tint (1.08, 1.02, 0.90)
    // and 4% Sunny haze (1.00, 0.85, 0.55): r = 155, g = 134, b = 106.
    assert(((mid_fb[0] >> 16) & 0xFFu) == 155u);
    assert(((mid_fb[0] >> 8) & 0xFFu) == 134u);
    assert((mid_fb[0] & 0xFFu) == 106u);

    // Apply post-processing to a bright highlight test frame and verify warm bloom boost
    pipeline.SetWeatherBloomEnabled(true);
    std::vector<uint32_t> fb(16 * 16, 0xFFC8C8C8u);
    pipeline.ApplyPostProcessArgb8888(fb, 16, 16, wstate);
    const uint32_t out_px = fb[0];
    const uint32_t out_r = (out_px >> 16) & 0xFFu;
    const uint32_t out_b = out_px & 0xFFu;
    assert(out_r > out_b);
}

/// Verifies the live party/weather bridge against a synthetic PMD Sky RAM image.
void TestGuestUiStateBridge() {
    using namespace skyrecomp;

    std::vector<uint8_t> ram(layout::kMainRamSize, 0);
    constexpr uint32_t kTeamMemberTable = 0x022ABDE0u;
    constexpr uint32_t kRosterOffset = 0x936Cu;
    constexpr uint32_t kActiveTeamOffset = 0x9877u;
    constexpr uint32_t kTeamMemberSize = 104u;
    constexpr uint32_t kEntityTableActiveMonsters = 0x50u;
    constexpr uint32_t kMonsterIsNotTeamMember = 0x06u;
    constexpr uint32_t kMonsterIsLeader = 0x07u;
    constexpr uint32_t kMonsterLevel = 0x0Au;
    constexpr uint32_t kMonsterTeamIndex = 0x0Cu;
    constexpr uint32_t kMonsterHp = 0x10u;
    constexpr uint32_t kMonsterMaxHpStat = 0x12u;
    constexpr uint32_t kMonsterMaxHpBoost = 0x16u;
    constexpr uint32_t kMonsterBelly = 0x146u;
    constexpr uint32_t kMonsterMaxBelly = 0x14Au;

    WriteGuest<uint8_t>(ram, kTeamMemberTable + kActiveTeamOffset, 0);
    auto write_member = [&](uint32_t slot, bool leader, uint8_t level,
                            int16_t hp, int16_t max_hp, int16_t belly,
                            int16_t max_belly, const char* name) {
        const uint32_t addr =
            kTeamMemberTable + kRosterOffset + slot * kTeamMemberSize;
        WriteGuest<uint8_t>(ram, addr, 1);
        WriteGuest<uint8_t>(ram, addr + 1, leader ? 1 : 0);
        WriteGuest<uint8_t>(ram, addr + 2, level);
        WriteGuest<int16_t>(ram, addr + 0x0E, hp);
        WriteGuest<int16_t>(ram, addr + 0x10, max_hp);
        WriteGuest<int16_t>(ram, addr + 0x44, belly);
        WriteGuest<int16_t>(ram, addr + 0x48, max_belly);
        for (size_t i = 0; name[i] != '\0' && i < 10; ++i) {
            WriteGuest<uint8_t>(ram, addr + 0x5E + static_cast<uint32_t>(i),
                                static_cast<uint8_t>(name[i]));
        }
    };
    write_member(0, true, 12, 40, 80, 25, 50, "PIKACHU");
    write_member(1, false, 10, 30, 60, 20, 50, "EEVEE");

    const uint32_t dungeon = 0x02080000u;
    WriteGuest<uint32_t>(
        ram, pmdsky::Get(pmdsky::Region::NA).DUNGEON_PTR_MASTER, dungeon);
    WriteGuest<uint8_t>(ram, dungeon + layout::dungeon::kWeatherOffset, 4);

    const uint32_t entity_table =
        dungeon + layout::dungeon::kEntityTableOffset;
    const uint32_t entity = 0x02081000u;
    const uint32_t monster = 0x02082000u;
    WriteGuest<uint32_t>(
        ram, entity_table + kEntityTableActiveMonsters, entity);
    WriteGuest<int32_t>(ram, entity + layout::entity::kTypeOffset, 1);
    WriteGuest<uint32_t>(ram, entity + layout::entity::kInfoPtrOffset,
                         monster);
    WriteGuest<uint8_t>(ram, monster + kMonsterIsNotTeamMember, 0);
    WriteGuest<uint8_t>(ram, monster + kMonsterIsLeader, 1);
    WriteGuest<uint8_t>(ram, monster + kMonsterLevel, 13);
    WriteGuest<int16_t>(ram, monster + kMonsterTeamIndex, 0);
    WriteGuest<int16_t>(ram, monster + kMonsterHp, 35);
    WriteGuest<int16_t>(ram, monster + kMonsterMaxHpStat, 80);
    WriteGuest<int16_t>(ram, monster + kMonsterMaxHpBoost, 0);
    WriteGuest<int16_t>(ram, monster + kMonsterBelly, 22);
    WriteGuest<int16_t>(ram, monster + kMonsterMaxBelly, 50);

    const GuestMemoryView mem(ram.data(), ram.size());
    const auto party = ui::ReadGuestParty(mem, pmdsky::Region::NA);
    assert(party.count >= 2);
    assert(party.party[0].active);
    assert(party.party[0].leader);
    assert(party.party[0].hp == 35);
    assert(party.party[0].max_hp == 80);
    assert(party.party[0].belly == 22);
    assert(std::strcmp(party.party[0].name.data(), "PIKACHU") == 0);
    assert(party.party[1].active);
    assert(party.party[1].hp == 30);
    assert(std::strcmp(party.party[1].name.data(), "EEVEE") == 0);
    assert(ui::ReadGuestWeatherId(mem, pmdsky::Region::NA) == 4);

    constexpr uint32_t kWindowListNA = 0x022A88DCu;
    WriteGuest<int8_t>(ram, kWindowListNA + 0xB6, 1);
    WriteGuest<uint8_t>(ram, kWindowListNA + 0x08, 1);  // SCREEN_SUB = top
    WriteGuest<uint8_t>(
        ram,
        dungeon + layout::dungeon::kDisplayDataOffset +
            layout::display_data::kTeamMenuOrGridOffset,
        1);
    const auto flags = ui::ReadGuestUiFlags(mem, pmdsky::Region::NA);
    assert(flags.top_screen_menu_open);
    assert(flags.team_menu_or_grid);
}

/// Verifies the layout-first remaster HUD draws party, floor/weather, and minimap.
void TestRemasterHudLayout() {
    using namespace skyrecomp::ui;

    RemasterHudState state{};
    state.party[0].active = true;
    state.party[0].leader = true;
    state.party[0].level = 12;
    state.party[0].hp = 40;
    state.party[0].max_hp = 80;
    state.party[0].belly = 25;
    state.party[0].max_belly = 50;
    std::strcpy(state.party[0].name.data(), "PIKACHU");
    state.party[1].active = true;
    state.party[1].level = 10;
    state.party[1].hp = 30;
    state.party[1].max_hp = 60;
    state.party[1].belly = 20;
    state.party[1].max_belly = 50;
    std::strcpy(state.party[1].name.data(), "EEVEE");
    state.party_count = 2;
    state.floor_number = 7;
    state.weather_id = 4;
    state.minimap_enabled = true;
    state.minimap_cells[5 * 56 + 10] = 3;

    RemasterHud hud;
    hud.SetState(state);
    std::vector<uint32_t> fb(342 * 192, 0xFF000000u);
    hud.RenderArgb8888(fb, 342, 192);

    // Leader panel changed the top-left.
    assert(fb[6 * 342 + 6] != 0xFF000000u);
    // Minimap changed near the top-right edge.
    assert(fb[8 * 342 + (342 - 56 - 8 + 10)] != 0xFF000000u);

    std::vector<uint32_t> fps_fb(342 * 192, 0xFF000000u);
    RemasterHud::RenderFpsMeter(fps_fb, 342, 192, 59.9);
    assert(fps_fb[5 * 342 + 280] != 0xFF000000u);
}

/// Verifies recomp-ui settings overlay input, rendering, runtime bindings, and persistence.
void TestRecompUiSettingsOverlay() {
    using namespace skyrecomp::ui;

    SkySettingsOverlay overlay;
    assert(overlay.IsValid());
    assert(!overlay.IsVisible());

    gpu2d::WidescreenPpu ppu;
    camera::DungeonCameraHook cam_hook;
    camera::SubpixelInterpolator interp;
    shaders::WeatherShaderPipeline shader_pipe;
    int observed_volume = 0;
    bool observed_muted = true;
    DsButtonMapping observed_mapping{};

    const auto tmp_cfg_path =
        (std::filesystem::temp_directory_path() / "pmd_sky_recomp_test_settings.toml").string();
    std::filesystem::remove(tmp_cfg_path);

    overlay.BindRuntimeConsumers(RuntimeConsumerBindings{
        .ppu = &ppu,
        .camera_hook = &cam_hook,
        .interpolator = &interp,
        .shader_pipeline = &shader_pipe,
        .on_audio_settings_changed = [&](int vol, bool muted) {
            observed_volume = vol;
            observed_muted = muted;
        },
        .on_controller_mapping_changed = [&](const DsButtonMapping& mapping) {
            observed_mapping = mapping;
        },
        .config_file_path = tmp_cfg_path,
    });
    assert(observed_volume == 100);
    assert(!observed_muted);
    assert(observed_mapping.confirm_bit == (1u << 0));

    // 1. Toggle overlay open via F1 key press
    assert(overlay.HandleInput(OverlayInputState{.f1_pressed = true}));
    assert(overlay.IsVisible());

    // Settings now open on the category list; enter Video before changing rows.
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_ACCEPT});

    // 2. Adjust Aspect Ratio (item 0) from 16:9 -> 21:9 using Right action
    assert(overlay.GetSelectedIndex() == static_cast<size_t>(SettingsItemIndex::AspectRatio));
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_RIGHT});
    assert(overlay.GetState().aspect_ratio == gpu2d::AspectRatioPreset::Ultrawide_21_9);
    // Bound subsystems should automatically receive the change immediately
    assert(ppu.GetAspectRatio() == gpu2d::AspectRatioPreset::Ultrawide_21_9);
    assert(cam_hook.GetAspectRatio() == gpu2d::AspectRatioPreset::Ultrawide_21_9);

    // 3. Navigate down to HUD Layout (item 1) and toggle to Single-Screen Modern
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_DOWN});
    assert(overlay.GetSelectedIndex() == static_cast<size_t>(SettingsItemIndex::HudLayout));
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_RIGHT});
    assert(overlay.GetState().hud_layout == gpu2d::HudLayoutMode::SingleScreenModern);
    assert(ppu.GetHudLayoutMode() == gpu2d::HudLayoutMode::SingleScreenModern);

    // 4. Navigate down to Refresh Interpolation (item 2) and switch from 120Hz -> 144Hz
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_DOWN});
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_RIGHT});
    assert(overlay.GetState().refresh_rate == camera::RefreshRateMode::HighRefresh_144Hz);
    assert(interp.GetRefreshRateMode() == camera::RefreshRateMode::HighRefresh_144Hz);

    // 5. Render the recomp-ui overlay onto a 342x192 ARGB8888 framebuffer
    std::vector<uint32_t> fb(342 * 192, 0xFF204060u);
    assert(overlay.RenderArgb8888(fb, 342, 192));
    // Verify center panel pixels were drawn over the backdrop
    assert(fb[96 * 342 + 171] != 0xFF204060u);

    // 6. Navigate to AudioVolume (item 5), lower volume to 80%, and test audio sample scaling
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_DOWN}); // ShaderFilter
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_DOWN}); // WeatherBloom
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_DOWN}); // AudioVolume
    assert(overlay.GetSelectedIndex() == static_cast<size_t>(SettingsItemIndex::AudioVolume));
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_LEFT}); // 100 -> 90
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_LEFT}); // 90 -> 80
    assert(observed_volume == 80);
    std::vector<int16_t> samples = {1000, -1000};
    overlay.ScaleAudioSamplesInt16(samples);
    assert(samples[0] == 800 && samples[1] == -800);
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_DOWN});
    assert(overlay.GetSelectedIndex() == static_cast<size_t>(SettingsItemIndex::FpsMeter));
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_RIGHT});
    assert(overlay.GetState().show_fps_meter);

    // Return to the section list, enter Controller, and choose the swapped A/B preset.
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_BACK});
    assert(overlay.GetSelectedIndex() == 0); // Video section
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_DOWN});
    assert(overlay.GetSelectedIndex() == 1); // Controller section
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_ACCEPT});
    assert(overlay.GetSelectedIndex() == 0); // Preset row
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_RIGHT}); // Modern Dungeon
    (void)overlay.HandleInput(OverlayInputState{.nav_input = RECOMP_RUNTIME_UI_INPUT_RIGHT}); // Swapped B/A
    assert(overlay.GetState().controller_preset == ControllerRemapPreset::SwappedConfirmCancel);
    assert(observed_mapping.confirm_bit == (1u << 0));
    assert(observed_mapping.cancel_bit == (1u << 1));
    // The preset edits gamepad bindings directly: SDL EAST (code 2) now
    // generates DS A, while SDL SOUTH (code 1) generates DS B.
    assert(overlay.GetState().controller_bindings[4] == 2);
    assert(overlay.GetState().controller_bindings[5] == 1);
    const uint16_t remapped_mask = overlay.RemapDsKeyinputMask(0x0001u);
    assert((remapped_mask & 0x0001u) != 0u);
    assert((remapped_mask & 0x0002u) == 0u);

    // 7. Test TOML and file round-trip persistence (CallbackSave already persisted to tmp_cfg_path)
    SkySettingsOverlay restored;
    gpu2d::WidescreenPpu restored_ppu;
    restored.BindRuntimeConsumers(RuntimeConsumerBindings{.ppu = &restored_ppu});
    assert(restored.LoadFromFile(tmp_cfg_path));
    assert(restored.GetState().aspect_ratio == gpu2d::AspectRatioPreset::Ultrawide_21_9);
    assert(restored.GetState().hud_layout == gpu2d::HudLayoutMode::SingleScreenModern);
    assert(restored.GetState().refresh_rate == camera::RefreshRateMode::HighRefresh_144Hz);
    assert(restored.GetState().show_fps_meter);
    assert(restored.GetState().controller_preset == ControllerRemapPreset::SwappedConfirmCancel);
    assert(restored.GetState().controller_bindings[4] == 2);
    assert(restored.GetState().controller_bindings[5] == 1);
    assert(restored.GetState().keyboard_bindings == kDefaultKeyboardBindings);
    assert(restored_ppu.GetAspectRatio() == gpu2d::AspectRatioPreset::Ultrawide_21_9);
    assert(restored_ppu.GetHudLayoutMode() == gpu2d::HudLayoutMode::SingleScreenModern);
    std::filesystem::remove(tmp_cfg_path);

    // 8. Toggle overlay closed via Gamepad Select+Start chord
    assert(!overlay.HandleInput(OverlayInputState{
        .gamepad_select_held = true,
        .gamepad_start_held = true,
    }));
    assert(!overlay.IsVisible());
}

/// Verifies widescreen edge cases ported from Asphaltian/pmd-space.
void TestPmdSpaceWidescreenEdgeCases() {
    using namespace skyrecomp::gpu2d;
    using namespace skyrecomp::camera;

    WidescreenPpu ppu(AspectRatioPreset::Widescreen_16_9);
    DungeonCameraHook cam_hook(pmdsky::Region::NA, AspectRatioPreset::Widescreen_16_9);

    // 1. FullScreenRectangles & DungeonDarkness 3D element expansion (margin = 43)
    Render3dElement64Quad rect_quad{
        .type = 2u,
        .vx = {0, 256, 0, 0},
        .vy = {0, 192, 0, 0},
    };
    assert(ppu.ExpandRender3dElementForWidescreen(rect_quad, false));
    assert(rect_quad.vx[0] == -43);
    assert(rect_quad.vx[1] == 256 + 86);

    Render3dElement64Quad dark_band{
        .type = 5u,
        .tex_w = 4,
        .tex_h = 4,
        .vx = {0, 256, 0, 256},
        .vy = {0, 0, 48, 48},
    };
    assert(ppu.ExpandRender3dElementForWidescreen(dark_band, true));
    assert(dark_band.vx[0] == -43 && dark_band.vx[1] == 256 + 43);

    // Room bounds partially overlapping the left widescreen margin (left = -20 relative to camera)
    RoomPixelBounds room_bounds{
        .left = 80,
        .top = 100,
        .right = 370,
        .bottom = 240,
    };
    const auto dark_pieces = ppu.BuildDungeonDarknessMarginQuads(room_bounds, 100, 90);
    assert(!dark_pieces.empty());

    // 2. MarginSprites culling shifts (Item, Shadow, Monster, OamPiece)
    // Item reach is [-32, 287]; at x = -48 (in left 16:9 margin), camera shift is -43
    assert(cam_hook.ComputeMarginCameraShift(-48, MarginSpriteKind::Item) == -43);
    assert(cam_hook.ComputeMarginCameraShift(300, MarginSpriteKind::Item) == +43);
    assert(cam_hook.ComputeMarginCameraShift(120, MarginSpriteKind::Item) == 0);
    assert(!cam_hook.IsOamPieceOffWidescreen(-90));
    assert(cam_hook.IsOamPieceOffWidescreen(-120));

    // 3. OffScreenActors waiting/moving adjustment in Ground Mode cutscenes
    std::vector<uint8_t> ram(layout::kMainRamSize, 0u);
    constexpr uint32_t kActorAddr = 0x02190000u;
    WriteGuest<uint8_t>(ram, kActorAddr + layout::ground::kLiveActorEnabledOffset, 1u);
    WriteGuest<int16_t>(ram, kActorAddr + layout::ground::kLiveActorKindOffset, 5);
    WriteGuest<int32_t>(ram, kActorAddr + layout::ground::kLiveActorHalfSizeOffset, 8 * 256);
    // Actor parked just off left screen edge (camera_x = 100, actor min_x = 80, max_x = 96, init_x = 88)
    WriteGuest<int32_t>(ram, kActorAddr + layout::ground::kLiveActorCoordMinOffset, 80 * 256);
    WriteGuest<int32_t>(ram, kActorAddr + layout::ground::kLiveActorCoordMaxOffset, 96 * 256);
    WriteGuest<int32_t>(ram, kActorAddr + layout::ground::kLiveActorInitialPosOffset, 88 * 256);

    GuestMemoryView mem(ram.data(), ram.size());
    const auto adj = cam_hook.ComputeOffScreenActorAdjustment(mem, kActorAddr, 100);
    assert(adj.should_shift);
    assert(!adj.adjusts_move_target);
    assert(adj.side == -1);
    assert(adj.delta_x_q8 == -43 * 256);
}

} // namespace

/// Entry point for the Milestone 3 runtime enhancement test suite.
int main() {
    TestWidescreenPpuAndHudAnchoring();
    TestOverlay29CameraAndTileStreaming();
    TestSubpixelInterpolationDeterminism();
    TestWeatherShaderPipeline();
    TestRecompUiSettingsOverlay();
    TestGuestUiStateBridge();
    TestRemasterHudLayout();
    TestPmdSpaceWidescreenEdgeCases();

    std::cout << "[milestone3_runtime_test] ALL MILESTONE 3 RUNTIME TESTS PASSED!\n";
    return 0;
}
