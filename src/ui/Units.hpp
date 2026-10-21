#pragma once
#include <cmath>
#include <cstdio>

namespace afteraction {

// ─────────────────────────────────────────────────────────────────────────────
//  Display units and coordinate formats (Settings → General). Internally
//  everything is SI: metres, metres/second, degrees.
// ─────────────────────────────────────────────────────────────────────────────
enum class AltUnit   : int { Feet = 0, Metres = 1 };
enum class SpeedUnit : int { Knots = 0, KmH = 1, MetresPerSec = 2 };
enum class CoordFmt  : int { DMS = 0, DDM = 1, Decimal = 2 };

struct UnitPrefs {
    AltUnit   alt   = AltUnit::Feet;
    SpeedUnit speed = SpeedUnit::Knots;
    CoordFmt  coord = CoordFmt::DMS;

    [[nodiscard]] float alt_value(float m) const { return alt == AltUnit::Feet ? m * 3.28084f : m; }
    [[nodiscard]] const char* alt_suffix() const { return alt == AltUnit::Feet ? "ft" : "m"; }
    // The other altitude unit, shown in parentheses.
    [[nodiscard]] float alt_value_alt(float m) const { return alt == AltUnit::Feet ? m : m * 3.28084f; }
    [[nodiscard]] const char* alt_suffix_alt() const { return alt == AltUnit::Feet ? "m" : "ft"; }

    [[nodiscard]] float speed_value(float mps) const {
        switch (speed) {
            case SpeedUnit::KmH:          return mps * 3.6f;
            case SpeedUnit::MetresPerSec: return mps;
            default:                      return mps * 1.94384f;
        }
    }
    [[nodiscard]] const char* speed_suffix() const {
        switch (speed) {
            case SpeedUnit::KmH:          return "km/h";
            case SpeedUnit::MetresPerSec: return "m/s";
            default:                      return "kt";
        }
    }

    // One coordinate, e.g. 40°04'44.0"N · 40°04.733'N · 40.07889°N
    void format_coord(double v, bool is_lat, char* buf, size_t n) const {
        const char hemi = is_lat ? (v >= 0 ? 'N' : 'S') : (v >= 0 ? 'E' : 'W');
        const double a = std::fabs(v);
        const int d = static_cast<int>(a);
        const double m = (a - d) * 60.0;
        switch (coord) {
            case CoordFmt::Decimal:
                std::snprintf(buf, n, "%.5f\xc2\xb0%c", a, hemi);
                break;
            case CoordFmt::DDM:
                std::snprintf(buf, n, "%d\xc2\xb0%06.3f'%c", d, m, hemi);
                break;
            default: {
                const int mi = static_cast<int>(m);
                std::snprintf(buf, n, "%d\xc2\xb0%02d'%04.1f\"%c", d, mi, (m - mi) * 60.0, hemi);
            }
        }
    }
};

} // namespace afteraction
