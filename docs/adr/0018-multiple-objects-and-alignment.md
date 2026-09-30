# 0018: Several volumes and CAD models in one project, and their alignment

Status: proposed (2026-09-30)

## Context

A project (ADR 0008) holds one scan today. Its steps form a chain: import, sieve, porosity,
surface, comparison, report. A CAD model appears only as a parameter of the nominal-actual
comparison (ADR 0010), which aligns it on its own and keeps the transform in its result.

Real inspections need more than one object:

- a scan and one or more CAD models (the part, a fixture, a mating part);
- several scans of the same part: before and after a load test, two machines, two settings;
- several scans of one large object, such as the Me-163 sub-volumes, that lie next to each other;
- one scan with several parts in it, each compared with its own CAD model.

These objects have to be placed relative to each other: by hand, by picked point pairs, by a best
fit of their surfaces and later on datums (3-2-1, RPS). The constraints stay the same as for one
scan: every change is a logged, undoable step; everything is available through the JSON API and
MCP; volumes of several hundred GB are never loaded, and two of them must not need twice the
memory of one.

## Decision

### Objects

A project gets a list of **objects**. An object is a thing the user sees and places: a volume or
a mesh. It has

- a stable id (`o1`, `o2`, …) and a name;
- a kind: `volume` or `mesh`;
- a source: the step output it was created from (a dataset, or an imported mesh);
- a **pose**: the rigid transform from the object's own coordinates to project coordinates, in
  mm (`RigidTransform` from `compare.hpp`).

Results derived from an object belong to that object and share its coordinates: the surface,
porosity result and materials of a volume, the display mesh of a CAD model. Moving an object
moves them with it. A step records the object it worked on (`Step::object`), so the protocol and
the report can say which object each result is about.

Objects are not a new kind of data. They are a view of the step list, like everything else in
the project: the object list and all poses are computed from the active steps. Undo and redo
therefore stay a cursor over the steps (ADR 0008) and need nothing new.

### Coordinates

- **Object coordinates** are those the data already has: voxel index times the voxel pitch per
  axis (ADR 0012) for a volume, the file's mm for a mesh. They never change; no voxel and no
  vertex is moved or resampled to place an object.
- **Project coordinates** are those of the **reference object**, whose pose is the identity. It
  is the first object added, until the user chooses another one. A CAD model is the natural
  reference, because results are then in part coordinates as on the drawing; choosing it is one
  step that re-expresses all poses and leaves the objects where they are relative to each other.
- A new object starts at the identity, or at the position its source states: an origin in the
  sidecar, or an offset in the file name as with the Me-163 sub-volumes, `(3072,4608,0)`.

Poses are rigid (rotation and translation). Scaling is not a pose: a wrong voxel size is fixed
in the voxel size (ADR 0012), not by stretching an object.

### Placing objects: the alignment operations

Every change of a pose is an operation, so it is a step with parameters, a result and a line in
the protocol. The operation writes `pose.json` (artifact `pose`): the object, the new pose, the
object it was aligned to, the method, and the fit residuals. The pose of an object is that of its
latest active `pose` step.

| operation | inputs | method |
| --- | --- | --- |
| `set_pose` | object | given transform: typed in, or the result of dragging in the 3D view |
| `align_points` | object, target | three or more point pairs picked in both objects; closed-form best rigid fit (Kabsch/Horn); residual per pair |
| `align_surfaces` | object, target | best fit of the surfaces: coarse by principal axes, fine by robust point-to-plane ICP, as ADR 0010 |
| `align_datums` (later) | object, target | 3-2-1 or RPS on fitted features |

`align_surfaces` works for every pair of kinds, because both sides reduce to a surface:

- a mesh is its triangles;
- a volume is its extracted surface (ADR 0009); aligning a volume needs a `surface` step on it
  first, and the studio offers to run it.

The moving side gives the points, the fixed side the distance: `MeshDistance` over the target's
mesh, or over the display mesh of the target's surface. For XXL volumes the display mesh is
already resampled to a triangle budget, and the fine fit reads only the sampled points; an
alignment reads the surfaces, never the grey values. The code of `compareToCad` is split into
`alignSurfaces` and the deviation measurement, so the comparison and the alignment share one fit.

Aligning puts the pose of the moving object in project coordinates at alignment time. Moving the
target later does not move objects aligned to it; poses are not a hierarchy. This keeps every
pose readable on its own. Moving objects together (groups) can come later as a pose step on
several objects.

Greyvalue registration of two volumes (mutual information) is not part of this decision.
Industrial parts have a sharp surface, and a surface fit is faster, works out of core and matches
how nominal-actual comparisons align.

### Operations with several objects

An operation input can now be wired to an object: `{"object": "o2", "output": "surface"}` means
the latest active output of that type belonging to `o2`. Inputs not given are taken from the
**active object** (selected in the UI, or named in the API call), then from the latest output of
the type as today. A project with one object behaves exactly as now.

The nominal-actual comparison becomes an operation between two objects, a volume (its surface)
and a mesh, that uses their current poses: alignment `none` by default when the user has placed
the CAD model, `auto` when it is new. The existing `compare_cad` with `cad_path` stays as a
shortcut that adds the mesh object first. The same measurement works between two volumes (the
surface of one against the surface mesh of the other), which gives the deviation between two
scans of a part.

### Memory and data access

Poses are metadata. Viewing and processing several volumes reads each one through its own
`Dataset`, in its own coordinates, and transforms only the positions that are asked for:

- The studio's open datasets share **one** brick cache budget instead of one budget each, so
  opening a second volume does not double the memory.
- A slice tile is a rectangle in a plane of project coordinates. For each object shown in it,
  the pixel positions are transformed into object coordinates and sampled at the mip level that
  matches the zoom; an axis-aligned plane through a rotated volume is an oblique plane in that
  volume and touches up to about √3 times as many 8³ leaves. The reference volume is sampled as
  today.
- Resampling a volume into another grid (fusion, difference volume, stitching of sub-volumes into
  one dataset) is an explicit operation that writes a new dataset with the streaming writer
  (ADR 0004). It is not part of this decision.

### Display

- **Slice view:** one volume is the base layer. Other volumes are drawn over it with an opacity
  and a colour, or as a checkerboard, and meshes as their cut line in the slice plane with the
  object's colour. The slice plane can be the axes of the project or of the active object.
- **3D view:** every visible object is drawn in project coordinates: meshes as surfaces, volumes
  ray-cast from their preview (at most 256³ each) with their own transfer function. The ray
  caster samples up to four volumes per ray in one pass, so overlapping volumes blend correctly.
- **Object list:** name, kind, visibility, colour, the pose and how it came about (the pose
  step), and which object is the reference. Visibility, colour and transfer function per object
  are view state (ADR 0008 addendum), not steps; they are saved with the project and in saved
  views.

### API, MCP and project file

- Methods `objects` (list with poses), `object_add` (a dataset output or a mesh file), and
  `object_select`; the pose operations are ordinary `run_<operation>` methods. An AI agent can
  therefore add a CAD model, align it and compare it without the UI.
- `project.json` gets a format version. Steps get an optional `object` field; objects are
  recorded by the step that creates them (`object_add`, and every import). A project without
  objects opens as one object per imported dataset, with all its steps attached to it.
- The report lists the objects with their source, pose, the method that placed them and its
  residuals, since a measurement in project coordinates is only traceable with its alignment.

### Order of work

1. Objects and poses in the project, `object_add` for datasets and STL meshes, `set_pose`,
   shared brick cache, API/MCP, migration of old projects.
2. `align_points` and `align_surfaces`; comparison between objects; 3D view with several objects.
3. Slice view with several volumes and mesh cut lines.
4. Later: datum alignment (3-2-1, RPS), groups, resampling operations (fusion, difference,
   stitching), several parts in one volume as objects that are regions of it.

## Consequences

- No new dependency: the fits need a 3×3 SVD or the quaternion method, a few lines on top of
  what `compare.cpp` already has.
- The step list stays the one source of truth; objects and poses are derived from it, so undo,
  redo, save-as and the protocol need no new mechanism.
- Every operation that takes a dataset now has to name the object it worked on; built-in
  operations get it from the input wiring, plugins too.
- Slice tiles of rotated volumes are slower than axis-aligned ones, since they touch more leaves
  and sample trilinearly.
- Mesh objects are held in memory; a CAD model of a few million triangles is some hundred MB with
  its BVH. That is acceptable for a few models; very large assemblies would need their own level
  of detail.
