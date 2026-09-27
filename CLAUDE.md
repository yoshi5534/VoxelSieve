# VoxelSieve

Open-source tooling for industrial CT volumes on top of OpenVDB. The core idea: remove only the
outside air from reconstructed CT volumes, keep the part and its internal voids (pores, cavities),
and store the result as a sparse VDB grid for fast rendering and analysis.

## Layout

- `include/voxelsieve/` public headers, `src/` library implementation (target `VoxelSieve::voxelsieve`)
  - `source.hpp` read access to volumes larger than RAM (`MappedRawSource`, `PhantomSource`, ...)
  - `sieve.hpp` in-memory sieve into one grid; `dataset.hpp` streaming sieve into a bricked dataset
    and `Dataset`, the cached read access to it; algorithms on large scans read through `Dataset`
  - `src/detail/` internals shared by both sieves (threshold, block map, flood fill)
  - `mesh.hpp` STL input; `synthetic.hpp` synthetic scans of meshes with defects and artefacts
  - `porosity.hpp` pores and loosened zones in a dataset, with JSON, PNG and VDB output
  - `report.hpp` evaluation against acceptance limits (BDG P 202) and the report template engine;
    the built-in template is `resources/report_template.html`, compiled in
  - `operation.hpp` operations with JSON-schema parameters, the registry and plugin loading;
    `project.hpp` projects with a step protocol, undo/redo and atomic saves (ADR 0008). The
    studio (UI, MCP) drives everything through operations; new processing becomes an operation
  - `studio.hpp` the studio engine and its JSON API; `mcp.hpp` serves it as MCP tools and
    `http_server.hpp` to the browser UI in `resources/ui/` (plain JS, compiled in); `slice.hpp`
    reads slice tiles with the pore and zone overlay. New studio
    features are API methods first, so the UI and AI systems get them alike
- `apps/` command-line tools, one directory per executable (`vs-phantom`, `vs-sieve`, `vs-synth`,
  `vs-porosity`, `vs-report`, `vs-studio`); `examples/` holds an example inspection order
  and an example plugin (`examples/plugins/histogram`)
- `tests/` GoogleTest unit tests plus CLI smoke tests registered in `tests/CMakeLists.txt`
- `docs/adr/` architecture decision records; read them before changing data types or formats

## Build and test

Dependencies (Ubuntu 24.04): `libopenvdb-dev libboost-iostreams-dev nlohmann-json3-dev libgtest-dev
cmake ninja-build clang-format clang-tidy`.

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan   # ASan + UBSan
tools/lint.sh                                                                  # clang-format + parallel clang-tidy
```

clang-tidy is slow because every file pulls in the large OpenVDB headers; `tools/lint.sh` runs it
in parallel and filters the noise about suppressed header findings. CI runs all of these; a PR is mergeable only when they pass. Presets build with warnings as errors.

## Conventions

- C++20, Google-based style from `.clang-format` (100 columns). Names: `CamelCase` types,
  `camelBack` functions, `lower_case` variables and members, `kCamelCase` constants, trailing `_`
  for private/protected members. `.clang-tidy` enforces this.
- Volumes are x-fastest, then y, then z. Raw files are headerless little-endian `uint16` with a
  JSON sidecar (`<name>.json`) describing dims, voxel size and format.
- Grey values are stored losslessly (`FloatGrid`, see ADR 0002). Never quantise silently.
- Code that must scale to XXL scans reads through `VolumeSource` and never assumes the volume fits
  in memory (ADR 0004).
- Every algorithm gets a test against the synthetic phantom (`voxelsieve/phantom.hpp`), whose
  geometry is known analytically. Defect analyses are scored against `SyntheticScan` ground truth. Prefer asserting against the analytic ground truth over
  snapshot values.
- Keep dependencies minimal; adding one needs a short ADR.
- Work happens on branches and pull requests; `main` stays green.
