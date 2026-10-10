# 0004: Out-of-core datasets as bricked, multi-resolution VDB

Status: accepted (2026-09-27)

## Context

Target data are XXL CT scans of several hundred GB, for example 300 GB of raw `uint16`, which is
about 150 billion voxels or 5300³. Neither the raw volume nor the sieved result fits in RAM. The
format has to support later work on such data (viewing, analysis, processing), not just small
files.

Rough numbers for 300 GB raw with 30 % of the voxels kept:

- 45 billion active voxels, about 88 million 8³ leaves.
- Float values of all leaves: about 180 GB.
- OpenVDB topology alone, even with delayed loading: measured 177 bytes per leaf for a delayed-
  loaded grid (512³ phantom, 74,088 leaves, 12.5 MB vs. 154 MB fully loaded), so about 15 GB.
- `openvdb::io::File::write` needs the complete grid in memory.

What OpenVDB offers for large data, and where it stops:

- **Delayed loading** (`io::File::open(true)`): the file is memory-mapped and leaf values are read
  on first access. Topology is still read completely, and loaded leaves are not evicted again.
- **Clipped reads** (`readGrid(name, bbox)`): only a region is kept in memory.
- **`tools::MultiResGrid`**: a mip-map of a grid, built and held in memory.
- Film and VFX volumes are typically a few GB per grid; large simulations are split over frames
  and grids, so OpenVDB is optimised for sparse grids that fit in RAM, not for one grid of
  hundreds of GB.

The bricked layout below builds on these features (every brick is delayed-loaded and read by
bounding box) and adds what is missing: bounded topology memory, eviction, and writing without
holding the whole result in RAM.

A single VDB grid per dataset therefore does not scale: it cannot be written without holding
everything in RAM, and even opening it costs gigabytes of topology.

## Decision

A dataset is a directory of independent VDB bricks plus an index, at several resolutions:

```
part.vsieve/
  index.json            dims, voxel size, brick size, value type, threshold, air level,
                        list of non-empty bricks per level, statistics
  overview.vdb          the whole volume in one grid of at most 256³ voxels (configurable)
  level0/x_y_z.vdb      full resolution, one grid per brick (default 256³ voxels); FloatGrid,
                        *amended:* uint16 or uint8 in the input's width (ADR 0021)
  level1/x_y_z.vdb      downsampled 2x
  level2/...            until the whole volume fits in one brick
```

- Bricks do not overlap and are aligned to the 8³ leaf grid. Bricks that contain only outside air
  are not written at all.
- Each brick is a normal VDB file: Blender or Houdini can open one directly (*amended:* only
  float datasets, `--value-type float`, since ADR 0021), and OpenVDB's delayed
  loading (memory-mapped, leaf data read on first access) keeps opening a brick cheap.
- Grid index space is global: voxel (i, j, k) of the scan has VDB coordinate (i, j, k) in every
  level-0 brick, so bricks can be combined without offsets.
- The coarse levels give a fast overview and let a viewer load detail only where it is needed.
- `overview.vdb` is the coarsest level as a single file: the entry point for viewers and a quick
  look in Blender. Downsampling averages, so pores smaller than an overview voxel are not visible
  there; they remain in the finer levels.
- The value type is recorded in `index.json`. Float stays the default (ADR 0002); a custom
  `uint16` tree can be added later without changing the layout.

## Streaming sieve

Memory stays bounded by the block map and a window of the input, independent of the scan size:

1. **Pass 1, statistics:** stream the input in slabs (memory-mapped raw or slice images), build
   the full 16-bit histogram and store per 8³ block the maximum grey value (2 bytes per block,
   about 600 MB for 5300³). Threshold by Otsu from the histogram. *Amended:* the valley after
   the air peak when one lies below Otsu's split (see below).
2. **Classify and flood-fill** on the block map in memory: material = block maximum above the
   threshold; outside air by flood fill from the volume faces (ADR 0003). The air margin is
   applied at block level first (neighbours of kept blocks), then trimmed to `margin_voxels`
   per brick.
3. **Pass 2, write:** process one layer of bricks at a time, read their voxels from the
   memory-mapped input, write level-0 bricks, and accumulate the 2x downsampled level 1 on the fly.
   Higher levels are built from level 1, which is 8x smaller.

Inputs are read through a small `VolumeSource` interface (memory-mapped raw first, TIFF stacks
later), since vendors deliver both.

## Access API

`Dataset::open(path)` reads only `index.json`. Bricks are opened on demand through an LRU cache
with a memory budget. The API offers iteration over bricks (in parallel), region reads by bounding
box, point sampling, and level selection. Algorithms that need neighbourhoods read a halo from
adjacent bricks through the same cache instead of storing overlap on disk.

## Consequences

- Per-brick files keep writes, partial updates and parallel processing simple, and any brick can
  be inspected with standard VDB tools.
- A dataset is a directory, not a single file. A single-file container (for example an archive
  of the bricks) can be added later if users need it.
- Many files: 300 GB raw at 256³ bricks gives about 9,000 potential level-0 bricks, fewer after
  sieving. That is fine for local file systems; the brick size is a parameter.
- `min_material_voxels > 1` needs more than the block maximum: pass 1 stores the k-th largest
  value per block instead (still 2 bytes; the k largest values are kept in per-task heaps for
  one row of blocks), so the threshold can still be chosen after pass 1.

## Implementation status

Implemented in `src/dataset.cpp` (`writeDataset`, `readDatasetInfo`, `readBrick`) with
`VolumeSource` implementations for memory-mapped raw files and computed phantoms. Level-0 bricks
are voxel-identical to the in-memory sieve (tested). The margin crosses brick boundaries through a
halo of `ceil(margin / 8)` blocks. Coarser levels are built brick by brick from the finer level,
loading one child at a time.

The access API is `Dataset` in `src/dataset_reader.cpp`: point sampling, region reads across
bricks, parallel brick iteration and an LRU cache bounded by a byte budget. Cached bricks are
loaded completely rather than delayed, so the memory counted against the budget does not grow
behind the cache's back. Not yet implemented: halo reads for neighbourhood algorithms (a region
read covers them for now). TIFF stacks are read through `TiffStackSource` (ADR 0011). `min_material_voxels` works for streaming too
(k-th largest value per block, see Consequences) and is recorded in `index.json`.

## Amendment: threshold for scans of several materials

Otsu's split falls between the two largest classes. In a scan of several materials (the LoDoInd
pipe: plastic pipe, organic fillings, mortar and stones) that is between mortar and stones, so
the pipe wall at the scan ends and the fillings counted as air: air entered the pipe through the
weak wall at the ends and the fillings were cut out in 8³ blocks. The estimate now looks for the
air peak, the lowest clear peak of the smoothed histogram (256 bins between the 0.1 and 99.9 %
quantiles, grey 0 and 65535 left out as clipped or masked values), and puts the threshold into the
valley towards the next peak when that valley lies at least 5 % of the highest bin below both
peaks. Without such a valley below Otsu's split (air and one material) the threshold stays
Otsu's. Both sieves use the same estimate (`detail::airThreshold`).
