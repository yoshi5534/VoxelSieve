# 0021: Native 16-bit and 8-bit unsigned grids

Status: proposed (2026-10-10)

Supersedes ADR 0002 in part: float stays available, but it is no longer the only value type.

## Context

ADR 0002 stores grey values in `FloatGrid`, because OpenVDB 10 has no unsigned 16-bit grid type
and `HalfGrid` would quantise. Float is lossless for 16-bit integers, but each voxel takes twice
the memory and disk space of the input. 8-bit scans take four times as much.

Measurements while working on the import preview (ADR 0020), 2026-10-09:

- A 4 GB `uint16` phantom (1280³, noisy, nothing sieved away) became a 6.3 GB dataset: 1.6
  times the input. The import is CPU-bound on 4 cores. Writing to RAM instead of the disk made
  it 37.3 s instead of 37.6 s. With the write cache limited to 256 MB it took 41.8 s, so
  the import already writes about as fast as that disk (172 MB/s) can take. Every extra core
  raises the write rate further.
- The brick cache of `Dataset` (ADR 0004) holds float leaves: 2 KB per 8³ leaf, so half as many
  bricks fit in a given memory budget as with 16-bit values.

OpenVDB does not need a built-in grid type for this. A grid is a template over its value type,
and OpenVDB 10 already names `uint16` and `uint8` (`typeNameAsString`). A prototype with
`Grid<Tree4<std::uint16_t, 5, 4, 3>::Type>` and the same for `std::uint8_t` builds, writes with
Blosc and reads back exactly, once the types are registered (`registerGrid()`). The prototype
converted the 45 level-0 bricks of the Me 163 dataset (56.7 million active voxels, all integer
values). It wrote them with the same compression we use, to memory:

| value type | size | write | read |
|---|---|---|---|
| float | 180.5 MB | 1.19 s | 0.36 s |
| uint16 | 144.5 MB | 0.64 s | 0.24 s |
| uint8* | 88.0 MB | 0.48 s | 0.18 s |

\*The upper 8 bits of the same values, for the size only. This scan is 16-bit.

On disk the gain is only 20 % for 16 bit, presumably because Blosc already compresses the
zero low bytes of float and the noise does not compress at all. Writing takes half the time, reading a third less, and memory per leaf halves (16 bit)
or quarters (8 bit).

The catch: only programs that register these types can read such grids. OpenVDB refuses an
unregistered grid type when it reads a file. As far as we know, Blender and Houdini
register only the standard types, so they cannot open a `uint16` brick.

## Decision

1. **Value types.** A dataset or `.vdb` holds `float`, `uint16` or `uint8` grey values.
   `vdb.hpp` defines `UInt16Grid` and `UInt8Grid` (`Tree4<T, 5, 4, 3>`, the same tree
   configuration as `FloatGrid`). A VoxelSieve `initializeVdb()` calls `openvdb::initialize()`
   and registers both types. Every place that now calls `openvdb::initialize()` calls it
   instead.
2. **Import picks the native type.** `VolumeSource` reports `sampleType()`. The default is 16
   bit. 8-bit raw files, TIFF stacks and DICOM stacks with at most 8 bits stored report 8 bit.
   Float input is mapped onto 16 bit (ADR 0015) and stays `uint16`. The grid stores exactly the
   input samples, so nothing is quantised. `DatasetOptions::value_type` and
   `vs-sieve --value-type <native|float|uint16>` override the choice. `float` is for viewers that
   need it. Narrowing 16-bit input to 8 bit is not offered here: it would quantise.
3. **Coarse levels** (level 1 and up) hold the mean of 2³ voxels. In integer datasets the mean is
   rounded to the nearest integer, so a whole dataset has one value type. The error is at most
   half a grey value, in levels that already average. `index.json` records
   `"coarse_levels": "rounded mean"`. Level 0 stays exact.
4. **Format.** `index.json` gets `"value_type": "uint16" | "uint8" | "float"` and format version
   2. Version-1 datasets, all float, still open unchanged. A single `.vdb` carries the type in its
   grid type name.
5. **Reading.** `Dataset::sample` and `Dataset::readRegion` keep returning float, which is exact
   for every integer value. Algorithms that read regions therefore stay as they are. The brick
   cache holds bricks in their stored type, so twice (16 bit) or four times (8 bit) as many fit.
   `Dataset::brick` and `forEachBrick` hand out a brick that can hold any of the three grid
   types. Code that walks leaves (porosity, surface, materials, slices) dispatches once per
   brick to a template over the grid type. This changes the dataset API that plugins use, so
   `kPluginApiVersion` goes up.
6. **Viewers.** An export to a float `.vdb` (operation and `vs-sieve --value-type float`) stays
   available for Blender and Houdini. Our own results are unchanged: they are written in their
   own types (pore labels, materials, the `.vss` surface).

## Consequences

- An integer dataset takes half (16 bit) or a quarter (8 bit) of the memory of a float one, in
  the brick cache and while bricks are built. On disk it is 20 % smaller for the measured 16-bit
  scan, and smaller still for 8-bit scans or scans with less noise.
- Pass 2 and the levels spend less time compressing and writing. How much the whole import
  gains is measured per phase with telemetry (ADR 0017) once this is built, against the numbers
  above.
- Bricks are no longer plain VDB files for every program: Blender and Houdini need the float
  export. ADR 0004 promised that each brick opens directly in them; that now holds only for
  float datasets.
- Code that touches grids becomes templated over three types. Where only values are needed,
  `readRegion` keeps it out of the algorithms.
- Tests: a lossless round trip of the phantom (ADR conventions) as `uint16` and as `uint8`
  (an 8-bit raw phantom), version-1 datasets still open, the rounded coarse levels against the
  analytic means, and `--value-type float` producing the old format.
- CLAUDE.md ("Grey values are stored losslessly (`FloatGrid`, see ADR 0002)") is updated when
  this is implemented.
