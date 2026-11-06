#include "skyrecomp/pmdsky_addrs.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace skyrecomp::pmdsky;

    // Verify compile-time evaluation (constexpr)
    static_assert(NA._start == 0x02000800);
    static_assert(NA.NitroMain == 0x02000C6C);
    static_assert(NA.RunDungeon == 0x022DEF38);
    static_assert(NA.GetWeatherColorTable == 0x022DE620);
    static_assert(NA.DUNGEON_PTR_MASTER == 0x0235353C);
    static_assert(NA.GROUND_STATE_PTRS == 0x02324CF4);
    static_assert(NA.GAME_STATE_VALUES == 0x020AF6B8);
    static_assert(NA.WONDER_MAIL_BITS_MAP == 0x0209E0E8);
    static_assert(NA.WONDER_MAIL_BITS_SWAP == 0x0209E108);

    // Verify EU addresses
    static_assert(EU._start == 0x02000800);
    static_assert(EU.NitroMain == 0x02000C6C);
    static_assert(EU.RunDungeon == 0x022DF878);
    static_assert(EU.GetWeatherColorTable == 0x022DEF60);
    static_assert(EU.DUNGEON_PTR_MASTER == 0x0235413C);
    static_assert(EU.GROUND_STATE_PTRS == 0x02325834);
    static_assert(EU.GAME_STATE_VALUES == 0x020AFF70);
    static_assert(EU.WONDER_MAIL_BITS_MAP == 0x0209E66C);
    static_assert(EU.WONDER_MAIL_BITS_SWAP == 0x0209E68C);

    // Verify helper selector
    const auto& na_ref = Get(Region::NA);
    assert(na_ref.RunDungeon == 0x022DEF38);

    const auto& eu_ref = Get(Region::EU);
    assert(eu_ref.RunDungeon == 0x022DF878);

    std::cout << "[pmdsky_bindings_test] ALL C++20 ASSERTIONS PASSED!\n";
    return 0;
}
