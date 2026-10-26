#!/usr/bin/env python3
"""
build_pack.py — build TDEM terrain-height packs from Copernicus DEM tiles.

    pip install numpy scipy rasterio
    python build_pack.py --cache ./dem_cache            # -> ../data/terrain.tdem

Steps per layer:
  1. Mosaic the needed 1x1 degree Copernicus tiles (downloaded into --cache on
     first use) for a band of output rows, plus a margin.
  2. Resample to the layer grid: cubic spline when upsampling, box prefilter +
     bilinear when downsampling, exact copy when the grids coincide.
  3. Flatten runways (geometry from OurAirports, longitudinal profile from the
     30 m DEM) so runways are level, consistently in every layer.
  4. Quantise, tile and compress (see ../FORMAT.md) and write the pack.

All layers listed in regions.json go into one file.

Also writes ../tests/vectors.json with exact grid-node samples so the C++
reader can be checked against this reference.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import time
import struct
import urllib.request
import zlib
from dataclasses import dataclass

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))

FT = 0.3048


# ── TDEM writer (format: ../FORMAT.md; the C++ reader is ../src/tdem.cpp) ────

@dataclass
class Layer:
    name: str
    north: float          # latitude of row 0 (deg)
    west: float           # longitude of column 0 (deg)
    dlat: float           # row spacing (deg, > 0; latitude decreases with row)
    dlon: float           # column spacing (deg, > 0)
    heights: np.ndarray   # float32 [rows, cols], metres MSL
    quant_step: float = 0.25
    blend_m: float = 0.0  # fade-out distance at the layer edge toward coarser layers
    resolution_m: float = 0.0
    tile_size: int = 256

    @property
    def rows(self) -> int:
        return self.heights.shape[0]

    @property
    def cols(self) -> int:
        return self.heights.shape[1]

    def tiles_xy(self) -> tuple[int, int]:
        T = self.tile_size
        return (self.cols + T - 1) // T, (self.rows + T - 1) // T


_FILE_HDR = struct.Struct("<4sIIIQ40s")        # 64 B
_LAYER_HDR = struct.Struct("<32s4dQ4I5f20x")   # 128 B
_TILE = struct.Struct("<QIiff")                # 24 B


def encode_tile(v: np.ndarray) -> bytes:
    """v: uint16 [T, T] quantised offsets from the tile base. Planar predictor
    (left + up - upleft, mod 2^16) -> zigzag -> lo/hi byte planes -> raw deflate."""
    a = v.astype(np.int64)
    pred = np.zeros_like(a)
    pred[0, 1:] = a[0, :-1]
    pred[1:, 0] = a[:-1, 0]
    pred[1:, 1:] = a[1:, :-1] + a[:-1, 1:] - a[:-1, :-1]
    res = ((a - pred) & 0xFFFF).astype(np.uint16).view(np.int16).astype(np.int32)
    zz = (((res << 1) ^ (res >> 15)) & 0xFFFF).astype(np.uint16)
    planes = np.concatenate([(zz & 0xFF).astype(np.uint8).ravel(),
                             (zz >> 8).astype(np.uint8).ravel()])
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    return c.compress(planes.tobytes()) + c.flush()


def write_pack(path: str, layers: list[Layer], description: str = "") -> dict:
    """Write layers to a .tdem file. Returns per-layer size statistics."""
    stats, blobs, tables = {}, [], []
    header_bytes = _FILE_HDR.size + _LAYER_HDR.size * len(layers)
    offset = header_bytes + sum(_TILE.size * l.tiles_xy()[0] * l.tiles_xy()[1] for l in layers)
    for layer in layers:
        T = layer.tile_size
        tx_n, ty_n = layer.tiles_xy()
        h = layer.heights
        # Pad to whole tiles by edge replication (compresses to almost nothing).
        padded = np.pad(h, ((0, ty_n * T - h.shape[0]), (0, tx_n * T - h.shape[1])), mode="edge")
        table, layer_bytes, absent = [], 0, 0
        for ty in range(ty_n):
            for tx in range(tx_n):
                t = padded[ty * T:(ty + 1) * T, tx * T:(tx + 1) * T]
                hmin, hmax = float(t.min()), float(t.max())
                if hmin == 0.0 and hmax == 0.0:
                    table.append((0, 0, 0, 0.0, 0.0))            # absent = sea (0 m)
                    absent += 1
                    continue
                qi = np.rint(t.astype(np.float64) / layer.quant_step).astype(np.int64)
                base = int(qi.min())
                if int(qi.max()) - base > 0xFFFF:
                    raise ValueError(f"{layer.name}: tile ({tx},{ty}) too tall for quant_step")
                blob = encode_tile((qi - base).astype(np.uint16))
                table.append((offset, len(blob), base, hmin, hmax))
                blobs.append(blob)
                offset += len(blob)
                layer_bytes += len(blob)
        tables.append(table)
        stats[layer.name] = dict(rows=layer.rows, cols=layer.cols, tiles=tx_n * ty_n,
                                 absent=absent, bytes=layer_bytes)

    with open(path, "wb") as f:
        f.write(_FILE_HDR.pack(b"TDEM", 1, len(layers), _FILE_HDR.size, _FILE_HDR.size,
                               description.encode()[:40]))
        table_off = header_bytes
        for layer, table in zip(layers, tables):
            f.write(_LAYER_HDR.pack(layer.name.encode()[:32], layer.north, layer.west,
                                    layer.dlat, layer.dlon, table_off,
                                    layer.rows, layer.cols, layer.tile_size, 0,
                                    layer.quant_step, layer.blend_m, layer.resolution_m,
                                    float(layer.heights.min()), float(layer.heights.max())))
            table_off += _TILE.size * len(table)
        for table in tables:
            for e in table:
                f.write(_TILE.pack(*e))
        for b in blobs:
            f.write(b)
    return stats


# ── Geodesy helpers ──────────────────────────────────────────────────────────

def m_per_deg_lat(lat: float) -> float:
    p = math.radians(lat)
    return 111132.92 - 559.82 * math.cos(2 * p) + 1.175 * math.cos(4 * p)


def m_per_deg_lon(lat: float) -> float:
    p = math.radians(lat)
    return 111412.84 * math.cos(p) - 93.5 * math.cos(3 * p)


# ── Copernicus source mosaic ─────────────────────────────────────────────────

class Source:
    def __init__(self, key: str, cfg: dict, cache: str, download: bool):
        self.key = key
        self.arcsec = cfg["arcsec"]
        self.d = self.arcsec / 3600.0
        self.per_deg = 3600 // self.arcsec
        self.name_fmt = cfg["name"]
        self.url_fmt = cfg["url"]
        self.dir = os.path.join(cache, key)
        self.download = download
        os.makedirs(self.dir, exist_ok=True)
        self._missing: set = set()

    def _tile_path(self, lat: int, lon: int) -> str | None:
        if (lat, lon) in self._missing:
            return None
        name = self.name_fmt.format(lat=lat, lon=lon)
        path = os.path.join(self.dir, name + ".tif")
        if os.path.exists(path):
            return path
        if not self.download:
            self._missing.add((lat, lon))
            return None
        url = self.url_fmt.format(name=name)
        try:
            print(f"    downloading {name}", flush=True)
            urllib.request.urlretrieve(url, path + ".part")
            os.replace(path + ".part", path)
            return path
        except Exception as e:  # ocean tiles do not exist in the bucket
            print(f"    (no tile {name}: {e}) -> treated as sea", flush=True)
            self._missing.add((lat, lon))
            return None

    def window(self, south: float, west: float, north: float, east: float):
        """Return (array, lat_of_row0, lon_of_col0) of point samples covering the
        box. Samples sit at integer multiples of the source spacing."""
        import rasterio
        n = self.per_deg
        r0 = math.floor((90.0 - north) * n)          # global row index from 90N
        r1 = math.ceil((90.0 - south) * n)
        c0 = math.floor((west + 180.0) * n)          # global col index from 180W
        c1 = math.ceil((east + 180.0) * n)
        out = np.zeros((r1 - r0 + 1, c1 - c0 + 1), dtype=np.float32)
        for lat in range(math.floor(south), math.floor(north) + 1):
            for lon in range(math.floor(west), math.floor(east) + 1):
                path = self._tile_path(lat, lon)
                if path is None:
                    continue
                # Tile (lat,lon): row 0 is at lat+1, n rows; col 0 at lon, n cols.
                tr0 = (90 - (lat + 1)) * n
                tc0 = (lon + 180) * n
                a0, a1 = max(r0, tr0), min(r1, tr0 + n - 1)
                b0, b1 = max(c0, tc0), min(c1, tc0 + n - 1)
                if a0 > a1 or b0 > b1:
                    continue
                with rasterio.open(path) as ds:
                    assert ds.width == n and ds.height == n, (path, ds.width)
                    data = ds.read(1, window=((a0 - tr0, a1 - tr0 + 1), (b0 - tc0, b1 - tc0 + 1)))
                out[a0 - r0:a1 - r0 + 1, b0 - c0:b1 - c0 + 1] = data
        lat0 = 90.0 - r0 / n
        lon0 = c0 / n - 180.0
        return out, lat0, lon0

    def sample(self, lats: np.ndarray, lons: np.ndarray) -> np.ndarray:
        """Bilinear sample at scattered points (used for runway profiles)."""
        pad = 2 * self.d
        arr, lat0, lon0 = self.window(lats.min() - pad, lons.min() - pad,
                                      lats.max() + pad, lons.max() + pad)
        y = (lat0 - lats) / self.d
        x = (lons - lon0) / self.d
        return ndimage.map_coordinates(arr, [y, x], order=1, mode="nearest")


# ── Runways ──────────────────────────────────────────────────────────────────

class Runway:
    SHOULDER_M = 30.0      # flat beyond the paved half-width
    LAT_BLEND_M = 120.0    # lateral fade back to the DEM
    OVERRUN_M = 60.0       # flat beyond each threshold
    END_BLEND_M = 150.0

    def __init__(self, row: dict, profile_src: Source):
        self.airfield = row["airport_ident"]
        self.ident = f'{row["le_ident"]}/{row["he_ident"]}'
        self.le = (float(row["le_latitude_deg"]), float(row["le_longitude_deg"]))
        self.he = (float(row["he_latitude_deg"]), float(row["he_longitude_deg"]))
        self.width = float(row["width_ft"] or 148) * FT
        self.pub_elev = (float(row["le_elevation_ft"]) * FT if row["le_elevation_ft"] else None,
                         float(row["he_elevation_ft"]) * FT if row["he_elevation_ft"] else None)
        self.lat_m = 0.5 * (self.le[0] + self.he[0])
        self.lon_m = 0.5 * (self.le[1] + self.he[1])
        self.ky = m_per_deg_lat(self.lat_m)
        self.kx = m_per_deg_lon(self.lat_m)
        ex = (self.he[1] - self.le[1]) * self.kx
        ey = (self.he[0] - self.le[0]) * self.ky
        self.length = math.hypot(ex, ey)
        self.ux, self.uy = ex / self.length, ey / self.length
        self._build_profile(profile_src)

    def _local(self, lat, lon):
        x = (lon - self.le[1]) * self.kx
        y = (lat - self.le[0]) * self.ky
        t = x * self.ux + y * self.uy             # along-track from LE threshold
        d = np.abs(-x * self.uy + y * self.ux)    # cross-track distance
        return t, d

    def _build_profile(self, src: Source):
        step = 10.0
        t = np.arange(-300.0, self.length + 300.0 + step, step)
        x = t * self.ux
        y = t * self.uy
        lats = self.le[0] + y / self.ky
        lons = self.le[1] + x / self.kx
        h = src.sample(lats, lons).astype(np.float64)
        # Robust smoothing: median (kills isolated DSM spikes) then Gaussian.
        h = ndimage.median_filter(h, size=31, mode="nearest")
        h = ndimage.gaussian_filter1d(h, sigma=10, mode="nearest")
        self.prof_t = t
        self.prof_h = h

    def bbox(self):
        reach = self.width / 2 + self.SHOULDER_M + self.LAT_BLEND_M + self.OVERRUN_M + self.END_BLEND_M
        dlat = reach / self.ky
        dlon = reach / self.kx
        return (min(self.le[0], self.he[0]) - dlat, min(self.le[1], self.he[1]) - dlon,
                max(self.le[0], self.he[0]) + dlat, max(self.le[1], self.he[1]) + dlon)

    def threshold_heights(self):
        return (float(np.interp(0.0, self.prof_t, self.prof_h)),
                float(np.interp(self.length, self.prof_t, self.prof_h)))

    def burn(self, h: np.ndarray, lats: np.ndarray, lons: np.ndarray):
        """Flatten in place. h[rows, cols]; lats[rows]; lons[cols]."""
        s, w, n, e = self.bbox()
        ri = np.nonzero((lats >= s) & (lats <= n))[0]
        ci = np.nonzero((lons >= w) & (lons <= e))[0]
        if ri.size == 0 or ci.size == 0:
            return
        LA, LO = np.meshgrid(lats[ri], lons[ci], indexing="ij")
        t, d = self._local(LA, LO)
        hw = self.width / 2 + self.SHOULDER_M
        w_lat = _smoothstep(1.0 - (d - hw) / self.LAT_BLEND_M)
        over = np.maximum(-self.OVERRUN_M - t, t - (self.length + self.OVERRUN_M))
        w_end = _smoothstep(1.0 - over / self.END_BLEND_M)
        wgt = w_lat * w_end
        flat = np.interp(np.clip(t, 0.0, self.length), self.prof_t, self.prof_h)
        sub = h[np.ix_(ri, ci)]
        h[np.ix_(ri, ci)] = (wgt * flat + (1.0 - wgt) * sub).astype(h.dtype)


def _smoothstep(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3.0 - 2.0 * x)


def load_runways(cfg: dict, src: Source) -> tuple[list[Runway], dict]:
    wanted = set(cfg["airfields"])
    runways, centers = [], {}
    with open(os.path.join(HERE, "runways_tr.csv"), encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row["airport_ident"] not in wanted or row["closed"] == "1":
                continue
            if not row["le_latitude_deg"] or not row["he_latitude_deg"]:
                continue
            runways.append(Runway(row, src))
    for icao in wanted:
        rs = [r for r in runways if r.airfield == icao]
        if not rs:
            raise SystemExit(f"no runway geometry for {icao}")
        centers[icao] = (sum(r.lat_m for r in rs) / len(rs), sum(r.lon_m for r in rs) / len(rs))
    return runways, centers


# ── Layer building ───────────────────────────────────────────────────────────

def layer_grid(spec: dict, centers: dict):
    """Return (north, west, dlat, dlon, rows, cols, resolution_m)."""
    if "bbox" in spec:
        s, w, n, e = spec["bbox"]
    else:
        clat, clon = centers[spec["center"]]
        half = spec["half_km"] * 1000.0
        s, n = clat - half / m_per_deg_lat(clat), clat + half / m_per_deg_lat(clat)
        w, e = clon - half / m_per_deg_lon(clat), clon + half / m_per_deg_lon(clat)
    mid = 0.5 * (s + n)
    if "spacing_arcsec" in spec:
        dlat = dlon = spec["spacing_arcsec"] / 3600.0
        # Snap to the arc-second lattice so 1" layers coincide with Copernicus samples.
        north = math.ceil(n / dlat - 1e-9) * dlat
        west = math.floor(w / dlon + 1e-9) * dlon
    else:
        m = spec["spacing_m"]
        dlat = m / m_per_deg_lat(mid)
        dlon = m / m_per_deg_lon(mid)
        north, west = n, w
    rows = int(math.floor((north - s) / dlat + 1e-9)) + 1
    cols = int(math.floor((e - west) / dlon + 1e-9)) + 1
    res_m = min(dlat * m_per_deg_lat(mid), dlon * m_per_deg_lon(mid))
    return north, west, dlat, dlon, rows, cols, res_m


def build_layer(spec: dict, sources: dict, runways: list, centers: dict) -> Layer:
    north, west, dlat, dlon, rows, cols, res_m = layer_grid(spec, centers)
    src: Source = sources[spec["source"]]
    print(f"  layer {spec['name']}: {rows}x{cols} @ {res_m:.1f} m "
          f"(N{north:.4f} W{west:.4f})", flush=True)
    out = np.zeros((rows, cols), dtype=np.float32)
    lons = west + np.arange(cols) * dlon
    ratio = (dlat / src.d, dlon / src.d)           # >1 downsample, <1 upsample
    band = 512
    margin = 6 * max(src.d, dlat, dlon)
    for r0 in range(0, rows, band):
        r1 = min(rows, r0 + band)
        lats = north - np.arange(r0, r1) * dlat
        arr, lat0, lon0 = src.window(lats.min() - margin, west - margin,
                                     lats.max() + margin, lons.max() + margin)
        y = (lat0 - lats) / src.d
        x = (lons - lon0) / src.d
        if ratio[0] > 1.01 or ratio[1] > 1.01:
            arr = ndimage.uniform_filter(arr, size=(max(1, round(ratio[0])), max(1, round(ratio[1]))),
                                         mode="nearest")
            order = 1
        elif ratio[0] > 0.99 and ratio[1] > 0.99:
            order = 1                                   # grids coincide: exact copy
        else:
            order = 3
        Y, X = np.meshgrid(y, x, indexing="ij")
        out[r0:r1] = ndimage.map_coordinates(arr, [Y, X], order=order, mode="nearest")
    lats_all = north - np.arange(rows) * dlat
    for rw in runways:
        rw.burn(out, lats_all, lons)
    return Layer(name=spec["name"], north=north, west=west, dlat=dlat, dlon=dlon,
                      heights=out, quant_step=spec["quant_m"], blend_m=spec["blend_m"],
                      resolution_m=res_m)


def test_vectors(layer: Layer, rng: np.random.Generator, n: int = 24) -> list:
    """Exact grid-node samples (quantised exactly as the encoder does)."""
    v = []
    for _ in range(n):
        r = int(rng.integers(0, layer.rows))
        c = int(rng.integers(0, layer.cols))
        q = float(np.rint(np.float64(layer.heights[r, c]) / layer.quant_step)) * layer.quant_step
        if layer.heights[r, c] == 0.0:
            q = 0.0
        v.append(dict(layer=layer.name, lat=layer.north - r * layer.dlat,
                      lon=layer.west + c * layer.dlon, h=q))
    return v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default=os.path.join(HERE, "regions.json"))
    ap.add_argument("--cache", required=True, help="directory for downloaded Copernicus tiles")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "data"))
    ap.add_argument("--only", action="append", help="build only these pack files")
    ap.add_argument("--no-download", action="store_true")
    ap.add_argument("--vectors", default=os.path.join(HERE, "..", "tests", "vectors.json"))
    args = ap.parse_args()

    cfg = json.load(open(args.config, encoding="utf-8"))
    sources = {k: Source(k, v, args.cache, not args.no_download) for k, v in cfg["sources"].items()}
    runways, centers = load_runways(cfg["runways"], sources[cfg["runways"]["profile_source"]])
    print("runways:")
    for r in runways:
        a, b = r.threshold_heights()
        pa, pb = r.pub_elev
        pub = f"published {pa:.1f}/{pb:.1f} m" if pa is not None and pb is not None else ""
        print(f"  {r.airfield} {r.ident:8s} L={r.length:6.0f} m W={r.width:4.0f} m  "
              f"DEM thresholds {a:.1f}/{b:.1f} m  {pub}")

    os.makedirs(args.out, exist_ok=True)
    rng = np.random.default_rng(1234)
    vectors = []
    if args.only and os.path.exists(args.vectors):
        vectors = json.load(open(args.vectors))["samples"]
    summary = []
    for pack in cfg["packs"]:
        if args.only and pack["file"] not in args.only:
            continue
        print(f"pack {pack['file']}", flush=True)
        t0 = time.time()
        layers = [build_layer(s, sources, runways, centers) for s in pack["layers"]]
        path = os.path.join(args.out, pack["file"])
        stats = write_pack(path, layers, pack.get("description", ""))
        names = {l.name for l in layers}
        vectors = [v for v in vectors if v["layer"] not in names]
        for l in layers:
            vectors += test_vectors(l, rng)
        for name, s in stats.items():
            summary.append((pack["file"], name, s))
            print(f"  {name}: {s['rows']}x{s['cols']}  tiles {s['tiles']} "
                  f"(absent {s['absent']})  {s['bytes'] / 1e6:.2f} MB", flush=True)
        print(f"  -> {path}: {os.path.getsize(path) / 1e6:.2f} MB in {time.time() - t0:.0f} s")

    os.makedirs(os.path.dirname(args.vectors), exist_ok=True)
    with open(args.vectors, "w") as f:
        json.dump({"samples": vectors}, f, indent=0)


if __name__ == "__main__":
    main()
