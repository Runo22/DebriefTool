#pragma once
#include <flecs.h>
#include <cstdint>

namespace afteraction::ecs {

// Registers all simulation systems into the given world.
// Call once during application init, before the first world.progress().
void register_systems(flecs::world& world);

// ── System parameter blocks ───────────────────────────────────────────────────
// These are passed via Flecs pipeline globals so systems can access app-level
// state without singletons polluting the component namespace.

struct InterpolationCtx {
    uint64_t playback_time_ns = 0;  // target time for interpolation
    bool     live_mode        = true; // if true, use "now"; if false, use playback_time_ns
};

struct TrailCtx {
    float sample_distance_sq = 4.0f; // minimum squared distance before appending a trail point
};

} // namespace afteraction::ecs
