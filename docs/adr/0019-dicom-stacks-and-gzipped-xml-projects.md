# 0019: DICOM stacks and gzipped XML projects as input

Status: accepted (2026-10-09)

## Context

Scans that were evaluated in another CT application reach VoxelSieve as a gzipped XML project (`.gzxml`) with
the files it was made from. A project does not hold the voxels: it refers to the input files by
absolute path on the machine that saved it, and stores the import settings, the grid, the pose of
the volume in the scene and everything evaluated on it (measurements, reports, masks). The
first projects users brought refer to stacks of DICOM slices, signed and written without DICOM
preamble, and to a mask in the application's own run-length format (`.mask`). DICOM is also what medical
and many industrial scanners export on their own.

## Decision

**DICOM stacks** are read by `DicomStackSource` (`include/voxelsieve/dicom.hpp`), with DCMTK
(module dcmdata) as the parser (ADR 0016):

- a directory of slice files, a single slice, or a list of files; in a directory, files with a
  DICOM extension (`.dcm`, `.dicom`, `.dic`, `.ima`), with the `DICM` marker or without extension
  are tried, and those DCMTK cannot read as an image are skipped and counted;
- files with or without preamble and meta header, uncompressed in any byte order, or RLE
  compressed; other compressions (JPEG, JPEG 2000) are refused with the transfer syntax named;
- several series in one directory: the one with the most slices is read, the others are
  reported and can be chosen by Series Instance UID;
- slices are sorted along the slice normal by Image Position (Patient); without positions by
  Instance Number, else by name. Uneven spacing (a gap, a duplicate slice) is refused, as are
  slices of another size, sample type, orientation or rescaling;
- the voxel size is Pixel Spacing and the distance of the slice positions (else Spacing Between
  Slices or Slice Thickness); a thinner Slice Thickness is recorded (ADR 0012);
- one grey-value sample of 8 or 16 bits with any number of bits stored. Grey values stay exact
  (ADR 0002): unsigned samples are the grey values, signed ones are shifted by 32768, and with
  the rescale slope and intercept this is the value mapping of ADR 0015: value = intercept +
  slope * (grey − 32768) for signed samples. The datasets of signed scans therefore carry a
  `value_mapping`, and their grey values are 32768 above the stored samples;
- `filePose()` gives the pose from Image Position and Image Orientation (Patient). The import
  places the object there (ADR 0018), as the motion of the import step;
- multi-frame files (one file per volume) are refused for now.

Slices are parsed whole, so the source reports `slowRandomAccess()` and `writeDataset` stages it
like a TIFF stack (ADR 0011). Decoded slices are kept in an LRU cache (default 256 MiB).

**Gzipped XML projects** are read by `readGzippedXmlProject` (`include/voxelsieve/gzipped_xml.hpp`): the project is
gzip-compressed XML (with a few bytes after the gzip stream, which are ignored), inflated with
zlib and parsed with pugixml. For every volume (`VolumeRenderObject`) it gives the name, the
class of the import settings, the files of its `FileIOList`, the grid size, the sampling distance,
the sample type and the pose: the grid's `Transform`, whose matrix the application stores column by
column, followed by half a voxel, since the application's grid has voxel corners at multiples of the
pitch and VoxelSieve's grid voxel centres. Each referenced file is looked for as written, then at
the same place relative to the project's folder (the project records where it was saved), then
by its name next to the project, so that a project folder copied to another machine works.
Further files the project names, such as masks (`.mask`), are listed as not imported.

Volumes imported with `DicomImportSettings` are read through `DicomStackSource`, with the
project's voxel size and pose. Import settings VoxelSieve does not apply (axis swap or mirror,
resampling on import) are reported. Other import classes are listed with their class name, and
importing them fails with that name; raw volumes, TIFF stacks and the application's own volume files
can be added when a project that uses them is available to test against.

Not read, and not planned without a published specification: `.mask` masks and regions of
interest, the evaluation results stored in the project (measurements, wall thickness, defect
detection), and the images the application keeps in a folder next to the project (thumbnails, report
logos). VoxelSieve
does not write `.gzxml` files.

The studio gets the operations `import_dicom` (`path`, `series`) and `import_gzipped_xml` (`path`,
`volume`), with the sieve parameters of the other imports; the file browser marks DICOM files
and directories with DICOM slices as kind `dicom` and projects as `gzipped_xml`. `vs-sieve` reads a DICOM
directory or the first volume of a project.

| Purpose | Library |
| --- | --- |
| DICOM | DCMTK (dcmdata, with its RLE decoder) |
| XML (gzipped XML projects) | pugixml |
| gzip (gzipped XML projects) | zlib |

## Consequences

- A gzipped XML project and its DICOM slices import without exporting the volume from the application
  first, at the place the application showed it, so CAD models and other scans aligned there can follow
  with the same coordinates.
- Three dependencies more: DCMTK without optional features (no OpenSSL, no image codecs), pugixml
  and zlib (already built for Boost and libzip, now used directly).
- Signed DICOM scans are stored with grey = sample + 32768; thresholds and histograms of these
  datasets are in grey values, like those of float TIFF stacks.
