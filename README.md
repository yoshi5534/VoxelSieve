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

## License

Apache-2.0, see [LICENSE](LICENSE).
