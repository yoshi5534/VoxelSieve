# 0003: Remove only outside air

Status: accepted (2026-09-27)

## Context

Internal air (pores, blowholes, closed cavities) is exactly what inspection measures. A global
threshold that drops every air voxel would destroy it.

## Decision

The sieve removes only air that is connected to the volume boundary:

1. Estimate the air/material threshold from a histogram of a downsampled copy.
2. Stream the raw file in slabs of 8 slices and classify each 8³ block as air, material or boundary
   from its min/max.
3. Flood-fill air blocks from the volume boundary; air blocks not reached are internal and stay.
4. Dilate the kept region by a margin of a few voxels towards the air, so sub-voxel surface
   determination still sees air values.
5. Write the kept blocks with their original grey values as VDB leaf nodes, plus metadata.

## Consequences

- Fixtures and holders that touch the part are kept as material; handling them is future work.
- Noise and ring artefacts in the air can create false material blocks; the threshold and block
  classification must be robust against them and are tested with the noisy phantom.

## Implementation status

`voxelsieve::sieve` (src/sieve.cpp) implements steps 1–5 on a volume held in memory. Blocks are
classified in parallel per slab of 8 slices; the margin is applied with OpenVDB's
`dilateActiveValues` (18-neighbourhood), so it is at least `margin_voxels` wide around every kept
block. Streaming the raw file from disk (memory-mapped) for volumes larger than RAM is a follow-up.
