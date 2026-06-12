#pragma once
#include <cstdint>
#include <array>
#include "../network/Packet.hpp"   // net::kCallsignLen

// ─────────────────────────────────────────────────────────────────────────────
//  AfterAction Session File Format  —  ".aar"
//
//  Layout:
//
//  [ FileHeader   64 bytes  ]
//  [ SceneOrigin  32 bytes  ]  — optional georeference for ENU positions
//  [ FrameData    variable  ]  — sequential EntityFrames
//  [ FrameIndex   variable  ]  — sparse seek table; written last
//  [ Footer        16 bytes ]  — points to index; enables O(log N) seek
//
//  Frame layout (per saved frame):
//    FrameHeader (16 bytes)
//    EntitySnapshot × entity_count
//
//  All values: little-endian, no struct padding (pack(1)).
//
//  Design goals:
//  - Sequential write during live recording (no seeks).
//  - Sparse index every kIndexInterval frames for fast scrubbing.
//  - Footer lets a reader skip to the index without scanning the whole file.
//  - Magic + version enables forward compatibility.
// ─────────────────────────────────────────────────────────────────────────────

namespace afteraction::persist {

inline constexpr uint64_t kFileMagic   = 0x5745495645524141ULL; // "AAREVIEW"
inline constexpr uint32_t kFileVersion = 2;  // v2: callsign widened 8 -> 32 bytes
inline constexpr uint32_t kIndexInterval = 100; // one index entry every N frames

#pragma pack(push, 1)

struct FileHeader {
    uint64_t magic;           // kFileMagic
    uint32_t version;         // kFileVersion
    uint32_t reserved;
    uint64_t start_time_ns;   // first frame timestamp
    uint64_t end_time_ns;     // last frame timestamp (filled on close)
    uint64_t frame_count;     // total frames (filled on close)
    uint64_t index_offset;    // byte offset of FrameIndex section (filled on close)
    char     session_name[32];
};
static_assert(sizeof(FileHeader) == 80);

struct SceneOrigin {
    double   lat_deg   = 0.0;  // WGS-84 latitude of local ENU origin
    double   lon_deg   = 0.0;  // WGS-84 longitude
    double   alt_m     = 0.0;  // altitude above ellipsoid (metres)
    uint64_t reserved  = 0;
};
static_assert(sizeof(SceneOrigin) == 32);

struct FrameHeader {
    uint64_t timestamp_ns;
    uint16_t source_id;
    uint16_t entity_count;
    uint32_t frame_bytes;   // total bytes of this frame including this header
};
static_assert(sizeof(FrameHeader) == 16);

struct EntitySnapshot {
    uint32_t entity_id;
    uint16_t type;
    uint16_t flags;
    float    position[3];
    float    orientation[4];
    float    velocity[3];
    uint8_t  health;
    char     callsign[net::kCallsignLen];   // v2: 32 bytes (was 8)
    uint8_t  pad;            // keep struct layout explicit
};
static_assert(sizeof(EntitySnapshot) == 4+2+2+12+16+12+1+32+1); // 82 bytes

// One entry per kIndexInterval frames, stored at end of file.
struct IndexEntry {
    uint64_t timestamp_ns;
    uint64_t file_offset;     // byte position of that FrameHeader in the file
};
static_assert(sizeof(IndexEntry) == 16);

struct FileFooter {
    uint64_t index_offset;    // mirrors FileHeader::index_offset
    uint64_t magic;           // kFileMagic — so a reader can verify integrity
};
static_assert(sizeof(FileFooter) == 16);

#pragma pack(pop)

} // namespace afteraction::persist
