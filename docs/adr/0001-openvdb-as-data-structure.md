# 0001: OpenVDB as the volume data structure

Status: accepted (2026-09-27)

## Context

Reconstructed industrial CT volumes are dense, uncompressed raw files, often tens of gigabytes.
Depending on the part, 50–90 % of the voxels are air that carries no information but costs
storage, I/O and rendering time.

## Decision

Store sieved volumes as OpenVDB grids. OpenVDB keeps only active 8³ leaf nodes, so removed air
costs almost nothing. It provides file compression, level sets and distance fields, morphology,
connected components and meshing, and NanoVDB for GPU rendering. Blender and Houdini read VDB
files natively, which gives users an immediate way to look at results.

## Consequences

- The core is C++ (OpenVDB is a C++ library, MPL-2.0). Python bindings can follow via OpenVDB's
  own bindings or nanobind.
- CI currently builds on Ubuntu with the distribution package (OpenVDB 10). Windows and macOS
  builds need OpenVDB from vcpkg or source and are planned, not yet set up.
- The sieve's natural block size is the VDB leaf size (8³), which lets the algorithm stream the
  raw file in slabs of 8 slices.
