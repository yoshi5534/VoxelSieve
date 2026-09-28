# 0008: Studio — projects, operations, plugins, MCP and a browser UI

Status: accepted (2026-09-27)

## Context

The command-line tools cover the pipeline from raw volume to report. Before the first release the
project needs an application around them:

- a minimalistic UI with a wizard: choose a dataset, run operations, create the report;
- every step logged and undoable; projects saved at any time;
- operations loadable as plugins;
- usable by AI systems, so an MCP interface;
- an own display besides Blender: a fast slice view first, a simple 3D viewer second;
- always datasets of several hundred GB (ADR 0004).

## Decision

### One engine, three front ends

A C++ engine (`voxelsieve/studio.hpp`) owns projects and operations. It is driven through one
JSON API (method name + JSON parameters → JSON result). The same API is exposed

- to the browser UI over HTTP (`vs-studio`, local server, Boost.Beast),
- to AI systems over MCP on stdin/stdout (`vs-studio --mcp`), each API method a tool,
- to C++ code and tests directly.

The UI therefore cannot do anything an AI agent cannot, and the MCP tools are exercised by the
same tests as the UI.

### Projects and the step log

A project is a directory with `project.json` and one directory per step. A step records the
operation, its parameters, its inputs (other steps' outputs or files), its outputs, start and end
time, status and messages; this list is the protocol shown in the UI and printed in reports.

Operations never modify their inputs; each writes new outputs into its own step directory.
Undo moves a cursor back and marks later steps as undone, redo moves it forward: both are instant,
whatever the data size. Running a new operation after an undo discards the undone steps and their
outputs. `project.json` is written atomically after every change, so a project is always saved;
"save as" copies the directory. Input volumes are referenced by path, never copied.

### Operations and plugins

An operation declares an id, title, description, typed inputs and outputs (`raw`, `dataset`,
`porosity`, `report`) and a JSON schema for its parameters, from which the UI builds its forms
and MCP its tool schema. Built-in operations wrap the existing library: import of a raw volume
into a dataset (sieve), porosity analysis, report.

Plugins are shared libraries that export `voxelsieve_plugin_api_version()` and
`voxelsieve_register_operations(OperationRegistry&)`, loaded from `--plugins <dir>` or
`VOXELSIEVE_PLUGIN_PATH`. They link against `libvoxelsieve` and must be built with the same
compiler and VoxelSieve version (checked through the API version); a C ABI for plugins in other
languages can come later.

### Browser UI

The UI is plain HTML, CSS and JavaScript with WebGL2, compiled into `vs-studio` like the report
template, with no build step and no JavaScript dependencies. A browser UI keeps the data where it
is (the workstation or a server next to the scanner) and needs no GUI toolkit; it is testable with
a headless browser.

The slice view never loads a volume. It requests tiles of 256 × 256 pixels for an axis, a slice
index and a zoom level; the server cuts them from the dataset level whose voxel size matches the
zoom (ADR 0004 mip levels), so a view of a 500 GB scan reads a few bricks. Pores and zones are
drawn as an overlay. The 3D viewer ray-casts the overview grid (at most 256³) in WebGL2.

## Consequences

- New dependency: Boost.Beast and Boost.Asio, header-only and part of the Boost headers already
  installed for Boost.Iostreams; no new package.
- `libvoxelsieve` is built as a shared library so plugins and `vs-studio` share one copy.
- The server listens on localhost by default. Serving to other machines needs authentication and
  is not part of this decision.
- Undone outputs stay on disk until they are discarded; a project can grow large. The UI shows
  the size per step.
- The C++ plugin ABI ties plugins to a VoxelSieve build. That is acceptable while plugins are
  written by us or by close partners; a stable C ABI is future work.

## Addendum (2026-09-27): slice view data access

Measured on an 830 MB scan: a slice tile at level 1 took 1.2 to 3.2 s on first access, because the
brick cache reads whole bricks (a dense 256³ brick is 50 to 70 MB on disk) to show one plane of
them. The viewer therefore opens datasets with `BrickLoading::kOnAccess`: only the topology of a
brick is read at first and the values of an 8³ leaf on first access, from the memory-mapped file.
A tile then reads about 1/32 of a brick and takes 0.1 to 0.2 s on first access and about 2 ms from
the cache. Algorithms that visit whole bricks keep `BrickLoading::kFull`. The cache re-measures
bricks loaded on access whenever it loads one, so its memory budget still holds.

Tiles go to the browser as float32 grey values plus one overlay byte per pixel and are windowed in
the browser, so contrast changes need no new requests. For MCP, `view_slice` renders a whole slice
as a PNG; PNGs are now compressed with zlib through Boost.Iostreams, which was already a
dependency.

The 3D view loads the finest level whose largest dimension is at most 256 voxels as 8-bit grey
values plus the overlay (`readVolumePreview`, at most 32 MB) and ray-casts it in WebGL2; on the
830 MB scan this is level 3 (129 × 97 × 66, 1.6 MB, 40 ms).
Colour and opacity per grey value come from a transfer function that the browser keeps as a
256-entry lookup texture; its histogram is counted from the same 8-bit preview, so editing it needs
no server round trip. Opacities hold per 1/128 of the largest axis, so the picture does not change
with the level shown.

Addendum (views): the project also stores how it is shown, so it opens as it was left. The UI
sends its view state (stage, slice viewer, camera, transfer function, colours, background) with
`view_set` shortly after each change; `project.json` keeps it under `view`. Named views
(`view_save`) keep such a state with a PNG of the canvas in `views/<id>.png` under
`saved_views`. Views are presentation, not processing: they are no steps, undo does not touch
them, and they can be saved while an operation runs on its copy of the project (the finished copy
takes the views over before it replaces the project). The state is opaque JSON to the core,
limited to 256 kB; pictures must be PNG. Suggested renderings are computed in the browser from the
histogram of the preview (Otsu split into air and material, the material peak and its width,
further prominent peaks), since they need no data beyond it.
