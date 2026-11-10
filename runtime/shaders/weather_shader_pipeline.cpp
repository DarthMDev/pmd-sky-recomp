#include "weather_shader_pipeline.hpp"

#include <algorithm>
#include <cmath>

namespace skyrecomp::shaders {
namespace {

struct WeatherAmbientProfile {
    std::array<float, 4> tint;      // RGB scale + A atmospheric haze blend
    std::array<float, 4> bloom_rgb; // RGB bloom highlight color
    float bloom_intensity;
    float bloom_threshold;
};

[[nodiscard]] constexpr WeatherAmbientProfile GetWeatherProfile(DungeonWeatherId weather) noexcept {
    switch (weather) {
        case DungeonWeatherId::Sunny:
            return {{1.08f, 1.02f, 0.90f, 0.04f}, {1.00f, 0.85f, 0.55f, 1.0f}, 0.34f, 0.54f};
        case DungeonWeatherId::Sandstorm:
            return {{1.06f, 0.94f, 0.76f, 0.12f}, {0.92f, 0.74f, 0.45f, 1.0f}, 0.22f, 0.62f};
        case DungeonWeatherId::Cloudy:
            return {{0.90f, 0.92f, 0.96f, 0.06f}, {0.80f, 0.86f, 0.95f, 1.0f}, 0.16f, 0.68f};
        case DungeonWeatherId::Rainy:
            return {{0.84f, 0.92f, 1.06f, 0.08f}, {0.55f, 0.78f, 1.00f, 1.0f}, 0.26f, 0.58f};
        case DungeonWeatherId::Hail:
            return {{0.88f, 0.98f, 1.08f, 0.07f}, {0.65f, 0.92f, 1.00f, 1.0f}, 0.30f, 0.56f};
        case DungeonWeatherId::Fog:
            return {{0.94f, 0.94f, 1.02f, 0.18f}, {0.86f, 0.84f, 0.98f, 1.0f}, 0.36f, 0.50f};
        case DungeonWeatherId::Snow:
            return {{0.95f, 0.99f, 1.06f, 0.06f}, {0.88f, 0.95f, 1.00f, 1.0f}, 0.32f, 0.55f};
        case DungeonWeatherId::Clear:
        case DungeonWeatherId::Random:
        default:
            return {{1.00f, 1.00f, 1.00f, 0.00f}, {1.00f, 0.98f, 0.92f, 1.0f}, 0.18f, 0.66f};
    }
}

[[nodiscard]] uint8_t ClampByte(float v) noexcept {
    const int iv = static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    return static_cast<uint8_t>(std::clamp(iv, 0, 255));
}

} // namespace

WeatherShaderPipeline::WeatherShaderPipeline(
    pmdsky::Region region,
    ShaderFilterPreset filter,
    bool weather_bloom_enabled) noexcept
    : region_(region),
      filter_(filter),
      weather_bloom_enabled_(weather_bloom_enabled) {}

void WeatherShaderPipeline::SetSubpixelGridStrength(float strength) noexcept {
    subpixel_grid_strength_ = std::clamp(strength, 0.0f, 1.0f);
}

GuestWeatherState WeatherShaderPipeline::ReadWeatherStateFromGuest(GuestMemoryView mem) const noexcept {
    GuestWeatherState state{};
    const uint32_t dungeon_ptr = mem.resolve_dungeon_ptr(region_);
    if (dungeon_ptr == 0u) {
        return state;
    }

    state.in_dungeon = true;
    state.nullify_weather =
        mem.read_or<uint8_t>(dungeon_ptr + layout::dungeon::kNullifyWeatherOffset, 0u) != 0u;

    const uint8_t raw_weather =
        mem.read_or<uint8_t>(dungeon_ptr + layout::dungeon::kWeatherOffset, 0u);
    if (state.nullify_weather || raw_weather > static_cast<uint8_t>(DungeonWeatherId::Random)) {
        state.weather = DungeonWeatherId::Clear;
    } else {
        state.weather = static_cast<DungeonWeatherId>(raw_weather);
    }

    // Read 256-entry color_table populated by GetWeatherColorTable (at dungeon + 0x01E0)
    const uint32_t lut_base = dungeon_ptr + layout::dungeon::kColorTableOffset;
    bool non_zero_seen = false;
    for (int i = 0; i < layout::dungeon::kColorTableEntries; ++i) {
        const uint32_t entry = mem.read_or<uint32_t>(
            lut_base + static_cast<uint32_t>(i * sizeof(uint32_t)),
            0u);
        state.color_table[static_cast<size_t>(i)] = entry;
        if (i > 0 && (entry & 0x00FFFFFFu) != 0u) {
            non_zero_seen = true;
        }
    }
    state.has_custom_color_table = non_zero_seen;
    return state;
}

SkyHdShaderUniforms WeatherShaderPipeline::BuildUniforms(
    int source_width,
    int source_height,
    int output_width,
    int output_height,
    const GuestWeatherState& weather_state) const noexcept {
    const float sw = static_cast<float>(std::max(1, source_width));
    const float sh = static_cast<float>(std::max(1, source_height));
    const float ow = static_cast<float>(std::max(1, output_width));
    const float oh = static_cast<float>(std::max(1, output_height));

    SkyHdShaderUniforms u{};
    u.source_width = sw;
    u.source_height = sh;
    u.inv_source_width = 1.0f / sw;
    u.inv_source_height = 1.0f / sh;
    u.output_width = ow;
    u.output_height = oh;
    u.scale_factor = std::max(1.0f, std::min(ow / sw, oh / sh));
    u.filter_mode = static_cast<uint32_t>(filter_);
    u.weather_id = static_cast<uint32_t>(weather_state.weather);
    u.bloom_enabled = weather_bloom_enabled_ ? 1u : 0u;
    u.subpixel_grid_strength = subpixel_grid_strength_;

    const WeatherAmbientProfile profile = GetWeatherProfile(
        weather_state.in_dungeon ? weather_state.weather : DungeonWeatherId::Clear);
    u.weather_tint = profile.tint;
    u.weather_bloom_rgb = profile.bloom_rgb;
    u.bloom_intensity = weather_bloom_enabled_ ? profile.bloom_intensity : 0.0f;
    u.bloom_threshold = profile.bloom_threshold;

    // Incorporate guest `dungeon->color_table` (GetWeatherColorTable) mid-tone shift if populated
    if (weather_state.in_dungeon && weather_state.has_custom_color_table) {
        const uint32_t mid = weather_state.color_table[128];
        const float lut_r = static_cast<float>(mid & 0xFFu) / 128.0f;
        const float lut_g = static_cast<float>((mid >> 8) & 0xFFu) / 128.0f;
        const float lut_b = static_cast<float>((mid >> 16) & 0xFFu) / 128.0f;
        if (lut_r > 0.1f && lut_g > 0.1f && lut_b > 0.1f) {
            u.weather_tint[0] = std::clamp(0.5f * u.weather_tint[0] + 0.5f * lut_r, 0.5f, 1.5f);
            u.weather_tint[1] = std::clamp(0.5f * u.weather_tint[1] + 0.5f * lut_g, 0.5f, 1.5f);
            u.weather_tint[2] = std::clamp(0.5f * u.weather_tint[2] + 0.5f * lut_b, 0.5f, 1.5f);
        }
    }

    return u;
}

uint32_t WeatherShaderPipeline::SampleSharpBilinearArgb(
    std::span<const uint32_t> src_argb,
    int src_width,
    int src_height,
    float u,
    float v,
    float scale_factor,
    ShaderFilterPreset preset) noexcept {
    if (src_width <= 0 || src_height <= 0 ||
        src_argb.size() < static_cast<size_t>(src_width) * static_cast<size_t>(src_height)) {
        return 0xFF000000u;
    }

    const float clamped_u = std::clamp(u, 0.0f, 0.999999f);
    const float clamped_v = std::clamp(v, 0.0f, 0.999999f);

    if (preset == ShaderFilterPreset::Nearest) {
        const int ix = std::clamp(static_cast<int>(clamped_u * static_cast<float>(src_width)), 0, src_width - 1);
        const int iy = std::clamp(static_cast<int>(clamped_v * static_cast<float>(src_height)), 0, src_height - 1);
        return src_argb[static_cast<size_t>(iy) * static_cast<size_t>(src_width) + static_cast<size_t>(ix)];
    }

    const float eff_scale = std::max(1.0f, scale_factor);
    const float region_range = 0.5f - 0.5f / eff_scale;

    auto warp_coord = [eff_scale, region_range](float norm_c, int dim) -> float {
        const float texel = norm_c * static_cast<float>(dim) - 0.5f;
        const float fl = std::floor(texel);
        const float frac = texel - fl;
        const float dist = frac - 0.5f;
        const float clamped_dist = std::clamp(dist, -region_range, region_range);
        const float f = std::clamp((dist - clamped_dist) * eff_scale + 0.5f, 0.0f, 1.0f);
        return fl + f;
    };

    const float wx = warp_coord(clamped_u, src_width);
    const float wy = warp_coord(clamped_v, src_height);

    const int x0 = std::clamp(static_cast<int>(std::floor(wx)), 0, src_width - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(wy)), 0, src_height - 1);
    const int x1 = std::clamp(x0 + 1, 0, src_width - 1);
    const int y1 = std::clamp(y0 + 1, 0, src_height - 1);
    const float fx = std::clamp(wx - std::floor(wx), 0.0f, 1.0f);
    const float fy = std::clamp(wy - std::floor(wy), 0.0f, 1.0f);

    const uint32_t c00 = src_argb[static_cast<size_t>(y0) * static_cast<size_t>(src_width) + static_cast<size_t>(x0)];
    const uint32_t c10 = src_argb[static_cast<size_t>(y0) * static_cast<size_t>(src_width) + static_cast<size_t>(x1)];
    const uint32_t c01 = src_argb[static_cast<size_t>(y1) * static_cast<size_t>(src_width) + static_cast<size_t>(x0)];
    const uint32_t c11 = src_argb[static_cast<size_t>(y1) * static_cast<size_t>(src_width) + static_cast<size_t>(x1)];

    auto bilerp_chan = [fx, fy](uint32_t p00, uint32_t p10, uint32_t p01, uint32_t p11, int shift) -> uint32_t {
        const float v00 = static_cast<float>((p00 >> shift) & 0xFFu);
        const float v10 = static_cast<float>((p10 >> shift) & 0xFFu);
        const float v01 = static_cast<float>((p01 >> shift) & 0xFFu);
        const float v11 = static_cast<float>((p11 >> shift) & 0xFFu);
        const float top = v00 + (v10 - v00) * fx;
        const float bot = v01 + (v11 - v01) * fx;
        const int res = static_cast<int>(std::lround(top + (bot - top) * fy));
        return static_cast<uint32_t>(std::clamp(res, 0, 255));
    };

    const uint32_t r = bilerp_chan(c00, c10, c01, c11, 16);
    const uint32_t g = bilerp_chan(c00, c10, c01, c11, 8);
    const uint32_t b = bilerp_chan(c00, c10, c01, c11, 0);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void WeatherShaderPipeline::ApplyPostProcessArgb8888(
    std::span<uint32_t> framebuffer_argb,
    int width,
    int height,
    const GuestWeatherState& weather_state) const noexcept {
    if (width <= 0 || height <= 0 ||
        framebuffer_argb.size() < static_cast<size_t>(width) * static_cast<size_t>(height)) {
        return;
    }

    const SkyHdShaderUniforms u = BuildUniforms(width, height, width, height, weather_state);

    for (int y = 0; y < height; ++y) {
        const bool crt_darken_row = (filter_ == ShaderFilterPreset::CrtScanline) && ((y & 1) == 1);
        for (int x = 0; x < width; ++x) {
            const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
            const uint32_t px = framebuffer_argb[idx];

            uint8_t r8 = static_cast<uint8_t>((px >> 16) & 0xFFu);
            uint8_t g8 = static_cast<uint8_t>((px >> 8) & 0xFFu);
            uint8_t b8 = static_cast<uint8_t>(px & 0xFFu);

            // 1. Optional direct LUT lookup from `dungeon->color_table` if populated by GetWeatherColorTable
            if (weather_state.in_dungeon && weather_state.has_custom_color_table) {
                const uint32_t lut_r = weather_state.color_table[r8];
                const uint32_t lut_g = weather_state.color_table[g8];
                const uint32_t lut_b = weather_state.color_table[b8];
                r8 = static_cast<uint8_t>(lut_r & 0xFFu);
                g8 = static_cast<uint8_t>((lut_g >> 8) & 0xFFu);
                b8 = static_cast<uint8_t>((lut_b >> 16) & 0xFFu);
            }

            float rf = (static_cast<float>(r8) / 255.0f) * u.weather_tint[0];
            float gf = (static_cast<float>(g8) / 255.0f) * u.weather_tint[1];
            float bf = (static_cast<float>(b8) / 255.0f) * u.weather_tint[2];

            // Atmospheric haze blend
            const float haze = u.weather_tint[3];
            rf = rf * (1.0f - haze) + u.weather_bloom_rgb[0] * haze;
            gf = gf * (1.0f - haze) + u.weather_bloom_rgb[1] * haze;
            bf = bf * (1.0f - haze) + u.weather_bloom_rgb[2] * haze;

            // 2. Per-weather ambient bloom on bright highlights
            if (u.bloom_enabled != 0u && u.bloom_intensity > 0.001f) {
                const float luma = 0.299f * rf + 0.587f * gf + 0.114f * bf;
                if (luma > u.bloom_threshold) {
                    const float highlight =
                        (luma - u.bloom_threshold) / std::max(0.001f, 1.0f - u.bloom_threshold);
                    const float boost = highlight * u.bloom_intensity;
                    rf += u.weather_bloom_rgb[0] * boost;
                    gf += u.weather_bloom_rgb[1] * boost;
                    bf += u.weather_bloom_rgb[2] * boost;
                }
            }

            // 3. Optional LCD / CRT sub-pixel mask
            if (filter_ == ShaderFilterPreset::NdsLcdSubpixelGrid && ((x % 3) == 0 || (y % 3) == 0)) {
                const float atten = 1.0f - 0.18f * u.subpixel_grid_strength;
                rf *= atten;
                gf *= atten;
                bf *= atten;
            } else if (crt_darken_row) {
                const float atten = 1.0f - 0.22f * u.subpixel_grid_strength;
                rf *= atten;
                gf *= atten;
                bf *= atten;
            }

            const uint32_t out_r = ClampByte(rf);
            const uint32_t out_g = ClampByte(gf);
            const uint32_t out_b = ClampByte(bf);
            framebuffer_argb[idx] = (px & 0xFF000000u) | (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}

} // namespace skyrecomp::shaders
