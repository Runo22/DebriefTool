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

#include <fstream>
#include <iostream>

namespace afteraction {

void ConfigManager::load_config(UIState& state, const std::string& path) {
    try {
        YAML::Node config = YAML::LoadFile(path);
        
        if (config["show_trails"]) state.show_trails = config["show_trails"].as<bool>();
        if (config["show_labels"]) state.show_labels = config["show_labels"].as<bool>();
        if (config["show_velocity_vec"]) state.show_velocity_vec = config["show_velocity_vec"].as<bool>();
        if (config["show_minimap"]) state.show_minimap = config["show_minimap"].as<bool>();
        if (config["ribbon_trails"]) state.ribbon_trails = config["ribbon_trails"].as<bool>();
        
        if (config["camera_yaw"]) state.camera_yaw = config["camera_yaw"].as<float>();
        if (config["camera_pitch"]) state.camera_pitch = config["camera_pitch"].as<float>();
        if (config["camera_distance"]) state.camera_distance = config["camera_distance"].as<float>();
        if (config["entity_3d_scale"]) state.entity_3d_scale = config["entity_3d_scale"].as<float>();
        if (config["trail_width_override"]) state.trail_width_override = config["trail_width_override"].as<float>();
        if (config["altitude_exaggerate"]) state.altitude_exaggerate = config["altitude_exaggerate"].as<float>();
        
        // Added far clip setting
        if (config["far_clip_plane"]) state.far_clip_plane = config["far_clip_plane"].as<float>();
        
        if (config["terrain_mode"]) state.terrain_mode = config["terrain_mode"].as<int>();
        if (config["terrain_height_scale"]) state.terrain_height_scale = config["terrain_height_scale"].as<float>();

        if (config["invert_look"]) state.invert_look = config["invert_look"].as<bool>();
        if (config["mouse_sensitivity"]) state.mouse_sensitivity = config["mouse_sensitivity"].as<float>();

        if (config["timeline_height"]) state.timeline_height = config["timeline_height"].as<float>();

    } catch (const YAML::Exception& e) {
        std::cerr << "Config load failed: " << e.what() << "\n";
    }
}

void ConfigManager::save_config(const UIState& state, const std::string& path) {
    try {
        YAML::Node config;
        
        config["show_trails"] = state.show_trails;
        config["show_labels"] = state.show_labels;
        config["show_velocity_vec"] = state.show_velocity_vec;
        config["show_minimap"] = state.show_minimap;
        config["ribbon_trails"] = state.ribbon_trails;
        
        config["camera_yaw"] = state.camera_yaw;
        config["camera_pitch"] = state.camera_pitch;
        config["camera_distance"] = state.camera_distance;
        config["entity_3d_scale"] = state.entity_3d_scale;
        config["trail_width_override"] = state.trail_width_override;
        config["altitude_exaggerate"] = state.altitude_exaggerate;
        
        config["far_clip_plane"] = state.far_clip_plane;
        
        config["terrain_mode"] = state.terrain_mode;
        config["terrain_height_scale"] = state.terrain_height_scale;

        config["invert_look"] = state.invert_look;
        config["mouse_sensitivity"] = state.mouse_sensitivity;

        config["timeline_height"] = state.timeline_height;

        std::ofstream fout(path);
        fout << config;
    } catch (const YAML::Exception& e) {
        std::cerr << "Config save failed: " << e.what() << "\n";
    }
}

} // namespace afteraction
