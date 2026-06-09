#pragma once
#include "../buffer/TelemetryStore.hpp"
#include "../network/Packet.hpp"
#include <raylib.h>
#include <array>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
//  ECS Components  —  registered with Flecs in Application::init_ecs()
//
//  Design notes:
//  - Plain aggregates only (no virtual, no inheritance).
//  - Raylib types (Vector3, Quaternion) used directly so the render system
//    can pass them to draw calls without conversion.
//  - The interpolated position/rotation components are what the render system
//    reads; the raw history is what the interpolation system writes.
// ─────────────────────────────────────────────────────────────────────────────

namespace debrief::ecs {

// ── Identity ──────────────────────────────────────────────────────────────────
struct EntityMeta {
    uint32_t  source_id  = 0;
    uint32_t  entity_id  = 0;
    uint16_t  type       = 0;   // net::EntityTypeId
    char      callsign[net::kCallsignLen]{};  // null-terminated, up to 31 chars
    bool      active     = true;
};

// ── Interpolated world-space state (written by InterpolationSystem) ───────────
struct Position    { Vector3 v{ 0, 0, 0 }; };
struct Rotation    { Quaternion q{ 0, 0, 0, 1 }; };
struct Velocity    { Vector3 v{ 0, 0, 0 }; };

// ── Raw telemetry history (written by Application when frames arrive) ─────────
//   Kept as a separate component so the interpolation system can be queried
//   independently of the render system.
struct HistoryComp { EntityStateHistory hist; };

// ── Visual ────────────────────────────────────────────────────────────────────
struct RenderModel {
    Model*     model_ptr = nullptr; // non-owning — AssetManager owns the Model
    float      scale     = 1.0f;
    Color      tint      = WHITE;
    Quaternion base_rot  = {0,0,0,1}; // model-space correction; see AssetManager
};

// ── Trail ribbon ──────────────────────────────────────────────────────────────
struct Trail {
    static constexpr uint32_t kMaxPoints = 512;
    std::array<Vector3, kMaxPoints> points{};
    uint32_t head  = 0;
    uint32_t count = 0;
    float    width = 2.0f;
    Color    color = { 0, 200, 255, 180 };

    void push(Vector3 p) noexcept {
        points[head % kMaxPoints] = p;
        ++head;
        if (count < kMaxPoints) ++count;
    }

    // Iterates oldest → newest.
    template<typename Fn>
    void for_each(Fn&& fn) const noexcept {
        if (count == 0) return;
        uint32_t start = (count < kMaxPoints) ? 0 : (head % kMaxPoints);
        for (uint32_t i = 0; i < count; ++i)
            fn(points[(start + i) % kMaxPoints]);
    }
};

// ── Selection ─────────────────────────────────────────────────────────────────
struct Selected {};   // tag component — entity is currently selected in UI

// ── Camera follow ─────────────────────────────────────────────────────────────
struct CameraTarget {};  // tag — camera tracks this entity

} // namespace debrief::ecs
