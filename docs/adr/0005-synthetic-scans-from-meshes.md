# 0005: Synthetic CT scans from meshes

Status: proposed (2026-09-27)

## Context

The porosity analysis that comes next needs test data with a known answer: real parts, defects of
known size and position, and the noise and artefacts of real scans. Real scans come without ground
truth, and the box phantom (ADR 0003) is too simple to develop an analysis on. The tool must also
scale: an analysis for XXL scans has to be tested on XXL volumes.

## Decision

`vs-synth` turns a closed STL mesh into a raw volume plus a JSON sidecar with the ground truth.
`SyntheticScan` is a `VolumeSource`, so voxels are computed on demand and a scan of any size can be
streamed to disk or straight into `writeDataset`.

- **Geometry:** OpenVDB's `meshToLevelSet` gives a narrow-band signed distance field at the voxel
  size, and `meshToSignedDistanceField` a coarse field (at most 128 voxels along the longest axis)
  with a filled interior for depth queries. No new dependency; STL is read by our own small parser
  (binary and ASCII). Units are mm.
- **Lunker (shrinkage cavity):** a core sphere plus 2 to 5 overlapping lobes inside an enclosing
  radius, which gives an irregular cavity. Placed at random where the depth leaves room for the
  enclosing sphere. Its void volume is estimated by Monte Carlo (200,000 samples, about 0.5 %).
- **Gefügeauflockerung (loosened microstructure):** a spherical zone filled with non-overlapping
  small pores up to a target porosity, clipped to the material. Pores are usually near or below the
  voxel size; they lower the grey value rather than appear as single pores. Void volume is exact.
- **Grey values:** the linear partial-volume model of the box phantom. Voxels touched by a lunker
  are supersampled 3³. Loosening pores subtract their exact overlap with the voxel (disk-rectangle
  area per slice, integrated over z by Gauss-Legendre), so even pores of a tenth of a voxel keep
  the total void volume right (tested to 0.2 %, the rounding to integers).
- **Artefacts:** Gaussian noise; cupping from beam hardening as darkening with depth below the
  surface (`1 - c * (1 - exp(-depth / L))`); ring artefacts as Gaussian rings of random radius and
  amplitude around the z axis through the volume centre (the rotation axis).
- **Determinism:** all random numbers come from a counter-based hash of the seed, so the same seed
  gives identical voxels on every platform and for any region order.
- **Ground truth:** the sidecar holds the mesh volume, every defect (type, centre, enclosing
  radius, lobes or pore count, void volume) and the total porosity.

## Consequences

- An analysis can be scored against exact numbers: void volume per defect, total porosity, and
  defect positions.
- The fine distance field lives in memory: about 7 voxels across the surface. That limits the part
  surface, not the volume size; very large parts at very small voxel sizes may need a bricked
  distance field later.
- Not modelled yet: streaks and metal artefacts, cone-beam artefacts, scatter, detector blur
  (the partial-volume ramp stands in for it), gas pores as separate spheres, and defects placed
  from a file. They can be added to `SyntheticSpec` without changing the output format.

## Addendum (2026-09-28): unsharpness and sample parts

Real scans have no sharp one-voxel partial-volume edges: focal spot, detector and reconstruction
filter blur them. `SyntheticSpec::blur_sigma_mm` convolves the grey values with a Gaussian point
spread function before rings and noise are added. The blur keeps the sum of the grey values, so
volumes stay exact, and regions are computed with a halo so the result does not depend on how the
volume is split. `voxelsieve/parts.hpp` adds sample castings (housing, bracket, hub) defined as
signed distance functions, so defect and surface analyses are tested on realistic geometry
(ADR 0009).
