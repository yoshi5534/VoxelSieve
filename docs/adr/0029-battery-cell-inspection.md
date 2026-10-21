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
- **Anodes as dark as the separator.** Hard carbon and graphite anodes often do not stand out
  from the separator and electrolyte between the cathodes. Then all layers found count as
  cathodes, and the anode is the band between neighbouring cathodes beyond their ends, where it
  still stands out against the electrolyte: its end is where that band drops to half way
  between its level just beyond the cathode ends and the level beyond the cathodes themselves.
  Where the band does not stand out by a share of the cathode contrast in most sections of an
  edge, that edge's overhang is reported as not measured, with a warning.
- **Particles and voids where the electrodes are.** Particles are looked for between the anode
  ends along the axis and from half a layer pitch inside the innermost to half a pitch beyond the
  outermost layer; voids inside the shortest cathode and not in the first and last turn, where
  the electrolyte at the strip ends looks like gas. Bright components longer than
  `max_particle_size_mm` are tabs, foil ends or welds, counted apart.
- **Section images.** Cells are sometimes handed over as images of sections instead of the
  volume: longitudinal sections through the axis every few degrees and cross-sections along it
  (`battery_images`, `vs-battery --images`, read from a folder or a ZIP archive with libpng and
  libzip). The cross-sections form a volume in which every slice repeats the nearest image;
  housing and layers are measured in the slices that hold an image. The overhang comes from both
  halves of each longitudinal section, the right half at its angle and the left one opposite,
  with the same layer tracking as in a volume. Particles and voids are components in each image,
  dark pixels connected to the core excluded, and a finding seen in several images counts once;
  they have an area, no volume. The pixel size is the span of the cross-section positions over
  the height of the longitudinal sections, unless given. The longitudinal section at 0° is taken
  to run along the x axis of the cross-sections; positions along the axis count from the bottom
  row of the longitudinal sections.
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
