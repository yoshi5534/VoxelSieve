# 0016: vcpkg for dependencies, established libraries for formats, compression and crypto

Status: accepted (2026-09-30)

## Context

VoxelSieve was built against Ubuntu 24.04 packages (OpenVDB 10, Boost 1.83, …). That ties the
build to one distribution and its versions, and every other platform needs its own recipe. To keep
the dependency list short, the TIFF and ZIP readers (ADR 0011), the PNG writer and the PLY
reader and writer were written in the library, including an LZW and a PackBits decoder and a
CRC-32 table. Such code is where subtle bugs and security holes live (hostile or truncated input,
format corners nobody tested), and established libraries have had those bugs found and fixed
already.

The maintainer's rule: keep dependencies few, but for file formats, compression and cryptography
always use an established library, never our own implementation.

## Decision

**vcpkg in manifest mode is the package manager.** `vcpkg.json` lists the direct dependencies
and pins every version through `builtin-baseline`. The CMake presets load vcpkg's toolchain from
`$VCPKG_ROOT`; `tools/setup-vcpkg.sh` clones vcpkg at the pinned commit. All presets share one
install tree (`build/vcpkg_installed`).

The overlay triplet `cmake/triplets/x64-linux-voxelsieve.cmake` builds shared libraries in
release only. Shared, because `libvoxelsieve`, the tools and plugins all use OpenVDB, and
OpenVDB's grid registry must exist once; release only, because debug builds of the dependencies
double the build time and nothing here debugs into OpenVDB or Boost.

CI builds the dependencies in one job and keeps the result in vcpkg's binary cache
(`actions/cache`, keyed by `vcpkg.json`, the triplet and the compiler version); the build, test
and lint jobs restore it. A cold build (OpenVDB, OpenEXR, Boost, TBB) runs only when the manifest,
the triplet or the runner's compiler changes.

**File formats, compression and crypto come from established libraries:**

| Purpose | Library | Replaces |
| --- | --- | --- |
| TIFF (incl. LZW, Deflate, PackBits, predictors, BigTIFF) | libtiff | own parser and decoders |
| ZIP archives (incl. ZIP64, CRC check) | libzip | own central directory reader |
| PNG | libpng | own chunk writer and CRC-32 |
| PLY | tinyply | own binary PLY writer and reader |
| zstd, memory-mapped files, CRC | Boost.Iostreams, Boost.CRC | (already used) |
| VDB | OpenVDB | (already used) |
| JSON | nlohmann/json | (already used) |
| DICOM, XML, gzip (ADR 0019) | DCMTK, pugixml, zlib | (new formats) |

Adding a dependency still needs a reason; the question for format and codec code is only *which*
established library, not whether to write our own. A new library is added to `vcpkg.json` and to
the table above.

Outside the rule, deliberately:

- VoxelSieve's own formats (raw with JSON sidecar, `index.json`, `.vss`, `.vsm`, the project
  file) are specified by us and built on the libraries above.
- STL stays in `mesh.cpp` for now: it is a fixed array of triangles, and the only libraries that
  read it (Assimp, OpenMesh) bring a large dependency tree. If mesh formats beyond STL and PLY are
  needed (OBJ, 3MF), Assimp is the candidate and STL moves with it.
- Base64 for data URLs and JSON transport is a text encoding, not a file format.
- Vendor raw headers are skipped by size (ADR 0004), not parsed.

## Consequences

- The TIFF reader now reads what libtiff reads: more compressions (anything libtiff is built with),
  float samples with the floating-point predictor, and libtiff's handling of damaged files. The
  public behaviour of `TiffStackSource` is unchanged (ADR 0011).
- Building from source needs vcpkg and, the first time, time to build OpenVDB and Boost (vcpkg
  keeps the result in its binary cache). The CMake files use plain `find_package`, so other
  package sources work too if they provide the same packages, but the presets and CI use vcpkg.
- Dependency versions change only with a new baseline in `vcpkg.json`, in a pull request of its
  own.
- Windows and macOS builds become a matter of triplets rather than new recipes.
