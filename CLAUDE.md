# VoxelSieve

Open-source tooling for industrial CT volumes on top of OpenVDB. The core idea: remove only the
outside air from reconstructed CT volumes, keep the part and its internal voids (pores, cavities),
and store the result as a sparse VDB grid for fast rendering and analysis.

## Layout

- `include/voxelsieve/` public headers, `src/` library implementation (target `VoxelSieve::voxelsieve`)
- `apps/` command-line tools, one directory per executable (`vs-phantom`, ...)
- `tests/` GoogleTest unit tests plus CLI smoke tests registered in `tests/CMakeLists.txt`
- `docs/adr/` architecture decision records; read them before changing data types or formats

## Build and test

Dependencies (Ubuntu 24.04): `libopenvdb-dev libboost-iostreams-dev nlohmann-json3-dev libgtest-dev
cmake ninja-build clang-format clang-tidy`.

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan   # ASan + UBSan
clang-format --dry-run --Werror $(git ls-files '*.cpp' '*.hpp')
clang-tidy -p build/debug $(git ls-files 'src/*.cpp' 'apps/*.cpp' 'tests/*.cpp')
```

CI runs all of these; a PR is mergeable only when they pass. Presets build with warnings as errors.

## Conventions

- C++20, Google-based style from `.clang-format` (100 columns). Names: `CamelCase` types,
  `camelBack` functions, `lower_case` variables and members, `kCamelCase` constants, trailing `_`
  for private/protected members. `.clang-tidy` enforces this.
- Volumes are x-fastest, then y, then z. Raw files are headerless little-endian `uint16` with a
  JSON sidecar (`<name>.json`) describing dims, voxel size and format.
- Grey values are stored losslessly (`FloatGrid`, see ADR 0002). Never quantise silently.
- Every algorithm gets a test against the synthetic phantom (`voxelsieve/phantom.hpp`), whose
  geometry is known analytically. Prefer asserting against the analytic ground truth over
  snapshot values.
- Keep dependencies minimal; adding one needs a short ADR.
- Work happens on branches and pull requests; `main` stays green.
