#pragma once
#include <raylib.h>
#include <cmath>

namespace afteraction {

// ─────────────────────────────────────────────────────────────────────────────
//  GeoFrame — the scene's local East/North/Up frame around a WGS-84 origin.
//
//  World axes (raylib, Y-up):  +X = East,  +Y = Up (metres MSL),  −Z = North.
//
//  Horizontal mapping is a local equirectangular projection: linear in latitude
//  and longitude, scaled by cos(origin latitude). It is exactly invertible and
//  shared by entities AND terrain, so the two always line up; distortion only
//  grows hundreds of km from the origin. Altitude is kept absolute (MSL) so it
//  can be compared directly with DEM heights (also MSL/EGM2008).
// ─────────────────────────────────────────────────────────────────────────────
class GeoFrame {
public:
    static constexpr double kMetPerDeg = 111319.9;
    static constexpr double kDegToRad  = 3.14159265358979323846 / 180.0;

    void set(double lat0, double lon0) {
        lat0_ = lat0;
        lon0_ = lon0;
        cos_lat0_ = std::cos(lat0 * kDegToRad);
        valid_ = true;
    }
    void reset() { valid_ = false; }

    [[nodiscard]] bool   valid() const noexcept { return valid_; }
    [[nodiscard]] double lat0()  const noexcept { return lat0_; }
    [[nodiscard]] double lon0()  const noexcept { return lon0_; }

    [[nodiscard]] Vector3 to_world(double lat, double lon, double alt_msl) const {
        return { static_cast<float>((lon - lon0_) * cos_lat0_ * kMetPerDeg),
                 static_cast<float>(alt_msl),
                 static_cast<float>(-(lat - lat0_) * kMetPerDeg) };
    }

    void to_geo(double x, double z, double* lat, double* lon) const {
        *lat = lat0_ - z / kMetPerDeg;
        *lon = lon0_ + x / (kMetPerDeg * cos_lat0_);
    }

    // World metres per degree (for converting grid spacings).
    [[nodiscard]] double m_per_deg_lat() const noexcept { return kMetPerDeg; }
    [[nodiscard]] double m_per_deg_lon() const noexcept { return kMetPerDeg * cos_lat0_; }

    // Great-circle-ish distance (equirectangular) between two points, metres.
    static double distance_m(double lat1, double lon1, double lat2, double lon2) {
        const double x = (lon2 - lon1) * std::cos(0.5 * (lat1 + lat2) * kDegToRad);
        const double y = lat2 - lat1;
        return std::sqrt(x * x + y * y) * kMetPerDeg;
    }

private:
    double lat0_ = 0.0, lon0_ = 0.0, cos_lat0_ = 1.0;
    bool   valid_ = false;
};

} // namespace afteraction
