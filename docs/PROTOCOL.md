# Debrief UDP Telemetry Protocol

This document describes exactly what to send to Debrief over UDP, what each
field means, the units, valid ranges, and copy-paste examples.

- **Transport:** UDP (one datagram = one batch of entity updates)
- **Default port:** `5555` (change with `--port N`, or in **Settings → Network**)
- **Byte order:** little-endian
- **Packing:** no padding (`#pragma pack(1)` / `struct` with `<` in Python)

A datagram is one `BatchHeader` followed by `count` `EntityUpdate` records:

```
+----------------------+
| BatchHeader  (10 B)  |
+----------------------+
| EntityUpdate (103 B) |  ← repeated `count` times (1 .. 14)
| EntityUpdate (103 B) |
|        ...           |
+----------------------+
```

Send **one packet per simulation tick** containing every entity you want to
show. A reasonable rate is 5–60 Hz; Debrief interpolates smoothly between
frames so you do not need a high rate.

---

## BatchHeader — 10 bytes

| Field       | Type       | Bytes | Description |
|-------------|------------|-------|-------------|
| `magic`     | `uint8[4]` | 4     | Must be the ASCII bytes `D B F 1` (`b"DBF1"`). |
| `count`     | `uint8`    | 1     | Number of `EntityUpdate` records that follow. `1`–`14` to stay within one 1500-byte MTU. |
| `source_id` | `uint8`    | 1     | Identifier for this data source. Use `0` if you only have one. Lets multiple feeds coexist. |
| `sequence`  | `uint32`   | 4     | Increment by 1 each packet you send. Used only for drop/reorder detection. Wraps cleanly (≈2.2 years at 60 Hz). Restarting your sender at `0` is fine. |

Python format string: `"<4sBBI"`

---

## EntityUpdate — 103 bytes

| Field      | Type        | Bytes | Unit / Range | Description |
|------------|-------------|-------|--------------|-------------|
| `id`       | `uint32`    | 4     | —            | **Stable, unique** ID for this unit. Never reuse or change it for the life of the track. |
| `type`     | `uint16`    | 2     | enum         | Entity type (see table below). Controls the model/shape and trail colour. |
| `health`   | `uint8`     | 1     | `0`–`255`    | `255` = full health, `0` = destroyed (the entity is hidden). Use `255` if you don't track health. |
| `callsign` | `char[32]`  | 32    | UTF-8        | Display name, null-padded. Up to **31 characters** + a trailing `\0`. e.g. `"VIPER01"`. |
| `lat`      | `float64`   | 8     | degrees      | Latitude, decimal degrees (e.g. `36.8500`). |
| `lon`      | `float64`   | 8     | degrees      | Longitude, decimal degrees (e.g. `35.1200`). |
| `alt`      | `float64`   | 8     | metres MSL   | Altitude in **metres above sea level** (absolute). Displayed in feet (metres in parentheses). |
| `phi`      | `float64`   | 8     | degrees      | Roll. `-180`..`+180`. Positive = right bank. |
| `theta`    | `float64`   | 8     | degrees      | Pitch. `-90`..`+90`. Positive = nose up. |
| `psi`      | `float64`   | 8     | degrees      | Heading. `0` = North, `90` = East, clockwise, `0`..`360`. |
| `speed`    | `float64`   | 8     | m/s          | Airspeed in metres/second. `0` = unknown. Used for the velocity vector and chase-cam lead. |
| `time_ns`  | `uint64`    | 8     | ns           | UNIX time in **nanoseconds**. Send `0` to have Debrief stamp it with the receive time. |

Python format string: `"<IHB32sdddddddQ"`

### Coordinate notes

- Debrief converts `lat`/`lon` to a local East/North/Up scene relative to the
  **first** position it sees (so the scene stays near the origin), but the
  vertical axis is the **absolute** `alt` — every entity reports its true MSL
  altitude regardless of arrival order.
- Angles follow the aviation convention above. If your loaded model appears to
  face the wrong way, it's a model orientation issue, not a protocol one.

### Entity types

| Value | Constant       | Meaning      |
|-------|----------------|--------------|
| 0     | `TYPE_UNKNOWN` | Unknown      |
| 1     | `TYPE_JET`     | Jet aircraft |
| 2     | `TYPE_MISSILE` | Missile      |
| 3     | `TYPE_AAA`     | AAA / gun    |
| 4     | `TYPE_GROUND`  | Ground unit  |
| 5     | `TYPE_HELO`    | Helicopter   |
| 6     | `TYPE_SHIP`    | Ship         |

---

## C++ structs

Declare the wire structs with 1-byte packing so they map exactly onto the
datagram. These are byte-for-byte identical to `src/network/Packet.hpp`.

```cpp
#include <cstdint>

#pragma pack(push, 1)
struct BatchHeader {
    uint8_t  magic[4];     // {'D','B','F','1'}
    uint8_t  count;        // number of EntityUpdate records following (1–14)
    uint8_t  source_id;    // 0 if you only have one source
    uint32_t sequence;     // increment each packet
};
struct EntityUpdate {
    uint32_t id;           // stable unique ID — never change it
    uint16_t type;         // EntityTypeId (1 = jet, …)
    uint8_t  health;       // 255 = full, 0 = destroyed
    char     callsign[32]; // null-padded, up to 31 chars
    double   lat, lon, alt;   // degrees, degrees, metres MSL
    double   phi, theta, psi; // roll, pitch, heading (degrees)
    double   speed;           // m/s (0 = unknown)
    uint64_t time_ns;         // UNIX ns, or 0 to let the server timestamp it
};
#pragma pack(pop)

static_assert(sizeof(BatchHeader)  == 10,  "BatchHeader must be 10 bytes");
static_assert(sizeof(EntityUpdate) == 103, "EntityUpdate must be 103 bytes");
```

## Example — C++ (single entity)

```cpp
#include <cstring>
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
#endif

int main() {
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(5555);
    ::inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    uint32_t seq = 0;

    EntityUpdate e{};
    e.id = 1; e.type = 1 /*TYPE_JET*/; e.health = 255;
    std::strncpy(e.callsign, "VIPER01", sizeof(e.callsign) - 1);
    e.lat = 36.85; e.lon = 35.12; e.alt = 3000.0;   // metres MSL
    e.phi = 0.0;   e.theta = 5.0;  e.psi = 270.0;    // heading West, 5° nose up
    e.speed = 220.0;                                  // m/s
    e.time_ns = 0;                                    // server timestamps it

    BatchHeader h{ {'D','B','F','1'}, 1, 0, seq++ };

    uint8_t buf[sizeof(h) + sizeof(e)];
    std::memcpy(buf,             &h, sizeof(h));
    std::memcpy(buf + sizeof(h), &e, sizeof(e));
    ::sendto(sock, reinterpret_cast<const char*>(buf), sizeof(buf), 0,
             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
}
```

## Example — C++ (multiple entities per packet)

Append up to **14** entities after one header, set `count`, and send the whole
buffer as a single datagram.

```cpp
#include <vector>
#include <cstring>

// Sends one datagram containing every entity in `ents`.
void send_batch(int sock, const sockaddr_in& dst,
                const std::vector<EntityUpdate>& ents, uint32_t& seq) {
    BatchHeader h{ {'D','B','F','1'},
                   static_cast<uint8_t>(ents.size()), 0, seq++ };

    std::vector<uint8_t> buf(sizeof(h) + ents.size() * sizeof(EntityUpdate));
    std::memcpy(buf.data(), &h, sizeof(h));
    std::memcpy(buf.data() + sizeof(h),
                ents.data(), ents.size() * sizeof(EntityUpdate));

    ::sendto(sock, reinterpret_cast<const char*>(buf.data()),
             static_cast<int>(buf.size()), 0,
             reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
}

// Helper to fill one record.
EntityUpdate make_entity(uint32_t id, uint16_t type, const char* callsign,
                         double lat, double lon, double alt,
                         double phi, double theta, double psi,
                         double speed, uint8_t health = 255) {
    EntityUpdate e{};
    e.id = id; e.type = type; e.health = health;
    std::strncpy(e.callsign, callsign, sizeof(e.callsign) - 1);
    e.lat = lat; e.lon = lon; e.alt = alt;
    e.phi = phi; e.theta = theta; e.psi = psi;
    e.speed = speed; e.time_ns = 0;
    return e;
}
```

> **Endianness:** the structs are sent in the host's native byte order. Debrief
> expects little-endian (the overwhelmingly common case on x86/ARM). On a
> big-endian host you would need to byte-swap each field before sending.

## Demo sender (Python)

A ready-to-run reference sender with a scripted multi-entity flight scenario
ships as [`scripts/test_sender.py`](../scripts/test_sender.py) — handy for
exercising the app without writing any code:

```sh
python scripts/test_sender.py --host 127.0.0.1 --port 5555 --hz 10
```

---

## Quick checklist

- [ ] `magic` is exactly `DBF1`.
- [ ] `count` matches the number of `EntityUpdate` records appended.
- [ ] Each entity has a **stable** `id`.
- [ ] `lat`/`lon` in decimal degrees, `alt` in metres MSL.
- [ ] Angles in degrees (`psi` 0=N/90=E CW, `theta` +up, `phi` +right).
- [ ] `speed` in m/s.
- [ ] `time_ns` is UNIX nanoseconds, or `0`.
- [ ] Increment `sequence` each packet.
