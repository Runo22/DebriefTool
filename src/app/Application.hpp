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
#include "../ui/DebriefUI.hpp"
#include "../demo/DemoMode.hpp"
#include <flecs.h>
#include <raylib.h>
#include <unordered_map>

namespace debrief {

struct AppConfig {
    uint16_t    udp_port      = 5555;
    std::string bind_addr     = "0.0.0.0";
    int         window_width  = 1600;
    int         window_height = 900;
    std::string window_title  = "Debrief  |  Tactical Analysis";
    int         target_fps    = 60;
    bool        demo_mode     = false;
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
    void init_ui_callbacks();

    void tick(float dt);
    void process_inbound_queue();
    void process_demo(float dt);
    void update_ecs(float dt);
    void render();
    void render_3d();
    void draw_terrain();

    // Procedural terrain surface height at a world (x,z). Mirrors the formula in
    // draw_terrain() so ground markers/drop-lines can sit on the terrain even when
    // it has relief. Returns 0 when terrain is disabled.
    float terrain_height_at(float wx, float wz) const;
    void render_ui();
    void handle_input(float dt);
    void update_camera_state(float dt);

    // Called for both live and demo paths — converts EntityState to ECS.
    void apply_state_to_ecs(net::EntityState& state);

    // Syncs ECS entities to the selected playback timestamp using TelemetryStore
    void sync_ecs_to_playback(uint64_t target_ns);

    // Destroys every tracked entity and clears the telemetry store, returning to
    // a clean live state (also resets the ENU origin so it re-seeds on next data).
    void clear_all_entities();

    // Restarts the UDP receiver on a new bind address / port (from the UI).
    void apply_network_settings(const std::string& bind_addr, uint16_t port);

    // Lazy-set scene origin from first received position; converts subsequent
    // lat/lon/alt to ENU metres and fills state.position[].
    // For demo states (position already ENU), pass enu_already=true.
    void ensure_origin_and_convert(net::EntityState& state, bool enu_already = false);

    flecs::entity get_or_create_entity(const net::EntityState& state);

    uint64_t entity_key(uint32_t src, uint32_t eid) const noexcept {
        return (static_cast<uint64_t>(src) << 32) | eid;
    }

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
    DebriefUI          ui_;

    demo::DemoMode     demo_;
    Vector3            camera_free_target_{ 0.0f, 0.0f, 0.0f };

    // Scene origin (WGS84) — set from first entity's position.
    double   origin_lat_ = 0.0, origin_lon_ = 0.0;
    float    origin_alt_ = 0.0f;
    bool     origin_set_ = false;

    // Flat buffer of current live entity states (for store ingestion).
    std::vector<net::EntityState> live_states_;

    bool running_ = true;
    // Set by the UI "Clear" callback; the actual wipe runs at the top of the
    // next tick() (shallow stack, outside ECS iteration / ImGui rendering).
    bool clear_requested_ = false;
};

} // namespace debrief
