# 0032: BDG P 203 and freely defined inspection regions

Status: proposed (2026-11-16)

## Context

The report evaluated porosity after the scheme of BDG P 202 (ADR 0007): per inspection zone, an
axis-aligned box or the whole part, limits on the largest pore extent, the pore count, the
porosity and loosened microstructure. Drawings of castings inspected by CT increasingly specify
porosity after BDG P 203 (porosity analysis by industrial CT, 2019). It names characteristics of
single pores and of volumes, combined into a porosity key such as `BDG P203 - PROI2/ Øaq3/ da3/
UØaq1` that is written next to a region on the drawing:

- volume porosity of the whole part (PG), of a freely defined region (PROI) and of a geometric
  reference volume, a cube unless agreed otherwise, placed inside the part where the porosity is
  highest (PV);
- per pore: volume (Vp), diameter of the enclosing sphere (Øp, the default for Ø) and of the sphere
  of equal volume (Øaq), sphericity (Ψ) and compactness (C);
- distance between neighbouring pores (Gap) and of a pore to the surface (da), whose computation
  the guideline leaves to the parties, noting that distances of enclosing circles are unsuitable;
- the number of pores in a region (Z), and pores to disregard (U plus a characteristic).

Regions on such drawings follow functional surfaces: sealing faces, bores, bosses. Boxes in scan
coordinates do not describe them, and the CAD model that does lives in the project as a mesh
object with its own pose (ADR 0018).

## Decision

- **Regions** (`region.hpp`): an inspection region is the union of included solids minus the
  union of excluded ones; no included solid means the whole part. Solids are boxes, spheres,
  capped cylinders between two points and closed meshes (STL with a pose). Meshes answer
  inside/outside from an OpenVDB level set of 256 voxels along their longest extent. In a project,
  a solid may name a mesh object (`mesh_object`); the operations replace it by the object's STL
  and the pose from its coordinates to those of the scanned volume, so regions drawn on the CAD
  model follow alignment and moves. `inspection_regions` writes every region as a mesh and adds
  it as an object, so the slice and 3D views show where it lies before the report is made.
- **Membership and part volume** stay as in ADR 0007: pores and loosened zones belong to a zone
  by their centre; the part volume of a region is the material per 8³ block weighted by the
  block's overlap with the region (exact for one box, else from 4³ samples where the block
  crosses the region's boundary). Where that boundary also runs through a block at the part's
  surface, the material sits on one side of the block and the overlap undercounts it (a region
  ending at the surface lost a fifth of its volume in a test); with the analysed dataset at hand
  (the report operation, `vs-report`) those blocks count their voxels inside the region instead.
- **Pore characteristics** are measured in the porosity analysis and stored per pore:
  - Øp: Welzl's smallest enclosing sphere of the centres of the pore's boundary voxels, plus one
    voxel pitch (a sphere of radius r has boundary voxel centres out to about r - 0.5 voxels);
  - surface area: faces between pore and non-pore voxels times 2/3, the mean ratio of a smooth
    surface to its voxel staircase; Ψ from it and the volume of the voxels below the threshold;
    C as pore volume over the volume of the enclosing sphere. The guideline itself warns that Ψ
    and C depend on the resolution;
  - da: the material between the pore's voxels and the nearest air voxel that touches the part
    (removed or outside air), the shortest distance between the voxel cubes, found by a
    nearest-node search through both OpenVDB trees (`detail/voxel_distance.hpp`). The air
    voxels next to the part are collected in the pass that sums the material per block, only in
    blocks at the surface.
  Results written before this ADR lack them; criteria that need them refuse such results with a
  request to rerun the analysis instead of silently passing.
- **Gap** is computed at evaluation from the pore labels: the smallest distance between the voxel
  cubes of two counted pores in the zone, with the same tree search, so neighbouring pores never
  come out at negative distances.
- **PV** places the reference volume (cube, cuboid, sphere or cylinder along an axis) on a grid of
  a sixteenth of its size wherever it may cover a void, in windows of 32³ placements, and keeps the
  highest void fraction among the placements whose voxels lie in blocks full of part (material
  and voids fill 95 % of the block) and inside the zone's region. The part is only known per
  block, so a placement keeps up to one block (8 voxels) off the surface; the report states the
  placement found. References of more than 64 voxels per axis are evaluated on cells of several
  voxels. The guideline's default, the largest cube that fits the wall, is not implemented: the
  order must give the size (`reference_volume`).
- **Porosity key**: `porosity_key` takes the key as written on the drawing, with the guideline
  prefix, decimal commas, `Ø`/`Ψ` or ASCII stand-ins (`Dp`, `Daq`, `D`, `Psi`). Every parameter
  is also a field of the zone (`max_enclosing_diameter_mm`, `min_surface_distance_mm`, ...,
  `disregard`), and explicit fields win over the key. Interpretations where the guideline leaves
  room: disregarded pores count for no characteristic of the zone, porosity included; porosities
  include the void of loosened microstructure, as for P 202; Z counts the pores of the region.
- **Report**: each zone shows its region, the key, the disregarded pores and per criterion where
  the value was found (pore number, pair of pores, placement of the reference volume). The pore
  table adds Øp, Ψ and da. The guideline defaults to "BDG P 203" when a zone uses its parameters.
  No text of the guideline is reproduced.

## Consequences

- Orders written for P 202 evaluate as before; P 202 and P 203 limits can be mixed in one zone.
- The porosity analysis costs an extra visit of the surface blocks and a distance search per pore;
  on the test phantoms it is a few percent of the analysis.
- PV near surfaces is conservative by up to a block; a voxel-exact placement would need the
  dataset during the search.
- Regions are evaluated by pore centres; a pore straddling a region boundary counts once, on the
  side of its centre. Distances (da, Gap) are measured between voxels below the threshold, so
  they lean towards the material side by up to half a voxel per surface.
