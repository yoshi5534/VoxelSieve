# 0012: Voxel edge length per axis and slice thickness

Status: accepted (2026-09-28)

## Context

Until now a dataset had one voxel size: cubic voxels. Many CT volumes are sampled differently
between slices than within them. The Me 163 volumes (ADR 0011's example data) have 0.33 mm
within a slice and 0.6 mm between slices; helical and line-detector scans, medical data and
stacks exported with a coarser z step look the same. Slices can also be thinner than their
spacing, which leaves a gap between them (DICOM: slice thickness below the spacing between
slices).

Treating such a volume as cubic stretches or squashes the part by the ratio of the pitches and
gets every volume, distance and position wrong along z.

## Decision

`VoxelSize` (`include/voxelsieve/voxel_size.hpp`) replaces the scalar voxel size everywhere:

- `pitch_mm[3]`: the edge length along x, y and z. The grid stays regular in voxel indices, so
  grey values are kept as they are (ADR 0002), and only measurements scale each axis by its own
  pitch.
- `slice_thickness_mm`: the slice thickness when it is below the pitch along z, 0 otherwise. It is
  recorded (dataset, surface, report) but not used for measuring: each voxel stands for its whole
  pitch, gaps included, which is what a reconstruction that interpolates between slices means.

A cubic `VoxelSize` converts from a number, so code and files with one voxel size keep working.

Files: `"voxel_size_mm"` stays a number for cubic voxels and becomes `[x, y, z]` otherwise;
`"slice_thickness_mm"` is added when it is set. Readers accept both forms, so all existing
datasets, surfaces and sidecars stay readable without a format version change. Older builds
reject the array form with a JSON type error rather than misreading it.

Where the pitch goes:

- VDB grids get a scale map per axis (`detail::voxelTransform`), so Blender, Houdini and every
  OpenVDB tool see the part in its true proportions. Coarser levels double every pitch.
- Porosity: pore and zone volumes use the voxel volume, positions are converted per axis, and the
  pore size (longest extent) is measured in mm per axis. The depth model for cupping stays in
  voxel units.
- Surface (`.vss`): the codes keep their distance in voxel units, so the triangulated zero
  crossing is unchanged; vertices are scaled per axis. The level set uses the mean edge for its
  values (its zero crossing does not depend on it).
- Nominal-actual comparison, report renders and the 3D view work in mm; projection and surface
  images are stretched to square pixels; the slice view draws voxels stretched to their true
  shape; `view_slice` reports the pixel width and height.
- Input: raw sidecars and the `voxel_size_mm` parameter take a number or `[x, y, z]`, the command
  lines `x,y,z`; TIFF stacks read the slice spacing of ImageJ files (`spacing=`), otherwise z
  gets the pixel width. `slice_thickness_mm` / `--slice-thickness` sets the thickness.

Synthetic scans from meshes (ADR 0005) keep cubic voxels: their mesh distance fields are OpenVDB
level sets, which need a uniform scale. The analytic phantom supports any pitch and is the ground
truth for the tests.

## Consequences

- Scans with a coarser slice spacing are measured correctly and look right in every view.
- Slices at irregular positions or missing slices are not covered: the grid must be regular.
  Such data would need a z position per slice and resampling or a position table in every
  measurement; that can be added on top of `VoxelSize` if real data needs it.
