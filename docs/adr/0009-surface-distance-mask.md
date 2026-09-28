# 0009: Surface as a distance mask with a few bits per voxel

Status: accepted (2026-09-28)

## Context

Many analyses need the surface of the part, not only its grey values: nominal/actual comparison,
wall thickness, the distance of a pore to the surface, rendering and export. The surface should be

- a voxel mask with a few bits per voxel that encode the distance to the true surface,
- very accurate where the surface is,
- extremely compressible, because most of a scan is nowhere near the surface,
- written and read out of core like the dataset (ADR 0004).

A mesh (STL) is the usual exchange format, but it is large (about 50 bytes per triangle, two
triangles per surface voxel) and has no inside/outside for a voxel query. A float level set (VDB)
keeps 32 bits per band voxel.

## Decision

### Codes

Every voxel gets a code of `bits` bits (default 4, 2 to 8). With `N = 2^bits - 1`:

| code | meaning |
| --- | --- |
| 0 | material, farther than `band` from the surface |
| 1 … (N-1)/2 | material, distance quantised in steps of `2 band / (N-1)` |
| (N+1)/2 … N-1 | air, same steps |
| N | air, farther than `band` |

The decoded distance of a band code is `-band + (code - 0.5) * step` voxels, negative in material.
The side of the surface is part of the code, so the sign of every voxel is exact and the mask is
also a binary segmentation; quantisation moves only the distance, by at most half a step. The
default band of ±1 voxel is the smallest that holds both ends of every voxel edge the surface
crosses, so the zero crossing can be triangulated from the codes alone; with 4 bits the step is
1/7 voxel.

### File layout (`.vss`)

```
"VSSURF1\n"
chunk payloads           zstd frames, one per chunk that contains surface
chunk table              zstd; per chunk: kind (u8), offset (u64), size (u64)
header                   JSON: dims, voxel size, bits, band, iso value, statistics
trailer                  table offset, table size, header offset, header size (u64 each),
                         "VSSEND1\n"
```

All integers are little endian. A chunk is a dataset brick (256³ by default); the table gives
every chunk a kind: all air, all material, or surface. Only surface chunks have a payload:

- two bits of block kind (air, material, surface) for every 8³ block of the chunk, four per byte;
- for every surface block, its 512 codes, x fastest, packed with `bits` bits each, lowest bits
  first (64 · bits bytes per block).

The file is written in one pass (payloads first, table and header at the end) and renamed into
place, so a reader never sees a half-written file. The chunk table allows random access; the
reader decodes chunks on demand into an LRU cache.

### Extraction

For each chunk the grey values are read through `Dataset::readRegion` with a halo of band + 3
voxels, the iso-surface at the iso value is triangulated (OpenVDB `levelSetRebuild`, marching
cubes with linear interpolation) and the exact Euclidean distance of every voxel to that
triangulation is taken inside the band. The sign comes from the grey value, so it agrees with the
triangulation. Chunks are independent (the halo holds every triangle that reaches into the band),
run in a TBB pipeline with four chunks in flight, and are written in order; memory stays bounded
whatever the scan size. Chunks without a stored brick are air; chunks whose halo lies entirely
above or below the iso value cost one table entry.

The default iso value is ISO 50 %: half way between the air level of the dataset and the material
level, the median of voxels above the dataset threshold in up to 16 bricks. A fixed value can be
given.

### Measured

Synthetic scans of the sample parts at full size with 0.1 mm voxels, noise σ = 2 % of the
contrast, 10 % cupping, an unsharpness of σ = 0.5 voxel and four lunkers, sieved into 256³ bricks
(`vs-synth --part … --voxel-size 0.1 --blur 0.05 --cupping 0.1 --lunker 4 --noise 400`):

| part | voxels | file | against 16-bit raw | bit per band voxel | surface RMS | p99 | volume error |
| --- | --- | --- | --- | --- | --- | --- | --- |
| housing | 520×400×220 | 156 kB | 588 : 1 | 0.72 | 0.033 voxel | 0.11 | 0.03 % |
| bracket | 420×260×300 | 58 kB | 1128 : 1 | 0.47 | 0.030 voxel | 0.12 | 0.01 % |
| hub | 460×460×160 | 209 kB | 323 : 1 | 1.39 | 0.041 voxel | 0.13 | 0.10 % |

Surface RMS and p99 are distances of the triangulated codes to the analytic surface. The ratio
grows with the scan: the stored blocks grow with the surface area, the scan with the volume, and
air or material far from the surface costs two bits per 8³ block before compression, or nothing
for whole chunks. The sieved float dataset of the same scans takes 42 to 63 MB, an STL of the
extracted housing surface 97 MB.

Alternatives measured on smaller scans of the housing and the hub, and rejected:

- 8 bits instead of 4: the surface RMS improves only from 0.067 to 0.064 voxels on the housing
  (the extraction, not the quantisation, dominates) at 4.4 times the size. 3 bits save 30 % and
  cost 2 to 19 % more error.
- zlib is 10 to 16 % larger. lzma is 3 to 7 % smaller but decodes much more slowly than zstd,
  which matters for slices read chunk by chunk. zstd level 19 is 17 to 23 % smaller than level 3.
- Residuals against a 3D Lorenzo predictor, one byte per code, and codes interleaved across
  blocks: equal or larger, because the band is only two voxels thick and the clamping breaks the
  linear prediction.
- Gaussian smoothing of the grey values before locating the surface: rounds edges more than it
  removes noise, larger errors at every tested width.

## Consequences

- `voxelsieve/surface.hpp`: `writeSurface`, `SurfaceMask` (codes, distances, regions, level set,
  mesh), `writeSurfaceImages`; the CLI `vs-surface`; the studio operation `surface`.
- The ISO 50 % surface follows the unsharpness of the scan: sharp edges are rounded by about the
  width of the point spread function. A local, gradient-based surface determination can replace
  the extraction later without changing the format.
- The format stores the surface of one iso value. Multi-material parts need one file per
  material boundary.
- The band must not exceed the air margin of the dataset (default 3 voxels), because removed air
  is treated as far from the surface.
- Sample parts with a known surface (`voxelsieve/parts.hpp`: housing, bracket, hub) and an
  unsharpness in the synthetic scan (`SyntheticSpec::blur_sigma_mm`) make the accuracy testable
  on realistic geometry.
