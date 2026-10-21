#pragma once
#include "../ui/AfterActionUI.hpp"
#include <flecs.h>
#include <raylib.h>
#include <functional>

namespace afteraction {

// ─────────────────────────────────────────────────────────────────────────────
//  CameraController — mouse/keyboard camera with three modes (UIState::camera_mode):
//    0 Free orbit   around a pivot that WASD / middle-drag pans
//    1 Focus        orbit around the selected track
//    2 Chase        spring-follow behind the selected track
//
//  Yaw, pitch and distance live in UIState (so they persist and the UI can edit
//  them); this class owns the free-orbit pivot and the "fly to" animation.
//  All positions are in render space (Y already multiplied by the vertical scale).
// ─────────────────────────────────────────────────────────────────────────────
class CameraController {
public:
    // Terrain height (m MSL, real scale) under a world (x, z).
    using GroundFn = std::function<float(float, float)>;

    void set_ground(GroundFn fn) { ground_ = std::move(fn); }

    // Read input and place `cam` for this frame.
    void update(Camera3D& cam, UIState& st, float dt);

    [[nodiscard]] Vector3 pivot() const noexcept { return pivot_; }
    void set_pivot(Vector3 p) { pivot_ = p; flying_ = false; }
    // Glide the free-orbit pivot to `p` over a fraction of a second.
    void fly_to(Vector3 p) { fly_target_ = p; flying_ = true; }

    // Centre on the first track when it is the only one (once; reset on Clear).
    void auto_frame(UIState& st, const flecs::world& world);
    void reset_auto_frame() { auto_framed_ = false; }

private:
    void handle_mouse(UIState& st, bool ui_has_mouse);
    void handle_keys(UIState& st, bool typing, float dt);
    void keep_above_ground(Camera3D& cam, const UIState& st) const;

    GroundFn ground_;
    Vector3  pivot_{ 0.0f, 1500.0f, 0.0f };
    Vector3  fly_target_{};
    bool     flying_ = false;
    bool     auto_framed_ = false;
};

} // namespace afteraction
