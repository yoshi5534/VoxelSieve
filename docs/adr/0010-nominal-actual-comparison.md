# 0010: Nominal-actual comparison with a CAD model

Status: accepted (2026-09-28)

## Context

The most common question to an industrial CT scan after porosity is how far the part deviates
from its nominal geometry: warpage, missing or excess material, worn or misplaced features. The
nominal geometry comes as a CAD model; STL is the one format every CAD system exports. The scan
and the CAD model do not share a coordinate system, and the part may lie in any orientation in the
scanner.

The surface of the scan already exists as a distance mask (ADR 0009). The comparison should
measure on that surface, run on scans larger than memory, and use no new dependency.

## Decision

### What is compared

- **Surface points:** the zero crossing of the distance mask, meshed without adaptivity so every
  vertex lies on the surface (`surfaceDisplayMesh` with adaptivity 0). Scans too large for the
  triangle budget are resampled coarser, as for display.
- **Deviation:** the signed Euclidean distance of each vertex to the CAD surface, positive outside
  the CAD model. A positive value means the part has more material than nominal; a negative value
  means material is missing. This is the usual convention of nominal-actual comparisons.
- **Internal voids:** only the largest connected surface, the outer skin, is compared by default.
  Surfaces of closed internal voids (pores) are measured by the porosity analysis (ADR 0006); here
  they would only add large negative deviations. Their count and area are reported.
- **Statistics:** they are weighted by area (a third of the adjacent triangle area per vertex), so
  that finely meshed regions do not dominate.

### Distance to the CAD model

The distance to the CAD model is exact: a bounding volume hierarchy over the triangles, with the
closest point on each triangle (Ericson). When the closest point lies on an edge or a vertex, the
angle-weighted normals of all triangles there decide the side. Meshes with inward winding
(negative volume) are turned around. A distance field of the CAD model was rejected, because it
limits accuracy to its voxel size at sharp CAD edges and needs a band wide enough for the coarse
alignment.

### Alignment

1. **Coarse:** the area-weighted centroid and principal axes of both surfaces (exact second
   moments per triangle, so coarse CAD triangulations work). All 24 proper assignments of the axes
   are candidates. If two principal moments are within 15 %, the part is close to rotationally
   symmetric, and rotations in 30° steps about the third axis are added.
2. **Ranking:** every candidate is scored on 200 points. The 16 best are refined briefly on 300
   points, and the 8 best distinct ones on 2000 points. The winner is chosen by its score on all
   fit points. The score is the mean distance with each point capped at five voxels. Unlike the
   median, it lets a small feature that breaks a symmetry decide, such as the keyway of the sample
   hub.
3. **Fine:** point-to-plane ICP on 20000 points sampled uniformly by area, with Huber weights
   (threshold twice the robust sigma, at least half a voxel) and rejection beyond five sigma. The
   transform is updated with the exact rotation of the solved rotation vector. The fit stops when
   no point moves by more than 1/2000 voxel.

`refine` starts the fine alignment from a given transform, and `none` uses that transform as it
is. The result is a best fit over the whole outer surface. Alignment on datums (3-2-1, RPS) is a
different, drawing-specific procedure and is left for later.

### Outputs

- `compare.json`: the transforms both ways, the fit, the statistics and a 40-bin histogram over
  ±range. The range is the 99th percentile of |deviation|, at least twice the tolerance, rounded
  to 1, 2, 2.5 or 5 × 10^n.
- `deviation.ply`: the scanned surface with a float `deviation` and an RGB colour per vertex, in
  binary PLY. MeshLab, ParaView, CloudCompare and Blender open it.
- `deviation_[xyz].png` are views along the axes. `cad_aligned.stl` is the CAD model in scan
  coordinates; it is optional, because CAD meshes can be large.

The colours are the same in the images, the PLY and the studio: green within the tolerance,
yellow to red above it and cyan to blue below it, saturated at ±range.

## Measurements

On synthetic scans of the sample parts (scale 0.3, 0.15 mm voxels, noise, cupping, unsharpness of
half a voxel), the CAD model was rotated by 40° about (1, 2, 3) and shifted by (30, -12, 7) mm:

| part | pose error (max over the part) | mean deviation | RMS deviation |
| --- | --- | --- | --- |
| housing | 0.066 voxel | 0.005 voxel | 0.14 voxel |
| bracket | 0.016 voxel | -0.029 voxel | 0.13 voxel |
| hub | 0.014 voxel | 0.006 voxel | 0.18 voxel |

The RMS is mostly real deviation: the unsharpness rounds the sharp CAD edges of these small
parts. The side walls of a box are measured without bias (-0.003 mm at 0.2 mm voxels), and a top
face 0.4 mm too high is measured as 0.400 mm.

On the full-size housing (520 × 400 × 220 voxels, 1 million surface points, CAD model with
7.7 million triangles), the comparison takes 8.6 s. A warped copy with a 0.4 mm dent shows the
dent at -0.400 mm.

## Consequences

- New algorithms that need the distance to a mesh can use `MeshDistance`.
- Wall thickness, datum alignment and tolerance zones per feature can build on this comparison.
- The report does not include the comparison yet.
