# 0040: More scan formats: wide raw samples, MetaImage, NRRD and PNG stacks

Status: accepted (2026-11-23)

## Context

VoxelSieve read raw files of unsigned 8 or 16 bit, TIFF stacks (ADR 0011, float slices with a
value mapping, ADR 0015), DICOM stacks (ADR 0019) and the DICOM volumes of gzipped XML projects.
Scans reach users in more forms than these:

- Many reconstructions write **32-bit float raw files**, some signed 16-bit or 32-bit integers.
  A raw file named `..._float32.raw` was refused with the advice to export TIFF.
- Research software, ITK-based tools and medical viewers exchange volumes as **MetaImage**
  (`.mhd` with a data file, `.mha` with the data attached) and **NRRD** (`.nhdr` detached,
  `.nrrd` attached). Both are a short text header (dimensions, sample type, byte order, spacing,
  position, data file, compression) in front of raw or gzip/zlib data.
- Reconstructions of desktop micro-CT and many published datasets are **PNG slice stacks**.

The rule of ADR 0016 is that file formats come from established libraries. The only library in
vcpkg that reads MetaImage and NRRD is ITK, a very large dependency; the NRRD reference library
(teem) is not in vcpkg.

## Decision

**Raw files of any common sample type.** `RawLayout` gets `stored_type` (`RawSampleType`: int8,
int16, uint32, int32, float32, float64 besides uint8 and uint16), a `value_range` and a
`compressed_offset` for compressed data behind a header in the same file. Grey values stay
lossless where they can (ADR 0021):

- int8 and int16 are shifted by 128 and 32768 into the unsigned range; the shift is the value
  mapping (offset -128 or -32768, scale 1), as for signed DICOM.
- uint32 and int32 within 0..65535 are kept as they are.
- Wider integers and floats are mapped linearly onto 0..65535 over a value range, given or
  estimated from 33 slices spread over the volume (over the first quarter for compressed files
  of more than 4 GiB), widened by 10 % on each side, as for float TIFF (ADR 0015). Values
  outside are clipped and counted once per region read.

The sample type comes from the parameters, the sidecar's `format.dtype`, or the file name
(`float32`, `float`, `double`, `uint32`, `int32`, `32bit`). The grey values show rows and
slices only of 8- and 16-bit samples, so a float file needs its dimensions in the name or a
sidecar. A compressed file whose header asks for more voxels than deflate can pack into it
(about 1:1032) is refused before anything is allocated.

**MetaImage and NRRD headers are read by our own code** (`header_volume.hpp`), as an exception to
ADR 0016 that the maintainer agreed to: the header is plain text of `key = value` or
`field: value` lines, like our JSON sidecar, and the binary data still goes through the raw
sources and zlib. Read are:

- MetaImage: `NDims` 2 or 3, `DimSize`, `ElementType` (`MET_UCHAR` to `MET_DOUBLE`, one
  channel), `ElementSpacing` or `ElementSize`, `BinaryDataByteOrderMSB`/`ElementByteOrderMSB`,
  `HeaderSize` (also -1), `CompressedData`, `Offset`/`Origin`/`Position`,
  `TransformMatrix`/`Rotation`/`Orientation`, `ElementDataFile` as `LOCAL`, a file, `LIST` or a
  numbered pattern (`slice%03d.raw 1 400 1`).
- NRRD: `type` (all spellings of 8-, 16- and 32-bit integers, float, double), `dimension` 2 or 3
  (or 4 with a first axis of one sample), `sizes`, `endian`, `encoding` raw or gzip, `space
  directions` or `spacings`, `space origin`, `line skip`, `byte skip` (also -1), `data file` as a
  file, `LIST` or a pattern; comments and `key:=value` pairs are skipped.

Several data files are read as one volume (`ConcatSource`) with one value range from up to 33 of
them. The header's position and axis directions become the pose of the object (ADR 0018);
mirrored or skewed axes keep the position only, with a note. Text, hex and bzip2 encodings,
64-bit integers and several channels per voxel are refused with a message that says so. The
operation is `import_header_volume`; damaged copies of headers are part of the tests.

**PNG stacks** are read with libpng (already a dependency, ADR 0016) and libzip:
`PngStackSource`, a directory or ZIP archive of slices, sorted and chosen like TIFF stacks
(ADR 0011, folders that look like labels last). 8- and 16-bit grey values are kept; colour slices
become grey. PNG files carry no voxel size that CT software writes reliably, so it comes from the
parameters or the folder name (`scan_50um`), else 1 mm is assumed with a warning. Every slice is
decoded whole, so the import stages the stack first. The operation is `import_png`.

`inspect_scan`, `open_scan` and `browse` know the new kinds (`metaimage`, `nrrd`, `png`); a raw
data file whose `.mhd` or `.nhdr` header lies next to it opens through the header. `vs-sieve`
reads all of them.

## Consequences

- Scans in these forms open with one click, with their spacing and position where the header has
  them. The value mapping of wide samples is recorded in the dataset as for float TIFF.
- The header readers are our own code: they get damaged-file tests and refuse what they do not
  understand rather than guessing.
- HDF5/NeXus (synchrotron data) comes next, with the `hdf5` library from vcpkg; OME-Zarr, NIfTI
  and MRC are left until someone asks for them.
