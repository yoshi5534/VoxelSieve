# 0019: DICOM stacks as input

Status: accepted (2026-10-09)

## Context

DICOM is what medical and many industrial scanners export on their own, often as a stack of
slices, signed and written without DICOM preamble. Such stacks should import without first
converting them to raw or TIFF.

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

The studio gets the operation `import_dicom` (`path`, `series`), with the sieve parameters of
the other imports; the file browser marks DICOM files and directories with DICOM slices as kind
`dicom`. `vs-sieve` reads a DICOM directory.

| Purpose | Library |
| --- | --- |
| DICOM | DCMTK (dcmdata, with its RLE decoder) |
| XML | pugixml |
| gzip | zlib |

## Consequences

- DICOM stacks import directly, at the place their files give, so CAD models and other scans
  aligned in the same coordinates can follow.
- Three dependencies more: DCMTK without optional features (no OpenSSL, no image codecs), pugixml
  and zlib (already built for Boost and libzip, now used directly).
- Signed DICOM scans are stored with grey = sample + 32768; thresholds and histograms of these
  datasets are in grey values, like those of float TIFF stacks.
