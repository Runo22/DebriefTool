"""
tdem.py — reader/writer for the TDEM terrain-height pack format (Python + numpy).

The format is specified in ../FORMAT.md. This module is the reference
implementation used by the build tool; the C++ library (../src/tdem.cpp) must
decode exactly the same bytes.

Heights are metres above mean sea level (EGM2008), on a regular WGS-84
latitude/longitude grid of point samples.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field

import numpy as np

MAGIC = b"TDEM"
VERSION = 1
FILE_HEADER_SIZE = 64
LAYER_HEADER_SIZE = 128
TILE_ENTRY_SIZE = 24

CODEC_DEFLATE_PLANAR = 1  # 2D predictor -> zigzag -> lo/hi byte planes -> raw deflate

_FILE_HDR = struct.Struct("<4sIIIQ40s")                 # 64 B
_LAYER_HDR = struct.Struct("<32s4dQ4I5f20x")            # 128 B
_TILE = struct.Struct("<QIiff")                         # 24 B
assert _FILE_HDR.size == FILE_HEADER_SIZE
assert _LAYER_HDR.size == LAYER_HEADER_SIZE
assert _TILE.size == TILE_ENTRY_SIZE


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
    flags: int = 0
    # Filled by the reader
    tiles: list = field(default_factory=list)

    @property
    def rows(self) -> int:
        return self.heights.shape[0]

    @property
    def cols(self) -> int:
        return self.heights.shape[1]


# ── Tile codec ────────────────────────────────────────────────────────────────

def _predict_residual(v: np.ndarray) -> np.ndarray:
    """uint16 tile -> uint16 residuals (mod 2^16) of the planar predictor."""
    a = v.astype(np.int64)
    pred = np.zeros_like(a)
    pred[0, 1:] = a[0, :-1]                                   # first row: left
    pred[1:, 0] = a[:-1, 0]                                   # first col: up
    pred[1:, 1:] = a[1:, :-1] + a[:-1, 1:] - a[:-1, :-1]      # left + up - upleft
    return ((a - pred) & 0xFFFF).astype(np.uint16)


def _unpredict(res: np.ndarray) -> np.ndarray:
    # The residual is the 2D mixed difference of v (zero outside the tile), so
    # v is simply its 2D prefix sum, mod 2^16.
    r = res.astype(np.int64)
    return (np.cumsum(np.cumsum(r, axis=0), axis=1) & 0xFFFF).astype(np.uint16)


def encode_tile(q: np.ndarray) -> bytes:
    """q: uint16 [T, T] quantised offsets from the tile base."""
    res = _predict_residual(q).view(np.int16).astype(np.int32)
    zz = ((res << 1) ^ (res >> 15)) & 0xFFFF                  # zigzag
    zz = zz.astype(np.uint16)
    planes = np.concatenate([(zz & 0xFF).astype(np.uint8).ravel(),
                             (zz >> 8).astype(np.uint8).ravel()])
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    return c.compress(planes.tobytes()) + c.flush()


def decode_tile(blob: bytes, tile_size: int) -> np.ndarray:
    raw = zlib.decompress(blob, -15)
    n = tile_size * tile_size
    p = np.frombuffer(raw, dtype=np.uint8)
    zz = p[:n].astype(np.uint32) | (p[n:2 * n].astype(np.uint32) << 8)
    res = ((zz >> 1) ^ (-(zz & 1).astype(np.int64) & 0xFFFF)).astype(np.uint16)
    return _unpredict(res.reshape(tile_size, tile_size))


# ── Writer ────────────────────────────────────────────────────────────────────

def write_pack(path: str, layers: list[Layer], description: str = "") -> dict:
    """Write layers to a .tdem file. Returns per-layer size statistics."""
    stats = {}
    header_bytes = FILE_HEADER_SIZE + LAYER_HEADER_SIZE * len(layers)
    blobs: list[bytes] = []
    tables: list[list[tuple]] = []
    offset = header_bytes + sum(
        TILE_ENTRY_SIZE * _tiles_xy(l)[0] * _tiles_xy(l)[1] for l in layers)

    for layer in layers:
        T = layer.tile_size
        tx_n, ty_n = _tiles_xy(layer)
        h = layer.heights
        # Pad to whole tiles by edge replication (compresses to almost nothing).
        padded = np.pad(h, ((0, ty_n * T - h.shape[0]), (0, tx_n * T - h.shape[1])), mode="edge")
        table = []
        layer_bytes = 0
        absent = 0
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
                span = int(qi.max()) - base
                if span > 0xFFFF:
                    raise ValueError(f"{layer.name}: tile ({tx},{ty}) spans {span} steps "
                                     f"(> 65535); increase quant_step")
                blob = encode_tile((qi - base).astype(np.uint16))
                table.append((offset, len(blob), base, hmin, hmax))
                blobs.append(blob)
                offset += len(blob)
                layer_bytes += len(blob)
        tables.append(table)
        stats[layer.name] = dict(rows=layer.rows, cols=layer.cols, tiles=tx_n * ty_n,
                                 absent=absent, bytes=layer_bytes)

    with open(path, "wb") as f:
        f.write(_FILE_HDR.pack(MAGIC, VERSION, len(layers), FILE_HEADER_SIZE,
                               FILE_HEADER_SIZE, description.encode()[:40]))
        table_off = header_bytes
        for layer, table in zip(layers, tables):
            hmin = float(layer.heights.min())
            hmax = float(layer.heights.max())
            f.write(_LAYER_HDR.pack(layer.name.encode()[:32], layer.north, layer.west,
                                    layer.dlat, layer.dlon, table_off,
                                    layer.rows, layer.cols, layer.tile_size, layer.flags,
                                    layer.quant_step, layer.blend_m, layer.resolution_m,
                                    hmin, hmax))
            table_off += TILE_ENTRY_SIZE * len(table)
        for table in tables:
            for e in table:
                f.write(_TILE.pack(*e))
        for b in blobs:
            f.write(b)
    return stats


def _tiles_xy(layer: Layer) -> tuple[int, int]:
    T = layer.tile_size
    return (layer.cols + T - 1) // T, (layer.rows + T - 1) // T


# ── Reader ────────────────────────────────────────────────────────────────────

class Pack:
    """Minimal reader. `layer_array(i)` decodes a whole layer (slow, pure Python —
    meant for tooling/verification; use the C++ library at runtime)."""

    def __init__(self, path: str):
        with open(path, "rb") as f:
            self.data = f.read()
        magic, ver, n, _hsize, ltab, desc = _FILE_HDR.unpack_from(self.data, 0)
        if magic != MAGIC or ver != VERSION:
            raise ValueError(f"{path}: not a TDEM v{VERSION} file")
        self.description = desc.rstrip(b"\0").decode()
        self.layers = []
        for i in range(n):
            (name, north, west, dlat, dlon, toff, rows, cols, T, flags,
             q, blend, res, hmin, hmax) = _LAYER_HDR.unpack_from(self.data, ltab + i * LAYER_HEADER_SIZE)
            tx_n, ty_n = (cols + T - 1) // T, (rows + T - 1) // T
            tiles = [_TILE.unpack_from(self.data, toff + k * TILE_ENTRY_SIZE)
                     for k in range(tx_n * ty_n)]
            self.layers.append(dict(name=name.rstrip(b"\0").decode(), north=north, west=west,
                                    dlat=dlat, dlon=dlon, rows=rows, cols=cols, tile_size=T,
                                    flags=flags, quant_step=q, blend_m=blend, resolution_m=res,
                                    min_h=hmin, max_h=hmax, tiles=tiles, tiles_x=tx_n))

    def tile(self, li: int, tx: int, ty: int) -> np.ndarray:
        L = self.layers[li]
        off, size, base, _, _ = L["tiles"][ty * L["tiles_x"] + tx]
        T = L["tile_size"]
        if off == 0:
            return np.zeros((T, T), dtype=np.float32)
        q = decode_tile(self.data[off:off + size], T).astype(np.int64) + base
        return (q * L["quant_step"]).astype(np.float32)
