#pragma once
#include "../network/Packet.hpp"
#include <raylib.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace afteraction {

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

// CPU-side parsed mesh (no GPU handles). Produced by the worker thread; uploaded
// to the GPU on the main thread. Keeps the slow Assimp work off the render thread.
struct CpuMesh {
    std::vector<float>          vertices, normals, texcoords;
    std::vector<unsigned short> indices;
};

class AssetManager {
public:
    AssetManager();
    ~AssetManager();

    AssetManager(const AssetManager&)            = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // ── Synchronous load (blocks; used by tools/preview and direct callers) ────
    // Imports via Assimp and registers by name. tint/base_rot let a manifest fix
    // the model's facing and colour.
    bool load(const std::string& name,
              const std::filesystem::path& path,
              float import_scale = 1.0f,
              Color tint = WHITE,
              Quaternion base_rot = {0,0,0,1}) noexcept;

    // ── Asynchronous load (non-blocking; keeps startup instant) ────────────────
    // Enqueues a parse job; the worker thread does the Assimp parse, then
    // pump_uploads() (main thread) does the GPU upload and binds type → model.
    void request_load(const std::string& name,
                      const std::filesystem::path& path,
                      uint16_t type, float import_scale,
                      Color tint, Quaternion base_rot) noexcept;

    // Call once per frame on the MAIN thread. Uploads up to `budget` ready meshes
    // (GL calls must be on this thread). Returns the entity types whose model just
    // became available, so the caller can hot-swap existing entities.
    std::vector<uint16_t> pump_uploads(int budget = 2) noexcept;

    [[nodiscard]] int  pending() const noexcept { return inflight_.load(); }

    // Override the procedural default for a type with a loaded model name.
    void map_type(uint16_t type_id, const std::string& name);

    // Remove a type's loaded-model override, reverting it to the procedural shape.
    void unmap_type(uint16_t type_id);

    // Returns the best available ModelEntry for a type (loaded → procedural → cube).
    // Pointer is stable for the lifetime of the AssetManager.
    [[nodiscard]] const ModelEntry* get_for_type(uint16_t type_id) const noexcept;

    void unload_all() noexcept;
    void init_procedural();

private:
    bool convert_assimp(const std::filesystem::path& path,
                        float scale, Color tint, Quaternion base_rot,
                        ModelEntry& out) noexcept;
    // Thread-safe: Assimp parse + CPU vertex assembly only (NO GL calls).
    bool parse_file(const std::filesystem::path& path, CpuMesh& out) noexcept;
    // Main-thread only: GPU upload of a CpuMesh into a ModelEntry.
    bool upload_cpu(const CpuMesh& cpu, float scale, Color tint,
                    Quaternion base_rot, ModelEntry& out) noexcept;
    void worker_loop();
    void stop_worker() noexcept;

    std::unordered_map<std::string,  ModelEntry> named_;       // loaded models
    std::unordered_map<uint16_t,     std::string> type_to_name_; // type → named key
    std::unordered_map<uint16_t,     ModelEntry> procedural_;  // built-in shapes

    ModelEntry fallback_{};
    bool       ready_ = false;

    // ── Async loading machinery ────────────────────────────────────────────────
    struct LoadJob { std::string name; std::filesystem::path path;
                     uint16_t type; float scale; Color tint; Quaternion base_rot; };
    struct ReadyModel { LoadJob job; CpuMesh cpu; bool ok = false; };

    std::thread              worker_;
    std::atomic<bool>        worker_run_{false};
    std::mutex               jobs_mu_;
    std::condition_variable  jobs_cv_;
    std::deque<LoadJob>      jobs_;
    std::mutex               ready_mu_;
    std::vector<ReadyModel>  ready_models_;
    std::atomic<int>         inflight_{0};   // queued + parsing, not yet uploaded
};

} // namespace afteraction
