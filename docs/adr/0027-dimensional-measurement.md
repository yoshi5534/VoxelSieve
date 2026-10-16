# 0027: Dimensional measurement and the first article report

Status: accepted (2026-10-16)

## Context

A typical industrial use of CT is the first article inspection: the first parts of a new tool,
process or supplier are measured against every characteristic of the drawing, and the supplier
hands the customer a report with a cover sheet (part, drawing and revision, reason for the
submission), the list of characteristics with nominal value, tolerance, actual value and result,
the drawing with balloons and a decision. VoxelSieve had the nominal-actual comparison (ADR 0010):
a colour map of surface deviations after a best fit. It could not measure a diameter, a distance
or a position, could not align on the datums of a drawing and had no report of that shape. An AI
system driving VoxelSieve over MCP could therefore not produce such a report.

## Decision

`measure.hpp` measures an inspection plan on the surface mask (ADR 0009) of a scan, with the CAD
model placed by a comparison; `writeMeasurement` writes `measurement.json`, `characteristics.csv`
and two views with balloons. The operation `measure` wraps it; `first_article_report` writes the
report; the GUI method `cad_features` lists the geometry of a CAD model so that a client can name
elements without knowing triangle ids.

- **Coordinates.** The plan is in the coordinates of the CAD model, which are taken to be those of
  the drawing. Nominal elements come from the CAD model or are given explicitly.
- **Elements.** Planes, cylinders, spheres and surface points. On the CAD model they are found by
  region growing over triangles with smooth edges (`FeatureFinder`): a plane grows while the
  normals stay within 1.5°, a cylinder or sphere while the triangles stay within a tolerance of
  2·10⁻⁴ of the model diagonal of the refitted element. `detect` lists the features largest
  first; `probe(type, at)` grows the feature of a type nearest to a point, which is how a plan
  names an element (`"at": [x, y, z]`). This keeps plans readable and stable when the CAD model
  is tessellated anew.
- **Scanned points.** Points are the zero crossings of the mask's distance at full resolution with
  normals from its gradient, taken within `search_mm` (default five voxels) of the nominal
  element, with a normal within 30° of the nominal one, on the element's own triangles of the CAD
  model and `edge_margin_mm` (default two voxels) away from its edges, where the scan rounds them.
- **Fits.** Least squares (Gauss): planes by principal components, cylinders (5 parameters) and
  spheres by Gauss-Newton. Points beyond three standard deviations are left out and the fit
  repeated, twice. Form deviation is the spread of the points about the least-squares element.
- **Alignment.** `"best_fit"` keeps the comparison's alignment. A list of datums aligns the part
  on them in order of precedence (3-2-1 with plane, line, point, or RPS points with given
  directions): each datum's equations are solved in the space the earlier ones left free
  (lexicographic linearised least squares, Gauss-Newton), so datum A fixes what it can, B what A
  left, and so on; what no datum fixes stays at the best fit. The result states how many degrees
  of freedom the datums fixed.
- **Characteristics.** Diameter, radius, distance (optionally along an axis), angle, coordinate,
  position (diameter of the zone about the nominal axis or centre, in the datum frame), flatness,
  cylindricity, sphericity, form, parallelism, perpendicularity (zone of the points about the
  datum direction), profile (twice the largest deviation of the points from the CAD model) and
  the deviation of a point along its normal. The nominal value defaults to the one of the nominal
  elements; tolerances are upper and lower deviations, or `tolerance` (± for sizes, the zone width
  for form, orientation, position and profile). Without a tolerance a characteristic is listed for
  information only. Every characteristic keeps its number from the drawing; drawing zone, method
  and note are copied.
- **Report.** `firstArticleData` formats the measurement for `resources/first_article_template.html`
  (compiled in, replaceable like the test report template). Missing fields of the cover sheet are
  "not given" with a warning per field. The measurement result is conforming, not conforming (a
  characteristic out of tolerance or porosity beyond the limits of the order) or incomplete (a
  characteristic not measured); the customer's decision is left to the customer. A comparison and
  a porosity result of the same object are included when present.

## Consequences

- An MCP client can go from a scan and a CAD model to a first article report on its own:
  `cad_features` shows the nominal elements with ids and a picture, the client writes the plan
  from the drawing's characteristics, `run_measure` and `run_first_article_report` do the rest.
  The MCP prompt `first_article_inspection` describes that sequence.
- Limits that the report and the documentation state rather than hide: fits are least squares,
  not minimum zone (form values can be somewhat larger than a minimum-zone evaluation);
  orientation and position use the fitted axis or plane, not derived median lines; points come
  from the 4-bit surface mask, whose distance steps of 1/7 voxel make faces that lie exactly along
  a voxel axis look flatter than they are; the alignment is rigid (a non-rigid comparison, ADR
  0022, is not used for measuring); the measurement uncertainty of the CT system is not
  determined by VoxelSieve and comes from the order.
- No new dependency; the fits and the small eigenvalue problems are a few hundred lines.
