#pragma once
#include "../network/Packet.hpp"
#include <raylib.h>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace debrief {

// ─────────────────────────────────────────────────────────────────────────────
//  Asset Manager
//
//  Two tiers of models:
//
//  1. PROCEDURAL (always available, no files needed):
//     Each EntityTypeId gets a distinct built-in Raylib mesh shape with a
//     colour tint so entities are recognisable out of the box.
//
//  2. LOADED (user-supplied via Assimp: FBX / OBJ / glTF):
//     Call load() to import a file.  map_type() binds a type ID to that model.
//     Loaded models override procedural ones for that type.
//
//  Free model sources (drop into assets/models/ then call load()):
//    • Kenney Military Kit  —  https://kenney.nl/assets/military-kit  (CC0, .glb)
//    • Quaternius Low-Poly  —  https://quaternius.itch.io              (CC0)
//    • OpenGameArt          —  https://opengameart.org                 (varies)
// ─────────────────────────────────────────────────────────────────────────────

struct ModelEntry {
    Model      model;
    Color      tint        = WHITE;
    float      scale       = 1.0f;
    Quaternion base_rot    = {0,0,0,1};  // model-space correction (applied before entity rotation)
};

class AssetManager {
public:
    AssetManager();
    ~AssetManager();

    AssetManager(const AssetManager&)            = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // Load a model from disk using Assimp and register it by name.
    bool load(const std::string& name,
              const std::filesystem::path& path,
              float import_scale = 1.0f) noexcept;

    // Override the procedural default for a type with a loaded model name.
    void map_type(uint16_t type_id, const std::string& name);

    // Returns the best available ModelEntry for a type (loaded → procedural → cube).
    // Pointer is stable for the lifetime of the AssetManager.
    [[nodiscard]] const ModelEntry* get_for_type(uint16_t type_id) const noexcept;

    void unload_all() noexcept;
    void init_procedural();

private:
    bool convert_assimp(const std::filesystem::path& path,
                        float scale, ModelEntry& out) noexcept;

    std::unordered_map<std::string,  ModelEntry> named_;       // loaded models
    std::unordered_map<uint16_t,     std::string> type_to_name_; // type → named key
    std::unordered_map<uint16_t,     ModelEntry> procedural_;  // built-in shapes

    ModelEntry fallback_{};
    bool       ready_ = false;
};

} // namespace debrief
