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
#include <initializer_list>

namespace afteraction {

using namespace ecs;

namespace {

// Layout metrics at 100 % scale; S() applies the interface scale.
constexpr float kPad    = 8.0f;     // gap between panels and screen edges
constexpr float kLeftW  = 280.0f;   // track panel width
constexpr float kRightW = 300.0f;   // inspector / minimap width
float g_scale = 1.0f;
inline float S(float v) { return v * g_scale; }

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
    case net::TYPE_UAV:     return ICON_FA_PLANE_UP;
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
    case net::TYPE_UAV:     return "UAV";
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
    case net::TYPE_UAV:     return { 0.78f, 0.47f, 1.00f, 1.0f };
    default:                return { 0.20f, 0.80f, 1.00f, 1.0f };
    }
}

void format_duration(uint64_t ns, char* buf, size_t n) {
    uint64_t sec  = ns / 1'000'000'000ULL;
    uint64_t ms   = (ns % 1'000'000'000ULL) / 1'000'000ULL;
    uint64_t mins = sec / 60; sec %= 60;
    uint64_t hour = mins / 60; mins %= 60;
    snprintf(buf, n, "%02llu:%02llu:%02llu.%03llu",
             (unsigned long long)hour, (unsigned long long)mins,
             (unsigned long long)sec, (unsigned long long)ms);
}

// Small coloured caption ("CAMERA", "TERRAIN" …) with a rule.
void section_header(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
}

// Grey explanatory line under a setting.
void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
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

// Status chip (LIVE, Record): the normal button face with a coloured dot.
//   on   — thin coloured outline, faint tint, coloured text, pulsing dot
//   off  — plain button; the dot is solid (`dot_when_off`) or a dim ring
bool chip_button(const char* id, const char* text, ImVec4 col, bool on, bool dot_when_off) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float dot_r = S(3.5f), gap = S(7.0f);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 size{ st.FramePadding.x * 2.0f + dot_r * 2.0f + gap + ts.x, ImGui::GetFrameHeight() };
    const ImVec2 p = ImGui::GetCursorScreenPos();

    if (on) {
        const ImVec4 f = st.Colors[ImGuiCol_Button];
        const ImVec4 face{ f.x + (col.x - f.x) * 0.10f, f.y + (col.y - f.y) * 0.10f, f.z + (col.z - f.z) * 0.10f, f.w };
        const ImVec4 hover{ f.x + (col.x - f.x) * 0.20f, f.y + (col.y - f.y) * 0.20f, f.z + (col.z - f.z) * 0.20f, f.w };
        ImGui::PushStyleColor(ImGuiCol_Button, face);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, hover);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(col.x, col.y, col.z, 0.45f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, S(1.0f)));
    }
    const bool clicked = ImGui::Button(id, size);
    if (on) { ImGui::PopStyleVar(); ImGui::PopStyleColor(4); }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 dot{ p.x + st.FramePadding.x + dot_r, p.y + size.y * 0.5f };
    const ImVec2 tp{ dot.x + dot_r + gap, p.y + (size.y - ts.y) * 0.5f };
    if (on) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
        dl->AddCircleFilled(dot, dot_r * (1.6f + 0.8f * pulse), ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.18f * (1.0f - pulse))));
        dl->AddCircleFilled(dot, dot_r, ImGui::GetColorU32(col));
        dl->AddText(tp, ImGui::GetColorU32(col), text);
    } else {
        if (dot_when_off) dl->AddCircleFilled(dot, dot_r, ImGui::GetColorU32(col));
        else              dl->AddCircle(dot, dot_r, ImGui::GetColorU32(kDim), 0, std::max(1.0f, S(1.2f)));
        dl->AddText(tp, ImGui::GetColorU32(ImGuiCol_Text), text);
    }
    return clicked;
}

// A labelled row in a two-column settings table: label left, widget right.
bool begin_settings_table(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, S(150.0f));
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}
void row_label(const char* label, const char* tooltip = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// Segmented control: one button per option, the active one highlighted.
bool segmented(const char* id, int* value, std::initializer_list<const char*> options) {
    bool changed = false;
    ImGui::PushID(id);
    int i = 0;
    const float w = (ImGui::GetContentRegionAvail().x - (options.size() - 1) * S(2.0f)) / float(options.size());
    for (const char* o : options) {
        if (i > 0) ImGui::SameLine(0, S(2.0f));
        const bool on = *value == i;
        if (on ? tinted_button(o, kAccent, { w, 0 }) : ImGui::Button(o, { w, 0 })) {
            if (!on) changed = true;
            *value = i;
        }
        ++i;
    }
    ImGui::PopID();
    return changed;
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

// ── Fonts ─────────────────────────────────────────────────────────────────────
void AfterActionUI::push_mono() {
    ImGui::PushFont(mono_font_, ImGui::GetStyle().FontSizeBase * 0.92f);
}
void AfterActionUI::push_heading() {
    ImGui::PushFont(heading_font_, ImGui::GetStyle().FontSizeBase * 1.18f);
}
void AfterActionUI::pop_font() { ImGui::PopFont(); }

// ── Theme ─────────────────────────────────────────────────────────────────────
void AfterActionUI::apply_theme(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    const float font_base = style.FontSizeBase;
    style = ImGuiStyle();   // start from defaults so re-scaling never compounds
    style.FontSizeBase = font_base;
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

    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;
}

// ── Main draw ─────────────────────────────────────────────────────────────────
float AfterActionUI::timeline_height(const TelemetryStore& store) const {
    auto [ts, te] = store.time_range_ns();
    if (ts == 0 && te == 0) return 0.0f;
    if (!state_.selected_entity.is_valid()) return S(62.0f);          // scrubber only
    return std::clamp(S(state_.timeline_height), S(140.0f), static_cast<float>(GetScreenHeight()) * 0.6f);
}

void AfterActionUI::draw(PlaybackController& pb,
                         const TelemetryStore& store,
                         const net::ReceiverStats& net_stats,
                         const persist::Recorder& recorder,
                         const flecs::world& world)
{
    // Re-build the style when the interface scale changes (not while a slider
    // is being dragged, so the slider doesn't move under the mouse).
    state_.ui_scale = std::clamp(state_.ui_scale, 0.75f, 2.0f);
    if (state_.ui_scale != applied_scale_ && !ImGui::IsAnyItemActive()) {
        apply_theme(state_.ui_scale);
        applied_scale_ = state_.ui_scale;
    }
    g_scale = applied_scale_;

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
    const UnitPrefs& u = state_.units;
    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize({ w, top_bar_height() });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10.0f), S(8.0f)));
    ImGui::Begin("##topbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);

    // Brand
    push_heading();
    ImGui::TextColored(kAccent, ICON_FA_LOCATION_CROSSHAIRS);
    ImGui::SameLine(0, S(6));
    ImGui::TextUnformatted("AfterAction");
    pop_font();
    ImGui::SameLine(0, S(18));

    // Recording: a red chip; while recording it shows the elapsed time.
    if (recorder.is_recording()) {
        if (rec_t0_ < 0.0) rec_t0_ = ImGui::GetTime();
        const int secs = static_cast<int>(ImGui::GetTime() - rec_t0_);
        char rec[32];
        snprintf(rec, sizeof(rec), "REC %02d:%02d", secs / 60, secs % 60);
        if (chip_button("##rec", rec, kBad, true, true) && cbs_.on_record_stop) cbs_.on_record_stop();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Recording — %llu frames written.\nClick to stop.",
                              (unsigned long long)recorder.frames_written());
    } else {
        rec_t0_ = -1.0;
        if (chip_button("##rec", "Record", kBad, false, true) && cbs_.on_record_start) cbs_.on_record_start();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Record everything to recordings/session_*.aar");
    }
    ImGui::SameLine(0, S(4));
    char save_lbl[48];
    snprintf(save_lbl, sizeof(save_lbl), ICON_FA_FLOPPY_DISK "  Last %.0f s", state_.dashcam_secs);
    if (ImGui::Button(save_lbl) && cbs_.on_save_dashcam) cbs_.on_save_dashcam(state_.dashcam_secs);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Save the last %.0f seconds of the buffer (dashcam).\nChange the length in Settings > Recording.",
                          state_.dashcam_secs);

    ImGui::SameLine(0, S(18));

    // Transport
    if (chip_button("##live", "LIVE", kGood, pb.is_live(), false) && !pb.is_live()) pb.stop();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(pb.is_live() ? "Showing live data" : "Back to live");
    ImGui::SameLine(0, S(4));
    if (ImGui::Button(ICON_FA_BACKWARD_FAST "##rew")) pb.set_speed(-4.0f);
    ImGui::SameLine(0, S(4));
    if (pb.state() == PlaybackState::Paused || pb.is_live()) {
        if (ImGui::Button(ICON_FA_PLAY "##play", { S(34), 0 })) pb.play(pb.speed());
    } else if (ImGui::Button(ICON_FA_PAUSE "##pause", { S(34), 0 })) {
        pb.pause();
    }
    ImGui::SameLine(0, S(4));
    if (ImGui::Button(ICON_FA_FORWARD_FAST "##ffwd")) pb.set_speed(4.0f);
    ImGui::SameLine(0, S(4));
    float spd = pb.speed();
    ImGui::SetNextItemWidth(S(110));
    if (ImGui::SliderFloat("##Speed", &spd, -8.0f, 8.0f, "%.2fx")) pb.set_speed(spd);

    ImGui::SameLine(0, S(18));
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open")) {
        if (cbs_.on_browse_file) cbs_.on_browse_file();
        else state_.show_settings_window = true;
    }
    ImGui::SameLine(0, S(4));
    if (ImGui::Button(ICON_FA_GEAR "##settings")) state_.show_settings_window = !state_.show_settings_window;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings");

    // ── Right side: cursor read-out, network, mode pill, FPS ─────────────────
    const SceneInfo& sc = state_.scene;
    char cursor[112] = "";
    if (sc.cursor_valid) {
        char la[32], lo[32];
        u.format_coord(sc.cursor_lat, true, la, sizeof(la));
        u.format_coord(sc.cursor_lon, false, lo, sizeof(lo));
        snprintf(cursor, sizeof(cursor), ICON_FA_LOCATION_DOT "  %s  %s   %.0f %s", la, lo,
                 u.alt_value(sc.cursor_elev), u.alt_suffix());
    }
    char net[48];
    snprintf(net, sizeof(net), ICON_FA_SATELLITE_DISH " %llu", (unsigned long long)stats.packets_received);
    const char* mode = sc.demo ? "DEMO" : pb.is_live() ? "LIVE" : pb.state() == PlaybackState::Playing ? "PLAYBACK" : "PAUSED";
    const ImVec4 mode_col = sc.demo ? kWarn : pb.is_live() ? kGood : pb.state() == PlaybackState::Playing ? kAccent : kWarn;
    char fps[16];
    snprintf(fps, sizeof(fps), "%d fps", sc.fps);

    push_mono();
    const float sp = S(18.0f);
    const float w_cursor = cursor[0] ? ImGui::CalcTextSize(cursor).x + sp : 0.0f;
    const float w_fps = ImGui::CalcTextSize(fps).x;
    pop_font();
    const float w_net  = ImGui::CalcTextSize(net).x + sp;
    const float w_mode = ImGui::CalcTextSize(mode).x + S(16.0f) + sp;
    float x = w - S(10.0f) - w_fps - w_mode - w_net - w_cursor;
    const float min_x = ImGui::GetCursorPosX() + S(24.0f);
    if (x < min_x && cursor[0]) { x += w_cursor; cursor[0] = '\0'; }   // narrow window: drop cursor
    ImGui::SameLine(std::max(x, min_x));
    if (cursor[0]) {
        push_mono();
        ImGui::TextColored(kDim, "%s", cursor);
        pop_font();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Terrain under the cursor (%.5f, %.5f)\n%.0f %s / %.0f %s MSL, %.0f m DEM grid",
                              sc.cursor_lat, sc.cursor_lon, u.alt_value(sc.cursor_elev), u.alt_suffix(),
                              u.alt_value_alt(sc.cursor_elev), u.alt_suffix_alt(), sc.cursor_res);
        ImGui::SameLine(0, sp);
    }
    ImGui::TextColored(sc.demo ? kDim : !sc.net_ok ? kBad : stats.packets_received ? kGood : kDim, "%s", net);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%llu packets, %.1f KB\n%llu dropped, %llu parse errors, %llu gaps",
                          sc.net_status.c_str(),
                          (unsigned long long)stats.packets_received, stats.bytes_received / 1024.0,
                          (unsigned long long)stats.packets_dropped, (unsigned long long)stats.parse_errors,
                          (unsigned long long)stats.sequence_gaps);
    ImGui::SameLine(0, sp);
    {   // mode pill
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 ts = ImGui::CalcTextSize(mode);
        ImGui::GetWindowDrawList()->AddRectFilled({ p.x, p.y - 1 }, { p.x + ts.x + S(16), p.y + ts.y + 1 },
            ImGui::GetColorU32(ImVec4(mode_col.x, mode_col.y, mode_col.z, 0.22f)), S(9.0f));
        ImGui::SetCursorScreenPos({ p.x + S(8), p.y });
        ImGui::TextColored(mode_col, "%s", mode);
        ImGui::SameLine(0, S(8) + sp);
    }
    push_mono();
    ImGui::TextColored(sc.fps >= 30 ? kDim : kWarn, "%s", fps);
    pop_font();
    (void)store;
    ImGui::End();
}

// ── Timeline ──────────────────────────────────────────────────────────────────
void AfterActionUI::draw_timeline(PlaybackController& pb, const TelemetryStore& store,
                                  const flecs::world& world, float panel_h)
{
    auto [t_start, t_end] = store.time_range_ns();
    const UnitPrefs& u = state_.units;
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
        ImGui::InvisibleButton("##tl_resize", { w, S(6.0f) });
        const bool grip = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (grip) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive())
            state_.timeline_height = std::clamp(state_.timeline_height - ImGui::GetIO().MouseDelta.y / g_scale,
                                                140.0f, h * 0.6f / g_scale);
        ImGui::GetWindowDrawList()->AddLine({ wp.x, wp.y + 1.0f }, { wp.x + w, wp.y + 1.0f },
            grip ? ImGui::GetColorU32(kAccent) : IM_COL32(40, 70, 100, 130), 2.0f);
        ImGui::SetCursorScreenPos(saved);
    }

    // Row: T+ clock · scrubber · length · state
    char cur[32], len[32];
    const uint64_t cur_off = pb.current_time_ns() > t_start ? pb.current_time_ns() - t_start : 0;
    format_duration(cur_off, cur, sizeof(cur));
    format_duration(t_end > t_start ? t_end - t_start : 0, len, sizeof(len));
    push_mono();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kAccent, "T+%s", cur);
    pop_font();
    ImGui::SameLine();

    float frac = pb.scrub_pos(t_start, t_end);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(240.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, S(14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8.0f));
    if (ImGui::SliderFloat("##scrub", &frac, 0.0f, 1.0f, ""))
        pb.seek(PlaybackController::scrub_to_ns(frac, t_start, t_end));
    ImGui::PopStyleVar(2);

    ImGui::SameLine();
    push_mono();
    ImGui::TextColored(kDim, "/ %s", len);
    pop_font();
    ImGui::SameLine();
    if (pb.is_live())                              ImGui::TextColored(kGood, ICON_FA_TOWER_BROADCAST " LIVE");
    else if (pb.state() == PlaybackState::Playing) ImGui::TextColored(kAccent, ICON_FA_PLAY " %.2fx", pb.speed());
    else                                           ImGui::TextColored(kWarn, ICON_FA_PAUSE " PAUSED");

    // ── Sample the selected entity's altitude/speed on the PLAYBACK clock ─────
    // Keyed to playback time (not wall/frame time) so the X axis matches the T+
    // readout, pauses hold still, and scrubbing backwards restarts the trace.
    // Stored in SI units so a unit change re-labels the whole history.
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
            plot_alt_  [plot_head_] = p.v.y;
            plot_gnd_  [plot_head_] = cbs_.ground_height ? cbs_.ground_height(p.v.x, p.v.z) : 0.0f;
            plot_speed_[plot_head_] = std::sqrt(v.v.x * v.v.x + v.v.y * v.v.y + v.v.z * v.v.z);
            plot_head_ = (plot_head_ + 1) % kPlotWindow;
            if (plot_count_ < kPlotWindow) ++plot_count_;
        }
    }

    // Linearise the ring into draw-order scratch buffers (oldest → newest).
    static float xs[kPlotWindow], alt[kPlotWindow], gnd[kPlotWindow], spd[kPlotWindow];
    const int start = plot_count_ < kPlotWindow ? 0 : plot_head_;
    for (int i = 0; i < plot_count_; ++i) {
        const int idx = (start + i) % kPlotWindow;
        xs[i]  = plot_time_[idx];
        alt[i] = u.alt_value(plot_alt_[idx]);
        gnd[i] = u.alt_value(plot_gnd_[idx]);
        spd[i] = u.speed_value(plot_speed_[idx]);
    }

    char alt_axis[24], spd_axis[16];
    snprintf(alt_axis, sizeof(alt_axis), "%s MSL", u.alt_suffix());
    snprintf(spd_axis, sizeof(spd_axis), "%s", u.speed_suffix());
    ImPlot::PushStyleColor(ImPlotCol_FrameBg,    { 0, 0, 0, 0 });
    ImPlot::PushStyleColor(ImPlotCol_PlotBg,     { 0.02f, 0.04f, 0.07f, 0.55f });
    ImPlot::PushStyleColor(ImPlotCol_PlotBorder, { 0.16f, 0.24f, 0.34f, 0.60f });
    ImPlot::PushStyleColor(ImPlotCol_LegendBg,   { 0.04f, 0.06f, 0.10f, 0.75f });
    if (ImPlot::BeginPlot("##alt", { -1, -1 },
                          ImPlotFlags_NoTitle | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect |
                          ImPlotFlags_NoMouseText))
    {
        ImPlot::SetupAxis(ImAxis_X1, "T+ (s)");
        ImPlot::SetupAxis(ImAxis_Y1, alt_axis);
        ImPlot::SetupAxis(ImAxis_Y2, spd_axis, ImPlotAxisFlags_Opposite | ImPlotAxisFlags_NoGridLines);
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
            auto [a0, a1] = padded(alo, ahi, u.alt_value(60.0f));
            auto [s0, s1] = padded(slo, shi, u.speed_value(5.0f));
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
    const float top = top_bar_height() + S(kPad);
    const float h = std::max(S(160.0f), static_cast<float>(GetScreenHeight()) - top - bottom_offset - S(kPad));
    ImGui::SetNextWindowPos({ S(kPad), top });
    ImGui::SetNextWindowSize({ S(kLeftW), h });
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
    const UnitPrefs& u = state_.units;
    int n = 0;
    world.query<const EntityMeta>().each([&](const EntityMeta&) { ++n; });

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kDim, "%d track%s", n, n == 1 ? "" : "s");
    const float clear_w = ImGui::CalcTextSize(ICON_FA_TRASH " Clear").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - clear_w);
    ImGui::PushStyleColor(ImGuiCol_Text, kBad);   // quiet, but clearly destructive
    const bool clear = ImGui::Button(ICON_FA_TRASH " Clear");
    ImGui::PopStyleColor();
    if (clear && cbs_.on_clear_entities) cbs_.on_clear_entities();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove all tracks and clear the telemetry buffer");

    const float list_h = ImGui::GetContentRegionAvail().y - (state_.selected_entity.is_valid() ? S(40.0f) : 0.0f);
    if (ImGui::BeginTable("##tracks", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_BordersInnerH,
                          { 0, std::max(S(80.0f), list_h) }))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed, S(20.0f));
        ImGui::TableSetupColumn("Callsign", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(u.alt_suffix(),   ImGuiTableColumnFlags_WidthFixed, S(52.0f));
        ImGui::TableSetupColumn(u.speed_suffix(), ImGuiTableColumnFlags_WidthFixed, S(42.0f));
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

                push_mono();
                ImGui::TableNextColumn();
                if (meta.active) ImGui::TextColored(kDim, "%5.0f", u.alt_value(pos.v.y));
                else             ImGui::TextDisabled("  ---");
                ImGui::TableNextColumn();
                const float v = std::sqrt(vel.v.x * vel.v.x + vel.v.y * vel.v.y + vel.v.z * vel.v.z);
                if (meta.active) ImGui::TextColored(kDim, "%4.0f", u.speed_value(v));
                pop_font();
                ImGui::PopID();
            });
        ImGui::EndTable();
    }
    if (n == 0) ImGui::TextDisabled("Waiting for UDP on :%u...", state_.listen_port);

    if (state_.selected_entity.is_valid()) {
        const float bw = (ImGui::GetContentRegionAvail().x - S(8.0f)) / 3.0f;
        auto mode_btn = [&](const char* label, int mode) {
            const bool on = state_.camera_mode == mode;
            if (on ? tinted_button(label, kAccent, { bw, 0 }) : ImGui::Button(label, { bw, 0 }))
                state_.camera_mode = mode;
        };
        mode_btn(ICON_FA_EYE " Free", 0);           ImGui::SameLine(0, S(4));
        mode_btn(ICON_FA_CROSSHAIRS " Focus", 1);   ImGui::SameLine(0, S(4));
        mode_btn(ICON_FA_VIDEO " Chase", 2);
    }
}

// Quick controls; the full set lives in Settings.
void AfterActionUI::draw_view_tab() {
    section_header("CAMERA");
    const char* cam_modes[] = { "Free orbit", "Focus track", "Chase track" };
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##CamMode", &state_.camera_mode, cam_modes, IM_ARRAYSIZE(cam_modes));
    if (state_.camera_mode > 0 && !state_.selected_entity.is_valid())
        ImGui::TextColored(kWarn, ICON_FA_CIRCLE_EXCLAMATION "  Select a track first");
    const float half = (ImGui::GetContentRegionAvail().x - S(6.0f)) * 0.5f;
    if (ImGui::Button(ICON_FA_HOUSE " Home", { half, 0 }) && cbs_.on_go_home) cbs_.on_go_home();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fly to the home position (H)");
    ImGui::SameLine(0, S(6));
    if (ImGui::Button(ICON_FA_LOCATION_DOT " Set home", { half, 0 }) && cbs_.on_set_home_here) cbs_.on_set_home_here();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Make the current view centre the home position");

    section_header("SCENE");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##Scale", &state_.entity_3d_scale, 0.25f, 4.0f, "Track size  %.2fx");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##AltExag", &state_.altitude_exaggerate, 1.0f, 5.0f, "Vertical scale  %.1fx");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stretches terrain and tracks together");

    section_header("TERRAIN");
    const char* terrain_modes[] = { "Off", "Tactical grid", "Shaded", "Shaded + grid", "Contours" };
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##TerrainMode", &state_.terrain_mode, terrain_modes, IM_ARRAYSIZE(terrain_modes));

    section_header("OVERLAYS");
    const float col2 = S(kLeftW) * 0.5f;
    ImGui::Checkbox("Labels", &state_.show_labels);
    ImGui::SameLine(col2);
    ImGui::Checkbox("Trails", &state_.show_trails);
    ImGui::Checkbox("Ribbons", &state_.ribbon_trails);
    ImGui::SameLine(col2);
    ImGui::Checkbox("Velocity", &state_.show_velocity_vec);
    ImGui::Checkbox("Minimap", &state_.show_minimap);
    ImGui::SameLine(col2);
    ImGui::Checkbox("Network", &state_.show_stats);

    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_GEAR "  All settings...", { -1, 0 })) state_.show_settings_window = true;
}

// ── Right column: inspector, minimap, network ─────────────────────────────────
void AfterActionUI::draw_right_column(const flecs::world& world, const net::ReceiverStats& stats,
                                      float bottom_offset)
{
    const float x = static_cast<float>(GetScreenWidth()) - S(kRightW) - S(kPad);
    float y = top_bar_height() + S(kPad);
    const float limit = static_cast<float>(GetScreenHeight()) - bottom_offset - S(kPad);

    if (state_.selected_entity.is_valid()) {
        ImGui::SetNextWindowPos({ x, y });
        ImGui::SetNextWindowSize({ S(kRightW), 0 });
        ImGui::Begin("##Inspector", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        draw_inspector(world);
        y += ImGui::GetWindowHeight() + S(kPad);
        ImGui::End();
    }
    if (state_.show_minimap && state_.scene.minimap.id != 0 && y + S(200.0f) < limit) {
        const float size = std::min(S(kRightW), limit - y);
        ImGui::SetNextWindowPos({ x, y });
        ImGui::SetNextWindowSize({ S(kRightW), size });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(4), S(4)));
        ImGui::Begin("##Minimap", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
        draw_minimap(world);
        ImGui::End();
        ImGui::PopStyleVar();
        y += size + S(kPad);
    }
    if (state_.show_stats && y + S(130.0f) < limit) {
        ImGui::SetNextWindowPos({ x, y });
        ImGui::SetNextWindowSize({ S(kRightW), 0 });
        ImGui::Begin(ICON_FA_SATELLITE_DISH "  Network", &state_.show_stats,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings);
        draw_network(stats);
        ImGui::End();
    }
}

void AfterActionUI::draw_inspector(const flecs::world& /*world*/) {
    const UnitPrefs& u = state_.units;
    auto e = state_.selected_entity;
    const auto& meta = e.get<EntityMeta>();
    const auto& pos  = e.get<Position>();
    const auto& vel  = e.get<Velocity>();
    const Quaternion q = e.has<Rotation>() ? e.get<Rotation>().q : Quaternion{ 0, 0, 0, 1 };

    // Header: icon + callsign, type and id, close button.
    const ImVec4 col = entity_type_color(meta.type);
    push_heading();
    ImGui::TextColored(col, "%s", entity_type_icon(meta.type));
    ImGui::SameLine();
    if (meta.callsign[0]) ImGui::Text("%.*s", net::kCallsignLen - 1, meta.callsign);
    else                  ImGui::Text("#%u", meta.entity_id);
    pop_font();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kDim, "%s  %u/%u", entity_type_name(meta.type), meta.source_id, meta.entity_id);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - S(18.0f));
    if (ImGui::SmallButton(ICON_FA_XMARK "##close")) { state_.selected_entity = {}; return; }
    if (!meta.active) ImGui::TextColored(kBad, ICON_FA_SKULL "  DESTROYED / NOT ACTIVE");

    double lat = 0.0, lon = 0.0;
    if (cbs_.to_geo) cbs_.to_geo(pos.v.x, pos.v.z, &lat, &lon);
    const float gnd = cbs_.ground_height ? cbs_.ground_height(pos.v.x, pos.v.z) : 0.0f;
    const float agl = pos.v.y - gnd;
    const float spd = std::sqrt(vel.v.x * vel.v.x + vel.v.y * vel.v.y + vel.v.z * vel.v.z);
    float hdg, pitch, roll;
    euler_from_quat(q, &hdg, &pitch, &roll);

    char la[32], lo[32];
    u.format_coord(lat, true, la, sizeof(la));
    u.format_coord(lon, false, lo, sizeof(lo));

    if (ImGui::BeginTable("##insp", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, S(74.0f));
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        auto row = [&](const char* k, ImVec4 c, const char* fmt, auto... args) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kDim, "%s", k);
            ImGui::TableNextColumn();
            push_mono();
            ImGui::TextColored(c, fmt, args...);
            pop_font();
        };
        const ImVec4 white = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        const char* a1 = u.alt_suffix();
        const char* a2 = u.alt_suffix_alt();
        row("Position", white, "%s", la);
        row("",         white, "%s", lo);
        row("Altitude", white, "%6.0f %-2s %6.0f %s", u.alt_value(pos.v.y), a1, u.alt_value_alt(pos.v.y), a2);
        row("AGL",      agl < 0 ? kBad : (agl < 150 ? kWarn : white), "%6.0f %-2s %6.0f %s",
            u.alt_value(agl), a1, u.alt_value_alt(agl), a2);
        row("Ground",   kDim,  "%6.0f %-2s %6.0f %s", u.alt_value(gnd), a1, u.alt_value_alt(gnd), a2);
        row("Speed",    white, "%6.0f %s", u.speed_value(spd), u.speed_suffix());
        if (u.alt == AltUnit::Feet) row("Vert spd", white, "%+6.0f fpm", vel.v.y * 196.85f);
        else                        row("Vert spd", white, "%+6.1f m/s", vel.v.y);
        row("Heading",  white, "%05.1f\xc2\xb0", hdg);
        row("Pitch",    white, "%+5.1f\xc2\xb0", pitch);
        row("Roll",     white, "%+5.1f\xc2\xb0", roll);
        ImGui::EndTable();
    }
}

void AfterActionUI::draw_minimap(const flecs::world& world) {
    const SceneInfo& sc = state_.scene;
    const UnitPrefs& u = state_.units;
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
        const float L = S(26.0f), spread = 0.45f;
        auto dir = [&](float ang) { return ImVec2{ c.x + std::sin(ang) * L, c.y - std::cos(ang) * L }; };
        dl->AddTriangleFilled(c, dir(a - spread), dir(a + spread), IM_COL32(255, 255, 255, 34));
        dl->AddCircleFilled(c, S(3.0f), IM_COL32(255, 255, 255, 200));
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
                const float L = S(sel ? 14.0f : 10.0f);
                dl->AddLine(c, { c.x + v.v.x / sp * L, c.y + v.v.z / sp * L }, col, 1.5f);
            }
            const float r = S(sel ? 4.5f : 3.5f);
            if (sel) dl->AddCircle(c, S(8.0f), IM_COL32(255, 220, 0, 160), 0, 1.5f);
            dl->AddCircleFilled(c, r, col);
            dl->AddCircle(c, r, IM_COL32(0, 0, 0, 160));
        });
    dl->PopClipRect();

    // North arrow and scale bar.
    dl->AddText({ p0.x + S(8), p0.y + S(6) }, IM_COL32(230, 236, 245, 230), "N " ICON_FA_ARROW_UP);
    float nice = 1.0f;
    for (float c : { 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f,
                     50000.0f, 100000.0f, 200000.0f })
        if (c <= sc.map_range * 0.3f) nice = c;
    const float bar = nice / sc.map_range * sz;
    const ImVec2 b0{ p0.x + S(10), p1.y - S(12) };
    dl->AddLine(b0, { b0.x + bar, b0.y }, IM_COL32(240, 240, 240, 230), 2.0f);
    dl->AddLine({ b0.x, b0.y - S(4) }, { b0.x, b0.y + 1 }, IM_COL32(240, 240, 240, 230), 2.0f);
    dl->AddLine({ b0.x + bar, b0.y - S(4) }, { b0.x + bar, b0.y + 1 }, IM_COL32(240, 240, 240, 230), 2.0f);
    char lbl[24];
    if (nice >= 1000.0f) snprintf(lbl, sizeof(lbl), "%.0f km", nice / 1000.0f);
    else                 snprintf(lbl, sizeof(lbl), "%.0f m", nice);
    dl->AddText({ b0.x + bar + S(6), b0.y - S(9) }, IM_COL32(240, 240, 240, 230), lbl);

    ImGui::InvisibleButton("##map", { sz, sz });
    if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetMousePos();
        const float wx = sc.map_cx - half + (m.x - p0.x) / sz * sc.map_range;
        const float wz = sc.map_cz - half + (m.y - p0.y) / sz * sc.map_range;
        double lat = 0, lon = 0;
        if (cbs_.to_geo) cbs_.to_geo(wx, wz, &lat, &lon);
        const float h = cbs_.ground_height ? cbs_.ground_height(wx, wz) : 0.0f;
        char la[32], lo[32];
        u.format_coord(lat, true, la, sizeof(la));
        u.format_coord(lon, false, lo, sizeof(lo));
        ImGui::SetTooltip("%s  %s\n%.0f %s MSL\nclick: look here", la, lo, u.alt_value(h), u.alt_suffix());
        if (ImGui::IsItemClicked() && cbs_.on_look_at) cbs_.on_look_at(wx, wz);
    }
}

void AfterActionUI::draw_network(const net::ReceiverStats& stats) {
    ImGui::TextColored(state_.scene.net_ok ? kGood : kBad, "%s", state_.scene.net_status.c_str());
    push_mono();
    ImGui::Text("Packets    %llu", (unsigned long long)stats.packets_received);
    ImGui::Text("Received   %.1f KB", stats.bytes_received / 1024.0);
    ImGui::Text("Dropped    %llu", (unsigned long long)stats.packets_dropped);
    ImGui::Text("Parse err  %llu", (unsigned long long)stats.parse_errors);
    ImGui::Text("Seq gaps   %llu", (unsigned long long)stats.sequence_gaps);
    pop_font();
}

// ── Settings Window ───────────────────────────────────────────────────────────
//  A category list on the left, the selected page on the right. Everything is
//  applied immediately and saved automatically (see Application autosave).
void AfterActionUI::draw_settings_window() {
    if (!state_.show_settings_window) return;

    struct Page { const char* icon; const char* name; void (AfterActionUI::*fn)(); };
    static const Page pages[] = {
        { ICON_FA_SLIDERS,         "General",   &AfterActionUI::settings_general },
        { ICON_FA_EYE,             "Display",   &AfterActionUI::settings_display },
        { ICON_FA_CAMERA,          "Camera",    &AfterActionUI::settings_camera },
        { ICON_FA_MOUNTAIN_SUN,    "Terrain",   &AfterActionUI::settings_terrain },
        { ICON_FA_TOWER_BROADCAST, "Network",   &AfterActionUI::settings_network },
        { ICON_FA_FLOPPY_DISK,     "Recording", &AfterActionUI::settings_recording },
        { ICON_FA_CUBE,            "Models",    &AfterActionUI::settings_models },
        { ICON_FA_KEYBOARD,        "Shortcuts", &AfterActionUI::settings_shortcuts },
    };
    constexpr int kPages = static_cast<int>(sizeof(pages) / sizeof(pages[0]));
    settings_page_ = std::clamp(settings_page_, 0, kPages - 1);

    // Re-centre and re-size whenever it opens or the interface scale changed.
    const ImGuiCond cond = settings_scale_ != applied_scale_ ? ImGuiCond_Always : ImGuiCond_Appearing;
    settings_scale_ = applied_scale_;
    const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
    ImGui::SetNextWindowPos({ sw * 0.5f, sh * 0.5f }, cond, { 0.5f, 0.5f });
    ImGui::SetNextWindowSize({ std::min(S(680.0f), sw - S(40.0f)), std::min(S(540.0f), sh - S(80.0f)) }, cond);
    ImGui::SetNextWindowSizeConstraints({ std::min(S(520.0f), sw), std::min(S(360.0f), sh) }, { FLT_MAX, FLT_MAX });
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.068f, 0.10f, 0.98f));
    const bool open = ImGui::Begin(ICON_FA_GEAR "  Settings###Settings", &state_.show_settings_window,
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleColor();
    if (open) {
        const float footer = ImGui::GetFrameHeightWithSpacing() + S(6.0f);

        // Category list
        ImGui::BeginChild("##cats", { S(150.0f), -footer }, ImGuiChildFlags_Borders);
        for (int i = 0; i < kPages; ++i) {
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "%s   %s", pages[i].icon, pages[i].name);
            if (ImGui::Selectable(lbl, settings_page_ == i, 0, { 0, ImGui::GetFrameHeight() }))
                settings_page_ = i;
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // Page
        ImGui::BeginChild("##page", { 0, -footer }, ImGuiChildFlags_None);
        push_heading();
        ImGui::TextUnformatted(pages[settings_page_].name);
        pop_font();
        ImGui::Spacing();
        (this->*pages[settings_page_].fn)();
        ImGui::EndChild();

        // Footer: autosave status
        ImGui::Separator();
        const SceneInfo& sc = state_.scene;
        const bool fresh = sc.last_saved_time >= 0.0 && GetTime() - sc.last_saved_time < 2.0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(fresh ? kGood : kDim, fresh ? ICON_FA_CHECK "  Saved" : "Changes are saved automatically");
        if (ImGui::IsItemHovered() && !sc.config_path.empty()) ImGui::SetTooltip("%s", sc.config_path.c_str());
        ImGui::SameLine(ImGui::GetContentRegionMax().x - S(80.0f));
        if (ImGui::Button("Close", { S(80.0f), 0 })) state_.show_settings_window = false;
    }
    ImGui::End();
}

void AfterActionUI::settings_general() {
    const UIState def{};
    section_header("INTERFACE");
    if (begin_settings_table("##gen")) {
        row_label("Interface scale", "Size of text and panels (applies when you release the slider)");
        int pct = static_cast<int>(std::lround(state_.ui_scale * 100.0f));
        if (ImGui::SliderInt("##uiscale", &pct, 75, 200, "%d %%")) state_.ui_scale = pct / 100.0f;
        ImGui::EndTable();
    }
    {
        const float bw = (ImGui::GetContentRegionAvail().x - S(12.0f)) / 4.0f;
        for (float s : { 1.0f, 1.25f, 1.5f, 2.0f }) {
            char b[16];
            snprintf(b, sizeof(b), "%d %%", static_cast<int>(s * 100));
            if (ImGui::Button(b, { bw, 0 })) state_.ui_scale = s;
            if (s != 2.0f) ImGui::SameLine(0, S(4));
        }
    }

    section_header("UNITS");
    if (begin_settings_table("##units")) {
        int alt = static_cast<int>(state_.units.alt);
        row_label("Altitude");
        if (segmented("alt", &alt, { "Feet", "Metres" })) state_.units.alt = static_cast<AltUnit>(alt);
        int spd = static_cast<int>(state_.units.speed);
        row_label("Speed");
        if (segmented("spd", &spd, { "Knots", "km/h", "m/s" })) state_.units.speed = static_cast<SpeedUnit>(spd);
        int crd = static_cast<int>(state_.units.coord);
        row_label("Coordinates", "Degrees-minutes-seconds, degrees-decimal minutes, or decimal degrees");
        if (segmented("crd", &crd, { "DMS", "DDM", "Decimal" })) state_.units.coord = static_cast<CoordFmt>(crd);
        ImGui::EndTable();
    }
    char ex[64], ey[64];
    state_.units.format_coord(40.0789, true, ex, sizeof(ex));
    state_.units.format_coord(32.5656, false, ey, sizeof(ey));
    ImGui::TextColored(kDim, "Example: %s  %s   %.0f %s   %.0f %s", ex, ey,
                       state_.units.alt_value(3000.0f), state_.units.alt_suffix(),
                       state_.units.speed_value(250.0f), state_.units.speed_suffix());

    ImGui::Spacing();
    if (ImGui::Button("Reset General")) { state_.ui_scale = def.ui_scale; state_.units = def.units; }
}

void AfterActionUI::settings_display() {
    const UIState def{};
    section_header("TRACKS");
    if (begin_settings_table("##disp")) {
        row_label("Track size", "Models are true size up close and grow with distance by this factor");
        ImGui::SliderFloat("##size", &state_.entity_3d_scale, 0.25f, 4.0f, "%.2fx");
        row_label("Vertical scale", "Stretches terrain and tracks together (1 = real)");
        ImGui::SliderFloat("##vscale", &state_.altitude_exaggerate, 1.0f, 5.0f, "%.1fx");
        row_label("Labels");
        ImGui::Checkbox("##labels", &state_.show_labels);
        row_label("Velocity vectors");
        ImGui::Checkbox("##vel", &state_.show_velocity_vec);
        ImGui::EndTable();
    }
    section_header("TRAILS");
    if (begin_settings_table("##trails")) {
        row_label("Show trails");
        ImGui::Checkbox("##trails", &state_.show_trails);
        row_label("Style");
        int ribbon = state_.ribbon_trails ? 1 : 0;
        if (segmented("style", &ribbon, { "Line", "Ribbon" })) state_.ribbon_trails = ribbon == 1;
        row_label("Ribbon width");
        ImGui::BeginDisabled(!state_.ribbon_trails);
        ImGui::SliderFloat("##tw", &state_.trail_width_override, 5.0f, 500.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    section_header("PANELS");
    if (begin_settings_table("##panels")) {
        row_label("Minimap");
        ImGui::Checkbox("##mm", &state_.show_minimap);
        row_label("Network panel");
        ImGui::Checkbox("##np", &state_.show_stats);
        row_label("Chart height");
        ImGui::SliderFloat("##ch", &state_.timeline_height, 140.0f, 500.0f, "%.0f px");
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::Button("Reset Display")) {
        state_.entity_3d_scale = def.entity_3d_scale; state_.altitude_exaggerate = def.altitude_exaggerate;
        state_.show_labels = def.show_labels; state_.show_velocity_vec = def.show_velocity_vec;
        state_.show_trails = def.show_trails; state_.ribbon_trails = def.ribbon_trails;
        state_.trail_width_override = def.trail_width_override;
        state_.show_minimap = def.show_minimap; state_.show_stats = def.show_stats;
        state_.timeline_height = def.timeline_height;
    }
}

void AfterActionUI::settings_camera() {
    const UIState def{};
    section_header("LENS");
    if (begin_settings_table("##lens")) {
        row_label("Field of view", "Vertical field of view");
        ImGui::SliderFloat("##fov", &state_.camera_fov, 25.0f, 100.0f, "%.0f\xc2\xb0");
        ImGui::EndTable();
    }
    section_header("MOUSE");
    if (begin_settings_table("##mouse")) {
        row_label("Rotate direction", "Grab: dragging moves the scene under the cursor");
        int inv = state_.invert_look ? 0 : 1;
        if (segmented("inv", &inv, { "Grab scene", "Move camera" })) state_.invert_look = inv == 0;
        row_label("Sensitivity");
        ImGui::SliderFloat("##sens", &state_.mouse_sensitivity, 0.2f, 3.0f, "%.2fx");
        ImGui::EndTable();
    }
    hint("Right-drag orbits, middle-drag or WASD pans, the wheel zooms. "
         "F focuses and G chases the selected track.");
    ImGui::Spacing();
    if (ImGui::Button("Reset Camera")) {
        state_.camera_fov = def.camera_fov;
        state_.invert_look = def.invert_look;
        state_.mouse_sensitivity = def.mouse_sensitivity;
    }
}

void AfterActionUI::settings_terrain() {
    const UIState def{};
    const SceneInfo& sc = state_.scene;
    section_header("RENDERING");
    if (begin_settings_table("##ter")) {
        row_label("Look");
        const char* looks[] = { "Off", "Tactical grid", "Shaded", "Shaded + grid", "Contours" };
        ImGui::Combo("##look", &state_.terrain_mode, looks, IM_ARRAYSIZE(looks));
        row_label("Detail", "Higher = finer terrain further away (more GPU work)");
        ImGui::SliderFloat("##detail", &state_.terrain_detail, 0.8f, 4.0f, "%.1f");
        row_label("Max view distance");
        float km = state_.far_clip_plane / 1000.0f;
        if (ImGui::SliderFloat("##far", &km, 20.0f, 3000.0f, "%.0f km", ImGuiSliderFlags_Logarithmic))
            state_.far_clip_plane = km * 1000.0f;
        ImGui::EndTable();
    }

    section_header("HOME POSITION");
    hint("Where the scene starts before any data arrives. Live data more than "
         "250 km away moves the scene to it.");
    if (begin_settings_table("##home")) {
        row_label("Latitude");
        ImGui::InputDouble("##hlat", &state_.origin_lat, 0, 0, "%.5f");
        row_label("Longitude");
        ImGui::InputDouble("##hlon", &state_.origin_lon, 0, 0, "%.5f");
        ImGui::EndTable();
    }
    state_.origin_lat = std::clamp(state_.origin_lat, -89.0, 89.0);
    state_.origin_lon = std::clamp(state_.origin_lon, -180.0, 180.0);
    struct Preset { const char* name; double lat, lon; };
    static const Preset presets[] = {
        { "Ak\xc4\xb1nc\xc4\xb1", 40.0789, 32.5656 }, { "Esenbo\xc4\x9f" "a", 40.1281, 32.9951 },
        { "\xc4\xb0ncirlik", 37.0021, 35.4259 },      { "\xc3\x87i\xc4\x9fli", 38.5130, 27.0101 },
    };
    for (const auto& p : presets) {
        if (ImGui::Button(p.name)) { state_.origin_lat = p.lat; state_.origin_lon = p.lon; }
        ImGui::SameLine(0, S(4));
    }
    if (tinted_button(ICON_FA_HOUSE "  Go home", kAccent) && cbs_.on_go_home) cbs_.on_go_home();

    section_header("DATA");
    ImGui::TextWrapped("%s", sc.terrain_status.c_str());
    push_mono();
    for (const auto& l : sc.terrain_layers) ImGui::TextColored(kDim, "%s", l.c_str());
    ImGui::TextColored(kDim, "scene origin %.5f, %.5f", sc.origin_lat, sc.origin_lon);
    ImGui::TextColored(kDim, "chunks %d drawn  %d cached  %d queued  %.0f MB",
                       sc.chunks_drawn, sc.chunks_cached, sc.chunks_queued, sc.terrain_gpu_mb);
    pop_font();
    ImGui::Spacing();
    if (ImGui::Button("Reset Terrain")) {
        state_.terrain_mode = def.terrain_mode; state_.terrain_detail = def.terrain_detail;
        state_.far_clip_plane = def.far_clip_plane;
    }
}

void AfterActionUI::settings_network() {
    const SceneInfo& sc = state_.scene;
    section_header("UDP LISTENER");
    if (begin_settings_table("##net")) {
        row_label("Bind address", "0.0.0.0 listens on all network interfaces");
        ImGui::InputText("##bind", state_.bind_addr, sizeof(state_.bind_addr));
        row_label("Port");
        int port = static_cast<int>(state_.listen_port);
        if (ImGui::InputInt("##port", &port)) state_.listen_port = static_cast<uint16_t>(std::clamp(port, 1, 65535));
        row_label("Status");
        ImGui::TextColored(sc.net_ok ? kGood : kBad, "%s %s", sc.net_ok ? ICON_FA_CIRCLE_CHECK : ICON_FA_CIRCLE_XMARK,
                           sc.net_status.c_str());
        ImGui::EndTable();
    }
    const bool pending = !sc.demo && (sc.net_active_port != state_.listen_port ||
                                      sc.net_active_addr != state_.bind_addr);
    if (pending) ImGui::TextColored(kWarn, ICON_FA_TRIANGLE_EXCLAMATION "  Not applied yet: press Apply to restart the listener.");
    if (tinted_button(ICON_FA_ROTATE "  Apply / restart listener", kGood) && cbs_.on_apply_network)
        cbs_.on_apply_network(state_.bind_addr, state_.listen_port);
    ImGui::SameLine();
    if (ImGui::Button("Defaults")) {
        snprintf(state_.bind_addr, sizeof(state_.bind_addr), "0.0.0.0");
        state_.listen_port = 22522;
    }
    ImGui::Spacing();
    hint("Packet format: docs/PROTOCOL.md. A command-line --port / --addr overrides "
         "these values for that run. Test sender: scripts/test_sender.py.");
}

void AfterActionUI::settings_recording() {
    const SceneInfo& sc = state_.scene;
    section_header("DASHCAM");
    if (begin_settings_table("##rec")) {
        row_label("Save last", "Length of the \"Last N s\" button in the top bar");
        ImGui::SliderFloat("##dash", &state_.dashcam_secs, 10.0f, 900.0f, "%.0f s", ImGuiSliderFlags_Logarithmic);
        row_label("Folder");
        ImGui::TextColored(kDim, "%s", sc.recordings_dir.c_str());
        ImGui::EndTable();
    }
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Show recordings folder") && cbs_.on_open_folder)
        cbs_.on_open_folder(sc.recordings_dir);

    section_header("OPEN");
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Browse...") && cbs_.on_browse_file) cbs_.on_browse_file();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##load_path", "...or paste a path to a .aar / .csv file",
                             state_.load_path, sizeof(state_.load_path));
    ImGui::BeginDisabled(state_.load_path[0] == '\0');
    if (ImGui::Button(ICON_FA_FILE "  Open .aar") && cbs_.on_load_file) cbs_.on_load_file(state_.load_path);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FILE_IMPORT "  Import CSV") && cbs_.on_load_csv) cbs_.on_load_csv(state_.load_path);
    ImGui::EndDisabled();

    section_header("TRACKS");
    hint("Remove every track and clear the telemetry buffer.");
    if (tinted_button(ICON_FA_TRASH "  Clear all tracks", kBad) && cbs_.on_clear_entities) cbs_.on_clear_entities();
}

void AfterActionUI::settings_models() {
    hint("Bind 3D models (FBX / OBJ / glTF) to entity types. Put files in the app's "
         "assets/models/ folder. Load applies live; Save writes assets/models.yaml. "
         "Preview scale and rotation with the 'hangar' tool.");
    ImGui::Spacing();

    const char* kTypeNames[] = { "Unknown", "Jet", "Missile", "AAA", "Ground", "Helo", "Ship", "UAV" };
    int remove_idx = -1;
    for (int i = 0; i < (int)state_.model_bindings.size(); ++i) {
        auto& b = state_.model_bindings[i];
        ImGui::PushID(i);
        const int tclamp = (b.type >= 0 && b.type < IM_ARRAYSIZE(kTypeNames)) ? b.type : 0;
        char hdr[320];
        snprintf(hdr, sizeof(hdr), "%s  %s   %s###mb%d", entity_type_icon(static_cast<uint16_t>(tclamp)),
                 kTypeNames[tclamp], b.file[0] ? b.file : "<no file>", i);
        if (ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen)) {
            if (begin_settings_table("##mb")) {
                row_label("Entity type");
                ImGui::Combo("##type", &b.type, kTypeNames, IM_ARRAYSIZE(kTypeNames));
                row_label("File");
                ImGui::InputTextWithHint("##file", "models/yourmodel.glb", b.file, sizeof(b.file));
                row_label("Scale");
                ImGui::SliderFloat("##scale", &b.scale, 0.001f, 50.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
                row_label("Tint");
                ImGui::ColorEdit3("##tint", b.tint);
                row_label("Yaw / pitch / roll");
                ImGui::SliderFloat3("##rot", &b.yaw, -180, 180, "%.0f\xc2\xb0");
                ImGui::EndTable();
            }
            ImGui::BeginDisabled(b.file[0] == '\0');
            if (tinted_button("Load / apply", kGood) && cbs_.on_model_load) {
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
            if (ImGui::Button("Use built-in shape") && cbs_.on_model_clear) {
                cbs_.on_model_clear((uint16_t)b.type);
                snprintf(b.status, sizeof(b.status), "reverted");
            }
            ImGui::SameLine();
            if (tinted_button(ICON_FA_TRASH "##remove", kBad)) remove_idx = i;
            if (b.status[0]) { ImGui::SameLine(); ImGui::TextDisabled("%s", b.status); }
        }
        ImGui::PopID();
    }
    if (remove_idx >= 0) {
        const uint16_t t = (uint16_t)state_.model_bindings[remove_idx].type;
        if (cbs_.on_model_clear) cbs_.on_model_clear(t);
        state_.model_bindings.erase(state_.model_bindings.begin() + remove_idx);
    }
    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_PLUS "  Add binding")) state_.model_bindings.push_back(UIState::ModelBinding{});
    ImGui::SameLine();
    if (tinted_button(ICON_FA_FLOPPY_DISK "  Save manifest", kAccent) && cbs_.on_models_save) {
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
}

void AfterActionUI::settings_shortcuts() {
    const char* rows[][2] = {
        { "Left click",        "Select a track (in 3D or the list)" },
        { "Double-click (list)", "Focus the track" },
        { "Right drag",        "Orbit the camera" },
        { "Middle drag / WASD", "Pan (hold Shift for faster)" },
        { "Mouse wheel",       "Zoom" },
        { "F",                 "Focus on / release the selected track" },
        { "G",                 "Chase camera on the selected track" },
        { "H",                 "Fly to the home position" },
        { "Space",             "Play / pause" },
        { "1  2  3  4",        "Speed 0.25x, 1x, 4x, -1x" },
        { "R",                 "Line / ribbon trails" },
        { "Esc",               "Close settings, then clear the selection" },
        { "Ctrl+Q",            "Quit" },
    };
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, S(170.0f));
        ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch);
        for (auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            push_mono();
            ImGui::TextColored(kAccent, "%s", r[0]);
            pop_font();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[1]);
        }
        ImGui::EndTable();
    }
}

} // namespace afteraction
