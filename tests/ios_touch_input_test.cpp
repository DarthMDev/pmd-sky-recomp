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
    if (touch.current() != (a | dpad)) return 1;
    if (touch.beginFrame() != (a | dpad)) return 2;
    if (touch.current() != (a | dpad)) return 3;
    if (touch.beginFrame() != dpad) return 4;

    touch.set(dpad, right);
    if (touch.beginFrame() != right) return 5;
    touch.set(dpad, 0);
    if (touch.beginFrame() != 0) return 6;

    touch.set(a, a);
    touch.clear();
    if (touch.current() != 0 || touch.beginFrame() != 0) return 7;
}
