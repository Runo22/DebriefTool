#!/usr/bin/env python3
"""
preview.py — render a hillshade PNG of one layer of a TDEM pack (for checking).

    python preview.py ../data/airbases.tdem akinci_5m akinci.png [--max 2048]
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))
import tdem  # noqa: E402


def decode_layer(pack: tdem.Pack, li: int) -> np.ndarray:
    L = pack.layers[li]
    T = L["tile_size"]
    ty_n = (L["rows"] + T - 1) // T
    out = np.zeros((ty_n * T, L["tiles_x"] * T), dtype=np.float32)
    for ty in range(ty_n):
        for tx in range(L["tiles_x"]):
            out[ty * T:(ty + 1) * T, tx * T:(tx + 1) * T] = pack.tile(li, tx, ty)
    return out[:L["rows"], :L["cols"]]


def hillshade(h: np.ndarray, dx: float, dy: float) -> np.ndarray:
    gy, gx = np.gradient(h, dy, dx)
    az, alt = np.radians(315.0), np.radians(45.0)
    slope = np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(-gx, gy)
    s = np.sin(alt) * np.cos(slope) + np.cos(alt) * np.sin(slope) * np.cos(az - aspect)
    return np.clip(s, 0, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("layer")
    ap.add_argument("out")
    ap.add_argument("--max", type=int, default=2048, help="max output edge in pixels")
    a = ap.parse_args()
    p = tdem.Pack(a.pack)
    li = [l["name"] for l in p.layers].index(a.layer)
    L = p.layers[li]
    h = decode_layer(p, li)
    step = max(1, int(np.ceil(max(h.shape) / a.max)))
    h = h[::step, ::step]
    lat = L["north"] - 0.5 * L["rows"] * L["dlat"]
    dy = L["dlat"] * step * 111000.0
    dx = L["dlon"] * step * 111000.0 * np.cos(np.radians(lat))
    shade = hillshade(h, dx, dy)
    # Hypsometric tint (low green -> high brown/white), modulated by the shade.
    lo, hi = np.percentile(h, 1), np.percentile(h, 99.5)
    t = np.clip((h - lo) / max(1.0, hi - lo), 0, 1)[..., None]
    c0, c1, c2 = np.array([70, 110, 60]), np.array([170, 140, 90]), np.array([240, 240, 235])
    col = np.where(t < 0.6, c0 + (c1 - c0) * (t / 0.6), c1 + (c2 - c1) * ((t - 0.6) / 0.4))
    col = np.where((h <= 0.0)[..., None], np.array([60, 90, 140]), col)
    img = (col * (0.35 + 0.65 * shade[..., None])).clip(0, 255).astype(np.uint8)
    Image.fromarray(img).save(a.out)
    print(f"{a.out}: {img.shape[1]}x{img.shape[0]}  h {h.min():.1f}..{h.max():.1f} m")


if __name__ == "__main__":
    main()
