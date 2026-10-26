#include "Systems.hpp"
#include "Components.hpp"
#include "Interp.hpp"
#include <raymath.h>
#include <cmath>

namespace afteraction::ecs {

void register_systems(flecs::world& world) {
    world.set<InterpolationCtx>({});
    world.set<TrailCtx>({});

    // Interpolation: reads HistoryComp, writes Position/Rotation/Velocity
    world.system<HistoryComp, Position, Rotation, Velocity>("InterpolationSystem")
        .kind(flecs::OnUpdate)
        .run([](flecs::iter& it) {
            // Singleton accessed once per system run, before iterating tables
            const InterpolationCtx ctx = it.world().get<InterpolationCtx>();
            if (!ctx.live_mode) return; // Skip in playback/VCR mode — sync_ecs_to_playback handles this

            const uint64_t target_ns = ctx.playback_time_ns;

            while (it.next()) {
                auto hist_arr = it.field<HistoryComp>(0);
                auto pos_arr  = it.field<Position>(1);
                auto rot_arr  = it.field<Rotation>(2);
                auto vel_arr  = it.field<Velocity>(3);

                for (auto i : it) {
                    const auto& h  = hist_arr[i].hist;
                    if (h.count == 0) continue;   // no live data (e.g. a track from a file)
                    const auto  br = h.bracket(target_ns);

                    const auto& klo = *br.lo;
                    const auto& khi = *br.hi;

                    float alpha = 0.0f;
                    if (khi.timestamp_ns > klo.timestamp_ns) {
                        alpha  = static_cast<float>(target_ns - klo.timestamp_ns) /
                                 static_cast<float>(khi.timestamp_ns - klo.timestamp_ns);
                        alpha  = std::clamp(alpha, 0.0f, 1.0f);
                    }

                    Vector3 v0{klo.velocity[0],  klo.velocity[1],  klo.velocity[2]};
                    Vector3 v1{khi.velocity[0],  khi.velocity[1],  khi.velocity[2]};

                    Quaternion q0{klo.orientation[0], klo.orientation[1],
                                  klo.orientation[2], klo.orientation[3]};
                    Quaternion q1{khi.orientation[0], khi.orientation[1],
                                  khi.orientation[2], khi.orientation[3]};

                    auto sample = [](const EntityStateHistory::Keyframe& k) {
                        return TrackSample{ { k.position[0], k.position[1], k.position[2] }, k.timestamp_ns };
                    };
                    const TrackSample a = sample(klo), b = sample(khi);
                    const TrackSample pv = br.prev ? sample(*br.prev) : a, nx = br.next ? sample(*br.next) : b;
                    pos_arr[i].v = track_position(br.prev ? &pv : nullptr, a, b, br.next ? &nx : nullptr, alpha);
                    rot_arr[i].q = QuaternionSlerp(q0, q1, alpha);
                    vel_arr[i].v = Vector3Lerp(v0, v1, alpha);
                }
            }
        });

    // Trail update: runs after interpolation (registered second = runs second within OnUpdate)
    world.system<Position, Trail>("TrailUpdateSystem")
        .kind(flecs::OnUpdate)
        .run([](flecs::iter& it) {
            const InterpolationCtx interp_ctx = it.world().get<InterpolationCtx>();
            if (!interp_ctx.live_mode) return; // Skip in playback/VCR mode — sync_ecs_to_playback handles this

            const TrailCtx ctx = it.world().get<TrailCtx>();
            const float min_dist_sq = ctx.sample_distance_sq;

            while (it.next()) {
                auto pos_arr   = it.field<Position>(0);
                auto trail_arr = it.field<Trail>(1);

                for (auto i : it) {
                    Trail& tr   = trail_arr[i];
                    Vector3 pos = pos_arr[i].v;

                    if (tr.count == 0) {
                        tr.push(pos);
                        continue;
                    }

                    uint32_t last_slot = (tr.head == 0)
                        ? Trail::kMaxPoints - 1
                        : (tr.head - 1) % Trail::kMaxPoints;
                    Vector3 diff = Vector3Subtract(pos, tr.points[last_slot]);
                    if (Vector3LengthSqr(diff) >= min_dist_sq)
                        tr.push(pos);
                }
            }
        });
}

} // namespace afteraction::ecs
