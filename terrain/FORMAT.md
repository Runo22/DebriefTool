# TDEM pack format, version 1

A `.tdem` file holds one or more **layers**. Each layer is a regular grid of
terrain heights (point samples) on WGS-84 latitude/longitude, in metres above
mean sea level (EGM2008, the Copernicus DEM vertical datum). All integers and
floats are **little-endian**.

```
[ File header   64 B ]
[ Layer header 128 B ] × layer_count
[ Tile table         ] per layer: tiles_y × tiles_x entries of 24 B, row-major
[ Tile blobs         ]
```

## File header (64 B)

| Offset | Type      | Field                                   |
|-------:|-----------|-----------------------------------------|
| 0      | char[4]   | magic `TDEM`                            |
| 4      | u32       | version = 1                             |
| 8      | u32       | layer_count                             |
| 12     | u32       | header_size = 64                        |
| 16     | u64       | offset of the first layer header        |
| 24     | char[40]  | description (UTF-8, zero padded)        |

## Layer header (128 B)

| Offset | Type     | Field                                                              |
|-------:|----------|--------------------------------------------------------------------|
| 0      | char[32] | name (zero padded)                                                 |
| 32     | f64      | north — latitude of row 0 (deg)                                    |
| 40     | f64      | west — longitude of column 0 (deg)                                 |
| 48     | f64      | dlat — row spacing (deg, > 0). Row *r* is at `north − r·dlat`      |
| 56     | f64      | dlon — column spacing (deg, > 0). Column *c* is at `west + c·dlon` |
| 64     | u64      | file offset of this layer's tile table                             |
| 72     | u32      | rows                                                               |
| 76     | u32      | cols                                                               |
| 80     | u32      | tile_size *T* (256)                                                |
| 84     | u32      | flags (reserved, 0)                                                |
| 88     | f32      | quant_step — height quantisation (m)                               |
| 92     | f32      | blend_m — edge fade distance toward coarser layers (m)             |
| 96     | f32      | resolution_m — nominal ground spacing (m)                          |
| 100    | f32      | min height of the layer (m)                                        |
| 104    | f32      | max height of the layer (m)                                        |
| 108    | —        | 20 bytes reserved (0)                                              |

`tiles_x = ceil(cols / T)`, `tiles_y = ceil(rows / T)`.

## Tile entry (24 B)

| Offset | Type | Field                                                        |
|-------:|------|--------------------------------------------------------------|
| 0      | u64  | blob offset; **0 = absent tile** (all heights 0 m — sea)     |
| 8      | u32  | blob size (bytes)                                            |
| 12     | i32  | base — tile minimum in quant steps                           |
| 16     | f32  | tile min height (m)                                          |
| 20     | f32  | tile max height (m)                                          |

## Tile blob (codec 1: planar predictor + DEFLATE)

Every tile stores a full *T×T* block; tiles on the right/bottom edge are padded
by repeating the last row/column.

Encoding:

1. `q = round(h / quant_step)`; `v = q − base` (fits in u16).
2. Residual (all arithmetic mod 2¹⁶), with `v` taken as 0 outside the tile:
   `r[i][j] = v[i][j] − v[i][j−1] − v[i−1][j] + v[i−1][j−1]`
3. Zigzag: `z = (r << 1) ^ (r >> 15)` with `r` read as int16.
4. Byte planes: all low bytes of `z` (T² bytes), then all high bytes (T² bytes).
5. Raw DEFLATE (RFC 1951, no zlib header).

Decoding reverses this; step 2 inverts to a 2D prefix sum:
`v[i][j] = Σ_{a≤i, b≤j} r[a][b]` (mod 2¹⁶), and `h = (base + v) · quant_step`.

## Querying multiple layers

Readers order layers finest first. For a point, each covering layer gives a
bilinear height *hᵢ* and an edge weight `wᵢ = smoothstep(d / blend_m)` where *d*
is the distance (m) to the layer's nearest border (`wᵢ = 1` when `blend_m = 0`).
The result is `w₁h₁ + (1−w₁)(w₂h₂ + (1−w₂)(…))`, renormalised at the outer
edge of the coarsest layer.
