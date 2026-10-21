#pragma once
#include "../geo/GeoFrame.hpp"
#include "../network/Packet.hpp"
#include <functional>
#include <vector>

namespace afteraction::demo {

// ─────────────────────────────────────────────────────────────────────────────
//  Demo Mode — scripted scenario around Akıncı Air Base (LTAE), no UDP needed.
//
//  tick() returns states exactly like the network would deliver them
//  (lat / lon / MSL altitude + Euler angles + speed), so the demo exercises the
//  same ingest path as live data. Ground units and low flyers follow the real
//  terrain via the height callback.
//
//    VIPER01  jet      — lines up on runway 03, takes off, flies a racetrack
//    VIPER02  jet      — wide orbit at 5 500 m
//    VIPER03  jet      — figure-of-eight at 4 200 m
//    AIM120   missile  — launched by VIPER02, chases VIPER03, self-destructs
//    BRAVO1   AAA      — air-defence site east of the field
//    EAGLE01  helo     — low patrol 150 m above the terrain
//    CONVOY1  ground   — vehicle loop on the plain south of the base
// ─────────────────────────────────────────────────────────────────────────────

// Terrain height (m MSL) at a lat/lon.
using HeightFn = std::function<float(double lat, double lon)>;

class DemoMode {
public:
    static constexpr double kAnchorLat = 40.0789;   // Akıncı Air Base
    static constexpr double kAnchorLon = 32.5656;

    DemoMode();

    void set_height_fn(HeightFn fn) { height_ = std::move(fn); }
    std::vector<net::EntityState> tick(float dt_sec);
    void reset();

    struct Waypoint { double x, z, alt, speed; bool agl; };

private:
    struct Flyer {
        Flyer(uint32_t id_, uint16_t type_, const char* cs) : id(id_), type(type_), callsign(cs) {}
        uint32_t    id;
        uint16_t    type;
        const char* callsign;
        double x = 0, z = 0, alt = 0;          // ENU (m) from the anchor, MSL alt
        double hdg = 0, pitch = 0, roll = 0;   // degrees
        double speed = 0;                      // m/s
        double max_turn = 3.0;                 // deg/s
        double max_climb = 40.0;               // m/s
        double accel = 8.0;                    // m/s²
        std::vector<Waypoint> route;
        size_t wp = 0;
        bool   on_ground = false;              // ground roll / ground vehicle
        double rotate_speed = 0;               // takeoff speed (0 = stays on ground)
        double hold_until = 0;                 // seconds before moving
        bool   alive = true;
    };

    double ground(double x, double z) const;
    void   step(Flyer& f, double dt);
    net::EntityState to_state(const Flyer& f) const;

    GeoFrame anchor_;
    HeightFn height_;
    double   time_ = 0.0;
    std::vector<Flyer> flyers_;
    // Missile engagement
    bool   missile_launched_ = false;
    double missile_t0_ = 0.0;
};

} // namespace afteraction::demo
