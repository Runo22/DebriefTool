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

// ── Helpers ───────────────────────────────────────────────────────────────────
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

// ── Main draw ─────────────────────────────────────────────────────────────────
void DebriefUI::draw(PlaybackController& pb,
                     const TelemetryStore& store,
                     const net::ReceiverStats& net_stats,
                     const persist::Recorder& recorder,
                     const flecs::world& world)
{
    draw_toolbar(pb, recorder);
    draw_timeline(pb, store);
    draw_entity_list(world);
    draw_inspector(world);
    draw_network_panel(net_stats);
    if (state_.show_minimap) draw_minimap(world);
}

// ── Toolbar ───────────────────────────────────────────────────────────────────
void DebriefUI::draw_toolbar(PlaybackController& pb,
                              const persist::Recorder& recorder)
{
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##toolbar", nullptr, flags);

    // ── Recording controls ────────────────────────────────────────────────────
    if (recorder.is_recording()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("[ REC ]") && cbs_.on_record_stop) cbs_.on_record_stop();
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("Record") && cbs_.on_record_start) cbs_.on_record_start();
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
    if (ImGui::Button("LIVE")) pb.stop();
    ImGui::SameLine();

    if (pb.state() == PlaybackState::Paused || pb.is_live()) {
        if (ImGui::Button(">")) pb.play(pb.speed());
    } else {
        if (ImGui::Button("||")) pb.pause();
    }

    ImGui::SameLine();

    float spd = pb.speed();
    ImGui::SetNextItemWidth(80);
    if (ImGui::SliderFloat("Speed", &spd, -8.0f, 8.0f, "%.1fx"))
        pb.set_speed(spd);

    ImGui::SameLine();
    if (ImGui::Button("<<")) pb.set_speed(-4.0f);
    ImGui::SameLine();
    if (ImGui::Button(">>")) pb.set_speed( 4.0f);

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // ── Display toggles ───────────────────────────────────────────────────────
    ImGui::Checkbox("Trails", &state_.show_trails);
    ImGui::SameLine();
    ImGui::Checkbox("Labels", &state_.show_labels);
    ImGui::SameLine();
    ImGui::Checkbox("Ribbon", &state_.ribbon_trails);
    ImGui::SameLine();
    ImGui::Checkbox("Vel Vec", &state_.show_velocity_vec);

    ImGui::End();
}

// ── Timeline ──────────────────────────────────────────────────────────────────
void DebriefUI::draw_timeline(PlaybackController& pb,
                               const TelemetryStore& store)
{
    auto [t_start, t_end] = store.time_range_ns();
    if (t_start == 0 && t_end == 0) return;

    const float h = GetScreenHeight();
    ImGui::SetNextWindowPos({0, h - 120.0f});
    ImGui::SetNextWindowSize({static_cast<float>(GetScreenWidth()), 120.0f});
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##timeline", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoScrollbar);

    // Scrubber slider
    float frac = pb.scrub_pos(t_start, t_end);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 160.0f);
    if (ImGui::SliderFloat("##scrub", &frac, 0.0f, 1.0f, "")) {
        uint64_t target = PlaybackController::scrub_to_ns(frac, t_start, t_end);
        pb.seek(target);
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

    ImGui::SameLine();
    if (pb.is_live()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
        ImGui::Text("  [LIVE]");
        ImGui::PopStyleColor();
    }

    // ImPlot altitude chart (selected entity)
    if (ImPlot::BeginPlot("##alt", {-1, 60},
                          ImPlotFlags_NoTitle | ImPlotFlags_NoMenus |
                          ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText))
    {
        ImPlot::SetupAxes("Time", "Alt", ImPlotAxisFlags_NoLabel, 0);
        if (plot_count_ > 0)
            ImPlot::PlotLine("Alt", plot_time_, plot_alt_, plot_count_);
        ImPlot::EndPlot();
    }

    ImGui::End();
}

// ── Entity list ───────────────────────────────────────────────────────────────
void DebriefUI::draw_entity_list(const flecs::world& world)
{
    ImGui::SetNextWindowPos({0, 40.0f});
    ImGui::SetNextWindowSize({200.0f, static_cast<float>(GetScreenHeight()) - 160.0f});
    ImGui::SetNextWindowBgAlpha(0.80f);
    ImGui::Begin("Entities", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    auto q = world.query<const EntityMeta, const Position>();
    q.each([&](flecs::entity e, const EntityMeta& meta, const Position& pos) {
        char label[64];
        if (meta.callsign[0])
            snprintf(label, sizeof(label), "[%s] %s", entity_type_name(meta.type), meta.callsign);
        else
            snprintf(label, sizeof(label), "[%s] #%u", entity_type_name(meta.type), meta.entity_id);

        bool selected = (state_.selected_entity == e);
        if (ImGui::Selectable(label, selected))
            state_.selected_entity = selected ? flecs::entity{} : e;

        if (!meta.active) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            ImGui::Text("[X]");
            ImGui::PopStyleColor();
        }
    });

    ImGui::End();
}

// ── Inspector ─────────────────────────────────────────────────────────────────
void DebriefUI::draw_inspector(const flecs::world& world)
{
    if (!state_.selected_entity.is_valid()) return;
    if (!world.is_alive(state_.selected_entity)) { state_.selected_entity = {}; return; }

    ImGui::SetNextWindowPos({static_cast<float>(GetScreenWidth()) - 220.0f, 40.0f});
    ImGui::SetNextWindowSize({220.0f, 340.0f});
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("Inspector", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    auto e = state_.selected_entity;
    const auto *meta = &e.get<EntityMeta>();
    const auto *pos = &e.get<Position>();
    const auto *vel = &e.get<Velocity>();

    if (meta) {
        ImGui::Text("Type:     %s", entity_type_name(meta->type));
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
    ImGui::SetNextWindowPos({static_cast<float>(GetScreenWidth()) - 220.0f,
                              static_cast<float>(GetScreenHeight()) - 220.0f});
    ImGui::SetNextWindowSize({220.0f, 160.0f});
    ImGui::SetNextWindowBgAlpha(0.75f);
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
    const float sz = 180.0f;
    ImGui::SetNextWindowPos({static_cast<float>(GetScreenWidth()) - 220.0f,
                              static_cast<float>(GetScreenHeight()) - 430.0f});
    ImGui::SetNextWindowSize({220.0f, 200.0f});
    ImGui::SetNextWindowBgAlpha(0.80f);
    ImGui::Begin("Minimap (top-down)", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar);

    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl    = ImGui::GetWindowDrawList();
    dl->AddRectFilled(canvas_pos, {canvas_pos.x+sz, canvas_pos.y+sz}, IM_COL32(10,20,10,200));
    dl->AddRect      (canvas_pos, {canvas_pos.x+sz, canvas_pos.y+sz}, IM_COL32(60,120,60,255));

    // Find scene bounds.
    float xmin = 1e9f, xmax = -1e9f, zmin = 1e9f, zmax = -1e9f;
    auto q = world.query<const Position, const EntityMeta>();
    q.each([&](const Position& p, const EntityMeta& meta) {
        if (!meta.active) return;
        xmin = std::min(xmin, p.v.x); xmax = std::max(xmax, p.v.x);
        zmin = std::min(zmin, p.v.z); zmax = std::max(zmax, p.v.z);
    });
    float range = std::max({xmax - xmin, zmax - zmin, 1000.0f}) * 1.1f;

    q.each([&](flecs::entity e, const Position& p, const EntityMeta& meta) {
        if (!meta.active) return;
        float fx = (p.v.x - xmin) / range;
        float fz = (p.v.z - zmin) / range;
        float px = canvas_pos.x + fx * sz;
        float py = canvas_pos.y + (1.0f - fz) * sz;

        ImU32 col = (e == state_.selected_entity)
                    ? IM_COL32(255, 255, 0, 255)
                    : IM_COL32(0, 200, 255, 220);
        dl->AddCircleFilled({px, py}, 3.0f, col);
    });

    ImGui::Dummy({sz, sz});
    ImGui::End();
}

} // namespace debrief
