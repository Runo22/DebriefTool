#include "DemoMode.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace afteraction::demo {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kG  = 9.81;
inline double rad(double d) { return d * kPi / 180.0; }
inline double deg(double r) { return r * 180.0 / kPi; }
inline double wrap180(double a) { a = std::fmod(a + 180.0, 360.0); return (a < 0 ? a + 360.0 : a) - 180.0; }

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// Runway 03/21 thresholds (OurAirports) and true heading.
constexpr double kRwy03Lat = 40.0662, kRwy03Lon = 32.5550;
constexpr double kRwyHdg   = 32.8;

} // namespace

DemoMode::DemoMode() {
    anchor_.set(kAnchorLat, kAnchorLon);
    reset();
}

double DemoMode::ground(double x, double z) const {
    if (!height_) return 0.0;
    double lat, lon;
    anchor_.to_geo(x, z, &lat, &lon);
    return height_(lat, lon);
}

void DemoMode::reset() {
    time_ = 0.0;
    missile_launched_ = false;
    flyers_.clear();

    const Vector3 thr = anchor_.to_world(kRwy03Lat, kRwy03Lon, 0.0);
    const double hs = std::sin(rad(kRwyHdg)), hc = std::cos(rad(kRwyHdg));
    // Point `d` metres along the runway axis from the 03 threshold, `s` to the right.
    auto along = [&](double d, double s) {
        return std::pair<double, double>{ thr.x + d * hs + s * hc, thr.z - d * hc + s * hs };
    };

    // VIPER01 — takeoff from runway 03, then a racetrack west of the field.
    {
        Flyer f{1, net::TYPE_JET, "VIPER01"};
        f.x = thr.x; f.z = thr.z;
        f.hdg = kRwyHdg;
        f.on_ground = true;
        f.rotate_speed = 82.0;
        f.accel = 3.2;
        f.hold_until = 4.0;
        f.max_turn = 4.0;
        f.max_climb = 25.0;
        auto [ax, az] = along(9000, 0);
        auto [bx, bz] = along(16000, -9000);
        auto [cx, cz] = along(4000, -16000);
        auto [dx, dz] = along(-6000, -9000);
        f.route = { {ax, az, 1800, 150, false}, {bx, bz, 3000, 210, false},
                    {cx, cz, 3000, 220, false}, {dx, dz, 3000, 220, false},
                    {ax, az, 3000, 220, false} };
        flyers_.push_back(f);
    }
    // VIPER02 — wide orbit at 5 500 m (approximated by 8 waypoints on a circle).
    {
        Flyer f{2, net::TYPE_JET, "VIPER02"};
        const double R = 12000, cx = 4000, cz = -6000;
        for (int i = 0; i < 8; ++i) {
            const double a = -i * kPi / 4;
            f.route.push_back({cx + R * std::cos(a), cz + R * std::sin(a), 5500, 260, false});
        }
        f.x = f.route[0].x; f.z = f.route[0].z + 3000; f.alt = 5500; f.speed = 260;
        f.hdg = 0;
        f.max_turn = 3.0;
        flyers_.push_back(f);
    }
    // VIPER03 — figure of eight at 4 200 m north-east.
    {
        Flyer f{6, net::TYPE_JET, "VIPER03"};
        const double cx = 10000, cz = -14000;
        f.route = { {cx - 7000, cz, 4200, 240, false}, {cx, cz - 5000, 4200, 240, false},
                    {cx + 7000, cz, 4200, 240, false}, {cx, cz + 5000, 4200, 240, false} };
        f.x = cx - 9000; f.z = cz + 2000; f.alt = 4200; f.speed = 240; f.hdg = 20;
        f.max_turn = 5.0;
        flyers_.push_back(f);
    }
    // EAGLE01 — helicopter patrol 150 m above the terrain, south of the field.
    {
        Flyer f{5, net::TYPE_HELO, "EAGLE01"};
        f.route = { {-3000, 4000, 150, 55, true}, {2000, 7000, 150, 55, true},
                    {5000, 3000, 150, 55, true}, {0, 1500, 150, 55, true} };
        f.x = -3000; f.z = 2500; f.speed = 55; f.hdg = 30;
        f.alt = ground(f.x, f.z) + 150;
        f.max_turn = 8.0;
        f.max_climb = 8.0;
        flyers_.push_back(f);
    }
    // ANKA01 — UAV holding pattern at 4 800 m west of the field.
    {
        Flyer f{8, net::TYPE_UAV, "ANKA01"};
        const double R = 7000, cx = -9000, cz = -3000;
        for (int i = 0; i < 8; ++i) {
            const double a = i * kPi / 4;
            f.route.push_back({cx + R * std::cos(a), cz + R * std::sin(a), 4800, 60, false});
        }
        f.x = cx + R; f.z = cz - 2000; f.alt = 4800; f.speed = 60; f.hdg = 180;
        f.max_turn = 3.0;
        f.max_climb = 5.0;
        flyers_.push_back(f);
    }
    // CONVOY1 — ground vehicle loop on the plain south-east of the base.
    {
        Flyer f{7, net::TYPE_GROUND, "CONVOY1"};
        f.route = { {1500, 2500, 0, 14, true}, {4500, 3500, 0, 14, true},
                    {5500, 6500, 0, 14, true}, {2000, 6000, 0, 14, true} };
        f.x = 1500; f.z = 2500; f.speed = 14; f.hdg = 70;
        f.on_ground = true;
        f.ride = 0.0;          // vehicle models sit on their wheels
        f.max_turn = 15.0;
        flyers_.push_back(f);
    }
}

void DemoMode::step(Flyer& f, double dt) {
    if (!f.alive) return;
    if (time_ < f.hold_until) {
        if (f.on_ground) f.alt = ground(f.x, f.z) + f.ride;
        return;
    }

    // Ground roll: straight down the runway until rotation speed.
    if (f.on_ground && f.rotate_speed > 0.0) {
        f.speed += f.accel * dt;
        f.x += f.speed * std::sin(rad(f.hdg)) * dt;
        f.z -= f.speed * std::cos(rad(f.hdg)) * dt;
        f.alt = ground(f.x, f.z) + f.ride;
        f.pitch = f.roll = 0.0;
        if (f.speed >= f.rotate_speed) { f.on_ground = false; f.accel = 6.0; }
        return;
    }

    if (f.route.empty()) return;
    const Waypoint& w = f.route[f.wp];
    const double dx = w.x - f.x, dz = w.z - f.z;
    const double dist = std::hypot(dx, dz);
    if (dist < std::max(250.0, f.speed * 5.0)) f.wp = (f.wp + 1) % f.route.size();

    // Heading toward the waypoint, turn-rate limited; bank from the turn rate.
    const double bearing = deg(std::atan2(dx, -dz));
    const double turn = std::clamp(wrap180(bearing - f.hdg) * 0.6, -f.max_turn, f.max_turn);
    f.hdg = std::fmod(f.hdg + turn * dt + 360.0, 360.0);
    const double bank = f.on_ground ? 0.0 : deg(std::atan(f.speed * rad(turn) / kG));
    f.roll += (bank - f.roll) * std::min(1.0, 3.0 * dt);

    f.speed += std::clamp(w.speed - f.speed, -f.accel * dt, f.accel * dt);

    const double gnd = ground(f.x, f.z);
    if (f.on_ground) {
        f.alt = gnd + f.ride;
        f.pitch = 0.0;
    } else {
        const double target = w.agl ? gnd + w.alt : w.alt;
        const double climb = std::clamp((target - f.alt) * 0.25, -f.max_climb, f.max_climb);
        f.alt = std::max(f.alt + climb * dt, gnd + 20.0);
        f.pitch += (deg(std::asin(std::clamp(climb / std::max(f.speed, 1.0), -1.0, 1.0))) - f.pitch)
                   * std::min(1.0, 2.0 * dt);
    }
    const double horiz = f.speed * std::cos(rad(f.pitch));
    f.x += horiz * std::sin(rad(f.hdg)) * dt;
    f.z -= horiz * std::cos(rad(f.hdg)) * dt;
}

net::EntityState DemoMode::to_state(const Flyer& f) const {
    net::EntityState s{};
    s.source_id    = 0;
    s.entity_id    = f.id;
    s.entity_type  = f.type;
    s.timestamp_ns = now_ns();
    s.health       = f.alive ? 255 : 0;
    s.destroyed    = !f.alive;
    anchor_.to_geo(f.x, f.z, &s.lat_deg, &s.lon_deg);
    s.alt_m     = f.alt;
    s.psi_deg   = f.hdg;
    s.theta_deg = f.pitch;
    s.phi_deg   = f.roll;
    s.speed_mps = f.speed;
    std::snprintf(s.callsign, sizeof(s.callsign), "%s", f.callsign);
    return s;
}

std::vector<net::EntityState> DemoMode::tick(float dt_sec) {
    // Integrate in small steps so low frame rates don't break the autopilot.
    const double dt = std::min(static_cast<double>(dt_sec), 0.25);
    const int n = std::max(1, static_cast<int>(std::ceil(dt / 0.05)));
    for (int i = 0; i < n; ++i) {
        time_ += dt / n;
        for (auto& f : flyers_) step(f, dt / n);
    }

    // AIM120: launched from VIPER02 at T+40 s, pure pursuit on VIPER03,
    // self-destructs after 30 s.
    Flyer* v2 = nullptr; Flyer* v3 = nullptr; Flyer* msl = nullptr;
    for (auto& f : flyers_) {
        if (f.id == 2) v2 = &f;
        if (f.id == 6) v3 = &f;
        if (f.id == 3) msl = &f;
    }
    if (!missile_launched_ && time_ >= 40.0 && v2) {
        Flyer m{3, net::TYPE_MISSILE, "AIM120"};
        m.x = v2->x; m.z = v2->z; m.alt = v2->alt - 10; m.hdg = v2->hdg; m.speed = v2->speed;
        flyers_.push_back(m);
        missile_launched_ = true;
        missile_t0_ = time_;
        msl = &flyers_.back();
        for (auto& f : flyers_) if (f.id == 6) v3 = &f;
    }
    if (msl && msl->alive && v3) {
        const double dx = v3->x - msl->x, dz = v3->z - msl->z, dy = v3->alt - msl->alt;
        const double d  = std::sqrt(dx * dx + dy * dy + dz * dz) + 1e-3;
        msl->speed = std::min(780.0, msl->speed + 120.0 * dt);
        const double step = msl->speed * dt;
        msl->x += dx / d * step; msl->z += dz / d * step; msl->alt += dy / d * step;
        msl->hdg   = std::fmod(deg(std::atan2(dx, -dz)) + 360.0, 360.0);
        msl->pitch = deg(std::asin(std::clamp(dy / d, -1.0, 1.0)));
        if (time_ - missile_t0_ > 30.0 || d < 60.0) msl->alive = false;
    }

    std::vector<net::EntityState> out;
    out.reserve(flyers_.size() + 1);
    for (const auto& f : flyers_) out.push_back(to_state(f));

    // BRAVO1 — static air-defence vehicle east of the field, facing the runway.
    {
        Flyer site{4, net::TYPE_AAA, "BRAVO1"};
        site.x = 3500; site.z = -2500;
        site.alt = ground(site.x, site.z);
        site.hdg = 250.0;
        out.push_back(to_state(site));
    }
    return out;
}

} // namespace afteraction::demo
