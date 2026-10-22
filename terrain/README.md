# tdem — Turkey & region terrain heights

Compact, multi-resolution terrain-height (DEM) data for Turkey and its
neighbours, plus a small dependency-free C++17 reader. This folder is
self-contained: copy it into any project (or `add_subdirectory` it) — nothing
here depends on AfterAction.

```
terrain/
├── data/terrain.tdem     all layers in one file (49 MB)
├── include/tdem/tdem.hpp C++ API
├── src/tdem.cpp          reader implementation
├── third_party/sinfl.h   public-domain DEFLATE decoder (vendored)
├── tools/                build_pack.py + regions.json (data builder), tdem_cli.cpp
├── tests/                C++ test + reference vectors
└── FORMAT.md             byte-level file format
```

## Coverage

| Layer | Area | Grid | Size |
|---|---|---|---|
| `region_30s` | 29–46 N, 19–52 E: Greece, Bulgaria, Cyprus, Georgia, Armenia, Azerbaijan, Syria, Iraq, Lebanon, western Iran … | 30″ (~740–930 m) | 4.4 MB |
| `turkey_7.5s` | 34.5–43.5 N, 24.0–46.0 E: Turkey plus a ~100–150 km band around it (Cyprus, northern Syria and Iraq, Armenia, Georgia, Nakhchivan, southern Bulgaria, the Aegean islands) | 7.5″ (~180–230 m) | 22.2 MB |
| `ankara_1s` | 39.4–40.6 N, 32.0–33.6 E (Esenboğa, Etimesgut, Güvercinlik inside) | 1″ (~24×31 m) | 15.3 MB |
| `akinci_5m` | Akıncı (LTAE), 16 × 16 km | **5 m** | 2.4 MB |
| `incirlik_1s` / `incirlik_5m` | İncirlik (LTAG), 40 km / 12 km | 1″ / **5 m** | 1.1 + 1.3 MB |
| `cigli_1s` / `cigli_5m` | İzmir Çiğli (LTBL), 40 km / 12 km | 1″ / **5 m** | 1.0 + 1.2 MB |

Heights are **metres above mean sea level** (EGM2008) — the same reference as
aircraft MSL altitude, so `aircraft_alt − height` is height above ground.
Coarse ↔ fine layers fade into each other at their borders (no steps).
Height steps: 0.1 m (5 m layers), 0.25 m (1″ layers), 1 m (Turkey and region).

Runways at LTAE, LTAG, LTBL, LTAC, LTAD and LTAB are flattened along a smoothed
longitudinal profile; DEM threshold heights match published elevations within
~1–3 m.

### What the 5 m layers are (and aren't)

The best freely redistributable DEM for Turkey is Copernicus GLO-30 (1″, ~30 m).
The 5 m layers are that data resampled with a cubic spline, plus crisp runway
flattening: smooth surfaces and correctly levelled runways at close range, but
no real detail finer than ~30 m. Copernicus is a surface model (DSM), so
buildings, hangars and tree stands appear as bumps. If higher-resolution survey
data becomes available, add it as a layer in `regions.json` and rebuild — the
format and the reader don't change.

## Usage

```cmake
add_subdirectory(terrain)
target_link_libraries(myapp PRIVATE tdem::tdem)
```

```cpp
#include <tdem/tdem.hpp>

tdem::Database db;
std::string err;
if (!db.add_pack("terrain/data/terrain.tdem", &err)) { /* report err */ }

std::optional<float> h = db.height(40.0789, 32.5656);   // ~835 m (Akıncı)
float h2 = db.height_or(lat, lon, 0.0f);                 // with fallback
float far = db.height(lat, lon, 150.0f).value_or(0);     // skip <150 m layers (distant LOD)

std::vector<float> grid(129 * 129);                      // mesh building
db.sample_grid(north, west, dlat, dlon, 129, 129, grid.data());
```

`Database` is thread-safe for queries. Decoded tiles are cached (LRU, 256 MB
default — `set_cache_limit`). A random query costs ~0.4 µs once tiles are hot.
`add_directory()` loads every `*.tdem` in a folder, so extra packs can be
dropped in next to `terrain.tdem` later.

## Command-line tool and tests

```sh
cmake -S terrain -B build-tdem && cmake --build build-tdem
build-tdem/tdem info  terrain/data
build-tdem/tdem query terrain/data 40.0789 32.5656 33.51 36.29
ctest --test-dir build-tdem
```

## Rebuilding the data

Needs Python with `numpy scipy rasterio` and network access once (Copernicus
tiles, ~2 GB, cached in `--cache`):

```sh
cd terrain/tools
python build_pack.py --cache ~/dem_cache      # writes ../data/terrain.tdem and ../tests/vectors.json
```

Edit `regions.json` to add areas: a layer is either a `bbox` or an airfield
`center` (ICAO, must have runways in `runways_tr.csv`) with `half_km`, at
`spacing_arcsec` (aligned to Copernicus samples) or `spacing_m` (resampled).

## Licences / attribution

See [ATTRIBUTION.md](ATTRIBUTION.md). Code in this folder: same licence as the
repository. `third_party/sinfl.h`: MIT / public domain.
