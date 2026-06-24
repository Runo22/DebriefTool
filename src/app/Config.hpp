#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../ui/AfterActionUI.hpp"

namespace afteraction {

// One entity-type → model binding from assets/models.yaml. Raylib-free so this
// header stays light; Application converts tint/euler to Color/Quaternion.
struct ModelSpec {
    uint16_t    type  = 0;            // net::EntityTypeId
    std::string file;                 // relative to assets/ (e.g. "models/f16.glb")
    float       scale = 1.0f;         // render size multiplier
    uint8_t     tint[3] = {255,255,255};
    float       yaw = 0, pitch = 0, roll = 0;  // base-rotation correction, degrees
};

class ConfigManager {
public:
    static void load_config(UIState& state, const std::string& path = "afteraction_config.yaml");
    static void save_config(const UIState& state, const std::string& path = "afteraction_config.yaml");

    // Parse the model manifest. Returns empty if the file is missing/invalid.
    static std::vector<ModelSpec> load_model_manifest(const std::string& path = "assets/models.yaml");
};

} // namespace afteraction
