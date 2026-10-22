# AfterAction

Real-time 3D tactical telemetry visualization and post-action review (AAR) tool.

Receives spatial data over UDP, renders it in 3D, records everything, and provides full VCR-style playback — scrub, rewind, fast-forward through any engagement.

![Demo screenshot placeholder](assets/screenshot_placeholder.png)

---

## Quick start (no UDP source needed)

```bat
git clone <this-repo>          :: all dependencies are vendored — works offline
scripts\bootstrap.bat
cmake --build build --config Release --parallel
build\Release\afteraction.exe --demo
```

The `--demo` flag runs a built-in scripted scenario at **Akıncı Air Base (LTAE)** —
a jet taking off from runway 03, two more jets, a missile shot, an air-defence
site, a helicopter and a convoy following the real terrain — so you can evaluate
the tool without any external data source.

---

## Building

All third-party libraries are vendored in [`third_party/`](third_party/README.md) —
no submodules and no downloads, so the repository builds on an offline machine.

### Windows (primary) — CMake presets (Ninja + cl.exe, x64)

Requirements: **Visual Studio 2022** Build Tools, **CMake 3.25+**, **Ninja**, **Git**.
From an **x64 Native Tools Command Prompt for VS**:

```bat
cmake --preset windows-release      :: or windows-debug
cmake --build --preset windows-release
```

Output goes to `build\windows-release\bin\AfterAction.exe` (with `assets\`
alongside). Release builds have **no console window** — runtime logs go to
`bin\logs\AfterAction.log`. (You can still use `scripts\bootstrap.bat` + a VS
solution if you prefer.)

### Linux

```sh
cmake --preset linux-release        # or linux-debug
cmake --build --preset linux-release
./build/linux-release/bin/AfterAction --demo
```

### macOS

Requirements: **Xcode Command Line Tools**, **CMake 3.25+**.

```sh
bash scripts/bootstrap.sh
```

### Install

```sh
cmake --install <build-dir>
```

Installs just **AfterAction** + `hangar` + `assets/` (plus the required runtime
DLLs on Windows) under `<prefix>/bin` — no third-party libs/headers. By default
the prefix is a local **`install/`** folder next to the source, so it needs **no
admin/root** (not `C:\Program Files`). Override with `--prefix <dir>` or
`-DCMAKE_INSTALL_PREFIX=<dir>` at configure time. Recordings, config, and logs
are written next to the executable at runtime.

---

## UDP Packet Format

Send packets to **UDP port 22522** (configurable with `--port`, or in-app under
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

Built-in models are at real size and loosely modelled on Turkish platforms
(override any of them with your own glTF/FBX/OBJ in Settings → Models):

| Value | Meaning | Built-in model |
|---|---|---|
| 0 | Unknown | Diamond marker |
| 1 | Jet aircraft | KAAN-style fighter |
| 2 | Missile | Air-to-air missile |
| 3 | AAA / gun | KORKUT-style tracked twin-gun vehicle |
| 4 | Ground target | 6×6 armoured vehicle |
| 5 | Helicopter | T129 ATAK-style (spinning rotors) |
| 6 | Ship | MİLGEM (Ada-class) corvette |
| 7 | UAV | ANKA-style MALE UAV (spinning propeller) |

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
    dst.sin_port   = htons(22522);
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
python scripts/test_sender.py --host 127.0.0.1 --port 22522 --hz 10
```

---

## Terrain

AfterAction renders **real terrain for Turkey and its neighbours** from the height data in
[`terrain/`](terrain/) (copied next to the executable at build time):

| Area | Grid |
|---|---|
| Neighbours: Greece, Bulgaria, Cyprus, Georgia, Armenia, Azerbaijan, Syria, Iraq, … (29–46 N, 19–52 E) | 30″ (~740–930 m) |
| Turkey + ~100–150 km around it (Cyprus, northern Syria/Iraq, Armenia, Georgia, Aegean islands) | 7.5″ (~180–230 m) |
| Ankara region (Esenboğa, Etimesgut, Güvercinlik) | 1″ (~24–31 m) |
| İncirlik and İzmir Çiğli surroundings (40 km) | 1″ |
| **Akıncı** (16 km), **İncirlik**, **Çiğli** (12 km) | **5 m** |

- Heights are metres **MSL**, the same reference as the protocol's `alt`, so the
  inspector shows **AGL** directly. Runways are levelled and drawn with markings.
- The scene starts at the **home position** (Akıncı by default; *View → Set home*
  or *Settings → Terrain*). Live data re-centres the scene only when it arrives
  more than 250 km away. Recordings remember their origin.
- *View → Terrain*: tactical grid, shaded, shaded + grid, or contour lines.
  *Vertical scale* stretches terrain **and** tracks together.
- The top bar shows the latitude/longitude/elevation under the mouse; the
  minimap is a hillshade of the area around the tracks (click to look there).

The terrain library (`tdem`) and its data are self-contained and reusable in
other projects — see [`terrain/README.md`](terrain/README.md).

---

## Adding 3D Models

The app ships with coloured procedural shapes as built-in defaults. To load proper models:

1. Drop an **FBX**, **OBJ**, or **glTF/GLB** file into `assets/models/`
2. Bind it to an entity type in **Settings → Models** (applies live; *Save
   manifest* writes `assets/models.yaml`), or edit `assets/models.yaml` directly.
   Preview scale/rotation with the `hangar` tool. Details:
   [`docs/ROADMAP_MODELS.md`](docs/ROADMAP_MODELS.md).

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
| **Left click** | Select track (double-click in the list = focus) |
| **Right drag** | Orbit camera |
| **Middle drag** / **WASD** | Pan (Shift = faster) |
| **Mouse wheel** | Zoom |
| **Space** | Play / Pause |
| **1** | Slow motion (×0.25) |
| **2** | Real-time (×1) |
| **3** | Fast-forward (×4) |
| **4** | Rewind (×−1) |
| **F** | Focus / unfocus selected track |
| **G** | Chase camera on selected track |
| **H** | Fly to the home position |
| **R** | Toggle Line / Ribbon trail mode |
| **Esc** | Close Settings, then clear the selection |
| **Ctrl+Q** | Quit |

---

## Settings

The gear button opens **Settings** (General · Display · Camera · Terrain ·
Network · Recording · Models · Shortcuts). Changes apply immediately and are
saved automatically to `afteraction_config.yaml` next to the executable.

- **General** — interface scale (75–200 %, for high-DPI screens) and units:
  altitude ft / m, speed kt / km/h / m/s, coordinates DMS / DDM / decimal.
- **Network** — bind address and port with a live listener status; a
  `--port` / `--addr` on the command line overrides them for that run.
- **Recording** — dashcam length and the recordings folder.

---

## Command-line flags

```
afteraction.exe [options]

  --demo            Run built-in flight demo (no UDP required)
  --port  N         UDP listen port (default 22522)
  --addr  IP        Bind address   (default 0.0.0.0)
  --width  N        Window width   (default 1600)
  --height N        Window height  (default 900)
  --terrain DIR     Terrain packs  (default <exe dir>/terrain)
  --open FILE       Open a .aar recording or .csv log at startup
                    (a bare file argument works too, e.g. from the shell)
```

---

## Features

- **Live 3D rendering** — entities rendered with custom models or built-in shapes
- **Real terrain** — Turkey + neighbours DEM with 5 m detail at Akıncı, İncirlik and Çiğli; runway overlays, contours, AGL
- **Inspector** — position (DMS), MSL / AGL / ground elevation, speed, vertical speed, heading / pitch / roll
- **Smooth interpolation** — cubic Hermite splines between telemetry keyframes; fluid at 60 FPS even with 5–10 Hz update rates
- **Trails** — line or camera-facing ribbon mode
- **VCR playback** — play, pause, fast-forward (×0.25 to ×8), rewind, scrub
- **Timeline** — scrubber plus altitude / speed chart with the terrain profile under the selected track
- **Dashcam buffer** — "Save Last N Seconds" saves a rolling window to disk instantly
- **Custom binary format** (`.aar`) — sparse-indexed for O(log N) seek
- **CSV import** — flexible column-mapping for importing logs from flight sims or DCS
- **Multi-source ready** — `source_id` field supports multiple simultaneous UDP sources

---

## Architecture

```
UDP Recv Thread ──SPSC queue──► Main Thread
                                 ├─ lat/lon → world (GeoFrame, shared with terrain)
                                 ├─ Euler → quaternion
                                 ├─ Flecs ECS (interpolation + trail systems)
                                 ├─ TelemetryStore (dashcam ring buffer)
                                 ├─ Recorder (async disk I/O thread)
                                 └─ Raylib + ImGui render loop
Terrain workers ──meshes──────►  TerrainSystem (quadtree LOD from terrain.tdem)
```

**Stack:** C++23 · CMake · [Raylib](https://raylib.com) · [Flecs 4](https://github.com/SanderMertens/flecs) · [Dear ImGui](https://github.com/ocornut/imgui) · [ImPlot](https://github.com/epezent/implot) · [Assimp](https://github.com/assimp/assimp) · [rlImGui](https://github.com/raylib-extras/rlImGui)

---

## License

MIT
