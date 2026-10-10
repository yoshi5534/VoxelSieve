# 0022: Non-rigid registration for flexible parts

Status: accepted (2026-10-10)

## Context

The nominal-actual comparison (ADR 0010) aligns the CAD model rigidly and measures the deviation
of every surface point. For parts that do not keep their nominal shape in the scanner this
measures the wrong thing: a thin sheet metal part, a plastic clip or a rubber seal lies bent under
its own weight or in its fixture, and the bend swamps every real deviation. Metrology handles
this with a restrained or "virtually clamped" measurement: the part is brought back into its
nominal shape, and only what remains counts as deviation.

Bringing it back must not take away what the comparison is for. A dent, a bump, a wall that is
too thin, a part that shrank in the mould: all of these can be fitted away by a deformation that
is free enough. The registration therefore has to bend what is easy to bend and nothing else, and
it has to say how far it moved the part, so that nobody mistakes a bent part for a good one.

The constraints stay those of ADR 0010: it measures on the surface mesh of the scan, runs on
surfaces of a million points, and needs no new dependency.

## Decision

The non-rigid registration is an optional step of the comparison after the rigid alignment
(`CompareOptions::deformation`, `--non-rigid`, operation parameter `non_rigid`). The rigid
alignment stays exactly as it is; the deformation only adds to it.

### Deformation

- **Field:** a displacement field in CAD coordinates, cubic B-splines on a regular grid of control
  points over the bounding box of the scanned surface (free-form deformation). It is smooth
  (C²) and changes over no less than the control point spacing. The spacing defaults to an
  eighth of the largest extent of the part, at most 40 cells along the longest axis and no less
  than two voxels. Features smaller than the spacing cannot be bent away.
- **Fit:** the scanned surface points, moved by the field, should lie on the CAD surface:
  point-to-plane distances to the closest CAD point, re-linearised in each iteration as in the
  rigid ICP, with the same Huber weights (twice the robust sigma, at least half a voxel) and
  rejection beyond five sigma. Once the bend is taken up, the sigma is that of the noise, so local
  defects lie beyond the rejection and pull no more. It stops when no point moves by more than a
  hundredth of half a voxel.
- **Regularisation: elastic energy.** The field pays for the strain it causes, the symmetric part
  of its gradient, integrated over the material of the part: points on a grid inside the CAD
  model (about 30 000 over the bounding box), plus the surface points for a skin half a grid step
  thick, which covers walls thinner than the grid. It is normalised by the volume and weighted by
  `stiffness × D²` (D the diagonal of the part), so that stiffness 1 makes a uniform strain e cost
  as much as a distance of D·e. A small ridge pins control points without material to zero.

The elastic energy decides what is easy to bend, the way a real part does. Rigid motions cost
nothing. Bending a wall of thickness t into a curvature κ strains it by about κ·t/2, so a thin
wall bends nearly freely and a thick section resists. Stretching or compressing, such as a uniform
shrinkage, strains the whole part and is expensive, so the size of the part stays a deviation.
A bending energy (second derivatives of the field) was rejected as the regulariser, because it
lets affine fields through at no cost and would fit away a scale error. Strain measured only at
the surface was rejected, because the field can then hide a gradient below the surface and
shrink a thick part almost unnoticed (a third of a 1 % oversize in the test below).

Each iteration solves the linear least-squares problem for all control point displacements at
once with conjugate gradients (Jacobi preconditioner, matrix-free, warm-started from the last
iteration). The strain is linear in the field, so large rotations of parts of the part (more than
about 10°) count as strain; such parts need a better rigid start, or a later geometrically
non-linear version.

### Results

The deviation is measured on the bent surface: the distance of every vertex, moved by the field,
to the CAD surface. Besides it:

- `compare.json` gets `deformation`: spacing, stiffness, control points, the fit, the statistics
  of the displacement (area-weighted, the tolerance bands give the area moved by more than the
  tolerance), and `rigid_deviation`, the statistics of the rigid alignment alone.
- `deviation.ply` gets a float `displacement` per vertex; `displacement_view_[12].png` show it,
  green within the tolerance, yellow to red above.
- The studio summary has the largest and mean displacement and the RMS and in-tolerance share of
  the rigid alignment alone. The 3D view colours the compared surface by its deviation or, on
  request, by its displacement (`GET /api/deviation?displacement=1`). The report states that the
  surface was bent, how far, with which spacing and stiffness, gives the rigid figures beside it
  and shows the displacement views.

## Measurements

Synthetic scans at 0.2 mm voxels, noise, cupping and unsharpness of half a voxel
(`tests/test_compare.cpp`), stiffness 1:

| part | rigid alone | non-rigid | what stays |
| --- | --- | --- | --- |
| plate 60 × 20 × 2.4 mm, bent by 1 mm, bump 0.4 mm | RMS 0.27 mm, 28 % within ±0.1 mm | RMS 0.04 mm, 96 % within, moved up to 0.56 mm | bump 0.37 mm (blur rounds its top) |
| block 20 × 14 × 10 mm, 1 % oversize | mean +0.072 mm | mean +0.070 mm, moved up to 0.004 mm | 98 % of the oversize |
| sample hub (0.2 mm voxels), nominal shape | RMS 0.030 mm | moved up to 0.0004 mm | everything |

On the full-size housing (972 592 surface points), the registration takes 1.3 s and the second
deviation pass 0.9 s on 4 cores, next to 1.8 s for the rigid alignment.

## Consequences

- A comparison with a non-rigid registration is a different measurement from a rigid one. The
  outputs and the report say so wherever the deviation appears, with the rigid figures beside it.
- Spacing and stiffness are the two knobs: a larger spacing or a higher stiffness keeps more as
  deviation. The defaults suit thin flexible parts against solid ones; a part with long thin arms
  on a solid body may need its own values.
- Clamping at given points (fixture positions, RPS points held at nominal) and a geometrically
  non-linear strain for large bends can build on the same field and solver.
