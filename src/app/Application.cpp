#include "Application.hpp"
#include "../ecs/Systems.hpp"
#include <rlImGui.h>
#include <imgui.h>
#include <implot.h>
#include <raymath.h>
#include <rlgl.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <initializer_list>
#include <vector>
#include <filesystem>
#include <cctype>
#include "Config.hpp"
#include "../persistence/CsvImporter.hpp"
#include "tinyfiledialogs.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace afteraction {

// ─────────────────────────────────────────────────────────────────────────────
//  Coordinate helpers
// ─────────────────────────────────────────────────────────────────────────────

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

// ── Paths anchored to the executable's directory (not the working directory) ──
std::string Application::app_dir() const {
    const char* d = GetApplicationDirectory();   // ends with a path separator
    return d ? std::string(d) : std::string();
}
std::string Application::asset_path(const std::string& rel) const {
    return app_dir() + "assets/" + rel;
}
std::string Application::recordings_dir() const {
    std::string dir = app_dir() + "recordings";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);   // no-op if it already exists
    return dir + "/";
}
std::string Application::config_path() const {
    return app_dir() + "afteraction_config.yaml";
}

Application::~Application() {
    if (!cfg_.demo_mode) udp_receiver_.stop();
    recorder_.stop();
    ConfigManager::save_text(config_path(), ConfigManager::to_yaml(ui_.state()));
    // Free GL resources (models/meshes) while the GL context is still alive.
    // The AssetManager member destructor runs after this body — i.e. after
    // CloseWindow() has destroyed the context — so unloading there would make GL
    // calls on a dead context and crash. unload_all() is idempotent.
    assets_.unload_all();
    if (minimap_job_.valid()) minimap_job_.wait();
    if (minimap_tex_.id) UnloadTexture(minimap_tex_);
    terrain_.shutdown();
    if (label_font_.texture.id != GetFontDefault().texture.id) UnloadFont(label_font_);
    ImPlot::DestroyContext();
    rlImGuiShutdown();
    CloseWindow();
}

// ── Init ──────────────────────────────────────────────────────────────────────
void Application::init_window() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(cfg_.window_width, cfg_.window_height, cfg_.window_title.c_str());
    SetTargetFPS(cfg_.target_fps);
    SetExitKey(KEY_NULL);   // Esc closes panels / clears the selection; Ctrl+Q quits

    // Window / taskbar icon — prefer the rounded, transparent-corner variant;
    // fall back to the square one. GLFW copies the pixels on SetWindowIcon, so
    // the Image can be unloaded immediately. Must be RGBA8 for raylib.
    Image icon = LoadImage(asset_path("icon_rounded.png").c_str());
    if (!icon.data) icon = LoadImage(asset_path("icon.png").c_str());
    if (icon.data) {
        ImageFormat(&icon, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        SetWindowIcon(icon);
        UnloadImage(icon);
    }

    // ImGui with our own fonts: Barlow for the UI (Font Awesome icons merged in
    // by rlImGui), JetBrains Mono for numeric read-outs.
    rlImGuiBeginInitImGui();
    ImGui::StyleColorsDark();
    {
        ImGuiIO& io = ImGui::GetIO();
        const std::string ui_font = asset_path("fonts/Barlow-Medium.ttf");
        if (FileExists(ui_font.c_str())) {
            io.Fonts->Clear();
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0100, 0x017F, 0x2000, 0x206F, 0 };  // Latin + Turkish
            io.Fonts->AddFontFromFileTTF(ui_font.c_str(), 16.0f, &cfg, ranges);
        }
    }
    rlImGuiEndInitImGui();
    AfterActionUI::apply_theme();
    {
        ImGuiIO& io = ImGui::GetIO();
        // Merge Font Awesome into each extra font too, so icons work everywhere.
        const std::string icons = asset_path("fonts/fa-solid-900.ttf");
        auto add_with_icons = [&](const std::string& path, float px) -> ImFont* {
            if (!FileExists(path.c_str())) return nullptr;
            static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0100, 0x017F, 0x2000, 0x206F, 0 };
            ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), px, nullptr, ranges);
            if (f && FileExists(icons.c_str())) {
                static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
                ImFontConfig cfg;
                cfg.MergeMode = true;
                cfg.PixelSnapH = true;
                cfg.GlyphMinAdvanceX = px;
                io.Fonts->AddFontFromFileTTF(icons.c_str(), px * 0.85f, &cfg, icon_ranges);
            }
            return f;
        };
        ui_.set_mono_font(add_with_icons(asset_path("fonts/JetBrainsMono-Regular.ttf"), 14.0f));
        ui_.set_heading_font(add_with_icons(asset_path("fonts/Barlow-SemiBold.ttf"), 19.0f));
    }

    // Raylib font for in-world labels (default bitmap font as a fallback).
    const std::string lbl = asset_path("fonts/Barlow-SemiBold.ttf");
    std::vector<int> cps;                                   // ASCII + Latin-1/Ext-A (Turkish) + dashes
    for (int c = 0x20; c < 0x7F; ++c) cps.push_back(c);
    for (int c = 0xA0; c < 0x180; ++c) cps.push_back(c);
    for (int c : { 0x2013, 0x2014, 0x2022, 0x00B0 }) cps.push_back(c);
    label_font_ = FileExists(lbl.c_str())
        ? LoadFontEx(lbl.c_str(), 40, cps.data(), static_cast<int>(cps.size()))
        : GetFontDefault();
    SetTextureFilter(label_font_.texture, TEXTURE_FILTER_BILINEAR);

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
    ecs::register_systems(world_);
}

void Application::init_assets() {
    // Create detailed procedural composite models now that InitWindow() has initialized the GPU context
    assets_.init_procedural();

    // Custom models are configured in assets/models.yaml (type → file/scale/tint/
    // base-rotation) — no recompile needed. They load ASYNCHRONOUSLY: we only
    // *request* them here so the window opens instantly; the worker thread parses
    // the files and tick() uploads + hot-swaps them in over the next frames.
    for (const auto& s : ConfigManager::load_model_manifest(asset_path("models.yaml"))) {
        Color tint{ s.tint[0], s.tint[1], s.tint[2], 255 };
        Quaternion base_rot = QuaternionFromEuler(s.pitch * DEG2RAD,
                                                  s.yaw   * DEG2RAD,
                                                  s.roll  * DEG2RAD);
        assets_.request_load(s.file, asset_path(s.file), s.type, s.scale, tint, base_rot);
        TraceLog(LOG_INFO, "Queued model '%s' for type %u", s.file.c_str(), s.type);
        // Seed the Settings → Models UI with current bindings.
        UIState::ModelBinding b;
        b.type = s.type;
        snprintf(b.file, sizeof(b.file), "%s", s.file.c_str());
        b.scale = s.scale;
        b.tint[0] = s.tint[0]/255.0f; b.tint[1] = s.tint[1]/255.0f; b.tint[2] = s.tint[2]/255.0f;
        b.yaw = s.yaw; b.pitch = s.pitch; b.roll = s.roll;
        ui_.state().model_bindings.push_back(b);
    }
}

void Application::init_camera() {
    // Entities in demo orbit at 3000-5500m altitude, 5-8km radius.
    // Start with a side-angle view that clearly shows altitude differences.
    camera_.position   = { 0.0f, 5000.0f, 12000.0f };
    camera_.target     = { 0.0f, 1500.0f, 0.0f };
    camera_.up         = { 0.0f, 1.0f,   0.0f };
    camera_.fovy       = 60.0f;
    camera_.projection = CAMERA_PERSPECTIVE;
    camera_ctl_.set_pivot({ 0.0f, 1500.0f, 0.0f });
    camera_ctl_.set_ground([this](float x, float z) { return terrain_height_at(x, z); });

    // Default initial settings before loading config
    ui_.state().camera_yaw      = 0.0f;
    ui_.state().camera_pitch    = 22.0f;   // angled to show height
    ui_.state().camera_distance = 12000.0f;
    ui_.state().entity_3d_scale = 1.0f;
    ui_.state().trail_width_override = 120.0f;
    ui_.state().terrain_mode    = static_cast<int>(TerrainLook::ShadedGrid);
    ui_.state().altitude_exaggerate = 1.0f;  // real scale (terrain + tracks together)
    ui_.state().far_clip_plane  = 2000000.0f;

    // Load config
    ConfigManager::load_config(ui_.state(), config_path());
}

void Application::init_terrain() {
    const std::string dir = cfg_.terrain_dir.empty() ? app_dir() + "terrain" : cfg_.terrain_dir;
    terrain_.load_data(dir);
    terrain_.init_gpu();
    const size_t nrw = airfields_.load_csv(dir + "/runways_tr.csv");
    TraceLog(LOG_INFO, "Airfields: %zu runways", nrw);
    // Start at the configured home origin (Akıncı by default) so terrain is
    // visible before any data arrives; live data re-centres only if far away.
    set_origin(ui_.state().origin_lat, ui_.state().origin_lon);
    demo_.set_height_fn([this](double lat, double lon) {
        return terrain_.height_at_geo(lat, lon).value_or(0.0f);
    });
    if (cfg_.demo_mode) {
        // Frame the airfield: the demo jet lines up on runway 03.
        set_origin(demo::DemoMode::kAnchorLat, demo::DemoMode::kAnchorLon);
        camera_ctl_.set_pivot({ -900.0f, terrain_height_at(-900.0f, 1400.0f) + 50.0f, 1400.0f });
        ui_.state().camera_distance = 3500.0f;
        ui_.state().camera_pitch = 18.0f;
        ui_.state().camera_yaw = 200.0f;
    }
}

void Application::set_origin(double lat, double lon) {
    ++origin_epoch_;
    geo_.set(lat, lon);
    terrain_.set_frame(geo_);
    airfields_.rebuild(geo_, terrain_);
    ui_.state().scene_lat = lat;
    ui_.state().scene_lon = lon;
    TraceLog(LOG_INFO, "Scene origin %.5f, %.5f", lat, lon);
}

void Application::init_ui_callbacks() {
    UICallbacks cbs;
    cbs.on_record_start = [this] {
        char name[64];
        time_t t = time(nullptr);
        tm* lt   = localtime(&t);
        strftime(name, sizeof(name), "session_%Y%m%d_%H%M%S.aar", lt);
        std::string path = recordings_dir() + name;   // <app_dir>/recordings/
        persist::SceneOrigin origin{};
        origin.lat_deg = geo_.lat0();
        origin.lon_deg = geo_.lon0();
        recorder_.start(path, name, origin);
        TraceLog(LOG_INFO, "Recording to %s", path.c_str());
    };
    cbs.on_record_stop = [this] { recorder_.stop(); };
    cbs.on_save_dashcam = [this](float secs) {
        char name[64];
        time_t t = time(nullptr);
        tm* lt   = localtime(&t);
        strftime(name, sizeof(name), "dashcam_%Y%m%d_%H%M%S.aar", lt);
        std::string path = recordings_dir() + name;   // <app_dir>/recordings/
        persist::SceneOrigin origin{};
        origin.lat_deg = geo_.lat0();
        origin.lon_deg = geo_.lon0();
        if (persist::Recorder::export_slice(store_, secs, path, "dashcam", origin))
            TraceLog(LOG_INFO, "Saved dashcam to %s", path.c_str());
        else
            TraceLog(LOG_WARNING, "Could not save dashcam to %s (no data or write error)", path.c_str());
    };
    cbs.on_load_file   = [this](std::string path) { load_session(path); };
    cbs.on_load_csv    = [this](std::string path) { import_csv(path); };
    cbs.on_browse_file = [this] { browse_and_load(); };
    cbs.on_load_model = [this](uint16_t type_id, std::string path) {
        if (assets_.load(path, path, 1.0f)) {
            assets_.map_type(type_id, path);
            repoint_entities_of_type(type_id);   // existing tracks switch too
        }
    };
    cbs.on_clear_entities = [this] { clear_requested_ = true; };
    cbs.on_apply_network  = [this](std::string addr, uint16_t port) {
        apply_network_settings(addr, port);
    };
    // Settings → Models: load/replace a model live (async; pump+repoint in tick).
    cbs.on_model_load = [this](const ModelBindRequest& r) {
        Color tint{ r.tint[0], r.tint[1], r.tint[2], 255 };
        Quaternion base_rot = QuaternionFromEuler(r.pitch * DEG2RAD,
                                                  r.yaw   * DEG2RAD,
                                                  r.roll  * DEG2RAD);
        assets_.request_load(r.file, asset_path(r.file), r.type, r.scale, tint, base_rot);
    };
    cbs.on_model_clear = [this](uint16_t type) {
        assets_.unmap_type(type);
        repoint_entities_of_type(type);   // back to the procedural shape
    };
    cbs.on_go_home = [this] {
        const auto& st = ui_.state();
        // Moving the origin would leave existing tracks (and a running recording,
        // whose file stores one origin) in the old frame — only do it when empty.
        if (GeoFrame::distance_m(geo_.lat0(), geo_.lon0(), st.origin_lat, st.origin_lon) > 250000.0) {
            if (entity_map_.empty() && !recorder_.is_recording())
                set_origin(st.origin_lat, st.origin_lon);
            else
                TraceLog(LOG_WARNING, "Home is far from the scene: keeping the origin while tracks/recording exist");
        }
        const Vector3 p = geo_.to_world(st.origin_lat, st.origin_lon, 0.0);
        ui_.state().camera_mode = 0;
        look_at(p.x, p.z);
    };
    cbs.on_set_home_here = [this] {
        double lat, lon;
        geo_.to_geo(camera_ctl_.pivot().x, camera_ctl_.pivot().z, &lat, &lon);
        ui_.state().origin_lat = lat;
        ui_.state().origin_lon = lon;
    };
    cbs.on_look_at = [this](float x, float z) {
        ui_.state().camera_mode = 0;
        look_at(x, z);
    };
    cbs.to_geo = [this](float x, float z, double* lat, double* lon) { geo_.to_geo(x, z, lat, lon); };
    cbs.ground_height = [this](float x, float z) { return terrain_.height_at(x, z); };
    cbs.on_models_save = [this](const std::vector<ModelBindRequest>& reqs) {
        std::vector<ModelSpec> specs;
        for (const auto& r : reqs) {
            ModelSpec s;
            s.type = r.type; s.file = r.file; s.scale = r.scale;
            s.tint[0] = r.tint[0]; s.tint[1] = r.tint[1]; s.tint[2] = r.tint[2];
            s.yaw = r.yaw; s.pitch = r.pitch; s.roll = r.roll;
            specs.push_back(std::move(s));
        }
        ConfigManager::save_model_manifest(asset_path("models.yaml"), specs);
    };
    cbs.on_open_folder = [](const std::string& dir) {
        if (!dir.empty()) OpenURL(dir.c_str());
    };
    ui_.set_callbacks(std::move(cbs));

    // Listener address: the saved settings, unless given on the command line.
    auto& st = ui_.state();
    if (cfg_.port_from_cli) st.listen_port = cfg_.udp_port;
    else                    cfg_.udp_port = st.listen_port;
    if (cfg_.addr_from_cli) snprintf(st.bind_addr, sizeof(st.bind_addr), "%s", cfg_.bind_addr.c_str());
    else                    cfg_.bind_addr = st.bind_addr;

    st.scene.recordings_dir = recordings_dir();
    st.scene.config_path = config_path();
    saved_config_ = ConfigManager::to_yaml(st);
}

void Application::autosave_settings() {
    if (GetTime() < next_autosave_check_) return;
    next_autosave_check_ = GetTime() + 1.0;
    std::string now = ConfigManager::to_yaml(ui_.state());
    if (now == saved_config_) return;
    if (ConfigManager::save_text(config_path(), now)) {
        saved_config_ = std::move(now);
        ui_.state().scene.last_saved_time = GetTime();
    }
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
    live_newest_ns_ = 0;
    source_offset_ns_.clear();
    ui_.state().selected_entity = {};
    origin_locked_ = false;   // the next live track may re-centre the scene
    camera_ctl_.reset_auto_frame();   // re-frame the next single track that arrives
    playback_.stop();   // return to live mode
    TraceLog(LOG_INFO, "Cleared all entities and telemetry store");
}

// ── Replay-file loading (typed path + native dialog share these) ──────────────
void Application::load_session(const std::string& path) {
    clear_all_entities();
    persist::SceneOrigin origin{};
    if (!persist::Recorder::load_into(path, store_, &origin)) {
        TraceLog(LOG_WARNING, "Could not load session %s", path.c_str());
        return;
    }
    // Recordings store positions relative to the origin they were made at.
    if (origin.lat_deg != 0.0 || origin.lon_deg != 0.0) {
        set_origin(origin.lat_deg, origin.lon_deg);
        origin_locked_ = true;
    }
    auto [ts, te] = store_.time_range_ns();
    playback_.seek(ts);
    playback_.pause();
    TraceLog(LOG_INFO, "Loaded session %s", path.c_str());
}

void Application::import_csv(const std::string& path) {
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

    std::stable_sort(states.begin(), states.end(),
        [](const net::EntityState& a, const net::EntityState& b) {
            return a.timestamp_ns < b.timestamp_ns;
        });

    clear_all_entities();
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
    store_.flush_pending();

    auto [ts, te] = store_.time_range_ns();
    playback_.seek(ts);
    playback_.pause();
    TraceLog(LOG_INFO, "Imported CSV %s", path.c_str());
}

void Application::browse_and_load() {
    // Native open dialog (Windows comdlg32 / zenity / osascript). Filter to the
    // formats we can replay; dispatch by extension.
    static const char* kFilters[] = { "*.aar", "*.csv" };
    const char* sel = tinyfd_openFileDialog(
        "Open replay or CSV log", recordings_dir().c_str(),
        2, kFilters, "AfterAction session (.aar) / CSV", 0);
    if (!sel) return;   // cancelled
    std::string path = sel;
    snprintf(ui_.state().load_path, sizeof(ui_.state().load_path), "%s", path.c_str());
    std::string ext = std::filesystem::path(path).extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".csv") import_csv(path);
    else               load_session(path);
}

void Application::repoint_entities_of_type(uint16_t type) {
    const ModelEntry* e = assets_.get_for_type(type);
    if (!e) return;
    world_.query<ecs::EntityMeta, ecs::RenderModel>()
        .each([&](ecs::EntityMeta& meta, ecs::RenderModel& rm) {
            if (meta.type == type) rm.entry = e;
        });
    TraceLog(LOG_INFO, "Re-pointed live entities of type %u to new model", type);
}

void Application::apply_network_settings(const std::string& bind_addr, uint16_t port) {
    cfg_.bind_addr = bind_addr;
    cfg_.udp_port  = port;
    start_listener();
}

void Application::start_listener() {
    SceneInfo& sc = ui_.state().scene;
    if (cfg_.demo_mode) {
        sc.net_ok = false;
        sc.net_status = "demo mode: live UDP is off";
        return;
    }
    udp_receiver_.stop();
    sc.net_active_addr = cfg_.bind_addr;
    sc.net_active_port = cfg_.udp_port;
    char buf[128];
    if (udp_receiver_.start(cfg_.bind_addr, cfg_.udp_port)) {
        snprintf(buf, sizeof(buf), "listening on %s:%u", cfg_.bind_addr.c_str(), cfg_.udp_port);
        sc.net_ok = true;
        TraceLog(LOG_INFO, "UDP receiver %s", buf);
    } else {
        snprintf(buf, sizeof(buf), "cannot listen on %s:%u (address in use or invalid?)",
                 cfg_.bind_addr.c_str(), cfg_.udp_port);
        sc.net_ok = false;
        TraceLog(LOG_WARNING, "UDP receiver %s", buf);
    }
    sc.net_status = buf;
}

// ── Main loop ─────────────────────────────────────────────────────────────────
void Application::run() {
    init_window();
    init_ecs();
    init_assets();
    init_camera();
    init_terrain();
    init_ui_callbacks();

    start_listener();

    if (!cfg_.open_file.empty()) {
        std::string ext = std::filesystem::path(cfg_.open_file).extension().string();
        for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (ext == ".csv") import_csv(cfg_.open_file);
        else               load_session(cfg_.open_file);
    }

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

    // Upload any models the worker finished parsing (GL must be on this thread),
    // and hot-swap them onto entities that already exist for that type.
    for (uint16_t type : assets_.pump_uploads(2))
        repoint_entities_of_type(type);

    handle_input(dt);
    ConfigManager::clamp_settings(ui_.state());   // sliders can be Ctrl+clicked past their range
    if (cfg_.demo_mode) process_demo(dt);
    else                process_inbound_queue();
    update_ecs(dt);
    
    // Sync checkbox state for trail renderer
    trails_.mode = ui_.state().ribbon_trails ? TrailMode::Ribbon : TrailMode::Line;
    
    camera_ctl_.update(camera_, ui_.state(), dt);
    update_scene_info();
    update_minimap();
    render();
    autosave_settings();
}

// ── Coordinate + orientation conversion ──────────────────────────────────────
void Application::ensure_origin_and_convert(net::EntityState& state) {
    if (!origin_locked_) {
        origin_locked_ = true;
        // Keep the current origin (and its already-built terrain) unless the data
        // is far away from it — beyond that the flat projection would distort.
        // Not while recording: the file stores a single origin.
        constexpr double kRecentreM = 250000.0;
        if (!geo_.valid() ||
            (!recorder_.is_recording() &&
             GeoFrame::distance_m(geo_.lat0(), geo_.lon0(), state.lat_deg, state.lon_deg) > kRecentreM))
            set_origin(state.lat_deg, state.lon_deg);
    }
    const Vector3 p = geo_.to_world(state.lat_deg, state.lon_deg, state.alt_m);
    state.position[0] = p.x;
    state.position[1] = p.y;
    state.position[2] = p.z;

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
        // A count==0 control packet clears all tracks. Do it IMMEDIATELY (we're at
        // a safe point — before world_.progress, not inside a query) rather than
        // deferring, so any initial-state frames a sender sends right after CLEAR,
        // in this same drain, are applied onto the freshly-cleared world instead
        // of being wiped on the next tick. (clear_all_entities() also resets
        // live_states_, so pre-CLEAR frames in this batch are correctly dropped.)
        if (frame->clear_all) { clear_all_entities(); continue; }
        for (auto& es : frame->entities) {
            es.timestamp_ns = to_local_time(es.source_id, es.timestamp_ns);
            ensure_origin_and_convert(es);
            apply_state_to_ecs(es);
            live_states_.push_back(es);
        }
    }

    if (!live_states_.empty()) {
        uint64_t ts = live_states_.back().timestamp_ns;
        store_.ingest(ts, live_states_);
        note_live_sample(ts);
        if (recorder_.is_recording())
            recorder_.submit(ts, 0, live_states_);
    }
}

// ── Demo data injection ───────────────────────────────────────────────────────
void Application::process_demo(float dt) {
    if (!playback_.is_live()) return;

    live_states_ = demo_.tick(dt);
    for (auto& es : live_states_) {
        ensure_origin_and_convert(es);
        apply_state_to_ecs(es);
    }

    if (!live_states_.empty()) {
        uint64_t ts = live_states_.back().timestamp_ns;
        store_.ingest(ts, live_states_);
        note_live_sample(ts);

        if (recorder_.is_recording())
            recorder_.submit(ts, 0, live_states_);
    }
}

// ── ECS entity creation / update ─────────────────────────────────────────────
flecs::entity Application::get_or_create_entity(const net::EntityState& state) {
    uint64_t key = entity_key(state.source_id, state.entity_id);
    if (auto it = entity_map_.find(key); it != entity_map_.end())
        return it->second;

    // Recordings and CSV files can carry callsigns that skipped the network
    // parser, so clean them here too.
    char callsign[net::kCallsignLen];
    std::memcpy(callsign, state.callsign, net::kCallsignLen);
    callsign[net::kCallsignLen - 1] = '\0';
    net::clean_callsign(callsign, net::kCallsignLen);

    // Unique per source as well (two senders may use the same id and callsign).
    char ename[80];
    snprintf(ename, sizeof(ename), "%s_%u_%u",
             callsign[0] ? callsign : "ent", state.source_id, state.entity_id);

    const auto* entry = assets_.get_for_type(state.entity_type);

    ecs::RenderModel rm{};
    rm.entry = entry;   // AssetManager owns the model

    // Per-type trail color for visual identification
    ecs::Trail trail_init{};
    trail_init.width = 10.0f;
    switch (state.entity_type) {
        case net::TYPE_JET:     trail_init.color = { 80, 160, 255, 200 }; break; // blue
        case net::TYPE_MISSILE: trail_init.color = { 255, 80,  80, 220 }; break; // red-orange
        case net::TYPE_AAA:     trail_init.color = { 80, 180,  80, 180 }; break; // green
        case net::TYPE_GROUND:  trail_init.color = {220, 140,  40, 180 }; break; // amber
        case net::TYPE_HELO:    trail_init.color = { 80, 220, 100, 200 }; break; // lime
        case net::TYPE_UAV:     trail_init.color = {200, 120, 255, 200 }; break; // violet
        case net::TYPE_UCAV:    trail_init.color = {150, 110, 255, 200 }; break; // indigo
        case net::TYPE_SAM:     trail_init.color = {240, 200,  60, 180 }; break; // yellow
        case net::TYPE_PROP:    trail_init.color = { 90, 215, 205, 200 }; break; // teal
        case net::TYPE_TRANSPORT: trail_init.color = {170, 188, 214, 190 }; break; // steel
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
    std::memcpy(meta.callsign, callsign, net::kCallsignLen);

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

        const auto* hist = store_.get_entity_history(source_id, entity_id);
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
uint64_t Application::to_local_time(uint32_t source_id, uint64_t ts_ns) {
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto it = source_offset_ns_.find(source_id);
    // New sender, or its clock is more than 5 s off ours (restart / jump): re-anchor.
    if (it == source_offset_ns_.end() || std::llabs(int64_t(ts_ns) + it->second - now) > 5'000'000'000LL)
        it = source_offset_ns_.insert_or_assign(source_id, now - int64_t(ts_ns)).first;
    return static_cast<uint64_t>(int64_t(ts_ns) + it->second);
}

void Application::note_live_sample(uint64_t ts_ns) {
    if (ts_ns + 5'000'000'000ULL < live_newest_ns_) live_newest_ns_ = 0;   // sender clock restarted
    if (ts_ns <= live_newest_ns_) return;
    if (live_newest_ns_ != 0) {
        const double gap = double(ts_ns - live_newest_ns_) * 1e-9;
        if (gap < 2.0) live_interval_ += (gap - live_interval_) * 0.1;
    }
    live_newest_ns_ = ts_ns;
    live_newest_at_ = GetTime();
}

uint64_t Application::advance_live_clock(float dt) {
    if (live_newest_ns_ == 0) return 0;
    // Stay ~1.5 sample intervals behind the newest data (absorbs network jitter).
    const double delay  = std::clamp(live_interval_ * 1.5 + 0.015, 0.03, 0.5);
    const double target = double(live_newest_ns_) + (GetTime() - live_newest_at_ - delay) * 1e9;
    live_clock_ns_ += double(dt) * 1e9;
    const double err = target - live_clock_ns_;
    if (std::fabs(err) > 1e9) live_clock_ns_ = target;              // first data, stall, new sender
    else                      live_clock_ns_ += err * std::min(1.0, double(dt) * 3.0);
    return static_cast<uint64_t>(std::max(0.0, live_clock_ns_));
}

void Application::update_ecs(float dt) {
    playback_.tick(dt);
    auto [ts, te] = store_.time_range_ns();
    if (ts) playback_.clamp(ts, te);
    if (!playback_.is_live()) {
        sync_ecs_to_playback(playback_.current_time_ns());
        world_.set<ecs::InterpolationCtx>({playback_.current_time_ns(), false});
    } else {
        // Back to live after viewing a file: tracks that only exist in the
        // file have no live history — hide them instead of drawing them at
        // the origin.
        if (!was_live_)
            world_.query<ecs::EntityMeta, const ecs::HistoryComp>()
                .each([](ecs::EntityMeta& m, const ecs::HistoryComp& h) { if (h.hist.count == 0) m.active = false; });
        world_.set<ecs::InterpolationCtx>({advance_live_clock(dt), true});
    }
    was_live_ = playback_.is_live();
    world_.progress(dt);

    camera_ctl_.auto_frame(ui_.state(), world_);
}

// ── Rendering ─────────────────────────────────────────────────────────────────
// Atmospheric palette — the horizon colour is reused as the fog target so
// distant terrain dissolves into the sky instead of hard-cutting.
static constexpr Color kSkyZenith  = { 10, 16, 28, 255 };  // deep blue, top of screen
static constexpr Color kSkyHorizon = { 74, 90, 110, 255 };  // haze at the horizon (= fog)

void Application::render() {
    BeginDrawing();
    // ClearBackground also clears the depth buffer; the gradient then paints
    // over the colour buffer to give a sky/horizon backdrop with real depth.
    ClearBackground(kSkyHorizon);
    // Sky: zenith colour fading to haze exactly at the screen-space horizon
    // (a far point straight ahead at camera height), haze below it.
    {
        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        Vector3 fwd = Vector3Subtract(camera_.target, camera_.position);
        fwd.y = 0.0f;
        int hy = sh / 2;
        if (Vector3Length(fwd) > 1e-3f) {
            fwd = Vector3Normalize(fwd);
            // (not named "far": <windows.h> defines far/near as empty macros)
            const Vector3 horizon = Vector3Add(camera_.position, Vector3Scale(fwd, 1.0e6f));
            hy = static_cast<int>(GetWorldToScreen(horizon, camera_).y);
        }
        hy = std::clamp(hy, 0, 4 * sh);
        if (hy > 0) DrawRectangleGradientV(0, 0, sw, hy, kSkyZenith, kSkyHorizon);
    }
    render_3d();
    render_ui();
    EndDrawing();
}

void Application::render_3d() {
    // Depth precision is governed by the FAR/NEAR ratio, not the absolute values,
    // so we keep that ratio roughly constant (~5000, which a 24-bit depth buffer
    // handles without z-fighting) by scaling the near plane to whatever far plane
    // we use. The far plane follows the zoom (terrain fades into fog by
    // ~cam_dist*2.6, so cam_dist*6 always covers it) and is capped by the user's
    // far_clip_plane, so raising that setting extends distance when zoomed out.
    // Use min/max (not std::clamp): if the user sets far_clip_plane below our
    // preferred cam_dist*6, clamp(x, lo, hi) with lo>hi would be UB. Here far is
    // cam_dist*6 capped by the user's setting, then floored so the near-plane
    // math stays valid even at extreme zoom-in.
    const float cam_dist = ui_.state().camera_distance;
    const TerrainStyle tstyle = terrain_style();
    terrain_.update(camera_, tstyle);

    // The far plane reaches just past the fog (beyond it everything is sky
    // colour anyway), capped by the user's setting. Depth precision depends on
    // FAR/NEAR, so the near plane scales with it.
    float far_plane = std::min(tstyle.fog_end * 1.05f, ui_.state().far_clip_plane);
    far_plane       = std::max(far_plane, 2000.0f);
    const float near_plane = std::clamp(far_plane / 20000.0f, 0.5f, 2000.0f);
    rlSetClipPlanes(near_plane, far_plane);
    BeginMode3D(camera_);

    const float exag = ui_.state().altitude_exaggerate;

    // Helper: apply the vertical scale to a world-space position
    auto exag_pos = [&](Vector3 p) -> Vector3 {
        return { p.x, p.y * exag, p.z };
    };

    // 1. Terrain (real DEM). Without data — or with terrain hidden — fall back
    //    to a tactical grid at sea level.
    if (ui_.state().terrain_mode > 0 && terrain_.has_data()) {
        draw_terrain();
        airfields_.draw(camera_, exag, std::min(far_plane, 80000.0f));
    } else {
        const float step = tstyle.grid_spacing;
        const float half = step * 50.0f;
        const float cx = roundf(camera_.position.x / step) * step;
        const float cz = roundf(camera_.position.z / step) * step;
        rlBegin(RL_LINES);
        rlColor4ub(0, 80, 150, 50);
        for (int i = -50; i <= 50; ++i) {
            rlVertex3f(cx + i * step, 0.f, cz - half);
            rlVertex3f(cx + i * step, 0.f, cz + half);
            rlVertex3f(cx - half, 0.f, cz + i * step);
            rlVertex3f(cx + half, 0.f, cz + i * step);
        }
        rlEnd();
    }

    // 2. Entities. Models use the shared lit shader (same sun and fog as the
    //    terrain) and are drawn two-sided, so flush and turn culling off once.
    assets_.set_frame_uniforms(camera_.position, kSunDir, tstyle.fog, tstyle.fog_start, tstyle.fog_end);
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
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
            if (!meta.active || !rm.entry) return;
            const ModelEntry& me = *rm.entry;

            // Apply altitude exaggeration to render position
            Vector3 rp = exag_pos(pos.v);  // rendered position (exaggerated Y)

            // Ground height directly beneath this entity (so markers/drop-lines
            // track terrain relief instead of sinking into hills).
            const float gh = terrain_height_at(rp.x, rp.z) * exag;

            const float len  = display_length(rp, me.length);   // on-screen size (m)
            const float grow = len / me.length;
            draw_entity_model(me, rp, QuaternionMultiply(rot.q, me.base_rot), me.scale * grow);
            draw_contact_shadow(rp, gh, len);

            // Altitude drop line from the terrain up to the entity, and a small
            // ring where it meets the ground. The selected track gets them in
            // the highlight colour (its screen-space brackets are drawn later).
            const bool selected = ui_.state().selected_entity == e;
            const Color drop_col = selected ? Color{ 255, 214, 10, 190 } : Color{ 0, 190, 255, 150 };
            if (rp.y - gh > 10.0f) {
                DrawLine3D({rp.x, gh, rp.z}, rp, drop_col);
                draw_ground_ring(rp.x, rp.z, std::max(0.6f * len, (rp.y - gh) * 0.03f),
                                 { drop_col.r, drop_col.g, drop_col.b, 100 });
            } else if (selected) {
                draw_ground_ring(rp.x, rp.z, 0.8f * len, { 255, 214, 10, 150 });
            }

            // Velocity vector (in exaggerated space)
            if (ui_.state().show_velocity_vec) {
                Vector3 ve  = { vel.v.x, vel.v.y * exag, vel.v.z };
                Vector3 tip = Vector3Add(rp, Vector3Scale(ve, 5.0f));
                DrawLine3D(rp, tip, {0, 255, 80, 200});
                DrawSphere(tip, len * 0.03f, {0, 255, 80, 180});
            }

            // Trail — copy ring buffer with exaggerated Y
            if (ui_.state().show_trails && trail.count >= 2) {
                ecs::Trail et = trail;
                for (uint32_t ti = 0; ti < ecs::Trail::kMaxPoints; ++ti)
                    et.points[ti].y = trail.points[ti].y * exag;
                trails_.draw(et, camera_, ui_.state().trail_width_override);
            }
        });

    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    EndMode3D();
    rlSetClipPlanes(0.01f, 1000.0f);

    if (ui_.state().terrain_mode > 0 && ui_.state().show_labels)
        airfields_.draw_labels(camera_, exag, label_font_);

    draw_selection_marker(exag);

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
                // Altitude in the chosen unit, the other one in parentheses.
                const UnitPrefs& u = ui_.state().units;
                const char* alt = TextFormat("%.0f %s (%.0f %s)", u.alt_value(pos.v.y), u.alt_suffix(),
                                             u.alt_value_alt(pos.v.y), u.alt_suffix_alt());

                const float fs = 17.0f * ui_.state().ui_scale, afs = 14.0f * ui_.state().ui_scale;

                // Put the label just outside the model's on-screen size (models
                // grow when the camera is close), joined by a short leader line.
                const ecs::RenderModel* rm = fe.try_get<ecs::RenderModel>();
                const float len = display_length(rp2, rm && rm->entry ? rm->entry->length : 20.0f);
                const Vector3 cam_right = Vector3Normalize(Vector3CrossProduct(cam_fwd, camera_.up));
                const Vector2 edge = GetWorldToScreen(Vector3Add(rp2, Vector3Scale(cam_right, len * 0.5f)), camera_);
                const float r_px = std::min(Vector2Distance(edge, sp), 240.0f);
                // (the selected track's label clears its corner brackets)
                const float off = sel ? std::max(24.0f * ui_.state().ui_scale, r_px * 1.25f)
                                      : std::max(9.0f, r_px * 0.8f);
                int tx = (int)(sp.x + off), ty = (int)(sp.y - off * 0.6f) - 8;
                int wlbl = (int)MeasureTextEx(label_font_, lbl, fs, 0.5f).x;
                int walt = alt ? (int)MeasureTextEx(label_font_, alt, afs, 0.5f).x : 0;
                int w = std::max(wlbl, walt);
                int h = (int)(fs + (alt ? afs : 0.0f));
                if (off > 16.0f)
                    DrawLineEx({ sp.x + off * 0.45f, sp.y - off * 0.27f }, { (float)tx - 4, (float)ty + h * 0.5f }, 1.0f,
                               { 160, 200, 230, (unsigned char)(a * 0.6f) });

                // Background plate keeps text legible over bright/busy terrain;
                // the stripe on the left is the track's type colour (as its trail).
                DrawRectangle(tx - 4, ty - 3, w + 8, h + 6,
                              { 6, 10, 18, (unsigned char)(a * 0.62f) });
                DrawRectangleLines(tx - 4, ty - 3, w + 8, h + 6,
                              sel ? Color{ 255, 220, 0, a }
                                  : Color{ 40, 90, 120, (unsigned char)(a * 0.7f) });
                if (const ecs::Trail* tr = fe.try_get<ecs::Trail>())
                    DrawRectangle(tx - 4, ty - 3, 3, h + 6, { tr->color.r, tr->color.g, tr->color.b, a });

                DrawTextEx(label_font_, lbl, { (float)tx, (float)ty }, fs, 0.5f,
                           sel ? Color{ 255, 220, 0, a } : Color{ 220, 236, 255, a });
                if (alt)
                    DrawTextEx(label_font_, alt, { (float)tx, (float)ty + fs }, afs, 0.5f,
                               { 120, 210, 255, (unsigned char)(a * 0.9f) });
            });
    }

    if (cfg_.demo_mode) {
        // Below the toolbar row so it never overlaps the transport buttons.
        const float k = ui_.state().ui_scale;
        int bw = static_cast<int>(440 * k), bh = static_cast<int>(28 * k), bx = GetScreenWidth()/2 - bw / 2,
            by = static_cast<int>(ui_.top_bar_height()) + 8;
        DrawRectangle(bx, by, bw, bh, {0,0,0,160});
        DrawRectangleLines(bx, by, bw, bh, {255,180,0,100});
        DrawTextEx(label_font_, "DEMO  \xe2\x80\x94  scripted scenario at Ak\xc4\xb1nc\xc4\xb1 (live UDP off)",
                   { (float)bx + 10 * k, (float)by + 4 * k }, 17.0f * k, 0.5f, {255,200,0,230});
    }
}


void Application::render_ui() {
    rlImGuiBegin();
    ui_.draw(playback_, store_, udp_receiver_.stats(), recorder_, world_);
    rlImGuiEnd();
}

// ── Input ─────────────────────────────────────────────────────────────────────
void Application::handle_input(float /*dt*/) {
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_Q)) running_ = false;
    if (IsKeyPressed(KEY_ESCAPE)) {   // close the settings window first, then deselect
        auto& st = ui_.state();
        if (st.show_settings_window) st.show_settings_window = false;
        else if (st.selected_entity.is_valid()) { st.selected_entity = {}; st.camera_mode = 0; }
    }
    // Keyboard shortcuts below are ignored while typing into a text field.
    if (ImGui::GetIO().WantTextInput) return;
    if (IsKeyPressed(KEY_H) && ui_.callbacks().on_go_home) ui_.callbacks().on_go_home();

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
        // Screen-space hit test: within the model's on-screen size (at least a
        // few pixels) of the cursor; the nearest such track to the cursor wins.
        // (A 3D distance-to-ray test caught almost every click when the camera
        // sat close behind a track.)
        const Vector2 mouse = GetMousePosition();
        const Vector3 cam_fwd = Vector3Normalize(Vector3Subtract(camera_.target, camera_.position));
        const Vector3 cam_right = Vector3Normalize(Vector3CrossProduct(cam_fwd, camera_.up));
        const float min_px = 14.0f * ui_.state().ui_scale;
        float best_score = 1e30f;
        flecs::entity best_ent{};
        const float exag = ui_.state().altitude_exaggerate;

        world_.query<const ecs::EntityMeta, const ecs::Position>()
            .each([&](flecs::entity e, const ecs::EntityMeta& meta, const ecs::Position& pos) {
                if (!meta.active) return;
                // Hit-test against the RENDERED position (altitude exaggeration
                // applied) — otherwise clicks miss whenever exaggeration > 1.
                const Vector3 rp = { pos.v.x, pos.v.y * exag, pos.v.z };
                if (Vector3DotProduct(Vector3Subtract(rp, camera_.position), cam_fwd) <= 0.0f) return;
                const ecs::RenderModel* rm = e.try_get<ecs::RenderModel>();
                const float len = display_length(rp, rm && rm->entry ? rm->entry->length : 20.0f);
                const Vector2 sp   = GetWorldToScreen(rp, camera_);
                const Vector2 edge = GetWorldToScreen(Vector3Add(rp, Vector3Scale(cam_right, len * 0.5f)), camera_);
                const float r = std::max(Vector2Distance(sp, edge), min_px);
                const float d = Vector2Distance(sp, mouse);
                if (d < r && d / r < best_score) {
                    best_score = d / r;
                    best_ent   = e;
                }
            });

        if (best_ent.is_valid()) {
            // Toggle selection
            ui_.state().selected_entity = (ui_.state().selected_entity == best_ent)
                ? flecs::entity{} : best_ent;
        }
    }
}

// Models are drawn at true size up close and grow with distance so a track
// stays readable from far away (the "Size" slider sets how much). The minimum
// is a length, so a missile grows as early as a fighter while a 100 m ship
// keeps its true size until it is far away.
float Application::display_length(Vector3 render_pos, float length) const {
    const float d = Vector3Distance(render_pos, camera_.position);
    return std::max(length, ui_.state().entity_3d_scale * d * 0.05f);
}

void Application::draw_entity_model(const ModelEntry& m, Vector3 rp, Quaternion rot, float scale) const {
    Vector3 axis{ 0, 1, 0 };
    float angle = 0.0f;
    QuaternionToAxisAngle(rot, &axis, &angle);
    const Vector3 sv{ scale, scale, scale };
    DrawModelEx(m.model, rp, axis, angle * RAD2DEG, sv, m.tint);

    // Rotors / propellers: spin about their own axis at the hub, then follow the body.
    const double t = GetTime();
    for (const Spinner& sp : m.spinners) {
        const float spin = static_cast<float>(std::fmod(t * sp.rev_per_sec, 1.0) * 2.0 * PI);
        Model part = sp.model;
        part.transform = MatrixMultiply(MatrixRotate(sp.axis, spin), MatrixTranslate(sp.hub.x, sp.hub.y, sp.hub.z));
        DrawModelEx(part, rp, axis, angle * RAD2DEG, sv, m.tint);
    }
}

// Soft dark disc on the ground under a model: anchors vehicles to the terrain
// and shows height for low flyers; fades out with height above ground.
void Application::draw_contact_shadow(Vector3 rp, float ground_y, float size) const {
    const float agl  = rp.y - ground_y;
    const float fade = 1.0f - std::clamp(agl / (size * 6.0f), 0.0f, 1.0f);
    if (fade <= 0.0f) return;
    const float exag = ui_.state().altitude_exaggerate;
    const float r    = size * 0.55f;
    const float lift = std::max(0.3f, Vector3Distance(rp, camera_.position) * 0.0015f);
    constexpr int kSegs = 24;
    Vector3 ring[kSegs];
    for (int i = 0; i < kSegs; ++i) {
        const float a = static_cast<float>(i) / kSegs * 2.0f * PI;
        const float x = rp.x + cosf(a) * r, z = rp.z + sinf(a) * r;
        ring[i] = { x, terrain_height_at(x, z) * exag + lift, z };
    }
    const Vector3 c{ rp.x, ground_y + lift, rp.z };
    const unsigned char a_in = static_cast<unsigned char>(110.0f * fade);
    rlBegin(RL_TRIANGLES);
    for (int i = 0; i < kSegs; ++i) {
        const Vector3& p0 = ring[i];
        const Vector3& p1 = ring[(i + 1) % kSegs];
        rlColor4ub(0, 0, 0, a_in); rlVertex3f(c.x, c.y, c.z);
        rlColor4ub(0, 0, 0, 0);    rlVertex3f(p1.x, p1.y, p1.z);
        rlColor4ub(0, 0, 0, 0);    rlVertex3f(p0.x, p0.y, p0.z);
    }
    rlEnd();
}

float Application::terrain_height_at(float wx, float wz) const {
    if (ui_.state().terrain_mode == 0) return 0.0f;
    return terrain_.height_at(wx, wz);
}

// A horizontal ground ring that hugs the terrain: each vertex is lifted to the
// terrain height at that point (plus a small offset), so it follows slopes
// instead of cutting a flat disc half-under the hill.
void Application::draw_ground_ring(float cx, float cz, float radius, Color col) const {
    // More segments so the polyline hugs the relief closely, and a lift that
    // scales with radius so the chord between samples doesn't dip into a hill.
    const int   segs = std::clamp(static_cast<int>(radius / 40.0f), 48, 160);
    const float exag = ui_.state().altitude_exaggerate;
    const float lift = std::max(3.0f, radius * 0.01f);
    // Draw with depth-test OFF so the marker is never hidden behind a hill — a
    // selection ring you can only half-see is worse than one drawn on top.
    rlDrawRenderBatchActive();   // flush queued 3D so the state change is clean
    rlDisableDepthTest();
    Vector3 prev{};
    for (int i = 0; i <= segs; ++i) {
        float a = (float)i / segs * 2.0f * PI;
        float x = cx + cosf(a) * radius;
        float z = cz + sinf(a) * radius;
        Vector3 cur{ x, terrain_height_at(x, z) * exag + lift, z };
        if (i > 0) DrawLine3D(prev, cur, col);
        prev = cur;
    }
    rlDrawRenderBatchActive();   // flush the ring while depth-test is still off
    rlEnableDepthTest();
}

// Selected track: corner brackets around the model on screen (they snap in
// from a larger size when the selection changes); an arrow on the screen edge
// when the track is off-screen or behind the camera.
void Application::draw_selection_marker(float exag) {
    flecs::entity sel = ui_.state().selected_entity;
    if (!sel.is_valid() || !sel.is_alive() || !sel.has<ecs::Position>()) { marker_entity_ = 0; return; }
    if (const ecs::EntityMeta* meta = sel.try_get<ecs::EntityMeta>(); meta && !meta->active) return;
    if (marker_entity_ != sel.id()) { marker_entity_ = sel.id(); marker_t0_ = GetTime(); }

    const Vector3 pos = sel.get<ecs::Position>().v;
    const Vector3 rp{ pos.x, pos.y * exag, pos.z };
    const ecs::RenderModel* rm = sel.try_get<ecs::RenderModel>();
    const float len = display_length(rp, rm && rm->entry ? rm->entry->length : 20.0f);

    const float k  = ui_.state().ui_scale;
    const Color col{ 255, 214, 10, 235 }, shade{ 0, 0, 0, 120 };
    const Vector3 fwd = Vector3Normalize(Vector3Subtract(camera_.target, camera_.position));
    const Vector3 to  = Vector3Subtract(rp, camera_.position);
    const bool ahead  = Vector3DotProduct(to, fwd) > 0.0f;
    const Vector2 sp  = GetWorldToScreen(rp, camera_);
    // Keep the marker in the part of the window the panels leave free.
    const Rectangle vr = ui_.view_rect();
    const float margin = 22.0f * k;
    const float left = vr.x + margin, right = vr.x + vr.width - margin;
    const float top = vr.y + margin, bottom = vr.y + vr.height - margin;

    if (ahead && sp.x > left && sp.x < right && sp.y > top && sp.y < bottom) {
        const Vector3 cam_right = Vector3Normalize(Vector3CrossProduct(fwd, camera_.up));
        const Vector2 edge  = GetWorldToScreen(Vector3Add(rp, Vector3Scale(cam_right, len * 0.5f)), camera_);
        float r = std::clamp(Vector2Distance(edge, sp) * 0.95f, 14.0f * k, 260.0f * k);
        const float t = static_cast<float>(GetTime() - marker_t0_) / 0.25f;     // snap-in
        if (t < 1.0f) r *= 1.0f + 0.8f * (1.0f - t) * (1.0f - t);
        const float arm = std::max(6.0f * k, r * 0.38f), th = std::max(1.5f, 2.0f * k);
        for (int i = 0; i < 4; ++i) {
            const float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f;
            const Vector2 c{ sp.x + sx * r, sp.y + sy * r };
            for (int pass = 0; pass < 2; ++pass) {      // dark halo, then the bracket
                const float w = pass ? th : th + 2.0f;
                const Color cc = pass ? col : shade;
                DrawLineEx(c, { c.x - sx * arm, c.y }, w, cc);
                DrawLineEx(c, { c.x, c.y - sy * arm }, w, cc);
            }
        }
        return;
    }

    // Off-screen (or under a panel): a triangle on the view's edge pointing toward the track.
    // Direction from the camera's own axes, which stays right for tracks behind
    // the camera or high above a camera looking down (projection flips there).
    const Vector2 c{ (left + right) * 0.5f, (top + bottom) * 0.5f };
    const Vector3 cam_x = Vector3Normalize(Vector3CrossProduct(fwd, camera_.up));
    const Vector3 cam_y = Vector3CrossProduct(cam_x, fwd);
    Vector2 d{ Vector3DotProduct(to, cam_x), -Vector3DotProduct(to, cam_y) };
    if (Vector2Length(d) < 1e-3f) d = { 0.0f, 1.0f };
    d = Vector2Normalize(d);
    const float hx = std::max(1.0f, (right - left) * 0.5f), hy = std::max(1.0f, (bottom - top) * 0.5f);
    const float sx = std::fabs(d.x) > 1e-4f ? hx / std::fabs(d.x) : 1e9f;
    const float sy = std::fabs(d.y) > 1e-4f ? hy / std::fabs(d.y) : 1e9f;
    const Vector2 tip{ c.x + d.x * std::min(sx, sy), c.y + d.y * std::min(sx, sy) };
    const Vector2 n{ -d.y, d.x };
    const float L = 16.0f * k, W = 8.0f * k;
    const Vector2 base{ tip.x - d.x * L, tip.y - d.y * L };
    const Vector2 a{ base.x + n.x * W, base.y + n.y * W }, b{ base.x - n.x * W, base.y - n.y * W };
    DrawTriangle(tip, b, a, col);
    DrawTriangle(tip, a, b, col);   // either winding
    DrawTriangleLines(tip, a, b, shade);
}

TerrainStyle Application::terrain_style() const {
    const auto& st = ui_.state();
    TerrainStyle s;
    s.look   = static_cast<TerrainLook>(std::clamp(st.terrain_mode, 0, 4));
    s.vscale = st.altitude_exaggerate;
    s.detail = st.terrain_detail;
    s.fog    = kSkyHorizon;

    // Fog follows the zoom (and the camera's height, which widens the horizon).
    const float cam_dist = st.camera_distance;
    const float cam_agl  = std::max(0.0f, camera_.position.y
                                         - terrain_height_at(camera_.position.x, camera_.position.z) * s.vscale);
    s.fog_end   = std::clamp(cam_dist * 7.0f + cam_agl * 12.0f, 25000.0f, 1200000.0f);
    s.fog_start = s.fog_end * 0.30f;

    // Grid every 10^n m, about a quarter of the view distance; contours at a
    // "nice" interval giving roughly 100 lines across the view.
    s.grid_spacing = std::pow(10.0f, std::floor(std::log10(std::max(cam_dist * 0.3f, 10.0f))));
    const float raw = cam_dist / 120.0f;
    float step = 10.0f;
    for (float c : {10.0f, 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f})
        if (c <= raw) step = c;
    s.contour_step = step;
    return s;
}

// Ray through the mouse cursor, built from the camera's own axes. raylib's
// GetMouseRay() unprojects with the *current* clip planes (reset to 0.01–1000 m
// after drawing), which loses all precision — even NaN — when the camera is
// hundreds of kilometres from the scene.
Ray Application::mouse_ray() const {
    const Vector2 m = GetMousePosition();
    const float w = static_cast<float>(GetScreenWidth()), h = static_cast<float>(GetScreenHeight());
    const float nx = 2.0f * m.x / w - 1.0f, ny = 1.0f - 2.0f * m.y / h;
    const float t = std::tan(camera_.fovy * 0.5f * DEG2RAD);
    const Vector3 fwd   = Vector3Normalize(Vector3Subtract(camera_.target, camera_.position));
    const Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, camera_.up));
    const Vector3 up    = Vector3CrossProduct(right, fwd);
    const Vector3 dir = Vector3Normalize(Vector3Add(fwd, Vector3Add(Vector3Scale(right, nx * t * w / h),
                                                                   Vector3Scale(up, ny * t))));
    return Ray{ camera_.position, dir };
}

void Application::look_at(float x, float z) {
    camera_ctl_.fly_to({ x, terrain_height_at(x, z) * ui_.state().altitude_exaggerate, z });
}

void Application::update_scene_info() {
    SceneInfo& sc = ui_.state().scene;
    sc.demo = cfg_.demo_mode;
    sc.fps = GetFPS();
    sc.origin_lat = geo_.lat0();
    sc.origin_lon = geo_.lon0();

    // Terrain under the mouse (skip while the mouse is over a panel).
    sc.cursor_valid = false;
    if (!ImGui::GetIO().WantCaptureMouse && terrain_.has_data() && ui_.state().terrain_mode > 0) {
        const float exag = ui_.state().altitude_exaggerate;
        Vector3 hit;
        if (terrain_.raycast(mouse_ray(), exag, 1500000.0f, &hit) &&
            std::isfinite(hit.x) && std::isfinite(hit.z)) {
            sc.cursor_valid = true;
            geo_.to_geo(hit.x, hit.z, &sc.cursor_lat, &sc.cursor_lon);
            sc.cursor_elev = terrain_.height_at(hit.x, hit.z);
            sc.cursor_res = terrain_.resolution_at(hit.x, hit.z);
        }
    }

    const TerrainStats ts = terrain_.stats();
    sc.chunks_drawn = ts.drawn;
    sc.chunks_cached = ts.cached;
    sc.chunks_queued = ts.queued;
    sc.terrain_gpu_mb = ts.gpu_mb;
    if (sc.terrain_layers.empty()) {
        sc.terrain_status = terrain_.status();
        for (const auto& L : terrain_.db().layers()) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%-13s %6.1f m  %.2f-%.2fN %.2f-%.2fE", L.name.c_str(),
                     L.resolution_m, L.south(), L.north, L.west, L.east());
            sc.terrain_layers.push_back(buf);
        }
    }
    sc.cam_x = camera_.position.x;
    sc.cam_z = camera_.position.z;
    sc.cam_yaw_deg = std::atan2(camera_.target.x - camera_.position.x,
                                -(camera_.target.z - camera_.position.z)) * RAD2DEG;
}

void Application::update_minimap() {
    SceneInfo& sc = ui_.state().scene;
    // Finished job → upload.
    if (minimap_job_.valid() &&
        minimap_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::vector<unsigned char> px = minimap_job_.get();
        if (minimap_pending_key_.origin != origin_epoch_) {
            minimap_key_ = {};   // rendered for an old origin — discard, render again
        } else if (!minimap_tex_.id) {
            Image img{ px.data(), kMinimapPx, kMinimapPx, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
            minimap_tex_ = LoadTextureFromImage(img);
            SetTextureFilter(minimap_tex_, TEXTURE_FILTER_BILINEAR);
        } else {
            UpdateTexture(minimap_tex_, px.data());
        }
        if (minimap_pending_key_.origin == origin_epoch_) {
            minimap_key_ = minimap_pending_key_;
            sc.minimap = minimap_tex_;
            sc.map_cx = minimap_key_.cx;
            sc.map_cz = minimap_key_.cz;
            sc.map_range = minimap_key_.range;
        }
    }
    if (!terrain_.has_data() || minimap_job_.valid()) return;

    // Area: all active tracks (or the view when there are none), as a power-of-
    // two range snapped to an eighth of itself so it only re-renders now and then.
    float xmin = 1e30f, xmax = -1e30f, zmin = 1e30f, zmax = -1e30f;
    world_.query<const ecs::EntityMeta, const ecs::Position>()
        .each([&](const ecs::EntityMeta& m, const ecs::Position& p) {
            if (!m.active) return;
            xmin = std::min(xmin, p.v.x); xmax = std::max(xmax, p.v.x);
            zmin = std::min(zmin, p.v.z); zmax = std::max(zmax, p.v.z);
        });
    float cx, cz, need;
    if (xmin > xmax) {
        cx = camera_.target.x; cz = camera_.target.z;
        need = ui_.state().camera_distance * 3.0f;
    } else {
        cx = 0.5f * (xmin + xmax); cz = 0.5f * (zmin + zmax);
        need = std::max(xmax - xmin, zmax - zmin) * 1.25f;
    }
    need = std::clamp(need, 4000.0f, 1600000.0f);
    const float range = std::pow(2.0f, std::ceil(std::log2(need)));
    const float snap = range / 8.0f;
    MapKey key{ std::round(cx / snap) * snap, std::round(cz / snap) * snap, range, origin_epoch_ };
    if (key.cx == minimap_key_.cx && key.cz == minimap_key_.cz && key.range == minimap_key_.range &&
        key.origin == minimap_key_.origin && minimap_tex_.id)
        return;
    minimap_pending_key_ = key;
    const GeoFrame frame = geo_;
    minimap_job_ = std::async(std::launch::async, [this, frame, key] {
        return terrain_.render_map(frame, key.cx, key.cz, key.range, kMinimapPx);
    });
}

void Application::draw_terrain() {
    terrain_.draw(camera_, terrain_style());
}

} // namespace afteraction
