#include "metal_hd_presenter.hpp"

#include <SDL3/SDL.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace skyrecomp::shaders {

struct MetalHdPresenter::State {
    SDL_Renderer* renderer = nullptr;
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    MTLPixelFormat output_format = MTLPixelFormatInvalid;
};

MetalHdPresenter::MetalHdPresenter() : state_(std::make_unique<State>()) {}
MetalHdPresenter::~MetalHdPresenter() = default;

std::unique_ptr<MetalHdPresenter> MetalHdPresenter::Create(SDL_Renderer* renderer) {
    @autoreleasepool {
        auto* layer = (__bridge CAMetalLayer*)SDL_GetRenderMetalLayer(renderer);
        if (!layer || !layer.device) return nullptr;

        NSError* error = nil;
        auto device = layer.device;
        auto library = [device newLibraryWithSource:@(kSkyHdMetalShaderSource)
                                           options:nil error:&error];
        if (!library) {
            std::fprintf(stderr, "[sky] Metal shader compilation failed: %s\n",
                         error.localizedDescription.UTF8String);
            return nullptr;
        }

        auto* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"sky_hd_vertex_main"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"sky_hd_fragment_main"];
        descriptor.colorAttachments[0].pixelFormat = layer.pixelFormat;
        auto pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) {
            std::fprintf(stderr, "[sky] Metal shader pipeline creation failed: %s\n",
                         error.localizedDescription.UTF8String);
            return nullptr;
        }

        auto presenter = std::unique_ptr<MetalHdPresenter>(new MetalHdPresenter);
        presenter->state_->renderer = renderer;
        presenter->state_->device = device;
        presenter->state_->pipeline = pipeline;
        presenter->state_->output_format = layer.pixelFormat;
        return presenter;
    }
}

bool MetalHdPresenter::Draw(const SDL_Rect& destination, const uint32_t* pixels,
                           int source_width, int source_height, ShaderFilterPreset filter,
                           const GuestWeatherState* weather) {
    @autoreleasepool {
        auto& state = *state_;
        auto* layer = (__bridge CAMetalLayer*)SDL_GetRenderMetalLayer(state.renderer);
        if (!layer || layer.pixelFormat != state.output_format || !pixels ||
            source_width <= 0 || source_height <= 0 ||
            destination.w <= 0 || destination.h <= 0) return false;

        SDL_FRect logical_rect{};
        int logical_width = 0, logical_height = 0;
        int output_width = 0, output_height = 0;
        if (!SDL_GetRenderLogicalPresentation(state.renderer, &logical_width, &logical_height, nullptr) ||
            !SDL_GetRenderLogicalPresentationRect(state.renderer, &logical_rect) ||
            !SDL_GetCurrentRenderOutputSize(state.renderer, &output_width, &output_height) ||
            logical_width <= 0 || logical_height <= 0 || logical_rect.w <= 0 || logical_rect.h <= 0)
            return false;

        const double scale_x = logical_rect.w / logical_width;
        const double scale_y = logical_rect.h / logical_height;
        const MTLViewport viewport{
            logical_rect.x + destination.x * scale_x, logical_rect.y + destination.y * scale_y,
            destination.w * scale_x, destination.h * scale_y, 0.0, 1.0,
        };
        // Integer-scale layouts can extend beyond a small window. Clip to the drawable.
        const int left = std::max(0, static_cast<int>(std::floor(logical_rect.x)));
        const int top = std::max(0, static_cast<int>(std::floor(logical_rect.y)));
        const int right = std::min(output_width, static_cast<int>(std::ceil(logical_rect.x + logical_rect.w)));
        const int bottom = std::min(output_height, static_cast<int>(std::ceil(logical_rect.y + logical_rect.h)));
        if (left >= right || top >= bottom) return false;

        // One upload per draw keeps the texture immutable while SDL's command buffer uses it.
        // Keep the default storage mode for CPU-writable textures on each GPU.
        auto* texture_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                              width:source_width
                                                                             height:source_height mipmapped:NO];
        texture_desc.usage = MTLTextureUsageShaderRead;
        auto texture = [state.device newTextureWithDescriptor:texture_desc];
        if (!texture) return false;
        [texture replaceRegion:MTLRegionMake2D(0, 0, source_width, source_height) mipmapLevel:0
                     withBytes:pixels bytesPerRow:source_width * sizeof(uint32_t)];

        // SDL flushes its queued draws before handing over the encoder. It owns
        // the render pass and will rebind its own state for subsequent draws.
        auto encoder = (__bridge id<MTLRenderCommandEncoder>)SDL_GetRenderMetalCommandEncoder(state.renderer);
        if (!encoder) return false;

        static const GuestWeatherState neutral_weather{};
        const auto& weather_state = weather ? *weather : neutral_weather;
        const WeatherShaderPipeline shader(pmdsky::Region::NA, filter, weather != nullptr);
        const auto uniforms = shader.BuildUniforms(source_width, source_height,
            static_cast<int>(std::lround(viewport.width)),
            static_cast<int>(std::lround(viewport.height)), weather_state);
        const MTLScissorRect scissor{
            static_cast<NSUInteger>(left), static_cast<NSUInteger>(top),
            static_cast<NSUInteger>(right - left), static_cast<NSUInteger>(bottom - top),
        };
        [encoder setRenderPipelineState:state.pipeline];
        [encoder setViewport:viewport];
        [encoder setScissorRect:scissor];
        [encoder setFragmentTexture:texture atIndex:0];
        [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [encoder setFragmentBytes:weather_state.color_table.data()
                           length:sizeof(weather_state.color_table) atIndex:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        return true;
    }
}

} // namespace skyrecomp::shaders
