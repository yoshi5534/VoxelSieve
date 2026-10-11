# 0023: Analysis results per scanned object

Status: accepted (2026-10-11)

## Context

Every analysis writes its result as a step output (ADR 0008): pores and loosened zones, material
segmentations, surfaces, nominal-actual comparisons, reports, plugin tables. Apart from the
protocol, nothing listed them. Each place in the studio looked for "the latest X of this dataset"
on its own: the pores beside the slice view, the materials below them, the surface and the
comparison only in the 3D view, the last result again in the analysis cards. With several scans
in a project (ADR 0018) and repeated analyses, users could not tell where to find a result, which
one the views showed, or how to get back to an earlier one. Exports, such as DICONDE, need the
same list.

## Decision

- **Model:** an `AnalysisResult` (`project.hpp`) is an output of an active, finished step that is
  neither an object's data (dataset, mesh) nor a motion (pose). It names its step and output,
  type, operation, the object it belongs to, the dataset or mesh it was computed from (followed
  through the results it builds on), those results (`based_on`), its summary, and whether it is
  `current`: a newer result of the same type for the same object replaces it.
- Results are **derived from the steps**, like the objects, and not stored: undo and redo need
  nothing extra, and plugin outputs are results without registering anything.
- **API:** `Project::results(object)`; the studio method `results` (with the files of each
  output, optionally only current ones) and `project_status` carry them, so the UI and MCP clients
  see the same list. Exports walk this list.
- **UI:** a stage *Results* between analysis and report lists the results per object: current
  ones as cards with key figures, pictures, files and *Show in the view*, replaced ones folded
  below. The views show the result chosen there (kept in the view state, not a step), else the
  current one, and name what they show above the picture.

## Consequences

- One place answers "what do we know about this scan"; the analysis cards only point there.
- Which result the views show is presentation: it is part of the saved view state, not of the
  protocol.
- Results of no object (none today) are listed under the project.
