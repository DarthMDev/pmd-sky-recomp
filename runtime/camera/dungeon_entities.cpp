#include "dungeon_entities.hpp"
#include "subpixel_interpolator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace skyrecomp::camera {
namespace {

// pmdsky-debug's oam_info and obj_graphics_control layouts.
constexpr uint32_t kOamInfoOffset = 0x20;
constexpr uint32_t kGroupCount = 0x140;
constexpr uint32_t kObjectCountOffset = 0x08;
constexpr uint32_t kGroupHeadsOffset = 0x0C;
constexpr uint32_t kUngroupedOffset = 0x10;
constexpr uint32_t kShouldCopyOffset = 0x14;
constexpr uint32_t kHardwareBaseOffset = 0x18;
constexpr uint32_t kObjectSize = 8;
constexpr uint32_t kEngineAOam = 0x07000000;
constexpr int32_t kMonsterType = 1;

} // namespace

DungeonEntityRaster dungeon_entity_raster;

void DungeonEntityRaster::BeginFrame(GuestMemoryView mem, pmdsky::Region region,
                                     uint64_t dungeon_identity) {
    if (identity_ != dungeon_identity || region_ != region)
        *this = {};
    mem_ = mem;
    region_ = region;
    symbols_ = &pmdsky::Get(region);
    identity_ = dungeon_identity;
    pending_.valid = false;
}

bool DungeonEntityRaster::SameFloor() const {
    const uint32_t dungeon = mem_.resolve_dungeon_ptr(region_);
    return dungeon != 0 && dungeon == identity_ >> 16 &&
           mem_.read_or<uint16_t>(dungeon + layout::dungeon::kFloorPairOffset) ==
               static_cast<uint16_t>(identity_);
}

uint32_t DungeonEntityRaster::MainOamInfo() const {
    const uint32_t control = mem_.read_or<uint32_t>(symbols_->OBJ_GRAPHICS_CONTROLS_PTR);
    const uint32_t info = control + kOamInfoOffset;
    if (!mem_.in_range(control, kOamInfoOffset + 0x20) ||
        mem_.read_or<uint32_t>(info + kHardwareBaseOffset) != kEngineAOam ||
        mem_.read_or<uint32_t>(info) != kSceneObjects)
        return 0;
    return info;
}

int DungeonEntityRaster::ObjectCount(uint32_t info) const {
    const int count = mem_.read_or<int32_t>(info + kObjectCountOffset, -1);
    return count >= 0 && count <= static_cast<int>(kSceneObjects) ? count : -1;
}

void DungeonEntityRaster::ObserveCall(uint32_t target, uint32_t return_pc,
                                      uint32_t sp, uint32_t arg) {
    const auto& sym = *symbols_;
    if (target == sym.DisplayMonster) {
        entity_ = 0;
        submission_ = {};
        const uint32_t first = static_cast<uint32_t>(identity_ >> 16) +
            layout::dungeon::kEntityTableOffset + layout::entity_table::kEntitiesArrayOffset;
        if (!SameFloor() || arg < first || (arg - first) % layout::entity::kSize ||
            (arg - first) / layout::entity::kSize >= layout::entity_table::kMaxEntities ||
            mem_.read_or<int32_t>(arg + layout::entity::kTypeOffset) != kMonsterType)
            return;
        entity_ = arg;
        entity_return_ = return_pc;
        entity_sp_ = sp;
        return;
    }

    if (target == sym.DisplayAndSwitchAnimationControlCurrentFrame || target == sym.DisplayMonsterShadow) {
        if (!entity_ || submission_.return_pc || !SameFloor()) return;
        const bool shadow = target == sym.DisplayMonsterShadow;
        if (!shadow && arg != entity_ + layout::entity::kGraphicalParamsOffset) return;
        const int count = ObjectCount(MainOamInfo());
        const uint16_t generation = mem_.read_or<uint16_t>(entity_ + layout::entity::kSpawnGenIdOffset);
        if (count < 0 || !generation) return;
        submission_ = {return_pc, sp, count, {
            entity_, generation, shadow,
            mem_.read_or<int32_t>(entity_ + layout::entity::kPixelPosOffset),
            mem_.read_or<int32_t>(entity_ + layout::entity::kPixelPosOffset + 4),
            shadow ? 0 : mem_.read_or<int32_t>(entity_ + layout::entity::kElevationOffset)}};
        return;
    }

    const uint32_t info = MainOamInfo();
    if (!info) return;
    // The wrapper receives obj_graphics_control and tail-calls GroupOamObjs
    // with its embedded oam_info. The copy and clear routines receive oam_info.
    if (target == sym.GroupOamAttributesWrapper) {
        if (arg != info - kOamInfoOffset) return;
        arg = info;
        if (ObjectCount(arg) == 0) return; // The guest retains its last grouped list.
        GroupObjects(arg);
        ungrouped_ = {};
        submission_ = {};
    } else if (arg != info) {
        return;
    } else if (target == sym.ClearGroupedOamObjsAndGroups) {
        ungrouped_ = {};
        grouped_.valid = false;
        submission_ = {};
    } else if (mem_.read_or<uint8_t>(arg + kShouldCopyOffset)) {
        copied_ = grouped_;
        if (!SameFloor()) copied_.valid = false;
    }
}

void DungeonEntityRaster::ObserveReturn(uint32_t pc, uint32_t sp) {
    if (pc == submission_.return_pc && sp == submission_.sp) {
        const int end = ObjectCount(MainOamInfo());
        for (int i = submission_.first; i < end; ++i)
            ungrouped_[i] = submission_.owner;
        submission_ = {};
    }
    if (pc == entity_return_ && sp == entity_sp_) {
        entity_ = 0;
        entity_return_ = 0;
        submission_ = {};
    }
}

void DungeonEntityRaster::GroupObjects(uint32_t info) {
    grouped_.valid = false;
    const int count = ObjectCount(info);
    const uint32_t group_heads = mem_.read_or<uint32_t>(info + kGroupHeadsOffset);
    const uint32_t objects = mem_.read_or<uint32_t>(info + kUngroupedOffset);
    if (count <= 0 || !SameFloor() || mem_.read_or<uint32_t>(info + 4) != kGroupCount ||
        !mem_.in_range(group_heads, kGroupCount * 2) || !mem_.in_range(objects, count * kObjectSize))
        return;

    // GroupOamObjs walks groups forwards and each linked list backwards,
    // writing from the last occupied hardware slot toward slot zero.
    int slot = count;
    std::array<bool, kSceneObjects> visited{};
    for (uint32_t group = 0; group < kGroupCount; ++group) {
        int index = mem_.read_or<int16_t>(group_heads + group * 2);
        while (index >= 0) {
            if (!slot || index >= count || visited[index]) return;
            visited[index] = true;
            --slot;
            const uint32_t addr = objects + index * kObjectSize;
            auto& attr = grouped_.attributes[slot];
            for (uint32_t word = 0; word < attr.size(); ++word)
                attr[word] = mem_.read_or<uint16_t>(addr + word * 2);
            // Window sprites define a mask, not a visible monster fragment.
            grouped_.owners[slot] = (attr[0] & 0xC00) == 0x800 ? Owner{} : ungrouped_[index];
            index = mem_.read_or<int16_t>(addr + 6);
        }
    }
    if (slot != 0) return;
    for (size_t i = count; i < kSceneObjects; ++i) {
        grouped_.attributes[i] = {0x200, 0, 0};
        grouped_.owners[i] = {};
    }
    grouped_.valid = true;
}

bool DungeonEntityRaster::MatchesOam(const OamList& list, const uint8_t* oam) const {
    if (!list.valid) return false;
    for (size_t i = 0; i < kSceneObjects; ++i) {
        const auto& attr = list.attributes[i];
        if (std::memcmp(attr.data(), oam + i * kObjectSize, attr.size() * sizeof(attr[0])) != 0)
            return false;
    }
    return true;
}

void DungeonEntityRaster::LatchOam(const uint8_t* oam) {
    pending_.valid = false;
    if (Enabled() && SameFloor() && MatchesOam(copied_, oam))
        pending_ = copied_;
}

void DungeonEntityRaster::FinishFrame(bool valid) {
    previous_ = current_;
    current_ = pending_;
    current_.valid = current_.valid && valid;
}

bool DungeonEntityRaster::MidpointOffsets(int16_t camera_dx_q8, int16_t camera_dy_q8,
                                           DungeonObjectOffsets& offsets) const {
    offsets = {};
    if (!previous_.valid || !current_.valid) return false;

    bool moved = false;
    for (size_t i = 0; i < kSceneObjects; ++i) {
        const auto& curr = current_.owners[i];
        if (!curr.entity) continue;
        const auto same_owner = [&](const Owner& owner) {
            return owner.entity == curr.entity && owner.generation == curr.generation &&
                   owner.shadow == curr.shadow;
        };
        const auto prev = std::find_if(previous_.owners.begin(), previous_.owners.end(), same_owner);
        if (prev == previous_.owners.end()) continue;

        // Multiple fragments can share an owner. If the game drew the same
        // entity at different positions, there is no single matching pose.
        const auto conflicts = [&](const Owners& owners, const Owner& expected) {
            return std::any_of(owners.begin(), owners.end(), [&](const Owner& owner) {
                return same_owner(owner) && (owner.x != expected.x || owner.y != expected.y ||
                                             owner.elevation != expected.elevation);
            });
        };
        if (conflicts(previous_.owners, *prev) || conflicts(current_.owners, curr))
            continue;

        const int64_t dx = int64_t{prev->x} - curr.x;
        const int64_t dy = int64_t{prev->y} - curr.y;
        const int64_t dz = int64_t{prev->elevation} - curr.elevation;
        if (std::abs(dx) > kMaxInterpolatedDeltaQ8 || std::abs(dy) > kMaxInterpolatedDeltaQ8 ||
            std::abs(dz) > kMaxInterpolatedDeltaQ8)
            continue;
        // Translate the current animation fragment by the midpoint of the
        // entity's world movement, less the midpoint camera displacement.
        const auto x = static_cast<int8_t>(std::lround((dx - 2 * camera_dx_q8) / 256.0));
        const auto y = static_cast<int8_t>(std::lround((dy - dz - 2 * camera_dy_q8) / 256.0));
        offsets[i] = {x, y};
        moved |= x != 0 || y != 0;
    }
    return moved;
}

} // namespace skyrecomp::camera
