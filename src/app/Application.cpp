#include "Application.hpp"
#include "../ecs/Systems.hpp"
#include <rlImGui.h>
#include <imgui.h>
#include <implot.h>
#include <raymath.h>
#include <rlgl.h>
#include <cmath>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <initializer_list>
#include <vector>
#include "Config.hpp"
#include "../persistence/CsvImporter.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace debrief {

// ─────────────────────────────────────────────────────────────────────────────
//  Coordinate helpers
// ─────────────────────────────────────────────────────────────────────────────

// WGS84 flat-earth approximation — accurate to <1 m within 500 km of origin.
//
// Horizontal (East/North) is measured relative to the scene origin so the scene
// stays near the world origin for float precision. Altitude (Up) is kept as the
// ABSOLUTE metres-above-sea-level value, NOT relative to the first entity — this
// way every entity reports its true altitude regardless of which one arrived
// first (previously the first entity defined alt0, so it always read 0 m and
// later entities read alt-relative-to-it).
static void lat_lon_to_enu(
    double lat, double lon, double alt,
    double lat0, double lon0, double /*alt0*/,
    float* x, float* y, float* z)
{
    constexpr double kMetPerDeg = 111319.9;
    const double cos_lat0 = std::cos(lat0 * M_PI / 180.0);
    *x = static_cast<float>((lon - lon0) * cos_lat0 * kMetPerDeg);  // East
    *y = static_cast<float>(alt);                                   // Up (absolute MSL)
    *z = -static_cast<float>((lat - lat0) * kMetPerDeg);            // -North
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
    ConfigManager::save_config(ui_.state(), "debrief_config.yaml");
    // Free GL resources (models/meshes) while the GL context is still alive.
    // The AssetManager member destructor runs after this body — i.e. after
    // CloseWindow() has destroyed the context — so unloading there would make GL
    // calls on a dead context and crash. unload_all() is idempotent.
    assets_.unload_all();
    ImPlot::DestroyContext();
    rlImGuiShutdown();
    CloseWindow();
}

// ── Init ──────────────────────────────────────────────────────────────────────
void Application::init_window() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(cfg_.window_width, cfg_.window_height, cfg_.window_title.c_str());
    SetTargetFPS(cfg_.target_fps);

    // Window / taskbar icon — prefer the rounded, transparent-corner variant;
    // fall back to the square one. GLFW copies the pixels on SetWindowIcon, so
    // the Image can be unloaded immediately. Must be RGBA8 for raylib.
    Image icon = LoadImage("assets/icon_rounded.png");
    if (!icon.data) icon = LoadImage("assets/icon.png");
    if (icon.data) {
        ImageFormat(&icon, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        SetWindowIcon(icon);
        UnloadImage(icon);
    }

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
    // Entities in demo orbit at 3000-5500m altitude, 5-8km radius.
    // Start with a side-angle view that clearly shows altitude differences.
    camera_.position   = { 0.0f, 5000.0f, 12000.0f };
    camera_.target     = { 0.0f, 3000.0f, 0.0f };   // look at entity centroid
    camera_.up         = { 0.0f, 1.0f,   0.0f };
    camera_.fovy       = 60.0f;
    camera_.projection = CAMERA_PERSPECTIVE;
    camera_free_target_ = { 0.0f, 3000.0f, 0.0f };

    // Default initial settings before loading config
    ui_.state().camera_yaw      = 0.0f;
    ui_.state().camera_pitch    = 22.0f;   // angled to show height
    ui_.state().camera_distance = 12000.0f;
    ui_.state().entity_3d_scale = 30.0f;
    ui_.state().trail_width_override = 120.0f;
    ui_.state().terrain_mode    = 3;       // Both
    ui_.state().altitude_exaggerate = 3.0f;  // 3x vertical exaggeration
    ui_.state().far_clip_plane  = 2000000.0f;

    // Load config
    ConfigManager::load_config(ui_.state(), "debrief_config.yaml");
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
        store_.clear();
        persist::Recorder::load_into(path, store_);
        auto [ts, te] = store_.time_range_ns();
        playback_.seek(ts);
        playback_.pause();
    };
    cbs.on_load_csv = [this](std::string path) {
        // Flexible importer: map the column names commonly emitted by sim/ACMI
        // exporters. Only columns actually present in the file are used.
        persist::CsvImporter imp;
        using F = persist::CsvField;
        auto m = [&](std::initializer_list<const char*> names, F f) {
            for (const char* n : names) imp.map(n, f);
        };
        m({"time", "timestamp", "t", "sec", "seconds"},   F::TimestampSec);
        m({"time_ms", "ms"},                               F::TimestampMs);
        m({"time_ns", "ns"},                               F::TimestampNs);
        m({"id", "entity_id", "entityid"},                 F::EntityId);
        m({"type", "entity_type"},                         F::EntityType);
        m({"source", "source_id"},                         F::SourceId);
        m({"callsign", "name"},                            F::Callsign);
        m({"x", "pos_x", "east", "posx"},                  F::PosX);
        m({"y", "alt", "altitude", "up", "posy"},          F::PosY);
        m({"z", "pos_z", "north", "posz"},                 F::PosZ);
        m({"vx", "vel_x"}, F::VelX);  m({"vy", "vel_y"}, F::VelY);  m({"vz", "vel_z"}, F::VelZ);
        m({"yaw", "heading", "psi"},                       F::YawDeg);
        m({"pitch", "theta"},                              F::PitchDeg);
        m({"roll", "phi"},                                 F::RollDeg);
        m({"health", "hp"},                                F::Health);

        std::vector<net::EntityState> states = imp.import_all(path);
        if (states.empty()) return;

        // Group rows by timestamp into per-frame snapshots, in ascending order.
        std::stable_sort(states.begin(), states.end(),
            [](const net::EntityState& a, const net::EntityState& b) {
                return a.timestamp_ns < b.timestamp_ns;
            });

        store_.clear();
        std::vector<net::EntityState> frame;
        uint64_t frame_ts = states.front().timestamp_ns;
        auto flush = [&] {
            if (!frame.empty()) store_.ingest(frame_ts, frame);
            frame.clear();
        };
        for (auto& s : states) {
            if (s.timestamp_ns != frame_ts) { flush(); frame_ts = s.timestamp_ns; }
            frame.push_back(s);
        }
        flush();

        auto [ts, te] = store_.time_range_ns();
        playback_.seek(ts);
        playback_.pause();
    };
    cbs.on_load_model = [this](uint16_t type_id, std::string path) {
        if (assets_.load(path, path, 1.0f))
            assets_.map_type(type_id, path);
    };
    cbs.on_clear_entities = [this] { clear_requested_ = true; };
    cbs.on_apply_network  = [this](std::string addr, uint16_t port) {
        apply_network_settings(addr, port);
    };
    ui_.set_callbacks(std::move(cbs));

    // Seed the network panel fields from the active configuration.
    ui_.state().listen_port = cfg_.udp_port;
    snprintf(ui_.state().bind_addr, sizeof(ui_.state().bind_addr), "%s",
             cfg_.bind_addr.c_str());
}

void Application::clear_all_entities() {
    // Destruct every spawned entity from our own handle map — NOT from inside a
    // flecs query each(): flecs locks the matched table during iteration, and
    // deleting from a locked table aborts in flecs_table_delete. Wrap the deletes
    // in defer so they're queued and flushed at a safe (unlocked) point.
    world_.defer_begin();
    for (auto& [key, e] : entity_map_)
        if (e.is_valid() && world_.is_alive(e)) e.destruct();
    world_.defer_end();

    entity_map_.clear();
    store_.clear();
    live_states_.clear();
    ui_.state().selected_entity = {};
    origin_set_ = false;
    playback_.stop();   // return to live mode
    TraceLog(LOG_INFO, "Cleared all entities and telemetry store");
}

void Application::apply_network_settings(const std::string& bind_addr, uint16_t port) {
    if (cfg_.demo_mode) return;   // demo has no live socket
    udp_receiver_.stop();
    cfg_.bind_addr = bind_addr;
    cfg_.udp_port  = port;
    if (udp_receiver_.start(cfg_.bind_addr, cfg_.udp_port))
        TraceLog(LOG_INFO, "UDP receiver now listening on %s:%u",
                 cfg_.bind_addr.c_str(), cfg_.udp_port);
    else
        TraceLog(LOG_WARNING, "Failed to bind UDP %s:%u",
                 cfg_.bind_addr.c_str(), cfg_.udp_port);
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
    // Run any deferred full clear here, before ECS iteration / rendering, so we
    // never destruct entities from inside a query or the ImGui draw call stack.
    if (clear_requested_) {
        clear_requested_ = false;
        clear_all_entities();
    }

    handle_input(dt);
    if (cfg_.demo_mode) process_demo(dt);
    else                process_inbound_queue();
    update_ecs(dt);
    
    // Sync checkbox state for trail renderer
    trails_.mode = ui_.state().ribbon_trails ? TrailMode::Ribbon : TrailMode::Line;
    
    update_camera_state(dt);
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

    char ename[64];
    snprintf(ename, sizeof(ename), "%s%u",
             state.callsign[0] ? state.callsign : "ent", state.entity_id);

    const auto* entry = assets_.get_for_type(state.entity_type);

    ecs::RenderModel rm{};
    rm.model_ptr = const_cast<Model*>(&entry->model); // AssetManager owns the model
    rm.tint      = entry->tint;
    rm.scale     = entry->scale;
    rm.base_rot  = entry->base_rot;

    // Per-type trail color for visual identification
    ecs::Trail trail_init{};
    trail_init.width = 10.0f;
    switch (state.entity_type) {
        case net::TYPE_JET:     trail_init.color = { 80, 160, 255, 200 }; break; // blue
        case net::TYPE_MISSILE: trail_init.color = { 255, 80,  80, 220 }; break; // red-orange
        case net::TYPE_AAA:     trail_init.color = { 80, 180,  80, 180 }; break; // green
        case net::TYPE_GROUND:  trail_init.color = {220, 140,  40, 180 }; break; // amber
        case net::TYPE_HELO:    trail_init.color = { 80, 220, 100, 200 }; break; // lime
        case net::TYPE_SHIP:    trail_init.color = {160, 160, 220, 180 }; break; // lavender
        default:                trail_init.color = {  0, 200, 255, 180 }; break; // cyan
    }

    auto e = world_.entity(ename)
        .set<ecs::EntityMeta>({state.source_id, state.entity_id,
                               state.entity_type,
                               {}, true})
        .set<ecs::Position>({})
        .set<ecs::Rotation>({})
        .set<ecs::Velocity>({})
        .set<ecs::HistoryComp>({})
        .set<ecs::Trail>(trail_init)
        .set<ecs::RenderModel>(rm);

    auto& meta = e.get_mut<ecs::EntityMeta>();
    std::memcpy(meta.callsign, state.callsign, net::kCallsignLen);
    meta.callsign[net::kCallsignLen - 1] = '\0';

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
// Atmospheric palette — the horizon colour is reused as the fog target so
// distant terrain dissolves into the sky instead of hard-cutting.
static constexpr Color kSkyZenith  = {  8, 12, 22, 255 };  // deep space, top of screen
static constexpr Color kSkyHorizon = { 32, 44, 62, 255 };  // hazy blue-grey at the horizon

void Application::render() {
    BeginDrawing();
    // ClearBackground also clears the depth buffer; the gradient then paints
    // over the colour buffer to give a sky/horizon backdrop with real depth.
    ClearBackground(kSkyHorizon);
    DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(),
                           kSkyZenith, kSkyHorizon);
    render_3d();
    render_ui();
    DrawFPS(GetScreenWidth() - 80, 4);
    EndDrawing();
}

void Application::render_3d() {
    // Depth precision scales with the near/far ratio. A fixed 1m near plane
    // against a 2000km far plane gives a ratio of ~2,000,000 which destroys
    // depth precision and causes z-fighting / wireframe flicker at far zoom.
    // Scale the near plane with camera distance so the ratio stays sane while
    // still letting close-up views keep a tight near plane.
    const float cam_dist   = ui_.state().camera_distance;
    const float near_plane = std::clamp(cam_dist * 0.01f, 1.0f, 2000.0f);
    const float far_plane  = std::max(ui_.state().far_clip_plane, cam_dist * 4.0f);
    rlSetClipPlanes(near_plane, far_plane);
    BeginMode3D(camera_);

    const float exag = ui_.state().altitude_exaggerate;

    // Helper: apply altitude exaggeration to a world-space position
    auto exag_pos = [&](Vector3 p) -> Vector3 {
        return { p.x, p.y * exag, p.z };
    };

    // 1. Terrain (drawn at real scale — it's already near ground level)
    if (ui_.state().terrain_mode > 0) {
        draw_terrain();
    } else {
        // Fallback tactical grid
        rlBegin(RL_LINES);
        rlColor4ub(0, 80, 150, 50);
        for (int i = -50; i <= 50; i += 5) {
            rlVertex3f((float)i * 1000.f, 0.f, -50000.f);
            rlVertex3f((float)i * 1000.f, 0.f,  50000.f);
            rlVertex3f(-50000.f, 0.f, (float)i * 1000.f);
            rlVertex3f( 50000.f, 0.f, (float)i * 1000.f);
        }
        rlEnd();
    }

    // 2. Entities
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

            // Apply altitude exaggeration to render position
            Vector3 rp = exag_pos(pos.v);  // rendered position (exaggerated Y)

            Quaternion q = rm.model_ptr
                ? QuaternionMultiply(rot.q, rm.base_rot) : rot.q;
            Vector3 axis{ 0, 1, 0 };
            float angle_rad = 0.0f;
            QuaternionToAxisAngle(q, &axis, &angle_rad);

            float final_scale = rm.scale * ui_.state().entity_3d_scale;
            if (rm.model_ptr) {
                DrawModelEx(*rm.model_ptr, rp,
                            axis, angle_rad * RAD2DEG,
                            {final_scale, final_scale, final_scale}, rm.tint);
                DrawModelWiresEx(*rm.model_ptr, rp,
                                 axis, angle_rad * RAD2DEG,
                                 {final_scale, final_scale, final_scale},
                                 ColorAlpha(rm.tint, 0.35f));
            } else {
                float sz = 60.0f * ui_.state().entity_3d_scale;
                DrawCylinder(rp, 0.0f, sz, sz * 1.5f, 4, {0, 220, 255, 255});
                DrawCylinderWires(rp, 0.0f, sz, sz * 1.5f, 4, {200, 240, 255, 200});
            }

            // Ground height directly beneath this entity (so markers/drop-lines
            // track terrain relief instead of sinking into hills).
            const float gh = terrain_height_at(rp.x, rp.z);

            // Selection highlight (using exaggerated position rp).
            // Deliberately does NOT draw a wire sphere around the model — that
            // obscured the body and made rotation hard to read. Instead we use a
            // floating chevron above the entity plus ground rings on the terrain,
            // both of which leave the model fully visible.
            if (ui_.state().selected_entity == e) {
                float sc    = ui_.state().entity_3d_scale;
                float mk    = std::max(60.0f, 180.0f * sc * 0.05f);   // marker size

                // Floating chevron (diamond) hovering above the model.
                float bob  = sinf((float)GetTime() * 3.0f) * mk * 0.3f;
                Vector3 dp = {rp.x, rp.y + mk * 2.5f + bob, rp.z};
                float ds   = mk * 0.6f;
                DrawCylinderWires(dp, 0.f, ds, ds*1.4f, 4, YELLOW);
                DrawCylinderWires({dp.x, dp.y - ds*1.4f, dp.z}, ds, 0.f, ds*1.4f, 4, YELLOW);

                // Bright vertical tie-line from the chevron down to the terrain.
                DrawLine3D({rp.x, gh, rp.z}, dp, {255, 220, 0, 130});

                // Ground rings on the terrain surface (visible over relief).
                DrawCircle3D({rp.x, gh + 4.f, rp.z}, 600.f,  {1,0,0}, 90.f, {255,220,0,160});
                DrawCircle3D({rp.x, gh + 4.f, rp.z}, 1200.f, {1,0,0}, 90.f, {255,220,0,80});
                DrawCircle3D({rp.x, gh + 4.f, rp.z}, 2400.f, {1,0,0}, 90.f, {255,220,0,35});
            }

            // Altitude drop line from the terrain up to the entity position.
            if (rp.y - gh > 10.0f) {
                DrawLine3D({rp.x, gh, rp.z}, rp, {0, 190, 255, 160});
                float sr = std::max(50.f, (pos.v.y - gh) * 0.03f);
                DrawCircle3D({rp.x, gh + 2.f, rp.z}, sr, {1,0,0}, 90.f, {0, 190, 255, 100});
            }

            // Velocity vector (in exaggerated space)
            if (ui_.state().show_velocity_vec) {
                Vector3 ve  = { vel.v.x, vel.v.y * exag, vel.v.z };
                Vector3 tip = Vector3Add(rp, Vector3Scale(ve, 5.0f));
                DrawLine3D(rp, tip, {0, 255, 80, 200});
                DrawSphere(tip, final_scale * 0.5f, {0, 255, 80, 180});
            }

            // Trail — copy ring buffer with exaggerated Y
            if (ui_.state().show_trails && trail.count >= 2) {
                ecs::Trail et = trail;
                for (uint32_t ti = 0; ti < ecs::Trail::kMaxPoints; ++ti)
                    et.points[ti].y = trail.points[ti].y * exag;
                trails_.draw(et, camera_, ui_.state().trail_width_override);
            }
        });

    EndMode3D();
    rlSetClipPlanes(0.01f, 1000.0f);

    // Screen-space labels (using exaggerated Y for GetWorldToScreen)
    if (ui_.state().show_labels) {
        Vector3 cam_fwd = Vector3Normalize(
            Vector3Subtract(camera_.target, camera_.position));
        world_.query<const ecs::EntityMeta, const ecs::Position>()
            .each([&](flecs::entity fe, const ecs::EntityMeta& meta, const ecs::Position& pos) {
                if (!meta.active) return;
                Vector3 rp2 = { pos.v.x, pos.v.y * exag, pos.v.z };

                // Cull anything behind the camera — GetWorldToScreen otherwise
                // projects rear points back onto screen as ghost labels.
                Vector3 to_ent = Vector3Subtract(rp2, camera_.position);
                if (Vector3DotProduct(to_ent, cam_fwd) <= 0.0f) return;

                Vector2 sp = GetWorldToScreen(rp2, camera_);
                if (sp.x < -64 || sp.x > GetScreenWidth() + 64 ||
                    sp.y < -32 || sp.y > GetScreenHeight() + 32) return;

                bool sel = (ui_.state().selected_entity == fe);

                // Distance fade — far labels dim out so clusters stay readable.
                // The selected entity always stays fully opaque.
                float dist = Vector3Length(to_ent);
                float fade = 1.0f - std::clamp(
                    (dist - cam_dist * 2.0f) / (cam_dist * 4.0f), 0.0f, 1.0f);
                if (sel) fade = 1.0f;
                if (fade < 0.08f) return;
                unsigned char a = (unsigned char)(fade * 255.0f);

                const char* lbl = meta.callsign[0]
                    ? meta.callsign : TextFormat("#%u", meta.entity_id);
                // Altitude shown in feet (primary), with metres in parentheses.
                int alt_ft = (int)(pos.v.y * 3.28084f);
                int alt_m  = (int)(pos.v.y);
                const char* alt = (alt_ft > 50) ? TextFormat("%d ft (%d m)", alt_ft, alt_m)
                                                : nullptr;

                const int fs = 15, afs = 12;
                int tx = (int)sp.x + 9, ty = (int)sp.y - 8;
                int wlbl = MeasureText(lbl, fs);
                int walt = alt ? MeasureText(alt, afs) : 0;
                int w = std::max(wlbl, walt);
                int h = fs + (alt ? afs + 2 : 0);

                // Background plate keeps text legible over bright/busy terrain.
                DrawRectangle(tx - 4, ty - 3, w + 8, h + 6,
                              { 6, 10, 18, (unsigned char)(a * 0.62f) });
                DrawRectangleLines(tx - 4, ty - 3, w + 8, h + 6,
                              sel ? Color{ 255, 220, 0, a }
                                  : Color{ 40, 90, 120, (unsigned char)(a * 0.7f) });

                DrawText(lbl, tx, ty, fs,
                         sel ? Color{ 255, 220, 0, a } : Color{ 200, 228, 255, a });
                if (alt)
                    DrawText(alt, tx, ty + fs + 2, afs,
                             { 0, 200, 255, (unsigned char)(a * 0.85f) });
            });
    }

    if (cfg_.demo_mode) {
        // Below the toolbar row so it never overlaps the transport buttons.
        int bw = 340, bh = 26, bx = GetScreenWidth()/2 - 170, by = 44;
        DrawRectangle(bx, by, bw, bh, {0,0,0,160});
        DrawRectangleLines(bx, by, bw, bh, {255,180,0,100});
        DrawText("DEMO MODE  \xe2\x80\x94  Live UDP Disabled", bx+10, by+5, 15, {255,200,0,230});
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

    if (IsKeyPressed(KEY_SPACE)) {
        if (playback_.state() == PlaybackState::Playing) playback_.pause();
        else                                              playback_.play();
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

    // ── Left-click entity selection in 3D viewport ────────────────────────────
    // Only when ImGui is not capturing the mouse
    if (!ImGui::GetIO().WantCaptureMouse && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        Ray ray = GetMouseRay(GetMousePosition(), camera_);
        float best_dist = 1e10f;
        flecs::entity best_ent{};
        const float exag = ui_.state().altitude_exaggerate;

        world_.query<const ecs::EntityMeta, const ecs::Position>()
            .each([&](flecs::entity e, const ecs::EntityMeta& meta, const ecs::Position& pos) {
                if (!meta.active) return;
                // Hit-test against the RENDERED position (altitude exaggeration
                // applied) — otherwise clicks miss whenever exaggeration > 1.
                Vector3 rp = { pos.v.x, pos.v.y * exag, pos.v.z };
                float pick_r = ui_.state().entity_3d_scale * 30.0f;
                Vector3 to_center = Vector3Subtract(rp, ray.position);
                float t = Vector3DotProduct(to_center, ray.direction);
                if (t < 0) return;
                Vector3 closest = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
                float dist = Vector3Length(Vector3Subtract(closest, rp));
                if (dist < pick_r && t < best_dist) {
                    best_dist = t;
                    best_ent  = e;
                }
            });

        if (best_ent.is_valid()) {
            // Toggle selection
            ui_.state().selected_entity = (ui_.state().selected_entity == best_ent)
                ? flecs::entity{} : best_ent;
        }
    }
}

float Application::terrain_height_at(float wx, float wz) const {
    const auto& state = ui_.state();
    if (state.terrain_mode == 0) return 0.0f;
    float h = 800.0f * sinf(wx * 0.00006f) * cosf(wz * 0.00006f)
            + 350.0f * sinf(wx * 0.00015f + 0.8f) * sinf(wz * 0.00012f + 1.2f)
            + 120.0f * cosf(wx * 0.0004f + 2.0f)  * cosf(wz * 0.0003f)
            +  40.0f * sinf(wx * 0.001f)           * sinf(wz * 0.001f);
    return h * state.terrain_height_scale;
}

void Application::draw_terrain() {
    auto& state = ui_.state();
    if (state.terrain_mode == 0) return;

    // ── LOD: adapt grid resolution to camera distance, with geomorphing ───────
    // Detail halves at each band boundary (step 1→2→4→8). To kill the visible
    // "pop" at a boundary, the fine-grid vertices that vanish at the next coarser
    // level are smoothly morphed toward the coarse surface over the upper half of
    // each band — so by the time the step actually doubles the meshes already
    // coincide (classic geomipmap geomorphing).
    const float cam_dist = state.camera_distance;
    int   lod_step = 8;
    float band_lo = 25000.0f, band_hi = 0.0f;   // band_hi==0 ⇒ coarsest, no morph
    if      (cam_dist < 3000.0f)  { lod_step = 1; band_lo = 0.0f;     band_hi = 3000.0f;  }
    else if (cam_dist < 10000.0f) { lod_step = 2; band_lo = 3000.0f;  band_hi = 10000.0f; }
    else if (cam_dist < 25000.0f) { lod_step = 4; band_lo = 10000.0f; band_hi = 25000.0f; }

    float morph = 0.0f;
    if (band_hi > 0.0f) {
        float morph_start = band_lo + (band_hi - band_lo) * 0.5f;
        morph = std::clamp((cam_dist - morph_start) / (band_hi - morph_start), 0.0f, 1.0f);
    }

    constexpr int   FULL_GRID = 80;            // 80 columns/rows at all LODs
    constexpr float BASE_TILE = 1000.0f;       // 1 km base tile
    float tile_sz = BASE_TILE * lod_step;
    int   grid_n  = FULL_GRID;                // Number of tiles is constant, so physical width expands with LOD

    // Infinite terrain: center on the camera, snapped to 2× tile size so that a
    // vertex's even/odd parity matches the next-coarser grid (needed for morphing).
    float snap = tile_sz * 2.0f;
    float cam_grid_x = roundf(camera_.position.x / snap) * snap;
    float cam_grid_z = roundf(camera_.position.z / snap) * snap;
    float start_x = cam_grid_x - (grid_n * tile_sz) * 0.5f;
    float start_z = cam_grid_z - (grid_n * tile_sz) * 0.5f;

    // Raw procedural height (world-space coords)
    auto base_height = [&](float wx, float wz) -> float {
        float h = 800.0f * sinf(wx * 0.00006f) * cosf(wz * 0.00006f)
                + 350.0f * sinf(wx * 0.00015f + 0.8f) * sinf(wz * 0.00012f + 1.2f)
                + 120.0f * cosf(wx * 0.0004f + 2.0f)  * cosf(wz * 0.0003f)
                +  40.0f * sinf(wx * 0.001f)           * sinf(wz * 0.001f);
        return h * state.terrain_height_scale;
    };

    // Geomorphed height at grid index (ix,iz). Odd vertices (those absent from the
    // next-coarser grid) blend toward the coarse surface by the morph factor.
    auto get_height = [&](int ix, int iz) -> float {
        float wx = start_x + ix * tile_sz, wz = start_z + iz * tile_sz;
        float h = base_height(wx, wz);
        if (morph <= 0.001f) return h;
        bool ox = (ix & 1) != 0, oz = (iz & 1) != 0;
        float ts = tile_sz, coarse = h;
        if (ox && !oz)      coarse = 0.5f  * (base_height(wx - ts, wz) + base_height(wx + ts, wz));
        else if (!ox && oz) coarse = 0.5f  * (base_height(wx, wz - ts) + base_height(wx, wz + ts));
        else if (ox && oz)  coarse = 0.25f * (base_height(wx - ts, wz - ts) + base_height(wx + ts, wz - ts)
                                            + base_height(wx - ts, wz + ts) + base_height(wx + ts, wz + ts));
        return h + (coarse - h) * morph;
    };

    // Fog blend [0..1] from the camera, scaled to zoom so the terrain's far edge
    // always dissolves into the horizon regardless of LOD/zoom level.
    auto fog_factor = [&](float wx, float wz) -> float {
        float dx = wx - camera_.position.x, dz = wz - camera_.position.z;
        float d  = sqrtf(dx * dx + dz * dz);
        float start = cam_dist * 0.6f, end = cam_dist * 2.6f;
        return std::clamp((d - start) / std::max(1.0f, end - start), 0.0f, 1.0f);
    };

    // Military-space surface colour, blended toward the horizon haze by fog.
    auto terrain_color = [&](float h, float wx, float wz) {
        float nh = (state.terrain_height_scale > 0.01f)
                   ? std::clamp((h + 1000.0f) / (2000.0f * state.terrain_height_scale), 0.0f, 1.0f)
                   : 0.5f;
        // Deep space blue/black base, fading to dark obsidian/slate peaks
        float r = 10.0f + nh * 15.0f;
        float g = 15.0f + nh * 20.0f;
        float b = 25.0f + nh * 30.0f;
        float f = fog_factor(wx, wz);
        r += (kSkyHorizon.r - r) * f;
        g += (kSkyHorizon.g - g) * f;
        b += (kSkyHorizon.b - b) * f;
        rlColor4ub((unsigned char)r, (unsigned char)g, (unsigned char)b, 255);
    };

    // ── Solid terrain ─────────────────────────────────────────────────────────
    if (state.terrain_mode == 2 || state.terrain_mode == 3) {
        rlBegin(RL_QUADS);
        for (int iz = 0; iz < grid_n; ++iz) {
            for (int ix = 0; ix < grid_n; ++ix) {
                float x0 = start_x + ix * tile_sz,  z0 = start_z + iz * tile_sz;
                float x1 = x0 + tile_sz,            z1 = z0 + tile_sz;

                float h00 = get_height(ix,     iz);
                float h10 = get_height(ix + 1, iz);
                float h11 = get_height(ix + 1, iz + 1);
                float h01 = get_height(ix,     iz + 1);
                float havg = (h00 + h10 + h11 + h01) * 0.25f;

                terrain_color(havg, x0 + tile_sz * 0.5f, z0 + tile_sz * 0.5f);
                rlVertex3f(x0, h00, z0);
                rlVertex3f(x0, h01, z1);
                rlVertex3f(x1, h11, z1);
                rlVertex3f(x1, h10, z0);
            }
        }
        rlEnd();
    }

    // ── Grid wireframe — stays legible at every zoom level ────────────────────
    if (state.terrain_mode == 1 || state.terrain_mode == 3) {
        // Keep the grid visible at all distances. Fade only gently at extreme
        // zoom so it never vanishes — it remains a subtle reference at any scale.
        float fade = 1.0f - std::clamp((cam_dist - 60000.0f) / 40000.0f, 0.0f, 0.55f);
        // Lone wireframe (mode 1) is the primary visual, so make it brighter than
        // the overlay grid drawn on top of solid terrain (mode 3).
        float peak = (state.terrain_mode == 1) ? 150.0f : 80.0f;
        unsigned char grid_alpha = (unsigned char)(fade * peak);
        // Lift the grid above the solid surface by a distance-scaled offset so it
        // never z-fights with the terrain once depth precision drops at far zoom.
        float wire_off = std::max(2.0f, cam_dist * 0.0015f);
        if (grid_alpha > 2) {
            rlBegin(RL_LINES);
            for (int iz = 0; iz < grid_n; ++iz) {
                for (int ix = 0; ix < grid_n; ++ix) {
                    float x0 = start_x + ix * tile_sz, z0 = start_z + iz * tile_sz;
                    float x1 = x0 + tile_sz,           z1 = z0 + tile_sz;

                    float h00 = get_height(ix,     iz);
                    float h10 = get_height(ix + 1, iz);
                    float h11 = get_height(ix + 1, iz + 1);
                    float h01 = get_height(ix,     iz + 1);

                    // Military space grid: desaturated cyan/grey, fading with the
                    // terrain into the horizon haze at distance.
                    float gf = 1.0f - fog_factor(x0 + tile_sz * 0.5f, z0 + tile_sz * 0.5f);
                    rlColor4ub(40, 90, 110, (unsigned char)(grid_alpha * gf));
                    rlVertex3f(x0, h00 + wire_off, z0);
                    rlVertex3f(x0, h01 + wire_off, z1);

                    rlVertex3f(x0, h00 + wire_off, z0);
                    rlVertex3f(x1, h10 + wire_off, z0);

                    rlVertex3f(x0, h01 + wire_off, z1);
                    rlVertex3f(x1, h11 + wire_off, z1);

                    rlVertex3f(x1, h10 + wire_off, z0);
                    rlVertex3f(x1, h11 + wire_off, z1);
                }
            }
            rlEnd();
        }
    }
}

void Application::update_camera_state(float dt) {
    auto& state = ui_.state();
    const bool ui_wants_mouse = ImGui::GetIO().WantCaptureMouse;

    // ── Scroll wheel: zoom ────────────────────────────────────────────────────
    if (!ui_wants_mouse) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            // Exponential zoom — faster when far, slower when close
            state.camera_distance *= powf(0.88f, wheel);
            state.camera_distance  = std::clamp(state.camera_distance, 30.0f, 80000.0f);
        }
    }

    // ── RMB: orbit / rotate in Free and Focus modes ──────────────────────────
    // In Chase mode, RMB adjusts chase yaw offset instead
    if (!ui_wants_mouse && IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        Vector2 delta = GetMouseDelta();
        // Sensitivity scales slightly with zoom so distant orbit feels right,
        // then by the user's multiplier.
        float sens = (0.20f + 0.05f * std::clamp(state.camera_distance / 20000.0f, 0.0f, 1.5f))
                     * state.mouse_sensitivity;
        // Inverted look = "grab the scene": dragging moves the world under the
        // cursor instead of swinging the camera the opposite way.
        float inv = state.invert_look ? -1.0f : 1.0f;
        if (state.camera_mode == 2) {
            // In chase mode — adjust lateral yaw offset around entity
            state.chase_yaw_offset   += inv * delta.x * 0.3f * state.mouse_sensitivity;
            state.chase_pitch_offset -= inv * delta.y * 0.2f * state.mouse_sensitivity;
            state.chase_pitch_offset  = std::clamp(state.chase_pitch_offset, -30.0f, 60.0f);
        } else {
            state.camera_yaw   += inv * delta.x * sens;
            state.camera_pitch -= inv * delta.y * sens;
            state.camera_pitch  = std::clamp(state.camera_pitch, -89.0f, 89.0f);
        }
    }

    // ── MMB: pan the camera target ───────────────────────────────────────────
    // Feels natural in any CAD/3D tool — drag the ground plane under the cursor
    if (!ui_wants_mouse && IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)
        && state.camera_mode == 0) {
        Vector2 delta = GetMouseDelta();
        float yaw_rad = state.camera_yaw * DEG2RAD;
        Vector3 cam_right   = { cosf(yaw_rad), 0.0f, -sinf(yaw_rad) };
        Vector3 cam_forward = { sinf(yaw_rad), 0.0f,  cosf(yaw_rad) };
        // Pan speed proportional to distance so it feels 1:1 with the scene
        float pan_scale = state.camera_distance * 0.0012f;
        camera_free_target_ = Vector3Subtract(camera_free_target_,
            Vector3Scale(cam_right,   delta.x * pan_scale));
        camera_free_target_ = Vector3Add(camera_free_target_,
            Vector3Scale(cam_forward, delta.y * pan_scale));
    }

    // ── F key: toggle focus on selected entity ────────────────────────────────
    flecs::entity sel = state.selected_entity;
    if (IsKeyPressed(KEY_F) && sel.is_valid() && world_.is_alive(sel)) {
        if (state.camera_mode >= 1) {
            // Return to free orbit, pivot point on where entity was
            state.camera_mode = 0;
            if (sel.has<ecs::Position>())
                camera_free_target_ = sel.get<ecs::Position>().v;
        } else {
            state.camera_mode = 1;
        }
    }

    // ── G key: toggle chase cam ───────────────────────────────────────────────
    if (IsKeyPressed(KEY_G) && sel.is_valid() && world_.is_alive(sel)) {
        state.camera_mode = (state.camera_mode == 2) ? 0 : 2;
        if (state.camera_mode == 2) {
            state.chase_yaw_offset   = 0.0f;
            state.chase_pitch_offset = 20.0f;  // default: slightly above-behind
        }
    }

    // ── MODE 1: Focus Orbit — orbit around selected entity ────────────────────
    if (state.camera_mode == 1 && sel.is_valid() && world_.is_alive(sel)) {
        if (sel.has<ecs::Position>()) {
            Vector3 ent_pos = sel.get<ecs::Position>().v;
            ent_pos.y *= state.altitude_exaggerate;  // Also apply exaggeration to Focus orbit target
            camera_.target  = ent_pos;

            float pitch_rad = state.camera_pitch * DEG2RAD;
            float yaw_rad   = state.camera_yaw   * DEG2RAD;
            camera_.position = Vector3Add(ent_pos, {
                state.camera_distance * cosf(pitch_rad) * sinf(yaw_rad),
                state.camera_distance * sinf(pitch_rad),
                state.camera_distance * cosf(pitch_rad) * cosf(yaw_rad)
            });
            camera_.up = { 0.0f, 1.0f, 0.0f };
        }
    }
    // ── MODE 2: Chase Camera — fly behind entity, angled down ─────────────────
    else if (state.camera_mode == 2 && sel.is_valid() && world_.is_alive(sel)) {
        if (sel.has<ecs::Position>() && sel.has<ecs::Rotation>() && sel.has<ecs::Velocity>()) {
            Vector3    ent_pos = sel.get<ecs::Position>().v;
            Quaternion ent_rot = sel.get<ecs::Rotation>().q;
            Vector3    ent_vel = sel.get<ecs::Velocity>().v;

            // CRITICAL: Apply altitude exaggeration so camera tracks the visual model height
            ent_pos.y *= state.altitude_exaggerate;

            // Entity's local axes
            Vector3 fwd = Vector3Normalize(Vector3RotateByQuaternion({0, 0, -1}, ent_rot));
            Vector3 up  = Vector3Normalize(Vector3RotateByQuaternion({0, 1,  0}, ent_rot));
            Vector3 rgt = Vector3CrossProduct(fwd, up);

            // Chase distance scales with camera_distance slider
            float base_dist = std::max(200.0f, state.camera_distance * 0.08f);

            // Apply user yaw/pitch offsets (RMB drag while in chase mode)
            float cy = state.chase_yaw_offset   * DEG2RAD;
            float cp = state.chase_pitch_offset * DEG2RAD;

            // Rotate the back-vector by user offset to allow swinging around
            Vector3 back_dir = Vector3Negate(fwd);
            // Yaw offset: spin around world-up
            back_dir = Vector3RotateByQuaternion(back_dir,
                QuaternionFromAxisAngle({0,1,0}, cy));
            // Pitch offset: tilt up/down
            back_dir = Vector3RotateByQuaternion(back_dir,
                QuaternionFromAxisAngle(rgt, cp));

            // Camera sits base_dist behind + elevated to give a top-down view angle
            float height_mult = 0.45f + state.chase_pitch_offset * 0.008f;
            Vector3 cam_offset = Vector3Add(
                Vector3Scale(back_dir, base_dist),
                Vector3Scale({0, 1, 0}, base_dist * height_mult)
            );

            Vector3 desired_pos = Vector3Add(ent_pos, cam_offset);

            // Spring-damped smoothing — tight enough to feel responsive
            float alpha = 1.0f - expf(-8.0f * dt);
            camera_.position = Vector3Lerp(camera_.position, desired_pos, alpha);

            // Look slightly ahead of the entity (in the direction of motion)
            float speed = Vector3Length(ent_vel);
            float look_ahead = std::min(speed * 1.5f, base_dist * 1.2f);
            Vector3 look_target = Vector3Add(ent_pos, Vector3Scale(fwd, look_ahead));
            camera_.target = Vector3Lerp(camera_.target, look_target, alpha);
            camera_.up     = { 0.0f, 1.0f, 0.0f };
        }
    }
    // ── MODE 0: Free Orbit ────────────────────────────────────────────────────
    else {
        float yaw_rad = state.camera_yaw * DEG2RAD;
        Vector3 forward = { sinf(yaw_rad), 0.0f,  cosf(yaw_rad) };
        Vector3 right   = { cosf(yaw_rad), 0.0f, -sinf(yaw_rad) };

        // WASD keyboard pan — speed scales with zoom level
        if (!ui_wants_mouse) {
            float pan_speed = state.camera_distance * 0.45f * dt;
            if (IsKeyDown(KEY_LEFT_SHIFT)) pan_speed *= 4.0f;

            if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))
                camera_free_target_ = Vector3Subtract(camera_free_target_, Vector3Scale(forward, pan_speed));
            if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))
                camera_free_target_ = Vector3Add(camera_free_target_, Vector3Scale(forward, pan_speed));
            if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))
                camera_free_target_ = Vector3Subtract(camera_free_target_, Vector3Scale(right, pan_speed));
            if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT))
                camera_free_target_ = Vector3Add(camera_free_target_, Vector3Scale(right, pan_speed));
        }

        camera_.target = camera_free_target_;

        float pitch_rad = state.camera_pitch * DEG2RAD;
        camera_.position = Vector3Add(camera_.target, {
            state.camera_distance * cosf(pitch_rad) * sinf(yaw_rad),
            state.camera_distance * sinf(pitch_rad),
            state.camera_distance * cosf(pitch_rad) * cosf(yaw_rad)
        });
        camera_.up = { 0.0f, 1.0f, 0.0f };
    }
}

} // namespace debrief
