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

    constexpr uint16_t down = 1u << 7;
    constexpr uint16_t directions = dpad | down;
    touch.set(directions, up);
    if (touch.beginFrame() != up) return 8;
    touch.set(directions, right);
    touch.set(directions, down);
    touch.set(directions, 0);
    if (touch.current() != down || touch.beginFrame() != down) return 9;
    if (touch.beginFrame() != 0) return 10;
}
