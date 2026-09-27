# VoxelSieve

Open-source tooling for industrial CT volumes, built on [OpenVDB](https://www.openvdb.org/).

Reconstructed CT volumes are large, uncompressed raw files in which most voxels are air.
VoxelSieve separates the surrounding air from the part, keeps internal voids such as pores,
and stores the result as a sparse VDB grid that can be rendered quickly and analysed further.

Status: early development. Two command-line tools exist: `vs-phantom` generates synthetic test
volumes with known ground truth, and `vs-sieve` converts a raw volume into a sparse VDB grid.

## Build

On Ubuntu 24.04:

```sh
sudo apt-get install cmake ninja-build g++ libopenvdb-dev libboost-iostreams-dev \
  nlohmann-json3-dev libgtest-dev
cmake --preset release && cmake --build --preset release && ctest --preset release
./build/release/apps/vs-phantom/vs-phantom --out phantom --dims 256
```

This writes `phantom.raw` (uint16, little endian, x fastest) and `phantom.json` with the format
description and the analytic ground truth (pores, wall thickness, material volume).

## Sieve a volume

VoxelSieve writes either a bricked dataset (any size, streamed) or a single `.vdb` grid (small
volumes, in memory).

### Bricked dataset for large scans

```sh
./build/release/apps/vs-sieve/vs-sieve scan.raw --out scan.vsieve
```

`scan.vsieve/` holds `index.json`, `overview.vdb` (the whole volume in one grid of at most 256³
voxels) and `level0/`, `level1/`, … with one VDB file per 256³ brick. Bricks that contain only
outside air are not written. The input is memory-mapped and processed in two streaming passes, so
the scan does not need to fit in RAM. `--phantom N` sieves a computed N³ phantom instead of a file.

Raw files from CT systems usually start with a proprietary header. VoxelSieve skips it: given the
dimensions, everything in the file beyond the voxel data is taken as the header, and the detected
size is printed. Use `--header <bytes>` when the file also has a footer, `--type uint8` for 8-bit
data and `--big-endian` for big-endian 16-bit samples. Check the printed header size: wrong
dimensions that are too small also leave extra bytes and would be misread as a header.

| Input | Raw size | Dataset | Time | Peak memory |
|---|---|---|---|---|
| 1024³ raw file (mmap) | 2 GB | 776 MB | 6.1 s | 2.5 GB, of which ~2 GB reclaimable mapped input pages |
| 1024³ computed phantom | (2 GB) | 776 MB | 35.5 s | 0.46 GB |
| 2048³ computed phantom | (16 GB) | 5.8 GB | 345 s | 0.87 GB |

The computed phantom is slower because every voxel is evaluated twice; its memory figure shows the
sieve's own footprint without mapped input pages.

Programs read a dataset through `voxelsieve::Dataset`, which loads bricks on demand into an LRU
cache with a memory budget:

```cpp
const auto dataset = voxelsieve::Dataset::open("scan.vsieve", /*cache_bytes=*/4ULL << 30U);
std::optional<float> value = dataset.sample(0, {512, 400, 300});  // nullopt for removed air
std::vector<float> slab(512 * 512 * 16);
dataset.readRegion(0, {{0, 0, 300}, {512, 512, 316}}, slab);      // across brick boundaries
dataset.forEachBrick(1, [](const auto& brick, const openvdb::FloatGrid& grid) { /* parallel */ });
```

### Single grid

```sh
./build/release/apps/vs-sieve/vs-sieve phantom.raw --out phantom.vdb
```

Dimensions and voxel size are read from `phantom.json`, or given with `--dims X Y Z` and
`--voxel-size MM`. The air/material threshold is estimated with Otsu's method unless `--threshold`
is set; `--margin` controls how many voxels of air stay around the part. `--dense` writes every
voxel without sieving, as a baseline. The resulting `.vdb` opens directly in Blender or Houdini.

Phantom benchmark (hollow box with pores filling about a quarter of the volume, noise σ = 500,
4 cores):

| Volume | Raw | Dense VDB | Sieved VDB | Active voxels | Sieve time |
|---|---|---|---|---|---|
| 256³ | 32 MB | 43.6 MB | 11.8 MB | 27 % | 0.03 s |
| 512³ | 256 MB | 348.9 MB | 85.9 MB | 26 % | 0.23 s |

Sieved files store grey values as 32-bit float (lossless, see ADR 0002), which is why the dense
baseline is larger than the raw file.

Design decisions are recorded in [docs/adr](docs/adr).

## Synthetic scans from STL

`vs-synth` turns a closed STL mesh (in mm) into a raw CT volume with artificial defects, noise and
artefacts, plus a JSON sidecar with the ground truth. It is the test bed for defect analyses.

```sh
./build/release/apps/vs-synth/vs-synth part.stl --out part --voxel-size 0.05 \
    --lunker 5 --loosening 2 --noise 500 --cupping 0.1 --rings 8
./build/release/apps/vs-sieve/vs-sieve part.raw --out part.vsieve
```

- `--lunker N`: shrinkage cavities, irregular unions of overlapping spheres.
- `--loosening N`: zones of loosened microstructure (Gefügeauflockerung), many small pores near or
  below the voxel size up to `--loosening-porosity` (default 5 %).
- `--noise`, `--cupping` (beam hardening), `--rings` (ring artefacts around the z axis).
- `--box X Y Z` uses a box instead of an STL file.

`part.json` lists every defect with its position, size and void volume, and the total porosity.
The grey values carry the void volume exactly, even for pores much smaller than a voxel (see
`docs/adr/0005-synthetic-scans-from-meshes.md`). The scan is computed on demand and streamed to
disk: 1040 × 840 × 640 voxels (1.1 GB) with 15 defects take 41 s on 4 cores.

## License

Apache-2.0, see [LICENSE](LICENSE).
