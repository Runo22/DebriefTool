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

## Example — Python

```python
import socket, struct, time

MAGIC    = b"DBF1"
HDR_FMT  = "<4sBBI"            # BatchHeader  (10 bytes)
ENT_FMT  = "<IHB32sdddddddQ"  # EntityUpdate (103 bytes)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
seq  = 0

def make_entity(eid, etype, callsign, lat, lon, alt,
                phi=0.0, theta=0.0, psi=0.0, speed=0.0, health=255):
    cs = callsign.encode()[:31].ljust(32, b"\x00")
    return struct.pack(ENT_FMT,
        eid, etype, health, cs,
        lat, lon, alt,        # degrees, degrees, metres MSL
        phi, theta, psi,      # roll, pitch, heading (degrees)
        speed,                # m/s
        time.time_ns())       # or 0 to let the server timestamp it

def send(entities, source_id=0):
    global seq
    hdr = struct.pack(HDR_FMT, MAGIC, len(entities), source_id, seq & 0xFFFFFFFF)
    sock.sendto(hdr + b"".join(entities), ("127.0.0.1", 5555))
    seq += 1

# One jet at 3000 m, heading west, 5° nose up, 220 m/s:
send([
    make_entity(1, 1, "VIPER01", 36.85, 35.12, 3000.0,
                phi=0.0, theta=5.0, psi=270.0, speed=220.0),
])
```

A complete multi-entity scenario sender is in
[`scripts/test_sender.py`](../scripts/test_sender.py):

```sh
python scripts/test_sender.py --host 127.0.0.1 --port 5555 --hz 10
```

---

## Example — C

```c
#include <string.h>
#include <stdint.h>

#pragma pack(push, 1)
struct BatchHeader {
    uint8_t  magic[4];
    uint8_t  count;
    uint8_t  source_id;
    uint32_t sequence;
};
struct EntityUpdate {
    uint32_t id;
    uint16_t type;
    uint8_t  health;
    char     callsign[32];
    double   lat, lon, alt;
    double   phi, theta, psi;
    double   speed;
    uint64_t time_ns;
};
#pragma pack(pop)

// Build a one-entity datagram into `buf`; returns its length.
size_t build_packet(uint8_t *buf, uint32_t seq) {
    struct BatchHeader h = { {'D','B','F','1'}, 1, 0, seq };
    struct EntityUpdate e = {0};
    e.id = 1; e.type = 1; e.health = 255;
    strncpy(e.callsign, "VIPER01", sizeof(e.callsign) - 1);
    e.lat = 36.85; e.lon = 35.12; e.alt = 3000.0;
    e.phi = 0.0;   e.theta = 5.0;  e.psi = 270.0;
    e.speed = 220.0; e.time_ns = 0;   // 0 = server timestamps it

    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &e, sizeof e);
    return sizeof h + sizeof e;       // 10 + 103 = 113
}
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
