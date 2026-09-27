# 0002: Lossless float voxels

Status: accepted (2026-09-27)

## Context

CT grey values are usually 16-bit unsigned integers. OpenVDB 10 has no unsigned 16-bit grid type.
`HalfGrid` (OpenVDB 11+) halves memory, but half precision has an 11-bit significand: above 2048
the step between representable values grows to 2, 4, … up to 32 near 65535. That is usually below
the scanner noise, but it is a silent, value-dependent quantisation.

## Decision

Store grey values in `FloatGrid`. A 32-bit float represents every 16-bit integer exactly, so the
conversion is lossless. Disk size is reduced with OpenVDB's Blosc/zip compression.

## Consequences

- In-memory size per active voxel is twice that of `uint16`. Since only the part and a margin stay
  active, this is still far below the dense raw volume for typical parts.
- A lossy `half` export for viewers can be added later as an explicit option.
- Revisit if memory becomes the bottleneck: a custom `uint16` tree is possible but is not readable
  by Blender or Houdini.
