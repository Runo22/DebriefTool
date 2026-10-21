#pragma once
#include "../playback/PlaybackController.hpp"
#include "../buffer/TelemetryStore.hpp"
#include "../network/UdpReceiver.hpp"
#include "../persistence/Recorder.hpp"
#include <flecs.h>
#include <imgui.h>
#include <raylib.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace afteraction {

// ─────────────────────────────────────────────────────────────────────────────
//  AfterActionUI  —  entire Dear ImGui + ImPlot overlay
//
//  Owns no heavy state — holds non-owning references to app subsystems.
//  All draw calls must happen between rlImGuiBegin() / rlImGuiEnd().
//
//  Layout:  top bar (transport, status, cursor read-out) · left track list ·
//           right inspector + terrain minimap · bottom timeline / chart.
// ─────────────────────────────────────────────────────────────────────────────

// A request to bind a model to an entity type (from the Settings → Models tab).
struct ModelBindRequest {
    uint16_t    type  = 1;
    std::string file;                     // relative to assets/ (e.g. "models/f16.glb")
    float       scale = 1.0f;
    uint8_t     tint[3] = {255,255,255};
    float       yaw = 0, pitch = 0, roll = 0;  // base-rotation, degrees
};

struct UICallbacks {
    std::function<void()>               on_record_start;
    std::function<void()>               on_record_stop;
    std::function<void(float)>          on_save_dashcam;       // arg: seconds
    std::function<void(std::string)>    on_load_file;
    std::function<void(std::string)>    on_load_csv;
    std::function<void(uint16_t, std::string)> on_load_model;
    std::function<void()>               on_clear_entities;     // wipe all tracks
    std::function<void(std::string, uint16_t)> on_apply_network; // bind addr, port
    std::function<void(const ModelBindRequest&)> on_model_load;  // load/replace model for a type
    std::function<void(uint16_t)>                on_model_clear; // revert type to procedural
    std::function<void(const std::vector<ModelBindRequest>&)> on_models_save; // persist manifest
    std::function<void()>               on_browse_file;        // native open dialog → load replay/CSV
    std::function<void()>               on_go_home;            // fly the camera to the home origin
    std::function<void()>               on_set_home_here;      // home origin = camera target
    std::function<void(float, float)>   on_look_at;            // centre the view on world (x,z)

    // World (x,z) → WGS-84, and terrain height (m MSL) under a world point.
    std::function<void(float, float, double*, double*)> to_geo;
    std::function<float(float, float)>                  ground_height;
};

// Read-only scene facts the application refreshes every frame for display.
struct SceneInfo {
    bool   demo = false;
    int    fps = 0;
    double origin_lat = 0.0, origin_lon = 0.0;

    // Terrain point under the mouse cursor.
    bool   cursor_valid = false;
    double cursor_lat = 0.0, cursor_lon = 0.0;
    float  cursor_elev = 0.0f, cursor_res = 0.0f;

    // Terrain data / renderer status.
    std::string terrain_status;
    std::vector<std::string> terrain_layers;
    int    chunks_drawn = 0, chunks_cached = 0, chunks_queued = 0;
    double terrain_gpu_mb = 0.0;

    // Minimap: a hillshade texture covering [cx ± range/2, cz ± range/2] (world).
    Texture2D minimap{};
    float  map_cx = 0.0f, map_cz = 0.0f, map_range = 0.0f;
    float  cam_x = 0.0f, cam_z = 0.0f, cam_yaw_deg = 0.0f;
};

struct UIState {
    // Timeline scrubber
    float   scrub_frac    = 1.0f;
    bool    scrub_dragging = false;

    // Inspector
    flecs::entity selected_entity{};

    // Network panel
    uint16_t listen_port = 22522;
    char     bind_addr[64] = "0.0.0.0";

    // Dashcam
    float dashcam_secs = 60.0f;

    // Session file loading (path typed into the Settings → Session Files panel)
    char load_path[256] = "";

    // Display options
    bool show_trails       = true;
    bool show_labels       = true;
    bool show_velocity_vec = false;
    bool show_stats        = false;
    bool show_minimap      = true;
    bool show_settings_window = false;

    // Trail mode toggle (line vs ribbon)
    bool ribbon_trails = false;

    // Camera & Visual settings
    int   camera_mode = 0;                 // 0 = Free Orbit, 1 = Focus Target, 2 = Chase Target
    float camera_yaw = 45.0f;
    float camera_pitch = 25.0f;
    float camera_distance = 2000.0f;
    float entity_3d_scale = 1.0f;          // on-screen size of track models (grows with distance)
    float trail_width_override = 15.0f;    // Trail width override in meters

    // Chase camera offsets (modified by RMB drag while in mode 2)
    float chase_yaw_offset   =   0.0f;    // degrees, lateral swing around entity
    float chase_pitch_offset =  20.0f;    // degrees, 0=dead-behind, +ve=look down from above

    // Mouse look tuning
    bool  invert_look      = true;        // RMB orbit: drag grabs the scene (inverted)
    float mouse_sensitivity = 1.0f;       // multiplier on RMB rotate speed

    // Vertical scale for the whole scene (terrain AND tracks), so aircraft on a
    // runway stay on it. 1 = real scale.
    float altitude_exaggerate = 1.0f;

    // Render settings
    float far_clip_plane = 2000000.0f;

    // Timeline / altitude chart panel height (resizable from Settings → Display)
    float timeline_height = 230.0f;       // total height of the bottom panel

    // Terrain (real DEM). Mode = TerrainLook: 0 None, 1 Grid, 2 Shaded,
    // 3 Shaded + grid, 4 Contours.
    int   terrain_mode   = 3;
    float terrain_detail = 1.4f;          // LOD distance factor

    // Home origin used before data arrives (Akıncı Air Base by default) and the
    // current scene origin (read-only, for display).
    double origin_lat = 40.0789, origin_lon = 32.5656;
    double scene_lat  = 0.0,     scene_lon  = 0.0;

    // Filled by the application every frame.
    SceneInfo scene;

    // ── Settings → Models tab: editable type → model bindings ─────────────────
    // Seeded from assets/models.yaml at startup; edited live; saved back to it.
    struct ModelBinding {
        int   type = 1;                 // EntityTypeId
        char  file[256] = "";           // relative to assets/ (e.g. "models/f16.glb")
        float scale = 1.0f;
        float tint[3] = {1, 1, 1};
        float yaw = 0, pitch = 0, roll = 0;  // base-rotation, degrees
        char  status[48] = "";          // last action feedback
    };
    std::vector<ModelBinding> model_bindings;
};

class AfterActionUI {
public:
    AfterActionUI() = default;

    void set_callbacks(UICallbacks cbs) { cbs_ = std::move(cbs); }
    void set_mono_font(ImFont* f)    { mono_font_ = f; }
    void set_heading_font(ImFont* f) { heading_font_ = f; }

    // Apply the colour theme / metrics once after ImGui is initialised.
    static void apply_theme();

    // Draw all ImGui panels.  Call every frame between rlImGuiBegin/End.
    void draw(PlaybackController& pb,
              const TelemetryStore& store,
              const net::ReceiverStats& net_stats,
              const persist::Recorder& recorder,
              const flecs::world& world);

    [[nodiscard]] UIState& state() noexcept { return state_; }
    [[nodiscard]] const UIState& state() const noexcept { return state_; }

    // Height of the bar docked at the top (3D overlays stay below it).
    static constexpr float kTopBarHeight = 44.0f;

private:
    void draw_top_bar(PlaybackController& pb, const TelemetryStore& store,
                      const net::ReceiverStats& stats, const persist::Recorder& recorder);
    void draw_timeline(PlaybackController& pb, const TelemetryStore& store,
                       const flecs::world& world, float height);
    void draw_track_panel(const flecs::world& world, float bottom_offset);
    void draw_tracks_tab(const flecs::world& world);
    void draw_view_tab();
    void draw_right_column(const flecs::world& world, const net::ReceiverStats& stats,
                           float bottom_offset);
    void draw_inspector(const flecs::world& world);
    void draw_minimap(const flecs::world& world);
    void draw_network(const net::ReceiverStats& stats);
    void draw_settings_window();

    float timeline_height(const TelemetryStore& store) const;

    UIState     state_{};
    UICallbacks cbs_{};
    ImFont*     mono_font_    = nullptr;
    ImFont*     heading_font_ = nullptr;

    // ImPlot altitude/speed telemetry for the selected entity.
    // Ring buffer sampled on playback time (not per-frame), so the chart keeps
    // scrolling forever instead of freezing once 512 frames have elapsed.
    // X values are seconds since session start (matches the T+ readout).
    static constexpr int   kPlotWindow     = 512;
    static constexpr float kPlotSampleSec  = 0.1f;   // ~51 s of visible history
    float plot_time_[kPlotWindow]{};
    float plot_alt_[kPlotWindow]{};     // feet MSL
    float plot_gnd_[kPlotWindow]{};     // feet MSL, terrain under the track
    float plot_speed_[kPlotWindow]{};   // knots
    int   plot_count_ = 0;
    int   plot_head_  = 0;              // next write slot (ring)
    flecs::entity last_plot_entity_{};
};

} // namespace afteraction
