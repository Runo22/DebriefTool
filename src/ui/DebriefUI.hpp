#pragma once
#include "../playback/PlaybackController.hpp"
#include "../buffer/TelemetryStore.hpp"
#include "../network/UdpReceiver.hpp"
#include "../persistence/Recorder.hpp"
#include <flecs.h>
#include <functional>
#include <optional>
#include <string>

namespace debrief {

// ─────────────────────────────────────────────────────────────────────────────
//  DebriefUI  —  entire Dear ImGui + ImPlot overlay
//
//  Owns no heavy state — holds non-owning references to app subsystems.
//  All draw calls must happen between rlImGuiBegin() / rlImGuiEnd().
// ─────────────────────────────────────────────────────────────────────────────

struct UICallbacks {
    std::function<void()>               on_record_start;
    std::function<void()>               on_record_stop;
    std::function<void(float)>          on_save_dashcam;       // arg: seconds
    std::function<void(std::string)>    on_load_file;
    std::function<void(std::string)>    on_load_csv;
    std::function<void(uint16_t, std::string)> on_load_model;
    std::function<void()>               on_clear_entities;     // wipe all tracks
    std::function<void(std::string, uint16_t)> on_apply_network; // bind addr, port
};

struct UIState {
    // Timeline scrubber
    float   scrub_frac    = 1.0f;
    bool    scrub_dragging = false;

    // Inspector
    flecs::entity selected_entity{};

    // Network panel
    uint16_t listen_port = 5555;
    char     bind_addr[64] = "0.0.0.0";

    // Dashcam
    float dashcam_secs = 60.0f;

    // Session file loading (path typed into the Settings → Session Files panel)
    char load_path[256] = "";

    // Display options
    bool show_trails       = true;
    bool show_labels       = true;
    bool show_velocity_vec = false;
    bool show_stats        = true;
    bool show_minimap      = true;
    bool show_settings_window = false;

    // Trail mode toggle (line vs ribbon)
    bool ribbon_trails = false;

    // Camera & Visual settings
    int   camera_mode = 0;                 // 0 = Free Orbit, 1 = Focus Target, 2 = Chase Target
    float camera_yaw = 45.0f;
    float camera_pitch = 25.0f;
    float camera_distance = 2000.0f;
    float entity_3d_scale = 20.0f;         // Visual scale multiplier
    float trail_width_override = 15.0f;    // Trail width override in meters

    // Chase camera offsets (modified by RMB drag while in mode 2)
    float chase_yaw_offset   =   0.0f;    // degrees, lateral swing around entity
    float chase_pitch_offset =  20.0f;    // degrees, 0=dead-behind, +ve=look down from above

    // Mouse look tuning
    bool  invert_look      = true;        // RMB orbit: drag grabs the scene (inverted)
    float mouse_sensitivity = 1.0f;       // multiplier on RMB rotate speed

    // Altitude exaggeration: multiplies entity Y for rendering to make height differences visible
    float altitude_exaggerate = 3.0f;     // 1 = real scale, 3 = 3x vertical stretch

    // Render settings
    float far_clip_plane = 2000000.0f;

    // Timeline / altitude chart panel height (resizable from Settings → Display)
    float timeline_height = 210.0f;       // total height of the bottom panel

    // Terrain settings
    int   terrain_mode = 3; // 0=None, 1=Wireframe, 2=Solid, 3=Both
    float terrain_height_scale = 1.0f;
};

class DebriefUI {
public:
    DebriefUI() = default;

    void set_callbacks(UICallbacks cbs) { cbs_ = std::move(cbs); }

    // Draw all ImGui panels.  Call every frame between rlImGuiBegin/End.
    void draw(PlaybackController& pb,
              const TelemetryStore& store,
              const net::ReceiverStats& net_stats,
              const persist::Recorder& recorder,
              const flecs::world& world);

    [[nodiscard]] UIState& state() noexcept { return state_; }
    [[nodiscard]] const UIState& state() const noexcept { return state_; }

private:
    void draw_toolbar(PlaybackController& pb,
                      const persist::Recorder& recorder);

    void draw_timeline(PlaybackController& pb,
                       const TelemetryStore& store,
                       const flecs::world& world);

    void draw_entity_list(const flecs::world& world, float bottom_offset);

    void draw_inspector(const flecs::world& world);

    void draw_network_panel(const net::ReceiverStats& stats, float bottom_offset);

    void draw_minimap(const flecs::world& world, float bottom_offset);
    void draw_settings_window();

    UIState    state_{};
    UICallbacks cbs_{};

    // ImPlot altitude/speed telemetry for the selected entity.
    // Ring buffer sampled on playback time (not per-frame), so the chart keeps
    // scrolling forever instead of freezing once 512 frames have elapsed.
    // X values are seconds since session start (matches the T+ readout).
    static constexpr int   kPlotWindow     = 512;
    static constexpr float kPlotSampleSec  = 0.1f;   // ~51 s of visible history
    float plot_time_[kPlotWindow]{};
    float plot_alt_[kPlotWindow]{};     // feet
    float plot_speed_[kPlotWindow]{};   // knots
    int   plot_count_ = 0;
    int   plot_head_  = 0;              // next write slot (ring)
    flecs::entity last_plot_entity_{};
};

} // namespace debrief
