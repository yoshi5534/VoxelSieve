# 0029: Battery cell inspection

Status: accepted (2026-10-20)

## Context

CT is the standard non-destructive inspection of lithium-ion cells, from development samples to
incoming goods and failure analysis. The questions differ from those of castings: whether the
anode reaches beyond the cathode at every edge of the electrode stack (anode overhang; a cathode
that sticks out lets lithium plate at the anode edge), whether the layers are complete, evenly
spaced and in contact, whether the housing is round or swollen, and whether metal particles or
gas sit between the layers. VoxelSieve had porosity, materials and dimensional measurement, none
of which knows about electrode layers.

## Decision

`battery.hpp` inspects one cell per dataset; the operations `battery_inspection` and
`battery_report` and the tool `vs-battery` wrap it. `cells.hpp` writes synthetic cells with known
geometry (`CellSource`, `vs-battery --cell`) to test it against.

- **Cell type and axis.** On a coarse level of the dataset the bounding box of the cell is taken
  from per-slice counts of material voxels, robust against noise in the air. A cross-section that
  fills about π/4 of its box is round: the cell is cylindrical (wound electrodes in a can), its
  axis the long axis of the box. Otherwise it is stacked (pouch or prismatic, sheets or a flat
  winding), the normal the thinnest axis. Both can be given. Axes are volume axes; tilts of a
  few degrees are followed because every section fits its own centre and layer positions.
- **Levels.** Grey value levels of air, the gaps between the layers (separator and electrolyte),
  the two electrodes and the housing come from the profiles: peaks of the layers split into two
  classes by 2-means, the brighter being the cathode unless told otherwise (a resolved copper foil
  can outshine the cathode coating). Without two distinct classes there is no overhang.
- **Housing and layers.** A cylindrical cell gets cross-sections along its axis, each with
  radial profiles (default 360), averaged over a few voxels along the layers against noise. A
  circle fitted to the outer surface gives diameter, out-of-roundness and dents; the first band
  gives the wall (full width at half maximum), the first crossing from the axis the core. Peaks
  with enough prominence above the noise (estimated from the spread between parallel rays) are
  the layers; count, pitch and gaps (a spacing above `gap_factor` times the median with a dark
  middle) follow. A stacked cell gets profiles along the normal on a grid across the cell.
- **Anode overhang.** Sections across each edge of the electrode stack (both ends of a
  cylindrical cell over the angle, the four sides of a stack along the edge) follow every layer
  from the middle to its end, where its grey value drops to half way between layer and gap. The
  overhang of a cathode end is the distance to the end of its nearer anode neighbour, positive
  when the anode reaches further. Neighbours count only at the usual layer spacing, so the start
  of a winding is not misread. Per edge: minimum, mean, maximum, spread of the cathode and anode
  ends (telescoping, shifted sheets) and the worst layer end.
- **Particles and voids.** Inside the electrode stack, voxels above the particle threshold (as
  far above the brighter electrode as that lies above the gaps) and below the void threshold
  (half way between air and gaps) are labelled as connected components with volume, extent,
  equivalent diameter and position in cell coordinates.
- **Output.** `battery.json` and PNG images: the cross-section with layers, particles and voids
  marked, the longitudinal section and the section with the smallest overhang at every edge.
- **Report.** `battery_report` (`vs-battery --order`) fills a template of its own
  (`resources/battery_report_template.html`, compiled in) with the order fields of the test
  report (ADR 0007), the cell per section, the overhang per edge with a chart of the smallest
  overhang per section against the required minimum, the particles and voids, and an assessment
  against the requirements in `order.battery` (overhang range, edge spread, layer count,
  diameter, length, thickness, swelling, roundness, wall, gaps, particle and void count and
  size); see `examples/battery_order.json`.
- **Scale.** Only the sections, profiles and the electrode stack for particles are read, through
  the brick cache (ADR 0004); sections run in parallel.

## Consequences

- The cell axis or stack normal must lie roughly along a volume axis; a strongly tilted cell has
  to be resampled first. Several cells in one scan need one dataset each (`isolate_part`).
- The overhang needs the electrodes told apart by grey value. At voxel sizes near the layer
  thickness the layers blur into each other, the levels say so and the overhang stays empty.
  Tab welds, the current collectors at the ends and the electrolyte filling are not measured.
- The thresholds and the layer model are tested on synthetic cells; the defaults will need
  adjusting on real scans of different cell formats.
