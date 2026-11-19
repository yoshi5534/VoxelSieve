# 0033: Beam hardening correction of reconstructed volumes

Status: proposed (2026-11-17)

## Context

The X-ray spectrum of an industrial CT is polychromatic, and material absorbs its soft part
first: the beam gets harder on its way through the part. The measured projection then grows less
than linearly with the path length, and the reconstruction shows

- **cupping**: material is darker far from the surface than near it, by 10 to 30 % in thick
  aluminium or steel castings;
- **streaks**: dark bands between dense or thick regions, along the directions of the longest
  paths.

Both move grey values by more than the contrast of small pores and loosened zones, so every
threshold-based analysis suffers. The porosity analysis compensates cupping for itself, comparing
blocks with the material level at the same depth (ADR 0006), but the sieve, the surface
(ADR 0009), the material segmentation (ADR 0013), the wall thickness and the views all see the
uncorrected values. The scanner software can correct beam hardening before the reconstruction
(linearisation with a calibration of the spectrum or a step wedge), but VoxelSieve gets the
reconstructed volume, often without the projections, the spectrum or the reconstruction
geometry. The correction therefore has to work on the volume alone, on datasets larger than
memory (ADR 0004), and must keep the grey values lossless in the input's type (ADR 0021).

## Decision

`correctBeamHardening` (`beam_hardening.hpp`), the operation `beam_hardening` and the tool
`vs-beam-hardening` write a new dataset with the same bricks and active voxels
(`writeDerivedDataset`, as denoising in ADR 0025). The correction is estimated on a coarse level
of the dataset, the finest one with at most `resolution` (256) voxels along its largest edge,
which fits in memory for any scan; it is interpolated trilinearly to level 0 brick by brick.
Beam hardening varies slowly, so a few hundred voxels across describe it.

### Reprojection (default)

Empirical, image-based beam hardening correction after Kyriakou et al. (Med. Phys. 37, 2010): a
polychromatic projection q relates to the monochromatic one p by a monotone curve, so p is a
polynomial in q, p = q + c2 q² + c3 q³ + ... . Filtered back projection is linear, hence the corrected volume is
the input plus c2 FBP(q²) + c3 FBP(q³) + ..., and q is the projection of the input itself.

- Every slice across the rotation axis (`rotation_axis`, default z) of the coarse level is
  projected as a parallel beam (`ParallelBeam`: bilinear ray sums over 180°, as many angles as
  pi/2 times the slice edge, bins of the smaller pitch) of the grey values above air, outside air
  set to zero. The projections are normalised to their largest value, raised to the powers 2 to
  `order` (3) and reconstructed with a Ram-Lak filter.
- The weights c_k are those that make the material most uniform: least squares of grey value +
  sum c_k b_k = m over the voxels above the threshold and more than one and a half coarse voxels
  below the surface. Four rounds leave out voxels more than three robust sigmas (1.4826 times the
  median absolute deviation) from the fit, so a second, denser material and pores do not count as
  hardening. The fit is closed-form; it needs neither a spectrum nor material constants.
- **Noise.** The powers reconstructed from a slice's own projection carry that slice's noise,
  ramp filtered, and the fit then used them to cancel the noise of the material instead of the
  hardening: with noise of a twelfth of the contrast the cupping was reduced from 22 % to 13 %
  only. Blurring the reconstructed powers, or projecting a smoothed model of the material, helped
  against the noise but bent the correction near edges and dense inclusions. Each slice is
  therefore projected from the mean of its two neighbouring slices: their noise is nearly
  independent of its own, while the part and its hardening change little from slice to slice.
- Cone-beam angles are neglected (parallel beam per slice), and the rotation axis has to be one
  of the volume axes, which it is for the usual reconstructions.

### Depth

For a scan whose rotation axis is unknown or not a volume axis: the material level by depth below
the surface (`measureCupping`) is measured on the coarse level, and the grey value above air of
every voxel is scaled by the level at the surface over the level at its depth. Depth is the exact
Euclidean distance (separable transform in mm, anisotropic pitch) to the outside air, which is
the removed air and the voxels below the threshold connected to it. This corrects cupping of one
material, but neither streaks nor hardening that depends on the path length rather than the
depth.

### Cupping profile

`measureCupping` reports the median material grey value per depth bin (one coarse voxel wide;
the outermost bin is partial volume and left out) and the cupping, (surface level - core level) /
(surface level - air), with the core level the median of the levels in the deepest fifth of the
depth range. The operation and the tool report it before and after the correction, with the
spread of the material grey values in the fit and the weights.

Outside air for the depth is seeded from removed voxels only: the sieve found it block by block,
robustly. Seeding also from noisy voxels below the threshold at the volume border let the
outside reach into a part that the border cuts through, and the measured depth stopped at 1.4 mm
instead of 2.9 mm.

## Measurements

A cylinder of 8 mm diameter (attenuation 0.5/mm) with two dense rods (1.5/mm, 1.6 mm diameter)
and a bore along the axis, projected, hardened as q = (1 - exp(-0.15 p)) / 0.15 and reconstructed
by filtered back projection; grey values 2000 + 20000 per 1/mm, plus Gaussian noise. Error: the
standard deviation of the difference from the reconstruction without hardening, in the material
away from surface, rods and bore (a shift of the whole material does not count); it contains the
noise. 4 cores, 2026-11-17.

160 x 160 x 24 voxels of 0.06 mm, estimated on level 0:

| Noise | Input | Order 2 | Order 3 | Order 4 | Depth |
| --- | --- | --- | --- | --- | --- |
| 0 | 598 | 236 | 57 | 52 | 211 |
| 300 | 668 | 387 | 315 | 315 | 417 |

512 x 512 x 128 voxels of 0.019 mm, noise 800, estimated on level 1 (256 x 256 x 64):

| | Input | Order 2 | Order 3 | Order 4 | Depth |
| --- | --- | --- | --- | --- | --- |
| Error | 996 | 835 | 807 | 807 | 1059 |
| Cupping | 22.9 % | -2.1 % | 0.8 % | 0.5 % | 0.4 % |
| Seconds | | 12.3 | 14.9 | 18.2 | |

- Order 3 leaves less than 1 % of the contrast beside the noise (807 against a noise of 800, 57
  without noise); order 2 under-corrects the long paths through the rods, order 4 adds nothing.
- The depth method flattens the cupping profile as well as the reprojection, but keeps the
  streaks between the rods: its error is no better than that of the input.
- Estimated on level 1, the correction is interpolated to level 0. Within the outermost coarse
  voxel or two below the surface, where the coarse level mixes material and air, the correction
  is extrapolated and up to 3 % too weak (unit test `CorrectionFromACoarseLevelIsInterpolated`).
- Time goes into the projection and the ramp-filtered reconstructions of the coarse slices,
  about 0.8 s per slice of 256² on one core with order 3; at the default resolution a scan of any
  size takes well under a minute on 16 cores, plus one pass over its bricks to apply it.

## Consequences

- Correction is a step of its own that writes a new dataset; it doubles the disk space of a scan
  while both are kept. Later steps of the object use the corrected dataset.
- The grey values of the material rise toward the level of short paths, and the coarse levels are
  rebuilt; integer grids clamp to their range, so `value_type` float is the choice for scans near
  the top of the 16-bit range.
- Several materials of very different density follow one polynomial; a fit per material (the
  two-material form of the method, q1^i q2^j) needs a segmentation first and is the next step if
  real scans of mixed parts show residual streaks.
- No real scan with a known reference was available; the measurements are synthetic. Rings, scatter
  and metal artefacts are not corrected.
