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
- **MCP** (`vs-studio --mcp`): the studio API as Model Context Protocol tools for AI systems
  (`voxelsieve::Studio`, `voxelsieve::McpServer`).
- **Read API** (`voxelsieve::Dataset`): samples, regions across bricks and parallel brick
  iteration through an LRU brick cache with a memory budget.
- **Synthetic scans** (`vs-synth`): STL meshes to raw volumes with shrinkage cavities, loosened
  microstructure, noise, cupping and ring artefacts, and a JSON ground truth (ADR 0005).
- **Porosity analysis** (`vs-porosity`): pores with partial-volume void volumes and zones of
  loosened microstructure against a depth-dependent material level, so cupping is not reported
  as porosity (ADR 0006). JSON, projection images and a VDB with pores and zones.
- **Inspection report** (`vs-report`): evaluation per inspection zone following the BDG P 202
  scheme, report structured after DIN EN ISO/IEC 17025 (7.8) and DIN EN ISO 15708-3, from an
  exchangeable HTML template (ADR 0007).

Known limitations are listed in the consequences of ADR 0006 and ADR 0007. Builds and CI cover
Ubuntu 24.04 only.
