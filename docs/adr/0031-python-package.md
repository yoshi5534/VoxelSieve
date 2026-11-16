# 0031: Python package

Status: accepted (2026-11-16)

## Context

Researchers and many CT engineers work in Python: numpy, SciPy, scikit-image, PyTorch and
notebooks are where new methods are tried and where scans are evaluated beyond what a tool
offers. VoxelSieve was reachable only through its command-line tools, the GUI and MCP, so using
it from Python meant calling programs and parsing their files, and a dataset's voxels could not
be read into numpy at all without writing a reader for the bricked format (ADR 0004).

What Python users need is read access to datasets of any size as numpy arrays, a way to turn
their own arrays into datasets, a way to process a scan larger than memory with their own code,
and the analyses VoxelSieve already has, without a second implementation of either.

## Decision

A package `voxelsieve` in `python/`: a small extension module `voxelsieve._core` over
`libvoxelsieve` and a pure Python layer around it.

- **Binding library: nanobind.** It is small, compiles fast, makes much smaller modules than
  pybind11 and passes numpy arrays in both directions without copies (`nb::ndarray`). It is a
  build tool of the Python package only, taken from pip like the build backend
  (scikit-build-core), not a dependency of the library or the tools: `vcpkg.json` is unchanged
  and builds without `VOXELSIEVE_BUILD_PYTHON` do not need Python. The package's only runtime
  dependency is numpy.
- **Arrays in numpy order.** Volumes are x fastest, so as C-ordered arrays they have shape
  `(nz, ny, nx)`, the order Python's imaging libraries use for stacks; no copy or transpose is
  needed. Metadata (dims, voxel sizes, brick indices, boxes) keeps VoxelSieve's (x, y, z), as in
  its JSON files and tools.
- **Datasets.** `Dataset` wraps `Dataset` (cache, levels, bricks) with numpy indexing per level
  (`ds[z0:z1, y0:y1, x0:x1]`, `ds.level(2)[...]`), boxes, an active mask and brick iteration.
  Values come in the dataset's own type; nothing is quantised (ADR 0021). Reads release the GIL.
- **Writing.** `write_dataset` sieves a uint16 or uint8 array through a `VolumeSource` over the
  array's memory, so a `numpy.memmap` of a raw file stays out of memory. Float arrays are mapped
  onto 16 bit with a recorded value mapping, as float TIFFs are (ADR 0015).
  `write_derived_dataset` calls a Python function per brick, with a halo for neighbourhood
  filters, through `writeDerivedDataset`, so any numpy or SciPy processing runs on scans larger
  than memory and keeps the part.
- **Analyses through operations and projects.** Instead of one binding per analysis, `Project`
  wraps `Project` (ADR 0008) and runs any registered operation by id with keyword parameters,
  validated against the operation's JSON schema; inputs are wired as in the GUI. New operations
  and plugins are available from Python without binding code, results land in the same project
  layout the GUI and the MCP server use, and the protocol, undo and telemetry come for free.
  Results are the operations' summaries and output directories; material volumes are readable
  as uint8 arrays (`MaterialVolume`).
- **Build.** `pip install .` (scikit-build-core, `pyproject.toml`) builds the library and the
  module with CMake, takes the dependencies from vcpkg when `VCPKG_ROOT` is set and installs the
  module, libvoxelsieve and the shared libraries from vcpkg into one package directory. In a CMake
  build, `-DVOXELSIEVE_BUILD_PYTHON=ON` builds the module next to the Python sources and registers
  the package's tests with CTest. The tests use the synthetic phantom through `vs.phantom`.

## Consequences

- Python users get numpy access and every analysis; binding work for a new analysis is nil.
- Typed results (pores as a table, meshes as arrays) are not bound yet; the result files are
  JSON, PNG, STL and VDB, which Python reads with its usual libraries.
- Wheels on PyPI need builds per platform and Python version with the libraries from vcpkg
  inside (for example with cibuildwheel); publishing is a separate step. Until then the package
  is built from source. nanobind's stable ABI could make one wheel per platform serve Python
  3.12 and newer.
- Reconstruction is not part of VoxelSieve; a bridge to reconstruction frameworks in Python can
  build on `write_dataset`.
