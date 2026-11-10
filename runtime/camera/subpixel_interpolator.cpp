#include "subpixel_interpolator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace skyrecomp::camera {
namespace {

/// Linearly interpolates between two 24.8 fixed-point coordinates using 64-bit widened subtraction.
[[nodiscard]] int32_t LerpFixedQ8(int32_t prev_q8, int32_t curr_q8, float alpha) noexcept {
    const int64_t diff = static_cast<int64_t>(curr_q8) - static_cast<int64_t>(prev_q8);
    const double delta = static_cast<double>(diff);
    return prev_q8 + static_cast<int32_t>(std::llround(delta * static_cast<double>(alpha)));
}

/// Returns true when the single-tick 24.8 fixed-point delta exceeds the teleport/cutscene threshold.
[[nodiscard]] bool ExceedsStepThreshold(int32_t prev_q8, int32_t curr_q8) noexcept {
    const int64_t diff = std::llabs(static_cast<int64_t>(curr_q8) - static_cast<int64_t>(prev_q8));
    return diff > static_cast<int64_t>(kMaxInterpolatedDeltaQ8);
}

/// Computes the interpolated sub-pixel pose for a single entity or actor between ticks t-1 and t.
[[nodiscard]] InterpolatedEntityPose InterpolateSingleEntity(
    const EntityCoordSample& prev,
    const EntityCoordSample& curr,
    float alpha,
    bool global_discontinuity,
    int32_t interp_cam_x_q8,
    int32_t interp_cam_y_q8) noexcept {
    InterpolatedEntityPose out{};
    if (!curr.active) {
        return out;
    }
    out.active = true;

    const bool entity_discontinuity =
        global_discontinuity ||
        !prev.active ||
        (prev.type != curr.type) ||
        (prev.spawn_genid != curr.spawn_genid) ||
        ExceedsStepThreshold(prev.world_x_q8, curr.world_x_q8) ||
        ExceedsStepThreshold(prev.world_y_q8, curr.world_y_q8);

    if (entity_discontinuity || alpha >= 0.9999f) {
        out.snapped_discontinuity = entity_discontinuity;
        out.interp_x_q8 = curr.world_x_q8;
        out.interp_y_q8 = curr.world_y_q8;
        out.interp_elevation_q8 = curr.elevation_q8;
        out.subpixel_dx_q8 = 0;
        out.subpixel_dy_q8 = 0;
    } else {
        out.interp_x_q8 = LerpFixedQ8(prev.world_x_q8, curr.world_x_q8, alpha);
        out.interp_y_q8 = LerpFixedQ8(prev.world_y_q8, curr.world_y_q8, alpha);
        out.interp_elevation_q8 = LerpFixedQ8(prev.elevation_q8, curr.elevation_q8, alpha);
        out.subpixel_dx_q8 = static_cast<int16_t>(out.interp_x_q8 - curr.world_x_q8);
        out.subpixel_dy_q8 = static_cast<int16_t>(out.interp_y_q8 - curr.world_y_q8);
    }

    out.relative_screen_x_q8 = out.interp_x_q8 - interp_cam_x_q8;
    out.relative_screen_y_q8 = out.interp_y_q8 - interp_cam_y_q8;
    return out;
}

} // namespace

/// Constructs a read-only sub-pixel interpolator for the given ROM region and refresh mode.
SubpixelInterpolator::SubpixelInterpolator(
    pmdsky::Region region,
    RefreshRateMode mode) noexcept
    : region_(region), refresh_mode_(mode) {}

/// Returns the canonical sub-frame alphas in (0, 1] for a single 60Hz logic tick at `target_hz`.
std::vector<float> SubpixelInterpolator::ComputeSubFrameAlphas(int target_hz) {
    if (target_hz <= 60) {
        return {1.0f};
    }
    const double step = 60.0 / static_cast<double>(target_hz);
    std::vector<float> alphas;
    double accum = step;
    while (accum < 1.0 - 1e-5) {
        alphas.push_back(static_cast<float>(accum));
        accum += step;
    }
    alphas.push_back(1.0f);
    return alphas;
}

/// Clears captured snapshots and resets interpolation history.
void SubpixelInterpolator::Reset() noexcept {
    snapshot_count_ = 0;
    prev_ = LogicTickSnapshot{};
    curr_ = LogicTickSnapshot{};
}

/// Captures a new authoritative 60Hz logic tick snapshot from read-only guest Main RAM.
void SubpixelInterpolator::CaptureLogicTick(GuestMemoryView mem) noexcept {
    LogicTickSnapshot next = ReadSnapshotFromGuest(mem);
    next.tick_index = snapshot_count_ + 1;
    if (snapshot_count_ == 0) {
        prev_ = next;
        curr_ = next;
    } else {
        prev_ = curr_;
        curr_ = next;
    }
    ++snapshot_count_;
}

/// Reads a complete camera and entity coordinate snapshot from read-only guest RAM.
LogicTickSnapshot SubpixelInterpolator::ReadSnapshotFromGuest(GuestMemoryView mem) const noexcept {
    LogicTickSnapshot snap{};
    if (!mem.valid()) {
        return snap;
    }

    const auto& syms = pmdsky::Get(region_);

    // 1. Check Dungeon Mode (overlay29: DUNGEON_PTR_MASTER / DUNGEON_PTR)
    const uint32_t dungeon_ptr = mem.resolve_dungeon_ptr(region_);
    if (dungeon_ptr != 0u) {
        snap.mode = ActiveGameMode::DungeonOverlay29;

        const uint32_t disp_base = dungeon_ptr + layout::dungeon::kDisplayDataOffset;
        const int16_t cam_px_x = mem.read_or<int16_t>(disp_base + layout::display_data::kCameraPixelPosOffset + 0u, 0);
        const int16_t cam_px_y = mem.read_or<int16_t>(disp_base + layout::display_data::kCameraPixelPosOffset + 2u, 0);
        snap.camera_x_q8 = static_cast<int32_t>(cam_px_x) * 256;
        snap.camera_y_q8 = static_cast<int32_t>(cam_px_y) * 256;
        snap.floor_number = mem.read_or<int16_t>(disp_base + layout::display_data::kFloorTouchScreenOffset, 0);

        const uint32_t entities_base =
            dungeon_ptr + layout::dungeon::kEntityTableOffset + layout::entity_table::kEntitiesArrayOffset;

        for (int i = 0; i < layout::entity_table::kMaxEntities; ++i) {
            const uint32_t ent_addr = entities_base + static_cast<uint32_t>(i) * layout::entity::kSize;
            if (!mem.in_range(ent_addr, layout::entity::kSize)) {
                break;
            }
            const int32_t ent_type = mem.read_or<int32_t>(ent_addr + layout::entity::kTypeOffset, 0);
            if (ent_type == 0) {
                continue;
            }
            EntityCoordSample sample{};
            sample.active = true;
            sample.type = ent_type;
            sample.spawn_genid = mem.read_or<uint16_t>(ent_addr + layout::entity::kSpawnGenIdOffset, 0u);
            sample.world_x_q8 = mem.read_or<int32_t>(ent_addr + layout::entity::kPixelPosOffset + 0u, 0);
            sample.world_y_q8 = mem.read_or<int32_t>(ent_addr + layout::entity::kPixelPosOffset + 4u, 0);
            sample.elevation_q8 = mem.read_or<int32_t>(ent_addr + layout::entity::kElevationOffset, 0);
            snap.dungeon_entities[static_cast<size_t>(i)] = sample;
        }
        return snap;
    }

    // 2. Check Ground Mode (overlay11: GROUND_STATE_MAP + GROUND_STATE_PTRS)
    const uint32_t ground_bg_ptr = mem.read_or<uint32_t>(syms.GROUND_STATE_MAP, 0u);
    const bool has_ground_bg = mem.in_range(ground_bg_ptr, layout::ground::kGroundBgSize);
    const bool has_ground_ptrs = mem.in_range(syms.GROUND_STATE_PTRS, layout::ground::kMainGroundDataSize);

    if (has_ground_bg || has_ground_ptrs) {
        if (has_ground_bg) {
            snap.mode = ActiveGameMode::GroundOverlay11;
            const uint32_t cam_addr = ground_bg_ptr + layout::ground::kGroundBgCameraPixelPosOffset;
            snap.camera_x_q8 = mem.read_or<int32_t>(cam_addr + 0u, 0);
            snap.camera_y_q8 = mem.read_or<int32_t>(cam_addr + 4u, 0);
        }

        if (has_ground_ptrs) {
            const uint32_t actors_ptr = mem.read_or<uint32_t>(
                syms.GROUND_STATE_PTRS + layout::ground::kGroundActorsPtrOffset, 0u);
            const uint32_t objects_ptr = mem.read_or<uint32_t>(
                syms.GROUND_STATE_PTRS + layout::ground::kGroundObjectsPtrOffset, 0u);
            const uint32_t performers_ptr = mem.read_or<uint32_t>(
                syms.GROUND_STATE_PTRS + layout::ground::kGroundPerformersPtrOffset, 0u);

            size_t slot = 0;

            // Live actors (24)
            if (mem.in_range(actors_ptr, layout::ground::kLiveActorSize * layout::ground::kMaxLiveActors)) {
                snap.mode = ActiveGameMode::GroundOverlay11;
                for (int i = 0; i < layout::ground::kMaxLiveActors; ++i, ++slot) {
                    const uint32_t base = actors_ptr + static_cast<uint32_t>(i) * layout::ground::kLiveActorSize;
                    const bool enabled = mem.read_or<uint8_t>(base + layout::ground::kLiveActorEnabledOffset, 0u) != 0u;
                    if (!enabled) {
                        continue;
                    }
                    EntityCoordSample sample{};
                    sample.active = true;
                    sample.type = 1; // Actor
                    sample.spawn_genid = mem.read_or<uint16_t>(base + layout::ground::kLiveActorIdOffset, 0u);
                    sample.world_x_q8 = mem.read_or<int32_t>(base + layout::ground::kLiveActorCoordMinOffset + 0u, 0);
                    sample.world_y_q8 = mem.read_or<int32_t>(base + layout::ground::kLiveActorCoordMinOffset + 4u, 0);
                    sample.elevation_q8 = mem.read_or<int32_t>(base + layout::ground::kLiveActorHeightOffset, 0);
                    snap.ground_entities[slot] = sample;
                }
            } else {
                slot += layout::ground::kMaxLiveActors;
            }

            // Live objects (16)
            if (mem.in_range(objects_ptr, layout::ground::kLiveObjectSize * layout::ground::kMaxLiveObjects)) {
                snap.mode = ActiveGameMode::GroundOverlay11;
                for (int i = 0; i < layout::ground::kMaxLiveObjects; ++i, ++slot) {
                    const uint32_t base = objects_ptr + static_cast<uint32_t>(i) * layout::ground::kLiveObjectSize;
                    const bool enabled = mem.read_or<uint8_t>(base + layout::ground::kLiveObjectEnabledOffset, 0u) != 0u;
                    if (!enabled) {
                        continue;
                    }
                    EntityCoordSample sample{};
                    sample.active = true;
                    sample.type = 2; // Object
                    sample.spawn_genid = mem.read_or<uint16_t>(base + layout::ground::kLiveObjectIdOffset, 0u);
                    sample.world_x_q8 = mem.read_or<int32_t>(base + layout::ground::kLiveObjectCoordMinOffset + 0u, 0);
                    sample.world_y_q8 = mem.read_or<int32_t>(base + layout::ground::kLiveObjectCoordMinOffset + 4u, 0);
                    snap.ground_entities[slot] = sample;
                }
            } else {
                slot += layout::ground::kMaxLiveObjects;
            }

            // Live performers (16)
            if (mem.in_range(performers_ptr, layout::ground::kLivePerformerSize * layout::ground::kMaxLivePerformers)) {
                snap.mode = ActiveGameMode::GroundOverlay11;
                for (int i = 0; i < layout::ground::kMaxLivePerformers; ++i, ++slot) {
                    const uint32_t base = performers_ptr + static_cast<uint32_t>(i) * layout::ground::kLivePerformerSize;
                    const bool enabled = mem.read_or<uint8_t>(base + layout::ground::kLivePerformerEnabledOffset, 0u) != 0u;
                    if (!enabled) {
                        continue;
                    }
                    EntityCoordSample sample{};
                    sample.active = true;
                    sample.type = 3; // Performer
                    sample.spawn_genid = mem.read_or<uint16_t>(base + layout::ground::kLivePerformerIdOffset, 0u);
                    sample.world_x_q8 = mem.read_or<int32_t>(base + layout::ground::kLivePerformerCoordMinOffset + 0u, 0);
                    sample.world_y_q8 = mem.read_or<int32_t>(base + layout::ground::kLivePerformerCoordMinOffset + 4u, 0);
                    snap.ground_entities[slot] = sample;
                }
            }
        }
    }

    return snap;
}

/// Evaluates the interpolated camera & entity coordinates at fractional phase `alpha` in [0.0f, 1.0f].
InterpolatedSubFrame SubpixelInterpolator::EvaluateSubFrame(float alpha) const noexcept {
    InterpolatedSubFrame out{};
    out.mode = curr_.mode;

    const float effective_alpha =
        (refresh_mode_ == RefreshRateMode::Locked_60Hz || snapshot_count_ < 2)
            ? 1.0f
            : std::clamp(alpha, 0.0f, 1.0f);
    out.alpha = effective_alpha;

    const bool global_discontinuity =
        (prev_.mode != curr_.mode) ||
        (prev_.floor_number != curr_.floor_number);

    const bool camera_discontinuity =
        global_discontinuity ||
        ExceedsStepThreshold(prev_.camera_x_q8, curr_.camera_x_q8) ||
        ExceedsStepThreshold(prev_.camera_y_q8, curr_.camera_y_q8);

    if (camera_discontinuity || effective_alpha >= 0.9999f) {
        out.camera_snapped = camera_discontinuity;
        out.camera_x_q8 = curr_.camera_x_q8;
        out.camera_y_q8 = curr_.camera_y_q8;
        out.camera_subpixel_dx_q8 = 0;
        out.camera_subpixel_dy_q8 = 0;
    } else {
        out.camera_x_q8 = LerpFixedQ8(prev_.camera_x_q8, curr_.camera_x_q8, effective_alpha);
        out.camera_y_q8 = LerpFixedQ8(prev_.camera_y_q8, curr_.camera_y_q8, effective_alpha);
        out.camera_subpixel_dx_q8 = static_cast<int16_t>(out.camera_x_q8 - curr_.camera_x_q8);
        out.camera_subpixel_dy_q8 = static_cast<int16_t>(out.camera_y_q8 - curr_.camera_y_q8);
    }

    if (curr_.mode == ActiveGameMode::DungeonOverlay29) {
        for (size_t i = 0; i < layout::entity_table::kMaxEntities; ++i) {
            out.dungeon_poses[i] = InterpolateSingleEntity(
                prev_.dungeon_entities[i],
                curr_.dungeon_entities[i],
                effective_alpha,
                global_discontinuity,
                out.camera_x_q8,
                out.camera_y_q8);
        }
    } else if (curr_.mode == ActiveGameMode::GroundOverlay11) {
        for (size_t i = 0; i < LogicTickSnapshot::kMaxGroundEntities; ++i) {
            out.ground_poses[i] = InterpolateSingleEntity(
                prev_.ground_entities[i],
                curr_.ground_entities[i],
                effective_alpha,
                global_discontinuity,
                out.camera_x_q8,
                out.camera_y_q8);
        }
    }

    return out;
}

} // namespace skyrecomp::camera
