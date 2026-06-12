#pragma once
#include <cstdint>
#include <vector>
#include <array>

// ─────────────────────────────────────────────────────────────────────────────
//  AFTERACTION UDP Wire Protocol  —  send packets to UDP port 22522
//
//  Packet layout:
//    [ BatchHeader   10 bytes ]
//    [ EntityUpdate 103 bytes ] × count   (1 – 14 per packet)
//
//  Example (Python, 2 entities):
//    import socket, struct, time
//    hdr = struct.pack('<4sBBI', b'DBF1', 2, 0, seq)            # 10 bytes
//    ent = struct.pack('<IHB32sdddddddQ',                       # 103 bytes
//              entity_id, entity_type, health, callsign_bytes,
//              lat, lon, alt, phi, theta, psi, speed, time.time_ns())
//    sock.sendto(hdr + ent + ent2, ('127.0.0.1', 22522))
//
//  See docs/PROTOCOL.md for the full field reference, units and examples.
//
//  Entity types: 0=unknown, 1=jet, 2=missile, 3=aaa, 4=ground, 5=helo, 6=ship
//
//  Angles: aviation convention
//    psi   = heading  (0 = North, 90 = East, clockwise, 0–360°)
//    theta = pitch    (positive = nose up, –90 to +90°)
//    phi   = roll     (positive = right bank, –180 to +180°)
// ─────────────────────────────────────────────────────────────────────────────

namespace afteraction::net {

// Maximum callsign length (including the null terminator) shared by the wire
// format, the in-memory state and the recording format.
inline constexpr int kCallsignLen = 32;

// ── Wire-format structs (no padding, little-endian) ───────────────────────────
#pragma pack(push, 1)

struct BatchHeader {
    uint8_t  magic[4];   // {'D','B','F','1'}
    uint8_t  count;      // number of EntityUpdate records that follow (1–14)
    uint8_t  source_id;  // 0 if you only have one data source
    uint32_t sequence;   // increment each packet; used for drop detection.
                         // 32-bit: ~2.2 years of headroom at 60 Hz before wrap.
};
// sizeof = 10

struct EntityUpdate {
    uint32_t id;                    // unique, stable ID for this unit (never reuse)
    uint16_t type;                  // EntityTypeId below
    uint8_t  health;                // 0 = destroyed, 255 = full (use 255 if N/A)
    char     callsign[kCallsignLen];// null-padded UTF-8, e.g. "VIPER01\0..."

    double   lat;                   // latitude  (decimal degrees, e.g.  36.8500)
    double   lon;                   // longitude (decimal degrees, e.g.  35.1200)
    double   alt;                   // altitude  (metres above sea level)

    double   phi;                   // roll    (degrees, –180 to +180)
    double   theta;                 // pitch   (degrees,  –90 to  +90)
    double   psi;                   // heading (degrees, 0 = North, 90 = East, clockwise)

    double   speed;                 // airspeed m/s   (0 = unknown)
    uint64_t time_ns;               // UNIX nanoseconds; 0 = server assigns current time
};
// sizeof = (4+2+1+32) + (8+8+8) + (8+8+8) + 8 + 8 = 103 bytes

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
inline constexpr uint8_t kMaxPerPkt   = 14;   // 10 + 14*103 = 1452 B, fits 1500 MTU

#pragma pack(pop)

static_assert(sizeof(BatchHeader)  == 10, "BatchHeader must be 10 bytes");
static_assert(sizeof(EntityUpdate) == 103, "EntityUpdate must be 103 bytes");

// ── In-memory decoded state (used throughout the engine) ──────────────────────
// Filled by PacketParser; lat/lon/euler stored verbatim from the wire.
// position[] and orientation[] are filled by Application (ENU conversion).
struct EntityState {
    uint32_t source_id    = 0;
    uint32_t entity_id    = 0;
    uint16_t entity_type  = 0;   // EntityTypeId
    uint64_t timestamp_ns = 0;
    uint8_t  health       = 255;
    char     callsign[kCallsignLen] = {};

    // As received from wire
    double   lat_deg  = 0.0;
    double   lon_deg  = 0.0;
    double   alt_m    = 0.0;
    double   phi_deg  = 0.0;    // roll
    double   theta_deg= 0.0;    // pitch
    double   psi_deg  = 0.0;    // heading

    double   speed_mps= 0.0;

    // Derived by Application (ENU metres from scene origin, quaternion)
    float    position[3]     = {};
    float    orientation[4]  = {0, 0, 0, 1};  // identity quaternion
    float    velocity[3]     = {};

    bool     destroyed = false;
};

struct ParsedFrame {
    uint8_t  source_id = 0;
    uint32_t sequence  = 0;
    std::vector<EntityState> entities;
};

} // namespace afteraction::net
