#include "Systems.hpp"
#include "Components.hpp"
#include <raymath.h>
#include <cmath>

namespace afteraction::ecs {

// ─────────────────────────────────────────────────────────────────────────────
//  Hermite cubic interpolation for position
//
//  p(t) = h00·p0 + h10·(dt·v0) + h01·p1 + h11·(dt·v1)
//  where t ∈ [0,1],  dt = interval in seconds,  v in m/s
// ─────────────────────────────────────────────────────────────────────────────
static Vector3 hermite_interp(
    Vector3 p0, Vector3 v0,
    Vector3 p1, Vector3 v1,
    float dt_sec, float t) noexcept
{
    const float t2 = t * t, t3 = t2 * t;
    const float h00 =  2*t3 - 3*t2 + 1;
    const float h10 =    t3 - 2*t2 + t;
    const float h01 = -2*t3 + 3*t2;
    const float h11 =    t3 -   t2;
    return {
        h00*p0.x + h10*(v0.x*dt_sec) + h01*p1.x + h11*(v1.x*dt_sec),
        h00*p0.y + h10*(v0.y*dt_sec) + h01*p1.y + h11*(v1.y*dt_sec),
        h00*p0.z + h10*(v0.z*dt_sec) + h01*p1.z + h11*(v1.z*dt_sec),
    };
}

// ─────────────────────────────────────────────────────────────────────────────
void register_systems(flecs::world& world) {
    world.set<InterpolationCtx>({});
    world.set<TrailCtx>({});

    // Interpolation: reads HistoryComp, writes Position/Rotation/Velocity
    world.system<HistoryComp, Position, Rotation, Velocity>("InterpolationSystem")
        .kind(flecs::OnUpdate)
        .run([](flecs::iter& it) {
            // Singleton accessed once per system run, before iterating tables
            const auto& ctx = it.world().get<InterpolationCtx>();
            if (!ctx.live_mode) return; // Skip in playback/VCR mode — sync_ecs_to_playback handles this

            const uint64_t target_ns = ctx.playback_time_ns;

            while (it.next()) {
                auto hist_arr = it.field<HistoryComp>(0);
                auto pos_arr  = it.field<Position>(1);
                auto rot_arr  = it.field<Rotation>(2);
                auto vel_arr  = it.field<Velocity>(3);

                for (auto i : it) {
                    const auto& h  = hist_arr[i].hist;
                    const auto  br = h.bracket(target_ns);

                    const auto& klo = *br.lo;
                    const auto& khi = *br.hi;

                    float alpha = 0.0f;
                    float dt_sec = 0.0f;
                    if (khi.timestamp_ns > klo.timestamp_ns) {
                        dt_sec = static_cast<float>(khi.timestamp_ns - klo.timestamp_ns) * 1e-9f;
                        alpha  = static_cast<float>(target_ns - klo.timestamp_ns) /
                                 static_cast<float>(khi.timestamp_ns - klo.timestamp_ns);
                        alpha  = std::clamp(alpha, 0.0f, 1.0f);
                    }

                    Vector3 p0{klo.position[0], klo.position[1], klo.position[2]};
                    Vector3 v0{klo.velocity[0],  klo.velocity[1],  klo.velocity[2]};
                    Vector3 p1{khi.position[0], khi.position[1], khi.position[2]};
                    Vector3 v1{khi.velocity[0],  khi.velocity[1],  khi.velocity[2]};

                    Quaternion q0{klo.orientation[0], klo.orientation[1],
                                  klo.orientation[2], klo.orientation[3]};
                    Quaternion q1{khi.orientation[0], khi.orientation[1],
                                  khi.orientation[2], khi.orientation[3]};

                    pos_arr[i].v = hermite_interp(p0, v0, p1, v1, dt_sec, alpha);
                    rot_arr[i].q = QuaternionSlerp(q0, q1, alpha);
                    vel_arr[i].v = Vector3Lerp(v0, v1, alpha);
                }
            }
        });

    // Trail update: runs after interpolation (registered second = runs second within OnUpdate)
    world.system<Position, Trail>("TrailUpdateSystem")
        .kind(flecs::OnUpdate)
        .run([](flecs::iter& it) {
            const auto& interp_ctx = it.world().get<InterpolationCtx>();
            if (!interp_ctx.live_mode) return; // Skip in playback/VCR mode — sync_ecs_to_playback handles this

            const auto& ctx = it.world().get<TrailCtx>();
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
