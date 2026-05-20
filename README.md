# Debrief

Real-time 3D tactical telemetry visualization and post-action review tool.

Receives spatial data over UDP, renders it in 3D, records everything, and provides full VCR-style playback — scrub, rewind, fast-forward through any engagement.

![Demo screenshot placeholder](assets/screenshot_placeholder.png)

---

## Quick start (no UDP source needed)

```bat
git clone --recurse-submodules <this-repo>
scripts\bootstrap.bat
cmake --build build --config Release --parallel
build\Release\debrief.exe --demo
```

The `--demo` flag runs a built-in scripted scenario (two jets, a missile, an AAA site, and a helicopter) so you can evaluate the tool without any external data source.

---

## Building

### Windows (primary)

Requirements: **Visual Studio 2022** with C++ workload, **CMake 3.25+**, **Git**.

```bat
scripts\bootstrap.bat
```

This runs `git submodule update --init --recursive` then configures CMake for VS 2022 x64. Open `build\debrief.sln` in Visual Studio, or build from the command line:

```bat
cmake --build build --config RelWithDebInfo --parallel
```

### macOS

Requirements: **Xcode Command Line Tools**, **CMake 3.25+**.

```sh
bash scripts/bootstrap.sh
```

---

## UDP Packet Format

Send packets to **UDP port 5555** (configurable with `--port`).

### Wire layout

```
[ BatchHeader   8 bytes ]
[ EntityUpdate 56 bytes ] × count   (max 26 per packet, fits standard MTU)
```

### Structs (C, little-endian, no padding)

```c
#pragma pack(push, 1)

struct BatchHeader {
    uint8_t  magic[4];   // must be {'D','B','F','1'}
    uint8_t  count;      // number of EntityUpdate records following (1–26)
    uint8_t  source_id;  // 0 if you only have one data source
    uint16_t sequence;   // increment each packet (for drop detection)
};

struct EntityUpdate {
    uint32_t id;          // stable unique ID for this unit — never change it
    uint16_t type;        // see entity types below
    uint8_t  health;      // 255 = full, 0 = destroyed
    char     callsign[5]; // null-padded, e.g. "F16\0\0"

    double   lat;         // latitude  (decimal degrees, e.g.  36.8500)
    double   lon;         // longitude (decimal degrees, e.g.  35.1200)
    float    alt;         // altitude  (metres above sea level)

    float    phi;         // roll    (degrees, –180 to +180)
    float    theta;       // pitch   (degrees, –90 to +90, positive = nose up)
    float    psi;         // heading (degrees, 0 = North, 90 = East, clockwise)

    float    speed;       // airspeed (m/s, 0 = unknown)
    uint64_t time_ns;     // UNIX nanoseconds; 0 = server assigns current time
};

#pragma pack(pop)
```

### Entity types

| Value | Meaning | Default shape |
|---|---|---|
| 0 | Unknown | White cube |
| 1 | Jet aircraft | Blue cone |
| 2 | Missile | Red cylinder |
| 3 | AAA / gun | Dark cylinder |
| 4 | Ground target | Orange box |
| 5 | Helicopter | Green sphere |
| 6 | Ship | Grey box |

### Python sender example

```python
import socket, struct, time

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
seq  = 0

def send(entities):
    global seq
    hdr  = struct.pack('<4sBBH', b'DBF1', len(entities), 0, seq)
    body = b''.join(
        struct.pack('<IHB5sdddfffffQ',
                    e['id'], e['type'], e['health'],
                    e['callsign'].encode().ljust(5, b'\x00'),
                    e['lat'], e['lon'], e['alt'],
                    e['phi'], e['theta'], e['psi'],
                    e['speed'], time.time_ns())
        for e in entities
    )
    sock.sendto(hdr + body, ('127.0.0.1', 5555))
    seq += 1

send([{
    'id': 1, 'type': 1, 'health': 255, 'callsign': 'F16',
    'lat': 36.85, 'lon': 35.12, 'alt': 3000.0,
    'phi': 0.0, 'theta': 5.0, 'psi': 270.0,   # heading West, 5° nose up
    'speed': 220.0,
}])
```

A full demo sender with a multi-entity flight scenario is at [`scripts/test_sender.py`](scripts/test_sender.py):

```sh
python scripts/test_sender.py --host 127.0.0.1 --port 5555 --hz 10
```

---

## Adding 3D Models

The app ships with coloured procedural shapes as built-in defaults. To load proper models:

1. Drop an **FBX**, **OBJ**, or **glTF/GLB** file into `assets/models/`
2. Add two lines to `Application::init_assets()` in [`src/app/Application.cpp`](src/app/Application.cpp):

```cpp
assets_.load("jet", "assets/models/jet.glb", 0.01f);  // 0.01 if model is in cm
assets_.map_type(net::TYPE_JET, "jet");
```

### Free model sources

| Pack | License | Format | URL |
|---|---|---|---|
| Kenney Military Kit | CC0 | GLB | https://kenney.nl/assets/military-kit |
| Quaternius Low-Poly Military | CC0 | OBJ | https://quaternius.itch.io |
| OpenGameArt | varies | OBJ / FBX | https://opengameart.org |

---

## Controls

| Input | Action |
|---|---|
| **Mouse drag** | Orbit camera |
| **Mouse wheel** | Zoom |
| **Space** | Play / Pause |
| **1** | Slow motion (×0.25) |
| **2** | Real-time (×1) |
| **3** | Fast-forward (×4) |
| **4** | Rewind (×−1) |
| **F** | Follow / unfollow selected entity |
| **R** | Toggle Line / Ribbon trail mode |
| **Esc** | Quit |

---

## Command-line flags

```
debrief.exe [options]

  --demo            Run built-in flight demo (no UDP required)
  --port  N         UDP listen port (default 5555)
  --addr  IP        Bind address   (default 0.0.0.0)
  --width  N        Window width   (default 1600)
  --height N        Window height  (default 900)
```

---

## Features

- **Live 3D rendering** — entities rendered with custom models or built-in shapes
- **Smooth interpolation** — cubic Hermite splines between telemetry keyframes; fluid at 60 FPS even with 5–10 Hz update rates
- **Trails** — line or camera-facing ribbon mode
- **VCR playback** — play, pause, fast-forward (×0.25 to ×8), rewind, scrub
- **Timeline** — ImPlot-powered scrubber with altitude chart for the selected entity
- **Dashcam buffer** — "Save Last N Seconds" saves a rolling window to disk instantly
- **Custom binary format** (`.dbr`) — sparse-indexed for O(log N) seek
- **CSV import** — flexible column-mapping for importing logs from flight sims or DCS
- **Multi-source ready** — `source_id` field supports multiple simultaneous UDP sources

---

## Architecture

```
UDP Recv Thread ──SPSC queue──► Main Thread
                                 ├─ lat/lon → ENU conversion
                                 ├─ Euler → quaternion
                                 ├─ Flecs ECS (interpolation + trail systems)
                                 ├─ TelemetryStore (dashcam ring buffer)
                                 ├─ Recorder (async disk I/O thread)
                                 └─ Raylib + ImGui render loop
```

**Stack:** C++23 · CMake · [Raylib](https://raylib.com) · [Flecs 4](https://github.com/SanderMertens/flecs) · [Dear ImGui](https://github.com/ocornut/imgui) · [ImPlot](https://github.com/epezent/implot) · [Assimp](https://github.com/assimp/assimp) · [rlImGui](https://github.com/raylib-extras/rlImGui)

---

## License

MIT
