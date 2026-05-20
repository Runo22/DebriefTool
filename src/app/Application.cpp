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
    // Create detailed procedural composite models now that InitWindow() has initialized the GPU context
    assets_.init_procedural();

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

void Application::sync_ecs_to_playback(uint64_t target_ns) {
    // Get all unique entities in store_
    std::vector<uint64_t> keys = store_.get_all_entity_keys();

    for (uint64_t key : keys) {
        uint32_t source_id = static_cast<uint32_t>(key >> 32);
        uint32_t entity_id = static_cast<uint32_t>(key & 0xFFFFFFFF);

        const std::vector<net::EntityState>* hist = store_.get_entity_history(source_id, entity_id);
        if (!hist || hist->empty()) continue;

        // Find floor and ceiling brackets
        auto it_hi = std::lower_bound(hist->begin(), hist->end(), target_ns,
            [](const net::EntityState& es, uint64_t ts) {
                return es.timestamp_ns < ts;
            });

        // Outside active lifetime check (if target_ns is before first keyframe or after last keyframe + 2.0s)
        const auto& first_kf = hist->front();
        const auto& last_kf = hist->back();
        
        bool active = true;
        if (target_ns < first_kf.timestamp_ns || target_ns > last_kf.timestamp_ns + 2'000'000'000ULL) {
            active = false;
        }

        // Get or create the ECS entity using the closest keyframe for metadata
        net::EntityState state_to_use = (it_hi == hist->end()) ? last_kf : *it_hi;
        state_to_use.timestamp_ns = target_ns;

        flecs::entity e = get_or_create_entity(state_to_use);
        auto& meta = e.get_mut<ecs::EntityMeta>();

        if (!active || state_to_use.destroyed || state_to_use.health == 0) {
            meta.active = false;
            continue;
        }
        meta.active = true;

        // Perform interpolation
        Vector3 pos{};
        Quaternion rot{};
        Vector3 vel{};

        if (it_hi == hist->begin()) {
            const auto& k = *it_hi;
            pos = { k.position[0], k.position[1], k.position[2] };
            rot = { k.orientation[0], k.orientation[1], k.orientation[2], k.orientation[3] };
            vel = { k.velocity[0], k.velocity[1], k.velocity[2] };
        } else if (it_hi == hist->end()) {
            const auto& k = last_kf;
            pos = { k.position[0], k.position[1], k.position[2] };
            rot = { k.orientation[0], k.orientation[1], k.orientation[2], k.orientation[3] };
            vel = { k.velocity[0], k.velocity[1], k.velocity[2] };
        } else {
            const auto& khi = *it_hi;
            const auto& klo = *(it_hi - 1);

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

            Quaternion q0{klo.orientation[0], klo.orientation[1], klo.orientation[2], klo.orientation[3]};
            Quaternion q1{khi.orientation[0], khi.orientation[1], khi.orientation[2], khi.orientation[3]};

            // Hermite position interpolation
            const float t2 = alpha * alpha, t3 = t2 * alpha;
            const float h00 =  2*t3 - 3*t2 + 1;
            const float h10 =    t3 - 2*t2 + alpha;
            const float h01 = -2*t3 + 3*t2;
            const float h11 =    t3 -   t2;
            pos = {
                h00*p0.x + h10*(v0.x*dt_sec) + h01*p1.x + h11*(v1.x*dt_sec),
                h00*p0.y + h10*(v0.y*dt_sec) + h01*p1.y + h11*(v1.y*dt_sec),
                h00*p0.z + h10*(v0.z*dt_sec) + h01*p1.z + h11*(v1.z*dt_sec),
            };

            rot = QuaternionSlerp(q0, q1, alpha);
            vel = Vector3Lerp(v0, v1, alpha);
        }

        // Set ECS components directly
        e.set<ecs::Position>({pos});
        e.set<ecs::Rotation>({rot});
        e.set<ecs::Velocity>({vel});

        // Dynamic Trail Generation from history up to target_ns
        auto& trail = e.get_mut<ecs::Trail>();
        trail.head = 0;
        trail.count = 0;
        trail.points = {};

        // Find how many points are before target_ns in hist
        size_t last_idx = std::distance(hist->begin(), it_hi);
        
        // We will sample up to kMaxPoints points from hist[0...last_idx]
        if (last_idx > 0) {
            size_t start_idx = 0;
            if (last_idx > ecs::Trail::kMaxPoints) {
                start_idx = last_idx - ecs::Trail::kMaxPoints;
            }

            for (size_t idx = start_idx; idx < last_idx; ++idx) {
                const auto& frame = (*hist)[idx];
                trail.push({frame.position[0], frame.position[1], frame.position[2]});
            }
        }
        
        // Push current pos as trail tip
        trail.push(pos);
    }
}

// ── ECS tick ──────────────────────────────────────────────────────────────────
void Application::update_ecs(float dt) {
    playback_.tick(dt);
    auto [ts, te] = store_.time_range_ns();
    if (ts) playback_.clamp(ts, te);
    if (!playback_.is_live()) {
        sync_ecs_to_playback(playback_.current_time_ns());
        world_.set<ecs::InterpolationCtx>({playback_.current_time_ns(), false});
    }
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
            if (ui_.state().selected_entity == e) {
                DrawSphereWires(pos.v, 60.0f, 6, 6, YELLOW);
                
                // Tactical range rings directly on the ground plane under the selected entity
                DrawCircle3D({pos.v.x, 0.1f, pos.v.z}, 300.0f, {1, 0, 0}, 90.0f, {255, 220, 0, 150});
                DrawCircle3D({pos.v.x, 0.1f, pos.v.z}, 600.0f, {1, 0, 0}, 90.0f, {255, 220, 0, 75});
                DrawCircle3D({pos.v.x, 0.1f, pos.v.z}, 900.0f, {1, 0, 0}, 90.0f, {255, 220, 0, 30});
            }

            // Altitude projection lines for airborne units
            if (pos.v.y > 2.0f && (meta.type == net::TYPE_JET || meta.type == net::TYPE_MISSILE || meta.type == net::TYPE_HELO)) {
                // Draw dashed vertical line to the ground
                float step = 150.0f;
                for (float h = 0.0f; h < pos.v.y; h += step * 2.0f) {
                    float h_end = std::min(h + step, pos.v.y);
                    DrawLine3D({pos.v.x, h, pos.v.z}, {pos.v.x, h_end, pos.v.z}, {0, 190, 255, 140});
                }
                // Draw ground projection circle
                DrawCircle3D({pos.v.x, 0.1f, pos.v.z}, 80.0f, {1, 0, 0}, 90.0f, {0, 190, 255, 120});
            }

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
