#include "Config.hpp"

// yaml-cpp is third-party but gets pulled into this translation unit, which is
// compiled with our strict warnings-as-errors (/W4 /WX on MSVC). Its headers
// (e.g. node/ptr.h) trip those warnings and break the build. Silence diagnostics
// originating from the yaml-cpp headers only — our own code below stays strict.
#if defined(_MSC_VER)
#  pragma warning(push, 0)
#elif defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wall"
#  pragma GCC diagnostic ignored "-Wextra"
#endif
#include <yaml-cpp/yaml.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#elif defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace afteraction {

namespace {

// Read `node[a][b]` (or legacy flat `node[legacy]`) into `out` if present.
// A value that doesn't parse (e.g. "1,25") keeps its default and only that
// key is skipped — not every key after it.
template <typename T>
void read(const YAML::Node& root, const char* section, const char* key, T& out,
          const char* legacy = nullptr) {
    try {
        if (root[section] && root[section][key]) { out = root[section][key].as<T>(); return; }
        if (legacy && root[legacy]) out = root[legacy].as<T>();
    } catch (const YAML::Exception& e) {
        std::cerr << "Config: ignoring " << section << "." << key << ": " << e.what() << "\n";
    }
}

void clamp_to(float& v, float lo, float hi, float def) {
    v = std::isfinite(v) ? std::clamp(v, lo, hi) : def;
}

const char* alt_name(AltUnit u)     { return u == AltUnit::Metres ? "m" : "ft"; }
const char* speed_name(SpeedUnit u) { return u == SpeedUnit::KmH ? "kmh" : u == SpeedUnit::MetresPerSec ? "mps" : "kt"; }
const char* coord_name(CoordFmt c)  { return c == CoordFmt::DDM ? "ddm" : c == CoordFmt::Decimal ? "decimal" : "dms"; }

} // namespace

void ConfigManager::load_config(UIState& state, const std::string& path) {
    try {
        const YAML::Node c = YAML::LoadFile(path);

        read(c, "ui", "scale", state.ui_scale);
        if (c["ui"] && c["ui"]["units"]) {
            const YAML::Node u = c["ui"]["units"];
            if (u["altitude"])    state.units.alt   = u["altitude"].as<std::string>() == "m" ? AltUnit::Metres : AltUnit::Feet;
            if (u["speed"]) {
                const std::string v = u["speed"].as<std::string>();
                state.units.speed = v == "kmh" ? SpeedUnit::KmH : v == "mps" ? SpeedUnit::MetresPerSec : SpeedUnit::Knots;
            }
            if (u["coordinates"]) {
                const std::string v = u["coordinates"].as<std::string>();
                state.units.coord = v == "ddm" ? CoordFmt::DDM : v == "decimal" ? CoordFmt::Decimal : CoordFmt::DMS;
            }
        }

        std::string bind;
        read(c, "network", "bind", bind);
        if (!bind.empty()) snprintf(state.bind_addr, sizeof(state.bind_addr), "%s", bind.c_str());
        int port = state.listen_port;
        read(c, "network", "port", port);
        if (port >= 1 && port <= 65535) state.listen_port = static_cast<uint16_t>(port);

        read(c, "camera", "fov",               state.camera_fov);
        read(c, "camera", "invert_look",       state.invert_look,       "invert_look");
        read(c, "camera", "mouse_sensitivity", state.mouse_sensitivity, "mouse_sensitivity");
        read(c, "camera", "yaw",               state.camera_yaw,        "camera_yaw");
        read(c, "camera", "pitch",             state.camera_pitch,      "camera_pitch");
        read(c, "camera", "distance",          state.camera_distance,   "camera_distance");

        read(c, "display", "labels",         state.show_labels,          "show_labels");
        read(c, "display", "trails",         state.show_trails,          "show_trails");
        read(c, "display", "ribbons",        state.ribbon_trails,        "ribbon_trails");
        read(c, "display", "velocity",       state.show_velocity_vec,    "show_velocity_vec");
        read(c, "display", "minimap",        state.show_minimap,         "show_minimap");
        read(c, "display", "network_panel",  state.show_stats);
        read(c, "display", "track_size",     state.entity_3d_scale,      "entity_size");
        read(c, "display", "ribbon_width",   state.trail_width_override, "trail_width_override");
        read(c, "display", "chart_height",   state.timeline_height,      "timeline_height");
        read(c, "display", "vertical_scale", state.altitude_exaggerate,  "vertical_scale");

        read(c, "terrain", "look",     state.terrain_mode,   "terrain_mode");
        read(c, "terrain", "detail",   state.terrain_detail, "terrain_detail");
        read(c, "terrain", "home_lat", state.origin_lat,     "home_lat");
        read(c, "terrain", "home_lon", state.origin_lon,     "home_lon");
        float km = state.far_clip_plane / 1000.0f;
        read(c, "terrain", "max_view_km", km);
        state.far_clip_plane = km * 1000.0f;

        read(c, "recording", "dashcam_seconds", state.dashcam_secs);
    } catch (const YAML::Exception& e) {
        std::cerr << "Config load failed: " << e.what() << "\n";
    }
    clamp_settings(state);
}

void ConfigManager::clamp_settings(UIState& s) {
    static const UIState d{};
    clamp_to(s.ui_scale,             0.75f, 2.0f,      d.ui_scale);
    clamp_to(s.camera_fov,           20.0f, 110.0f,    d.camera_fov);
    clamp_to(s.mouse_sensitivity,    0.05f, 5.0f,      d.mouse_sensitivity);
    clamp_to(s.camera_yaw,           -1e6f, 1e6f,      d.camera_yaw);
    clamp_to(s.camera_pitch,         -89.0f, 89.0f,    d.camera_pitch);
    clamp_to(s.camera_distance,      30.0f, 900000.0f, d.camera_distance);
    clamp_to(s.entity_3d_scale,      0.1f, 10.0f,      d.entity_3d_scale);
    clamp_to(s.trail_width_override, 1.0f, 2000.0f,    d.trail_width_override);
    clamp_to(s.timeline_height,      100.0f, 1000.0f,  d.timeline_height);
    clamp_to(s.altitude_exaggerate,  1.0f, 10.0f,      d.altitude_exaggerate);
    clamp_to(s.terrain_detail,       0.5f, 6.0f,       d.terrain_detail);
    clamp_to(s.far_clip_plane,       5000.0f, 5e6f,    d.far_clip_plane);
    clamp_to(s.dashcam_secs,         5.0f, 900.0f,     d.dashcam_secs);
    s.terrain_mode = std::clamp(s.terrain_mode, 0, 4);
    if (!std::isfinite(s.origin_lat) || std::fabs(s.origin_lat) > 89.0) s.origin_lat = d.origin_lat;
    if (!std::isfinite(s.origin_lon) || std::fabs(s.origin_lon) > 180.0) s.origin_lon = d.origin_lon;
}

std::string ConfigManager::to_yaml(const UIState& state) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "ui" << YAML::Value << YAML::BeginMap
        << YAML::Key << "scale" << YAML::Value << state.ui_scale
        << YAML::Key << "units" << YAML::Value << YAML::BeginMap
            << YAML::Key << "altitude"    << YAML::Value << alt_name(state.units.alt)
            << YAML::Key << "speed"       << YAML::Value << speed_name(state.units.speed)
            << YAML::Key << "coordinates" << YAML::Value << coord_name(state.units.coord)
        << YAML::EndMap << YAML::EndMap;
    out << YAML::Key << "network" << YAML::Value << YAML::BeginMap
        << YAML::Key << "bind" << YAML::Value << std::string(state.bind_addr)
        << YAML::Key << "port" << YAML::Value << static_cast<int>(state.listen_port)
        << YAML::EndMap;
    out << YAML::Key << "camera" << YAML::Value << YAML::BeginMap
        << YAML::Key << "fov"               << YAML::Value << state.camera_fov
        << YAML::Key << "invert_look"       << YAML::Value << state.invert_look
        << YAML::Key << "mouse_sensitivity" << YAML::Value << state.mouse_sensitivity
        << YAML::Key << "yaw"               << YAML::Value << state.camera_yaw
        << YAML::Key << "pitch"             << YAML::Value << state.camera_pitch
        << YAML::Key << "distance"          << YAML::Value << state.camera_distance
        << YAML::EndMap;
    out << YAML::Key << "display" << YAML::Value << YAML::BeginMap
        << YAML::Key << "labels"         << YAML::Value << state.show_labels
        << YAML::Key << "trails"         << YAML::Value << state.show_trails
        << YAML::Key << "ribbons"        << YAML::Value << state.ribbon_trails
        << YAML::Key << "velocity"       << YAML::Value << state.show_velocity_vec
        << YAML::Key << "minimap"        << YAML::Value << state.show_minimap
        << YAML::Key << "network_panel"  << YAML::Value << state.show_stats
        << YAML::Key << "track_size"     << YAML::Value << state.entity_3d_scale
        << YAML::Key << "ribbon_width"   << YAML::Value << state.trail_width_override
        << YAML::Key << "chart_height"   << YAML::Value << state.timeline_height
        << YAML::Key << "vertical_scale" << YAML::Value << state.altitude_exaggerate
        << YAML::EndMap;
    out << YAML::Key << "terrain" << YAML::Value << YAML::BeginMap
        << YAML::Key << "look"        << YAML::Value << state.terrain_mode
        << YAML::Key << "detail"      << YAML::Value << state.terrain_detail
        << YAML::Key << "max_view_km" << YAML::Value << std::round(state.far_clip_plane / 1000.0f)
        << YAML::Key << "home_lat"    << YAML::Value << state.origin_lat
        << YAML::Key << "home_lon"    << YAML::Value << state.origin_lon
        << YAML::EndMap;
    out << YAML::Key << "recording" << YAML::Value << YAML::BeginMap
        << YAML::Key << "dashcam_seconds" << YAML::Value << state.dashcam_secs
        << YAML::EndMap;
    out << YAML::EndMap;
    return std::string("# AfterAction settings (written automatically)\n") + out.c_str() + "\n";
}

bool ConfigManager::save_text(const std::string& path, const std::string& text) {
    // Write to a temp file and rename, so a crash mid-write can't leave a
    // truncated config behind.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        f.close();                       // flush now: a full disk must fail here,
        if (!f) {                        // not after the real file was replaced
            std::error_code rm;
            std::filesystem::remove(tmp, rm);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {   // e.g. Windows refusing to replace: fall back to copy
        std::filesystem::copy_file(tmp, path, std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code rm;              // separate code: removing the temp must not hide a copy failure
        std::filesystem::remove(tmp, rm);
    }
    return !ec;
}

void ConfigManager::save_config(const UIState& state, const std::string& path) {
    save_text(path, to_yaml(state));
}

std::vector<ModelSpec> ConfigManager::load_model_manifest(const std::string& path) {
    std::vector<ModelSpec> out;
    try {
        YAML::Node root = YAML::LoadFile(path);
        const YAML::Node& models = root["models"];
        if (!models || !models.IsSequence()) return out;
        for (const auto& n : models) {
            ModelSpec s;
            if (n["type"])  s.type  = static_cast<uint16_t>(n["type"].as<int>());
            if (n["file"])  s.file  = n["file"].as<std::string>();
            if (n["scale"]) s.scale = n["scale"].as<float>();
            if (n["tint"] && n["tint"].IsSequence() && n["tint"].size() == 3)
                for (int i = 0; i < 3; ++i)
                    s.tint[i] = static_cast<uint8_t>(n["tint"][i].as<int>());
            if (n["base_rot"] && n["base_rot"].IsSequence() && n["base_rot"].size() == 3) {
                s.yaw   = n["base_rot"][0].as<float>();
                s.pitch = n["base_rot"][1].as<float>();
                s.roll  = n["base_rot"][2].as<float>();
            }
            if (!s.file.empty()) out.push_back(std::move(s));
        }
    } catch (const YAML::Exception&) {
        // Missing/invalid manifest is fine — procedural shapes are used.
    }
    return out;
}

bool ConfigManager::save_model_manifest(const std::string& path,
                                        const std::vector<ModelSpec>& models) {
    try {
        YAML::Node root;
        for (const auto& s : models) {
            YAML::Node n;
            n["type"]  = (int)s.type;
            n["file"]  = s.file;
            n["scale"] = s.scale;
            YAML::Node tint(YAML::NodeType::Sequence);
            tint.push_back((int)s.tint[0]); tint.push_back((int)s.tint[1]); tint.push_back((int)s.tint[2]);
            tint.SetStyle(YAML::EmitterStyle::Flow);
            n["tint"] = tint;
            YAML::Node br(YAML::NodeType::Sequence);
            br.push_back(s.yaw); br.push_back(s.pitch); br.push_back(s.roll);
            br.SetStyle(YAML::EmitterStyle::Flow);
            n["base_rot"] = br;
            root["models"].push_back(n);
        }
        std::ofstream fout(path);
        if (!fout) return false;
        fout << "# AfterAction — model manifest (written by Settings -> Models)\n";
        fout << root;
        return true;
    } catch (const YAML::Exception& e) {
        std::cerr << "Model manifest save failed: " << e.what() << "\n";
        return false;
    }
}

} // namespace afteraction
