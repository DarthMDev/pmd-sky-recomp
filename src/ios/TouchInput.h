#pragma once

#include <cstdint>

class SkyTouchInput {
public:
    void set(uint16_t buttons, uint16_t pressed) {
        pending_ |= pressed & ~held_;
        held_ = (held_ & ~buttons) | pressed;
    }

    uint16_t take() {
        // A press and release between guest frames still needs a frame down.
        const uint16_t pressed = held_ | pending_;
        pending_ = 0;
        return pressed;
    }

    void clear() { held_ = pending_ = 0; }

private:
    uint16_t held_ = 0;
    uint16_t pending_ = 0;
};
