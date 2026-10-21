// tdem.hpp — read terrain heights from TDEM packs (see FORMAT.md).
//
// Self-contained C++17 library: no dependencies besides the vendored
// public-domain inflater (third_party/sinfl.h). Thread-safe for concurrent
// queries; decoded tiles are kept in a bounded LRU cache.
//
//   tdem::Database db;
//   db.add_directory("terrain/data");
//   auto h = db.height(40.0789, 32.5656);        // metres MSL, or nullopt
//
// Heights are metres above mean sea level (EGM2008) on WGS-84 lat/lon. When
// several layers cover a point the finest wins; near a fine layer's border it
// fades into the next coarser one over the layer's blend distance, so there
// are no steps between resolutions.
#pragma once

#include "export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tdem {

struct LayerInfo {
    std::string name;
    std::string pack;          // file the layer came from
    double   north = 0.0;      // latitude of row 0 (deg)
    double   west  = 0.0;      // longitude of column 0 (deg)
    double   dlat  = 0.0;      // row spacing (deg); latitude decreases with row
    double   dlon  = 0.0;      // column spacing (deg)
    uint32_t rows = 0, cols = 0, tile_size = 0;
    float    quant_step   = 0.0f;  // height quantisation (m)
    float    blend_m      = 0.0f;  // edge fade distance into coarser layers (m)
    float    resolution_m = 0.0f;  // nominal ground spacing (m)
    float    min_h = 0.0f, max_h = 0.0f;

    double south() const { return north - (rows - 1) * dlat; }
    double east()  const { return west + (cols - 1) * dlon; }
    bool contains(double lat, double lon) const {
        return lat <= north && lat >= south() && lon >= west && lon <= east();
    }
};

class TDEM_API Database {
public:
    Database();
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Load one .tdem file. May be called several times; layers from all packs
    // are merged and ordered finest first. On failure returns false and, if
    // err is given, a reason.
    bool add_pack(const std::string& path, std::string* err = nullptr);

    // Load every *.tdem in a directory. Returns how many packs were loaded.
    size_t add_directory(const std::string& dir, std::string* err = nullptr);

    bool empty() const;
    const std::vector<LayerInfo>& layers() const;

    // Blended height at a point, metres MSL. nullopt if no layer covers it.
    std::optional<float> height(double lat, double lon) const;

    // Same, but ignoring layers finer than min_resolution_m — use for distant
    // terrain LODs so far-away meshes don't alias 5 m data.
    std::optional<float> height(double lat, double lon, float min_resolution_m) const;

    float height_or(double lat, double lon, float fallback = 0.0f) const;

    // Bilinear height from one layer only (no blending). nullopt outside it.
    std::optional<float> layer_height(size_t layer_index, double lat, double lon) const;

    // Fill out[rows*cols] (row-major, row 0 = north) with blended heights of a
    // regular lat/lon grid. Uncovered samples get `fallback`. Returns the
    // number of covered samples.
    size_t sample_grid(double north, double west, double dlat, double dlon,
                       int rows, int cols, float* out,
                       float fallback = 0.0f, float min_resolution_m = 0.0f) const;

    // Finest resolution (m) available at a point, or 0 if not covered.
    float resolution_at(double lat, double lon) const;

    // Decoded-tile cache (default 256 MB).
    void   set_cache_limit(size_t bytes);
    size_t cache_bytes() const;
    void   clear_cache();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tdem
