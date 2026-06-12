#pragma once
#include "../ecs/Components.hpp"
#include <raylib.h>
#include <raymath.h>

namespace afteraction {

// ─────────────────────────────────────────────────────────────────────────────
//  Trail Renderer
//
//  Two rendering modes:
//
//  1. LINE mode  (default, cheap):  DrawLine3D between consecutive trail
//     points, alpha-faded toward the tail.  Great for most use cases.
//
//  2. RIBBON mode (high quality):  Constructs a camera-facing triangle strip
//     with constant screen-space width.  Produces the "exhaust plume" look.
//     Slightly more CPU-side work — 2 vertices per trail point.
//
//  The ribbon mode computes the ribbon plane normal as:
//    right = normalize(cross(segment_dir, to_camera))
//  Then offsets each point ±(width/2)*right to form the strip.
// ─────────────────────────────────────────────────────────────────────────────

enum class TrailMode { Line, Ribbon };

class TrailRenderer {
public:
    TrailMode mode = TrailMode::Line;

    void draw(const ecs::Trail& trail, const Camera3D& camera, float width_override = -1.0f) const noexcept {
        if (trail.count < 2) return;

        if (mode == TrailMode::Line)   draw_line(trail);
        else                            draw_ribbon(trail, camera, width_override);
    }

private:
    void draw_line(const ecs::Trail& trail) const noexcept {
        Vector3 prev{};
        bool    first = true;
        uint32_t idx  = 0;

        trail.for_each([&](Vector3 p) {
            float alpha_f = (trail.count > 1)
                ? static_cast<float>(idx) / static_cast<float>(trail.count - 1)
                : 1.0f;
            ++idx;
            if (first) { prev = p; first = false; return; }

            Color c = trail.color;
            c.a = static_cast<unsigned char>(trail.color.a * alpha_f);
            DrawLine3D(prev, p, c);
            prev = p;
        });
    }

    void draw_ribbon(const ecs::Trail& trail, const Camera3D& camera, float width_override = -1.0f) const noexcept {
        // Build a flat list of points (ordered oldest→newest).
        std::vector<Vector3> pts;
        pts.reserve(trail.count);
        trail.for_each([&](Vector3 p) { pts.push_back(p); });

        if (pts.size() < 2) return;

        Vector3 cam_pos = camera.position;
        float w = (width_override > 0.0f) ? width_override : trail.width;
        float hw = w * 0.5f;

        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            Vector3 dir   = Vector3Normalize(Vector3Subtract(pts[i+1], pts[i]));
            Vector3 to_cam= Vector3Normalize(Vector3Subtract(cam_pos, pts[i]));
            Vector3 right = Vector3Normalize(Vector3CrossProduct(dir, to_cam));

            float t = static_cast<float>(i) / static_cast<float>(pts.size() - 1);
            Color c = trail.color;
            c.a = static_cast<unsigned char>(trail.color.a * t);

            Vector3 a0 = Vector3Add(pts[i],   Vector3Scale(right,  hw));
            Vector3 a1 = Vector3Add(pts[i],   Vector3Scale(right, -hw));
            Vector3 b0 = Vector3Add(pts[i+1], Vector3Scale(right,  hw));
            Vector3 b1 = Vector3Add(pts[i+1], Vector3Scale(right, -hw));

            DrawTriangle3D(a0, b0, a1, c);
            DrawTriangle3D(a1, b0, b1, c);
        }
    }
};

} // namespace afteraction
