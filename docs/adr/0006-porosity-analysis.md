# 0006: Porosity analysis

Status: proposed (2026-09-27)

## Context

Casting inspection asks two questions of a CT scan: where are the resolved voids (gas pores,
shrinkage cavities, "Lunker"), and where is the material loosened by voids too small to resolve
("Gefügeauflockerung"). The second shows up only as a slightly lower grey value, of the same order
as cupping from beam hardening. The analysis has to run on sieved datasets of several hundred GB
(ADR 0004), so it cannot hold the volume in memory, and it is scored against the ground truth of
synthetic scans (ADR 0005).

## Decision

`analyzePorosity` reads level 0 of a dataset brick by brick through `Dataset::forEachBrick` and
keeps only sparse state: voxels below the air/material threshold, statistics per 8³ block, and the
blocks near the surface. Memory grows with the voids and the part surface, not with the volume.

- **Pores:** voxels below the threshold are candidates. Candidates connected to removed air or to
  the volume border are outside air and flooded away; the rest form 26-connected components. A
  component of at least `min_pore_voxels` (3) is a pore. Its volume is the partial-volume void sum
  over the component dilated by one voxel, `(m - v) / (m - air)` per voxel against the local
  material level `m`, so edge voxels count with their fraction and the volume does not depend on
  the threshold.
- **Material level by depth:** cupping lowers the grey value with depth below the surface, by as
  much as a loosened zone. Each 8³ block gets a depth from a Dijkstra over blocks inside the part,
  seeded at the surface with sub-block precision from the part fraction of the surface blocks. The
  median block mean per 2-voxel depth bin gives the material level, interpolated linearly in depth
  and extrapolated toward the surface. Blocks that are not fully material, or that touch a pore
  shell, stay out of the reference.
- **Zones:** a block is flagged when its void fraction against the local level exceeds both
  `min_zone_void_fraction` (1 %) and `zone_sigma` (5) standard errors of a block mean. Flagged
  blocks form 26-connected zones of at least `min_zone_blocks` (2). A zone's void volume sums the
  flagged blocks and a one-block rim, minus a baseline from the next outer ring, so the soft edge of
  a zone counts and a residual cupping bias does not. Zones whose mean void fraction ends up below
  `min_zone_void_fraction` are dropped. Pore shells are left out of zone blocks, so no void counts
  twice.
- **Surface band:** voxels within 3 voxels of outside air are darkened by partial volume and the
  unsharpness of the scan. On faces that run obliquely through the blocks (cones, fillets, bores)
  a fully material block can hold a sliver of that edge and look a few percent void, which flagged
  hundreds of false zones on a thin-walled bell housing. Band voxels are left out of the material
  statistics and of the zone deficit; the deficit of a block at the surface takes the void
  fraction of its other voxels for the band's share of the part (band grey values against the
  local level, divided by one minus that void fraction). Added after the first release, when the
  sample parts grew oblique walls.
- **Part volume:** the material fraction of every kept voxel against the local material level plus
  the void of pores and zones. Porosity is void volume over part volume.
- **Output:** `porosity.json` (levels, noise, part volume, every pore and zone with centre, bounds
  and volume), projection images along x, y and z (thickness in grey, pores red, zones yellow) as
  PNG from our own minimal writer (stored deflate, no new dependency), and a VDB file with a
  "pores" and a "zones" grid in the dataset's world space for Blender or Houdini.
- **Threading:** per-thread accumulators (`tbb::combinable`), never a mutex around OpenVDB calls:
  OpenVDB runs nested TBB tasks, and a thread waiting on the lock can be the one that has to finish
  them.

## Consequences

- Against synthetic scans with noise and 10 % cupping: lunker volumes within 1 to 2 %, loosened
  zones inside the part within 1 to 10 %, total porosity of an 830 MB scan within 1 %, no findings
  on sound parts.
- Zones within about one block (8 voxels) of the surface are underestimated, because the reference
  there is extrapolated, the rim is cut by the surface, and blocks that are mostly surface band
  are skipped: on the bell housing such zones come out about 10 % low, deeper ones within 2 %.
- Strong ring artefacts (amplitude of the order of the noise) can produce small false zones of 2 to
  3 blocks; very steep cupping biases the part volume slightly. A ring correction before the
  analysis would be the fix, not a looser detection limit.
- Pores and zones are held in memory as sparse grids; a scan with millions of pores needs more
  memory than one with a few large ones. The per-pore shell grids could be streamed if that
  becomes a problem.
- The analysis assumes one material. Multi-material parts need a reference per material, which
  fits the depth bins as an additional key.

## Amendment (2026-10-19): a reference per material

On a woven carbon fibre laminate the resin pockets between the fibre bundles are darker than the
bundles, and as one material they came out as one zone of loosened microstructure over the whole
part (4.3 % "porosity" in a part without voids). `PorosityOptions::materials` takes a material
segmentation of the dataset (ADR 0013); the operation `porosity` wires the latest one as an
optional input (`per_material`, default true, turns it off) and `vs-porosity` takes
`--materials`.

- **Levels per material.** Each material gets its own level by depth, from the blocks whose
  material voxels are nearly all of that material (all but a sixteenth). A block's reference is
  the mix of the levels of the materials of its voxels, counted over the voxels that also enter
  its deficit (outside the surface band and the pore shells), so a resin skin at the surface does
  not lower the reference of the bundle behind it. The levels go into `porosity.json` as
  `material_levels`.
- **Variation of the material.** The grey value of a fibre bundle follows its fibre content,
  which varies from bundle to bundle without any void. The detection limit therefore also counts
  the spread of the block means of the block's materials around their levels (median absolute
  deviation), beyond what the noise explains. Without a segmentation nothing changes.
- **Limits.** Unresolved voids that lower a material below the threshold between two classes
  move its voxels into the darker class and are not seen as a zone; at a fibre content that
  varies by ±1.5 %, zones need about 5 % void to stand out. On the laminate, the zone void fell
  from 33 mm³ to 0.23 mm³, all of it in the dark arc of a cone beam artefact at the end of the
  scanned range.
