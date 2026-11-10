#pragma once

#include "skyrecomp/guest_mem.hpp"
#include "skyrecomp/pmdsky_addrs.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace skyrecomp::camera {

/// Target display refresh / sub-pixel interpolation mode (Task 3.2).
enum class RefreshRateMode : uint8_t {
    Locked_60Hz = 0,     // Vanilla 60Hz (alpha = 1.0, zero sub-pixel offset)
    ProMotion_120Hz = 1, // 2 display frames per 60Hz tick (alpha = 0.5, 1.0)
    HighRefresh_144Hz = 2, // 144Hz fractional cadence (step = 60 / 144)
    Esports_240Hz = 3,   // 4 display frames per 60Hz tick (alpha = 0.25, 0.50, 0.75, 1.0)
    UncappedVariable = 4, // Variable VRR / ProMotion cadence driven by elapsed wall-clock time
};

/// Active engine mode detected from guest RAM pointers.
enum class ActiveGameMode : uint8_t {
    None = 0,
    DungeonOverlay29 = 1,
    GroundOverlay11 = 2,
};

/// Maximum single-tick movement eligible for interpolation (24 pixels in 24.8 fixed-point).
/// Movements larger than 1 dungeon tile per 60Hz tick are treated as teleports/camera cuts and snap immediately.
inline constexpr int32_t kMaxInterpolatedDeltaQ8 = 24 * 256;

/// Snapshot of a single entity or ground actor/object/performer at a 60Hz logic tick.
struct EntityCoordSample {
    bool active = false;
    int32_t type = 0;
    uint16_t spawn_genid = 0;
    int32_t world_x_q8 = 0; // 24.8 fixed-point X
    int32_t world_y_q8 = 0; // 24.8 fixed-point Y
    int32_t elevation_q8 = 0; // 24.8 fixed-point Z/height
};

/// Snapshot of the full Dungeon + Ground camera and entity state at a 60Hz logic tick.
struct LogicTickSnapshot {
    uint64_t tick_index = 0;
    ActiveGameMode mode = ActiveGameMode::None;
    int16_t floor_number = 0;

    // Camera position in 24.8 fixed-point pixels
    int32_t camera_x_q8 = 0;
    int32_t camera_y_q8 = 0;

    // Dungeon entities (up to 149 in overlay29 entity_table)
    std::array<EntityCoordSample, layout::entity_table::kMaxEntities> dungeon_entities{};

    // Ground entities (24 actors + 16 objects + 16 performers = 56 in overlay11)
    static constexpr size_t kMaxGroundEntities =
        layout::ground::kMaxLiveActors +
        layout::ground::kMaxLiveObjects +
        layout::ground::kMaxLivePerformers;
    std::array<EntityCoordSample, kMaxGroundEntities> ground_entities{};
};

/// Interpolated sub-pixel result for a single entity at fractional phase alpha in [0, 1].
struct InterpolatedEntityPose {
    bool active = false;
    bool snapped_discontinuity = false;
    int32_t interp_x_q8 = 0;
    int32_t interp_y_q8 = 0;
    int32_t interp_elevation_q8 = 0;
    // Delta relative to tick t's authoritative position (interp - curr), suitable for OAM sprite shifting
    int16_t subpixel_dx_q8 = 0;
    int16_t subpixel_dy_q8 = 0;
    // Screen-space 24.8 fixed-point position relative to the interpolated camera
    int32_t relative_screen_x_q8 = 0;
    int32_t relative_screen_y_q8 = 0;
};

/// Complete interpolated sub-frame state for rendering at 120Hz / 144Hz / 240Hz.
struct InterpolatedSubFrame {
    ActiveGameMode mode = ActiveGameMode::None;
    float alpha = 1.0f;
    bool camera_snapped = false;
    int32_t camera_x_q8 = 0;
    int32_t camera_y_q8 = 0;
    int16_t camera_subpixel_dx_q8 = 0;
    int16_t camera_subpixel_dy_q8 = 0;

    std::array<InterpolatedEntityPose, layout::entity_table::kMaxEntities> dungeon_poses{};
    std::array<InterpolatedEntityPose, LogicTickSnapshot::kMaxGroundEntities> ground_poses{};
};

/// Read-only 120Hz / 144Hz / 240Hz Sub-Pixel Camera & Entity Interpolator (Task 3.2).
/// Never mutates guest RAM, guaranteeing 100% turn-based PRNG & gameplay determinism.
class SubpixelInterpolator {
public:
    explicit SubpixelInterpolator(
        pmdsky::Region region = pmdsky::Region::NA,
        RefreshRateMode mode = RefreshRateMode::ProMotion_120Hz) noexcept;

    void SetRegion(pmdsky::Region region) noexcept { region_ = region; }
    [[nodiscard]] pmdsky::Region GetRegion() const noexcept { return region_; }

    void SetRefreshRateMode(RefreshRateMode mode) noexcept { refresh_mode_ = mode; }
    [[nodiscard]] RefreshRateMode GetRefreshRateMode() const noexcept { return refresh_mode_; }

    /// Returns the number of display sub-frames per 60Hz logic tick for integer-multiple modes,
    /// or 0 for variable/fractional modes.
    [[nodiscard]] static constexpr int SubFramesPerLogicTick(RefreshRateMode mode) noexcept {
        switch (mode) {
            case RefreshRateMode::Locked_60Hz:
                return 1;
            case RefreshRateMode::ProMotion_120Hz:
                return 2;
            case RefreshRateMode::Esports_240Hz:
                return 4;
            case RefreshRateMode::HighRefresh_144Hz:
            case RefreshRateMode::UncappedVariable:
            default:
                return 0;
        }
    }

    /// Returns the canonical sub-frame alphas in (0, 1] for a single 60Hz logic tick at `target_hz`.
    [[nodiscard]] static std::vector<float> ComputeSubFrameAlphas(int target_hz);

    /// Captures a new authoritative 60Hz logic tick snapshot from read-only guest Main RAM (`t-1 <- t`, `t <- mem`).
    void CaptureLogicTick(GuestMemoryView mem) noexcept;

    /// Evaluates the interpolated camera & entity coordinates at fractional phase `alpha` in [0.0f, 1.0f].
    /// When `refresh_mode_ == RefreshRateMode::Locked_60Hz`, `alpha` is forced to 1.0f.
    [[nodiscard]] InterpolatedSubFrame EvaluateSubFrame(float alpha) const noexcept;

    [[nodiscard]] const LogicTickSnapshot& PreviousTick() const noexcept { return prev_; }
    [[nodiscard]] const LogicTickSnapshot& CurrentTick() const noexcept { return curr_; }
    [[nodiscard]] bool HasTwoSnapshots() const noexcept { return snapshot_count_ >= 2; }

    void Reset() noexcept;

private:
    [[nodiscard]] LogicTickSnapshot ReadSnapshotFromGuest(GuestMemoryView mem) const noexcept;

    pmdsky::Region region_ = pmdsky::Region::NA;
    RefreshRateMode refresh_mode_ = RefreshRateMode::ProMotion_120Hz;
    uint64_t snapshot_count_ = 0;
    LogicTickSnapshot prev_{};
    LogicTickSnapshot curr_{};
};

} // namespace skyrecomp::camera
