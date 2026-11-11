#include "remaster_hud.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace skyrecomp::ui {
namespace {

constexpr uint32_t kPanelFill = 0xD0101824u;
constexpr uint32_t kPanelFillAlt = 0xD0182434u;
constexpr uint32_t kPanelBorder = 0xFFD8A848u;
constexpr uint32_t kPanelBorderSoft = 0x90785028u;
constexpr uint32_t kTextMain = 0xFFF4F0E8u;
constexpr uint32_t kTextMuted = 0xFFB8C0D0u;
constexpr uint32_t kHpGreen = 0xFF44D868u;
constexpr uint32_t kHpLow = 0xFFE84848u;
constexpr uint32_t kBellyAmber = 0xFFF4B43Au;
constexpr uint32_t kBarTrack = 0xE028303Cu;

constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;

uint32_t Blend(uint32_t dst, uint32_t src) noexcept {
    const uint32_t a = (src >> 24) & 0xFFu;
    if (a == 0u) return dst;
    if (a == 0xFFu) return src;
    const uint32_t inv = 255u - a;
    const uint32_t dr = (dst >> 16) & 0xFFu;
    const uint32_t dg = (dst >> 8) & 0xFFu;
    const uint32_t db = dst & 0xFFu;
    const uint32_t sr = (src >> 16) & 0xFFu;
    const uint32_t sg = (src >> 8) & 0xFFu;
    const uint32_t sb = src & 0xFFu;
    const uint32_t r = (sr * a + dr * inv) / 255u;
    const uint32_t g = (sg * a + dg * inv) / 255u;
    const uint32_t b = (sb * a + db * inv) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void PutPixel(std::span<uint32_t> fb, int width, int height, int x, int y,
              uint32_t color) noexcept {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) +
                         static_cast<size_t>(x);
    if (index >= fb.size()) return;
    fb[index] = Blend(fb[index], color);
}

void FillRect(std::span<uint32_t> fb, int width, int height, int x, int y,
              int w, int h, uint32_t color) noexcept {
    if (w <= 0 || h <= 0) return;
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(width, x + w);
    const int y1 = std::min(height, y + h);
    for (int py = y0; py < y1; ++py) {
        const size_t row = static_cast<size_t>(py) * static_cast<size_t>(width);
        for (int px = x0; px < x1; ++px) {
            const size_t index = row + static_cast<size_t>(px);
            if (index < fb.size()) fb[index] = Blend(fb[index], color);
        }
    }
}

void DrawPanel(std::span<uint32_t> fb, int width, int height, int x, int y,
               int w, int h, bool alternate = false) noexcept {
    FillRect(fb, width, height, x, y, w, h,
             alternate ? kPanelFillAlt : kPanelFill);
    // A 1px frame with the four corner pixels omitted reads as a softly
    // rounded panel without needing a stencil or restoring the background.
    const uint32_t border = kPanelBorder;
    FillRect(fb, width, height, x + 1, y, w - 2, 1, border);
    FillRect(fb, width, height, x + 1, y + h - 1, w - 2, 1, border);
    FillRect(fb, width, height, x, y + 1, 1, h - 2, border);
    FillRect(fb, width, height, x + w - 1, y + 1, 1, h - 2, border);
    // Inner highlight keeps the panels from looking flat.
    FillRect(fb, width, height, x + 2, y + 1, w - 4, 1, kPanelBorderSoft);
}

const char* Glyph(char ch) noexcept {
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))) {
        case 'A': return "01110100011000111111100011000110001";
        case 'B': return "11110100011000111110100011000111110";
        case 'C': return "01111100001000010000100001000001111";
        case 'D': return "11110100011000110001100011000111110";
        case 'E': return "11111100001000011110100001000011111";
        case 'F': return "11111100001000011110100001000010000";
        case 'G': return "01111100001000010111100011000101111";
        case 'H': return "10001100011000111111100011000110001";
        case 'I': return "11111001000010000100001000010011111";
        case 'J': return "00111000100001000010100101001001100";
        case 'K': return "10001100101010011000101001001010001";
        case 'L': return "10000100001000010000100001000011111";
        case 'M': return "10001110111010110101100011000110001";
        case 'N': return "10001110011010110011100011000110001";
        case 'O': return "01110100011000110001100011000101110";
        case 'P': return "11110100011000111110100001000010000";
        case 'Q': return "01110100011000110001101011001001101";
        case 'R': return "11110100011000111110101001001010001";
        case 'S': return "01111100001000001110000010000111110";
        case 'T': return "11111001000010000100001000010000100";
        case 'U': return "10001100011000110001100011000101110";
        case 'V': return "10001100011000110001100010101000100";
        case 'W': return "10001100011000110101101011101110001";
        case 'X': return "10001100010101000100010101000110001";
        case 'Y': return "10001100010101000100001000010000100";
        case 'Z': return "11111000010001000100010001000011111";
        case '0': return "01110100011001110101110011000101110";
        case '1': return "00100011000010000100001000010001110";
        case '2': return "01110100010000100010001000100011111";
        case '3': return "11110000010000101110000010000111110";
        case '4': return "00010001100101010010111110001000010";
        case '5': return "11111100001000011110000010000111110";
        case '6': return "01110100001000011110100011000101110";
        case '7': return "11111000010001000100010001000010000";
        case '8': return "01110100011000101110100011000101110";
        case '9': return "01110100011000101111000010000101110";
        case ':': return "00000001000010000000001000010000000";
        case '.': return "00000000000000000000000000010000100";
        case '-': return "00000000000000011111000000000000000";
        case '/': return "00001000100001000100010001000010000";
        case '(': return "00010001000100001000010000010000010";
        case ')': return "01000001000001000010000100010001000";
        case '_': return "00000000000000000000000000000011111";
        case ' ': return "00000000000000000000000000000000000";
        default:  return "01110000010001000100001000000000100";
    }
}

void DrawText(std::span<uint32_t> fb, int width, int height, int x, int y,
              std::string_view text, uint32_t color, int scale = 1) noexcept {
    if (scale <= 0) return;
    int cursor = x;
    for (const char ch : text) {
        const char* glyph = Glyph(ch);
        for (int gy = 0; gy < kGlyphH; ++gy) {
            for (int gx = 0; gx < kGlyphW; ++gx) {
                if (glyph[gy * kGlyphW + gx] != '1') continue;
                FillRect(fb, width, height, cursor + gx * scale,
                         y + gy * scale, scale, scale, color);
            }
        }
        cursor += (kGlyphW + 1) * scale;
    }
}

int TextWidth(std::string_view text, int scale = 1) noexcept {
    return static_cast<int>(text.size()) * (kGlyphW + 1) * scale;
}

void DrawBar(std::span<uint32_t> fb, int width, int height, int x, int y,
             int w, int h, int value, int maximum, uint32_t fill,
             uint32_t track) noexcept {
    if (w <= 0 || h <= 0) return;
    FillRect(fb, width, height, x, y, w, h, track);
    const int clamped_max = std::max(1, maximum);
    const int clamped_value = std::clamp(value, 0, clamped_max);
    const int fill_w = (clamped_value * w) / clamped_max;
    if (fill_w > 0) FillRect(fb, width, height, x, y, fill_w, h, fill);
}

std::string_view WeatherName(uint8_t id, bool nullified) noexcept {
    if (nullified) return "CLEAR";
    switch (id) {
        case 1: return "SUNNY";
        case 2: return "SAND";
        case 3: return "CLOUD";
        case 4: return "RAIN";
        case 5: return "HAIL";
        case 6: return "FOG";
        case 7: return "SNOW";
        case 8: return "RANDOM";
        default: return "CLEAR";
    }
}

std::string_view TrimName(const std::array<char, 11>& name) noexcept {
    size_t length = 0;
    while (length < name.size() && name[length] != '\0') ++length;
    return std::string_view(name.data(), length);
}

} // namespace

void RemasterHud::RenderArgb8888(std::span<uint32_t> framebuffer, int width,
                                 int height) const noexcept {
    if (width <= 0 || height <= 0 ||
        framebuffer.size() < static_cast<size_t>(width) *
                                 static_cast<size_t>(height)) {
        return;
    }

    // The DS top screen is 192px high. Keep the HUD at native pixel scale and
    // anchor it to the widescreen edges; taller future buffers scale cleanly.
    const int scale = std::max(1, height / 192);
    const int margin = 4 * scale;
    const int panel_w = std::min(116 * scale, std::max(64 * scale,
                                                       width / 2 - margin));
    const int panel_h = 28 * scale;
    const int row_gap = 4 * scale;

    int y = margin;
    for (int i = 0; i < 2; ++i) {
        const RemasterPartyMember& member =
            state_.party[static_cast<size_t>(i)];
        if (!member.active) {
            y += panel_h + row_gap;
            continue;
        }
        DrawPanel(framebuffer, width, height, margin, y, panel_w, panel_h,
                  i == 1);
        const int text_x = margin + 5 * scale;
        const int name_y = y + 4 * scale;
        std::string_view name = TrimName(member.name);
        if (name.empty()) name = member.leader ? "LEADER" : "PARTNER";
        if (name.size() > 9) name = name.substr(0, 9);
        DrawText(framebuffer, width, height, text_x, name_y, name,
                 kTextMain, scale);

        char hp_text[16]{};
        const int hp_len = std::snprintf(
            hp_text, sizeof(hp_text), "%d/%d", std::max(0, member.hp),
            std::max(1, member.max_hp));
        const int hp_x = margin + panel_w - 5 * scale -
                         TextWidth(std::string_view(hp_text,
                                                    static_cast<size_t>(
                                                        std::max(0, hp_len))),
                                   scale);
        DrawText(framebuffer, width, height, hp_x, name_y,
                 std::string_view(hp_text,
                                  static_cast<size_t>(std::max(0, hp_len))),
                 kTextMuted, scale);

        const int bar_x = margin + 5 * scale;
        const int bar_w = panel_w - 10 * scale;
        const uint32_t hp_color =
            (member.hp * 4 <= member.max_hp) ? kHpLow : kHpGreen;
        DrawBar(framebuffer, width, height, bar_x, y + 14 * scale, bar_w,
                5 * scale, member.hp, member.max_hp, hp_color, kBarTrack);
        DrawBar(framebuffer, width, height, bar_x, y + 21 * scale, bar_w,
                4 * scale, member.belly, member.max_belly, kBellyAmber,
                kBarTrack);
        y += panel_h + row_gap;
    }

    // Floor/weather readout, centred between the party stack and minimap.
    const int info_w = 100 * scale;
    const int info_h = 18 * scale;
    const int info_x = std::max(margin + panel_w + margin,
                                (width - info_w) / 2);
    const int info_y = margin;
    DrawPanel(framebuffer, width, height, info_x, info_y, info_w, info_h);
    char floor_text[12]{};
    const int floor_len = std::snprintf(floor_text, sizeof(floor_text), "F%d",
                                        std::max(0, state_.floor_number));
    DrawText(framebuffer, width, height, info_x + 5 * scale,
             info_y + 5 * scale,
             std::string_view(floor_text,
                              static_cast<size_t>(std::max(0, floor_len))),
             kTextMain, scale);
    DrawText(framebuffer, width, height, info_x + 32 * scale,
             info_y + 5 * scale,
             WeatherName(state_.weather_id, state_.weather_nullified),
             kTextMuted, scale);

    // Minimap, anchored to the true widescreen right edge.
    if (state_.minimap_enabled) {
        constexpr int kMapW = 56;
        constexpr int kMapH = 32;
        const int map_w = kMapW * scale;
        const int map_h = kMapH * scale;
        const int map_x = width - map_w - 6 * scale;
        const int map_y = margin;
        DrawPanel(framebuffer, width, height, map_x - 2 * scale,
                  map_y - 2 * scale, map_w + 4 * scale, map_h + 4 * scale);
        for (int my = 0; my < kMapH; ++my) {
            for (int mx = 0; mx < kMapW; ++mx) {
                const uint8_t cell =
                    state_.minimap_cells[static_cast<size_t>(my * kMapW + mx)];
                if (cell == 0u) continue;
                uint32_t color = 0xB870A8D8u;
                if (cell == 2u) color = 0xB83868C0u;
                else if (cell == 3u) color = 0xFFFFF040u;
                else if (cell == 4u) color = 0xFF50E8FFu;
                else if (cell == 5u) color = 0xFFFF4848u;
                FillRect(framebuffer, width, height, map_x + mx * scale,
                         map_y + my * scale, scale, scale, color);
            }
        }
    }
}

} // namespace skyrecomp::ui
