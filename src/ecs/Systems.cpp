#include "Systems.hpp"
#include "Components.hpp"
#include <raymath.h>
#include <cmath>

namespace debrief::ecs {

// ─────────────────────────────────────────────────────────────────────────────
//  Hermite cubic interpolation for position
//
//  p(t) = h00·p0 + h10·(dt·v0) + h01·p1 + h11·(dt·v1)
//  where t ∈ [0,1],  dt = interval in seconds,  v in m/s
//
//  Produces physically plausible arcs between telemetry keyframes — much
//  smoother than linear lerp when the update rate is low (e.g. 5–10 Hz).
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
//  Interpolation System
//  Reads:  HistoryComp, InterpolationCtx (world singleton)
//  Writes: Position, Rotation, Velocity
// ─────────────────────────────────────────────────────────────────────────────
static void interpolation_system(flecs::iter& it,
                                 HistoryComp* hist_arr,
                                 Position* pos_arr,
                                 Rotation* rot_arr,
                                 Velocity* vel_arr)
{
    const auto* ctx = it.world().get<InterpolationCtx>();
    const uint64_t target_ns = ctx ? ctx->playback_time_ns : 0;

    for (auto i : it) {
        const auto& h   = hist_arr[i].hist;
        const auto  br  = h.bracket(target_ns);

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

// ─────────────────────────────────────────────────────────────────────────────
//  Trail Update System
//  Reads:  Position (after interpolation)
//  Writes: Trail
// ─────────────────────────────────────────────────────────────────────────────
static void trail_update_system(flecs::iter& it,
                                Position* pos_arr,
                                Trail* trail_arr)
{
    const auto* ctx = it.world().get<TrailCtx>();
    const float min_dist_sq = ctx ? ctx->sample_distance_sq : 4.0f;

    for (auto i : it) {
        Trail& tr   = trail_arr[i];
        Vector3 pos = pos_arr[i].v;

        if (tr.count == 0) {
            tr.push(pos);
            continue;
        }

        uint32_t last_slot = (tr.head == 0) ? Trail::kMaxPoints - 1 : (tr.head - 1) % Trail::kMaxPoints;
        Vector3 diff = Vector3Subtract(pos, tr.points[last_slot]);
        if (Vector3LengthSqr(diff) >= min_dist_sq)
            tr.push(pos);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void register_systems(flecs::world& world) {
    // Register singleton context components
    world.set<InterpolationCtx>({});
    world.set<TrailCtx>({});

    // Interpolation runs first (depends on history, writes position/rotation/vel)
    world.system<HistoryComp, Position, Rotation, Velocity>("InterpolationSystem")
        .kind(flecs::OnUpdate)
        .iter(interpolation_system);

    // Trail update runs after interpolation (needs fresh position)
    world.system<Position, Trail>("TrailUpdateSystem")
        .kind(flecs::OnUpdate)
        .after("InterpolationSystem")
        .iter(trail_update_system);
}

} // namespace debrief::ecs
