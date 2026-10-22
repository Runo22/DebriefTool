#pragma once
#include "../network/UdpReceiver.hpp"
#include "../buffer/SPSCQueue.hpp"
#include "../buffer/TelemetryStore.hpp"
#include "../ecs/Components.hpp"
#include "../ecs/Systems.hpp"
#include "../render/AssetManager.hpp"
#include "../render/TrailRenderer.hpp"
#include "../playback/PlaybackController.hpp"
#include "../persistence/Recorder.hpp"
#include "../ui/AfterActionUI.hpp"
#include "../demo/DemoMode.hpp"
#include "../geo/GeoFrame.hpp"
#include "../terrain/TerrainSystem.hpp"
#include "../terrain/Airfields.hpp"
#include "CameraController.hpp"
#include <flecs.h>
#include <raylib.h>
#include <future>
#include <unordered_map>

namespace afteraction {

struct AppConfig {
    uint16_t    udp_port      = 22522;
    std::string bind_addr     = "0.0.0.0";
    bool        port_from_cli = false;    // --port / --addr override the saved settings
    bool        addr_from_cli = false;
    int         window_width  = 1600;
    int         window_height = 900;
    std::string window_title  = "AfterAction  |  Tactical Debrief";
    int         target_fps    = 60;
    bool        demo_mode     = false;
    std::string terrain_dir;              // empty = <exe dir>/terrain
    std::string open_file;                // .aar / .csv to load at startup
};

class Application {
public:
    explicit Application(AppConfig cfg = {});
    ~Application();

    void run();

private:
    void init_window();
    void init_ecs();
    void init_assets();
    void init_camera();
    void init_terrain();
    void init_ui_callbacks();

    void tick(float dt);
    void process_inbound_queue();
    void process_demo(float dt);
    void update_ecs(float dt);
    void render();
    void render_3d();
    void draw_terrain();
    TerrainStyle terrain_style() const;

    // Terrain height (m MSL, real scale) under a world (x,z) — 0 when terrain is
    // hidden or the point is outside DEM coverage. Multiply by the vertical
    // scale for rendering.
    float terrain_height_at(float wx, float wz) const;
    // On-screen length (m) of a model of real length `length` at render_pos:
    // true size up close, never smaller than a distance-based minimum.
    float display_length(Vector3 render_pos, float length) const;
    void draw_entity_model(const ModelEntry& m, Vector3 rp, Quaternion rot, float scale) const;
    void draw_contact_shadow(Vector3 rp, float ground_y, float size) const;
    void draw_selection_marker(float exag);
    uint64_t marker_entity_ = 0;   // selection the marker animation started for
    double   marker_t0_     = 0.0;

    // A ground ring that follows the terrain relief (vertices lifted to terrain
    // height) — used for selection/altitude markers so they don't sink into hills.
    void draw_ground_ring(float cx, float cz, float radius, Color col) const;

    // Scene origin. Changing it re-centres the world and rebuilds the terrain.
    void set_origin(double lat, double lon);

    // Per-frame read-outs for the UI (cursor position, terrain status, minimap).
    void update_scene_info();
    void update_minimap();
    // Move the free camera so it looks at a world (x,z), keeping the zoom.
    void look_at(float x, float z);
    // Ray from the camera through the mouse cursor (precise at any distance).
    Ray mouse_ray() const;
    void render_ui();
    void handle_input(float dt);

    // Called for both live and demo paths — converts EntityState to ECS.
    void apply_state_to_ecs(net::EntityState& state);

    // Syncs ECS entities to the selected playback timestamp using TelemetryStore
    void sync_ecs_to_playback(uint64_t target_ns);

    // Destroys every tracked entity and clears the telemetry store, returning to
    // a clean live state (also resets the ENU origin so it re-seeds on next data).
    void clear_all_entities();

    // Restarts the UDP receiver on a new bind address / port (from the UI).
    void apply_network_settings(const std::string& bind_addr, uint16_t port);
    void start_listener();

    // Write the settings when they changed (checked about once a second).
    void autosave_settings();

    // After a model finishes async-loading, point existing entities of that type
    // at the new model (newly spawned entities pick it up via get_for_type).
    void repoint_entities_of_type(uint16_t type);


    // Replay-file loaders (shared by the typed path, and the native file dialog).
    void load_session(const std::string& path);   // .aar recording
    void import_csv(const std::string& path);      // flexible CSV log
    void browse_and_load();                        // native open dialog → dispatch

    // Converts lat/lon/alt to world metres (fills position/orientation/velocity).
    // The first live track re-centres the scene if it is far from the current
    // origin (so data anywhere works), otherwise the origin is kept.
    void ensure_origin_and_convert(net::EntityState& state);

    flecs::entity get_or_create_entity(const net::EntityState& state);

    uint64_t entity_key(uint32_t src, uint32_t eid) const noexcept {
        return (static_cast<uint64_t>(src) << 32) | eid;
    }

    // ── Filesystem layout (everything relative to the EXECUTABLE, not the cwd) ──
    // Directory containing the running executable (with trailing separator).
    std::string app_dir() const;
    // <app_dir>/assets/<rel>  — bundled assets are copied next to the exe.
    std::string asset_path(const std::string& rel) const;
    // <app_dir>/recordings/ , created on demand. Sessions/dashcams are saved here.
    std::string recordings_dir() const;
    // <app_dir>/afteraction_config.yaml
    std::string config_path() const;

    // ── Subsystems ────────────────────────────────────────────────────────────
    AppConfig cfg_;

    flecs::world world_;
    std::unordered_map<uint64_t, flecs::entity> entity_map_;

    SPSCQueue<net::ParsedFrame, 4096> inbound_queue_;
    net::UdpReceiver                  udp_receiver_;

    TelemetryStore     store_;
    PlaybackController playback_;

    Camera3D           camera_{};
    AssetManager       assets_;
    TrailRenderer      trails_;

    persist::Recorder  recorder_;
    AfterActionUI          ui_;

    demo::DemoMode     demo_;
    CameraController   camera_ctl_;

    // Scene origin (WGS-84) and the real-terrain renderer built around it.
    GeoFrame      geo_;
    TerrainSystem terrain_;
    Airfields     airfields_;
    Font          label_font_{};   // 3D labels (Barlow SemiBold)

    // Minimap hillshade: built on a worker, uploaded here.
    struct MapKey { float cx = 0, cz = 0, range = 0; };
    Texture2D     minimap_tex_{};
    MapKey        minimap_key_{}, minimap_pending_key_{};
    std::future<std::vector<unsigned char>> minimap_job_;
    static constexpr int kMinimapPx = 256;
    // Set once live data has decided the origin; cleared by Clear.
    bool          origin_locked_ = false;

    // Flat buffer of current live entity states (for store ingestion).
    std::vector<net::EntityState> live_states_;

    // Live render clock: runs with real time and is steered to stay a little
    // behind the newest sample, so tracks interpolate between packets instead
    // of stepping to each one (and cameras following them stay smooth).
    void     note_live_sample(uint64_t ts_ns);
    uint64_t advance_live_clock(float dt);
    uint64_t live_newest_ns_ = 0;      // newest sample timestamp
    double   live_newest_at_ = 0.0;    // GetTime() when it arrived
    double   live_interval_  = 0.05;   // smoothed time between samples (s)
    double   live_clock_ns_  = 0.0;    // render time

    bool running_ = true;
    std::string saved_config_;          // last settings text written to disk
    double      next_autosave_check_ = 0.0;
    // Set by the UI "Clear" callback; the actual wipe runs at the top of the
    // next tick() (shallow stack, outside ECS iteration / ImGui rendering).
    bool clear_requested_ = false;
};

} // namespace afteraction
