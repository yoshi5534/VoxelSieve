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
- Volume-based thickness maps (thickness at every material voxel from the distance transform)
  and the comparison with nominal thicknesses from the CAD model are not covered yet.
