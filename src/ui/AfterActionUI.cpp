#include "AfterActionUI.hpp"
#include "../ecs/Components.hpp"
#include <imgui.h>
#include <implot.h>
#include <rlImGui.h>
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace afteraction {

using namespace ecs;

namespace {

constexpr float kPad       = 8.0f;     // gap between panels and screen edges
constexpr float kLeftW     = 280.0f;   // track panel width
constexpr float kRightW    = 300.0f;   // inspector / minimap width
constexpr float kM2Ft      = 3.28084f;
constexpr float kMps2Kt    = 1.94384f;

const ImVec4 kAccent   { 0.18f, 0.66f, 0.95f, 1.0f };
const ImVec4 kDim      { 0.55f, 0.63f, 0.74f, 1.0f };
const ImVec4 kWarn     { 1.00f, 0.72f, 0.20f, 1.0f };
const ImVec4 kGood     { 0.25f, 0.85f, 0.55f, 1.0f };
const ImVec4 kBad      { 1.00f, 0.36f, 0.36f, 1.0f };

// ── Icons, names, colours per entity type ────────────────────────────────────
const char* entity_type_icon(uint16_t t) {
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

const char* entity_type_name(uint16_t t) {
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

// Same hues as the 3D trails, so list, map and scene agree.
ImVec4 entity_type_color(uint16_t t) {
    switch (t) {
    case net::TYPE_JET:     return { 0.38f, 0.66f, 1.00f, 1.0f };
    case net::TYPE_MISSILE: return { 1.00f, 0.38f, 0.34f, 1.0f };
    case net::TYPE_AAA:     return { 0.40f, 0.78f, 0.40f, 1.0f };
    case net::TYPE_GROUND:  return { 0.93f, 0.62f, 0.22f, 1.0f };
    case net::TYPE_HELO:    return { 0.42f, 0.90f, 0.50f, 1.0f };
    case net::TYPE_SHIP:    return { 0.70f, 0.70f, 0.92f, 1.0f };
    default:                return { 0.20f, 0.80f, 1.00f, 1.0f };
    }
}

void format_duration(uint64_t ns, char* buf, size_t n) {
    uint64_t sec  = ns / 1'000'000'000ULL;
    uint64_t ms   = (ns % 1'000'000'000ULL) / 1'000'000ULL;
    uint64_t min  = sec / 60; sec %= 60;
    uint64_t hour = min / 60; min %= 60;
    snprintf(buf, n, "%02llu:%02llu:%02llu.%03llu",
             (unsigned long long)hour, (unsigned long long)min,
             (unsigned long long)sec, (unsigned long long)ms);
}

// 40°04'44.0"N
void format_dms(double v, bool is_lat, char* buf, size_t n) {
    const char hemi = is_lat ? (v >= 0 ? 'N' : 'S') : (v >= 0 ? 'E' : 'W');
    v = std::fabs(v);
    const int d = static_cast<int>(v);
    const double m_full = (v - d) * 60.0;
    const int m = static_cast<int>(m_full);
    const double s = (m_full - m) * 60.0;
    snprintf(buf, n, "%d\xc2\xb0%02d'%04.1f\"%c", d, m, s, hemi);
}

// Small coloured caption ("TRACKS", "TERRAIN" …) with a rule.
void section_header(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
}

// Button tinted with a custom colour.
bool tinted_button(const char* label, ImVec4 c, ImVec2 size = {0, 0}) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(c.x * 0.55f, c.y * 0.55f, c.z * 0.55f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x * 0.75f, c.y * 0.75f, c.z * 0.75f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  c);
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

// Heading / pitch / roll (degrees) from an orientation quaternion.
void euler_from_quat(Quaternion q, float* hdg, float* pitch, float* roll) {
    auto rot = [&](Vector3 v) {   // q * v * q^-1
        const float x = q.x, y = q.y, z = q.z, w = q.w;
        const float ix = w * v.x + y * v.z - z * v.y;
        const float iy = w * v.y + z * v.x - x * v.z;
        const float iz = w * v.z + x * v.y - y * v.x;
        const float iw = -x * v.x - y * v.y - z * v.z;
        return Vector3{ ix * w + iw * -x + iy * -z - iz * -y,
                        iy * w + iw * -y + iz * -x - ix * -z,
                        iz * w + iw * -z + ix * -y - iy * -x };
    };
    const Vector3 f = rot({ 0, 0, -1 }), r = rot({ 1, 0, 0 }), u = rot({ 0, 1, 0 });
    *hdg   = std::fmod(std::atan2(f.x, -f.z) * RAD2DEG + 360.0f, 360.0f);
    *pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f)) * RAD2DEG;
    *roll  = std::atan2(-r.y, u.y) * RAD2DEG;
}

} // namespace

// ── Theme ─────────────────────────────────────────────────────────────────────
void AfterActionUI::apply_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 6.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.TabRounding       = 4.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;
    style.WindowPadding     = ImVec2(10.0f, 10.0f);
    style.FramePadding      = ImVec2(8.0f, 4.0f);
    style.ItemSpacing       = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    style.CellPadding       = ImVec2(6.0f, 3.0f);
    style.ScrollbarSize     = 10.0f;
    style.GrabMinSize       = 10.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextAlign      = ImVec2(0.0f, 0.5f);
    style.SeparatorTextPadding    = ImVec2(12.0f, 2.0f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.88f, 0.92f, 0.97f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.46f, 0.53f, 0.62f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.055f, 0.075f, 0.11f, 0.90f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.04f, 0.055f, 0.085f, 0.60f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.06f, 0.08f, 0.12f, 0.97f);
    c[ImGuiCol_Border]               = ImVec4(0.16f, 0.24f, 0.34f, 0.70f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = ImVec4(0.10f, 0.14f, 0.21f, 0.80f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.14f, 0.21f, 0.31f, 0.90f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.17f, 0.27f, 0.40f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.06f, 0.09f, 0.14f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.08f, 0.13f, 0.21f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.06f, 0.09f, 0.14f, 0.60f);
    c[ImGuiCol_Header]               = ImVec4(0.12f, 0.25f, 0.40f, 0.70f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.15f, 0.32f, 0.52f, 0.80f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.10f, 0.45f, 0.72f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.12f, 0.19f, 0.29f, 0.90f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.14f, 0.36f, 0.56f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.10f, 0.50f, 0.80f, 1.00f);
    c[ImGuiCol_Tab]                  = ImVec4(0.09f, 0.13f, 0.20f, 0.90f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.15f, 0.30f, 0.48f, 1.00f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.12f, 0.33f, 0.55f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]  = kAccent;
    c[ImGuiCol_SliderGrab]           = ImVec4(0.20f, 0.60f, 0.90f, 0.90f);
    c[ImGuiCol_SliderGrabActive]     = kAccent;
    c[ImGuiCol_CheckMark]            = kAccent;
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.18f, 0.26f, 0.36f, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.22f, 0.34f, 0.48f, 0.90f);
    c[ImGuiCol_ScrollbarGrabActive]  = kAccent;
    c[ImGuiCol_Separator]            = ImVec4(0.16f, 0.24f, 0.34f, 0.70f);
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.08f, 0.12f, 0.18f, 1.00f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.16f, 0.24f, 0.34f, 0.40f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.16f, 0.24f, 0.34f, 0.70f);
}

// ── Main draw ─────────────────────────────────────────────────────────────────
float AfterActionUI::timeline_height(const TelemetryStore& store) const {
    auto [ts, te] = store.time_range_ns();
    if (ts == 0 && te == 0) return 0.0f;
    if (!state_.selected_entity.is_valid()) return 62.0f;          // scrubber only
    return std::clamp(state_.timeline_height, 140.0f, static_cast<float>(GetScreenHeight()) * 0.6f);
}

void AfterActionUI::draw(PlaybackController& pb,
                         const TelemetryStore& store,
                         const net::ReceiverStats& net_stats,
                         const persist::Recorder& recorder,
                         const flecs::world& world)
{
    if (state_.selected_entity.is_valid() && !world.is_alive(state_.selected_entity))
        state_.selected_entity = {};

    const float bottom = timeline_height(store);
    draw_top_bar(pb, store, net_stats, recorder);
    if (bottom > 0.0f) draw_timeline(pb, store, world, bottom);
    draw_track_panel(world, bottom);
    draw_right_column(world, net_stats, bottom);
    draw_settings_window();
}

// ── Top bar ───────────────────────────────────────────────────────────────────
void AfterActionUI::draw_top_bar(PlaybackController& pb, const TelemetryStore& store,
                                 const net::ReceiverStats& stats, const persist::Recorder& recorder)
{
    const float w = static_cast<float>(GetScreenWidth());
    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize({ w, kTopBarHeight });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    ImGui::Begin("##topbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);

    // Brand
    if (heading_font_) ImGui::PushFont(heading_font_, 0.0f);
    ImGui::TextColored(kAccent, ICON_FA_LOCATION_CROSSHAIRS);
    ImGui::SameLine(0, 6);
    ImGui::TextUnformatted("AfterAction");
    if (heading_font_) ImGui::PopFont();
    ImGui::SameLine(0, 18);

    // Recording
    if (recorder.is_recording()) {
        const float blink = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
        if (tinted_button(ICON_FA_STOP "  Stop", ImVec4(0.95f * blink, 0.20f, 0.22f, 1.0f)) && cbs_.on_record_stop)
            cbs_.on_record_stop();
        ImGui::SameLine();
        ImGui::TextColored(kBad, "REC %llu", (unsigned long long)recorder.frames_written());
    } else if (tinted_button(ICON_FA_CIRCLE "  Record", ImVec4(0.85f, 0.25f, 0.28f, 1.0f)) && cbs_.on_record_start) {
        cbs_.on_record_start();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Record everything to recordings/session_*.aar");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(56);
    ImGui::InputFloat("##dashcam_s", &state_.dashcam_secs, 0, 0, "%.0f s");
    ImGui::SameLine(0, 4);
    if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save last") && cbs_.on_save_dashcam)
        cbs_.on_save_dashcam(state_.dashcam_secs);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the last N seconds of the buffer (dashcam)");

    ImGui::SameLine(0, 18);

    // Transport
    if (pb.is_live()) tinted_button(ICON_FA_TOWER_BROADCAST "  LIVE", kGood);
    else if (ImGui::Button(ICON_FA_TOWER_BROADCAST "  LIVE")) pb.stop();
    ImGui::SameLine(0, 4);
    if (ImGui::Button(ICON_FA_BACKWARD_FAST "##rew")) pb.set_speed(-4.0f);
    ImGui::SameLine(0, 4);
    if (pb.state() == PlaybackState::Paused || pb.is_live()) {
        if (ImGui::Button(ICON_FA_PLAY "##play", { 34, 0 })) pb.play(pb.speed());
    } else if (ImGui::Button(ICON_FA_PAUSE "##pause", { 34, 0 })) {
        pb.pause();
    }
    ImGui::SameLine(0, 4);
    if (ImGui::Button(ICON_FA_FORWARD_FAST "##ffwd")) pb.set_speed(4.0f);
    ImGui::SameLine(0, 4);
    float spd = pb.speed();
    ImGui::SetNextItemWidth(110);
    if (ImGui::SliderFloat("##Speed", &spd, -8.0f, 8.0f, "%.2fx")) pb.set_speed(spd);

    ImGui::SameLine(0, 18);
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open")) {
        if (cbs_.on_browse_file) cbs_.on_browse_file();
        else state_.show_settings_window = true;
    }
    ImGui::SameLine(0, 4);
    if (ImGui::Button(ICON_FA_GEAR "##settings")) state_.show_settings_window = !state_.show_settings_window;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings");

    // ── Right side: cursor read-out, network, mode pill, FPS ─────────────────
    const SceneInfo& sc = state_.scene;
    char cursor[96] = "";
    if (sc.cursor_valid) {
        char la[24], lo[24];
        format_dms(sc.cursor_lat, true, la, sizeof(la));
        format_dms(sc.cursor_lon, false, lo, sizeof(lo));
        snprintf(cursor, sizeof(cursor), ICON_FA_LOCATION_DOT "  %s  %s   %.0f m / %.0f ft", la, lo,
                 sc.cursor_elev, sc.cursor_elev * kM2Ft);
    }
    char net[48];
    snprintf(net, sizeof(net), ICON_FA_SATELLITE_DISH " %llu", (unsigned long long)stats.packets_received);
    const char* mode = sc.demo ? "DEMO" : pb.is_live() ? "LIVE" : pb.state() == PlaybackState::Playing ? "PLAYBACK" : "PAUSED";
    const ImVec4 mode_col = sc.demo ? kWarn : pb.is_live() ? kGood : pb.state() == PlaybackState::Playing ? kAccent : kWarn;
    char fps[16];
    snprintf(fps, sizeof(fps), "%d fps", sc.fps);

    if (mono_font_) ImGui::PushFont(mono_font_, 0.0f);
    const float sp = 18.0f;
    float w_cursor = cursor[0] ? ImGui::CalcTextSize(cursor).x + sp : 0.0f;
    const float w_fps = ImGui::CalcTextSize(fps).x;
    if (mono_font_) ImGui::PopFont();
    const float w_net  = ImGui::CalcTextSize(net).x + sp;
    const float w_mode = ImGui::CalcTextSize(mode).x + 16.0f + sp;
    float x = w - 10.0f - w_fps - w_mode - w_net - w_cursor;
    const float min_x = ImGui::GetCursorPosX() + 24.0f;
    if (x < min_x && cursor[0]) { x += w_cursor; cursor[0] = '\0'; }   // narrow window: drop cursor
    ImGui::SameLine(std::max(x, min_x));
    if (cursor[0]) {
        if (mono_font_) ImGui::PushFont(mono_font_, 0.0f);
        ImGui::TextColored(kDim, "%s", cursor);
        if (mono_font_) ImGui::PopFont();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Terrain under the cursor (%.5f, %.5f), %.0f m DEM grid",
                              sc.cursor_lat, sc.cursor_lon, sc.cursor_res);
        ImGui::SameLine(0, sp);
    }
    ImGui::TextColored(stats.packets_received ? kGood : kDim, "%s", net);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UDP %s:%u\n%llu packets, %.1f KB\n%llu dropped, %llu parse errors, %llu gaps",
                          state_.bind_addr, state_.listen_port,
                          (unsigned long long)stats.packets_received, stats.bytes_received / 1024.0,
                          (unsigned long long)stats.packets_dropped, (unsigned long long)stats.parse_errors,
                          (unsigned long long)stats.sequence_gaps);
    ImGui::SameLine(0, sp);
    {   // mode pill
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 ts = ImGui::CalcTextSize(mode);
        ImGui::GetWindowDrawList()->AddRectFilled({ p.x, p.y - 1 }, { p.x + ts.x + 16, p.y + ts.y + 1 },
            ImGui::GetColorU32(ImVec4(mode_col.x, mode_col.y, mode_col.z, 0.22f)), 9.0f);
        ImGui::SetCursorScreenPos({ p.x + 8, p.y });
        ImGui::TextColored(mode_col, "%s", mode);
        ImGui::SameLine(0, 8 + sp);
    }
    if (mono_font_) ImGui::PushFont(mono_font_, 0.0f);
    ImGui::TextColored(sc.fps >= 30 ? kDim : kWarn, "%s", fps);
    if (mono_font_) ImGui::PopFont();
    (void)store;
    ImGui::End();
}

// ── Timeline ──────────────────────────────────────────────────────────────────
void AfterActionUI::draw_timeline(PlaybackController& pb, const TelemetryStore& store,
                                  const flecs::world& world, float panel_h)
{
    auto [t_start, t_end] = store.time_range_ns();
    const float h = static_cast<float>(GetScreenHeight());
    const float w = static_cast<float>(GetScreenWidth());
    ImGui::SetNextWindowPos({ 0, h - panel_h });
    ImGui::SetNextWindowSize({ w, panel_h });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##timeline", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    const bool with_chart = state_.selected_entity.is_valid();
    if (with_chart) {   // drag the top edge to resize the chart panel
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 saved = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos({ wp.x, wp.y });
        ImGui::InvisibleButton("##tl_resize", { w, 6.0f });
        const bool grip = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (grip) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive())
            state_.timeline_height = std::clamp(state_.timeline_height - ImGui::GetIO().MouseDelta.y,
                                                140.0f, h * 0.6f);
        ImGui::GetWindowDrawList()->AddLine({ wp.x, wp.y + 1.0f }, { wp.x + w, wp.y + 1.0f },
            grip ? ImGui::GetColorU32(kAccent) : IM_COL32(40, 70, 100, 130), 2.0f);
        ImGui::SetCursorScreenPos(saved);
    }

    // Row: T+ clock · scrubber · length · state
    char cur[32], len[32];
    const uint64_t cur_off = pb.current_time_ns() > t_start ? pb.current_time_ns() - t_start : 0;
    format_duration(cur_off, cur, sizeof(cur));
    format_duration(t_end > t_start ? t_end - t_start : 0, len, sizeof(len));
    if (mono_font_) ImGui::PushFont(mono_font_, 0.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kAccent, "T+%s", cur);
    if (mono_font_) ImGui::PopFont();
    ImGui::SameLine();

    float frac = pb.scrub_pos(t_start, t_end);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 240.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    if (ImGui::SliderFloat("##scrub", &frac, 0.0f, 1.0f, ""))
        pb.seek(PlaybackController::scrub_to_ns(frac, t_start, t_end));
    ImGui::PopStyleVar(2);

    ImGui::SameLine();
    if (mono_font_) ImGui::PushFont(mono_font_, 0.0f);
    ImGui::TextColored(kDim, "/ %s", len);
    if (mono_font_) ImGui::PopFont();
    ImGui::SameLine();
    if (pb.is_live())                              ImGui::TextColored(kGood, ICON_FA_TOWER_BROADCAST " LIVE");
    else if (pb.state() == PlaybackState::Playing) ImGui::TextColored(kAccent, ICON_FA_PLAY " %.2fx", pb.speed());
    else                                           ImGui::TextColored(kWarn, ICON_FA_PAUSE " PAUSED");

    // ── Sample the selected entity's altitude/speed on the PLAYBACK clock ─────
    // Keyed to playback time (not wall/frame time) so the X axis matches the T+
    // readout, pauses hold still, and scrubbing backwards restarts the trace.
    flecs::entity sel = state_.selected_entity;
    if (sel != last_plot_entity_) {
        plot_count_ = 0; plot_head_ = 0;
        last_plot_entity_ = sel;
    }
    if (!with_chart) { ImGui::End(); return; }
    if (world.is_alive(sel) && sel.has<Position>() && sel.has<Velocity>()) {
        const float t = static_cast<float>(static_cast<double>(pb.current_time_ns() - t_start) * 1e-9);
        const int newest = (plot_head_ + kPlotWindow - 1) % kPlotWindow;
        const float newest_t = plot_count_ > 0 ? plot_time_[newest] : 0.0f;
        if (plot_count_ > 0 && t < newest_t - 1e-3f) { plot_count_ = 0; plot_head_ = 0; }   // rewound
        if (plot_count_ == 0 || t - newest_t >= kPlotSampleSec) {
            const auto& p = sel.get<Position>();
            const auto& v = sel.get<Velocity>();
            plot_time_ [plot_head_] = t;
            plot_alt_  [plot_head_] = p.v.y * kM2Ft;
            plot_gnd_  [plot_head_] = (cbs_.ground_height ? cbs_.ground_height(p.v.x, p.v.z) : 0.0f) * kM2Ft;
            plot_speed_[plot_head_] = std::sqrt(v.v.x * v.v.x + v.v.y * v.v.y + v.v.z * v.v.z) * kMps2Kt;
            plot_head_ = (plot_head_ + 1) % kPlotWindow;
            if (plot_count_ < kPlotWindow) ++plot_count_;
        }
    }

    // Linearise the ring into draw-order scratch buffers (oldest → newest).
    static float xs[kPlotWindow], alt[kPlotWindow], gnd[kPlotWindow], spd[kPlotWindow];
    const int start = plot_count_ < kPlotWindow ? 0 : plot_head_;
    for (int i = 0; i < plot_count_; ++i) {
        const int idx = (start + i) % kPlotWindow;
        xs[i] = plot_time_[idx]; alt[i] = plot_alt_[idx]; gnd[i] = plot_gnd_[idx]; spd[i] = plot_speed_[idx];
    }

    ImPlot::PushStyleColor(ImPlotCol_FrameBg,    { 0, 0, 0, 0 });
    ImPlot::PushStyleColor(ImPlotCol_PlotBg,     { 0.02f, 0.04f, 0.07f, 0.55f });
    ImPlot::PushStyleColor(ImPlotCol_PlotBorder, { 0.16f, 0.24f, 0.34f, 0.60f });
    ImPlot::PushStyleColor(ImPlotCol_LegendBg,   { 0.04f, 0.06f, 0.10f, 0.75f });
    if (ImPlot::BeginPlot("##alt", { -1, -1 },
                          ImPlotFlags_NoTitle | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect |
                          ImPlotFlags_NoMouseText))
    {
        ImPlot::SetupAxis(ImAxis_X1, "T+ (s)");
        ImPlot::SetupAxis(ImAxis_Y1, "ft MSL");
        ImPlot::SetupAxis(ImAxis_Y2, "kt", ImPlotAxisFlags_Opposite | ImPlotAxisFlags_NoGridLines);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);

        // Explicit, padded limits recomputed every frame (AutoFit misbehaves on
        // flat traces); X pinned to the sample span so the trace fills the plot.
        if (plot_count_ >= 2) {
            const double x1 = xs[plot_count_ - 1];
            const double x0 = std::min(static_cast<double>(xs[0]), x1 - 5.0);
            ImPlot::SetupAxisLimits(ImAxis_X1, x0, x1, ImPlotCond_Always);
            float alo = 1e30f, ahi = -1e30f, slo = 1e30f, shi = -1e30f;
            for (int i = 0; i < plot_count_; ++i) {
                alo = std::min({ alo, alt[i], gnd[i] }); ahi = std::max({ ahi, alt[i], gnd[i] });
                slo = std::min(slo, spd[i]);             shi = std::max(shi, spd[i]);
            }
            auto padded = [](float lo, float hi, float min_span) {
                const float span = std::max(hi - lo, min_span);
                return std::pair<double, double>{ lo - span * 0.10, hi + span * 0.12 };
            };
            auto [a0, a1] = padded(alo, ahi, 200.0f);
            auto [s0, s1] = padded(slo, shi, 10.0f);
            ImPlot::SetupAxisLimits(ImAxis_Y1, a0, a1, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y2, s0, s1, ImPlotCond_Always);

            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            ImPlotSpec gspec;                                         // terrain profile
            gspec.LineColor = ImVec4{ 0.62f, 0.50f, 0.32f, 1.0f };
            gspec.FillColor = ImVec4{ 0.45f, 0.36f, 0.22f, 0.45f };
            gspec.LineWeight = 1.0f;
            ImPlot::PlotShaded("Terrain", xs, gnd, plot_count_, a0, gspec);
            ImPlot::PlotLine("Terrain", xs, gnd, plot_count_, gspec);

            ImPlotSpec aspec;
            aspec.LineColor  = kAccent;
            aspec.LineWeight = 2.0f;
            ImPlot::PlotLine("Altitude", xs, alt, plot_count_, aspec);

            ImPlotSpec sspec;
            sspec.LineColor  = ImVec4{ 1.00f, 0.72f, 0.20f, 0.90f };
            sspec.LineWeight = 1.5f;
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlot::PlotLine("Speed", xs, spd, plot_count_, sspec);
        }
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor(4);
    ImGui::End();
}

// ── Left: tracks / view ───────────────────────────────────────────────────────
void AfterActionUI::draw_track_panel(const flecs::world& world, float bottom_offset) {
    const float top = kTopBarHeight + kPad;
    const float h = std::max(160.0f, static_cast<float>(GetScreenHeight()) - top - bottom_offset - kPad);
    ImGui::SetNextWindowPos({ kPad, top });
    ImGui::SetNextWindowSize({ kLeftW, h });
    ImGui::Begin("##SidePanel", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::BeginTabBar("LeftTabs")) {
        if (ImGui::BeginTabItem(ICON_FA_LIST "  Tracks")) { draw_tracks_tab(world); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(ICON_FA_CAMERA "  View"))  { draw_view_tab();       ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void AfterActionUI::draw_tracks_tab(const flecs::world& world) {
    int n = 0;
    world.query<const EntityMeta>().each([&](const EntityMeta&) { ++n; });

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kDim, "%d track%s", n, n == 1 ? "" : "s");
    const float clear_w = ImGui::CalcTextSize(ICON_FA_TRASH " Clear").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - clear_w);
    if (tinted_button(ICON_FA_TRASH " Clear", kBad) && cbs_.on_clear_entities) cbs_.on_clear_entities();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove all tracks and clear the telemetry buffer");

    const float list_h = ImGui::GetContentRegionAvail().y - (state_.selected_entity.is_valid() ? 40.0f : 0.0f);
    if (ImGui::BeginTable("##tracks", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_BordersInnerH,
                          { 0, std::max(80.0f, list_h) }))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed, 20.0f);
        ImGui::TableSetupColumn("Callsign", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ft",       ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("kt",       ImGuiTableColumnFlags_WidthFixed, 38.0f);
        ImGui::TableHeadersRow();

        world.query<const EntityMeta, const Position, const Velocity>()
            .each([&](flecs::entity e, const EntityMeta& meta, const Position& pos, const Velocity& vel) {
                ImGui::PushID(static_cast<int>((meta.source_id << 16) ^ meta.entity_id));
                ImGui::TableNextRow();
                const bool selected = state_.selected_entity == e;
                ImVec4 col = entity_type_color(meta.type);
                if (!meta.active) col = ImVec4(0.45f, 0.48f, 0.55f, 0.7f);

                ImGui::TableNextColumn();
                ImGui::TextColored(col, "%s", entity_type_icon(meta.type));
                ImGui::TableNextColumn();
                char label[48];
                if (meta.callsign[0]) snprintf(label, sizeof(label), "%s", meta.callsign);
                else                  snprintf(label, sizeof(label), "#%u", meta.entity_id);
                if (!meta.active) ImGui::PushStyleColor(ImGuiCol_Text, col);
                if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns))
                    state_.selected_entity = selected ? flecs::entity{} : e;
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    state_.selected_entity = e;
                    state_.camera_mode = 1;
                }
                if (!meta.active) ImGui::PopStyleColor();

                if (mono_font_) ImGui::PushFont(mono_font_, 0.0f);
                ImGui::TableNextColumn();
                if (meta.active) ImGui::TextColored(kDim, "%5.0f", pos.v.y * kM2Ft);
                else             ImGui::TextDisabled("  ---");
                ImGui::TableNextColumn();
                const float kt = std::sqrt(vel.v.x * vel.v.x + vel.v.y * vel.v.y + vel.v.z * vel.v.z) * kMps2Kt;
                if (meta.active) ImGui::TextColored(kDim, "%4.0f", kt);
                if (mono_font_) ImGui::PopFont();
                ImGui::PopID();
            });
        ImGui::EndTable();
    }
    if (n == 0) ImGui::TextDisabled("Waiting for UDP on :%u...", state_.listen_port);

    if (state_.selected_entity.is_valid()) {
        const float bw = (ImGui::GetContentRegionAvail().x - 8.0f) / 3.0f;
        auto mode_btn = [&](const char* label, int mode) {
            const bool on = state_.camera_mode == mode;
            if (on ? tinted_button(label, kAccent, { bw, 0 }) : ImGui::Button(label, { bw, 0 }))
                state_.camera_mode = mode;
        };
        mode_btn(ICON_FA_EYE " Free", 0);           ImGui::SameLine(0, 4);
        mode_btn(ICON_FA_CROSSHAIRS " Focus", 1);   ImGui::SameLine(0, 4);
        mode_btn(ICON_FA_VIDEO " Chase", 2);
    }
}

void AfterActionUI::draw_view_tab() {
    section_header("CAMERA");
    const char* cam_modes[] = { "Free orbit", "Focus track", "Chase track" };
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##CamMode", &state_.camera_mode, cam_modes, IM_ARRAYSIZE(cam_modes));
    if (state_.camera_mode > 0 && !state_.selected_entity.is_valid())
        ImGui::TextColored(kWarn, ICON_FA_CIRCLE_EXCLAMATION "  Select a track first");
    const float half = (ImGui::GetContentRegionAvail().x - 6.0f) * 0.5f;
    if (ImGui::Button(ICON_FA_HOUSE " Home", { half, 0 }) && cbs_.on_go_home) cbs_.on_go_home();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fly to the home position (%.4f, %.4f)", state_.origin_lat, state_.origin_lon);
    ImGui::SameLine(0, 6);
    if (ImGui::Button(ICON_FA_LOCATION_DOT " Set home", { half, 0 }) && cbs_.on_set_home_here) cbs_.on_set_home_here();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Make the current view centre the home position");

    section_header("SCENE");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##Scale", &state_.entity_3d_scale, 0.25f, 4.0f, "Track size  %.2fx");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##AltExag", &state_.altitude_exaggerate, 1.0f, 5.0f, "Vertical scale  %.1fx");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stretches terrain and tracks together");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##TWidth", &state_.trail_width_override, 5.0f, 500.0f, "Ribbon width  %.0f m");

    section_header("TERRAIN");
    const char* terrain_modes[] = { "Off", "Tactical grid", "Shaded", "Shaded + grid", "Contours" };
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##TerrainMode", &state_.terrain_mode, terrain_modes, IM_ARRAYSIZE(terrain_modes));
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##TerrainDetail", &state_.terrain_detail, 0.8f, 4.0f, "Detail  %.1f");

    section_header("OVERLAYS");
    ImGui::Checkbox("Labels", &state_.show_labels);
    ImGui::SameLine(kLeftW * 0.5f);
    ImGui::Checkbox("Trails", &state_.show_trails);
    ImGui::Checkbox("Ribbons", &state_.ribbon_trails);
    ImGui::SameLine(kLeftW * 0.5f);
    ImGui::Checkbox("Velocity", &state_.show_velocity_vec);
    ImGui::Checkbox("Minimap", &state_.show_minimap);
    ImGui::SameLine(kLeftW * 0.5f);
    ImGui::Checkbox("Network", &state_.show_stats);

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Controls")) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        const char* rows[][2] = {
            { "LMB",          "select track" },     { "Double-click", "focus track" },
            { "RMB drag",     "orbit" },            { "MMB drag",     "pan" },
            { "Wheel",        "zoom" },             { "WASD / arrows", "pan (Shift = fast)" },
            { "F / G",        "focus / chase" },    { "Space",        "play / pause" },
            { "1 2 3 4",      "0.25x 1x 4x -1x" },  { "R",            "ribbon trails" },
        };
        if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit)) {
            for (auto& r : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r[0]);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r[1]);
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleColor();
    }
}

// ── Right column: inspector, minimap, network ─────────────────────────────────
void AfterActionUI::draw_right_column(const flecs::world& world, const net::ReceiverStats& stats,
                                      float bottom_offset)
{
    const float x = static_cast<float>(GetScreenWidth()) - kRightW - kPad;
    float y = kTopBarHeight + kPad;
    const float limit = static_cast<float>(GetScreenHeight()) - bottom_offset - kPad;

    if (state_.selected_entity.is_valid()) {
        ImGui::SetNextWindowPos({ x, y });
        ImGui::SetNextWindowSize({ kRightW, 0 });
        ImGui::Begin("##Inspector", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        draw_inspector(world);
        y += ImGui::GetWindowHeight() + kPad;
        ImGui::End();
    }
    if (state_.show_minimap && state_.scene.minimap.id != 0 && y + 200.0f < limit) {
        const float size = std::min(kRightW, limit - y);
        ImGui::SetNextWindowPos({ x, y });
        ImGui::SetNextWindowSize({ kRightW, size });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
        ImGui::Begin("##Minimap", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
        draw_minimap(world);
        ImGui::End();
        ImGui::PopStyleVar();
        y += size + kPad;
    }
    if (state_.show_stats && y + 130.0f < limit) {
        ImGui::SetNextWindowPos({ x, y });
        ImGui::SetNextWindowSize({ kRightW, 0 });
        ImGui::Begin(ICON_FA_SATELLITE_DISH "  Network", &state_.show_stats,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings);
        draw_network(stats);
        ImGui::End();
    }
}

void AfterActionUI::draw_inspector(const flecs::world& /*world*/) {
    auto e = state_.selected_entity;
    const auto& meta = e.get<EntityMeta>();
    const auto& pos  = e.get<Position>();
    const auto& vel  = e.get<Velocity>();
    const Quaternion q = e.has<Rotation>() ? e.get<Rotation>().q : Quaternion{ 0, 0, 0, 1 };

    // Header: icon + callsign, type and id, close button.
    const ImVec4 col = entity_type_color(meta.type);
    if (heading_font_) ImGui::PushFont(heading_font_, 0.0f);
    ImGui::TextColored(col, "%s", entity_type_icon(meta.type));
    ImGui::SameLine();
    if (meta.callsign[0]) ImGui::Text("%.*s", net::kCallsignLen - 1, meta.callsign);
    else                  ImGui::Text("#%u", meta.entity_id);
    if (heading_font_) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(kDim, "%s  %u/%u", entity_type_name(meta.type), meta.source_id, meta.entity_id);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 18.0f);
    if (ImGui::SmallButton(ICON_FA_XMARK "##close")) { state_.selected_entity = {}; return; }
    if (!meta.active) ImGui::TextColored(kBad, ICON_FA_SKULL "  DESTROYED / NOT ACTIVE");

    double lat = 0.0, lon = 0.0;
    if (cbs_.to_geo) cbs_.to_geo(pos.v.x, pos.v.z, &lat, &lon);
    const float gnd = cbs_.ground_height ? cbs_.ground_height(pos.v.x, pos.v.z) : 0.0f;
    const float agl = pos.v.y - gnd;
    const float spd = std::sqrt(vel.v.x * vel.v.x + vel.v.y * vel.v.y + vel.v.z * vel.v.z);
    float hdg, pitch, roll;
    euler_from_quat(q, &hdg, &pitch, &roll);

    char la[24], lo[24];
    format_dms(lat, true, la, sizeof(la));
    format_dms(lon, false, lo, sizeof(lo));

    if (ImGui::BeginTable("##insp", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 74.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        auto row = [&](const char* k, ImVec4 c, const char* fmt, auto... args) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kDim, "%s", k);
            ImGui::TableNextColumn();
            if (mono_font_) ImGui::PushFont(mono_font_, 0.0f);
            ImGui::TextColored(c, fmt, args...);
            if (mono_font_) ImGui::PopFont();
        };
        const ImVec4 white = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        row("Position", white, "%s", la);
        row("",         white, "%s", lo);
        row("Altitude", white, "%6.0f ft  %5.0f m", pos.v.y * kM2Ft, pos.v.y);
        row("AGL",      agl < 0 ? kBad : (agl < 150 ? kWarn : white), "%6.0f ft  %5.0f m", agl * kM2Ft, agl);
        row("Ground",   kDim,  "%6.0f ft  %5.0f m", gnd * kM2Ft, gnd);
        row("Speed",    white, "%6.0f kt  %5.0f m/s", spd * kMps2Kt, spd);
        row("Vert spd", white, "%+6.0f fpm", vel.v.y * 196.85f);
        row("Heading",  white, "%05.1f\xc2\xb0", hdg);
        row("Pitch",    white, "%+5.1f\xc2\xb0", pitch);
        row("Roll",     white, "%+5.1f\xc2\xb0", roll);
        ImGui::EndTable();
    }
}

void AfterActionUI::draw_minimap(const flecs::world& world) {
    const SceneInfo& sc = state_.scene;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float sz = std::min(avail.x, avail.y);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1{ p0.x + sz, p0.y + sz };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage(ImTextureRef(ImTextureID(sc.minimap.id)), p0, p1);
    dl->AddRect(p0, p1, IM_COL32(40, 70, 100, 200));

    const float half = sc.map_range * 0.5f;
    auto to_px = [&](float x, float z) {
        return ImVec2{ p0.x + (x - (sc.map_cx - half)) / sc.map_range * sz,
                       p0.y + (z - (sc.map_cz - half)) / sc.map_range * sz };
    };
    dl->PushClipRect(p0, p1, true);

    // Camera: position + view direction wedge.
    {
        const ImVec2 c = to_px(sc.cam_x, sc.cam_z);
        const float a = sc.cam_yaw_deg * DEG2RAD;
        const float L = 26.0f, spread = 0.45f;
        auto dir = [&](float ang) { return ImVec2{ c.x + std::sin(ang) * L, c.y - std::cos(ang) * L }; };
        dl->AddTriangleFilled(c, dir(a - spread), dir(a + spread), IM_COL32(255, 255, 255, 34));
        dl->AddCircleFilled(c, 3.0f, IM_COL32(255, 255, 255, 200));
    }
    // Tracks
    world.query<const EntityMeta, const Position, const Velocity>()
        .each([&](flecs::entity e, const EntityMeta& meta, const Position& p, const Velocity& v) {
            if (!meta.active) return;
            const ImVec2 c = to_px(p.v.x, p.v.z);
            const bool sel = e == state_.selected_entity;
            const ImU32 col = sel ? IM_COL32(255, 220, 0, 255) : ImGui::GetColorU32(entity_type_color(meta.type));
            const float sp = std::sqrt(v.v.x * v.v.x + v.v.z * v.v.z);
            if (sp > 1.0f) {
                const float L = sel ? 14.0f : 10.0f;
                dl->AddLine(c, { c.x + v.v.x / sp * L, c.y + v.v.z / sp * L }, col, 1.5f);
            }
            if (sel) dl->AddCircle(c, 8.0f, IM_COL32(255, 220, 0, 160), 0, 1.5f);
            dl->AddCircleFilled(c, sel ? 4.5f : 3.5f, col);
            dl->AddCircle(c, sel ? 4.5f : 3.5f, IM_COL32(0, 0, 0, 160));
        });
    dl->PopClipRect();

    // North arrow and scale bar.
    dl->AddText({ p0.x + 8, p0.y + 6 }, IM_COL32(230, 236, 245, 230), "N " ICON_FA_ARROW_UP);
    float nice = 1.0f;
    for (float c : { 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f,
                     50000.0f, 100000.0f, 200000.0f })
        if (c <= sc.map_range * 0.3f) nice = c;
    const float bar = nice / sc.map_range * sz;
    const ImVec2 b0{ p0.x + 10, p1.y - 12 };
    dl->AddLine(b0, { b0.x + bar, b0.y }, IM_COL32(240, 240, 240, 230), 2.0f);
    dl->AddLine({ b0.x, b0.y - 4 }, { b0.x, b0.y + 1 }, IM_COL32(240, 240, 240, 230), 2.0f);
    dl->AddLine({ b0.x + bar, b0.y - 4 }, { b0.x + bar, b0.y + 1 }, IM_COL32(240, 240, 240, 230), 2.0f);
    char lbl[24];
    if (nice >= 1000.0f) snprintf(lbl, sizeof(lbl), "%.0f km", nice / 1000.0f);
    else                 snprintf(lbl, sizeof(lbl), "%.0f m", nice);
    dl->AddText({ b0.x + bar + 6, b0.y - 9 }, IM_COL32(240, 240, 240, 230), lbl);

    ImGui::InvisibleButton("##map", { sz, sz });
    if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetMousePos();
        const float wx = sc.map_cx - half + (m.x - p0.x) / sz * sc.map_range;
        const float wz = sc.map_cz - half + (m.y - p0.y) / sz * sc.map_range;
        double lat = 0, lon = 0;
        if (cbs_.to_geo) cbs_.to_geo(wx, wz, &lat, &lon);
        const float h = cbs_.ground_height ? cbs_.ground_height(wx, wz) : 0.0f;
        ImGui::SetTooltip("%.4f, %.4f\n%.0f m (%.0f ft)\nclick: look here", lat, lon, h, h * kM2Ft);
        if (ImGui::IsItemClicked() && cbs_.on_look_at) cbs_.on_look_at(wx, wz);
    }
}

void AfterActionUI::draw_network(const net::ReceiverStats& stats) {
    if (mono_font_) ImGui::PushFont(mono_font_, 0.0f);
    ImGui::Text("Listen     %s:%u", state_.bind_addr, state_.listen_port);
    ImGui::Text("Packets    %llu", (unsigned long long)stats.packets_received);
    ImGui::Text("Received   %.1f KB", stats.bytes_received / 1024.0);
    ImGui::Text("Dropped    %llu", (unsigned long long)stats.packets_dropped);
    ImGui::Text("Parse err  %llu", (unsigned long long)stats.parse_errors);
    ImGui::Text("Seq gaps   %llu", (unsigned long long)stats.sequence_gaps);
    if (mono_font_) ImGui::PopFont();
}

// ── Settings Window ───────────────────────────────────────────────────────────
void AfterActionUI::draw_settings_window() {
    if (!state_.show_settings_window) return;

    const ImVec2 center{ GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f };
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, { 0.5f, 0.5f });
    ImGui::SetNextWindowSize({ 460.0f, 540.0f }, ImGuiCond_FirstUseEver);
    if (ImGui::Begin(ICON_FA_GEAR "  Settings", &state_.show_settings_window, ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::BeginTabBar("SettingsTabs")) {

            // ── Network ───────────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Network")) {
                ImGui::TextDisabled("UDP telemetry input. Apply restarts the listener.");
                ImGui::Spacing();
                ImGui::Text("Bind address");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##bind_addr", state_.bind_addr, sizeof(state_.bind_addr));
                ImGui::TextDisabled("0.0.0.0 = all interfaces");
                ImGui::Spacing();
                ImGui::Text("Listen port");
                int port = static_cast<int>(state_.listen_port);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputInt("##listen_port", &port)) {
                    port = std::clamp(port, 1, 65535);
                    state_.listen_port = static_cast<uint16_t>(port);
                }
                ImGui::Spacing();
                if (tinted_button("Apply / restart listener", kGood, { -1, 0 }) && cbs_.on_apply_network)
                    cbs_.on_apply_network(state_.bind_addr, state_.listen_port);
                ImGui::EndTabItem();
            }

            // ── Session Files ─────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Files")) {
                ImGui::TextDisabled("Open a recorded .aar session or import a CSV log.");
                if (ImGui::Button(ICON_FA_FOLDER_OPEN " Browse...") && cbs_.on_browse_file)
                    cbs_.on_browse_file();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##load_path", "...or paste a path to a .aar / .csv file",
                                         state_.load_path, sizeof(state_.load_path));
                const bool has_path = state_.load_path[0] != '\0';
                ImGui::BeginDisabled(!has_path);
                if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open .aar") && cbs_.on_load_file)
                    cbs_.on_load_file(state_.load_path);
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_FILE_IMPORT " Import CSV") && cbs_.on_load_csv)
                    cbs_.on_load_csv(state_.load_path);
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::TextDisabled("Remove all current tracks and clear the buffer.");
                if (tinted_button("Clear all tracks", kBad, { -1, 0 }) && cbs_.on_clear_entities)
                    cbs_.on_clear_entities();
                ImGui::EndTabItem();
            }

            // ── Display ───────────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Display")) {
                ImGui::Checkbox("Callsign labels",  &state_.show_labels);
                ImGui::Checkbox("Trail lines",      &state_.show_trails);
                ImGui::Checkbox("Ribbon trails",    &state_.ribbon_trails);
                ImGui::Checkbox("Velocity vectors", &state_.show_velocity_vec);
                ImGui::Checkbox("Minimap",          &state_.show_minimap);
                ImGui::Checkbox("Network panel",    &state_.show_stats);
                ImGui::Separator();
                ImGui::Text("Chart panel height");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderFloat("##TimelineH", &state_.timeline_height, 140.0f, 500.0f, "%.0f px");
                ImGui::Text("Mouse");
                ImGui::Checkbox("Invert rotate (grab the scene)", &state_.invert_look);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderFloat("##MouseSens", &state_.mouse_sensitivity, 0.2f, 3.0f, "Sensitivity %.2fx");
                ImGui::EndTabItem();
            }

            // ── Terrain ───────────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Terrain")) {
                const SceneInfo& sc = state_.scene;
                ImGui::Text("Look");
                const char* terrain_modes[] = { "Off", "Tactical grid", "Shaded", "Shaded + grid", "Contours" };
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::Combo("##SettingsTerrainMode", &state_.terrain_mode, terrain_modes, IM_ARRAYSIZE(terrain_modes));
                ImGui::Text("Detail");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderFloat("##TerrainDetail2", &state_.terrain_detail, 0.8f, 4.0f, "%.1f");
                ImGui::Text("Vertical scale (terrain + tracks)");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderFloat("##SettingsAltExag", &state_.altitude_exaggerate, 1.0f, 5.0f, "%.1fx");
                ImGui::Text("Render distance cap");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderFloat("##FarClip", &state_.far_clip_plane, 10000.0f, 3000000.0f, "%.0f m",
                                   ImGuiSliderFlags_Logarithmic);

                section_header("HOME POSITION");
                ImGui::TextDisabled("Where the scene starts before any data arrives.");
                ImGui::SetNextItemWidth(130);
                ImGui::InputDouble("Lat##home", &state_.origin_lat, 0, 0, "%.5f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(130);
                ImGui::InputDouble("Lon##home", &state_.origin_lon, 0, 0, "%.5f");
                if (ImGui::Button(ICON_FA_HOUSE " Go home") && cbs_.on_go_home) cbs_.on_go_home();
                ImGui::SameLine();
                if (ImGui::Button("Ak\xc4\xb1nc\xc4\xb1")) { state_.origin_lat = 40.0789; state_.origin_lon = 32.5656; }
                ImGui::SameLine();
                if (ImGui::Button("\xc4\xb0ncirlik")) { state_.origin_lat = 37.0021; state_.origin_lon = 35.4259; }
                ImGui::SameLine();
                if (ImGui::Button("\xc3\x87i\xc4\x9fli")) { state_.origin_lat = 38.5130; state_.origin_lon = 27.0101; }

                section_header("DATA");
                ImGui::TextWrapped("%s", sc.terrain_status.c_str());
                ImGui::TextDisabled("Scene origin %.5f, %.5f", sc.origin_lat, sc.origin_lon);
                if (mono_font_) ImGui::PushFont(mono_font_, 0.0f);
                for (const auto& l : sc.terrain_layers) ImGui::TextColored(kDim, "%s", l.c_str());
                ImGui::TextColored(kDim, "chunks %d drawn / %d cached / %d queued  %.0f MB",
                                   sc.chunks_drawn, sc.chunks_cached, sc.chunks_queued, sc.terrain_gpu_mb);
                if (mono_font_) ImGui::PopFont();
                ImGui::EndTabItem();
            }

            // ── Models ────────────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Models")) {
                ImGui::TextWrapped("Bind 3D models (FBX / OBJ / glTF) to entity types. "
                    "Put files in the app's assets/models/ folder. Load applies live; "
                    "Save writes assets/models.yaml so it persists across launches.");
                ImGui::TextDisabled("Tip: preview & tune scale/rotation with the 'hangar' tool.");
                ImGui::Separator();

                const char* kTypeNames[] = { "Unknown", "Jet", "Missile", "AAA", "Ground", "Helo", "Ship" };
                int remove_idx = -1;
                for (int i = 0; i < (int)state_.model_bindings.size(); ++i) {
                    auto& b = state_.model_bindings[i];
                    ImGui::PushID(i);
                    const int tclamp = (b.type >= 0 && b.type < 7) ? b.type : 0;
                    char hdr[320];
                    snprintf(hdr, sizeof(hdr), "Type %d (%s)  %s###mb%d",
                             b.type, kTypeNames[tclamp], b.file[0] ? b.file : "<no file>", i);
                    if (ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::SetNextItemWidth(-1.0f);
                        ImGui::Combo("##type", &b.type, kTypeNames, IM_ARRAYSIZE(kTypeNames));
                        ImGui::SetNextItemWidth(-1.0f);
                        ImGui::InputTextWithHint("##file", "models/yourmodel.glb", b.file, sizeof(b.file));
                        ImGui::SliderFloat("Scale", &b.scale, 0.001f, 50.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
                        ImGui::ColorEdit3("Tint", b.tint);
                        ImGui::SliderFloat("Yaw",   &b.yaw,   -180, 180, "%.0f");
                        ImGui::SliderFloat("Pitch", &b.pitch, -180, 180, "%.0f");
                        ImGui::SliderFloat("Roll",  &b.roll,  -180, 180, "%.0f");

                        ImGui::BeginDisabled(b.file[0] == '\0');
                        if (ImGui::Button("Load / Apply") && cbs_.on_model_load) {
                            ModelBindRequest r;
                            r.type  = (uint16_t)b.type;
                            r.file  = b.file;
                            r.scale = b.scale;
                            r.tint[0] = (uint8_t)(b.tint[0] * 255);
                            r.tint[1] = (uint8_t)(b.tint[1] * 255);
                            r.tint[2] = (uint8_t)(b.tint[2] * 255);
                            r.yaw = b.yaw; r.pitch = b.pitch; r.roll = b.roll;
                            cbs_.on_model_load(r);
                            snprintf(b.status, sizeof(b.status), "loading...");
                        }
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        if (ImGui::Button("Revert to procedural") && cbs_.on_model_clear) {
                            cbs_.on_model_clear((uint16_t)b.type);
                            snprintf(b.status, sizeof(b.status), "reverted");
                        }
                        ImGui::SameLine();
                        if (tinted_button("Remove", kBad)) remove_idx = i;
                        if (b.status[0]) { ImGui::SameLine(); ImGui::TextDisabled("%s", b.status); }
                    }
                    ImGui::PopID();
                }
                if (remove_idx >= 0) {
                    const uint16_t t = (uint16_t)state_.model_bindings[remove_idx].type;
                    if (cbs_.on_model_clear) cbs_.on_model_clear(t);
                    state_.model_bindings.erase(state_.model_bindings.begin() + remove_idx);
                }
                ImGui::Separator();
                if (ImGui::Button("+ Add binding")) state_.model_bindings.push_back(UIState::ModelBinding{});
                ImGui::SameLine();
                if (ImGui::Button("Save manifest") && cbs_.on_models_save) {
                    std::vector<ModelBindRequest> reqs;
                    for (auto& b : state_.model_bindings) {
                        if (b.file[0] == '\0') continue;
                        ModelBindRequest r;
                        r.type = (uint16_t)b.type; r.file = b.file; r.scale = b.scale;
                        r.tint[0] = (uint8_t)(b.tint[0] * 255); r.tint[1] = (uint8_t)(b.tint[1] * 255);
                        r.tint[2] = (uint8_t)(b.tint[2] * 255);
                        r.yaw = b.yaw; r.pitch = b.pitch; r.roll = b.roll;
                        reqs.push_back(std::move(r));
                    }
                    cbs_.on_models_save(reqs);
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace afteraction
