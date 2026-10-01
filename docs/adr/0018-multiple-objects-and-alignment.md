# 0018: Several volumes and CAD models in one project, and their alignment

Status: accepted (2026-10-01); phases 1 and 2 implemented

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
- a **pose**: the rigid transform from the object's own coordinates to the global coordinate
  system of the project, in mm (`RigidTransform` from `compare.hpp`).

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
- **Global coordinates:** every project has one global, right-handed coordinate system in mm. It
  belongs to the project, not to any object: a project of volumes only has it as well as one with
  CAD models, and removing an object never moves the others. Every pose maps object coordinates
  to global coordinates, and all positions, measurements and views in the project are global.
- A new object is placed at the position its source states: an origin in the sidecar, or an
  offset in the file name as with the Me-163 sub-volumes, `(3072,4608,0)`. Without one it lies at
  the origin, with its own axes along the global axes.
- Nothing needs a CAD model. When one is there, the user may want results in part coordinates;
  that is one alignment step that moves all other objects so that the CAD model's pose becomes the
  identity, not a special kind of coordinate system.

Poses are rigid (rotation and translation). Scaling is not a pose: a wrong voxel size is fixed
in the voxel size (ADR 0012), not by stretching an object.

### Placing objects: alignment steps

An alignment moves objects relative to each other. Every alignment is an operation, so it is a
step with parameters, a result and a line in the protocol. It writes `pose.json` (artifact
`pose`): the objects it moved, the rigid **motion** applied to them in global coordinates, the
resulting poses, the target it was aligned to, the method and the fit residuals.

Alignments are applied **in order**. Each one starts from the poses the previous steps left and
applies its motion on top: pose ← motion ∘ pose. The pose of an object is therefore its initial
pose followed by the motions of all active alignment steps that moved it. Sequences such as these
are ordinary step lists:

- coarse by three picked point pairs, then fine by a surface best fit, then on datums;
- scan B to scan A, then CAD model to scan A, then scan C to scan B;
- the Me-163 sub-volumes placed by their offsets, then each one refined to its neighbour.

Undoing an alignment removes its motion and every later step, as for every other step (ADR 0008).
An alignment can move a set of objects together (all scans of one part, a part and its fixture),
so objects that already fit each other stay fitted.

| operation | moves | method |
| --- | --- | --- |
| `move` | objects | given motion: typed in, or the result of dragging in the 3D view |
| `align_points` | objects onto target | three or more point pairs picked in both; closed-form best rigid fit (Kabsch/Horn); residual per pair |
| `align_surfaces` | objects onto target | best fit of the surfaces: coarse by principal axes (optional), fine by robust point-to-plane ICP, as ADR 0010 |
| `align_datums` (later) | objects onto target | 3-2-1 or RPS on fitted features |

Each method starts from the current poses, so a later step refines an earlier one:
`align_surfaces` after `align_points` skips the coarse stage. A step only computes a motion from
the poses it sees; it records that motion, so repeating the protocol on the same data gives the
same poses.

`align_surfaces` works for every pair of kinds, because both sides reduce to a surface:

- a mesh is its triangles;
- a volume is its extracted surface (ADR 0009); aligning a volume needs a `surface` step on it
  first, and the studio offers to run it.

The moving side gives the points, the fixed side the distance: `MeshDistance` over the target's
mesh, or over the display mesh of the target's surface. For XXL volumes the display mesh is
already resampled to a triangle budget, and the fine fit reads only the sampled points; an
alignment reads the surfaces, never the grey values. The code of `compareToCad` is split into
`alignSurfaces` and the deviation measurement, so the comparison and the alignment share one fit.

An alignment moves the objects it names and nothing else. Objects aligned to a target earlier do
not follow when the target is moved later; to move them together, the later step names them too.
Poses stay readable on their own, and no hidden hierarchy changes positions behind the user's back.

A sequence of alignment steps can be saved as an **alignment recipe** (the steps' operations and
parameters, with objects as roles such as "scan" and "nominal") and applied to the next scan of
the same part. This matters for series inspection and comes after the operations themselves.

Greyvalue registration of two volumes (mutual information) is not part of this decision.
Industrial parts have a sharp surface, and a surface fit is faster, works out of core and matches
how nominal-actual comparisons align.

### Operations with several objects

An operation input can now be wired to an object: `{"object": "o2", "output": "surface"}` means
the latest active output of that type belonging to `o2`. Inputs not given are taken from the
**active object** (selected in the UI, or named in the API call), then from the latest output of
the type as today. A project with one object behaves exactly as now.

The nominal-actual comparison becomes an operation between two objects, a volume (its surface)
and a mesh, that uses their current poses and measures in global coordinates. It no longer aligns
on its own: alignment is the steps before it. The existing `compare_cad` with `cad_path` stays as
a shortcut that adds the mesh object and runs `align_surfaces` first. The
same measurement works between two volumes (the surface of one against the surface mesh of the
other), which gives the deviation between two scans of a part.

### Memory and data access

Poses are metadata. Viewing and processing several volumes reads each one through its own
`Dataset`, in its own coordinates, and transforms only the positions that are asked for:

- The studio's open datasets share **one** brick cache budget instead of one budget each, so
  opening a second volume does not double the memory.
- A slice tile is a rectangle in a plane of global coordinates. For each object shown in it,
  the pixel positions are transformed into object coordinates and sampled at the mip level that
  matches the zoom; an axis-aligned plane through a rotated volume is an oblique plane in that
  volume and touches up to about √3 times as many 8³ leaves. A volume whose axes lie along the
  global axes is sampled as today.
- Resampling a volume into another grid (fusion, difference volume, stitching of sub-volumes into
  one dataset) is an explicit operation that writes a new dataset with the streaming writer
  (ADR 0004). It is not part of this decision.

### Display

- **Slice view:** one volume is the base layer. Other volumes are drawn over it with an opacity
  and a colour, or as a checkerboard, and meshes as their cut line in the slice plane with the
  object's colour. The slice plane can follow the global axes or those of the active object.
- **3D view:** every visible object is drawn in global coordinates, with the global axes shown: meshes as surfaces, volumes
  ray-cast from their preview (at most 256³ each) with their own transfer function. The ray
  caster samples up to four volumes per ray in one pass, so overlapping volumes blend correctly.
- **Object list:** name, kind, visibility, colour, the pose and the alignment steps that
  produced it. Visibility, colour and transfer function per object
  are view state (ADR 0008 addendum), not steps; they are saved with the project and in saved
  views.

### API, MCP and project file

- Methods `objects` (list with poses), `object_add` (a dataset output or a mesh file), and
  `object_select`; the pose operations are ordinary `run_<operation>` methods. An AI agent can
  therefore add a CAD model, align it and compare it without the UI.
- `project.json` gets a format version. Steps get an optional `object` field; objects are
  recorded by the step that creates them (`object_add`, and every import). A project without
  objects opens as one object per imported dataset, with all its steps attached to it.
- The report lists the objects with their source and pose, and the alignment sequence with the
  method and residuals of each step, since a measurement in global coordinates is only traceable
  with the alignments that led to it.

### Order of work

1. Objects, global coordinates and poses in the project, `object_add` for datasets and STL
   meshes, `move`, shared brick cache, API/MCP, migration of old projects.
2. `align_points` and `align_surfaces` as steps in sequence; comparison between objects; 3D view
   with several objects.
3. Slice view with several volumes and mesh cut lines.
4. Later: datum alignment (3-2-1, RPS), alignment recipes, resampling operations (fusion,
   difference, stitching), several parts in one volume as objects that are regions of it.

## Consequences

- No new dependency: the fits need a 3×3 SVD or the quaternion method, a few lines on top of
  what `compare.cpp` already has.
- The step list stays the one source of truth; objects and poses are derived from it, so undo,
  redo, save-as and the protocol need no new mechanism, and an alignment sequence is simply part
  of the protocol.
- Every operation that takes a dataset now has to name the object it worked on; built-in
  operations get it from the input wiring, plugins too.
- Slice tiles of rotated volumes are slower than axis-aligned ones, since they touch more leaves
  and sample trilinearly.
- Mesh objects are held in memory; a CAD model of a few million triangles is some hundred MB with
  its BVH. That is acceptable for a few models; very large assemblies would need their own level
  of detail.

## Implementation of phase 1 (2026-10-01)

- `Step` has `object` and, on the step that created it, `object_name`; `project.json` is format 2.
  A format 1 project is migrated on open: every step that made a dataset from no object's data
  becomes an object, and steps whose inputs come from it belong to it.
- `Project::objects()` derives the objects and their poses from the active steps; the active
  object is stored in `project.json` (`active_object`) and is not a step. Object ids (`o1`, …) are
  never reused, also after undone steps were discarded.
- A step that moves objects records `moved_objects` and `motion` (row-major 4×4) in its summary
  and writes the same with the new poses to `pose.json`. `move` is the first such operation:
  rotation about an axis through a centre, then a translation, or `pose` for one object.
- `add_mesh` references an STL file as a mesh object (artifact `mesh`).
- `RigidTransform` moved to `transform.hpp`. Operations see the objects and their poses in
  `OperationContext::objects`; this changes the plugin ABI, so the plugin API version is 2.
- `BrickCache` holds one budget for the bricks of several datasets; the studio opens up to eight
  datasets on one 1 GB cache instead of two datasets with 512 MB each.
- Studio methods `objects`, `object_add`, `object_select`; every `run_<operation>` takes `object`
  and, for steps that create one, `object_name`. MCP lists them like every method.
- Every new object starts at the origin. Reading its position from a sidecar or a file name comes
  with the importers that know such conventions.

## Implementation of phase 2 (2026-10-01)

- `compareToCad` is split: `scanSurface` meshes the surface of a volume, `alignSurfaces` does
  the coarse (principal axes) and fine (robust point-to-plane ICP) fit between two meshes, and
  `fitRigid` the closed-form fit of point pairs (Horn's quaternion method; it rejects fewer than
  three pairs and pairs on one line). The comparison and the alignment share the fit.
- `align_points` and `align_surfaces` take the objects to move and a target, read the surfaces in
  global coordinates (the poses applied) and record their motion like `move`. Points of
  `align_points` are global coordinates where the objects lie when the step runs. A volume needs a
  surface step first; only its outer skin is fitted.
- `compare_objects` runs on the surface of a volume object (the step's object) against a nominal
  object, a mesh or another volume's surface, starting from where both lie
  (`actual pose⁻¹ ∘ nominal pose`); `refine` fits once more for the measurement only and moves
  nothing. Its output is a comparison like `compare_cad`'s, so the 3D view and the report show it
  the same way. `compare_cad` stays as it was.
- Operations see the outputs of every object in `OperationContext::objects` and the step's
  object in `OperationContext::object` (plugin API 3).
- The studio serves what the 3D view draws of an object (`Studio::objectShape`,
  `GET /api/object_mesh`): a mesh's triangles, the display mesh of a volume's latest surface, or
  the outline of the box a volume fills before it has one. The view "Objects" (`scene.js`)
  draws them with their poses, each in its colour, with the global axes, and picks points by
  ray casting on the CPU for `align_points`. `view_objects` renders the scene as a PNG with the
  CPU renderer for MCP clients. The volume ray casting of several volumes and the slice view with
  several objects remain for phase 3.
