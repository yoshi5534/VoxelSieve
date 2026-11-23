# 0039: Opening a scan without questions, and suggested analyses

Status: accepted (2026-11-23)

## Context

A first user had to know too much before seeing anything of their scan. The UI asked for a
project directory and name before any data, then showed the full import form (16 parameters,
among them brick size, staging and value type). A raw file without a JSON sidecar, the common
case for a scanner's export, failed with "No dims given and no sidecar", and the failed step
stayed in the protocol. After the import the UI showed a table of the dataset; the 3D view was
two clicks away, and the analysis stage listed every operation with no hint which suit the scan.

## Decision

**A scan opens in one step, and the analyses that suit it are suggested.** All of it is API
methods first (CLAUDE.md), so the UI and AI clients get it alike:

- `inspect_scan` says what a file or directory is (raw, TIFF, DICOM, dataset, gzipped XML, STL,
  project), the operation and parameters that open it, what was found and where each value comes
  from (sidecar, files, file name, grey values, assumed), and `check` when a value was guessed.
- `open_scan` creates a project next to the scan (`<name>.vsproj`) when none is open, or opens
  that project again when it imported this scan before, and runs the import with the detected
  parameters. The detected values are parameters of the step, so the protocol repeats it.
- `suggest_analyses` measures on a coarse level only (at most 2^21 voxels for the shape, 2^24 for
  the histogram), so it answers in well under a second for scans of any size: the materials
  (classes of the histogram's Gaussian mixture with at least 5 % of the voxels that are narrow
  for their grey value, so partial volume at the edges is no material), the cupping
  (`measureCupping`), whether the part is round across an axis, CAD models next to the scan and
  in the project, and words in the name ("18650", "cfrp"). Each suggestion carries its reason and
  the parameters to run it with, and is marked done once it ran.

**Raw files without a sidecar** (`detectRawLayout`, `source.hpp`): the name gives what it can
("512x512x400", "16bit", "uint8", "bigendian", "0.05mm", "50um"; also the directory's name). The
rest comes from the grey values, read at three places inside the file (a few MB):

1. Sample type and byte order from which bytes vary smoothly: the high byte of 16-bit values
   changes slowly, the low byte is noisy, and in 8-bit data neighbouring bytes are as alike as
   bytes two apart.
2. The row length is the offset (16 to 16384 voxels) at which voxels are most alike, the
   shortest such offset that divides the best one (rows and slices further apart are alike too).
3. The slice size likewise, among the heights whose whole slices fill the file up to a header
   of less than a slice and at most 1 MiB; the number of slices follows from the file size and
   what is left is a header at the start.

The confidence is how much more alike neighbours along y and z are than unrelated voxels; below
0.3 nothing is detected (noise, compressed data). Without a voxel size, 1 mm is assumed and the
import warns, as for TIFF stacks. The UI shows detected and assumed values for a check before
the import; values from sidecars and files open at once.

**The UI** opens on "Open a scan …"; after the import it shows the 3D view with the suggested
analyses above it, each started with one click, and shows the result where it suits (pores in a
translucent part, deviations and wall thickness on the surface). Every import setting stays
reachable ("Import with settings …").

## Consequences

- No new dependency. The detection reads a few MB, also of files on network shares.
- A guess can be wrong (a scan of noise, a volume whose slices repeat with another period).
  The UI therefore asks for a check whenever the size came from the grey values, and the import
  preview shows the slices while the import runs.
- 32-bit and float raw files are still not read; a name that says so gets an error that points to
  float TIFF stacks.
- `import_raw` no longer defaults `sample_type` and `big_endian` in its schema: they come from
  the parameters, the sidecar's `format`, the detection, else uint16 little endian.
