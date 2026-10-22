#include "CameraController.hpp"
#include "../ecs/Components.hpp"
#include "../render/AssetManager.hpp"
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"   // raymath.h
#endif
#include <raymath.h>
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace afteraction {

namespace {

// Orbit offset for yaw/pitch (degrees) at a distance.
Vector3 orbit_offset(float yaw_deg, float pitch_deg, float dist) {
    const float y = yaw_deg * DEG2RAD, p = pitch_deg * DEG2RAD;
    return { dist * std::cos(p) * std::sin(y), dist * std::sin(p), dist * std::cos(p) * std::cos(y) };
}

bool usable(flecs::entity e) { return e.is_valid() && e.is_alive() && e.has<ecs::Position>(); }

} // namespace

void CameraController::update(Camera3D& cam, UIState& st, float dt) {
    const ImGuiIO& io = ImGui::GetIO();
    cam.fovy = std::clamp(st.camera_fov, 20.0f, 110.0f);
    cam.up   = { 0.0f, 1.0f, 0.0f };

    handle_mouse(st, io.WantCaptureMouse);
    handle_keys(st, io.WantTextInput, dt);

    flecs::entity sel = st.selected_entity;
    if (st.camera_mode != 0 && !usable(sel)) st.camera_mode = 0;   // track gone → free orbit

    if (st.camera_mode == 1) {
        // ── Focus: orbit around the selected track ────────────────────────────
        Vector3 p = sel.get<ecs::Position>().v;
        p.y *= st.altitude_exaggerate;
        cam.target   = p;
        cam.position = Vector3Add(p, orbit_offset(st.camera_yaw, st.camera_pitch, st.camera_distance));
    } else if (st.camera_mode == 2 && sel.has<ecs::Rotation>()) {
        // ── Chase: behind the track along its (smoothed) heading ──────────────
        // Only the horizontal heading is used, so banking and pitching don't
        // swing the camera; the position is then placed exactly relative to the
        // track (no spring lag, so nothing wobbles).
        Vector3 p = sel.get<ecs::Position>().v;
        p.y *= st.altitude_exaggerate;
        Vector3 fwd = Vector3RotateByQuaternion({ 0, 0, -1 }, sel.get<ecs::Rotation>().q);
        fwd.y = 0.0f;
        if (Vector3Length(fwd) > 1e-3f) fwd = Vector3Normalize(fwd);
        else                            fwd = chase_dir_;

        const ecs::RenderModel* rm = sel.try_get<ecs::RenderModel>();
        const float length = rm && rm->entry ? rm->entry->length : 20.0f;
        const float want_base = length * chase_lengths_;
        if (chase_entity_ != sel.id()) {
            chase_entity_ = sel.id();
            chase_dir_  = fwd;
            chase_base_ = want_base;
        }
        const float k_dir = 1.0f - std::exp(-2.5f * dt);
        chase_dir_  = Vector3Normalize(Vector3Lerp(chase_dir_, fwd, k_dir));
        chase_base_ += (want_base - chase_base_) * (1.0f - std::exp(-6.0f * dt));

        const Vector3 dir = Vector3RotateByQuaternion(
            chase_dir_, QuaternionFromAxisAngle({ 0, 1, 0 }, st.chase_yaw_offset * DEG2RAD));
        const float elev = std::clamp(st.chase_pitch_offset, -30.0f, 60.0f) * DEG2RAD;
        cam.position = Vector3Add(p, Vector3Add(Vector3Scale(dir, -chase_base_ * std::cos(elev)),
                                                { 0, chase_base_ * std::sin(elev), 0 }));
        // Look a little ahead so the model sits just below the centre.
        cam.target = Vector3Add(p, Vector3Scale(dir, chase_base_ * 0.3f));
    } else {
        // ── Free orbit around the pivot ───────────────────────────────────────
        if (flying_) {
            const float a = 1.0f - std::exp(-6.0f * dt);
            pivot_ = Vector3Lerp(pivot_, fly_target_, a);
            if (Vector3Distance(pivot_, fly_target_) < 1.0f) { pivot_ = fly_target_; flying_ = false; }
        }
        cam.target   = pivot_;
        cam.position = Vector3Add(pivot_, orbit_offset(st.camera_yaw, st.camera_pitch, st.camera_distance));
    }
    // Switching mode or track glides over ~0.7 s instead of cutting.
    const uint64_t sel_id = st.camera_mode != 0 && usable(sel) ? sel.id() : 0;
    if (st.camera_mode != last_mode_ || sel_id != last_sel_) {
        if (last_mode_ >= 0) { glide_from_pos_ = prev_pos_; glide_from_tgt_ = prev_tgt_; glide_t_ = 0.0f; }
        // Leaving a track view for free orbit: keep orbiting where we were looking.
        if (st.camera_mode == 0 && last_mode_ > 0) {
            set_pivot(prev_tgt_);
            cam.target   = pivot_;
            cam.position = Vector3Add(pivot_, orbit_offset(st.camera_yaw, st.camera_pitch, st.camera_distance));
        }
        last_mode_ = st.camera_mode;
        last_sel_  = sel_id;
        if (sel_id == 0) chase_entity_ = 0;
    }
    if (glide_t_ < 1.0f) {
        glide_t_ = std::min(1.0f, glide_t_ + dt / 0.7f);
        const float t = glide_t_ * glide_t_ * (3.0f - 2.0f * glide_t_);   // smoothstep
        cam.position = Vector3Lerp(glide_from_pos_, cam.position, t);
        cam.target   = Vector3Lerp(glide_from_tgt_, cam.target, t);
    }
    keep_above_ground(cam, st);
    prev_pos_ = cam.position;
    prev_tgt_ = cam.target;
}

void CameraController::handle_mouse(UIState& st, bool ui_has_mouse) {
    if (ui_has_mouse) return;

    // Wheel: exponential zoom — faster when far, finer when close.
    // In chase view the wheel sets the distance behind the track (in model lengths).
    if (const float wheel = GetMouseWheelMove(); wheel != 0.0f) {
        if (st.camera_mode == 2) chase_lengths_ = std::clamp(chase_lengths_ * std::pow(0.88f, wheel), 1.2f, 300.0f);
        else st.camera_distance = std::clamp(st.camera_distance * std::pow(0.88f, wheel), 30.0f, 900000.0f);
    }

    // RMB: orbit (free / focus) or swing around the track (chase). "Invert"
    // = grab the scene: the world moves with the cursor.
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        const Vector2 d = GetMouseDelta();
        const float inv = st.invert_look ? -1.0f : 1.0f;
        if (st.camera_mode == 2) {
            st.chase_yaw_offset   += inv * d.x * 0.3f * st.mouse_sensitivity;
            st.chase_pitch_offset  = std::clamp(st.chase_pitch_offset - inv * d.y * 0.2f * st.mouse_sensitivity,
                                                -30.0f, 60.0f);
        } else {
            const float sens = (0.20f + 0.05f * std::clamp(st.camera_distance / 20000.0f, 0.0f, 1.5f))
                               * st.mouse_sensitivity;
            st.camera_yaw  += inv * d.x * sens;
            st.camera_pitch = std::clamp(st.camera_pitch - inv * d.y * sens, -89.0f, 89.0f);
        }
    }

    // MMB: drag the ground plane (free orbit only), 1:1-ish with the zoom.
    if (st.camera_mode == 0 && IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
        const Vector2 d = GetMouseDelta();
        const float y = st.camera_yaw * DEG2RAD, k = st.camera_distance * 0.0012f;
        const Vector3 right{ std::cos(y), 0.0f, -std::sin(y) }, fwd{ std::sin(y), 0.0f, std::cos(y) };
        pivot_ = Vector3Add(Vector3Subtract(pivot_, Vector3Scale(right, d.x * k)), Vector3Scale(fwd, d.y * k));
        flying_ = false;
    }
}

void CameraController::handle_keys(UIState& st, bool typing, float dt) {
    if (typing) return;
    flecs::entity sel = st.selected_entity;

    // F: focus on / release the selected track (release keeps looking at it).
    if (IsKeyPressed(KEY_F) && usable(sel)) {
        if (st.camera_mode >= 1) {
            st.camera_mode = 0;
            Vector3 p = sel.get<ecs::Position>().v;
            p.y *= st.altitude_exaggerate;
            set_pivot(p);
        } else {
            st.camera_mode = 1;
        }
    }
    // G: chase camera on / off.
    if (IsKeyPressed(KEY_G) && usable(sel)) {
        st.camera_mode = st.camera_mode == 2 ? 0 : 2;
        if (st.camera_mode == 2) { st.chase_yaw_offset = 0.0f; st.chase_pitch_offset = 20.0f; }
    }

    // WASD / arrows: pan the free-orbit pivot, speed proportional to the zoom.
    if (st.camera_mode == 0) {
        float speed = st.camera_distance * 0.45f * dt;
        if (IsKeyDown(KEY_LEFT_SHIFT)) speed *= 4.0f;
        const float y = st.camera_yaw * DEG2RAD;
        const Vector3 fwd{ std::sin(y), 0.0f, std::cos(y) }, right{ std::cos(y), 0.0f, -std::sin(y) };
        Vector3 move{};
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))    move = Vector3Subtract(move, fwd);
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))  move = Vector3Add(move, fwd);
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))  move = Vector3Subtract(move, right);
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) move = Vector3Add(move, right);
        if (Vector3LengthSqr(move) > 0.0f) {
            pivot_ = Vector3Add(pivot_, Vector3Scale(move, speed));
            flying_ = false;
        }
    }
}

// Never let the camera dip below the terrain in any mode — otherwise you see
// through the ground (it clips against the near plane).
void CameraController::keep_above_ground(Camera3D& cam, const UIState& st) const {
    if (!ground_) return;
    const float g = ground_(cam.position.x, cam.position.z) * st.altitude_exaggerate;
    const float view_dist = st.camera_mode == 2 ? chase_base_ : st.camera_distance;
    const float min_y = g + std::max(st.camera_mode == 2 ? 2.0f : 5.0f, view_dist * 0.02f);
    if (cam.position.y < min_y) cam.position.y = min_y;
}

// When the very first track shows up and it's the ONLY active one, centre the
// free orbit on it with a sensible distance and a downward 3/4 view (a saved
// steep or upward pitch would otherwise frame it badly). One-shot.
void CameraController::auto_frame(UIState& st, const flecs::world& world) {
    if (auto_framed_ || st.camera_mode != 0) return;
    int active = 0;
    Vector3 p{};
    world.query<const ecs::EntityMeta, const ecs::Position>()
        .each([&](const ecs::EntityMeta& meta, const ecs::Position& pos) {
            if (meta.active) { ++active; p = pos.v; }
        });
    if (active == 0) return;          // nothing yet — keep waiting
    auto_framed_ = true;              // only at the first appearance
    if (active != 1) return;          // several at once — keep the current view
    set_pivot({ p.x, p.y * st.altitude_exaggerate, p.z });
    st.camera_distance = 3000.0f;
    st.camera_pitch    = 18.0f;
    TraceLog(LOG_INFO, "Auto-framed camera on first entity (alt %.0f m)", p.y);
}

} // namespace afteraction
