#include "CsvImporter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace afteraction::persist {

// ── Helpers ───────────────────────────────────────────────────────────────────

// Strip surrounding whitespace and a single pair of double quotes.
static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    if (e - b >= 2 && s[b] == '"' && s[e - 1] == '"') { ++b; --e; }
    return s.substr(b, e - b);
}

static double to_d(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
static uint64_t to_u64(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 10); }

// Aviation Euler → quaternion (x,y,z,w), matching Application::euler_to_quat:
//   psi=heading (CW), theta=pitch (+up), phi=roll (+right).
static void mul(const float a[4], const float b[4], float r[4]) {
    float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    r[0] = x; r[1] = y; r[2] = z; r[3] = w;
}

static void euler_to_quat(float phi_deg, float theta_deg, float psi_deg, float out[4]) {
    const float d2r = 0.017453293f;
    float pr = -psi_deg   * d2r;   // heading: CW → CCW about Y
    float tr =  theta_deg * d2r;
    float rr = -phi_deg   * d2r;   // roll about -Z
    float qy[4] = { 0.0f, std::sin(pr * 0.5f), 0.0f, std::cos(pr * 0.5f) };
    float qx[4] = { std::sin(tr * 0.5f), 0.0f, 0.0f, std::cos(tr * 0.5f) };
    float qz[4] = { 0.0f, 0.0f, std::sin(rr * 0.5f), std::cos(rr * 0.5f) };
    float tmp[4];
    mul(qy, qx, tmp);
    mul(tmp, qz, out);
}

// ── Public API ──────────────────────────────────────────────────────────────

std::vector<std::string> CsvImporter::split_line(const std::string& line) const {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (char c : line) {
        if (c == '"') { in_quotes = !in_quotes; cur.push_back(c); }
        else if (c == delimiter && !in_quotes) { out.push_back(trim(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(trim(cur));
    return out;
}

std::optional<net::EntityState> CsvImporter::parse_row(
    const std::vector<std::string>& headers,
    const std::vector<std::string>& values,
    uint64_t& base_time_ns) const noexcept
{
    net::EntityState s{};
    bool have_time = false;
    bool have_euler = false;

    const size_t n = std::min(headers.size(), values.size());
    for (size_t i = 0; i < n; ++i) {
        auto it = mappings_.find(headers[i]);
        if (it == mappings_.end()) continue;
        const std::string& v = values[i];
        if (v.empty()) continue;

        switch (it->second) {
        case CsvField::Ignore: break;
        case CsvField::TimestampSec: s.timestamp_ns = (uint64_t)(to_d(v) * 1e9); have_time = true; break;
        case CsvField::TimestampMs:  s.timestamp_ns = (uint64_t)(to_d(v) * 1e6); have_time = true; break;
        case CsvField::TimestampNs:  s.timestamp_ns = to_u64(v);                 have_time = true; break;
        case CsvField::EntityId:     s.entity_id    = (uint32_t)to_u64(v); break;
        case CsvField::EntityType:   s.entity_type  = (uint16_t)to_u64(v); break;
        case CsvField::SourceId:     s.source_id    = (uint32_t)to_u64(v); break;
        case CsvField::PosX:         s.position[0]  = (float)to_d(v); break;
        case CsvField::PosY:         s.position[1]  = (float)to_d(v); s.alt_m = s.position[1]; break;
        case CsvField::PosZ:         s.position[2]  = (float)to_d(v); break;
        case CsvField::VelX:         s.velocity[0]  = (float)to_d(v); break;
        case CsvField::VelY:         s.velocity[1]  = (float)to_d(v); break;
        case CsvField::VelZ:         s.velocity[2]  = (float)to_d(v); break;
        case CsvField::YawDeg:       s.psi_deg      = (float)to_d(v); have_euler = true; break;
        case CsvField::PitchDeg:     s.theta_deg    = (float)to_d(v); have_euler = true; break;
        case CsvField::RollDeg:      s.phi_deg      = (float)to_d(v); have_euler = true; break;
        case CsvField::Health:       s.health       = (uint8_t)to_u64(v); break;
        case CsvField::Callsign: {
            std::size_t len = std::min<std::size_t>(v.size(), sizeof(s.callsign) - 1);
            std::memcpy(s.callsign, v.data(), len);
            s.callsign[len] = '\0';
            break;
        }
        }
    }

    // Without a timestamp column, synthesise a monotonic 20 Hz timeline so each
    // row still becomes a distinct playback frame.
    if (!have_time) { s.timestamp_ns = base_time_ns; base_time_ns += 50'000'000ULL; }

    if (have_euler)
        euler_to_quat(s.phi_deg, s.theta_deg, s.psi_deg, s.orientation);

    return s;
}

size_t CsvImporter::import(const std::filesystem::path& path, RowCallback cb) const noexcept {
    std::ifstream f(path);
    if (!f.is_open()) return 0;

    std::vector<std::string> headers;
    std::string line;
    uint64_t base_time_ns = 0;
    size_t count = 0;

    if (has_header) {
        if (!std::getline(f, line)) return 0;
        headers = split_line(line);
    }

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<std::string> values = split_line(line);
        auto st = parse_row(headers, values, base_time_ns);
        if (st) { cb(*st); ++count; }
    }
    return count;
}

} // namespace afteraction::persist
