#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace skyrecomp::ui {

/// A single active party member as shown by the remaster HUD.
struct RemasterPartyMember {
    bool active = false;
    bool leader = false;
    uint8_t level = 1;
    int hp = 0;
    int max_hp = 1;
    int belly = 0;
    int max_belly = 1;
    std::array<char, 11> name{};
};

/// Read-only snapshot the remaster HUD needs for one presented frame.
struct RemasterHudState {
    std::array<RemasterPartyMember, 4> party{};
    int party_count = 0;
    int floor_number = 1;
    uint8_t weather_id = 0;
    bool weather_nullified = false;
    bool minimap_enabled = true;
    std::array<uint8_t, 56 * 32> minimap_cells{};
};

/// Layout-first gameplay HUD for the single-screen (PMD DX-style) presentation.
///
/// This deliberately keeps the original PMD visual language: dark blue
/// translucent panels, gold frames, green HP, amber belly, and a compact
/// corner minimap. Art can be replaced later without changing the layout.
class RemasterHud {
public:
    /// Replaces the state used for the next render.
    void SetState(const RemasterHudState& state) noexcept { state_ = state; }
    /// Returns the last state passed to SetState.
    [[nodiscard]] const RemasterHudState& GetState() const noexcept { return state_; }

    /// Renders the HUD over an ARGB8888 top-screen framebuffer.
    void RenderArgb8888(std::span<uint32_t> framebuffer, int width, int height) const noexcept;
    /// Draws a compact FPS badge anchored to the top-right of a guest frame.
    static void RenderFpsMeter(std::span<uint32_t> framebuffer, int width, int height,
                               double fps) noexcept;

private:
    RemasterHudState state_{};
};

} // namespace skyrecomp::ui
