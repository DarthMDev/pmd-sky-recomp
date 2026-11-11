#pragma once

#include "remaster_hud.hpp"
#include "skyrecomp/guest_mem.hpp"
#include "skyrecomp/pmdsky_addrs.hpp"

#include <array>
#include <cstdint>

namespace skyrecomp::ui {

/// Party snapshot read from the live dungeon entity table, falling back to
/// the persistent team roster for names and members that are not spawned.
struct GuestPartyState {
    std::array<RemasterPartyMember, 4> party{};
    int count = 0;
};

/// Reads the active party for the remaster HUD.
[[nodiscard]] GuestPartyState ReadGuestParty(GuestMemoryView mem,
                                             pmdsky::Region region) noexcept;

/// Reads the current dungeon weather ID. Returns 0 (Clear) outside dungeon or
/// when weather is nullified.
[[nodiscard]] uint8_t ReadGuestWeatherId(GuestMemoryView mem,
                                         pmdsky::Region region,
                                         bool* nullified = nullptr) noexcept;

/// Menu/window flags used to decide whether the corner dungeon inset is safe.
struct GuestUiFlags {
    bool top_screen_menu_open = false;
    bool team_menu_or_grid = false;
};

/// Reads active top-screen menu windows and the dungeon team-menu/touch-grid flag.
[[nodiscard]] GuestUiFlags ReadGuestUiFlags(GuestMemoryView mem,
                                            pmdsky::Region region) noexcept;

} // namespace skyrecomp::ui
