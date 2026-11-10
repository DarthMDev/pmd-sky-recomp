#pragma once

#include "sky_hd_shaders.metal.h"
#include "skyrecomp/guest_mem.hpp"
#include "skyrecomp/pmdsky_addrs.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace skyrecomp::shaders {

/// Pixel-art reconstruction and display filter modes (Task 3.3).
enum class ShaderFilterPreset : uint8_t {
    Nearest = 0,            // Raw nearest-neighbor point sampling
    SharpBilinear = 1,      // Pre-warped sharp-bilinear anti-aliased pixel art
    Pixellate = 2,          // Exact pixel-box coverage integration
    NdsLcdSubpixelGrid = 3, // Sharp-bilinear + authentic NDS RGB LCD cell grid
    CrtScanline = 4,        // Sharp-bilinear + subtle aperture-grille scanlines
};

/// PMD Sky `enum weather_id` values from `overlay29` (`dungeon->weather` at +0xCD38).
enum class DungeonWeatherId : uint8_t {
    Clear = 0,
    Sunny = 1,
    Sandstorm = 2,
    Cloudy = 3,
    Rainy = 4,
    Hail = 5,
    Fog = 6,
    Snow = 7,
    Random = 8,
};

/// Guest weather & LUT state extracted from `DUNGEON_PTR_MASTER` and `GetWeatherColorTable`.
struct GuestWeatherState {
    bool in_dungeon = false;
    bool nullify_weather = false;
    DungeonWeatherId weather = DungeonWeatherId::Clear;
    bool has_custom_color_table = false;
    // 256-entry RGBX8888 LUT from `dungeon->color_table` (+0x01E0) populated by GetWeatherColorTable
    std::array<uint32_t, layout::dungeon::kColorTableEntries> color_table{};
};

/// Host/GPU uniform buffer layout matching `SkyHdUniforms` in `sky_hd_shaders.metal.h`.
struct SkyHdShaderUniforms {
    float source_width = 342.0f;
    float source_height = 192.0f;
    float inv_source_width = 1.0f / 342.0f;
    float inv_source_height = 1.0f / 192.0f;
    float output_width = 1368.0f;
    float output_height = 768.0f;
    float scale_factor = 4.0f;
    uint32_t filter_mode = static_cast<uint32_t>(ShaderFilterPreset::SharpBilinear);
    uint32_t weather_id = static_cast<uint32_t>(DungeonWeatherId::Clear);
    uint32_t bloom_enabled = 1u;
    float bloom_intensity = 0.22f;
    float bloom_threshold = 0.62f;
    std::array<float, 4> weather_tint{1.0f, 1.0f, 1.0f, 0.0f};      // RGB multiplier + A haze factor
    std::array<float, 4> weather_bloom_rgb{1.0f, 1.0f, 1.0f, 1.0f}; // RGB bloom color
    float subpixel_grid_strength = 0.45f;
};

/// Custom Apple Metal & SDL_GPU HD Shader Pipeline + CPU Reference Post-Processor (Task 3.3).
class WeatherShaderPipeline {
public:
    explicit WeatherShaderPipeline(
        pmdsky::Region region = pmdsky::Region::NA,
        ShaderFilterPreset filter = ShaderFilterPreset::SharpBilinear,
        bool weather_bloom_enabled = true) noexcept;

    void SetRegion(pmdsky::Region region) noexcept { region_ = region; }
    [[nodiscard]] pmdsky::Region GetRegion() const noexcept { return region_; }

    void SetFilterPreset(ShaderFilterPreset filter) noexcept { filter_ = filter; }
    [[nodiscard]] ShaderFilterPreset GetFilterPreset() const noexcept { return filter_; }

    void SetWeatherBloomEnabled(bool enabled) noexcept { weather_bloom_enabled_ = enabled; }
    [[nodiscard]] bool IsWeatherBloomEnabled() const noexcept { return weather_bloom_enabled_; }

    void SetSubpixelGridStrength(float strength) noexcept;
    [[nodiscard]] float GetSubpixelGridStrength() const noexcept { return subpixel_grid_strength_; }

    /// Returns the embedded Apple Metal Shading Language (MSL) shader source string.
    [[nodiscard]] static constexpr std::string_view GetMetalShaderSource() noexcept {
        return kSkyHdMetalShaderSource;
    }

    /// Returns the guest function address of `GetWeatherColorTable` for the configured region
    /// (`0x022DE620` NA / `0x022DEF60` EU).
    [[nodiscard]] uint32_t GetWeatherColorTableGuestAddress() const noexcept {
        return pmdsky::Get(region_).GetWeatherColorTable;
    }

    /// Reads the active weather ID and 256-entry `color_table` from `DUNGEON_PTR_MASTER` in guest RAM.
    [[nodiscard]] GuestWeatherState ReadWeatherStateFromGuest(GuestMemoryView mem) const noexcept;

    /// Computes GPU shader uniforms for the given viewport dimensions and weather state.
    [[nodiscard]] SkyHdShaderUniforms BuildUniforms(
        int source_width,
        int source_height,
        int output_width,
        int output_height,
        const GuestWeatherState& weather_state) const noexcept;

    /// Samples a single pixel from an ARGB8888 source image using the active `sharp-bilinear` / `pixellate`
    /// reconstruction kernel at normalized coordinates `(u, v)` in [0, 1].
    [[nodiscard]] static uint32_t SampleSharpBilinearArgb(
        std::span<const uint32_t> src_argb,
        int src_width,
        int src_height,
        float u,
        float v,
        float scale_factor,
        ShaderFilterPreset preset) noexcept;

    /// Applies `GetWeatherColorTable` LUT grading, per-weather ambient bloom, and optional LCD/CRT mask
    /// in-place to an ARGB8888 framebuffer (`width * height`).
    void ApplyPostProcessArgb8888(
        std::span<uint32_t> framebuffer_argb,
        int width,
        int height,
        const GuestWeatherState& weather_state) const noexcept;

private:
    pmdsky::Region region_ = pmdsky::Region::NA;
    ShaderFilterPreset filter_ = ShaderFilterPreset::SharpBilinear;
    bool weather_bloom_enabled_ = true;
    float subpixel_grid_strength_ = 0.45f;
};

} // namespace skyrecomp::shaders
