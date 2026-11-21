#include "shaders/metal_hd_presenter.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>

using namespace skyrecomp::shaders;

namespace {

bool CheckPixel(SDL_Surface* surface, int x, int y, uint32_t expected) {
    Uint8 r{}, g{}, b{}, a{};
    if (!SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a)) return false;
    const uint32_t pixel = (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    if (pixel == expected) return true;
    std::fprintf(stderr, "pixel (%d, %d): got %08x, expected %08x\n", x, y, pixel, expected);
    return false;
}

int RunTest(SDL_Renderer* renderer) {
    auto presenter = MetalHdPresenter::Create(renderer);
    if (!presenter) return 1;

    const std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> target(
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 24, 16),
        SDL_DestroyTexture);
    if (!target || !SDL_SetRenderTarget(renderer, target.get()) ||
        !SDL_SetRenderLogicalPresentation(renderer, 12, 8, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE) ||
        !SDL_SetRenderDrawColor(renderer, 11, 22, 33, 255) || !SDL_RenderClear(renderer))
        return 1;

    constexpr uint32_t pattern[]{0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u};
    if (!presenter->Draw({1, 1, 4, 4}, pattern, 2, 2, ShaderFilterPreset::Nearest, nullptr))
        return 1;

    GuestWeatherState weather{};
    weather.in_dungeon = true;
    weather.weather = DungeonWeatherId::Sunny;
    weather.has_custom_color_table = true;
    for (uint32_t i = 0; i < weather.color_table.size(); ++i)
        weather.color_table[i] = std::min(255u, i + 12u) | (i << 8) | ((i > 12 ? i - 12 : 0) << 16);
    constexpr uint32_t gray = 0xFF808080u;
    if (!presenter->Draw({7, 1, 3, 4}, &gray, 1, 1, ShaderFilterPreset::SharpBilinear, &weather))
        return 1;

    // Each output pixel integrates one red and one blue texel when downscaling.
    constexpr uint32_t stripes[]{0xFFFF0000u, 0xFF0000FFu, 0xFFFF0000u, 0xFF0000FFu};
    if (!presenter->Draw({7, 6, 1, 1}, stripes, 4, 1, ShaderFilterPreset::Pixellate, nullptr))
        return 1;

    const SDL_FRect overlay{1, 1, 1, 1};
    if (!SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255) || !SDL_RenderFillRect(renderer, &overlay))
        return 1;
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
        SDL_RenderReadPixels(renderer, nullptr), SDL_DestroySurface);
    if (!surface) return 1;

    if (!CheckPixel(surface.get(), 1, 1, 0xFF0B1621u) ||
        !CheckPixel(surface.get(), 2, 2, 0xFFFFFFFFu) ||
        !CheckPixel(surface.get(), 4, 4, pattern[0]) ||
        !CheckPixel(surface.get(), 7, 4, pattern[1]) ||
        !CheckPixel(surface.get(), 4, 7, pattern[2]) ||
        !CheckPixel(surface.get(), 7, 7, pattern[3]) ||
        !CheckPixel(surface.get(), 15, 3, 0xFF9B866Au) ||
        !CheckPixel(surface.get(), 14, 12, 0xFF800080u) ||
        !CheckPixel(surface.get(), 15, 12, 0xFF800080u)) return 1;

    // Both masks are visible within an eight-by-eight output region for one source texel.
    if (!presenter->Draw({0, 0, 4, 4}, &gray, 1, 1, ShaderFilterPreset::NdsLcdSubpixelGrid, nullptr) ||
        !presenter->Draw({5, 0, 4, 4}, &gray, 1, 1, ShaderFilterPreset::CrtScanline, nullptr))
        return 1;
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> masks(
        SDL_RenderReadPixels(renderer, nullptr), SDL_DestroySurface);
    return masks && CheckPixel(masks.get(), 0, 1, 0xFF737373u) &&
           CheckPixel(masks.get(), 1, 1, gray) &&
           CheckPixel(masks.get(), 10, 0, gray) &&
           CheckPixel(masks.get(), 10, 3, 0xFF747474u) ? 0 : 1;
}

} // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
    int status = 77;
    SDL_Window* window = SDL_CreateWindow("Metal shader test", 24, 16, SDL_WINDOW_HIDDEN);
    if (window) {
        if (SDL_Renderer* renderer = SDL_CreateRenderer(window, "metal")) {
            status = RunTest(renderer);
            SDL_DestroyRenderer(renderer);
        }
        SDL_DestroyWindow(window);
    }
    if (status != 0) std::fprintf(stderr, "%s\n", SDL_GetError());
    SDL_Quit();
    return status;
}
