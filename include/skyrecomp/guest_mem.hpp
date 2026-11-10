#pragma once

#include "skyrecomp/pmdsky_addrs.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>

namespace skyrecomp {

/// Exact guest memory constants and verified struct layout offsets from pmdsky-debug
/// (headers/types/dungeon_mode/dungeon.h, dungeon_mode.h, ground_mode/ground_mode.h).
namespace layout {

inline constexpr uint32_t kMainRamBase = 0x02000000u;
inline constexpr uint32_t kMainRamSize = 0x00400000u; // 4 MiB standard NDS Main RAM

/// struct dungeon (size: 0x2CB14 bytes in overlay29)
namespace dungeon {
inline constexpr uint32_t kSize = 0x2CB14u;
inline constexpr uint32_t kCameraFrameCounterOffset = 0x001Cu; // int16_t
inline constexpr uint32_t kCurrentActiveEntityOffset = 0x00C4u; // struct entity*
inline constexpr uint32_t kColorTableOffset = 0x01E0u; // struct rgba[256] (1024 bytes)
inline constexpr uint32_t kWeatherOffset = 0xCD38u; // uint8_t weather_id
inline constexpr uint32_t kNaturalWeatherOffset = 0xCD39u; // uint8_t weather_id
inline constexpr uint32_t kWeatherTurnsOffset = 0xCD3Au; // uint16_t[8]
inline constexpr uint32_t kNullifyWeatherOffset = 0xCD5Du; // bool
inline constexpr uint32_t kTilePtrsOffset = 0xD2E4u; // struct tile*[32][56]
inline constexpr uint32_t kEntityTableOffset = 0x12B28u; // struct entity_table
inline constexpr uint32_t kDisplayDataOffset = 0x1A21Cu; // struct display_data (0x48 bytes)

inline constexpr int kMaxGridY = 32;
inline constexpr int kMaxGridX = 56;
inline constexpr int kColorTableEntries = 256;
} // namespace dungeon

/// struct tile (size: 0x14 = 20 bytes)
namespace tile {
inline constexpr uint32_t kSize = 0x14u;
inline constexpr uint32_t kTerrainFlagsOffset = 0x00u; // uint16_t (bits 0..1 = terrain_type)
inline constexpr uint32_t kVisibilityFlagsOffset = 0x02u; // uint16_t (bit 0 = f_revealed, bit 1 = f_visited)
inline constexpr uint32_t kTextureIdOffset = 0x04u; // uint16_t
inline constexpr uint32_t kRoomIndexOffset = 0x07u; // uint8_t (0xFF = hallway)
inline constexpr uint32_t kWalkableNeighborFlagsOffset = 0x08u; // uint8_t[4]
inline constexpr uint32_t kMonsterPtrOffset = 0x0Cu; // struct entity*
inline constexpr uint32_t kObjectPtrOffset = 0x10u; // struct entity*
} // namespace tile

/// struct entity_table (size: 0x76F4 bytes, embedded in struct dungeon at +0x12B28)
namespace entity_table {
inline constexpr uint32_t kMonsterSlotPtrsOffset = 0x000u; // struct entity*[20]
inline constexpr uint32_t kActiveMonsterPtrsOffset = 0x050u; // struct entity*[20]
inline constexpr uint32_t kItemPtrsOffset = 0x0A0u; // struct entity*[64]
inline constexpr uint32_t kTrapPtrsOffset = 0x1A0u; // struct entity*[64]
inline constexpr uint32_t kHiddenStairsPtrOffset = 0x2A0u; // struct entity*
inline constexpr uint32_t kEntitiesArrayOffset = 0x2A4u; // struct entity[149]
inline constexpr int kMaxEntities = 149;
} // namespace entity_table

/// struct entity (size: 0xB8 = 184 bytes)
namespace entity {
inline constexpr uint32_t kSize = 0xB8u;
inline constexpr uint32_t kTypeOffset = 0x00u; // int32_t (0=NONE, 1=MONSTER, 2=TRAP, 3=ITEM, 5=HIDDEN_STAIRS)
inline constexpr uint32_t kPosOffset = 0x04u; // int16_t x, y (tile coordinates)
inline constexpr uint32_t kPrevPosOffset = 0x08u; // int16_t x, y
inline constexpr uint32_t kPixelPosOffset = 0x0Cu; // int32_t x, y (24.8 fixed-point world pixel coordinates)
inline constexpr uint32_t kPixelPosMirrorOffset = 0x14u; // int32_t x, y (24.8 fixed-point)
inline constexpr uint32_t kElevationOffset = 0x1Cu; // int32_t (24.8 fixed-point altitude)
inline constexpr uint32_t kIsVisibleOffset = 0x20u; // bool
inline constexpr uint32_t kTransparentOffset = 0x22u; // uint8_t
inline constexpr uint32_t kRoomIdxOffset = 0x25u; // uint8_t
inline constexpr uint32_t kSpawnGenIdOffset = 0x26u; // uint16_t
inline constexpr uint32_t kGraphicalParamsOffset = 0x2Cu; //animation/sprite fields
inline constexpr uint32_t kDirectionOffset = 0x44u; // uint8_t
inline constexpr uint32_t kInfoPtrOffset = 0xB4u; // void* (monster/item/trap info struct)
} // namespace entity

/// struct display_data (size: 0x48 = 72 bytes, embedded in struct dungeon at +0x1A21C)
namespace display_data {
inline constexpr uint32_t kSize = 0x48u;
inline constexpr uint32_t kCameraPosOffset = 0x00u; // int16_t x, y (tile coordinates)
inline constexpr uint32_t kCameraPosMirrorOffset = 0x04u; // int16_t x, y
inline constexpr uint32_t kCameraPixelPosOffset = 0x08u; // int16_t x, y (pixel coordinates)
inline constexpr uint32_t kCameraPixelPosMirrorOffset = 0x0Cu; // int16_t x, y
inline constexpr uint32_t kCameraTargetPtrOffset = 0x10u; // struct entity*
inline constexpr uint32_t kScreenShakeOffset = 0x14u; // uint32_t
inline constexpr uint32_t kVisibilityRangeOffset = 0x21u; // uint8_t (tiles in hallways; 0 = room-aware default)
inline constexpr uint32_t kBlindedOffset = 0x22u; // bool
inline constexpr uint32_t kLuminousOffset = 0x23u; // bool (all tiles/entities revealed)
inline constexpr uint32_t kNaturalLightingOffset = 0x24u; // bool
inline constexpr uint32_t kMapSurveyorOffset = 0x25u; // bool
inline constexpr uint32_t kCanSeeTrapsOffset = 0x26u; // bool
inline constexpr uint32_t kHallucinatingOffset = 0x27u; // bool
inline constexpr uint32_t kDarknessOffset = 0x2Du; // bool
inline constexpr uint32_t kLeaderPointedOffset = 0x2Fu; // bool
inline constexpr uint32_t kDropeyeOffset = 0x34u; // bool
inline constexpr uint32_t kTeamMenuOrGridOffset = 0x35u; // bool
inline constexpr uint32_t kFloorTouchScreenOffset = 0x3Cu; // int16_t
inline constexpr uint32_t kLeaderLevelTouchScreenOffset = 0x3Eu; // int16_t
inline constexpr uint32_t kLeaderHpTouchScreenOffset = 0x40u; // int16_t
inline constexpr uint32_t kLeaderMaxHpTouchScreenOffset = 0x42u; // int16_t
} // namespace display_data

/// Overlay 11 Ground Mode structures (headers/types/ground_mode/ground_mode.h)
namespace ground {
// struct ground_bg (size: 0x55C = 1372 bytes; pointed to by GROUND_STATE_MAP)
inline constexpr uint32_t kGroundBgSize = 0x55Cu;
inline constexpr uint32_t kGroundBgCameraPixelPosOffset = 0x200u; // struct pixel_position[2] (each 8 bytes: int32_t x, y in 24.8 fixed-point)

// struct main_ground_data (size: 0x18 = 24 bytes; pointed to by GROUND_STATE_PTRS)
inline constexpr uint32_t kMainGroundDataSize = 0x18u;
inline constexpr uint32_t kGroundActorsPtrOffset = 0x08u; // struct live_actor* (array of 24)
inline constexpr uint32_t kGroundObjectsPtrOffset = 0x0Cu; // struct live_object* (array of 16)
inline constexpr uint32_t kGroundPerformersPtrOffset = 0x10u; // struct live_performer* (array of 16)

// struct live_actor (size: 0x250 = 592 bytes, 24 entries)
inline constexpr uint32_t kLiveActorSize = 0x250u;
inline constexpr int kMaxLiveActors = 24;
inline constexpr uint32_t kLiveActorEnabledOffset = 0x04u; // bool
inline constexpr uint32_t kLiveActorIdOffset = 0x06u; // int16_t
inline constexpr uint32_t kLiveActorCoordMinOffset = 0x15Cu; // int32_t x, y (24.8 fixed-point)
inline constexpr uint32_t kLiveActorCoordMaxOffset = 0x164u; // int32_t x, y (24.8 fixed-point)
inline constexpr uint32_t kLiveActorHeightOffset = 0x16Cu; // int32_t height, height_2 (24.8 fixed-point)

// struct live_object (size: 0x218 = 536 bytes, 16 entries)
inline constexpr uint32_t kLiveObjectSize = 0x218u;
inline constexpr int kMaxLiveObjects = 16;
inline constexpr uint32_t kLiveObjectEnabledOffset = 0x08u; // bool
inline constexpr uint32_t kLiveObjectIdOffset = 0x0Au; // int16_t
inline constexpr uint32_t kLiveObjectCoordMinOffset = 0x134u; // int32_t x, y (24.8 fixed-point)
inline constexpr uint32_t kLiveObjectCoordMaxOffset = 0x13Cu; // int32_t x, y (24.8 fixed-point)

// struct live_performer (size: 0x214 = 532 bytes, 16 entries)
inline constexpr uint32_t kLivePerformerSize = 0x214u;
inline constexpr int kMaxLivePerformers = 16;
inline constexpr uint32_t kLivePerformerEnabledOffset = 0x08u; // bool
inline constexpr uint32_t kLivePerformerIdOffset = 0x0Au; // int16_t
inline constexpr uint32_t kLivePerformerCoordMinOffset = 0x130u; // int32_t x, y (24.8 fixed-point)
inline constexpr uint32_t kLivePerformerCoordMaxOffset = 0x138u; // int32_t x, y (24.8 fixed-point)
} // namespace ground

} // namespace layout

/// Bounds-checked read-only view into NDS Main RAM (0x02000000..0x02000000+size).
/// Guarantees zero mutation of guest memory and prevents out-of-bounds pointer dereferences.
class GuestMemoryView {
public:
    constexpr GuestMemoryView() noexcept = default;
    constexpr GuestMemoryView(const uint8_t* main_ram, size_t size = layout::kMainRamSize) noexcept
        : main_ram_(main_ram), size_(size) {}

    [[nodiscard]] constexpr bool valid() const noexcept {
        return main_ram_ != nullptr && size_ > 0;
    }

    [[nodiscard]] constexpr bool in_range(uint32_t guest_addr, size_t byte_count) const noexcept {
        if (main_ram_ == nullptr || guest_addr < layout::kMainRamBase) {
            return false;
        }
        const uint64_t offset = static_cast<uint64_t>(guest_addr - layout::kMainRamBase);
        return offset + byte_count <= size_;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> read(uint32_t guest_addr) const noexcept {
        static_assert(std::is_trivially_copyable_v<T>, "GuestMemoryView::read requires trivially copyable type");
        if (!in_range(guest_addr, sizeof(T))) {
            return std::nullopt;
        }
        T val{};
        std::memcpy(&val, main_ram_ + (guest_addr - layout::kMainRamBase), sizeof(T));
        return val;
    }

    template <typename T>
    [[nodiscard]] T read_or(uint32_t guest_addr, T fallback = T{}) const noexcept {
        const auto res = read<T>(guest_addr);
        return res.has_value() ? *res : fallback;
    }

    [[nodiscard]] const uint8_t* raw_ptr(uint32_t guest_addr, size_t byte_count) const noexcept {
        if (!in_range(guest_addr, byte_count)) {
            return nullptr;
        }
        return main_ram_ + (guest_addr - layout::kMainRamBase);
    }

    /// Resolves DUNGEON_PTR_MASTER (or DUNGEON_PTR fallback) and verifies that the entire
    /// struct dungeon header up to display_data fits in Main RAM.
    [[nodiscard]] uint32_t resolve_dungeon_ptr(pmdsky::Region region = pmdsky::Region::NA) const noexcept {
        const auto& syms = pmdsky::Get(region);
        uint32_t ptr = read_or<uint32_t>(syms.DUNGEON_PTR_MASTER, 0u);
        if (!in_range(ptr, layout::dungeon::kDisplayDataOffset + layout::display_data::kSize)) {
            ptr = read_or<uint32_t>(syms.DUNGEON_PTR, 0u);
            if (!in_range(ptr, layout::dungeon::kDisplayDataOffset + layout::display_data::kSize)) {
                return 0u;
            }
        }
        return ptr;
    }

private:
    const uint8_t* main_ram_ = nullptr;
    size_t size_ = 0;
};

} // namespace skyrecomp
