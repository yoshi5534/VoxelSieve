# 0007: Inspection report from a template

Status: proposed (2026-09-27)

## Context

A porosity analysis (ADR 0006) is only useful to a foundry or a test lab once it ends in a test
report: who ordered what, which part, how it was scanned, what was found, and whether the part
meets the limits from the drawing. Labs accredited under DIN EN ISO/IEC 17025 must state a fixed
set of information in every report (section 7.8), and a CT test report under DIN EN ISO 15708-3
adds the scan settings. The acceptance limits for castings usually follow BDG P 202 (volume
deficits of castings), with tighter limits in functional zones such as sealing faces. Every lab
has its own report layout.

## Decision

`vs-report` analyses a dataset, evaluates it against the limits of an inspection order and
renders a report from a template.

- **Inspection order (JSON):** laboratory, customer, order, part, scan settings, measurement
  uncertainty, approval and the acceptance limits. Anything VoxelSieve cannot measure comes from
  the order. Mandatory fields that are missing appear in the report as "not given" and are
  listed as warnings, so a report is never silently incomplete.
- **Evaluation (BDG P 202 scheme):** inspection zones are axis-aligned boxes in dataset
  coordinates or the whole part. Per zone, optional limits: largest pore extent (longest edge of
  its bounding box), number of pores above a minimum size, porosity (pores plus loosened zones
  relative to the part volume in the zone), and whether loosened microstructure is allowed. Pores
  and zones belong to the inspection zone that contains their centre. The part volume of a box is
  the material per 8³ block, weighted by the block's overlap with the box, plus the voids in it.
  The limits themselves come from the drawing or customer; VoxelSieve ships none.
- **Template:** a Mustache subset (variables, sections, inverted sections, comments), implemented
  in about 200 lines instead of adding a dependency. The built-in English A4 template is compiled
  in from `resources/report_template.html`; `--template` replaces it and `--print-template`
  prints it as a starting point. The report is one self-contained HTML file with the projection
  images as data URIs, printed to PDF from the browser. `report.json` holds the same data for
  other tools.
- **Norm texts:** the standards are not freely available and not reproduced. The template uses
  their structure and our own wording for field names.

## Consequences

- A lab can adapt the layout, language and logo without recompiling; the data model is the
  contract (documented by `examples/inspection_order.json` and `report.json`).
- No PDF library: page breaks and headers depend on the browser's print engine. A PDF writer can
  be added later behind the same data model.
- Inspection zones are boxes. Zones from CAD surfaces or masks, and registration of the scan to
  the CAD coordinate system, are future work; until then zone boxes are given in scan coordinates.
- Criteria beyond P 202 size, count and porosity (pore distance, pores near the surface, pore
  density per area on a cut plane) can be added as further optional limits.
- The report states detection limits (smallest pore, zone detection limit) from the analysis
  settings. The measurement uncertainty must come from the lab, which has to determine it on
  reference parts; synthetic scans (ADR 0005) validate the method but not a specific device.
