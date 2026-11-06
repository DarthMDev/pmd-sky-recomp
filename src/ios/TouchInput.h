#pragma once

#include <cstdint>

class SkyTouchInput {
public:
    void set(uint16_t buttons, uint16_t pressed) {
        pending_ |= pressed & ~held_;
        held_ = (held_ & ~buttons) | pressed;
    }

    uint16_t current() const { return held_ | pending_ | framePresses_; }

    uint16_t beginFrame() {
        // A press and release between guest frames still needs a frame down.
        framePresses_ = pending_;
        pending_ = 0;
        return current();
    }

    void clear() { held_ = pending_ = framePresses_ = 0; }

private:
    uint16_t held_ = 0;
    uint16_t pending_ = 0;
    uint16_t framePresses_ = 0;
};
