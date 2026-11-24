# 0044: HDF5 volumes

Status: accepted (2026-11-23)

## Context

Synchrotron beamlines, laboratory tomography pipelines and many published datasets store
reconstructions in HDF5 files: the data exchange layout (`/exchange/data`), NeXus files
(`/entry/data/data`, `.nxs`), or one or more datasets of any name written by h5py, often chunked
and deflated, with label volumes beside the grey values. HDF5 is a complex binary format (B-trees,
chunk indices, filters) that only the HDF Group's library reads reliably. The maintainer agreed to
the dependency (ADR 0016: formats from established libraries; a heavy one only when asked).

## Decision

**`hdf5` from vcpkg, C API, zlib filter only** (`default-features: false, features: [zlib]`), so
the build adds no szip, MPI, C++ or tools. On Linux distributions the system package works with
the same `find_package(HDF5 COMPONENTS C)`.

`Hdf5Source` (`hdf5.hpp`) reads one dataset as a volume:

- The datasets that can be volumes are found by walking the groups (at most 10 000 objects, 8
  levels): three dimensions, or more of which all but three have size 1, of 8-, 16- or 32-bit
  integers or 32/64-bit floats. Without a choice the largest whose path does not look like labels
  (`gt`, `label`, `mask`, `seg`, as for TIFF folders) is read; the others are reported.
- C order as numpy writes it: the file's last dimension is x.
- Samples become grey values as in raw files (ADR 0043): signed shifted, 32-bit and float mapped
  over a given or estimated value range with clipped values counted. The library converts to the
  machine's byte order.
- The voxel size comes from the parameters, an attribute of the dataset (`element_size_um` as z,
  y, x in µm, as several tools write it; `voxel_size`, `voxel_size_mm`, `pixel_size` or `spacing`
  in mm, one value or x, y, z), the file name, else 1 mm with a warning.
- The library is not built thread-safe, so every call goes through one lock, and the source
  reports slow random access: the import copies the volume to the staging file once, slice by
  slice, and then reads that in parallel.

The operation is `import_hdf5` (`path`, `dataset`, `value_range`, voxel size and sieve
parameters); `inspect_scan`, `open_scan`, `browse` (kind `hdf5`) and `vs-sieve` (`--dataset`)
know it. Files are recognised by the HDF5 signature at offset 0, 512, 1024, … , not by their
extension.

## Consequences

- HDF5 and NeXus reconstructions open with one click. Files compressed with filters other than
  deflate (szip, blosc, LZ4 plugins) are refused when read, with a message that names the cause.
- The HDF5 library is part of every build and package. Damaged files are the library's to
  handle; no damaged-file test is run against it, since VoxelSieve does not parse the format.
