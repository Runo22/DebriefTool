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

Send packets to **UDP port 5555** (configurable with `--port`, or in-app under
**Settings → Network**).

> **Full reference:** see **[`docs/PROTOCOL.md`](docs/PROTOCOL.md)** for every field,
> its units, valid ranges, and ready-to-copy Python/C senders.

### Wire layout

```
[ BatchHeader   10 bytes ]
[ EntityUpdate 103 bytes ] × count   (max 14 per packet, fits standard MTU)
```

### Structs (C, little-endian, no padding)

```c
#pragma pack(push, 1)

struct BatchHeader {
    uint8_t  magic[4];     // must be {'D','B','F','1'}
    uint8_t  count;        // number of EntityUpdate records following (1–14)
    uint8_t  source_id;    // 0 if you only have one data source
    uint32_t sequence;     // increment each packet (drop detection; wraps cleanly)
};

struct EntityUpdate {
    uint32_t id;           // stable unique ID for this unit — never change it
    uint16_t type;         // see entity types below
    uint8_t  health;       // 255 = full, 0 = destroyed
    char     callsign[32]; // null-padded UTF-8, up to 31 chars, e.g. "VIPER01\0..."

    double   lat;          // latitude  (decimal degrees, e.g.  36.8500)
    double   lon;          // longitude (decimal degrees, e.g.  35.1200)
    double   alt;          // altitude  (metres above sea level)

    double   phi;          // roll    (degrees, –180 to +180)
    double   theta;        // pitch   (degrees, –90 to +90, positive = nose up)
    double   psi;          // heading (degrees, 0 = North, 90 = East, clockwise)

    double   speed;        // airspeed (m/s, 0 = unknown)
    uint64_t time_ns;      // UNIX nanoseconds; 0 = server assigns current time
};

#pragma pack(pop)
```

All scalar fields are now **doubles** (previously some were 32-bit floats), and the
**sequence counter is 32-bit** so long sessions no longer wrap after ~18 minutes.

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

### C++ sender example

```cpp
#include <cstdint>
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

#pragma pack(push, 1)
struct BatchHeader {
    uint8_t  magic[4];     // {'D','B','F','1'}
    uint8_t  count;        // number of EntityUpdate records following (1–14)
    uint8_t  source_id;    // 0 if you only have one source
    uint32_t sequence;     // increment each packet
};
struct EntityUpdate {
    uint32_t id;           // stable unique ID — never change it
    uint16_t type;         // 1 = jet (see entity types)
    uint8_t  health;       // 255 = full, 0 = destroyed
    char     callsign[32]; // null-padded, up to 31 chars
    double   lat, lon, alt;   // deg, deg, metres MSL
    double   phi, theta, psi; // roll, pitch, heading (deg)
    double   speed;           // m/s (0 = unknown)
    uint64_t time_ns;         // UNIX ns, or 0 to let the server stamp it
};
#pragma pack(pop)
static_assert(sizeof(BatchHeader)  == 10);
static_assert(sizeof(EntityUpdate) == 103);

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
    e.lat = 36.85; e.lon = 35.12; e.alt = 3000.0;  // metres MSL
    e.phi = 0.0;   e.theta = 5.0;  e.psi = 270.0;   // heading West, 5° nose up
    e.speed = 220.0;                                 // m/s
    e.time_ns = 0;                                   // server timestamps it

    BatchHeader h{ {'D','B','F','1'}, 1, 0, seq++ };

    // Header + entities packed contiguously, sent as one datagram.
    uint8_t buf[sizeof(h) + sizeof(e)];
    std::memcpy(buf,             &h, sizeof(h));
    std::memcpy(buf + sizeof(h), &e, sizeof(e));
    ::sendto(sock, reinterpret_cast<const char*>(buf), sizeof(buf), 0,
             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
}
```

See **[`docs/PROTOCOL.md`](docs/PROTOCOL.md)** for a multi-entity C++ sender and
the full field reference. A ready-to-run Python demo with a scripted flight
scenario is at [`scripts/test_sender.py`](scripts/test_sender.py):

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
