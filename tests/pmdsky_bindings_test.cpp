#include "skyrecomp/pmdsky_addrs.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace skyrecomp::pmdsky;

    // Verify compile-time evaluation (constexpr)
    static_assert(NA._start == 0x02000800);
    static_assert(NA.NitroMain == 0x02000C6C);
    static_assert(NA.ExplorersOfSkyMain == 0x022DC240);
    static_assert(NA.AddSimpleObjToOam == 0x0201B9F8);
    static_assert(NA.InitRender3dElement64 == 0x0201E730);
    static_assert(NA.NewWindowScreenCheck == 0x02027648);
    static_assert(NA.WINDOW_LIST == 0x022A88DC);
    static_assert(NA.WAN_TABLE == 0x020AFC68);
    static_assert(NA.OBJ_GRAPHICS_CONTROLS_PTR == 0x020AFC4C);
    static_assert(NA.RunDungeon == 0x022DEF38);
    static_assert(NA.GetWeatherColorTable == 0x022DE620);
    static_assert(NA.DisplayMonstersAndItems == 0x022E1854);
    static_assert(NA.DisplayItem == 0x023457C8);
    static_assert(NA.DisplayThrownItemShadow == 0x022E9488);
    static_assert(NA.UpdateEntityPixelPos == 0x022E1A40);
    static_assert(NA.SetEntityPixelPosXY == 0x022E1A84);
    static_assert(NA.IncrementEntityPixelPosXY == 0x022E1A90);
    static_assert(NA.ShouldDisplayEntity == 0x022E2574);
    static_assert(NA.IsPositionVisibleAndOnScreen == 0x022E2CA0);
    static_assert(NA.PointCameraToMonster == 0x022E2E54);
    static_assert(NA.UpdateCamera == 0x022E2EC4);
    static_assert(NA.GetVisibilityRange == 0x022E333C);
    static_assert(NA.DUNGEON_PTR_MASTER == 0x0235353C);
    static_assert(NA.LEADER_PTR == 0x0235355C);
    static_assert(NA.MESSAGE_LOG_INFO == 0x023537CC);
    static_assert(NA.DUNGEON_FADES_PTR == 0x023537E0);
    static_assert(NA.GROUND_STATE_PTRS == 0x02324CF4);
    static_assert(NA.GROUND_STATE_MAP == 0x02324CC0);
    static_assert(NA.GROUND_WEATHER_TABLE == 0x0231DE10);
    static_assert(NA.PERSONALITY_TEST_PTR == 0x0238CEA0);
    static_assert(NA.GAME_STATE_VALUES == 0x020AF6B8);
    static_assert(NA.WONDER_MAIL_BITS_MAP == 0x0209E0E8);
    static_assert(NA.WONDER_MAIL_BITS_SWAP == 0x0209E108);

    // Verify EU addresses
    static_assert(EU._start == 0x02000800);
    static_assert(EU.NitroMain == 0x02000C6C);
    static_assert(EU.ExplorersOfSkyMain == 0x022DCB80);
    static_assert(EU.AddSimpleObjToOam == 0x0201BA94);
    static_assert(EU.InitRender3dElement64 == 0x0201E7CC);
    static_assert(EU.NewWindowScreenCheck == 0x0202793C);
    static_assert(EU.WINDOW_LIST == 0x022A921C);
    static_assert(EU.WAN_TABLE == 0x020B0524);
    static_assert(EU.OBJ_GRAPHICS_CONTROLS_PTR == 0x020B0508);
    static_assert(EU.RunDungeon == 0x022DF878);
    static_assert(EU.GetWeatherColorTable == 0x022DEF60);
    static_assert(EU.DisplayMonstersAndItems == 0x022E2194);
    static_assert(EU.DisplayItem == 0x023463B4);
    static_assert(EU.DisplayThrownItemShadow == 0x022E9E38);
    static_assert(EU.UpdateEntityPixelPos == 0x022E2380);
    static_assert(EU.SetEntityPixelPosXY == 0x022E23C4);
    static_assert(EU.IncrementEntityPixelPosXY == 0x022E23D0);
    static_assert(EU.ShouldDisplayEntity == 0x022E2EB4);
    static_assert(EU.IsPositionVisibleAndOnScreen == 0x022E3624);
    static_assert(EU.PointCameraToMonster == 0x022E3804);
    static_assert(EU.UpdateCamera == 0x022E3874);
    static_assert(EU.GetVisibilityRange == 0x022E3CEC);
    static_assert(EU.DUNGEON_PTR_MASTER == 0x0235413C);
    static_assert(EU.LEADER_PTR == 0x0235415C);
    static_assert(EU.MESSAGE_LOG_INFO == 0x023543E4);
    static_assert(EU.DUNGEON_FADES_PTR == 0x023543F8);
    static_assert(EU.GROUND_STATE_PTRS == 0x02325834);
    static_assert(EU.GROUND_STATE_MAP == 0x02325800);
    static_assert(EU.GROUND_WEATHER_TABLE == 0x0231E7F0);
    static_assert(EU.PERSONALITY_TEST_PTR == 0x0238D9E0);
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
