#include "Airfields.hpp"
#include "TerrainSystem.hpp"
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"   // raymath.h
#endif
#include <raymath.h>
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#include <rlgl.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

namespace afteraction {

namespace {

constexpr float kStep = 30.0f;   // metres between height samples along a runway
constexpr float kFt   = 0.3048f;

// Split one CSV line (double-quoted fields, no embedded newlines).
std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
            else if (c == '"') quoted = false;
            else cur += c;
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

} // namespace

size_t Airfields::load_csv(const std::string& path) {
    std::ifstream f(path);
    if (!f) return 0;
    std::string line;
    if (!std::getline(f, line)) return 0;
    const auto head = split_csv(line);
    std::unordered_map<std::string, size_t> col;
    for (size_t i = 0; i < head.size(); ++i) col[head[i]] = i;
    for (const char* k : { "airport_ident", "width_ft", "closed", "le_ident", "he_ident",
                           "le_latitude_deg", "le_longitude_deg", "he_latitude_deg", "he_longitude_deg" })
        if (!col.count(k)) return 0;

    runways_.clear();
    while (std::getline(f, line)) {
        const auto v = split_csv(line);
        auto get = [&](const char* k) -> const std::string& {
            static const std::string empty;
            const size_t i = col[k];
            return i < v.size() ? v[i] : empty;
        };
        if (get("closed") == "1" || get("le_latitude_deg").empty() || get("he_latitude_deg").empty())
            continue;
        Runway r;
        r.airfield = get("airport_ident");
        r.ident    = get("le_ident") + "/" + get("he_ident");
        r.le_lat = std::atof(get("le_latitude_deg").c_str());
        r.le_lon = std::atof(get("le_longitude_deg").c_str());
        r.he_lat = std::atof(get("he_latitude_deg").c_str());
        r.he_lon = std::atof(get("he_longitude_deg").c_str());
        const float w = static_cast<float>(std::atof(get("width_ft").c_str()));
        r.width_m  = (w > 0 ? w : 148.0f) * kFt;
        r.length_m = static_cast<float>(GeoFrame::distance_m(r.le_lat, r.le_lon, r.he_lat, r.he_lon));
        if (r.length_m < 300.0f) continue;
        runways_.push_back(r);
    }
    return runways_.size();
}

void Airfields::rebuild(const GeoFrame& frame, const TerrainSystem& terrain) {
    strips_.clear();
    labels_.clear();
    if (!frame.valid()) return;
    std::map<std::string, std::pair<Vector3, int>> centres;
    for (const auto& r : runways_) {
        Strip s;
        s.a = frame.to_world(r.le_lat, r.le_lon, 0.0);
        s.b = frame.to_world(r.he_lat, r.he_lon, 0.0);
        const Vector3 d = Vector3Subtract(s.b, s.a);
        s.length = Vector3Length(d);
        if (s.length < 1.0f || s.length > 6000.0f) continue;
        s.along = Vector3Scale(d, 1.0f / s.length);
        s.side  = { -s.along.z, 0.0f, s.along.x };
        s.width = r.width_m;
        const int n = static_cast<int>(std::ceil(s.length / kStep)) + 1;
        for (int i = 0; i < n; ++i) {
            const float t = std::min(s.length, i * kStep);
            const Vector3 p = Vector3Add(s.a, Vector3Scale(s.along, t));
            s.h.push_back(terrain.height_at(p.x, p.z));
        }
        auto& c = centres[r.airfield];
        c.first = Vector3Add(c.first, Vector3Scale(Vector3Add(s.a, s.b), 0.5f));
        c.second++;
        strips_.push_back(std::move(s));
    }
    for (auto& [icao, c] : centres) {
        Vector3 p = Vector3Scale(c.first, 1.0f / c.second);
        p.y = terrain.height_at(p.x, p.z);
        labels_.push_back({ icao, p });
    }
}

void Airfields::draw(const Camera3D& cam, float vscale, float max_dist) const {
    if (strips_.empty()) return;
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    for (const auto& s : strips_) {
        const Vector3 mid = Vector3Scale(Vector3Add(s.a, s.b), 0.5f);
        const float dist = Vector3Distance({ mid.x, s.h[s.h.size() / 2] * vscale, mid.z }, cam.position);
        if (dist > max_dist) continue;
        // Lift grows with distance so the strip never z-fights with the ground.
        const float lift = std::max(0.25f, dist * 0.0006f);
        const int n = static_cast<int>(s.h.size());
        auto at = [&](float t, float off) {
            const float ft = std::clamp(t / kStep, 0.0f, float(n - 1));
            const int   i  = std::min(int(ft), n - 2);
            const float h  = s.h[i] + (s.h[i + 1] - s.h[i]) * (ft - i);
            Vector3 p = Vector3Add(Vector3Add(s.a, Vector3Scale(s.along, t)), Vector3Scale(s.side, off));
            p.y = h * vscale + lift;
            return p;
        };
        auto quad = [&](float t0, float t1, float o0, float o1, Color c) {
            rlColor4ub(c.r, c.g, c.b, c.a);
            const Vector3 p0 = at(t0, o0), p1 = at(t0, o1), p2 = at(t1, o1), p3 = at(t1, o0);
            rlVertex3f(p0.x, p0.y, p0.z); rlVertex3f(p1.x, p1.y, p1.z); rlVertex3f(p2.x, p2.y, p2.z);
            rlVertex3f(p0.x, p0.y, p0.z); rlVertex3f(p2.x, p2.y, p2.z); rlVertex3f(p3.x, p3.y, p3.z);
        };
        const float hw = s.width * 0.5f;
        rlBegin(RL_TRIANGLES);
        // Asphalt, with a lighter shoulder edge.
        for (float t = 0; t < s.length; t += kStep)
            quad(t, std::min(t + kStep, s.length), -hw, hw, { 46, 48, 52, 255 });
        if (dist < 12000.0f) {
            const float ml = std::max(0.9f, dist * 0.0012f);       // marking width
            const Color white{ 225, 228, 232, 255 };
            for (float t = 60.0f; t + 30.0f < s.length - 60.0f; t += 50.0f)   // centreline
                quad(t, t + 30.0f, -ml * 0.5f, ml * 0.5f, white);
            for (int end = 0; end < 2; ++end) {                    // threshold "piano keys"
                const float t0 = end == 0 ? 6.0f : s.length - 36.0f;
                for (int k = 0; k < 8; ++k) {
                    const float o = -hw + 3.0f + k * (s.width - 6.0f) / 8.0f;
                    quad(t0, t0 + 30.0f, o, o + std::max(1.8f, ml), white);
                }
            }
            quad(0.0f, 1.5f + ml, -hw, hw, white);                 // threshold bars
            quad(s.length - 1.5f - ml, s.length, -hw, hw, white);
        }
        rlEnd();
    }
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
}

void Airfields::draw_labels(const Camera3D& cam, float vscale, const Font& font) const {
    const Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    for (const auto& l : labels_) {
        const Vector3 p{ l.pos.x, l.pos.y * vscale, l.pos.z };
        const Vector3 to = Vector3Subtract(p, cam.position);
        const float d = Vector3Length(to);
        if (Vector3DotProduct(to, fwd) <= 0.0f || d > 250000.0f || d < 1500.0f) continue;
        const Vector2 sp = GetWorldToScreen(p, cam);
        const float fs = 15.0f;
        const Vector2 sz = MeasureTextEx(font, l.text.c_str(), fs, 0.5f);
        const float a = std::clamp(1.0f - (d - 120000.0f) / 130000.0f, 0.0f, 1.0f);
        DrawCircleLines(int(sp.x), int(sp.y), 5.0f, Fade({ 255, 255, 255, 255 }, 0.8f * a));
        DrawTextEx(font, l.text.c_str(), { sp.x - sz.x * 0.5f, sp.y + 8.0f }, fs, 0.5f,
                   Fade({ 235, 240, 245, 255 }, 0.9f * a));
    }
}

} // namespace afteraction
