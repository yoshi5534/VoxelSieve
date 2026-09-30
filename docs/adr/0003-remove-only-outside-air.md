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

## Amendment (2026-09-30): where outside air enters

A part that the first and last slice cut through (a pipe, a long part scanned in sections, a
drill core) is open towards those slices. Seeded from all six faces, the flood fill then runs into
the part through its ends and removes every inner region below the threshold, block by block; in
the slice view this shows as 8-voxel steps inside the part. The first such scan (LoDoInd, a pipe
with organic fillings barely above air) lost most of its fillings this way.

`outside_air_axes` (`SieveOptions`, `DatasetOptions`, `vs-sieve --air-from`, parameter of
`import_raw` and `import_tiff`) names the axes whose two boundary faces let outside air in. The
default stays `xyz`; `xy` keeps everything inside the pipe wall. The choice is recorded in
`index.json` when it is not the default.
