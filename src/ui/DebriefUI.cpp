#include "DebriefUI.hpp"
#include "../ecs/Components.hpp"
#include <imgui.h>
#include <implot.h>
#include <rlImGui.h>
#include <raylib.h>
#include <cstdio>
#include <cstring>

namespace debrief {

using namespace ecs;

// ── Icons and naming ─────────────────────────────────────────────────────────
static const char* entity_type_icon(uint16_t t) {
    switch (t) {
    case net::TYPE_JET:     return ICON_FA_PLANE;
    case net::TYPE_MISSILE: return ICON_FA_ROCKET;
    case net::TYPE_AAA:     return ICON_FA_SHIELD;
    case net::TYPE_GROUND:  return ICON_FA_TRUCK;
    case net::TYPE_HELO:    return ICON_FA_HELICOPTER;
    case net::TYPE_SHIP:    return ICON_FA_SHIP;
    default:                return ICON_FA_CIRCLE_QUESTION;
    }
}

static const char* entity_type_name(uint16_t t) {
    switch (t) {
    case net::TYPE_JET:     return "Jet";
    case net::TYPE_MISSILE: return "Missile";
    case net::TYPE_AAA:     return "AAA";
    case net::TYPE_GROUND:  return "Ground";
    case net::TYPE_HELO:    return "Helo";
    case net::TYPE_SHIP:    return "Ship";
    default:                return "Unknown";
    }
}

static void format_duration(uint64_t ns, char* buf, size_t n) {
    uint64_t sec  = ns / 1'000'000'000ULL;
    uint64_t ms   = (ns % 1'000'000'000ULL) / 1'000'000ULL;
    uint64_t min  = sec / 60; sec %= 60;
    uint64_t hour = min / 60; min %= 60;
    snprintf(buf, n, "%02llu:%02llu:%02llu.%03llu",
             (unsigned long long)hour,
             (unsigned long long)min,
             (unsigned long long)sec,
             (unsigned long long)ms);
}

// ── Custom cybernetic dark slate / navy theme ────────────────────────────────
static void apply_premium_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    
    // Rounded futuristic styling
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    
    ImVec4* colors = style.Colors;
    
    // Cozy digital command deck colors
    colors[ImGuiCol_Text]                   = ImVec4(0.85f, 0.92f, 0.98f, 1.00f); 
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.40f, 0.50f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.06f, 0.08f, 0.13f, 0.88f); 
    colors[ImGuiCol_ChildBg]                = ImVec4(0.04f, 0.06f, 0.10f, 0.50f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.06f, 0.08f, 0.14f, 0.96f);
    colors[ImGuiCol_Border]                 = ImVec4(0.18f, 0.28f, 0.40f, 0.75f); 
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    
    colors[ImGuiCol_FrameBg]                = ImVec4(0.09f, 0.13f, 0.22f, 0.65f); 
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.14f, 0.22f, 0.35f, 0.85f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.18f, 0.30f, 0.48f, 1.00f);
    
    colors[ImGuiCol_Header]                 = ImVec4(0.12f, 0.22f, 0.38f, 0.65f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.16f, 0.32f, 0.54f, 0.85f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.00f, 0.50f, 0.80f, 1.00f); 
    
    colors[ImGuiCol_Button]                 = ImVec4(0.11f, 0.20f, 0.34f, 0.80f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.00f, 0.45f, 0.70f, 0.90f); 
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.00f, 0.60f, 0.90f, 1.00f); 
    
    colors[ImGuiCol_TitleBg]                = ImVec4(0.06f, 0.09f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.10f, 0.18f, 0.30f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.06f, 0.09f, 0.16f, 0.50f);
    
    colors[ImGuiCol_Tab]                    = ImVec4(0.09f, 0.13f, 0.22f, 0.80f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.16f, 0.28f, 0.44f, 0.90f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.12f, 0.33f, 0.60f, 1.00f);
    
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.00f, 0.50f, 0.80f, 0.80f); 
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.00f, 0.65f, 0.95f, 1.00f);
    
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.04f, 0.06f, 0.10f, 0.40f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.14f, 0.22f, 0.35f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.18f, 0.30f, 0.48f, 0.90f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.00f, 0.50f, 0.80f, 1.00f);
    
    colors[ImGuiCol_CheckMark]              = ImVec4(0.00f, 0.85f, 1.00f, 1.00f); 
    colors[ImGuiCol_Separator]              = ImVec4(0.18f, 0.28f, 0.40f, 0.75f);
}

// ── Main draw ─────────────────────────────────────────────────────────────────
void DebriefUI::draw(PlaybackController& pb,
                     const TelemetryStore& store,
                     const net::ReceiverStats& net_stats,
                     const persist::Recorder& recorder,
                     const flecs::world& world)
{
    apply_premium_theme();
    draw_toolbar(pb, recorder);
    draw_timeline(pb, store);
    draw_entity_list(world);
    draw_inspector(world);
    draw_network_panel(net_stats);
    if (state_.show_minimap) draw_minimap(world);
    draw_settings_window();
}

// ── Toolbar ───────────────────────────────────────────────────────────────────
void DebriefUI::draw_toolbar(PlaybackController& pb,
                              const persist::Recorder& recorder)
{
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::SetNextWindowPos({0, 0});
    ImGui::Begin("##toolbar", nullptr, flags);

    // ── Recording controls ────────────────────────────────────────────────────
    if (recorder.is_recording()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.15f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.25f, 0.30f, 1.0f));
        if (ImGui::Button("● STOP REC") && cbs_.on_record_stop) cbs_.on_record_stop();
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.40f, 0.25f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.55f, 0.35f, 1.00f));
        if (ImGui::Button("● RECORD") && cbs_.on_record_start) cbs_.on_record_start();
        ImGui::PopStyleColor(2);
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputFloat("##dashcam_s", &state_.dashcam_secs, 0, 0, "%.0f s");
    ImGui::SameLine();
    if (ImGui::Button("Save Last N")) {
        if (cbs_.on_save_dashcam) cbs_.on_save_dashcam(state_.dashcam_secs);
    }

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // ── Playback transport ────────────────────────────────────────────────────
    if (pb.is_live()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.60f, 0.40f, 1.00f));
        ImGui::Button("LIVE");
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("LIVE")) pb.stop();
    }
    
    ImGui::SameLine();

    if (pb.state() == PlaybackState::Paused || pb.is_live()) {
        if (ImGui::Button(" ▶ PLAY ")) pb.play(pb.speed());
    } else {
        if (ImGui::Button(" ‖ PAUSE ")) pb.pause();
    }

    ImGui::SameLine();

    float spd = pb.speed();
    ImGui::SetNextItemWidth(90);
    if (ImGui::SliderFloat("##Speed", &spd, -8.0f, 8.0f, "Speed: %.1fx"))
        pb.set_speed(spd);

    ImGui::SameLine();
    if (ImGui::Button(" ◀◀ ")) pb.set_speed(-4.0f);
    ImGui::SameLine();
    if (ImGui::Button(" ▶▶ ")) pb.set_speed( 4.0f);

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // ── Settings button ───────────────────────────────────────────────────────
    if (ImGui::Button(ICON_FA_GEAR " Settings")) {
        state_.show_settings_window = !state_.show_settings_window;
    }

    ImGui::End();
}

// ── Timeline ──────────────────────────────────────────────────────────────────
void DebriefUI::draw_timeline(PlaybackController& pb,
                               const TelemetryStore& store)
{
    auto [t_start, t_end] = store.time_range_ns();
    if (t_start == 0 && t_end == 0) return;

    const float h = GetScreenHeight();
    ImGui::SetNextWindowPos({0, h - 130.0f});
    ImGui::SetNextWindowSize({static_cast<float>(GetScreenWidth()), 130.0f});
    ImGui::Begin("##timeline", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoScrollbar);

    // Large custom-styled scrubber slider
    float frac = pb.scrub_pos(t_start, t_end);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 220.0f);
    
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 24.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.08f, 0.12f, 0.20f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.12f, 0.20f, 0.32f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.16f, 0.28f, 0.44f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.00f, 0.70f, 1.00f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.00f, 0.85f, 1.00f, 1.00f));
    
    if (ImGui::SliderFloat("##scrub", &frac, 0.0f, 1.0f, "")) {
        uint64_t target = PlaybackController::scrub_to_ns(frac, t_start, t_end);
        pb.seek(target);
    }
    
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();

    ImGui::SameLine();
    
    // Status text
    if (pb.is_live()) {
        ImGui::TextColored({0.0f, 0.9f, 0.4f, 1.0f}, "  LIVE FEED  ");
    } else if (pb.state() == PlaybackState::Playing) {
        ImGui::TextColored({0.0f, 0.75f, 1.0f, 1.0f}, "  PLAY %.1fx  ", pb.speed());
    } else {
        ImGui::TextColored({1.0f, 0.75f, 0.0f, 1.0f}, "  PAUSED     ");
    }

    ImGui::SameLine();
    char buf[32];
    uint64_t elapsed = (t_end > t_start) ? (t_end - t_start) : 0;
    format_duration(elapsed, buf, sizeof(buf));
    ImGui::Text("Len: %s", buf);

    ImGui::SameLine();
    uint64_t cur_off = (pb.current_time_ns() > t_start) ? pb.current_time_ns() - t_start : 0;
    format_duration(cur_off, buf, sizeof(buf));
    ImGui::Text("  T+%s", buf);

    // ImPlot altitude area-shaded chart (selected entity)
    if (ImPlot::BeginPlot("##alt", {-1, 70},
                          ImPlotFlags_NoTitle | ImPlotFlags_NoMenus |
                          ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText))
    {
        ImPlot::SetupAxes("Time", "Alt", ImPlotAxisFlags_NoLabel, 0);
        if (plot_count_ > 0) {
            ImPlotSpec spec;
            spec.LineColor = ImVec4{0.00f, 0.70f, 1.00f, 1.00f};
            spec.FillColor = ImVec4{0.00f, 0.70f, 1.00f, 0.15f};
            spec.LineWeight = 2.0f;
            ImPlot::PlotShaded("Altitude", plot_time_, plot_alt_, plot_count_, 0.0, spec);
            ImPlot::PlotLine("Altitude", plot_time_, plot_alt_, plot_count_, spec);
        }
        ImPlot::EndPlot();
    }

    ImGui::End();
}

// ── Entity list ───────────────────────────────────────────────────────────────
void DebriefUI::draw_entity_list(const flecs::world& world)
{
    ImGui::SetNextWindowPos({0, 40.0f});
    ImGui::SetNextWindowSize({220.0f, static_cast<float>(GetScreenHeight()) - 175.0f});
    ImGui::Begin("##SidePanel", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    if (ImGui::BeginTabBar("LeftTabs")) {
        // ── Entities Tab ──────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(" Units ")) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.7f, 1.0f, 0.8f));
            ImGui::TextUnformatted("TRACK LIST");
            ImGui::PopStyleColor();
            ImGui::Separator();

            auto q = world.query<const EntityMeta, const Position, const Velocity>();
            q.each([&](flecs::entity e, const EntityMeta& meta, const Position& pos, const Velocity& vel) {
                char label[80];
                if (meta.callsign[0])
                    snprintf(label, sizeof(label), "%s  %s", entity_type_icon(meta.type), meta.callsign);
                else
                    snprintf(label, sizeof(label), "%s  #%u", entity_type_icon(meta.type), meta.entity_id);

                bool selected = (state_.selected_entity == e);

                if (!meta.active) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.65f, 0.75f));
                }

                if (ImGui::Selectable(label, selected, 0, ImVec2(0, 32))) {
                    state_.selected_entity = selected ? flecs::entity{} : e;
                }

                if (!meta.active) {
                    ImGui::PopStyleColor();
                } else {
                    // Altitude and speed mini info row
                    float spd = sqrtf(vel.v.x*vel.v.x + vel.v.y*vel.v.y + vel.v.z*vel.v.z);
                    ImGui::SameLine(0, 4);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 15);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.65f, 0.9f, 0.75f));
                    ImGui::Text("%.0fm  %.0fkt", pos.v.y, spd * 1.944f);
                    ImGui::PopStyleColor();
                }

                if (selected && meta.active) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.38f, 0.6f, 0.85f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.5f, 0.8f, 1.0f));
                    if (ImGui::SmallButton("  FOCUS  ")) state_.camera_mode = 1;
                    ImGui::SameLine();
                    if (ImGui::SmallButton(" CHASE "))  state_.camera_mode = 2;
                    ImGui::PopStyleColor(2);
                }
            });

            ImGui::EndTabItem();
        }

        // ── Visuals Tab ───────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(" Camera ")) {
            // Camera mode
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.75f, 1.0f, 1.0f));
            ImGui::TextUnformatted("CAMERA MODE");
            ImGui::PopStyleColor();

            const char* cam_modes[] = { "  Free Orbit", "  Focus Target", "  Chase Cam" };
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##CamMode", &state_.camera_mode, cam_modes, IM_ARRAYSIZE(cam_modes));

            if (state_.camera_mode > 0 && !state_.selected_entity.is_valid()) {
                ImGui::TextColored({1.0f, 0.5f, 0.3f, 1.0f}, "! Select an entity");
            }

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.75f, 1.0f, 1.0f));
            ImGui::TextUnformatted("CONTROLS");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.65f, 0.8f, 0.9f));
            ImGui::TextUnformatted("LMB        Click to select");
            ImGui::TextUnformatted("RMB+drag   Rotate / Chase adj");
            ImGui::TextUnformatted("MMB+drag   Pan scene");
            ImGui::TextUnformatted("Scroll     Zoom in/out");
            ImGui::TextUnformatted("WASD/Arrows  Pan (free)");
            ImGui::TextUnformatted("Shift      4x faster pan");
            ImGui::TextUnformatted("F          Focus / Free toggle");
            ImGui::TextUnformatted("G          Chase cam toggle");
            ImGui::PopStyleColor();

            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.75f, 1.0f, 1.0f));
            ImGui::TextUnformatted("SCENE SCALE");
            ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##Scale", &state_.entity_3d_scale, 1.0f, 150.0f, "Entities: %.0fx");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##AltExag", &state_.altitude_exaggerate, 1.0f, 20.0f, "Alt Exag: %.1fx");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##TWidth", &state_.trail_width_override, 5.0f, 500.0f, "Trail W: %.0fm");

            ImGui::Spacing();
            if (ImGui::Button("Zoom In", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 2, 0))) {
                state_.camera_distance = std::max(30.0f, state_.camera_distance * 0.7f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Zoom Out", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                state_.camera_distance = std::min(80000.0f, state_.camera_distance * 1.4f);
            }

            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.75f, 1.0f, 1.0f));
            ImGui::TextUnformatted("TERRAIN (Quick)");
            ImGui::PopStyleColor();
            const char* terrain_modes[] = { "None", "Wireframe", "Solid", "Both" };
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##TerrainMode", &state_.terrain_mode, terrain_modes, IM_ARRAYSIZE(terrain_modes));
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##Hills", &state_.terrain_height_scale, 0.0f, 4.0f, "Hills: %.1fx");

            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.75f, 1.0f, 1.0f));
            ImGui::TextUnformatted("OVERLAYS");
            ImGui::PopStyleColor();
            ImGui::Checkbox("Callsign Labels",  &state_.show_labels);
            ImGui::Checkbox("Trail Lines",      &state_.show_trails);
            ImGui::Checkbox("Ribbon Mode",      &state_.ribbon_trails);
            ImGui::Checkbox("Velocity Vectors", &state_.show_velocity_vec);

            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}


// ── Inspector ─────────────────────────────────────────────────────────────────
void DebriefUI::draw_inspector(const flecs::world& world)
{
    if (!state_.selected_entity.is_valid()) return;
    if (!world.is_alive(state_.selected_entity)) { state_.selected_entity = {}; return; }

    ImGui::SetNextWindowPos({static_cast<float>(GetScreenWidth()) - 230.0f, 40.0f});
    ImGui::SetNextWindowSize({230.0f, 350.0f});
    ImGui::Begin("Inspector", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    auto e = state_.selected_entity;
    const auto *meta = &e.get<EntityMeta>();
    const auto *pos = &e.get<Position>();
    // const auto *rot = &e.get<Rotation>(); // Unused but kept for reference
    const auto *vel = &e.get<Velocity>();

    if (meta) {
        ImGui::Text("Type:     %s  %s", entity_type_icon(meta->type), entity_type_name(meta->type));
        ImGui::Text("ID:       %u/%u", meta->source_id, meta->entity_id);
        if (meta->callsign[0]) ImGui::Text("Callsign: %.8s", meta->callsign);
        ImGui::Text("Active:   %s", meta->active ? "Yes" : "DESTROYED");
    }
    if (pos) ImGui::Text("Pos: %.1f, %.1f, %.1f", pos->v.x, pos->v.y, pos->v.z);
    if (vel) {
        float spd = sqrtf(vel->v.x*vel->v.x + vel->v.y*vel->v.y + vel->v.z*vel->v.z);
        ImGui::Text("Speed:    %.1f m/s (%.0f kt)", spd, spd * 1.944f);
        if (pos) ImGui::Text("Altitude: %.0f m", pos->v.y);
    }

    // Update ImPlot sample buffer — reset on entity switch, cap at kPlotWindow
    if (e != last_plot_entity_) {
        plot_count_ = 0;
        last_plot_entity_ = e;
    }
    if (pos && vel && plot_count_ < kPlotWindow) {
        float spd = sqrtf(vel->v.x*vel->v.x + vel->v.y*vel->v.y + vel->v.z*vel->v.z);
        plot_time_ [plot_count_] = static_cast<float>(ImGui::GetTime());
        plot_alt_  [plot_count_] = pos->v.y;
        plot_speed_[plot_count_] = spd;
        ++plot_count_;
    }

    ImGui::End();
}

// ── Network panel ─────────────────────────────────────────────────────────────
void DebriefUI::draw_network_panel(const net::ReceiverStats& stats)
{
    ImGui::SetNextWindowPos({static_cast<float>(GetScreenWidth()) - 230.0f,
                              static_cast<float>(GetScreenHeight()) - 230.0f});
    ImGui::SetNextWindowSize({230.0f, 160.0f});
    ImGui::Begin("Network", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    ImGui::Text("RX pkts:    %llu", (unsigned long long)stats.packets_received);
    ImGui::Text("RX bytes:   %.1f KB", stats.bytes_received / 1024.0);
    ImGui::Text("Dropped:    %llu", (unsigned long long)stats.packets_dropped);
    ImGui::Text("Parse err:  %llu", (unsigned long long)stats.parse_errors);
    ImGui::Text("Seq gaps:   %llu", (unsigned long long)stats.sequence_gaps);

    ImGui::End();
}

// ── Minimap ───────────────────────────────────────────────────────────────────
void DebriefUI::draw_minimap(const flecs::world& world)
{
    const float sz = 190.0f;
    ImGui::SetNextWindowPos({static_cast<float>(GetScreenWidth()) - 230.0f,
                              static_cast<float>(GetScreenHeight()) - 450.0f});
    ImGui::SetNextWindowSize({230.0f, 215.0f});
    ImGui::Begin("Minimap (top-down)", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar);

    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl    = ImGui::GetWindowDrawList();
    
    // Background and border
    dl->AddRectFilled(canvas_pos, {canvas_pos.x+sz, canvas_pos.y+sz}, IM_COL32(5, 10, 18, 220));
    dl->AddRect      (canvas_pos, {canvas_pos.x+sz, canvas_pos.y+sz}, IM_COL32(24, 45, 75, 255));

    // 1. Draw Grid Lines
    for (float i = 30.0f; i < sz; i += 30.0f) {
        dl->AddLine({canvas_pos.x + i, canvas_pos.y}, {canvas_pos.x + i, canvas_pos.y + sz}, IM_COL32(40, 60, 90, 40));
        dl->AddLine({canvas_pos.x, canvas_pos.y + i}, {canvas_pos.x + sz, canvas_pos.y + i}, IM_COL32(40, 60, 90, 40));
    }

    // 2. Draw concentric radar range rings from the center
    ImVec2 center = { canvas_pos.x + sz/2.0f, canvas_pos.y + sz/2.0f };
    dl->AddCircle(center, sz / 4.0f, IM_COL32(0, 180, 255, 30), 0, 1.0f);
    dl->AddCircle(center, sz / 2.0f, IM_COL32(0, 180, 255, 45), 0, 1.0f);
    dl->AddCircle(center, sz * 0.75f / 2.0f, IM_COL32(0, 180, 255, 15), 0, 1.0f);

    // 3. Radar Sweep Effect
    float time = static_cast<float>(ImGui::GetTime());
    float sweep_angle = time * 1.5f; 
    for (int i = 0; i < 20; ++i) {
        float alpha_factor = (20 - i) / 20.0f;
        float angle = sweep_angle - i * 0.03f;
        float rx = center.x + cosf(angle) * (sz * sqrtf(2.0f) / 2.0f);
        float ry = center.y + sinf(angle) * (sz * sqrtf(2.0f) / 2.0f);
        
        rx = std::clamp(rx, canvas_pos.x, canvas_pos.x + sz);
        ry = std::clamp(ry, canvas_pos.y, canvas_pos.y + sz);
        
        dl->AddLine(center, {rx, ry}, IM_COL32(0, 220, 255, static_cast<int>(60.0f * alpha_factor)), 1.2f);
    }

    // Find scene bounds
    float xmin = 1e9f, xmax = -1e9f, zmin = 1e9f, zmax = -1e9f;
    auto q = world.query<const Position, const EntityMeta>();
    q.each([&](const Position& p, const EntityMeta& meta) {
        if (!meta.active) return;
        xmin = std::min(xmin, p.v.x); xmax = std::max(xmax, p.v.x);
        zmin = std::min(zmin, p.v.z); zmax = std::max(zmax, p.v.z);
    });
    float range = std::max({xmax - xmin, zmax - zmin, 1000.0f}) * 1.2f;
    
    float cx = (xmin != 1e9f) ? (xmin + xmax) * 0.5f : 0.0f;
    float cz = (zmin != 1e9f) ? (zmin + zmax) * 0.5f : 0.0f;

    q.each([&](flecs::entity e, const Position& p, const EntityMeta& meta) {
        if (!meta.active) return;
        float fx = 0.5f + (p.v.x - cx) / range;
        float fz = 0.5f + (p.v.z - cz) / range;
        
        float px = canvas_pos.x + fx * sz;
        float py = canvas_pos.y + (1.0f - fz) * sz;
        
        if (px >= canvas_pos.x && px <= canvas_pos.x + sz &&
            py >= canvas_pos.y && py <= canvas_pos.y + sz) 
        {
            ImVec2 center_pt = {px, py};
            ImU32 col = (e == state_.selected_entity)
                        ? IM_COL32(255, 220, 0, 255)
                        : IM_COL32(0, 190, 255, 230);
                        
            if (e == state_.selected_entity) {
                dl->AddCircle(center_pt, 7.0f, IM_COL32(255, 220, 0, 120), 0, 1.2f);
                dl->AddCircle(center_pt, 25.0f, IM_COL32(255, 220, 0, 40), 0, 1.0f);
            }
            
            dl->AddCircleFilled(center_pt, 3.5f, col);
        }
    });

    ImGui::Dummy({sz, sz});
    ImGui::End();
}

// ── Settings Window ───────────────────────────────────────────────────────────
void DebriefUI::draw_settings_window() {
    if (!state_.show_settings_window) return;

    ImGui::SetNextWindowSize({300.0f, 400.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin(ICON_FA_GEAR " Settings", &state_.show_settings_window)) {
        
        if (ImGui::CollapsingHeader("Display & Overlays", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Callsign Labels",  &state_.show_labels);
            ImGui::Checkbox("Trail Lines",      &state_.show_trails);
            ImGui::Checkbox("Ribbon Mode",      &state_.ribbon_trails);
            ImGui::Checkbox("Velocity Vectors", &state_.show_velocity_vec);
            ImGui::Checkbox("Minimap",          &state_.show_minimap);
            ImGui::Checkbox("Network Stats",    &state_.show_stats);
        }

        if (ImGui::CollapsingHeader("Graphics & Engine", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Far Clip Plane (Render Distance)");
            ImGui::SliderFloat("##FarClip", &state_.far_clip_plane, 10000.0f, 2000000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
            
            ImGui::Text("Terrain Mode");
            const char* terrain_modes[] = { "None", "Wireframe", "Solid", "Both" };
            ImGui::Combo("##SettingsTerrainMode", &state_.terrain_mode, terrain_modes, IM_ARRAYSIZE(terrain_modes));
            
            ImGui::Text("Terrain Height Scale");
            ImGui::SliderFloat("##SettingsHills", &state_.terrain_height_scale, 0.0f, 4.0f, "%.1fx");
        }
        
        if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("Changes made here persist across sessions.");
        }
        
        ImGui::Separator();
        if (ImGui::Button("Close", {-1, 0})) {
            state_.show_settings_window = false;
        }
    }
    ImGui::End();
}

} // namespace debrief
