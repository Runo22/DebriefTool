#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <utility>

// -----------------------------------------------------------------------------
//  AFTERACTION UDP Wire Protocol  --  send packets to UDP port 22522
//
//  Packet layout:
//    [ BatchHeader   10 bytes ]
//    [ EntityUpdate 103 bytes ]  repeated 'count' times   (1..14 per packet)
//
//  Example (Python, 2 entities):
//    import socket, struct, time
//    hdr = struct.pack('<4sBBI', b'DBF1', 2, 0, seq)            # 10 bytes
//    ent = struct.pack('<IHB32sdddddddQ',                       # 103 bytes
//              entity_id, entity_type, health, callsign_bytes,
//              lat, lon, alt, phi, theta, psi, speed, time.time_ns())
//    sock.sendto(hdr + ent + ent2, ('127.0.0.1', 22522))
//
//  CLEAR ALL TRACKS: send a header-only packet with count = 0 (no EntityUpdate
//    records). This wipes every entity, e.g. at the start of a new session:
//    sock.sendto(struct.pack('<4sBBI', b'DBF1', 0, 0, seq), ('127.0.0.1', 22522))
//
//  See docs/PROTOCOL.md for the full field reference, units and examples.
//
//  Entity types: 0=unknown, 1=jet, 2=missile, 3=aaa, 4=ground, 5=helo, 6=ship,
//                7=uav, 8=ucav, 9=sam, 10=prop, 11=transport
//
//  Angles: aviation convention (binary float64, values in degrees)
//    psi   = heading  (0 = North, 90 = East, clockwise, 0..360 deg)
//    theta = pitch    (positive = nose up, -90 to +90 deg)
//    phi   = roll     (positive = right bank, -180 to +180 deg)
// -----------------------------------------------------------------------------

namespace afteraction::net {

// Maximum callsign length (including the null terminator) shared by the wire
// format, the in-memory state and the recording format.
inline constexpr int kCallsignLen = 32;

// Windows-1254 (Turkish ANSI) byte → UTF-8 for the 12 Turkish letters, or
// nullptr. Windows programs often send names in this code page, not UTF-8.
inline const char* cp1254_turkish_utf8(unsigned char c) noexcept {
    switch (c) {
    case 0xC7: return "\xC3\x87";  case 0xE7: return "\xC3\xA7";   // Ç ç
    case 0xD0: return "\xC4\x9E";  case 0xF0: return "\xC4\x9F";   // Ğ ğ
    case 0xDD: return "\xC4\xB0";  case 0xFD: return "\xC4\xB1";   // İ ı
    case 0xD6: return "\xC3\x96";  case 0xF6: return "\xC3\xB6";   // Ö ö
    case 0xDE: return "\xC5\x9E";  case 0xFE: return "\xC5\x9F";   // Ş ş
    case 0xDC: return "\xC3\x9C";  case 0xFC: return "\xC3\xBC";   // Ü ü
    default:   return nullptr;
    }
}

// Make a received callsign displayable, in place (`cs` null-terminated within
// `n` bytes, n <= 64):
//  - valid UTF-8 is kept (Turkish letters included);
//  - Turkish letters in Windows-1254 are converted to UTF-8;
//  - the name ends at the first other byte that isn't readable text (control
//    character, broken UTF-8 — e.g. uninitialised memory after the name — or
//    U+FFFD, the mark of an earlier failed conversion);
//  - trailing spaces, and a trailing run of two or more '?' (what Windows'
//    WideCharToMultiByte writes for characters it can't convert), are dropped.
inline void clean_callsign(char* cs, int n) noexcept {
    if (n <= 0) return;
    if (n > 64) n = 64;
    char out[64];
    int i = 0, o = 0;
    while (i < n && cs[i] != '\0') {
        const auto c = static_cast<unsigned char>(cs[i]);
        int len = 0;
        if (c >= 0x20 && c < 0x7F)    len = 1;
        else if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        bool ok = len > 0 && i + len <= n;
        for (int k = 1; ok && k < len; ++k)
            ok = (static_cast<unsigned char>(cs[i + k]) & 0xC0) == 0x80;
        if (ok && len >= 3) {   // reject overlong forms, surrogates and > U+10FFFF
            const auto c1 = static_cast<unsigned char>(cs[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F) ||
                (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F))
                ok = false;
        }
        if (ok && len == 3 && c == 0xEF && static_cast<unsigned char>(cs[i + 1]) == 0xBF &&
            static_cast<unsigned char>(cs[i + 2]) == 0xBD)
            break;                                          // U+FFFD
        const char* src = cs + i;
        int put = len;
        if (!ok) {
            src = cp1254_turkish_utf8(c);
            if (!src) break;
            put = 2;
            len = 1;
        }
        if (o + put > n - 1) break;                         // keep whole characters only
        for (int k = 0; k < put; ++k) out[o++] = src[k];
        i += len;
    }
    while (o > 0 && out[o - 1] == ' ') --o;
    int q = 0;
    while (q < o && out[o - 1 - q] == '?') ++q;
    if (q >= 2) { o -= q; while (o > 0 && out[o - 1] == ' ') --o; }
    for (int k = 0; k < o; ++k) cs[k] = out[k];
    for (int k = o; k < n; ++k) cs[k] = '\0';   // zero the rest: no stale bytes after the name
}

// -- Wire-format structs (no padding, little-endian) ------------------------
#pragma pack(push, 1)

struct BatchHeader {
    uint8_t  magic[4];   // {'D','B','F','1'}
    uint8_t  count;      // number of EntityUpdate records that follow (1..14).
                         // count == 0 is a "clear all tracks" control packet.
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

    double   phi;                   // roll    (degrees, -180 to +180)
    double   theta;                 // pitch   (degrees,  -90 to  +90)
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
    TYPE_UAV     = 7,
    TYPE_UCAV    = 8,   // jet-powered unmanned combat aircraft (ANKA-3 style)
    TYPE_SAM     = 9,   // surface-to-air missile launcher
    TYPE_PROP    = 10,  // propeller aircraft (HÜRKUŞ style)
    TYPE_TRANSPORT = 11, // transport / support aircraft (C-130 style)
};

inline constexpr uint8_t kMagic[4]    = {'D','B','F','1'};
inline constexpr uint8_t kMaxPerPkt   = 14;   // 10 + 14*103 = 1452 B, fits 1500 MTU

#pragma pack(pop)

static_assert(sizeof(BatchHeader)  == 10, "BatchHeader must be 10 bytes");
static_assert(sizeof(EntityUpdate) == 103, "EntityUpdate must be 103 bytes");

// -- In-memory decoded state (used throughout the engine) --------------------
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
    bool     clear_all = false;   // set when count == 0 (a "clear all tracks" packet)
    // Callsigns that had to be cleaned, as received (entity id, raw 32 bytes) —
    // logged once per track so a sender's encoding problem can be diagnosed.
    std::vector<std::pair<uint32_t, std::array<char, kCallsignLen>>> raw_callsigns;
};

} // namespace afteraction::net
