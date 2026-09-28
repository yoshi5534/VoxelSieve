# Changelog

## 0.1.0 (unreleased)

First version: from a raw CT volume to an inspection report.

- **Sieve** (`vs-sieve`): removes only the outside air and keeps internal voids (ADR 0003). Writes
  a bricked multi-resolution VDB dataset with an overview grid, streamed in two passes over a
  memory-mapped raw file, so scans larger than RAM work (ADR 0004). Reads raw files with vendor
  headers, 8-bit samples and big-endian data. Grey values stay lossless as float (ADR 0002).
- **Projects and operations** (`voxelsieve::Project`, `voxelsieve::OperationRegistry`): every
  step is recorded with parameters, inputs, outputs and messages, can be undone and redone, and
  the project is saved after each change. Import, porosity and report are operations; more can be
  loaded as plugins (ADR 0008).
- **Studio** (`vs-studio`): browser UI with a wizard (dataset, operations, report), the step
  protocol and undo/redo, served locally; operations run while the UI stays responsive.
- **Slice view**: tiled slices along x, y and z from the matching resolution level, with pores and
  zones tinted, in the studio and as `view_slice` for MCP. `Dataset` can load bricks on access
  (`BrickLoading::kOnAccess`) for sparse reads.
- **3D view**: WebGL2 ray casting of a coarse level as a surface, with a transfer function or
  as a maximum intensity projection. Histogram editor for the transfer function (control points
  with opacity and colour, colour maps, presets) and the surface threshold; colours for pores,
  zones, surface and background (studio, gradient, plain); gradient lighting; a cut along x.
  Suggested renderings from the histogram when a volume is loaded.
- **Views**: the project keeps how it was shown and opens the same way; named views with a
  picture are saved in the project, shown again with a click and exported as PNG; `view_*`
  methods for MCP.
- **MCP** (`vs-studio --mcp`): the studio API as Model Context Protocol tools for AI systems
  (`voxelsieve::Studio`, `voxelsieve::McpServer`).
- **Read API** (`voxelsieve::Dataset`): samples, regions across bricks and parallel brick
  iteration through an LRU brick cache with a memory budget.
- **Synthetic scans** (`vs-synth`): STL meshes to raw volumes with shrinkage cavities, loosened
  microstructure, noise, cupping, ring artefacts and unsharpness, and a JSON ground truth
  (ADR 0005). Sample castings with a known surface: housing, bracket and hub (`--part`).
- **Surface** (`vs-surface`, operation `surface`): the surface as a voxel mask with a few bits per
  voxel that encode the distance to it; only 8³ blocks at the surface are stored, compressed with
  zstd. Several hundred to over a thousand times smaller than the raw scan, surface accurate to a
  few hundredths of a voxel; export as mesh or VDB level set (ADR 0009).
- **Surface view**: the 3D view shows only the extracted surface as a mesh at full resolution,
  coarsened to a triangle budget (`surfaceDisplayMesh`, `/api/surface`).
- **Nominal-actual comparison** (`vs-compare`, operation `compare_cad`): aligns a CAD model (STL)
  to the scanned surface (principal axes, then a robust best fit) and measures the signed
  deviation per surface point. Writes statistics, a coloured PLY and views; the 3D view shows the
  deviation in colour (ADR 0010).
- **Report images**: the inspection report shows the part and its pores in 3D (part as glass,
  pores and loosened zones true to scale) when given a surface, and the nominal-actual comparison
  with coloured views and a deviation histogram when given a comparison. Operations can have
  optional inputs for this. The shaded views come from a CPU renderer (`voxelsieve::render`).
- **Porosity analysis** (`vs-porosity`): pores with partial-volume void volumes and zones of
  loosened microstructure against a depth-dependent material level, so cupping is not reported
  as porosity (ADR 0006). JSON, projection images and a VDB with pores and zones.
- **Inspection report** (`vs-report`): evaluation per inspection zone following the BDG P 202
  scheme, report structured after DIN EN ISO/IEC 17025 (7.8) and DIN EN ISO 15708-3, from an
  exchangeable HTML template (ADR 0007).

Known limitations are listed in the consequences of ADR 0006 and ADR 0007. Builds and CI cover
Ubuntu 24.04 only.
