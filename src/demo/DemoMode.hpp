#pragma once
#include "../network/Packet.hpp"
#include <vector>

namespace afteraction::demo {

// ─────────────────────────────────────────────────────────────────────────────
//  Demo Mode — scripted flight scenario, no UDP required.
//
//  Call tick() each frame; it returns a list of EntityState objects in
//  world-space ENU (metres, Y-up) ready to be injected directly into the ECS,
//  bypassing the network stack entirely.
//
//  Scenario (total ~90 seconds):
//    VIPER01 (Blue Jet)   — circular orbit, 3 000 m, R=5 000 m, 200 m/s
//    VIPER02 (Red Jet)    — counter-orbit,  4 000 m, R=7 000 m, 250 m/s
//    AIM120  (Missile)    — launches at t=15 s, intercepts VIPER02 at ~t=30 s
//    BRAVO1  (Ground)     — static AAA site at (2000, 0, -3000)
//    EAGLE01 (Helicopter) — slow patrol,   200 m, R=1 500 m, 60 m/s
// ─────────────────────────────────────────────────────────────────────────────

struct DemoMode {
    double time = 0.0;   // accumulated simulation seconds

    // Returns entity states for the current frame.
    // Positions are ENU metres from scene origin (lat=0,lon=0 → Raylib 0,0,0).
    std::vector<net::EntityState> tick(float dt_sec);

    void reset() { time = 0.0; }

private:
    bool missile_fired_    = false;
    bool missile_active_   = false;
    float missile_x_       = 0.0f;
    float missile_y_       = 0.0f;
    float missile_z_       = 0.0f;
};

} // namespace afteraction::demo
