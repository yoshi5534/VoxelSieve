# VoxelSieve

Open-source tooling for industrial CT volumes on top of OpenVDB. The core idea: remove only the
outside air from reconstructed CT volumes, keep the part and its internal voids (pores, cavities),
and store the result as a sparse VDB grid for fast rendering and analysis.

## Layout

- `include/voxelsieve/` public headers, `src/` library implementation (target `VoxelSieve::voxelsieve`)
  - `voxel_size.hpp` voxel edge length per axis and slice thickness (ADR 0012)
  - `source.hpp` read access to volumes larger than RAM (`MappedRawSource`, `PhantomSource`, ...)
    and `tiff.hpp` TIFF stacks, also inside ZIP archives (`TiffStackSource`, ADR 0011); float
    input is mapped onto 16 bit with a recorded `ValueMapping` (ADR 0015)
  - `sieve.hpp` in-memory sieve into one grid; `dataset.hpp` streaming sieve into a bricked dataset
    and `Dataset`, the cached read access to it; algorithms on large scans read through `Dataset`
  - `src/detail/` internals shared by both sieves (threshold, block map, flood fill)
  - `mesh.hpp` STL input; `synthetic.hpp` synthetic scans of meshes with defects and artefacts;
    `parts.hpp` sample castings (housing, bracket, hub) with an analytic surface
  - `surface.hpp` the surface as a few-bit distance mask (`.vss`, ADR 0009); `compare.hpp` the
    nominal-actual comparison of that surface with a CAD model (ADR 0010) and the fits that
    align objects (`fitRigid`, `alignSurfaces`, ADR 0018); `render.hpp` a CPU
    renderer for shaded report and documentation images
  - `porosity.hpp` pores and loosened zones in a dataset, with JSON, PNG and VDB output
  - `materials.hpp` segmentation into material classes, the material volume and scoring against
    labels (ADR 0013); `model.hpp` learned models (.vsm) run on the CPU tile by tile (ADR 0014),
    trained with the PyTorch scripts in `tools/models/`
  - `report.hpp` evaluation against acceptance limits (BDG P 202) and the report template engine;
    the built-in template is `resources/report_template.html`, compiled in
  - `telemetry.hpp` time and resource use per phase of every operation and tool (ADR 0017); mark
    the phases of new long-running code with `TelemetryPhase`
  - `operation.hpp` operations with JSON-schema parameters, the registry and plugin loading;
    `project.hpp` projects with a step protocol, undo/redo and atomic saves (ADR 0008), and their
    objects (volumes, meshes) with poses in one global coordinate system (ADR 0018,
    `transform.hpp`); poses change only through steps. The
    studio (UI, MCP) drives everything through operations; new processing becomes an operation
  - `studio.hpp` the studio engine and its JSON API; `mcp.hpp` serves it as MCP tools and
    `http_server.hpp` to the browser UI in `resources/ui/` (plain JS, compiled in); `slice.hpp`
    reads slice tiles with the pore and zone overlay, and other objects in a slice (`samplePlane`,
    `cutMesh`). New studio
    features are API methods first, so the UI and AI systems get them alike
- `apps/` command-line tools, one directory per executable (`vs-phantom`, `vs-sieve`, `vs-synth`,
  `vs-porosity`, `vs-segment`, `vs-surface`, `vs-compare`, `vs-report`, `vs-studio`); `examples/` holds an example inspection order
  and an example plugin (`examples/plugins/histogram`)
- `tests/` GoogleTest unit tests plus CLI smoke tests registered in `tests/CMakeLists.txt`
- `docs/adr/` architecture decision records; read them before changing data types or formats

## Build and test

Dependencies come from vcpkg in manifest mode (`vcpkg.json`, versions pinned by
`builtin-baseline`, ADR 0016). `tools/setup-vcpkg.sh` clones vcpkg at that commit into
`$VCPKG_ROOT` (default `~/vcpkg`); export `VCPKG_ROOT` before configuring. The first configure
builds OpenVDB, Boost and TBB from source; vcpkg's binary cache makes later ones fast. Tools from
the system: `cmake ninja-build g++ clang-format clang-tidy` (plus `autoconf automake libtool` for
some ports).

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan   # ASan + UBSan
tools/lint.sh                                                                  # clang-format + parallel clang-tidy
```

Windows and macOS build with the presets `windows` (MSVC, from a Developer PowerShell) and
`macos` (Apple silicon); CI builds and tests both. Platform code stays behind `#ifdef _WIN32` /
`__APPLE__` in few places (plugin loading, telemetry counters, signals); tests must not use a shell.

clang-tidy is slow because every file pulls in the large OpenVDB headers; `tools/lint.sh` runs it
in parallel and filters the noise about suppressed header findings. CI runs all of these; a PR is mergeable only when they pass. Presets build with warnings as errors.

## Conventions

- C++20, Google-based style from `.clang-format` (100 columns). Names: `CamelCase` types,
  `camelBack` functions, `lower_case` variables and members, `kCamelCase` constants, trailing `_`
  for private/protected members. `.clang-tidy` enforces this.
- Volumes are x-fastest, then y, then z. Raw files are headerless little-endian `uint16` with a
  JSON sidecar (`<name>.json`) describing dims, voxel size and format.
- Grey values are stored losslessly (`FloatGrid`, see ADR 0002). Never quantise silently.
- Voxels need not be cubes: `VoxelSize` (`voxel_size.hpp`, ADR 0012) has a pitch per axis. Work
  in voxel indices and convert with `toMm`/`toVoxels` or `voxel_size[axis]`; never multiply by one
  scalar voxel size.
- Code that must scale to XXL scans reads through `VolumeSource` and never assumes the volume fits
  in memory (ADR 0004).
- Every algorithm gets a test against the synthetic phantom (`voxelsieve/phantom.hpp`), whose
  geometry is known analytically. Defect analyses are scored against `SyntheticScan` ground truth. Prefer asserting against the analytic ground truth over
  snapshot values.
- Keep dependencies minimal; adding one needs a short ADR and goes into `vcpkg.json`.
- File formats, compression and crypto always come from an established library (libtiff, libzip,
  libpng, tinyply, Boost.Iostreams, OpenVDB, nlohmann/json, …), never our own parser, codec or
  checksum (ADR 0016). Only VoxelSieve's own formats are written here.
- Work happens on branches and pull requests; `main` stays green.
