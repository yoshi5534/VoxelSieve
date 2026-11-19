# Review: very large scans on modest hardware (November 2026)

A review of the whole code base (HEAD `e7a2f45`) with one question: what breaks or slows down
when a scan is much larger than RAM and the GPU is small? It ends in a plan of steps that can be
done one pull request at a time.

## Target and method

**Reference workload:** raw `uint16` scans of 100–300 GB and more, up to about 5300³ voxels
(1.5·10¹¹). **Reference machine:** 16–64 GB RAM, 8–16 cores, one NVMe SSD with limited free
space, a GPU with 4–8 GB VRAM or an integrated one, WebGL2 in the browser.

**Method:**

- 11 subsystem reviews of `src/`, `include/`, `apps/`, `resources/ui/` and `tools/models/`
  (core dataset, import sources, browser UI, porosity/orientation/surface,
  materials/model/denoise, GUI/HTTP/MCP, operations/project/telemetry,
  thickness/compare/render, dimensional, report/DICONDE/synthetic, battery).
- My own review of the CLI tools, the tests, CI and the build.
- Research on prior art for out-of-core volume processing and rendering.
- Measurements with the release binaries on synthetic phantoms from 256³ to 2048³, on a
  machine with 64 GB RAM and 24 threads.

**Verification marks** on each finding:

- **[M]** measured.
- **[V]** I checked the cited code myself.
- **[R]** reported by a subsystem review and not checked line by line. Treat it as a strong
  lead, and re-check it when you start the step.

## 1. Measurements

Peak private memory in MB, default options, hollow-box phantom with 18 % material:

| tool         | 256³ | 512³ | 1024³ | 2048³      | wall at 2048³ | growth per 8× voxels |
|--------------|-----:|-----:|------:|-----------:|--------------:|---------------------:|
| vs-sieve     |  152 |  751 |  1792 |  2526      |  38 s         | 1.4×                 |
| vs-porosity  |   97 |  454 |  3077 | **17 875** | 119 s         | **5.8×**             |
| vs-surface   |  495 | 1803 |  2908 |  7345      |  55 s         | 2.5×                 |
| vs-segment   |  327 | 2926 |  8370 | 11 252     |  54 s         | thread-bound         |
| vs-thickness |  738 |  978 |  2210 |  5913      |  83 s         | 2.7×                 |
| vs-compare   |  730 |  938 |  1514 |  3855      |  22 s         | 2.5×                 |
| vs-report    |  907 | 1324 |  3042 | 17 871     | 132 s         | contains porosity    |
| vs-denoise   |  335 |  534 |  2262 | –          | 197 s at 1024³| –                    |

What the numbers mean for the reference workload:

- **vs-porosity grows about linearly with the number of voxels: about 2 B per voxel.** It
  reaches 64 GB near 3100³. At 5300³ it would need about 300 GB, so porosity is the first
  analysis that fails on the reference machine. The memory goes to the phase
  "pores and zones" (13.9 GB at 2048³). See P1–P3.
- **vs-sieve** stays bounded in private memory: 2.5 GB at 2048³. Its working set reached
  18.7 GB, though, because it maps the whole 16 GB raw input and never releases those pages
  (S4). The OS can reclaim the pages, but they push the rest of the machine into paging.
- **vs-segment** is bounded by threads × per-brick buffers (316 MB per brick), not by the
  scan (C1).
- **Brick re-reads [M].** The telemetry read counters at 2048³ show the following for a
  dataset of 5.3 GB on disk:
  - vs-thickness, "thickness" phase: 226 GB read
  - vs-segment, "bricks" phase: 76 GB read
  - vs-porosity: 21 + 12 GB read

  A controlled re-run of vs-thickness on the same data:

  | `--cache`          | thickness phase | data read  | kernel CPU | peak RAM |
  |--------------------|----------------:|-----------:|-----------:|---------:|
  | 1024 MB (default)  | 37.5 s          | **183 GB** | 239 s      | 5.7 GB   |
  | 8192 MB            | 7.5 s           | 2.4 GB     | 6 s        | 7.1 GB   |

  With the new `brick_read_mb` counter (step 0.2), level 0 of the 2048³ dataset (4.7 GB) is
  read 46× by thickness, 9.8× by porosity and 9.4× by surface. At 512³ every tool stays near
  1×, because the whole dataset fits in the cache: thrashing only shows on large scans.

  The default 1 GiB cache holds about 29 full `uint16` bricks. 24 threads each need a brick
  plus its neighbours, so the cache thrashes: 77× the reads and 5× the time. The default
  budget doesn't even cap memory: the process peaks at 5.7 GB anyway, because bricks held by
  threads and readers sit outside the budget.

## 2. Findings by theme

Severity is the one given by the review, adjusted where I checked the code. Effort:

- **S:** at most 1 day
- **M:** 2–5 days
- **L:** 1–3 weeks
- **XL:** more than 3 weeks

### A. No memory or thread budget for the whole process

| id | finding | where | sev | mark |
|----|---------|-------|-----|------|
| A1 | No process-wide budget. Each `Dataset`, `SurfaceMask`, `MaterialVolume`, TIFF/DICOM source and GUI view cache has its own fixed default (mostly 1 GiB), and their sum is unbounded. Operations open their own caches beside the GUI's. | `include/voxelsieve/dataset.hpp:196`, `src/gui.cpp:212,436`, `include/voxelsieve/operation.hpp:61` | high | [V] |
| A2 | The default 1 GiB cache thrashes as soon as T threads need T × 27 bricks for halo reads (thickness, surface, materials, denoise, thickness map, models). Concurrent misses on the same brick load it more than once. | `src/dataset_reader.cpp:137-178` | high | [M][V] |
| A3 | No `--threads` option and no `tbb::global_control` anywhere: every tool always uses every core, and per-thread buffers scale with that. | all `apps/` | medium | [V] |
| A4 | `--cache` exists in only 7 of the 13 tools. vs-gui, vs-sieve, vs-compare and vs-diconde have none. | `apps/*/main.cpp` | medium | [V] |
| A5 | `kOnAccess`: the budget is checked only on a miss, and each miss calls `memUsage()` on every cached brick under the lock. Leaves that load between misses are not counted. In the GUI, 1 GiB of topology-only bricks can grow to about 6 GiB loaded. | `src/dataset_reader.cpp:148-178` | medium | [V] |
| A6 | `forEachBrick` is a plain `parallel_for`. Its peak is the budget + T bricks + whatever the caller holds. | `src/dataset_reader.cpp:353` | medium | [V] |
| A7 | `kOnAccess` uses OpenVDB delayed loading without `setCopyMaxBytes(0)`. OpenVDB then copies each brick file to a private temp file before mapping it. (OpenVDB's default size limit is not in the header; check it when you start the step.) | `src/dataset.cpp:704-712` | high | [V] |
| A8 | `readRegion` always returns `float`, at twice the bytes of `uint16`, and does one accessor probe per voxel after pre-filling the output. | `src/dataset_reader.cpp:297-351` | medium | [V] |
| A9 | On Windows and macOS, telemetry reports mapped pages as `peak_anon_mb` (16.5 GB against 2.5 GB private for vs-sieve). Mapped reads count as 0 in `read_mb`, and the peaks cover the whole process lifetime, not one phase. Memory regression tests can't rely on these numbers. | `src/telemetry.cpp:168` | medium | [M][V] |

### B. Analyses that keep per-scan state in RAM

| id | finding | where | sev | mark |
|----|---------|-------|-----|------|
| P1 | Porosity keeps one `BoolGrid` per pore (about 300 KB each, mostly the empty 32³ and 16³ internal nodes) until the end of the analysis. 10⁵ pores take 30 GB. | `src/porosity.cpp:709-720` | **critical** | [V] |
| P2 | Each void voxel enclosed in the part costs about 17 B: a `Coord` list per component (also below `min_pore_voxels`), an Int32 label and a piece byte. | `src/porosity.cpp:265-287,725` | high | [V] |
| P3 | Per-block bookkeeping lives in `std::map`/`std::set` nodes, about 270 B per 8³ block, and is copied again into `material_blocks` and `deficits`. The final merge and the zone loop are serial. | `src/porosity.cpp:126-132,722,898` | high | [V] |
| P4 | `VoxelComponents` deep-copies the mask and stores 512 B of labels per leaf plus a hash-map entry: about 0.75 KB per leaf of the whole candidate mask, which includes the air band around the whole surface. | `src/detail/components.cpp:119-140` | high | [V] |
| P5 | Porosity results are built in memory before writing: a JSON DOM plus its text (about 1.8 KB per pore, 18 GB at 10⁷ pores), an in-memory VDB archive and deep copies of the grids. The GUI loads them whole as well. | `src/porosity.cpp:1066,1280`, `src/gui.cpp:930` | medium | [R] |
| P6 | Porosity makes 4–5 full passes over level 0 and has a long serial tail. | `src/porosity.cpp:749` | medium | [R] |
| S1 | The display mesh, STL and level set are built from the whole full-resolution surface first. Only then are triangles counted and the grid coarsened, with the fine and the coarse grid both alive. Used by the GUI, report, compare and thickness. About 40 GB at 10⁹ surface voxels. | `src/surface.cpp:965-1000`, `src/compare.cpp:1533` | high | [V] |
| C1 | Threshold segmentation holds about 316 MB of float work buffers per brick on every thread. Bricks in flight are limited by cores, not memory. | `src/materials.cpp:728-750` | high | [V] |
| C2 | The `MaterialVolume` cache is limited to 64 bricks, not bytes. The bricks are unpruned Int32 grids (64 MiB each), and a miss at capacity drops the whole cache. | `src/materials.cpp:896-905` | high | [V] |
| C3 | The material overlay in the GUI reads level-0 Int32 label bricks at every zoom level, through that cache. | `src/gui.cpp:948` | high | [V] |
| O1 | Orientation results: one `OrientationBlock` per 16³ block, with per-thread copies before the merge (about 1.6 GB at the reference size), plus a `std::map` for writing. | `src/orientation.cpp`, `writeOrientationResult` | medium | [R] |
| B1 | The particle and void cap in the battery analysis is checked per thread, and the buffers are combined before the global check, so several GB pile up before the error. | `src/battery.cpp:1212-1222` | high | [V] |
| M1 | The renderer keeps 56 B of doubles per supersample: 625 MB per report image, 1.7 GB in glass mode. | `src/render.cpp:146` | medium | [R] |

### C. Access patterns that defeat the brick cache

They all share one pattern: work is split along a scan axis (rows, slices, rays) instead of
bricks, so each brick is decoded many times.

| id | finding | where | sev | mark |
|----|---------|-------|-----|------|
| X1 | Orientation reads one row of blocks through the whole x extent, and each brick is decoded hundreds of times. | `src/orientation.cpp:266-282,380-394` | high | [V] |
| X2 | The DICONDE export reads one z slice per task in parallel over z. Each slice touches all ~441 bricks of its layer. | `src/diconde.cpp:420` | high | [V] |
| X3 | The battery overhang reads a full float plane per row and thread, so every brick of a slab is re-read for every row. | `src/battery.cpp:1774-1779` | high | [V] |
| X4 | Thickness rays: vertices are sorted by brick, but TBB hands 512-vertex ranges from all over the volume to 24 threads at once (see the measurement). | `src/thickness.cpp:784` | high | [M][V] |
| X5 | Halo reads in the surface writer, materials, the thickness map and denoise load whole neighbour bricks, about 9× re-reads each. | `src/surface.cpp:309`, `src/materials.cpp:748`, `src/thickness_map.cpp:283` | medium | [R] |
| X6 | `scoreMaterials` reads full-width slabs one brick high, 6 B per voxel, and scores them serially with a hash lookup per voxel. | `src/materials.cpp:1012-1018` | high | [V] |
| X7 | Learned segmentation runs bricks and tiles serially. The model tile is fixed at 128 and not derived from memory. | `src/model.cpp:660-676,151` | high | [V] |
| X8 | `writeDerivedDataset` builds, compresses and writes bricks on one thread, and allocates a zeroed 64 MB float vector per brick. | `src/dataset.cpp:887` | medium | [R] |
| X9 | Slice tiles with the pore or zone overlay scan every pore-label leaf per tile, and the 3D preview does it up to 256 times. | `src/slice.cpp:135` | medium | [R] |
| X10 | Dimensional measurement: `surfacePoints` decodes the whole element box at full resolution, serially, whatever the stride. The stride sampling can also miss axis-aligned planes completely. | `src/dimensional.cpp:625,644` | high | [R] |

### D. Disk: staging, temp space, resume

| id | finding | where | sev | mark |
|----|---------|-------|-----|------|
| D1 | Every TIFF and DICOM stack, and every `.raw.gz`, is staged to a temp file of 2 B per voxel, also for 8-bit input and also for uncompressed local stacks (`slowRandomAccess()` is always `true`). A 300 GB scan needs 300 GB of temp beside the dataset. | `include/voxelsieve/tiff.hpp:51`, `include/voxelsieve/dicom.hpp:54`, `src/dataset.cpp:132` | high | [V] |
| D2 | The staging copy is written through a dirty read-write mapping that only the OS bounds; nothing throttles the decoders. | `src/dataset.cpp:146-224` | high | [R] |
| D3 | A long import can't resume. Bricks are written in place, `index.json` is written last and not atomically, and a failed run leaves a directory that blocks the retry. | `src/dataset.cpp:690,753`, `src/sieve.cpp:197` | high | [R] |
| D4 | GUI/MCP imports can't choose the staging directory or turn staging off. | `src/builtin_operations.cpp:469` | high | [R] |
| D5 | A step killed mid-run is marked failed on reload, but its partial outputs stay on disk, possibly a 300 GB staging file. | `src/project.cpp:157` | high | [V] |
| D6 | Free space is checked only for the staging copy, never for the output of a step. Output streams are mostly unchecked, so a full disk can produce a "done" step with truncated files. | `src/project.cpp:512,585`, `src/surface.cpp:631` | medium | [R] |
| D7 | TIFF slice files are reopened and their directory parsed again about 20 times per strip. | `src/tiff.cpp:276-290`, `src/detail/tiff.cpp:308,348` | high | [V] |
| D8 | A raw file that is read directly is one mapping of the whole file. It is never released and gets no read-ahead hint. I/O errors on a mapping end the process. | `src/source.cpp:162-189` | medium | [M][R] |
| D9 | Opening a DICOM ZIP inflates every entry completely to read its header. | `src/dicom.cpp:113` | medium | [R] |

### E. Browser and GPU memory

| id | finding | where | sev | mark |
|----|---------|-------|-----|------|
| G1 | The 3D view shows at most 256 voxels per axis: a 256³ overview plus one detail box of at most 256³. At 5300³ this is a 166³ view of the part. | `resources/ui/volume.js:478,841`, `src/slice.cpp:322` | high | [V] |
| G2 | The slice tile cache holds 160 tiles, fewer than one screen of tiles on a large display or with several layers, so it can evict tiles it is drawing (livelock). The level is chosen from CSS pixels, not device pixels. | `resources/ui/viewer.js:13,653` | high | [V] |
| G3 | Each UI re-render creates a new canvas and WebGL2 context. Old ones are not released, and context loss is not handled. | `resources/ui/volume.js:527` | medium | [R] |
| G4 | Each change of the detail box transfers 33 MB and re-uploads 76–100 MB in one frame, including visibility textures when there is no segmentation. | `resources/ui/volume.js:967` | medium | [R] |
| G5 | The ray caster marches every pixel at full device resolution through empty space. MIP and translucent modes neither skip nor stop early. | `resources/ui/volume.js:320` | medium | [R] |
| G6 | `readVolumePreview` builds a float copy of the whole preview, and HTTP copies it again: about 1 GB per request at `max=512`, never cancelled. | `src/slice.cpp:337` | medium | [R] |

### F. Long runs: cancel, progress, concurrency, safety

| id | finding | where | sev | mark |
|----|---------|-------|-----|------|
| F1 | Cancel, progress and GUI shutdown don't reach the long analyses. The option structs of porosity, surface, materials, orientation, compare and battery have no progress or cancel hook, so they run to the end. | `include/voxelsieve/*.hpp`, `src/builtin_operations.cpp:827` | high | [V] |
| F2 | While a background step runs, saving a view rewrites `project.json` from the pre-run copy, racing on the same temp file. | `src/project.cpp:738` | high | [R] |
| F3 | `Gui::mutex_` and `view_mutex_` are held across I/O-heavy calls. Loading a porosity result blocks every tile. | `src/gui.cpp:930,2285` | medium | [V] |
| F4 | Slice-tile work is never cancelled on the server, and each connection gets its own unbounded thread. | `src/http_server.cpp:127` | medium | [R] |
| F5 | The HTTP API has a Host-header check but no token: any local process, or the LAN with `--host 0.0.0.0`, gets full file-system access. | `src/http_server.cpp:96` | medium | [V] |
| F6 | Size fields of untrusted files (ZIP, surface `.vss`, TIFF) drive allocations and arithmetic without limits, with 64-bit overflow in bounds checks. | `src/detail/zip.cpp:83`, `src/surface.cpp:477` | medium | [R] |
| F7 | Long operations run as one blocking POST: after a reload the UI shows neither progress nor Cancel. | `resources/ui/app.js:476` | medium | [R] |
| F8 | One global `cancel_` flag, reset when a run starts, can lose an MCP cancellation. | `src/gui.cpp:1978` | low | [R] |

### G. Tests, CI and tooling

- There are 279 unit tests and 49 CLI smoke tests, but the largest test dataset is 128³. No
  test checks peak memory, and none checks scaling. The cache budget has only two tests
  (`CacheStaysWithinBudget`, `DatasetsShareOneCacheBudget`). [V]
- CI has no TSan preset, no nightly run on a large dataset and no performance tracking. The
  presets set no architecture flags; the only explicit SIMD code is in
  `src/detail/convolution.cpp`. [V]
- All 13 tools parse their arguments by hand, and the sieve parameter schema exists three
  times in `builtin_operations.cpp` and again in each CLI. [V][R]
- `dimensional.cpp` (2600 lines) and `battery.cpp` (3557 lines) mix several concerns, and
  `battery.cpp` has its own 6-connected component labelling beside `VoxelComponents`. [R]

### What is already good

The base is sound and the plan builds on it rather than replacing it:

- The bricked multi-resolution dataset (ADR 0004) with native integer grids (ADR 0021).
- The bounded two-pass sieve: pass 1 is about 0.6 GB and the flood fill about 1 GB at
  5300³.
- Staging only for slow sources, with page release.
- Telemetry per phase, and a GUI that drives everything through operations.
- The phantom- and ground-truth-based tests.

None of the steps below needs a new file format.

## 3. Plan

Each step is meant to be one pull request, or a few. Each one says when it is done: a
measurable criterion, usually from the scale benchmark of step 0.1. Steps within a phase are
mostly independent; phases build on each other where noted.

### Phase 0: Measure first (about 1 week)

**0.1 Scale benchmark as a tool (M).**
- Turn the measurement scripts used for this review into
  `tools/bench/scale.{sh,ps1}`: phantom at N = 256…2048, then every tool, then peak private
  memory, wall time and the telemetry read counters into a CSV.
- Add a CMake target `bench-scale`, kept out of `ctest`.
- *Done when* one command reproduces the table in section 1.

**0.2 Telemetry that can be trusted (S–M; A9).**
- Measure private memory on Windows (`PrivateUsage` of `PROCESS_MEMORY_COUNTERS_EX`) and
  macOS (`phys_footprint`) separately from mapped file pages.
- Sample the peak per phase (a light 50 ms sampler thread) instead of the process-lifetime
  peak.
- Count bytes of mapped reads (`readRegion` of mapped sources).
- *Done when* vs-sieve at 2048³ reports about 2.5 GB private and the mapped bytes separately.

**0.3 Memory regression tests (S).**
- Add a `ctest` label `scale` with 512³ runs of porosity, surface, segment and thickness that
  fail above a stored peak + 20 % or above a stored brick re-read factor (bytes read ÷ dataset
  size).
- Run it in a new nightly CI job together with 0.1 at 1024³.

**0.4 TSan preset (S).**
- The later steps add concurrency to the cache and to the schedulers, so the race detector
  has to come first.

### Phase 1: Quick wins (about 2 weeks, each S unless noted)

They need no new architecture, and several give large factors at once.

1. **Cache size from RAM and the same options everywhere (A2, A4).**
   - The default cache becomes a share of physical RAM (for example 25 %, at least 2 GiB),
     instead of 1 GiB.
   - Add `--cache` to vs-gui, vs-sieve, vs-compare and vs-diconde.
   - Expected: vs-thickness at 2048³ goes from 37 s to about 8 s in the thickness phase
     (measured above).
2. **`--threads N` in all tools (A3)**, through one `tbb::global_control` in a shared app
   helper, and a matching GUI setting.
3. **`setCopyMaxBytes(0)` for delayed loading (A7).** One line plus a test that counts temp
   files and bytes read per slice request.
4. **Deduplicate in-flight brick loads (A2).** Use a per-key `shared_future` in `BrickCache`,
   so two threads never decode the same brick at once.
5. **Brick-local scheduling for thickness rays (X4).**
   - Group vertices by brick into tasks, one task per brick.
   - Order the tasks along a Morton curve and run them through a `parallel_pipeline` with a
     limited number of tokens, so the bricks in use stay a small sliding window.
   - *Done when* the re-read factor is below 2 with the default cache.
6. **DICONDE export over z-slabs one brick high (X2).** Read a slab with `kFull` loading and
   write its 256 slices from the in-memory slab in parallel.
7. **Battery: global atomic cap (B1)** checked inside the loop, and one plane buffer per
   thread reused across rows (X3, short-term part).
8. **Porosity without per-pore grids (P1, M).**
   - Dilate all pore labels once into one shell mask.
   - Walk its leaves in parallel and add each shell voxel to per-label arrays.
   - The analysis then no longer has a constant 300 KB per pore. Check against the phantom's
     analytic pores.
9. **Viewer tile cache sized from the view (G2).**
   - Size the cache at 3 × visible tiles × (1 + layers), under a byte budget.
   - Never evict a tile drawn in the current frame.
   - Choose the level from device pixels.
10. **Staging and temp files:**
    - A `staging_dir` parameter for GUI/MCP imports (D4).
    - Delete stale `.voxelsieve-staging` files on start, and partial outputs of steps marked
      "interrupted" (D5).
    - Write `index.json` atomically (D3, short-term part).
11. **Checked output streams** with a clear error on a full disk (D6, short-term part).

*Phase result:* the 2048³ numbers for thickness and segment improve markedly, and porosity
loses its pore-count term. Porosity is still linear in voxels after this phase; phase 3 fixes
that.

**Status (2026-11-17): done** on branch `phase1-quick-wins` (commits 3b54f0b to d1f989b).
Measured with `tools/bench/scale.cmake` at 2048³, 16 threads, 64 GB, default options:

| tool         | wall Phase 0 | wall Phase 1 | level-0 re-reads | own peak Phase 1 |
|--------------|-------------:|-------------:|-----------------:|-----------------:|
| vs-surface   | 41 s         | 33 s         | 9.4× → 1.0×      | 7.2 GB           |
| vs-porosity  | 101 s        | 97 s         | 9.8× → 1.0×      | 17.1 GB          |
| vs-segment   | 54 s         | 37 s         | → 1.0×           | 10.8 GB          |
| vs-thickness | 59 s         | 34 s         | 46× → 0.94×      | 7.6 GB           |
| vs-compare   | 22 s         | 17 s         | –                | 3.7 GB           |
| vs-report    | 132 s        | 103 s        | → 1.0×           | 17.1 GB          |

- With the old 1 GiB budget (`--cache 1024`), the thickness phase still takes 11 s instead
  of 37 s and reads level 0 3.2 instead of 46 times, thanks to the brick-local scheduling.
- Porosity's "pores and zones" phase takes 36 s instead of 57 s (6.3 instead of 2.9 cores),
  and its results match the old ones to 1e-10. Its peak is still about 2 B per voxel
  (P2, P3): phase 3.
- Own peaks grew by up to 2 GB where the bigger cache now holds what was read again before.
- Not verified locally: the Linux and macOS code paths (cgroup limit, no OpenVDB temp
  copies, process check), and the viewer change in a browser.

### Phase 2: One budget for the whole process (about 3–4 weeks)

The basis for everything after it. It needs an ADR ("Execution context and memory budget").

**2.1 `ExecutionContext` (M; A1).**
- One object per process, or per GUI operation, with:
  - a memory budget (default: a share of physical RAM, overridable)
  - a `task_arena` with the thread count
  - a temp directory with free-space accounting
  - the cancel/progress token (see 5.1)
- It is passed through `OperationContext` and the CLI helpers.
- One shared `BrickCache` per context replaces the per-object defaults. The GUI view cache
  becomes a share of the same budget.

**2.2 Pinned brick handles (M; A5, A6).**
- `Dataset::pin(level, brick)` returns a ref-counted handle. The cache counts pinned and
  unpinned bricks against the budget and evicts only unpinned ones.
- The cache accounts by the actual size after load, so it no longer re-measures every cached
  brick on each miss.
- `kOnAccess` charges a brick's full size on its first leaf access, or is replaced by
  brick-local leaf decoding.

**2.3 Brick sweep with halo and admission control (L; A2, A6, X5).**
- A single iterator for "process every brick with a halo of h voxels":
  - It visits bricks in z-slabs or Morton order.
  - It pins the 3×3×3 neighbourhood once.
  - It limits the bricks in flight with `parallel_pipeline` tokens computed from the budget:
    tokens = (budget − cache floor) ÷ per-task cost, where the caller declares the per-task
    cost.
- Port `forEachBrick`, the surface writer, materials, denoise, the thickness map, models and
  `writeDerivedDataset` (parallel compress and write, X8) to it.
- *Done when* the re-read factor is below 1.5 for every tool in the benchmark and peak
  private memory stays within the budget ± 10 % at 2048³ with `--memory 8G`.

**2.4 Typed, block-wise `readRegion` (M; A8).**
- Overloads for `uint16_t`, `uint8_t` and `float` output.
- Copy leaf by leaf (`probeConstLeaf` + `memcpy` of rows) instead of one accessor probe per
  voxel.
- Callers that only threshold (orientation mask, materials, thickness map) switch to
  `uint16`, which halves their buffers.

**2.5 Admission control for operations (S–M).**
- A background operation declares an estimate (from dataset info) and waits, or runs with a
  smaller share, when the budget is taken.
- Set the per-operation estimates from the 0.1 benchmark.

**Status (2026-11-19): done** on branch `phase2-budget` (commit 6e32448, ADR 0031), on top of
phase 4:

- 2.1: one budget per process (`memoryBudgetBytes`, `--memory`, 3/4 of RAM by default) instead
  of an `ExecutionContext` object: tools run one analysis, and the GUI's operations share the
  process. Every `BrickCache` registers its budget; the shared cache is a third, the GUI view
  cache a sixteenth; `workMemoryBytes` is the rest, split between admitted operations. Cancel and
  progress stay in `OperationContext` (phase 5).
- 2.2: bricks a caller holds are pinned (not evicted, counted); bricks loaded on access are
  charged all their leaves at once, so the cache no longer re-measures itself on every miss.
- 2.3: `Dataset::forEachBrick(level, f, BrickSweep{halo, task_bytes})`: Morton order, tasks in
  flight from the work memory, isolated, halo bricks pinned. Segmentation, models, orientation and
  the surface writer size their work from the same budget; `writeDerivedDataset` builds and
  writes bricks in parallel behind the serial values (X8) and resumes from a journal.
- 2.4: `readRegion` copies leaf by leaf, with `uint16`/`uint8` overloads; only the DICONDE export
  switched (orientation, materials and the thickness map compute in float anyway).
- 2.5: `Operation::estimate` (memory and output size), checked by the project before a step:
  free space (`requireFreeSpace`, also 4.4) and `admitOperation`. Plugin API version 5. The
  estimates are rough figures per active voxel, not fitted to the benchmark.
- Measured at 2048³ with `--memory 8192` (16 threads, 64 GB; branch `phase34-rest`, segment
  and surface after setting their task costs from this run):

  | tool         | wall   | own peak | level-0 re-reads |
  |--------------|-------:|---------:|-----------------:|
  | vs-sieve     | 37 s   | 2.3 GB   | –                |
  | vs-porosity  | 37 s   | 7.9 GB   | 4.5×             |
  | vs-surface   | 19 s   | 8.7 GB   | 1.0×             |
  | vs-segment   | 33 s   | 9.3 GB   | 2.6×             |
  | vs-thickness | 18 s   | 6.9 GB   | 2.1×             |
  | thickness map| 111 s  | 7.8 GB   | 3.8×             |
  | vs-compare   | 2.2 s  | 0.4 GB   | –                |
  | vs-report    | 38 s   | 8.3 GB   | 4.6×             |
  | vs-denoise   | 1092 s | 5.5 GB   | 6.1×             |

  The *done* criterion is only half met. Peaks stay within the budget +10 % except segment
  (+13 %), where about 2.7 GB more than buffers and cache add up are taken, probably memory the
  allocator keeps (not verified). Re-reads below 1.5 hold only for single-pass tools: the cache
  of 2.7 GB is smaller than level 0 (4.7 GB), so every further pass over level 0 reads it again
  (porosity: scan, pores and zones, images; segment: the histogram for the thresholds; the
  thickness map: mesh, rays, map). That needs fewer passes, not a better cache. Denoise is
  compute-bound (197 s at 1024³ in phase 0).

### Phase 3: Analyses whose memory doesn't grow with the scan (about 6–8 weeks)

The order follows the measured risk: porosity first.

**3.1 Porosity out of core (L; P2–P6).**
- Label connected components per brick with `labelLeaf`. Only pieces that touch a brick face
  go into a global union-find; ids are 32 bits wide, and the union-find stores no per-voxel
  labels.
- Accumulate per-component statistics in arrays: count, bounds, sums, moments and the shell
  contribution. Components below `min_pore_voxels` are dropped by a size array before they
  are labelled. No coordinate lists.
- Write the label grid in a second sweep per brick from the merged mapping.
- Keep block data in block-resolution VDB grids (float mean and variance, Int32 counts,
  float deficit) instead of `std::map`/`std::set`, merged per thread with `tree().merge`.
- Stream the results: write `porosity.json` incrementally, write the VDB grids brick by
  brick, and have the GUI read an index (pore id → bounds and offset) instead of the whole
  file.
- *Done when* peak private memory at 2048³ is below 4 GB, porosity grows at most 2× per 8×
  voxels and stays within the budget, the phantom pore tests are unchanged, and the run is
  checked once at 3000³ on 32 GB.

**3.2 Slim `VoxelComponents` (M; P4).**
- Short term, also usable by battery and isolate-part:
  - Don't copy the mask.
  - Recompute local labels per leaf on demand instead of storing 512 B.
  - Use 32-bit piece ids, reuse `roots` as labels, and replace the `unordered_map` with
    `probeConstLeaf`.
- Long term it becomes the per-brick labeller of 3.1.
- Battery's own 6-connected labelling moves onto it.

**3.3 Surface meshes chunk by chunk (L; S1).**
- Choose the coarsening up front from the `.vss` header (`band_voxel_count` against
  `max_triangles`).
- Decode the surface mask chunk by chunk, reduce the distance field by 2^k, mesh each chunk
  with a one-cell overlap and weld at chunk faces.
- Cache the display mesh next to the `.vss`.
- `scanSurface` (compare, thickness) and `--stl` use the same path. Full-resolution STL is
  streamed to the file.
- *Done when* vs-surface and vs-compare at 2048³ peak below 3 GB, and the deviation and
  thickness tests are unchanged.

**3.4 Materials within a byte budget (M; C1–C3).**
- Bricks in flight are set by the brick sweep (2.3), not by the core count.
- The `MaterialVolume` cache counts bytes and evicts by LRU, not "drop everything".
- Label bricks are stored as `UInt8Grid` when there are at most 255 classes (needs an ADR
  note per ADR 0021) and are pruned. The overlay reads the matching level instead of level 0
  (add label levels in the writer, by majority vote).

**3.5 Orientation in brick-aligned tiles (M; X1, O1).**
- One task per stored brick, or per 128³ sub-tile, plus a halo, in Morton order.
- Results go into block-resolution grids, not into vectors and maps.

**3.6 Segmentation and models (M; X6, X7).**
- Bricks run in parallel through the sweep, and the tile size comes from the budget.
- Convolutions reuse padded buffers.
- `scoreMaterials` works per brick, in parallel, with an array lookup instead of a hash.

**3.7 Smaller items (S each):**
- Renderer accumulation in float, or per tile (M1).
- `readStl` streams the file (`src/mesh.cpp:67`).
- `MeshDistance` gets a smaller BVH (`src/compare.cpp:1302`).
- Dimensional: decode at the stride and fix the axis-aligned plane miss (X10).
- Thickness map written as `uint16` with a scale, or as half floats.
- Battery overhang through the sweep (X3).

**Status (2026-11-18): mostly done** on branch `phase3-analyses` (commits 53ba885 to
a30c1b8), without phase 2 first. Measured at 2048³, 16 threads, 64 GB:

| tool         | Phase 1        | Phase 3        | note                                        |
|--------------|---------------:|---------------:|---------------------------------------------|
| vs-porosity  | 97 s, 17.1 GB  | 39 s, 8.7 GB   | analysis 4.1 GB with `--cache 1024`         |
| vs-report    | 103 s, 17.1 GB | 39 s, 9.5 GB   | contains porosity                           |
| vs-compare   | 17 s, 3.7 GB   | 2.2 s, 1.1 GB  | display mesh from coarse samples            |
| vs-thickness | 34 s           | 20 s           |                                             |

- 3.1: no coordinate lists, labels as tiles, block values in block-resolution grids, results
  streamed. The phantom's cavity (5·10⁸ voxels) cost 12 B per voxel before. What remains is
  the porosity VDB output, 4 B per pore voxel. The GUI still loads the whole JSON (P5). Not
  yet checked at 3000³ on 32 GB.
- 3.2: done, except that battery still has its own 6-connected labelling (bounded by
  `kMaxFindingVoxels`).
- 3.3: coarse level sets read from the codes, streamed STL. The display mesh is not cached
  next to the `.vss`.
- 3.4: pruned label bricks, LRU by bytes, bricks in work from memory. Still missing:
  `UInt8Grid` labels and label levels for the overlay.
- 3.5: tiles in Morton order, not measured at scale (no command-line tool).
- 3.6: scoring and model bricks in parallel. Convolution buffers are not reused yet.
- 3.7: renderer layers in floats, streamed `readStl`. Still missing: the BVH, dimensional
  stride, thickness map in 16 bit and battery overhang.

### Phase 4: Import and disk (about 3–4 weeks)

**4.1 Direct reads instead of staging where possible (L; D1, D7).**
- Uncompressed local TIFF and DICOM stacks record the offset of every strip, tile or slice in
  one header pass and serve `readRegion` by positioned reads, like a raw file.
  `slowRandomAccess()` becomes `true` only for compressed or archived stacks.
- TIFF gets a whole-slice path: one pooled file handle, the stored `TiffPage`, one libtiff
  handle per slice, and strips decoded straight into the caller's span, bypassing the chunk
  cache.
- Record the change in an ADR amending ADR 0011.

**4.2 Leaner staging (M; D1, D2).**
- Stage in the input's width (1 B for 8-bit).
- Write with sequential buffered I/O and bounded write-behind instead of a dirty read-write
  mapping.
- Optionally, use Blosc/LZ4 per slab; Blosc is already a dependency through OpenVDB.
- Compute the histogram and block maxima while each slice is in memory, which saves the
  pass-1 read of the copy.
- *Done when* a TIFF import whose copy is 2× RAM runs without paging, with private memory
  below 2 GB.

**4.3 Resumable import (L; D3).**
- Keep a small journal in the output directory: options, threshold, the block-map checksum
  and finished bricks.
- Write bricks as temp file + rename.
- A restart with the same options skips finished bricks and resumes the levels.
- Use the same mechanism for the thickness map and `writeDerivedDataset`.

**4.4 Disk-space accounting (M; D6).**
- Each operation declares the estimated output size, the project checks it before the run,
  and the writers check it periodically.

**4.5 Release mapped input pages (S; D8).**
- Use `PageRelease` for direct raw input too, behind a slab window, with a read-ahead hint
  (`MADV_SEQUENTIAL`, `PrefetchVirtualMemory`).
- Report I/O errors on a mapping as an exception (guard with SEH or `SIGBUS`) instead of
  ending the process.
- *Done when* the vs-sieve working set at 2048³ is below 4 GB.

**Status (2026-11-18): mostly done** on branch `phase4-import` (commits 4767041 to 05c9587):

- 4.1: uncompressed local TIFF slices (8/16 bit, both byte orders, strips or tiles,
  multi-page) are read in place through a pool of mapped files and are no longer staged
  (ADR 0011 amendment). Not yet done for DICOM; not measured at scale (no TIFF writer for
  large phantoms).
- 4.2: 8-bit input is staged in 8 bit. Still missing: buffered writes instead of the
  mapping, Blosc slabs, and the histogram during staging.
- 4.3: an interrupted import resumes from its journal (`import.json`, `pass1.bin`,
  `bricks.log`); pass 1 and finished bricks are skipped. Each brick runs in a TBB context of
  its own, so a failure no longer cancels the OpenVDB loops of the others; that cancellation
  had crashed with half-built trees. The thickness map and `writeDerivedDataset` do not
  resume yet.
- 4.4: free space is checked against the kept blocks before pass 2. Operations do not
  declare their output sizes yet.
- 4.5: the pages of a mapped raw input are released during the passes. vs-sieve at 2048³
  maps at most 4.1 GB instead of 16.4 GB (working set 5.3 instead of 18.9 GB). There is no
  read-ahead hint and no guard for I/O errors on mappings.

**Status (2026-11-19): phases 3 and 4 completed** on branch `phase34-rest` (on top of
`phase2-budget`, commits 8a6891d to b7de7c4):

- 3.3: the display mesh is cached beside the `.vss` (`<file>.mesh-<budget>-<adaptivity>.bin`,
  stamped with the surface file's size and time).
- 3.4: material volume format 2: pruned `UInt8Grid` labels and coarser label levels (majority of
  2 × 2 × 2); slice overlay and 3D view read the level of their zoom (ADR 0013 amended). Version 1
  volumes still read.
- 3.6: the model feature pool (which already reused padded buffers) is bounded by the work
  memory. The model tile size is still the option's 128.
- 3.7: `MeshDistance` BVH about 98 instead of 136 B per triangle (normals computed on demand,
  float node boxes rounded outwards); dimensional measurement finds surfaces between strided
  samples (the axis-aligned plane miss) and runs its slabs in parallel, but still decodes the
  box at full resolution; the thickness map is stored in 16 bit with a scale (ADR 0030
  amended); the battery overhang reads its rows in chunks of 128, one brick layer or two at a
  time.
- 4.1: uncompressed local DICOM stacks are read in place (ADR 0019 amended).
- 4.2: the staging copy is written with positioned writes, as many slices at once as the work
  memory holds; with `min_material_voxels` 1, pass 1 is taken while staging, so the copy is read
  once. No Blosc slabs (the copy stays a plain raw file that is mapped for pass 2).
- 4.3: the thickness map and `writeDerivedDataset` (with a `resume_key`) resume.
- 4.4: operations declare their output size (`Operation::estimate`), checked before a step.
  The writers do not check the free space periodically; a full disk is still reported by the
  checked writes of phase 1.
- 4.5: read-ahead hints for mapped raw files, TIFF and DICOM slices; I/O errors of a mapping
  become exceptions (SEH on Windows, SIGBUS elsewhere; the POSIX path is only compiled in CI).
- 3000³ check (3.1), with `--memory 32768` on the 64 GB machine: `vs-sieve --phantom 3000`
  283 s, 2.6 GB own peak (16 GB dataset); `vs-porosity` 143 s, 25.0 GB own peak, within the
  budget. The largest phase is the VDB output (17.8 → 25.0 GB, the phantom's large cavity at
  4 B per pore voxel). Not run on a machine with 32 GB of RAM.
- Not done: battery's own labelling is 6-connected, `VoxelComponents` 26-connected, so moving it
  over would change the results (needs a connectivity option). The 3.1 target of a porosity peak
  below 4 GB at 2048³ is not met (7.9 GB with `--memory 8192`, most of it cache and the VDB
  output).

### Phase 5: Long runs that can be controlled (about 2–3 weeks, can run alongside phases 3–4)

**5.1 Cancel and progress in every analysis (M; F1).**
- One `ProgressToken` in all option structs, from the `ExecutionContext`, checked at brick
  granularity by the sweep (2.3).
- `analyzePorosity`, surface, materials, orientation, compare, battery and dimensional report
  progress.
- *Done when* every operation in the registry can be cancelled within 2 s at 1024³ (test).

**5.2 Project and server concurrency (M; F2–F4, F8).**
- `project.json` is saved only from one serialized writer, with a merge of step state and
  view state.
- Hold mutexes only for state, not across I/O.
- Run HTTP on a bounded worker pool, with tile requests cancellable by generation number.
- One cancel token per run.

**5.3 Async operations in the UI (M; F7).**
- POST returns a run id.
- Progress comes from a polled `GET` endpoint or SSE, and the UI resumes it after a reload.

**5.4 Safety (S–M; F5, F6).**
- Use a random token per server start (in the URL the GUI opens, and checked on every
  request).
- Set size limits and overflow-safe arithmetic for ZIP, `.vss` and TIFF headers, with fuzz
  targets.

### Phase 6: 3D view within a VRAM budget (about 4–6 weeks, XL)

Independent of phases 3–5 once phase 2 is in.

**6.1 Brick endpoint (M).**
- `GET api/brick?level&i&j&k` in 8 or 16 bit, with a one-voxel apron, cacheable.
- A per-level brick index with min/max values, written by the sieve.
- `readVolumePreview` without the float copy (G6).

**6.2 Brick atlas and page table in WebGL2 (L; G1).**
- A fixed atlas `TEXTURE_3D` (for example 512³ R16 or R8 = 128–256 MB) and a page-table
  texture per level.
- LOD by screen-space error, a prioritised and cancellable request queue, and LRU in the
  atlas under a VRAM budget setting (default 512 MB, adjustable by the user; WebGL can't
  query free VRAM).
- Upload with `texSubImage3D` spread over frames (G4).

**6.3 Empty-space skipping and early termination (M; G5).**
- Use the per-brick min/max in the page table, stop early in translucent modes, and render at
  reduced resolution while the camera moves.

**6.4 One GL context per view (M; G3)**, kept across re-renders, with context-loss handling.
Merge the duplicated camera, input and upload code of `volume.js` and `scene.js`.

- *Done when* a 2048³ scan can be explored down to level 0 in any region with a 1 GB VRAM
  budget at 30 fps on the integrated GPU.

### Phase 7: Maintenance (ongoing, low priority)

- **Shared CLI helper:** parsing, `--threads`, `--cache`, `--memory`, `--telemetry`. Generate
  the sieve options from the operation schema, so they exist once instead of four times.
- **Split up the two largest files:** `dimensional.cpp` into fits, features, plan,
  extraction and datums; `battery.cpp` into views, cylinder, stack and images.
- **UI tests** for the tile cache, request queue, level choice and detail region (node
  tests).
- **Training scripts:** sample from bricks in a streamed way and use the GPU when available
  (`tools/models/train_*.py`).

## 4. Order and dependencies

```
Phase 0 ──► Phase 1 (quick wins) ──► Phase 2 (budget, sweep) ──┬─► Phase 3 (analyses)
                                                              ├─► Phase 4 (import, disk)
                                                              ├─► Phase 5 (cancel, concurrency)
                                                              └─► Phase 6 (3D view, VRAM)
```

**Where to start:** Phase 0.1/0.2, then from phase 1: 1 (cache from RAM), 5 (thickness
scheduling), 8 (porosity shells) and 3 (`setCopyMaxBytes`). Together they give the largest
measured gains for about two weeks of work.

**Biggest single risk:** porosity (3.1). Until it is done, the reference machine can't analyse
scans above about 3000³.

## 5. Not recommended

These came up in the research or the reviews and don't pay off now:

- **A new primary format or codec** (Zarr, our own compression). The bricked VDB with Blosc is
  adequate; OME-Zarr can come later as an export.
- **NanoVDB or CUDA for analyses.** The bottleneck is memory and I/O, not arithmetic, and
  WebGL2 has no compute stage.
- **A WebGPU-only renderer.** WebGPU isn't available on all target browsers yet; the brick atlas
  works in WebGL2.
- **An exact unbounded distance transform** for the thickness map. A bounded band plus the
  sphere method is enough for the acceptance limits in use.
