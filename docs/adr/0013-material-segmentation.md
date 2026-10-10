# 0013: Material segmentation by grey value

Status: accepted (2026-09-29)

## Context

Assemblies consist of several materials: the Me 163 scan (ADR 0011) holds aluminium sheets and
dense steel parts, and its published labels mark each component. Analyses per material (volume,
porosity or wall thickness of one material, colour in the views) need a segmentation into
materials. The sieve (ADR 0003) separates only air from material. The target labels of the Me 163
dataset are component instances, numbered anew in every sub-volume, not material classes.

A learned segmentation may come later. It needs a baseline to beat and a way to score it, and
the core should stay free of machine-learning dependencies.

## Decision

A classical segmentation (`include/voxelsieve/materials.hpp`, `segmentMaterials`):

- **Material or air.** A voxel is material when it is above the air threshold and at least
  `min_neighbours` (default 6) of its 3³ neighbourhood are. An isolated noise spike has 1, a wall
  one voxel thin has 9, so the criterion removes speckle without smoothing away thin sheets.
  Material then grows `grow_steps` times into touching voxels above a lower threshold
  (hysteresis), which closes gaps that noise tears into walls.
- **Classes.** A material voxel gets the class of the mean grey value of the material voxels in
  its 3³ neighbourhood. Classes are split by thresholds, by default multi-level Otsu over the grey
  values above the air threshold.
- **Streaming.** One level-0 brick at a time, read with a halo of `grow_steps + 1` voxels through
  `Dataset`, so memory does not depend on the volume size (ADR 0004).
- **Output: material volume.** A directory with `materials.json` (thresholds, classes with name,
  colour, voxel count and volume) and `level0/<x>_<y>_<z>.vdb`, one `Int32Grid` "material" per
  brick that holds material, with the dataset's brick layout. `MaterialVolume` reads it with a
  small brick cache. Class ids fit a byte; the slice overlay carries material m as 16 + m.
- **Scoring.** `scoreMaterials` compares a material volume with a label volume of components. A
  component's true material is the class of its median grey value under the segmentation's
  thresholds; labels of parts numbered independently are kept apart by the part's position.
  Scores are Dice per material and for material versus air, plus the confusion matrix.
- **Parts.** `ConcatSource` joins sub-volumes along an axis (`vs-sieve --join`), so a scan
  reconstructed in parts is sieved, segmented and scored as one volume.

Tools: `vs-segment` (with `--truth` for scoring) and the studio operation `segment_materials`;
the slice view shows the classes in colour. The classes can be defined in the histogram of the
studio's 3D view: boundaries between them (the first is the air threshold, multi-level Otsu
proposes the others), a name and a colour each, with a 3D preview of the grey value classes
before segmenting. They reach the operation as `air_threshold`, `material_thresholds`,
`material_names` and `material_colors`, and the names and colours are kept in `materials.json`.
The colours of a segmentation can still be changed for the views; that is view state, not a step.

## Consequences

- On the Me 163 scan (512 × 3584 × 512, 0.33 × 0.33 × 0.6 mm, sieved with threshold 2500 and
  `--min-material 8`, segmented with air threshold 3000 and one growing step), two classes
  reach Dice 0.878 for material versus air, 0.854 for the light and 0.943 for the dense material
  (344 components). Most remaining errors are grey values
  between air and sheet near the sheet surfaces, which a threshold cannot decide; this is the
  baseline for a learned segmentation.
- Ground-truth materials come from grey values, so the per-material scores measure how well the
  voxel classes follow the component classes, not an independent material identification.
- A learned model can write the same material volume format and be scored the same way; ADR
  0014 does this with models that VoxelSieve runs itself.
