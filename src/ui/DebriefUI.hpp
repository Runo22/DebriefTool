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

    // Display options
    bool show_trails       = true;
    bool show_labels       = true;
    bool show_velocity_vec = false;
    bool show_stats        = true;
    bool show_minimap      = true;

    // Trail mode toggle (line vs ribbon)
    bool ribbon_trails = false;
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
                       const TelemetryStore& store);

    void draw_entity_list(const flecs::world& world);

    void draw_inspector(const flecs::world& world);

    void draw_network_panel(const net::ReceiverStats& stats);

    void draw_minimap(const flecs::world& world);

    UIState    state_{};
    UICallbacks cbs_{};

    // ImPlot altitude/speed telemetry data for the selected entity.
    // Linear buffer reset whenever the selected entity changes.
    static constexpr int kPlotWindow = 512;
    float plot_time_[kPlotWindow]{};
    float plot_alt_[kPlotWindow]{};
    float plot_speed_[kPlotWindow]{};
    int   plot_count_= 0;
    flecs::entity last_plot_entity_{};
};

} // namespace debrief
