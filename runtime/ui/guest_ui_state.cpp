#include "guest_ui_state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace skyrecomp::ui {
namespace {

constexpr uint32_t kTeamMemberTableNA = 0x022ABDE0u;
constexpr uint32_t kTeamMemberTableEU = 0x022AC720u;

constexpr uint32_t kTeamMemberTableRosterOffset = 0x936Cu;
constexpr uint32_t kTeamMemberTableActiveTeamOffset = 0x9877u;
constexpr uint32_t kTeamMemberSize = 104u;
constexpr uint32_t kTeamMemberSlots = 4u;
constexpr uint32_t kTeamMemberTeamStride = kTeamMemberSize * kTeamMemberSlots;

constexpr uint32_t kTeamMemberIsValid = 0x0u;
constexpr uint32_t kTeamMemberIsLeader = 0x1u;
constexpr uint32_t kTeamMemberLevel = 0x2u;
constexpr uint32_t kTeamMemberCurrentHp = 0x0Eu;
constexpr uint32_t kTeamMemberMaxHp = 0x10u;
constexpr uint32_t kTeamMemberBelly = 0x44u;
constexpr uint32_t kTeamMemberMaxBelly = 0x48u;
constexpr uint32_t kTeamMemberName = 0x5Eu;
constexpr uint32_t kTeamMemberNameLength = 10u;

constexpr uint32_t kMonsterIsNotTeamMember = 0x06u;
constexpr uint32_t kMonsterIsLeader = 0x07u;
constexpr uint32_t kMonsterLevel = 0x0Au;
constexpr uint32_t kMonsterTeamIndex = 0x0Cu;
constexpr uint32_t kMonsterHp = 0x10u;
constexpr uint32_t kMonsterMaxHpStat = 0x12u;
constexpr uint32_t kMonsterMaxHpBoost = 0x16u;
constexpr uint32_t kMonsterBelly = 0x146u;
constexpr uint32_t kMonsterMaxBelly = 0x14Au;
constexpr uint32_t kMonsterSize = 0x240u;

constexpr uint32_t kEntityMonsterType = 1u;

constexpr uint32_t kWindowListNA = 0x022A88DCu;
constexpr uint32_t kWindowListEU = 0x022A921Cu;
constexpr uint32_t kWindowSize = 224u;
constexpr uint32_t kWindowCount = 20u;
constexpr uint32_t kWindowScreenOffset = 0x08u;
constexpr uint32_t kWindowValidOffset = 0xB6u;

uint32_t TeamMemberTableAddress(pmdsky::Region region) noexcept {
    return region == pmdsky::Region::EU ? kTeamMemberTableEU
                                        : kTeamMemberTableNA;
}

void CopyName(GuestMemoryView mem, uint32_t address,
              std::array<char, 11>& out) noexcept {
    const uint8_t* bytes = mem.raw_ptr(address, kTeamMemberNameLength);
    if (bytes == nullptr) return;
    size_t length = 0;
    while (length < kTeamMemberNameLength && bytes[length] != 0u) {
        out[length] = static_cast<char>(bytes[length]);
        ++length;
    }
    out[length] = '\0';
}

} // namespace

GuestPartyState ReadGuestParty(GuestMemoryView mem,
                               pmdsky::Region region) noexcept {
    GuestPartyState out{};
    if (!mem.valid()) return out;

    const uint32_t table = TeamMemberTableAddress(region);
    const int active_team = std::clamp<int>(
        mem.read_or<uint8_t>(table + kTeamMemberTableActiveTeamOffset, 0u),
        0, 2);
    const uint32_t roster_base = table + kTeamMemberTableRosterOffset +
                                 static_cast<uint32_t>(active_team) *
                                     kTeamMemberTeamStride;

    for (uint32_t slot = 0; slot < kTeamMemberSlots; ++slot) {
        const uint32_t member = roster_base + slot * kTeamMemberSize;
        const uint8_t flags = mem.read_or<uint8_t>(
            member + kTeamMemberIsValid, 0u);
        if ((flags & 1u) == 0u) continue;

        RemasterPartyMember& party = out.party[slot];
        party.active = true;
        party.leader = mem.read_or<uint8_t>(
                           member + kTeamMemberIsLeader, 0u) != 0u;
        party.level = mem.read_or<uint8_t>(member + kTeamMemberLevel, 1u);
        party.hp = mem.read_or<int16_t>(member + kTeamMemberCurrentHp, 0);
        party.max_hp = std::max<int>(
            1, mem.read_or<int16_t>(member + kTeamMemberMaxHp, 1));
        party.belly = mem.read_or<int16_t>(member + kTeamMemberBelly, 0);
        party.max_belly = std::max<int>(
            1, mem.read_or<int16_t>(member + kTeamMemberMaxBelly, 1));
        CopyName(mem, member + kTeamMemberName, party.name);
        ++out.count;
    }

    // The persistent roster is refreshed at floor start; live HP/belly for the
    // current floor comes from the active dungeon monsters. Match them by
    // monster::team_index so names from the roster survive.
    const uint32_t dungeon = mem.resolve_dungeon_ptr(region);
    if (dungeon != 0u) {
        const uint32_t active_monsters =
            dungeon + layout::dungeon::kEntityTableOffset +
            layout::entity_table::kActiveMonsterPtrsOffset;
        for (uint32_t i = 0; i < 20u; ++i) {
            const uint32_t entity =
                mem.read_or<uint32_t>(active_monsters + i * 4u, 0u);
            if (!mem.in_range(entity, layout::entity::kSize)) continue;
            const int32_t type = mem.read_or<int32_t>(
                entity + layout::entity::kTypeOffset, 0);
            if (type != static_cast<int32_t>(kEntityMonsterType)) continue;
            const uint32_t monster = mem.read_or<uint32_t>(
                entity + layout::entity::kInfoPtrOffset, 0u);
            if (!mem.in_range(monster, kMonsterSize)) continue;
            if (mem.read_or<uint8_t>(monster + kMonsterIsNotTeamMember, 0u) !=
                0u) {
                continue;
            }
            const int team_index = mem.read_or<int16_t>(
                monster + kMonsterTeamIndex, -1);
            if (team_index < 0 || team_index >= 4) continue;

            RemasterPartyMember& party =
                out.party[static_cast<size_t>(team_index)];
            if (!party.active) {
                party.active = true;
                ++out.count;
            }
            party.leader = mem.read_or<uint8_t>(
                               monster + kMonsterIsLeader, 0u) != 0u;
            party.level = mem.read_or<uint8_t>(monster + kMonsterLevel,
                                               party.level);
            party.hp = mem.read_or<int16_t>(monster + kMonsterHp, party.hp);
            const int max_stat = mem.read_or<int16_t>(
                monster + kMonsterMaxHpStat, party.max_hp);
            const int max_boost = mem.read_or<int16_t>(
                monster + kMonsterMaxHpBoost, 0);
            party.max_hp = std::max(1, max_stat + max_boost);
            party.belly = mem.read_or<int16_t>(monster + kMonsterBelly,
                                               party.belly);
            party.max_belly = std::max<int>(
                1, mem.read_or<int16_t>(monster + kMonsterMaxBelly,
                                        party.max_belly));
        }
    }

    return out;
}

GuestUiFlags ReadGuestUiFlags(GuestMemoryView mem,
                               pmdsky::Region region) noexcept {
    GuestUiFlags flags{};
    if (!mem.valid()) return flags;

    const uint32_t window_list =
        region == pmdsky::Region::EU ? kWindowListEU : kWindowListNA;
    for (uint32_t i = 0; i < kWindowCount; ++i) {
        const uint32_t window = window_list + i * kWindowSize;
        const int8_t valid =
            mem.read_or<int8_t>(window + kWindowValidOffset, 0);
        if (valid == 0) continue;
        const uint8_t screen =
            mem.read_or<uint8_t>(window + kWindowScreenOffset, 0);
        // enum screen: SCREEN_MAIN = 0 (bottom engine A),
        //              SCREEN_SUB  = 1 (top engine B).
        if (screen == 1u) {
            flags.top_screen_menu_open = true;
            break;
        }
    }

    const uint32_t dungeon = mem.resolve_dungeon_ptr(region);
    if (dungeon != 0u) {
        flags.team_menu_or_grid = mem.read_or<uint8_t>(
            dungeon + layout::dungeon::kDisplayDataOffset +
                layout::display_data::kTeamMenuOrGridOffset,
            0u) != 0u;
    }
    return flags;
}

uint8_t ReadGuestWeatherId(GuestMemoryView mem, pmdsky::Region region,
                           bool* nullified) noexcept {
    if (nullified != nullptr) *nullified = false;
    if (!mem.valid()) return 0;
    const uint32_t dungeon = mem.resolve_dungeon_ptr(region);
    if (dungeon == 0u) return 0;
    const bool is_nullified = mem.read_or<uint8_t>(
                                  dungeon + layout::dungeon::kNullifyWeatherOffset,
                                  0u) != 0u;
    if (nullified != nullptr) *nullified = is_nullified;
    if (is_nullified) return 0;
    return mem.read_or<uint8_t>(
        dungeon + layout::dungeon::kWeatherOffset, 0u);
}

} // namespace skyrecomp::ui
