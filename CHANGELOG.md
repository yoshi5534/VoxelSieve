# Changelog

## Unreleased

- **English throughout**: the studio (labels, operation names, hints, 3D view presets), the
  built-in report template, the report's field names and verdicts, and the example inspection
  order are now in English; numbers in the report use a decimal point. Projects saved with German
  step titles show the current English titles. A German report is still possible with an own
  template (`--template`), except for the few phrases the report data itself carries.

- **Threshold for scans of several materials**: the estimated air threshold is now the valley
  between the air peak and the next material, not Otsu's split between the two largest classes
  (ADR 0004 amendment). In the LoDoInd pipe Otsu's split lay between mortar and stones, so the
  pipe wall at the weakly lit scan ends counted as air, outside air flooded the inside of the pipe
  and the organic fillings were cut out in 8³ blocks. With air and a single material the
  threshold stays Otsu's.

- **Slice view without seams**: tiles are drawn at whole device pixels, so no faint line shows
  between neighbouring tiles, and nothing is drawn past the edge of the volume.

- **Staging lets go of the input** (`VolumeSource::releaseMemory`): once a TIFF stack is copied,
  its decoded-strip cache is emptied and the pages of the memory-mapped ZIP are dropped (and
  marked sequential while reading), so they no longer push the copy that pass 2 reads out of
  memory. With the 22 GB LoDoInd archive they had stayed resident (27.6 GB peak) and pass 2 took
  101 s instead of 22 s.

- **ZIP archives are read memory-mapped**: libzip now reads the archive from a memory mapping
  instead of through stdio, where it read 4 KiB at a time with a seek before each read (about
  11 million system calls for a 22 GB archive). The operating system now reads the file in large
  blocks; staging a cold 2.9 GB float stack took 11 s instead of 15 s on 4 cores.

- **A full disk is reported as such**: OpenVDB does not report failed writes, so a dataset written
  to a full disk failed later with "not a VDB file". Writing a grid now stops with "No space left
  on the device while writing ...".

- **Faster TIFF import** (`VolumeSource::slowRandomAccess`, `DatasetOptions::stage_slow_sources`
  and `staging_dir`, `vs-sieve --staging-dir` and `--no-staging`, ADR 0011 amendment): a TIFF
  stack is first decoded once, slice by slice on all cores, into a temporary raw file (2 bytes per
  voxel, removed at the end) that both passes then read. Before, pass 2 inflated and decoded
  every slice again for each brick it touched as soon as a brick layer did not fit into the 1 GiB
  slice cache. A float stack of 512 slices of 1250² in a ZIP now takes 41 s instead of 302 s on 4
  cores, with an identical dataset. The coarser levels are built leaf by leaf, a third faster,
  and the float value range is estimated from its 33 slices in parallel.

- **Slice view scrolls by whole slices**: a slice once started loads completely before the next
  one starts, and the view shows the nearest complete slice until the current one is there, so
  scrolling no longer leaves single tiles of other slices behind.

- **Finer 3D view close up** (`VolumeRequest`, `readVolumePreview` with a region, `/api/volume`
  with `x0..z1`, `low`, `high`): when the camera comes close, the 3D view loads the part nearest
  it at the level a pixel there needs, up to 256 voxels per axis, and blends it into the coarse
  volume. The camera can now come much closer and be moved (right or shift drag).

- **Outside air only from some sides** (`outside_air_axes`, `vs-sieve --air-from xy`, parameter
  of `import_raw` and `import_tiff`, ADR 0003 amendment): for parts that the first and last slice
  cut through, such as pipes, the flood fill no longer enters through the end slices and removes
  low-density fillings block by block.

- **Progress of the streaming sieve** (`DatasetOptions::progress`): `vs-sieve` shows per pass the
  percentage done, the time so far and the time left (one line rewritten in place on a terminal,
  a line every 10 % in logs); `import_raw` and `import_tiff` report it to the studio.

- **Float TIFF stacks** (`TiffStackOptions::value_range`, `vs-sieve --value-range`, parameter
  `value_range` of `import_tiff`, ADR 0015): 32 and 64-bit float slices, as reconstruction
  software writes them (attenuation values, negative ones included), also with the
  floating-point predictor. They are mapped linearly onto 16-bit grey values over a given or
  estimated value range; the mapping is recorded in `index.json` (`value_mapping`) and in the
  grid metadata of a single `.vdb`, and clipped values are counted.

- **vcpkg** (ADR 0016): dependencies are declared in `vcpkg.json` and pinned by its baseline; the
  CMake presets use vcpkg's toolchain, `tools/setup-vcpkg.sh` fetches it, and CI keeps built
  packages in vcpkg's binary cache. File formats and compression come from established
  libraries: TIFF is read with libtiff, ZIP archives with libzip, PNG is written with libpng and
  PLY with tinyply; the own readers, decoders and checksums are gone. libtiff also reads float
  slices with the floating-point predictor and every compression it is built with.

- **Learned segmentation** (`voxelsieve::Model`, `segmentMaterialsWithModel`, `vs-segment
  --model`, studio operation `segment_model`, ADR 0014): small 3D networks (U-Nets) stored as
  `.vsm` run on the CPU tile by tile with a halo and write the same material volume as the
  threshold segmentation. Training and export with PyTorch in `tools/models/`. `vs-segment
  --region` scores only a box. On Me 163, scored on volumes left out of training: Dice
  material/air 0.861 → 0.898 (V2) and 0.843 → 0.922 (V6).
- **Material segmentation** (`voxelsieve::segmentMaterials`, `vs-segment`, studio operation
  `segment_materials`, ADR 0013): splits the part into material classes by grey value with a
  neighbourhood criterion that removes noise spikes but keeps walls one voxel thin, hysteresis and
  multi-level Otsu thresholds. Writes a material volume with volumes per class; the slice view
  shows the classes in colour. `vs-segment --truth` scores the result against labelled components
  (Dice per material). On Me 163: Dice 0.878 material/air, 0.854 light, 0.943 dense.
- **Scans in parts** (`voxelsieve::ConcatSource`, `vs-sieve <part>... --join <axis>`): several
  volumes joined along an axis are read as one.
- **Noise-robust sieve for datasets** (`DatasetOptions::min_material_voxels`, `vs-sieve
  --min-material`, parameter `min_material_voxels` of `import_raw` and `import_tiff`): an 8^3
  block counts as material only with at least n voxels above the threshold, so speckle noise in
  the air no longer keeps whole blocks. Voxel-identical to the in-memory sieve; recorded in
  `index.json`. On the Me 163 scan (512 x 3584 x 512, threshold 2500), n = 8 keeps 14 % of the
  voxels instead of 70 % and still covers 99.4 % of the blocks with structure.
- **Voxels that are not cubes** (`voxelsieve::VoxelSize`, ADR 0012): an edge length per axis, for
  scans with a coarser slice spacing, and the slice thickness when slices are thinner than their
  spacing. Volumes, positions, pore sizes, surfaces, the nominal-actual comparison, report images
  and the slice and 3D views use the pitch per axis; VDB grids carry a scale per axis. Existing
  files stay readable. `--voxel-size x,y,z`, `--slice-thickness`, and the same as parameters.
- **TIFF stacks** (`voxelsieve::TiffStackSource`, `vs-sieve`, studio operation `import_tiff`): a
  directory of slices, a multi-page TIFF or a ZIP archive of either, read without extracting it.
  Classic TIFF and BigTIFF, strips and tiles, uncompressed, LZW, Deflate and PackBits, horizontal
  predictor; unsigned 8/16-bit samples and 32-bit labels up to 65535. Label folders next to the
  grey values are recognised and can be chosen with `--folder` (ADR 0011).

## 0.1.0 (2026-09-28)

First version: from a raw CT volume to an inspection report.

- **Sieve** (`vs-sieve`): removes only the outside air and keeps internal voids (ADR 0003). Writes
  a bricked multi-resolution VDB dataset with an overview grid, streamed in two passes over a
  memory-mapped raw file, so scans larger than RAM work (ADR 0004). Reads raw files with vendor
  headers, 8-bit samples and big-endian data. Grey values stay lossless as float (ADR 0002).
- **Projects and operations** (`voxelsieve::Project`, `voxelsieve::OperationRegistry`): every
  step is recorded with parameters, inputs, outputs and messages, can be undone and redone, and
  the project is saved after each change. Import, porosity and report are operations; more can be
  loaded as plugins (ADR 0008).
- **Studio** (`vs-studio`): browser UI with a wizard (dataset, operations, report), the step
  protocol and undo/redo, served locally; operations run while the UI stays responsive.
- **Slice view**: tiled slices along x, y and z from the matching resolution level, with pores and
  zones tinted, in the studio and as `view_slice` for MCP. `Dataset` can load bricks on access
  (`BrickLoading::kOnAccess`) for sparse reads.
- **3D view**: WebGL2 ray casting of a coarse level as a surface, with a transfer function or
  as a maximum intensity projection. Histogram editor for the transfer function (control points
  with opacity and colour, colour maps, presets) and the surface threshold; colours for pores,
  zones, surface and background (studio, gradient, plain); gradient lighting; a cut along x.
  Suggested renderings from the histogram when a volume is loaded.
- **Views**: the project keeps how it was shown and opens the same way; named views with a
  picture are saved in the project, shown again with a click and exported as PNG; `view_*`
  methods for MCP.
- **MCP** (`vs-studio --mcp`): the studio API as Model Context Protocol tools for AI systems
  (`voxelsieve::Studio`, `voxelsieve::McpServer`).
- **Read API** (`voxelsieve::Dataset`): samples, regions across bricks and parallel brick
  iteration through an LRU brick cache with a memory budget.
- **Synthetic scans** (`vs-synth`): STL meshes to raw volumes with shrinkage cavities, loosened
  microstructure, noise, cupping, ring artefacts and unsharpness, and a JSON ground truth
  (ADR 0005). Sample castings with a known surface: housing, bracket and hub (`--part`).
- **Surface** (`vs-surface`, operation `surface`): the surface as a voxel mask with a few bits per
  voxel that encode the distance to it; only 8³ blocks at the surface are stored, compressed with
  zstd. Several hundred to over a thousand times smaller than the raw scan, surface accurate to a
  few hundredths of a voxel; export as mesh or VDB level set (ADR 0009).
- **Surface view**: the 3D view shows only the extracted surface as a mesh at full resolution,
  coarsened to a triangle budget (`surfaceDisplayMesh`, `/api/surface`).
- **Nominal-actual comparison** (`vs-compare`, operation `compare_cad`): aligns a CAD model (STL)
  to the scanned surface (principal axes, then a robust best fit) and measures the signed
  deviation per surface point. Writes statistics, a coloured PLY and views; the 3D view shows the
  deviation in colour (ADR 0010).
- **Report images**: the inspection report shows the part and its pores in 3D (part as glass,
  pores and loosened zones true to scale) when given a surface, and the nominal-actual comparison
  with coloured views and a deviation histogram when given a comparison. Operations can have
  optional inputs for this. The shaded views come from a CPU renderer (`voxelsieve::render`).
- **Porosity analysis** (`vs-porosity`): pores with partial-volume void volumes and zones of
  loosened microstructure against a depth-dependent material level, so cupping is not reported
  as porosity (ADR 0006). JSON, projection images and a VDB with pores and zones.
- **Inspection report** (`vs-report`): evaluation per inspection zone following the BDG P 202
  scheme, report structured after DIN EN ISO/IEC 17025 (7.8) and DIN EN ISO 15708-3, from an
  exchangeable HTML template (ADR 0007).

Known limitations are listed in the consequences of ADR 0006 and ADR 0007. Builds and CI cover
Ubuntu 24.04 only.
