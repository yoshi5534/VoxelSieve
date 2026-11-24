# 0040: One workspace for analyses, views and results, with layers

Status: accepted (2026-11-24)

## Context

After a scan opened (ADR 0039) the browser UI had five stages: Dataset, Analysis, Results,
Report and View. The Analysis stage listed every operation that takes a dataset (about 15, from
denoising over battery cells to the DICONDE export) as equal cards with their parameters
folded away. A result was on the Results stage, its picture on the View stage. What the views
showed was set in a row of controls that mixed things of different kinds: one list for the
rendering held the grey value modes (surface, transfer function, maximum intensity projection)
next to results (extracted surface, nominal-actual deviation, wall thickness); next to it boxes
"Pores" (pores and zones together) and "Materials", background and video memory. The slice view
had one box for pores, zones, materials and the thickness map together.

## Decision

**Analysis, results and the views are one workspace in three columns**: objects and layers on
the left, the view (slice, 3D, objects) in the middle, the analyses and the history of results on
the right. The stages are Scan, Analysis (the workspace) and Report. Saved views and project
states that name the old stages open in the workspace.

**What the views show follows one model of layers**, the same in the slice and the 3D view
(`state.layers`, saved with the view state):

| Layer | Rule | Entries |
| --- | --- | --- |
| Grey values | one style (3D) | solid surface, translucent (transfer function), maximum intensity projection, extracted surface mesh |
| Colour by | one at a time | grey value, materials, nominal-actual deviation, wall thickness |
| Overlays | any number | pores, loosened zones, other objects |

A colouring that lies on the surface only (deviation, wall thickness by rays) replaces the grey
values in 3D; the slice says so, or shows the thickness map when the analysis made one. Without
a choice the materials of a segmentation colour the part, as before. Only results that exist are
listed. Selecting an entry shows its properties under the tree: the window or rendering style of
the grey values, the materials with their colours and switches, the list of pores (a click shows
the pore in the slice), the colours of pores and zones, the other objects of the slice view.
Background and video memory moved into "View settings" of the 3D view.

Pores and zones are separate switches in both viewers (`VolumeViewer.zones`,
`SliceViewer.setShow`); views saved before keep their one switch for both.

**The analyses come in groups in the order of the work**: Prepare, Segment, Defects, Geometry,
Reports and export, then Plugins (operations no group names). Each card shows one sentence; open,
it shows the main settings (`MAIN_PARAMETERS`, else the required ones), the rest under "More
settings", Run, and the key figures of its latest result with "Show" (its layer on) and
"Details" (the history). An analysis that needs a result the project lacks says which. The
suggested analyses (ADR 0039) not yet done stand above the groups. Imports, object operations
(the Objects view) and the test report (its stage) are no analyses here.

The protocol of steps stays, as a column that can be hidden (header button "Protocol"); it
starts hidden on screens narrower than 1600 pixels, where the three columns need the room.

## Consequences

- The UI is still plain JavaScript over the same API methods; no method changed.
- Inspection regions as objects of the project, which analyses can be limited to, follow as
  their own decision; the tree and the analysis cards are where they will appear.
