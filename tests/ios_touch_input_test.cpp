#include "TouchInput.h"

int main() {
    constexpr uint16_t a = 1u;
    constexpr uint16_t up = 1u << 6;
    constexpr uint16_t right = 1u << 4;
    constexpr uint16_t dpad = up | right;
    SkyTouchInput touch;

    touch.set(dpad, dpad);
    touch.set(a, a);
    touch.set(a, 0);
    if (touch.take() != (a | dpad)) return 1;
    if (touch.take() != dpad) return 2;

    touch.set(dpad, right);
    if (touch.take() != right) return 3;
    touch.set(dpad, 0);
    if (touch.take() != 0) return 4;

    touch.set(a, a);
    touch.clear();
    if (touch.take() != 0) return 5;
}
