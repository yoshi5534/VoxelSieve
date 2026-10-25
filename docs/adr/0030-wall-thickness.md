# 0030: Wall thickness

Status: accepted (2026-10-24)

## Context

Castings, mouldings and printed parts are designed with minimum and maximum wall thicknesses: a
wall that came out too thin fails under load or leaks, a wall that is too thick is a hot spot
that shrinks and forms porosity. A CT scan is the only non-destructive way to see every wall of
a part, including those inside, but VoxelSieve could only compare the surface with a CAD model
(ADR 0010), which shows where a face lies, not how thick the wall behind it is. Wall thickness
needs no CAD model and is the second analysis, after porosity, that a casting is inspected for.

Two definitions are in use. The **ray** thickness is the distance from a surface point along
the inward normal to the opposite face of the wall, as a calliper would measure it on parallel
faces. The **sphere** thickness is the diameter of the largest sphere inside the material that
touches the surface at the point; it is defined everywhere, also at ribs and in corners, and is
how a designer reads "local wall thickness". Both are wanted, for different parts.

## Decision

`thickness.hpp` measures the wall thickness on the vertices of the surface mesh extracted from
the distance mask (ADR 0009, `scanSurface`); the operation `wall_thickness` and the tool
`vs-thickness` wrap it.

- **Surface.** The mesh comes from the `.vss` surface of the dataset (a surface step of the
  project, or extracted on the fly), within a triangle budget (default 2 million). Vertex normals
  are area-weighted and smoothed once over their neighbours; their orientation is decided by
  the majority against the grey value gradient. All connected surfaces are used to limit the
  walls, so an internal pore thins the wall it lies in, but by default only the largest one, the
  outer skin, gets values (`outer_surface_only`).
- **Ray method (default).** From each vertex the start is moved onto the iso crossing of the
  grey values along the normal, then the ray marches inward in steps of half the smallest voxel
  pitch through the trilinearly interpolated grey values, read through the dataset's brick cache
  (`Dataset::Sampler`). The opposite face is where the grey values fall below the surface value
  and stay below for one more step (so one noisy voxel does not end the wall), interpolated to
  sub-voxel accuracy. The face must face along the ray: its outward normal (against the grey
  value gradient) must lie within the search angle of the ray (default 30°). A ray that leaves
  through an edge or runs along a rib fails that test and leaves the point unmeasured instead of
  reporting a length that is not a wall.
- **Sphere method.** The shrinking ball algorithm (Ma, Bae and Oude Elberink 2012) on the mesh
  vertices with a k-d tree: a ball of half the maximum thickness touching the vertex is shrunk
  until no other surface point lies inside. Neighbours that lie inside only by the noise of the
  surface (seen with the vertex under less than 30° from the centre) do not shrink it.
- **Range.** Walls up to `max_thickness_mm` are measured; thicker ones stay unmeasured (NaN).
  By default it is half the middle extent of the part's bounding box.
- **Statistics and regions.** Statistics are weighted by the surface area around each vertex
  (minimum, mean, percentiles 1, 5, 50, 95, 99 %, histogram). With a minimum or maximum wall,
  the connected surface regions beyond it are listed with their area, extreme value and location.
- **Output.** `wall_thickness.json` (parameters, statistics, regions), `thickness.ply` (the
  measured surface in mm with a `thickness` property and colours per vertex, NaN where not
  measured) and two rendered views. The colour scale runs from red (thin) over yellow and green
  to blue (thick); magenta marks walls below the minimum, violet above the maximum, grey points
  without a value. The browser UI shows the same scale as a 3D view mode
  (`GET /api/wall_thickness`).
- **Report.** The report step takes a wall thickness as optional input. The inspection order
  gets a `wall_thickness` section: `min_mm`, `max_mm`, and `max_area_below_min_mm2` to tolerate
  small thin spots (such as sharp edges) instead of judging the single thinnest point. Statistics
  and regions are recomputed from the PLY with the order's limits, so an order can be evaluated
  without measuring again. The criteria form a block of their own in the assessment and count
  for its verdict.
- **Scale.** Only the mesh lives in memory (bounded by the triangle budget); the grey values are
  read along the rays through the brick cache, with the vertices sorted by brick and processed
  in parallel so that one task reads few bricks (ADR 0004).

## Consequences

- The ray method is accurate on walls with roughly parallel faces and leaves points at ribs,
  edges and steep tapers unmeasured; the share measured is reported so that a low coverage is
  visible. Widening the search angle measures more points at the cost of slanted lengths.
- The sphere method is defined at ribs and corners, but at convex edges it is inherently small
  (the ball must fit between the two faces of the edge), so edges show as thin. It is the method
  for "local thickness" maps and is not the default for limits; `max_area_below_min_mm2` lets an
  order tolerate edge regions.
- Thickness is measured on mesh vertices, so features smaller than the mesh spacing (about one
  voxel within the triangle budget) are sampled, not resolved. Larger scans need a larger budget
  or a coarser mesh.
- The comparison with nominal thicknesses from the CAD model: see the second amendment.

## Amendment (2026-10-24): thickness map of the whole part

The surface tells how thick a wall is where it can be seen from outside; a material
accumulation inside a casting (a hot spot where a wall meets a flange or a boss) shows only as
a slightly thicker surface value nearby. `writeThicknessMap` (`thickness.hpp`) writes the local
thickness of every material voxel, the diameter of the largest sphere inside the material that
contains it (Hildebrand and Rüegsegger 1997), as a float dataset `thickness_map/` beside the
other outputs. The operation `wall_thickness` writes it with `volume_map`, `vs-thickness` with
`--volume-map`.

- **Material.** Voxels at or above the iso value of the surface (`.vss`) are material; removed
  voxels are air. Pores are air, so they thin the material around them.
- **Method.** Per tile with a halo of the largest diameter: the exact Euclidean distance
  transform of the material (Felzenszwalb and Huttenlocher, separable, with the pitch per axis,
  ADR 0012), then the ridge of the distance field as sphere centres, and every sphere painted
  with its diameter onto the voxels it covers, keeping the largest. Voxels beyond the volume
  count as material so that a part cut by the scan's edge is not thinned there.
- **Ridge.** Painting every voxel's sphere is far too slow; only voxels whose distance rises to
  no neighbour by more than 0.9 of the step (per mm) are centres. Strictly a ridge point rises
  by less than 1; the 0.9 drops most of the ragged near-ridge of a noisy surface (the housing
  sample: from 18 s to 8 s) at the price of a few voxels whose covering sphere is missed; every
  voxel is therefore initialised with its own sphere. The result is a slight underestimate
  there, below a voxel.
- **Convex edges.** By definition the largest sphere at a convex edge is tiny, so the edge of
  every plate would read as a thin region. Ridge branches that run into an edge (a steady rise
  of at least 0.5 per mm) are followed up to their top and painted with that diameter, so an
  edge belongs to the wall it bounds. Concave corners and fillets keep their larger values: they
  are where material accumulates.
- **Range.** Thicknesses are computed up to `max_thickness_mm`; thicker material gets that value
  and counts as capped. The default is the smallest extent of the part (from its bricks), which
  no sphere exceeds, but at most 64 voxels of the finest pitch: time and memory grow with the
  cube of the halo. The value is `2d - p/2` for a distance `d` to the nearest air voxel centre
  and the finest pitch `p`, so a wall of n voxels reads n voxels thick.
- **Scale.** Bricks are written one at a time through `writeDerivedDataset` (ADR 0004); a tile is
  a brick, halved until tile and halo hold at most 2^26 voxels, so memory stays bounded for any
  scan. Within a tile the transforms and the painting run in parallel over lines and planes.
- **Statistics and regions.** Volume-weighted statistics (minimum, mean, percentiles, histogram)
  and a fine histogram in `thickness_map.json`, from which the report recomputes shares for the
  limits of an order. Regions thinner than the minimum or thicker than the maximum are the
  26-connected components on a grid of 8³ voxel blocks (coarser for large scans), with volume,
  extreme value and location. `thickness_map_[xyz].png` are slices through the thickest spot.
- **Report and order.** The wall thickness section of the report gets "Thickness inside the
  part": statistics, the three slices, shares beyond the order's limits and the regions when the
  order's limits are those of the analysis. With a `max_mm` in the order the criterion
  "Thickest spot inside the part" is added; a map that reached its maximum fails it, since it
  only shows that the material is at least that thick.
- **GUI.** The slice view colours the material by the map of the step with the scale of the
  surface (`GET /api/tile?...&thickness=<step>` appends one float32 thickness per pixel).

Consequences: the map is a second, volumetric reading of thickness, defined everywhere, and the
one to find material accumulations; the surface methods stay the ones for the limits of thin
walls. Its run time grows with the volume of material and with the cube of the maximum
thickness, so thick blocks of material are capped rather than measured.

## Amendment (2026-10-24): nominal thickness from the CAD model

A wall of 2.4 mm passes a minimum of 2 mm, but it is a defect if the drawing says 3 mm: a core
that shifted in the mould makes one wall thinner and the opposite one thicker, both within the
absolute limits. With a CAD model aligned to the scan (ADR 0010, ADR 0018), `addNominalThickness`
(`thickness.hpp`) measures the nominal thickness of the model at every point of the measured
surface and the deviation of the actual thickness from it.

- **Where.** Every vertex of the measured surface is mapped into the model's coordinates by the
  transform of the comparison (`cad_to_scan`) and the nearest point of the model is taken
  (`MeshDistance`, exact, with its bounding volume hierarchy). The nominal thickness is measured
  there with the outward normal of the model's triangle; vertices farther than the measuring range
  from the model get none.
- **Same method.** The nominal thickness uses the method of the actual one, so both answer the
  same question. Ray: sphere tracing along the inward normal through the exact distance to the
  model up to the opposite face, which must face along the ray within the same search angle.
  Sphere: the shrinking ball against the exact distance to the model, stopped by the same rule
  for points close to the touching point as on the scan, so that convex edges read alike (without
  it, edges read 1 to 2 mm thicker on the scan than on the model).
- **Deviation.** Actual minus nominal per vertex where both exist, its area-weighted statistics
  against a tolerance, and the connected regions thinner or thicker than nominal by more than the
  tolerance, largest first. Where the actual wall lies elsewhere than in the model, a ray can reach
  a different wall on the two (it runs along a side wall on one and through the wall on the other);
  this shows as a large deviation on a narrow strip, so regions are ranked by area and an order can
  tolerate an area.
- **Output.** `thickness.ply` gets the float properties `nominal` and `deviation`,
  `wall_thickness.json` a `nominal` section, and `thickness_deviation_view_[12].png` show the
  deviation in the colours of the comparison (green within the tolerance, blue thinner, red
  thicker). The browser UI shows them as an option of the wall thickness view
  (`GET /api/wall_thickness?nominal=1`).
- **Operation.** `wall_thickness` takes a nominal-actual comparison as an optional input; like
  every optional input it is wired to the latest comparison of the object, so a wall thickness
  after `compare_cad` or `compare_objects` is compared with nominal unless `nominal_thickness` is
  false. The default tolerance is twice that of the comparison, since each face may deviate by
  it. A non-rigid comparison (ADR 0022) contributes its rigid alignment only. `vs-thickness
  --nominal <dir>` takes the output of `vs-compare`.
- **Report and order.** The wall thickness section gets "Wall thickness against the CAD model":
  deviation statistics, views, histogram and the regions. The order's `wall_thickness` section
  takes `nominal_tolerance_mm` (criteria: the largest shortfall and excess within it) and
  `max_area_outside_nominal_mm2` (criteria: the area thinner and the area thicker than nominal by
  more than the tolerance, each within it). An order with `nominal_tolerance_mm` for an analysis
  without a CAD model is an error.

Consequences: the deviation from nominal finds walls that are wrong for their place, which the
absolute limits do not. It depends on the alignment: a model placed off by 0.1 mm shifts the
faces, not the thicknesses, so it is less sensitive to alignment than the surface deviation, but
a wrong orientation compares the wrong walls. Measuring the nominal thickness costs about as much
as the actual one (the housing sample with a CAD model of 1.9 million triangles: 12 s for a
million vertices on 4 cores).
