#include "Application.hpp"
#include "../ecs/Systems.hpp"
#include <rlImGui.h>
#include <imgui.h>
#include <implot.h>
#include <raymath.h>
#include <cmath>
#include <cstring>
#include <ctime>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace debrief {

// ─────────────────────────────────────────────────────────────────────────────
//  Coordinate helpers
// ─────────────────────────────────────────────────────────────────────────────

// WGS84 flat-earth approximation — accurate to <1 m within 500 km of origin.
static void lat_lon_to_enu(
    double lat, double lon, float alt,
    double lat0, double lon0, float alt0,
    float* x, float* y, float* z)
{
    constexpr double kMetPerDeg = 111319.9;
    const double cos_lat0 = std::cos(lat0 * M_PI / 180.0);
    *x = static_cast<float>((lon - lon0) * cos_lat0 * kMetPerDeg);  // East
    *y = alt - alt0;                                                  // Up
    *z = -static_cast<float>((lat - lat0) * kMetPerDeg);             // -North
}

// Aviation Euler → Raylib quaternion.
//   psi   = heading  (0=North, 90=East, clockwise)
//   theta = pitch    (+ = nose up)
//   phi   = roll     (+ = right bank)
//
// Convention in Raylib (right-handed Y-up):
//   heading rotates around  Y (positive angle rotates -Z toward +X)
//   pitch   rotates around  X (positive = nose up)
//   roll    rotates around -Z (positive = right bank)
//
// Note: if your loaded model appears flipped, negate psi_rad below.
static Quaternion euler_to_quat(float phi_deg, float theta_deg, float psi_deg)
{
    const float psi_rad   = -psi_deg   * DEG2RAD;  // neg: CW heading → CCW around Y
    const float theta_rad =  theta_deg * DEG2RAD;
    const float phi_rad   =  phi_deg   * DEG2RAD;

    // ZYX: heading first, then pitch, then roll (applied to body frame)
    Quaternion qy = QuaternionFromAxisAngle({0, 1, 0},  psi_rad);
    Quaternion qx = QuaternionFromAxisAngle({1, 0, 0},  theta_rad);
    Quaternion qz = QuaternionFromAxisAngle({0, 0, 1}, -phi_rad);   // -Z = forward axis
    return QuaternionMultiply(QuaternionMultiply(qy, qx), qz);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Application
// ─────────────────────────────────────────────────────────────────────────────

Application::Application(AppConfig cfg)
    : cfg_(std::move(cfg)), udp_receiver_(inbound_queue_) {}

Application::~Application() {
    if (!cfg_.demo_mode) udp_receiver_.stop();
    recorder_.stop();
    ImPlot::DestroyContext();
    rlImGuiShutdown();
    CloseWindow();
}

// ── Init ──────────────────────────────────────────────────────────────────────
void Application::init_window() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(cfg_.window_width, cfg_.window_height, cfg_.window_title.c_str());
    SetTargetFPS(cfg_.target_fps);
    rlImGuiSetup(true);
    ImPlot::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // don't write imgui.ini
}

void Application::init_ecs() {
    world_.component<ecs::EntityMeta>();
    world_.component<ecs::Position>();
    world_.component<ecs::Rotation>();
    world_.component<ecs::Velocity>();
    world_.component<ecs::HistoryComp>();
    world_.component<ecs::RenderModel>();
    world_.component<ecs::Trail>();
    world_.component<ecs::Selected>();
    world_.component<ecs::CameraTarget>();
    ecs::register_systems(world_);
}

void Application::init_assets() {
    // Procedural shapes are created in AssetManager constructor.
    // To load custom models:
    //   assets_.load("my_jet", "assets/models/jet.glb", 0.01f);
    //   assets_.map_type(net::TYPE_JET, "my_jet");
}

void Application::init_camera() {
    camera_.position   = { 0.0f, 8000.0f, 14000.0f };
    camera_.target     = { 0.0f, 0.0f, 0.0f };
    camera_.up         = { 0.0f, 1.0f, 0.0f };
    camera_.fovy       = 45.0f;
    camera_.projection = CAMERA_PERSPECTIVE;
}

void Application::init_ui_callbacks() {
    UICallbacks cbs;
    cbs.on_record_start = [this] {
        char fn[128];
        time_t t = time(nullptr);
        tm* lt   = localtime(&t);
        strftime(fn, sizeof(fn), "session_%Y%m%d_%H%M%S.dbr", lt);
        recorder_.start(fn, fn);
    };
    cbs.on_record_stop = [this] { recorder_.stop(); };
    cbs.on_save_dashcam = [this](float secs) {
        char fn[128];
        time_t t = time(nullptr);
        tm* lt   = localtime(&t);
        strftime(fn, sizeof(fn), "dashcam_%Y%m%d_%H%M%S.dbr", lt);
        persist::Recorder::export_slice(store_, secs, fn, "dashcam");
    };
    cbs.on_load_file = [this](std::string path) {
        persist::Recorder::load_into(path, store_);
        auto [ts, te] = store_.time_range_ns();
        playback_.seek(ts);
        playback_.pause();
    };
    cbs.on_load_model = [this](uint16_t type_id, std::string path) {
        if (assets_.load(path, path, 1.0f))
            assets_.map_type(type_id, path);
    };
    ui_.set_callbacks(std::move(cbs));
}

// ── Main loop ─────────────────────────────────────────────────────────────────
void Application::run() {
    init_window();
    init_ecs();
    init_assets();
    init_camera();
    init_ui_callbacks();

    if (!cfg_.demo_mode)
        udp_receiver_.start(cfg_.bind_addr, cfg_.udp_port);

    while (!WindowShouldClose() && running_) {
        const float dt = GetFrameTime();
        tick(dt);
    }
}

void Application::tick(float dt) {
    handle_input(dt);
    if (cfg_.demo_mode) process_demo(dt);
    else                process_inbound_queue();
    update_ecs(dt);
    render();
}

// ── Coordinate + orientation conversion ──────────────────────────────────────
void Application::ensure_origin_and_convert(net::EntityState& state, bool enu_already) {
    if (!enu_already) {
        if (!origin_set_) {
            origin_lat_ = state.lat_deg;
            origin_lon_ = state.lon_deg;
            origin_alt_ = state.alt_m;
            origin_set_ = true;
        }
        lat_lon_to_enu(state.lat_deg, state.lon_deg, state.alt_m,
                       origin_lat_,  origin_lon_,  origin_alt_,
                       &state.position[0], &state.position[1], &state.position[2]);
    }

    // Build quaternion from Euler angles (always, even for demo states)
    Quaternion q = euler_to_quat(state.phi_deg, state.theta_deg, state.psi_deg);
    state.orientation[0] = q.x;
    state.orientation[1] = q.y;
    state.orientation[2] = q.z;
    state.orientation[3] = q.w;

    // Velocity from speed + heading
    const float psi_r = -state.psi_deg * DEG2RAD;
    const float th_r  =  state.theta_deg * DEG2RAD;
    state.velocity[0] =  state.speed_mps * sinf(-psi_r) * cosf(th_r);
    state.velocity[1] =  state.speed_mps * sinf(th_r);
    state.velocity[2] = -state.speed_mps * cosf(-psi_r) * cosf(th_r);
}

// ── Live network ingestion ────────────────────────────────────────────────────
void Application::process_inbound_queue() {
    if (!playback_.is_live()) return;

    live_states_.clear();
    while (auto frame = inbound_queue_.try_pop()) {
        for (auto& es : frame->entities) {
            ensure_origin_and_convert(es, false);
            apply_state_to_ecs(es);
            live_states_.push_back(es);
        }
    }

    if (!live_states_.empty()) {
        uint64_t ts = live_states_.back().timestamp_ns;
        store_.ingest(ts, live_states_);
        world_.set<ecs::InterpolationCtx>({ts, true});
    }
}

// ── Demo data injection ───────────────────────────────────────────────────────
void Application::process_demo(float dt) {
    if (!playback_.is_live()) return;
    origin_set_ = true; // ENU origin is (0,0,0) in demo

    auto states = demo_.tick(dt);
    live_states_ = states;

    for (auto& es : live_states_) {
        ensure_origin_and_convert(es, true); // positions already ENU
        apply_state_to_ecs(es);
    }

    if (!live_states_.empty()) {
        uint64_t ts = live_states_.back().timestamp_ns;
        store_.ingest(ts, live_states_);
        world_.set<ecs::InterpolationCtx>({ts, true});

        if (recorder_.is_recording())
            recorder_.submit(ts, 0, live_states_);
    }
}

// ── ECS entity creation / update ─────────────────────────────────────────────
flecs::entity Application::get_or_create_entity(const net::EntityState& state) {
    uint64_t key = entity_key(state.source_id, state.entity_id);
    if (auto it = entity_map_.find(key); it != entity_map_.end())
        return it->second;

    char ename[32];
    snprintf(ename, sizeof(ename), "%s%u",
             state.callsign[0] ? state.callsign : "ent", state.entity_id);

    const auto* entry = assets_.get_for_type(state.entity_type);

    ecs::RenderModel rm{};
    rm.model_ptr = const_cast<Model*>(&entry->model); // AssetManager owns the model
    rm.tint      = entry->tint;
    rm.scale     = entry->scale;
    rm.base_rot  = entry->base_rot;

    auto e = world_.entity(ename)
        .set<ecs::EntityMeta>({state.source_id, state.entity_id,
                               state.entity_type,
                               {}, true})
        .set<ecs::Position>({})
        .set<ecs::Rotation>({})
        .set<ecs::Velocity>({})
        .set<ecs::HistoryComp>({})
        .set<ecs::Trail>({})
        .set<ecs::RenderModel>(rm);

    auto& meta = e.get_mut<ecs::EntityMeta>();
    std::memcpy(meta.callsign, state.callsign, 8);

    entity_map_.emplace(key, e);
    TraceLog(LOG_INFO, "Spawned entity '%s' type=%d", ename, state.entity_type);
    return e;
}

void Application::apply_state_to_ecs(net::EntityState& state) {
    auto e = get_or_create_entity(state);

    auto& meta = e.get_mut<ecs::EntityMeta>();
    if (state.destroyed) { meta.active = false; return; }
    meta.active = true;

    EntityStateHistory::Keyframe kf{};
    kf.timestamp_ns    = state.timestamp_ns;
    kf.position[0]     = state.position[0];
    kf.position[1]     = state.position[1];
    kf.position[2]     = state.position[2];
    kf.orientation[0]  = state.orientation[0];
    kf.orientation[1]  = state.orientation[1];
    kf.orientation[2]  = state.orientation[2];
    kf.orientation[3]  = state.orientation[3];
    kf.velocity[0]     = state.velocity[0];
    kf.velocity[1]     = state.velocity[1];
    kf.velocity[2]     = state.velocity[2];
    kf.health          = state.health;

    e.get_mut<ecs::HistoryComp>().hist.push(kf);
}

// ── ECS tick ──────────────────────────────────────────────────────────────────
void Application::update_ecs(float dt) {
    playback_.tick(dt);
    auto [ts, te] = store_.time_range_ns();
    if (ts) playback_.clamp(ts, te);
    if (!playback_.is_live())
        world_.set<ecs::InterpolationCtx>({playback_.current_time_ns(), false});
    world_.progress(dt);

    // Camera follow
    world_.query<const ecs::CameraTarget, const ecs::Position>()
        .each([&](const ecs::CameraTarget&, const ecs::Position& p) {
            camera_.target = p.v;
        });
}

// ── Rendering ─────────────────────────────────────────────────────────────────
void Application::render() {
    BeginDrawing();
    ClearBackground({12, 16, 22, 255});
    render_3d();
    render_ui();
    DrawFPS(GetScreenWidth() - 80, 4);
    EndDrawing();
}

void Application::render_3d() {
    BeginMode3D(camera_);
    DrawGrid(200, 500.0f);

    world_.query<const ecs::EntityMeta,
                  const ecs::Position,
                  const ecs::Rotation,
                  const ecs::RenderModel,
                  const ecs::Trail,
                  const ecs::Velocity>()
        .each([&](flecs::entity e,
                  const ecs::EntityMeta& meta,
                  const ecs::Position& pos,
                  const ecs::Rotation& rot,
                  const ecs::RenderModel& rm,
                  const ecs::Trail& trail,
                  const ecs::Velocity& vel)
        {
            if (!meta.active) return;

            // Compose base model rotation with entity world rotation
            Quaternion q = rm.model_ptr
                ? QuaternionMultiply(rot.q, rm.base_rot)
                : rot.q;

            Vector3 axis{ 0, 1, 0 };
            float angle_rad = 0.0f;
            QuaternionToAxisAngle(q, &axis, &angle_rad);

            if (rm.model_ptr)
                DrawModelEx(*rm.model_ptr, pos.v,
                            axis, angle_rad * RAD2DEG,
                            {rm.scale, rm.scale, rm.scale}, rm.tint);

            // Selection ring
            if (ui_.state().selected_entity == e)
                DrawSphereWires(pos.v, 60.0f, 6, 6, YELLOW);

            // Velocity vector (5-second look-ahead)
            if (ui_.state().show_velocity_vec) {
                Vector3 tip = Vector3Add(pos.v, Vector3Scale(vel.v, 5.0f));
                DrawLine3D(pos.v, tip, GREEN);
            }

            // Trail
            if (ui_.state().show_trails)
                trails_.draw(trail, camera_);
        });

    EndMode3D();

    // Screen-space labels
    if (ui_.state().show_labels) {
        world_.query<const ecs::EntityMeta, const ecs::Position>()
            .each([&](const ecs::EntityMeta& meta, const ecs::Position& pos) {
                if (!meta.active) return;
                Vector2 sp = GetWorldToScreen(pos.v, camera_);
                const char* lbl = meta.callsign[0]
                    ? meta.callsign
                    : TextFormat("#%u", meta.entity_id);
                DrawText(lbl,
                         static_cast<int>(sp.x) + 6,
                         static_cast<int>(sp.y) - 4,
                         14, {200, 230, 255, 220});
            });
    }

    // Demo mode banner
    if (cfg_.demo_mode) {
        DrawText("DEMO MODE  —  no UDP input",
                 GetScreenWidth() / 2 - 160, 8, 20,
                 {255, 200, 0, 200});
    }
}

void Application::render_ui() {
    rlImGuiBegin();
    ui_.draw(playback_, store_, udp_receiver_.stats(), recorder_, world_);
    rlImGuiEnd();
}

// ── Input ─────────────────────────────────────────────────────────────────────
void Application::handle_input(float /*dt*/) {
    if (IsKeyPressed(KEY_ESCAPE)) running_ = false;

    if (!ImGui::GetIO().WantCaptureMouse)
        UpdateCamera(&camera_, CAMERA_ORBITAL);

    if (IsKeyPressed(KEY_SPACE)) {
        if (playback_.state() == PlaybackState::Playing) playback_.pause();
        else                                              playback_.play();
    }

    if (IsKeyPressed(KEY_F) && ui_.state().selected_entity.is_valid()) {
        auto se = ui_.state().selected_entity;
        if (world_.is_alive(se)) {
            if (se.has<ecs::CameraTarget>()) se.remove<ecs::CameraTarget>();
            else                              se.add<ecs::CameraTarget>();
        }
    }

    if (IsKeyPressed(KEY_R)) {
        ui_.state().ribbon_trails = !ui_.state().ribbon_trails;
        trails_.mode = ui_.state().ribbon_trails ? TrailMode::Ribbon : TrailMode::Line;
    }

    // Hotkeys for playback speed
    if (IsKeyPressed(KEY_ONE))   playback_.set_speed(0.25f);
    if (IsKeyPressed(KEY_TWO))   playback_.set_speed(1.0f);
    if (IsKeyPressed(KEY_THREE)) playback_.set_speed(4.0f);
    if (IsKeyPressed(KEY_FOUR))  playback_.set_speed(-1.0f);
}

} // namespace debrief
