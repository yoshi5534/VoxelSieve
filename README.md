# VoxelSieve

Open-source tooling for industrial CT volumes, built on [OpenVDB](https://www.openvdb.org/).

Reconstructed CT volumes are large, uncompressed raw files in which most voxels are air.
VoxelSieve separates the surrounding air from the part, keeps internal voids such as pores,
and stores the result as a sparse, bricked VDB dataset that works for scans larger than memory.
On top of it, it finds pores and loosened microstructure and writes an inspection report.

| CT slice | Porosity analysis | Inspection report |
|---|---|---|
| ![Slice through a synthetic casting scan with two zones of loosened microstructure and a shrinkage cavity](docs/images/ct-slice.png) | ![Projection of the part in grey with pores in red and loosened zones in yellow](docs/images/porosity-projection.png) | ![Evaluation table of the inspection report with two inspection zones](docs/images/report-evaluation.png) |
| Synthetic scan with noise and cupping: two loosened zones and a shrinkage cavity | `vs-porosity`: pores in red, loosened zones in yellow | `vs-report`: evaluation per inspection zone (BDG P 202 scheme) |

All three images come from one synthetic scan of 1025 × 775 × 525 voxels (830 MB), made with
`vs-synth --box 80 60 40 --voxel-size 0.08 --lunker 6 --loosening 3 --noise 500 --cupping 0.1
--seed 7`. Every lunker volume and every zone is found within 2 % of the ground truth.

Status: early development (0.1.0, see [CHANGELOG.md](CHANGELOG.md)). The tools:

| Tool | Does |
|---|---|
| `vs-sieve` | raw volume (with vendor header) → sparse bricked dataset, streamed, out of core |
| `vs-porosity` | pores and loosened zones → JSON, projection images, VDB for Blender or Houdini |
| `vs-report` | porosity analysis + acceptance limits → inspection report (HTML, print to PDF) |
| `vs-synth` | STL mesh → synthetic CT scan with defects, noise and artefacts, plus ground truth |
| `vs-phantom` | simple box phantom with analytic ground truth |

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

## Porosity analysis

`vs-porosity` finds internal pores and zones of loosened microstructure in a sieved dataset:

```sh
./build/release/apps/vs-porosity/vs-porosity part.vsieve --out part.porosity
```

- `porosity.json`: part volume, porosity, and every pore and zone with centre, bounds and void
  volume.
- `projection_x.png`, `projection_y.png`, `projection_z.png`: the part as a grey thickness image,
  pores in red, zones in yellow.
- `porosity.vdb`: grids `pores` and `zones` in the dataset's world space, to view next to the part
  in Blender or Houdini.

Pore volumes come from the grey values, so partially filled edge voxels count with their fraction.
Zones are 8³ blocks whose grey value lies clearly below the material level at the same depth below
the surface, so cupping from beam hardening is not reported as porosity (see
`docs/adr/0006-porosity-analysis.md`). On an 830 MB synthetic scan (1025 × 775 × 525 voxels,
6 lunkers, 3 loosened zones, noise and 10 % cupping) every lunker is within 2 % of its true volume,
every zone within 2 %, and the total porosity within 1 %; the analysis takes 14 s on 4 cores.
`PorosityOptions` and the `--min-pore`, `--zone-sigma` and `--zone-min` options set the detection
limits.

## Inspection report

`vs-report` runs the porosity analysis, evaluates it against the acceptance limits of an
inspection order and writes a test report as one self-contained HTML file (print to PDF from the
browser):

```sh
./build/release/apps/vs-report/vs-report part.vsieve --order examples/inspection_order.json \
    --out part.report
```

The report is structured after the report contents of DIN EN ISO/IEC 17025 (7.8) and the CT test
report of DIN EN ISO 15708-3: order, part, method, scan settings, analysis parameters and
detection limits, results with projection images and pore list, evaluation and approval. The
inspection order (`examples/inspection_order.json`) supplies what the scan cannot know, such as
the customer, the part number or the tube voltage; missing mandatory fields are marked
"nicht angegeben" in the report and printed as warnings.

Acceptance follows the scheme of BDG P 202: per inspection zone (a box in dataset coordinates, or
the whole part) optional limits on the largest pore extent, the number of pores above a size, the
porosity, and whether loosened microstructure is allowed. The limits come from the drawing or the
customer.

The layout is a template: `vs-report --print-template > my_template.html` prints the built-in one,
`--template my_template.html` uses your own. Templates use a Mustache subset (`{{name}}`,
`{{#list}}...{{/list}}`, `{{^name}}...{{/name}}`); `report.json` in the output shows all available
data. See `docs/adr/0007-inspection-report.md`.

## Projects, operations and plugins

The library records work as a project: a directory with `project.json` and one directory per step.
Each step stores its operation, parameters, inputs, outputs and messages; undo and redo move a
cursor over the steps, and a new step after an undo discards the undone ones. Operations never
change their inputs.

```cpp
voxelsieve::OperationRegistry registry;
voxelsieve::registerBuiltinOperations(registry);   // open_dataset, import_raw, porosity, report
registry.loadPlugins("plugins");                    // every *.so in the directory
auto project = voxelsieve::Project::create("casting.vsproj", "Casting 4711");
project.run(registry, "import_raw", {{"path", "scan.raw"}});
project.run(registry, "porosity");                  // input: the latest dataset
project.run(registry, "report", {{"order_path", "examples/inspection_order.json"}});
project.undo();
```

A plugin is a shared library that exports `voxelsieve_plugin_api_version()` and
`voxelsieve_register_operations()`; `examples/plugins/histogram` is a complete one. See
`docs/adr/0008-studio-projects-operations-ui.md`.

## License

Apache-2.0, see [LICENSE](LICENSE).
