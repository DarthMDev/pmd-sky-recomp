#include "widescreen_ppu.hpp"

#include <algorithm>
#include <cmath>

namespace skyrecomp::gpu2d {
namespace {

[[nodiscard]] constexpr int WrapCoord(int val, int mod) noexcept {
    if (mod <= 0) {
        return 0;
    }
    const int r = val % mod;
    return (r < 0) ? (r + mod) : r;
}

[[nodiscard]] constexpr int RoundFixedQ8(int val_q8) noexcept {
    // Symmetric round-to-nearest from 24.8 fixed-point to integer pixel
    return (val_q8 >= 0) ? ((val_q8 + 128) >> 8) : -((-val_q8 + 128) >> 8);
}

[[nodiscard]] constexpr uint32_t AlphaBlendArgb(uint32_t dst, uint32_t src) noexcept {
    const uint32_t sa = (src >> 24) & 0xFFu;
    if (sa == 0u) {
        return dst;
    }
    if (sa == 255u) {
        return src;
    }
    const uint32_t inv_a = 255u - sa;
    const uint32_t r = (((src >> 16) & 0xFFu) * sa + ((dst >> 16) & 0xFFu) * inv_a) / 255u;
    const uint32_t g = (((src >> 8) & 0xFFu) * sa + ((dst >> 8) & 0xFFu) * inv_a) / 255u;
    const uint32_t b = ((src & 0xFFu) * sa + (dst & 0xFFu) * inv_a) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void FillRectArgb(
    std::span<uint32_t> fb,
    int fb_width,
    int fb_height,
    int x0,
    int y0,
    int w,
    int h,
    uint32_t color_argb) noexcept {
    const int x_start = std::max(0, x0);
    const int y_start = std::max(0, y0);
    const int x_end = std::min(fb_width, x0 + w);
    const int y_end = std::min(fb_height, y0 + h);
    if (x_start >= x_end || y_start >= y_end) {
        return;
    }
    for (int y = y_start; y < y_end; ++y) {
        const size_t row_off = static_cast<size_t>(y) * static_cast<size_t>(fb_width);
        for (int x = x_start; x < x_end; ++x) {
            fb[row_off + static_cast<size_t>(x)] = AlphaBlendArgb(fb[row_off + static_cast<size_t>(x)], color_argb);
        }
    }
}

} // namespace

WidescreenPpu::WidescreenPpu(AspectRatioPreset preset, HudLayoutMode hud_mode) noexcept
    : metrics_(ComputeMetrics(preset)), hud_mode_(hud_mode) {}

void WidescreenPpu::SetAspectRatio(AspectRatioPreset preset) noexcept {
    metrics_ = ComputeMetrics(preset);
}

HudAnchor WidescreenPpu::ClassifyOamHudAnchor(
    int vanilla_x,
    int vanilla_y,
    uint8_t bg_priority,
    bool is_hud_element,
    bool is_message_window_element) const noexcept {
    if (!is_hud_element && bg_priority > 0) {
        return HudAnchor::WorldSpace;
    }
    if (is_message_window_element || vanilla_y >= 128) {
        // Bottom dialogue / message log window and portrait sprites stay centered
        return HudAnchor::Center;
    }
    // Top HUD band (y < 32) or priority-0 HUD indicators:
    // Left third (0..104): floor, level, HP digits -> anchor to true left widescreen edge
    // Right third (152..256): status icons, weather/belly indicators -> anchor to true right widescreen edge
    // Center third (105..151): boss HP / floor banner -> anchor to center
    if (vanilla_x <= 104) {
        return HudAnchor::LeftEdge;
    }
    if (vanilla_x >= 152) {
        return HudAnchor::RightEdge;
    }
    return HudAnchor::Center;
}

int WidescreenPpu::RemapSpriteX(const WidescreenOamSprite& sprite) const noexcept {
    const int subpixel_dx = RoundFixedQ8(sprite.subpixel_dx_q8);
    const int extra_width = metrics_.width - kVanillaWidth;

    switch (sprite.anchor) {
        case HudAnchor::LeftEdge:
            return static_cast<int>(sprite.vanilla_x);
        case HudAnchor::RightEdge:
            return static_cast<int>(sprite.vanilla_x) + extra_width;
        case HudAnchor::Center:
            return static_cast<int>(sprite.vanilla_x) + metrics_.left_margin;
        case HudAnchor::WorldSpace:
        default:
            return static_cast<int>(sprite.vanilla_x) + metrics_.left_margin + subpixel_dx;
    }
}

int WidescreenPpu::RemapSpriteY(const WidescreenOamSprite& sprite) const noexcept {
    if (sprite.anchor != HudAnchor::WorldSpace) {
        return static_cast<int>(sprite.vanilla_y);
    }
    return static_cast<int>(sprite.vanilla_y) + RoundFixedQ8(sprite.subpixel_dy_q8);
}

WindowHorizontalBounds WidescreenPpu::RemapWindowHorizontal(
    uint8_t vanilla_x1,
    uint8_t vanilla_x2,
    HudAnchor anchor) const noexcept {
    const int x1 = static_cast<int>(vanilla_x1);
    const int x2 = (vanilla_x2 == 0 && vanilla_x1 > 0) ? kVanillaWidth : static_cast<int>(vanilla_x2);

    // Full-width vanilla window (0..255 or 0..256) expands to cover the full widescreen frame
    if (x1 == 0 && x2 >= 255) {
        return {0u, static_cast<uint16_t>(metrics_.width)};
    }

    const int extra_width = metrics_.width - kVanillaWidth;
    int mapped_x1 = x1;
    int mapped_x2 = x2;

    switch (anchor) {
        case HudAnchor::LeftEdge:
            break;
        case HudAnchor::RightEdge:
            mapped_x1 += extra_width;
            mapped_x2 += extra_width;
            break;
        case HudAnchor::Center:
        case HudAnchor::WorldSpace:
        default:
            mapped_x1 += metrics_.left_margin;
            mapped_x2 += metrics_.left_margin;
            break;
    }

    mapped_x1 = std::clamp(mapped_x1, 0, metrics_.width);
    mapped_x2 = std::clamp(mapped_x2, 0, metrics_.width);
    return {
        static_cast<uint16_t>(mapped_x1),
        static_cast<uint16_t>(mapped_x2),
    };
}

int WidescreenPpu::SampleBgSourceX(const BgLayerConfig& layer, int screen_x) const noexcept {
    if (!layer.enabled || screen_x < 0 || screen_x >= metrics_.width) {
        return -1;
    }

    const int vw = (layer.virtual_width > 0) ? layer.virtual_width : 512;

    switch (layer.role) {
        case BgLayerRole::WorldTilemap: {
            // Shift by -left_margin so vanilla screen center (128) aligns with widescreen center (width/2),
            // and wrap seamlessly across the 512px virtual tilemap ring buffer.
            const int sub_x = RoundFixedQ8(layer.subpixel_hofs_q8);
            const int world_u = layer.hofs + sub_x + (screen_x - metrics_.left_margin);
            return WrapCoord(world_u, vw);
        }

        case BgLayerRole::DialogueWindow: {
            // Centered 256px dialogue / message log layer: do not repeat into the widescreen margins!
            const int local_x = screen_x - metrics_.left_margin;
            if (local_x < 0 || local_x >= kVanillaWidth) {
                return -1;
            }
            return WrapCoord(layer.hofs + local_x, kVanillaWidth);
        }

        case BgLayerRole::HudStatusOverlay: {
            // Split top HUD layer: left 112px anchored to true left edge, right 112px anchored to true right edge,
            // middle 32px anchored to center if active.
            if (screen_x < 112) {
                return WrapCoord(layer.hofs + screen_x, kVanillaWidth);
            }
            const int right_start = metrics_.width - 112;
            if (screen_x >= right_start) {
                const int vanilla_x = kVanillaWidth - 112 + (screen_x - right_start);
                return WrapCoord(layer.hofs + vanilla_x, kVanillaWidth);
            }
            const int center_local = screen_x - metrics_.left_margin;
            if (center_local >= 112 && center_local < 144) {
                return WrapCoord(layer.hofs + center_local, kVanillaWidth);
            }
            return -1;
        }
    }
    return -1;
}

void WidescreenPpu::RasterizeBgScanline(
    const BgLayerConfig& layer,
    std::span<const uint32_t> virtual_bg_argb,
    int scanline_y,
    std::span<uint32_t> out_scanline_argb) const noexcept {
    if (!layer.enabled || scanline_y < 0 || scanline_y >= metrics_.height) {
        return;
    }
    if (static_cast<int>(out_scanline_argb.size()) < metrics_.width) {
        return;
    }

    const int vw = (layer.virtual_width > 0) ? layer.virtual_width : 512;
    const int vh = (layer.virtual_height > 0) ? layer.virtual_height : 256;
    if (virtual_bg_argb.size() < static_cast<size_t>(vw) * static_cast<size_t>(vh)) {
        return;
    }

    const int sub_y = (layer.role == BgLayerRole::WorldTilemap) ? RoundFixedQ8(layer.subpixel_vofs_q8) : 0;
    const int src_y = WrapCoord(layer.vofs + sub_y + scanline_y, vh);
    const size_t row_offset = static_cast<size_t>(src_y) * static_cast<size_t>(vw);

    for (int screen_x = 0; screen_x < metrics_.width; ++screen_x) {
        const int src_x = SampleBgSourceX(layer, screen_x);
        if (src_x < 0 || src_x >= vw) {
            continue;
        }
        const uint32_t src_px = virtual_bg_argb[row_offset + static_cast<size_t>(src_x)];
        if ((src_px >> 24) != 0u) {
            out_scanline_argb[static_cast<size_t>(screen_x)] =
                AlphaBlendArgb(out_scanline_argb[static_cast<size_t>(screen_x)], src_px);
        }
    }
}

void WidescreenPpu::CompositeOamSprites(
    std::span<const WidescreenOamSprite> sprites,
    std::span<uint32_t> framebuffer_argb) const noexcept {
    const int fb_w = metrics_.width;
    const int fb_h = metrics_.height;
    if (framebuffer_argb.size() < static_cast<size_t>(fb_w) * static_cast<size_t>(fb_h)) {
        return;
    }

    // Render from lowest priority (3) to highest priority (0)
    for (int prio = 3; prio >= 0; --prio) {
        for (const auto& sp : sprites) {
            if (static_cast<int>(sp.priority) != prio || sp.argb_pixels == nullptr) {
                continue;
            }
            const int dst_x0 = RemapSpriteX(sp);
            const int dst_y0 = RemapSpriteY(sp);

            for (int sy = 0; sy < static_cast<int>(sp.height); ++sy) {
                const int dy = dst_y0 + sy;
                if (dy < 0 || dy >= fb_h) {
                    continue;
                }
                const size_t dst_row = static_cast<size_t>(dy) * static_cast<size_t>(fb_w);
                const size_t src_row = static_cast<size_t>(sy) * static_cast<size_t>(sp.width);
                for (int sx = 0; sx < static_cast<int>(sp.width); ++sx) {
                    const int dx = dst_x0 + sx;
                    if (dx < 0 || dx >= fb_w) {
                        continue;
                    }
                    const uint32_t px = sp.argb_pixels[src_row + static_cast<size_t>(sx)];
                    if ((px >> 24) != 0u) {
                        framebuffer_argb[dst_row + static_cast<size_t>(dx)] =
                            AlphaBlendArgb(framebuffer_argb[dst_row + static_cast<size_t>(dx)], px);
                    }
                }
            }
        }
    }
}

void WidescreenPpu::RenderModernSingleScreenHud(
    const ModernHudState& state,
    std::span<uint32_t> framebuffer_argb) const noexcept {
    if (hud_mode_ != HudLayoutMode::SingleScreenModern) {
        return;
    }
    const int fb_w = metrics_.width;
    const int fb_h = metrics_.height;
    if (framebuffer_argb.size() < static_cast<size_t>(fb_w) * static_cast<size_t>(fb_h)) {
        return;
    }

    // 1. Top-left translucent status pill (Floor badge + HP bar + Belly bar) anchored at x = 6, y = 6
    FillRectArgb(framebuffer_argb, fb_w, fb_h, 6, 6, 132, 22, 0xB0101824u);

    // HP bar track and fill
    FillRectArgb(framebuffer_argb, fb_w, fb_h, 10, 9, 88, 6, 0xDF28303Cu);
    const int max_hp = std::max<int>(1, state.leader_max_hp);
    const int hp_clamped = std::clamp<int>(state.leader_hp, 0, max_hp);
    const int hp_fill_w = (hp_clamped * 88) / max_hp;
    const uint32_t hp_color = (hp_clamped * 4 <= max_hp) ? 0xFFE84848u : 0xFF44D868u;
    FillRectArgb(framebuffer_argb, fb_w, fb_h, 10, 9, hp_fill_w, 6, hp_color);

    // Belly bar track and fill
    FillRectArgb(framebuffer_argb, fb_w, fb_h, 10, 18, 88, 5, 0xDF28303Cu);
    const int max_belly = std::max<int>(1, state.max_belly);
    const int belly_clamped = std::clamp<int>(state.belly, 0, max_belly);
    const int belly_fill_w = (belly_clamped * 88) / max_belly;
    FillRectArgb(framebuffer_argb, fb_w, fb_h, 10, 18, belly_fill_w, 5, 0xFFF4B43Au);

    // 2. Top-right translucent minimap overlay anchored to true right widescreen edge
    if (state.minimap_enabled) {
        constexpr int kMapW = 56;
        constexpr int kMapH = 32;
        const int map_x0 = fb_w - kMapW - 8;
        const int map_y0 = 6;
        FillRectArgb(framebuffer_argb, fb_w, fb_h, map_x0 - 2, map_y0 - 2, kMapW + 4, kMapH + 4, 0x900C121Cu);

        for (int my = 0; my < kMapH; ++my) {
            for (int mx = 0; mx < kMapW; ++mx) {
                const uint8_t cell = state.minimap_cells[static_cast<size_t>(my * kMapW + mx)];
                if (cell == 0u) {
                    continue;
                }
                uint32_t cell_color = 0xB870A8D8u; // 1 = revealed floor
                if (cell == 2u) {
                    cell_color = 0xB83868C0u; // water / secondary terrain
                } else if (cell == 3u) {
                    cell_color = 0xFFFFF040u; // leader yellow dot
                } else if (cell == 4u) {
                    cell_color = 0xFF50E8FFu; // stairs cyan dot
                } else if (cell == 5u) {
                    cell_color = 0xFFFF4848u; // enemy red dot
                }
                const int px = map_x0 + mx;
                const int py = map_y0 + my;
                if (px >= 0 && px < fb_w && py >= 0 && py < fb_h) {
                    const size_t idx = static_cast<size_t>(py) * static_cast<size_t>(fb_w) + static_cast<size_t>(px);
                    framebuffer_argb[idx] = AlphaBlendArgb(framebuffer_argb[idx], cell_color);
                }
            }
        }
    }

    // 3. Optional bottom-center message log backdrop when open
    if (state.message_log_open) {
        const int box_w = 240;
        const int box_h = 44;
        const int box_x = (fb_w - box_w) / 2;
        const int box_y = fb_h - box_h - 6;
        FillRectArgb(framebuffer_argb, fb_w, fb_h, box_x, box_y, box_w, box_h, 0xCC101828u);
    }
}

bool WidescreenPpu::ExpandRender3dElementForWidescreen(
    Render3dElement64Quad& elem,
    bool is_building_dungeon_darkness) const noexcept {
    const int margin = metrics_.left_margin;
    if (margin <= 0) {
        return false;
    }

    // Case 1: Full-screen 3D rectangle (RENDER64_RECTANGLE = 2) at (0, 0) with size (256, 192)
    // Ported from Asphaltian/pmd-space FullScreenRectangles.cs (GPL-3.0).
    if (elem.type == 2u &&
        elem.vx[0] == 0 && elem.vy[0] == 0 &&
        elem.vx[1] == kVanillaWidth && elem.vy[1] == kVanillaHeight) {
        elem.vx[0] = static_cast<int16_t>(-margin);
        elem.vx[1] = static_cast<int16_t>(kVanillaWidth + 2 * margin);
        return true;
    }

    // Case 2: Dungeon darkness horizontal 4x4 band (RENDER64_TILING = 5, tex_w == 4, tex_h == 4)
    // Ported from Asphaltian/pmd-space DungeonDarkness.cs (GPL-3.0).
    if (is_building_dungeon_darkness &&
        elem.type == 5u &&
        elem.tex_w == 4 && elem.tex_h == 4) {
        bool modified = false;
        for (int i = 0; i < 4; ++i) {
            if (elem.vx[static_cast<size_t>(i)] == 0) {
                elem.vx[static_cast<size_t>(i)] = static_cast<int16_t>(-margin);
                modified = true;
            } else if (elem.vx[static_cast<size_t>(i)] == kVanillaWidth) {
                elem.vx[static_cast<size_t>(i)] = static_cast<int16_t>(kVanillaWidth + margin);
                modified = true;
            }
        }
        return modified;
    }

    return false;
}

std::vector<Render3dElement64Quad> WidescreenPpu::BuildDungeonDarknessMarginQuads(
    const RoomPixelBounds& room_bounds,
    int16_t camera_pixel_x,
    int16_t camera_pixel_y) const {
    std::vector<Render3dElement64Quad> pieces;
    const int margin = metrics_.left_margin;
    if (margin <= 0) {
        return pieces;
    }

    // Ported from Asphaltian/pmd-space DungeonDarkness.AddMarginPieces (GPL-3.0).
    constexpr int kCornerSize = 0x20;
    constexpr int kCornerReachLeft = -0x20;
    constexpr int kCornerReachRight = 0x120;
    constexpr int kCornerReachBottom = 0xE0;
    constexpr int16_t kBandTexSize = 4;
    constexpr int16_t kCornerTexSize = 0x30;

    const int left = static_cast<int>(room_bounds.left) - static_cast<int>(camera_pixel_x);
    const int top = static_cast<int>(room_bounds.top) - static_cast<int>(camera_pixel_y);
    const int right = static_cast<int>(room_bounds.right) - static_cast<int>(camera_pixel_x);
    const int bottom = static_cast<int>(room_bounds.bottom) - static_cast<int>(camera_pixel_y);

    const int lit_top = std::max(top, 0);
    const int lit_height = std::min(bottom - lit_top, kVanillaHeight);

    auto make_quad = [](int x0, int y0, int x1, int y1, int16_t tex_size, int16_t u, int16_t v) {
        Render3dElement64Quad q{};
        q.type = 5u; // RENDER64_TILING
        q.tex_w = tex_size;
        q.tex_h = tex_size;
        q.tex_u = u;
        q.tex_v = v;
        q.vx = {
            static_cast<int16_t>(x0),
            static_cast<int16_t>(x1),
            static_cast<int16_t>(x0),
            static_cast<int16_t>(x1),
        };
        q.vy = {
            static_cast<int16_t>(y0),
            static_cast<int16_t>(y0),
            static_cast<int16_t>(y1),
            static_cast<int16_t>(y1),
        };
        return q;
    };

    if (left <= 0 && left > -margin && lit_height > 0) {
        pieces.push_back(make_quad(-margin, lit_top, left, lit_top + lit_height, kBandTexSize, 0, 0));
    }
    if (right >= kVanillaWidth && right < kVanillaWidth + margin && lit_height > 0) {
        pieces.push_back(make_quad(right, lit_top, kVanillaWidth + margin, lit_top + lit_height, kBandTexSize, 0, 0));
    }

    struct CornerSpec {
        int x;
        int y;
        int16_t u;
        int16_t v;
    };
    const std::array<CornerSpec, 4> corners{{
        {left, top, 0, 0},
        {right - kCornerSize, top, kCornerTexSize, 0},
        {left, bottom - kCornerSize, 0, kCornerTexSize},
        {right - kCornerSize, bottom - kCornerSize, kCornerTexSize, kCornerTexSize},
    }};

    for (const auto& c : corners) {
        const bool in_margin =
            (c.x <= kCornerReachLeft && c.x > kCornerReachLeft - margin) ||
            (c.x >= kCornerReachRight && c.x < kCornerReachRight + margin);
        if (in_margin && c.y > kCornerReachLeft && c.y < kCornerReachBottom) {
            pieces.push_back(make_quad(c.x, c.y, c.x + kCornerSize, c.y + kCornerSize, kCornerTexSize, c.u, c.v));
        }
    }

    return pieces;
}

} // namespace skyrecomp::gpu2d
