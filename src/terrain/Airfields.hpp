#pragma once
#include "../geo/GeoFrame.hpp"
#include <raylib.h>
#include <string>
#include <vector>

namespace afteraction {

class TerrainSystem;

// ─────────────────────────────────────────────────────────────────────────────
//  Airfields — runway overlays (asphalt strip, centreline, threshold bars)
//  draped on the terrain, from the OurAirports runway table shipped with the
//  terrain data (terrain/runways_tr.csv).
// ─────────────────────────────────────────────────────────────────────────────
class Airfields {
public:
    struct Runway {
        std::string airfield, ident;
        double le_lat, le_lon, he_lat, he_lon;
        float  width_m, length_m;
    };

    // Load a runways CSV (OurAirports columns). Returns the number loaded.
    size_t load_csv(const std::string& path);

    // Rebuild world-space geometry for a new origin (heights from the terrain).
    void rebuild(const GeoFrame& frame, const TerrainSystem& terrain);

    // Draw runways near the camera (inside BeginMode3D).
    void draw(const Camera3D& cam, float vscale, float max_dist) const;

    // Screen-space ICAO labels for airfields in view (after EndMode3D).
    void draw_labels(const Camera3D& cam, float vscale, const Font& font) const;

    [[nodiscard]] const std::vector<Runway>& runways() const { return runways_; }

private:
    struct Strip {
        Vector3 a, b;            // threshold centres (world, real height)
        Vector3 along, side;     // unit vectors
        float   length, width;
        std::vector<float> h;    // terrain height every kStep metres along the axis
    };
    struct Label { std::string text; Vector3 pos; };
    std::vector<Runway> runways_;
    std::vector<Strip>  strips_;
    std::vector<Label>  labels_;
};

} // namespace afteraction
