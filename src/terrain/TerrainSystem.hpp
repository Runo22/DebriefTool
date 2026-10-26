#pragma once
#include "../geo/GeoFrame.hpp"
#include <tdem/tdem.hpp>
#include <raylib.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace afteraction {

// How the terrain surface is drawn.
enum class TerrainLook : int {
    None     = 0,   // nothing (tactical grid at sea level only)
    Grid     = 1,   // dark surface + metric grid (classic "tactical" look)
    Shaded   = 2,   // natural colours with hillshade
    ShadedGrid = 3, // shaded + grid
    Contours = 4,   // shaded + elevation contour lines
};

struct TerrainStyle {
    TerrainLook look   = TerrainLook::ShadedGrid;
    float vscale       = 1.0f;     // vertical exaggeration applied to the whole scene
    float detail       = 1.4f;     // LOD: split a chunk when closer than detail × its size
    Color fog          = { 32, 44, 62, 255 };
    float fog_start    = 20000.0f;
    float fog_end      = 120000.0f;
    float grid_spacing = 1000.0f;  // metres between grid lines
    float contour_step = 50.0f;    // metres between contour lines
};

struct TerrainStats {
    int    drawn = 0, cached = 0, queued = 0;
    double gpu_mb = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  TerrainSystem — real terrain from TDEM height packs (terrain/ folder).
//
//  The world is a quadtree of square chunks centred on the scene origin. Each
//  chunk is a 64×64-quad mesh sampled from the DEM; chunks split while the
//  camera is close (detail × size) until their grid spacing matches the best DEM
//  resolution under them (5 m at the detailed air bases, ~24 m around Ankara,
//  ~180 m elsewhere). Meshes are built on worker threads and uploaded on the
//  main thread; a parent stays visible until all four children are ready, so
//  there are never holes. Skirts hide cracks between LOD levels.
//
//  Heights are metres MSL; entities use the same reference, so altitude above
//  ground is simply alt − height_at().
// ─────────────────────────────────────────────────────────────────────────────

// Direction toward the sun (unit, world space) — shared by terrain and models.
inline constexpr Vector3 kSunDir{ -0.53233f, 0.72590f, -0.43554f };

class TerrainSystem {
public:
    TerrainSystem();
    ~TerrainSystem();
    TerrainSystem(const TerrainSystem&) = delete;
    TerrainSystem& operator=(const TerrainSystem&) = delete;

    // Load every .tdem pack in `data_dir`. Safe to call before the GL context.
    bool load_data(const std::string& data_dir);
    // Compile the shader and start workers. Needs the GL context.
    void init_gpu();
    // Free GPU resources (call while the GL context is alive).
    void shutdown();

    // The scene origin. Changing it invalidates every chunk.
    void set_frame(const GeoFrame& frame);
    [[nodiscard]] const GeoFrame& frame() const noexcept { return frame_; }

    [[nodiscard]] bool has_data() const noexcept { return !db_.empty(); }
    [[nodiscard]] const tdem::Database& db() const noexcept { return db_; }
    [[nodiscard]] const std::string& status() const noexcept { return status_; }

    // Terrain height (m MSL) under a world point; 0 outside DEM coverage (sea).
    [[nodiscard]] float height_at(float x, float z) const;
    [[nodiscard]] std::optional<float> height_at_geo(double lat, double lon) const;
    // Finest DEM resolution at a world point (m), 0 if uncovered.
    [[nodiscard]] float resolution_at(float x, float z) const;

    // First intersection of a ray with the (vertically scaled) terrain surface.
    bool raycast(const Ray& ray, float vscale, float max_dist, Vector3* hit) const;

    // RGBA hillshade map (size×size, row 0 = north) of the square of side `range`
    // metres centred on world (cx, cz). Thread-safe; meant for a worker thread.
    std::vector<unsigned char> render_map(const GeoFrame& frame, float cx, float cz,
                                          float range, int size) const;

    // Per frame: choose chunks for the camera, queue builds, upload results.
    void update(const Camera3D& cam, const TerrainStyle& style);
    // Draw the chunks chosen by update() (inside BeginMode3D).
    void draw(const Camera3D& cam, const TerrainStyle& style);

    [[nodiscard]] TerrainStats stats() const;

private:
    using Key = uint64_t;
    struct Chunk {
        Mesh     mesh{};
        bool     uploaded = false;
        float    hmin = 0.0f, hmax = 0.0f;
        uint64_t last_used = 0;
    };
    struct Built {
        Key      key = 0;
        uint64_t epoch = 0;
        Mesh     mesh{};
        float    hmin = 0.0f, hmax = 0.0f;
    };
    struct Request {
        Key   key;
        float priority;   // smaller = sooner
    };

    static Key make_key(int level, int ix, int iz) {
        return (uint64_t(level) << 48) | (uint64_t(ix) << 24) | uint64_t(iz);
    }
    static void split_key(Key k, int* level, int* ix, int* iz) {
        *level = int(k >> 48);
        *ix = int((k >> 24) & 0xFFFFFF);
        *iz = int(k & 0xFFFFFF);
    }
    void chunk_bounds(int level, int ix, int iz, double* x0, double* z0, double* size) const;
    float finest_resolution(double x0, double z0, double size) const;
    void select(int level, int ix, int iz, const Camera3D& cam, const TerrainStyle& style,
                std::vector<Request>& requests);
    bool box_visible(float x0, float y0, float z0, float x1, float y1, float z1) const;
    void worker_main();
    Built build_chunk(Key key, const GeoFrame& frame, uint64_t epoch) const;
    void clear_chunks();

    tdem::Database db_;
    GeoFrame       frame_;
    std::string    status_ = "no terrain data";
    bool           gpu_ready_ = false;
    Shader         shader_{};
    Material       material_{};
    int loc_cam_ = -1, loc_fog_col_ = -1, loc_fog_rng_ = -1, loc_vscale_ = -1,
        loc_grid_ = -1, loc_contour_ = -1, loc_look_ = -1, loc_sun_ = -1;

    // Main-thread chunk cache and this frame's draw list.
    std::unordered_map<Key, Chunk> chunks_;
    std::vector<Key> draw_list_;
    uint64_t frame_no_ = 0;
    float    planes_[5][4]{};
    float    split_scale_ = 1.0f;
    double   gpu_bytes_ = 0.0;
    int      queued_ = 0;

    // Worker shared state (guarded by mu_).
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::vector<Request>    queue_;
    std::unordered_set<Key> in_flight_;
    std::vector<Built>      done_;
    GeoFrame                worker_frame_;
    uint64_t                epoch_ = 1;
    bool                    quit_ = false;
    std::vector<std::thread> workers_;
};

} // namespace afteraction
