# 0041: Regions of a scan, drawn in the views and used by the analyses

Status: accepted (2026-11-24)

## Context

Inspection regions (ADR 0032) exist only inside an inspection order: the zones of the
acceptance limits name boxes, spheres, cylinders and meshes, and the report evaluates the pores
whose centre lies in each. In the browser UI there was no way to say "analyse only this flange"
or "leave out the clamping area": a region had to be typed as JSON into an order, and the
analyses themselves always covered the whole part. The workspace (ADR 0040) shows results as
layers of the scan, so regions belong there too.

## Decision

**A region is a step of the project** (`define_region`, output type `region`): a name, a colour
and the solids of ADR 0032 (`include`, `exclude`), in mm in the dataset's coordinates. The step
belongs to the scan object it was defined on, so the region follows the scan when the scan is
moved (ADR 0018); a solid `{"mesh_object": id}` takes a CAD model of the project with its
current pose. A later step with the same name replaces the region, one with `remove` deletes it;
both are steps of the protocol, so undo brings a region back. The step writes `region.json` and
the solids as closed STL meshes (`region.stl`, `excluded.stl`) for the views and for other tools.

**Analyses take a `region` parameter** with the same keys (`name`, `include`, `exclude`), copied
into the step, so the protocol says exactly where an analysis looked:

- Porosity: pores are told from the outside air through the connectivity of the whole part, so
  the analysis still runs on all of it. `restrictToRegion` then keeps the pores and zones whose
  centre lies in the region (the rule of the report's zones), removes the others from the pore
  labels and zone blocks, and takes the part volume inside the region (`regionPartVolumeMm3`), so
  the porosity is that of the region.
- Wall thickness: only the surface inside the region gets values and statistics; the rays still
  run through all of the part, so a wall that leaves the region is measured in full. This also
  saves the time of the vertices outside.

Other analyses (nominal-actual comparison, fibre orientation) take regions later; their
parameter will have the same form.

**The UI draws regions in the slice view**: a box is a rectangle dragged in the slice through
the whole depth of the scan, a sphere is dragged from its centre, a cylinder from its centre
along the slice normal through the whole scan; "Inside a CAD model" takes a mesh object. Solids
can be added or cut out. The slice view draws the lines where each region cuts the slice (the
solids tessellated in the browser as in `region.cpp`, meshes from the step's STL), the 3D view
draws them translucent over the part. Regions are a group of the layer tree with a switch each;
the analyses column has one "Where" choice (whole part or a region) for the analyses that take
one.

## Consequences

- A porosity analysis in a region costs as much as one of the whole part. Restricting the
  computation to the bricks of the region would need the outside air to be known first, which is
  the connectivity pass over all bricks; that is not worth it for the sizes seen so far.
- The inspection zones of an order still define their own regions; naming a project region from
  an order follows separately.
- Drawn solids are axis-aligned boxes and cylinders along a slice normal; other orientations are
  possible through the API or a mesh.
