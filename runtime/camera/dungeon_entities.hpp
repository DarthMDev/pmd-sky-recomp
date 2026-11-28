#pragma once

#include "skyrecomp/guest_mem.hpp"

#include <array>
#include <cstdint>

namespace skyrecomp::camera {

// X/Y offsets in half pixels, one pair per hardware OAM entry.
using DungeonObjectOffsets = std::array<std::array<int8_t, 2>, 128>;

// Read-only observation of the guest calls that submit and sort dungeon OBJ
// fragments. Only the serial Sky presentation path enables this observer.
class DungeonEntityRaster {
public:
    void BeginFrame(GuestMemoryView mem, pmdsky::Region region, uint64_t dungeon_identity);
    void ObserveCall(uint32_t target, uint32_t return_pc, uint32_t sp, uint32_t arg);
    void ObserveReturn(uint32_t pc, uint32_t sp);
    void LatchOam(const uint8_t* oam);
    void InvalidateOam() { pending_.valid = false; }
    void FinishFrame(bool valid);

    [[nodiscard]] bool WatchesCall(uint32_t target) const {
        return identity_ && (target == symbols_->DisplayMonster ||
            target == symbols_->DisplayMonsterShadow ||
            target == symbols_->DisplayAndSwitchAnimationControlCurrentFrame ||
            target == symbols_->GroupOamAttributesWrapper || target == symbols_->CopyAttributesToOam ||
            target == symbols_->ClearGroupedOamObjsAndGroups);
    }
    [[nodiscard]] bool WatchesReturn(uint32_t pc) const {
        return identity_ && (pc == submission_.return_pc || pc == entity_return_);
    }
    [[nodiscard]] bool Enabled() const { return identity_ != 0; }
    [[nodiscard]] bool MidpointOffsets(int16_t camera_dx_q8, int16_t camera_dy_q8,
                                      DungeonObjectOffsets& offsets) const;

private:
    static constexpr size_t kSceneObjects = 112;
    struct Owner {
        uint32_t entity = 0;
        uint16_t generation = 0;
        bool shadow = false;
        int32_t x = 0, y = 0, elevation = 0;
    };
    using Owners = std::array<Owner, kSceneObjects>;
    struct Submission {
        uint32_t return_pc = 0;
        uint32_t sp = 0;
        int first = 0;
        Owner owner{};
    };
    struct OamList {
        Owners owners{};
        std::array<std::array<uint16_t, 3>, kSceneObjects> attributes{};
        bool valid = false;
    };

    [[nodiscard]] bool SameFloor() const;
    [[nodiscard]] uint32_t MainOamInfo() const;
    [[nodiscard]] int ObjectCount(uint32_t info) const;
    void GroupObjects(uint32_t info);
    [[nodiscard]] bool MatchesOam(const OamList& list, const uint8_t* oam) const;

    GuestMemoryView mem_{};
    const pmdsky::SymbolAddresses* symbols_ = nullptr;
    pmdsky::Region region_ = pmdsky::Region::NA;
    uint64_t identity_ = 0;
    uint32_t entity_ = 0;
    uint32_t entity_return_ = 0, entity_sp_ = 0;
    Submission submission_{};
    Owners ungrouped_{};
    OamList grouped_{}, copied_{}, pending_{}, current_{}, previous_{};
};

extern DungeonEntityRaster dungeon_entity_raster;

} // namespace skyrecomp::camera
