#include "DemoMode.hpp"
#include <chrono>
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace debrief::demo {

static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// Build a state with ENU position directly — bypasses lat/lon conversion.
// orientation[] left identity; Application::apply_state_to_ecs reads psi_deg
// from state and builds the quaternion. So we still fill psi/theta/phi here.
static net::EntityState make_state(
    uint32_t id, uint16_t type, const char* callsign,
    float x, float y, float z,
    float psi_deg, float theta_deg, float phi_deg,
    float speed_mps, uint8_t health = 255)
{
    net::EntityState s{};
    s.source_id    = 0;
    s.entity_id    = id;
    s.entity_type  = type;
    s.timestamp_ns = now_ns();
    s.health       = health;
    s.psi_deg      = psi_deg;
    s.theta_deg    = theta_deg;
    s.phi_deg      = phi_deg;
    s.speed_mps    = speed_mps;
    s.alt_m        = y;

    // Position already in ENU metres — skip lat/lon conversion.
    s.position[0]  = x;
    s.position[1]  = y;
    s.position[2]  = z;

    // Velocity from speed + heading
    float psi_rad  = psi_deg  * static_cast<float>(M_PI) / 180.0f;
    float theta_rad= theta_deg* static_cast<float>(M_PI) / 180.0f;
    s.velocity[0]  =  speed_mps * sinf(psi_rad) * cosf(theta_rad);
    s.velocity[1]  =  speed_mps * sinf(theta_rad);
    s.velocity[2]  = -speed_mps * cosf(psi_rad) * cosf(theta_rad);

    std::strncpy(s.callsign, callsign, 7);
    s.callsign[7] = '\0';
    return s;
}

// Circular orbit helper.
// Returns (x, y, z) and heading at angle `theta_orbit` (radians, clockwise from North).
static void orbit(float cx, float cy, float cz,
                  float radius, float theta_orbit,
                  float& out_x, float& out_y, float& out_z, float& out_psi_deg)
{
    out_x = cx + radius * sinf(theta_orbit);
    out_y = cy;
    out_z = cz - radius * cosf(theta_orbit);
    // Tangent heading (clockwise from North = psi)
    out_psi_deg = fmodf(theta_orbit * 180.0f / static_cast<float>(M_PI) + 90.0f, 360.0f);
    if (out_psi_deg < 0) out_psi_deg += 360.0f;
}

std::vector<net::EntityState> DemoMode::tick(float dt_sec) {
    time += dt_sec;
    const float t = static_cast<float>(time);

    std::vector<net::EntityState> states;
    states.reserve(8);

    // ── VIPER01 — blue jet, CW orbit at 3 000 m altitude, R=5 000 m ──────────
    {
        const float speed  = 200.0f;           // m/s
        const float radius = 5000.0f;
        const float omega  = speed / radius;   // rad/s
        float x, y, z, psi;
        orbit(0, 3000, 0, radius, omega * t, x, y, z, psi);

        // Gentle bank into the turn: phi ≈ atan(v²/R/g) in degrees ≈ 22°
        states.push_back(make_state(1, net::TYPE_JET, "VIPER01", x, y, z,
                                    psi, 0.0f, 22.0f, speed));
    }

    // ── VIPER02 — red jet, CCW orbit at 4 000 m, R=7 000 m ───────────────────
    {
        const float speed  = 250.0f;
        const float radius = 7000.0f;
        const float omega  = speed / radius;
        float x, y, z, psi;
        orbit(1000, 4000, -500, radius, -(omega * t) + 0.8f, x, y, z, psi);
        // CCW: negate theta and adjust psi
        psi = fmodf(psi + 180.0f, 360.0f);

        states.push_back(make_state(2, net::TYPE_JET, "VIPER02", x, y, z,
                                    psi, 0.0f, -22.0f, speed));
    }

    // ── AIM-120 — missile, launches from VIPER01 at t=15 s ───────────────────
    if (t >= 15.0f && t < 45.0f) {
        if (!missile_fired_) {
            // Snapshot VIPER01 launch position
            const float speed  = 200.0f;
            const float radius = 5000.0f;
            const float omega  = speed / radius;
            float psi_unused;
            orbit(0, 3000, 0, radius, omega * 15.0f,
                  missile_x_, missile_y_, missile_z_, psi_unused);
            missile_fired_ = missile_active_ = true;
        }

        // Fly toward VIPER02's current position
        const float speed2 = 250.0f;
        const float radius2= 7000.0f;
        const float omega2 = speed2 / radius2;
        float tx, ty, tz, tp;
        orbit(1000, 4000, -500, radius2, -(omega2 * t) + 0.8f, tx, ty, tz, tp);

        float dx = tx - missile_x_;
        float dy = ty - missile_y_;
        float dz = tz - missile_z_;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz) + 0.001f;

        const float missile_speed = 500.0f; // m/s
        float step = missile_speed * dt_sec;

        missile_x_ += dx / dist * step;
        missile_y_ += dy / dist * step;
        missile_z_ += dz / dist * step;

        float psi   = atan2f(dx, -dz) * 180.0f / static_cast<float>(M_PI);
        float theta = asinf(std::clamp(dy / dist, -1.0f, 1.0f)) * 180.0f / static_cast<float>(M_PI);

        states.push_back(make_state(3, net::TYPE_MISSILE, "AIM120",
                                    missile_x_, missile_y_, missile_z_,
                                    psi, theta, 0.0f, missile_speed));
    } else if (t >= 45.0f) {
        missile_active_ = false;
    }

    // ── BRAVO1 — static AAA site at (2000, 0, -3000) ─────────────────────────
    {
        states.push_back(make_state(4, net::TYPE_AAA, "BRAVO1",
                                    2000.0f, 0.0f, -3000.0f,
                                    0.0f, 0.0f, 0.0f, 0.0f));
    }

    // ── EAGLE01 — helicopter, slow CCW patrol at 200 m, R=1 500 m ────────────
    {
        const float speed  = 60.0f;
        const float radius = 1500.0f;
        const float omega  = speed / radius;
        float x, y, z, psi;
        orbit(-2000, 200, 1000, radius, -(omega * t) + (float)M_PI, x, y, z, psi);
        psi = fmodf(psi + 180.0f, 360.0f);

        // Gentle hover oscillation
        y += 20.0f * sinf(t * 0.3f);

        states.push_back(make_state(5, net::TYPE_HELO, "EAGLE01",
                                    x, y, z, psi, 0.0f, 5.0f, speed));
    }

    return states;
}

} // namespace debrief::demo
