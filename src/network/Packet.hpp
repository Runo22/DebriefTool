#pragma once
#include <cstdint>
#include <vector>
#include <array>

// ─────────────────────────────────────────────────────────────────────────────
//  DEBRIEF UDP Wire Protocol  —  send packets to UDP port 5555
//
//  Packet layout:
//    [ BatchHeader   8 bytes ]
//    [ EntityUpdate 56 bytes ] × count   (1 – 26 per packet)
//
//  Example (Python, 2 entities):
//    import socket, struct, time
//    hdr = struct.pack('<4sBBH', b'DBF1', 2, 0, seq)
//    ent = struct.pack('<IHB5sdddfffffQ',
//              entity_id, entity_type, health, callsign_bytes,
//              lat, lon, alt, phi, theta, psi, speed, time.time_ns())
//    sock.sendto(hdr + ent + ent2, ('127.0.0.1', 5555))
//
//  Entity types: 0=unknown, 1=jet, 2=missile, 3=aaa, 4=ground, 5=helo, 6=ship
//
//  Angles: aviation convention
//    psi   = heading  (0 = North, 90 = East, clockwise, 0–360°)
//    theta = pitch    (positive = nose up, –90 to +90°)
//    phi   = roll     (positive = right bank, –180 to +180°)
// ─────────────────────────────────────────────────────────────────────────────

namespace debrief::net {

// ── Wire-format structs (no padding, little-endian) ───────────────────────────
#pragma pack(push, 1)

struct BatchHeader {
    uint8_t  magic[4];   // {'D','B','F','1'}
    uint8_t  count;      // number of EntityUpdate records that follow (1–26)
    uint8_t  source_id;  // 0 if you only have one data source
    uint16_t sequence;   // increment each packet; used for drop detection
};
// sizeof = 8

struct EntityUpdate {
    uint32_t id;            // unique, stable ID for this unit (never reuse)
    uint16_t type;          // EntityTypeId below
    uint8_t  health;        // 0 = destroyed, 255 = full (use 255 if N/A)
    char     callsign[5];   // null-padded, e.g. "F16\0\0"

    double   lat;           // latitude  (decimal degrees, e.g.  36.8500)
    double   lon;           // longitude (decimal degrees, e.g.  35.1200)
    float    alt;           // altitude  (metres above sea level)

    float    phi;           // roll    (degrees, –180 to +180)
    float    theta;         // pitch   (degrees,  –90 to  +90)
    float    psi;           // heading (degrees, 0 = North, 90 = East, clockwise)

    float    speed;         // airspeed m/s   (0 = unknown)
    uint64_t time_ns;       // UNIX nanoseconds; 0 = server assigns current time
};
// sizeof = (4+2+1+5) + (8+8+4) + (4+4+4) + 4 + 8 = 56 bytes

enum EntityTypeId : uint16_t {
    TYPE_UNKNOWN = 0,
    TYPE_JET     = 1,
    TYPE_MISSILE = 2,
    TYPE_AAA     = 3,
    TYPE_GROUND  = 4,
    TYPE_HELO    = 5,
    TYPE_SHIP    = 6,
};

inline constexpr uint8_t kMagic[4]    = {'D','B','F','1'};
inline constexpr uint8_t kMaxPerPkt   = 26;   // fits standard Ethernet MTU

#pragma pack(pop)

// ── In-memory decoded state (used throughout the engine) ──────────────────────
// Filled by PacketParser; lat/lon/euler stored verbatim from the wire.
// position[] and orientation[] are filled by Application (ENU conversion).
struct EntityState {
    uint32_t source_id    = 0;
    uint32_t entity_id    = 0;
    uint16_t entity_type  = 0;   // EntityTypeId
    uint64_t timestamp_ns = 0;
    uint8_t  health       = 255;
    char     callsign[8]  = {};

    // As received from wire
    double   lat_deg  = 0.0;
    double   lon_deg  = 0.0;
    float    alt_m    = 0.0f;
    float    phi_deg  = 0.0f;   // roll
    float    theta_deg= 0.0f;   // pitch
    float    psi_deg  = 0.0f;   // heading

    float    speed_mps= 0.0f;

    // Derived by Application (ENU metres from scene origin, quaternion)
    float    position[3]     = {};
    float    orientation[4]  = {0, 0, 0, 1};  // identity quaternion
    float    velocity[3]     = {};

    bool     destroyed = false;
};

struct ParsedFrame {
    uint8_t  source_id = 0;
    uint16_t sequence  = 0;
    std::vector<EntityState> entities;
};

} // namespace debrief::net
