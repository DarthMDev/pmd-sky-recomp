#pragma once

namespace skyrecomp::shaders {

/// Embedded Apple Metal Shading Language (MSL) shader source for SDL_GPU / Metal backend (Task 3.3).
/// Implements:
///   1. Crisp pixel-art reconstruction (`sharp-bilinear` & `pixellate` box-filter integration)
///   2. Optional NDS RGB LCD sub-pixel grid & CRT scanline phosphor mask
///   3. Per-weather dungeon ambient bloom & LUT color grading driven by `GetWeatherColorTable` (0x022DE620 NA)
inline constexpr const char* kSkyHdMetalShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct SkyHdUniforms {
    float2 source_size;       // e.g. (342.0, 192.0)
    float2 inv_source_size;   // (1.0 / source_size.x, 1.0 / source_size.y)
    float2 output_size;       // e.g. (1368.0, 768.0)
    float  scale_factor;      // Integer/fractional upscale ratio
    uint   filter_mode;       // 0=Nearest, 1=SharpBilinear, 2=Pixellate, 3=NdsLcdGrid, 4=CrtScanline
    uint   weather_id;        // 0=Clear, 1=Sunny, 2=Sandstorm, 3=Cloudy, 4=Rainy, 5=Hail, 6=Fog, 7=Snow
    uint   bloom_enabled;     // 0 or 1
    float  bloom_intensity;   // 0.0 .. 1.0
    float  bloom_threshold;   // 0.0 .. 1.0
    float4 weather_tint;      // RGB multiplier + A fog/haze blend factor from GetWeatherColorTable
    float4 weather_bloom_rgb; // RGB ambient bloom tint for current weather
    float  subpixel_grid_strength; // 0.0 .. 1.0
};

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut sky_hd_vertex_main(uint vid [[vertex_id]]) {
    const float2 positions[4] = {
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0,  1.0)
    };
    const float2 uvs[4] = {
        float2(0.0, 1.0),
        float2(1.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 0.0)
    };
    VertexOut out;
    out.position = float4(positions[vid], 0.0, 1.0);
    out.uv = uvs[vid];
    return out;
}

// Sharp-bilinear UV pre-warping for razor-sharp pixel art at non-integer scales
static inline float2 sharp_bilinear_uv(float2 uv, float2 tex_size, float2 inv_tex_size, float scale) {
    float2 texel = uv * tex_size;
    float2 texel_floored = floor(texel);
    float2 s = fract(texel);
    float region_range = 0.5 - 0.5 / max(scale, 1.0);
    float2 center_dist = s - 0.5;
    float2 f = (center_dist - clamp(center_dist, -region_range, region_range)) * max(scale, 1.0) + 0.5;
    return (texel_floored + f) * inv_tex_size;
}

fragment float4 sky_hd_fragment_main(
    VertexOut in [[stage_in]],
    texture2d<float, access::sample> src_tex [[texture(0)]],
    constant SkyHdUniforms& u [[buffer(0)]])
{
    constexpr sampler linear_samp(coord::normalized, address::clamp_to_edge, filter::linear);
    constexpr sampler nearest_samp(coord::normalized, address::clamp_to_edge, filter::nearest);

    float3 color = float3(0.0);
    if (u.filter_mode == 0u) {
        color = src_tex.sample(nearest_samp, in.uv).rgb;
    } else if (u.filter_mode == 2u) {
        // Integrate the output pixel's footprint in source texel edge coordinates.
        float2 center = in.uv * u.source_size;
        float2 half_box = 0.5 * u.source_size / u.output_size;
        float2 lo = max(center - half_box, float2(0.0));
        float2 hi = min(center + half_box, u.source_size);
        int2 first = int2(floor(lo));
        int2 end = int2(ceil(hi));
        for (int y = first.y; y < end.y; ++y) {
            for (int x = first.x; x < end.x; ++x) {
                float2 texel = float2(x, y);
                float2 overlap = min(hi, texel + 1.0) - max(lo, texel);
                color += src_tex.read(uint2(x, y)).rgb * (overlap.x * overlap.y);
            }
        }
        float2 box_size = hi - lo;
        color /= box_size.x * box_size.y;
    } else {
        float2 warped_uv = sharp_bilinear_uv(in.uv, u.source_size, u.inv_source_size, u.scale_factor);
        color = src_tex.sample(linear_samp, warped_uv).rgb;
    }

    // Apply GetWeatherColorTable LUT grading & atmospheric haze
    color = clamp(color * u.weather_tint.rgb, 0.0, 1.0);
    color = mix(color, u.weather_bloom_rgb.rgb, u.weather_tint.a);

    // Apply per-weather dungeon ambient bloom
    if (u.bloom_enabled != 0u && u.bloom_intensity > 0.001) {
        float2 d = u.inv_source_size;
        float3 n0 = src_tex.sample(linear_samp, in.uv + float2(-d.x,  0.0)).rgb;
        float3 n1 = src_tex.sample(linear_samp, in.uv + float2( d.x,  0.0)).rgb;
        float3 n2 = src_tex.sample(linear_samp, in.uv + float2( 0.0, -d.y)).rgb;
        float3 n3 = src_tex.sample(linear_samp, in.uv + float2( 0.0,  d.y)).rgb;
        float3 avg = (color + n0 + n1 + n2 + n3) * 0.2;
        float luma = dot(avg, float3(0.299, 0.587, 0.114));
        float highlight = max(0.0, luma - u.bloom_threshold) / max(1.0 - u.bloom_threshold, 0.001);
        color = clamp(color + avg * u.weather_bloom_rgb.rgb * (highlight * u.bloom_intensity), 0.0, 1.0);
    }

    // Optional NDS LCD sub-pixel grid (mode 3) or CRT scanline mask (mode 4)
    if (u.filter_mode == 3u) {
        float2 texel_pos = fract(in.uv * u.source_size);
        float border = (texel_pos.x < 0.08 || texel_pos.y < 0.08) ? (1.0 - 0.22 * u.subpixel_grid_strength) : 1.0;
        color *= border;
    } else if (u.filter_mode == 4u) {
        float scan = 0.5 + 0.5 * cos(in.uv.y * u.source_size.y * 6.2831853);
        float mask = mix(1.0, 0.78 + 0.22 * scan, u.subpixel_grid_strength);
        color *= mask;
    }

    return float4(color, 1.0);
}
)MSL";

} // namespace skyrecomp::shaders
