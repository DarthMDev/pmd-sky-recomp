#pragma once

#include "weather_shader_pipeline.hpp"

#include <memory>

struct SDL_Rect;
struct SDL_Renderer;

namespace skyrecomp::shaders {

class MetalHdPresenter {
public:
    static std::unique_ptr<MetalHdPresenter> Create(SDL_Renderer* renderer);
    ~MetalHdPresenter();

    // Draws one LCD in SDL's logical coordinates, using the current Metal render pass.
    bool Draw(const SDL_Rect& destination, const uint32_t* pixels,
              int source_width, int source_height, ShaderFilterPreset filter,
              const GuestWeatherState* weather);

private:
    struct State;
    MetalHdPresenter();
    std::unique_ptr<State> state_;
};

} // namespace skyrecomp::shaders
