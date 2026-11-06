#pragma once
#include <raylib.h>
#include <cstdint>

namespace afteraction::ecs {

// A reported position and its time.
struct TrackSample { Vector3 p; uint64_t t_ns; };

// Position between samples a and b (alpha ∈ [0,1]) on a cubic through every
// reported position. Tangents come from the neighbouring samples
// (non-uniform Catmull-Rom), not from the reported speed/heading — those may
// be airspeed or nose heading rather than ground speed/track, which made the
// path bulge away from the reported positions between updates. A neighbour
// much further away in time than the a→b interval is ignored.
inline Vector3 track_position(const TrackSample* prev, const TrackSample& a, const TrackSample& b,
                              const TrackSample* next, float alpha) noexcept {
    if (b.t_ns <= a.t_ns) return a.p;
    const double dt = double(b.t_ns - a.t_ns) * 1e-9;
    auto slope = [](const TrackSample& x, const TrackSample& y) -> Vector3 {
        const double s = double(y.t_ns - x.t_ns) * 1e-9;
        if (s <= 0.0) return { 0, 0, 0 };
        return { float((y.p.x - x.p.x) / s), float((y.p.y - x.p.y) / s), float((y.p.z - x.p.z) / s) };
    };
    const bool use_prev = prev && prev->t_ns < a.t_ns && double(a.t_ns - prev->t_ns) * 1e-9 <= 3.0 * dt;
    const bool use_next = next && next->t_ns > b.t_ns && double(next->t_ns - b.t_ns) * 1e-9 <= 3.0 * dt;
    const Vector3 m0 = use_prev ? slope(*prev, b) : slope(a, b);
    const Vector3 m1 = use_next ? slope(a, *next) : slope(a, b);

    const float t = alpha, t2 = t * t, t3 = t2 * t;
    const float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t;
    const float h01 = -2 * t3 + 3 * t2,    h11 = t3 - t2;
    const float d = static_cast<float>(dt);
    return { h00 * a.p.x + h10 * m0.x * d + h01 * b.p.x + h11 * m1.x * d,
             h00 * a.p.y + h10 * m0.y * d + h01 * b.p.y + h11 * m1.y * d,
             h00 * a.p.z + h10 * m0.z * d + h01 * b.p.z + h11 * m1.z * d };
}

} // namespace afteraction::ecs
