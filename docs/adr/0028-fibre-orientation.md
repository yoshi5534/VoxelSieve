# 0028: Fibre orientation

Status: accepted (2026-10-19)

## Context

Fibre-reinforced parts (woven or non-crimp fabrics, laminates, short-fibre mouldings) are
inspected for the direction of their fibres as much as for voids: a ply laid at the wrong angle,
fibre bundles that wash out, waviness or a missing ply weaken the part without any void. At
typical resolutions of 5 to 20 µm the single carbon or glass fibre (5 to 15 µm) is not resolved,
but the fibre bundles are, and their texture streaks along the fibres. VoxelSieve could separate
bundles from resin (ADR 0013) but not say where the fibres point.

## Decision

`orientation.hpp` estimates one fibre direction per block of the dataset (default 16³ voxels)
from the gradient structure tensor; the operation `fibre_orientation` wraps it.

- **Structure tensor.** The grey values are smoothed with a Gaussian (σ = 1 voxel by default)
  and differentiated with central differences in mm, so that voxels need not be cubes (ADR 0012).
  The outer products of the gradients are summed over the analysed voxels of a block. Along the
  fibres the grey value hardly changes, so the eigenvector of the smallest eigenvalue is the
  fibre direction. The coherence 1 − λmin/λmid is 0 for isotropic structure and near 1 for
  aligned streaks; blocks below `min_coherence` (resin, noise) stay out of the families.
- **Material.** With a material segmentation only the voxels of one material count, by default
  the one with the highest grey value (the fibre bundles next to the resin). Without one, every
  voxel above the air threshold counts. Blocks with less than a quarter analysed voxels are left
  out.
- **Families.** The directions of the coherent blocks are grouped by axial k-means (a direction
  and its opposite are the same, the distance is 1 − |cos|) into a given number of families,
  weighted by analysed voxels: two for a woven fabric, one per ply direction for a laminate. Each
  family has its mean direction, its share and the mean angle of its blocks to it. The
  second-order orientation tensor a_ij = ⟨p_i p_j⟩ and its eigen decomposition summarise the
  whole part as used by micromechanics.
- **Output.** `orientation.json` (families, tensor), `orientation.vdb` with the grids `direction`
  (Vec3s), `coherence` and `family` at block resolution in the dataset's world space, and three
  slices through the middle coloured by direction (|x| red, |y| green, |z| blue, paler with lower
  coherence) over the grey values.
- **Scale.** The dataset is read one row of blocks at a time with a halo for the smoothing, rows
  in parallel, so memory stays bounded by a few rows (ADR 0004).

## Consequences

- A block gives one direction, so bundles thinner than a block or crossing inside one are
  averaged; a smaller block resolves them at more noise. The local direction of single fibres
  needs a resolution that resolves them and is not attempted.
- Families come from k-means with a given count; a wrong count merges or splits families, which
  the spread and share make visible. Choosing the count automatically is left open.
- Fibre volume content per family, waviness along a bundle and the comparison with the nominal
  layup of a ply book are not covered yet.
