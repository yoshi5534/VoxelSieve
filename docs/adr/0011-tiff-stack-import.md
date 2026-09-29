# 0011: TIFF stacks and ZIP archives as input

Status: accepted (2026-09-28)

## Context

Besides raw files with vendor headers (ADR 0004), CT volumes are exchanged as stacks of TIFF
slices: exported from reconstruction software, published as training and benchmark data, often
as a ZIP archive with one folder of grey values and one of labels. The first example dataset a
user brought is such an archive (512 uncompressed 16-bit slices in `data/`, 32-bit labels in
`target/`, 805 MB unpacked).

The input must be read the way raw files are: through `VolumeSource`, out of core, without a
converted copy on disk, and without changing a grey value (ADR 0002).

## Decision

`TiffStackSource` (`include/voxelsieve/tiff.hpp`) reads

- a directory of slices, a single multi-page TIFF, or a ZIP archive holding either;
- classic TIFF and BigTIFF, little and big endian, strips and tiles;
- no compression, LZW, Deflate and PackBits, with or without horizontal predictor (2);
- one sample per pixel, unsigned 8 or 16 bit, and 32 bit when every value fits into 16 bit, so
  that label volumes work. Signed and colour images are refused rather than quantised; float
  images are mapped onto 16 bit explicitly and with a recorded mapping since ADR 0015.

Slices are sorted by name with digit runs compared by value. When several folders hold TIFF files,
the one chosen is the one not named like labels or masks (`label*`, `mask*`, `seg*`, `gt`,
`target(s)`, `annotation(s)`), then the one with the most slices; the others are reported and can
be chosen explicitly. The voxel size is taken from a centimetre resolution or from the unit of an
ImageJ description; inch resolutions are print settings and are ignored. Without either, 1 mm is
assumed and reported, and the caller can set it.

Pages are parsed on first access and strips or tiles are decoded on demand into an LRU cache
(default 1 GiB), so a region read touches only the slices and chunks it needs. A ZIP entry is
inflated as a whole when one of its chunks is needed, since deflate has no random access; slices
are small compared with the cache, and stored entries could be read in place later if needed.

The TIFF and ZIP readers are written in the library (`src/detail/tiff.cpp`, `src/detail/zip.cpp`,
about 1000 lines) on top of zlib and CRC-32 from Boost, which VoxelSieve already uses. No new
dependency: libtiff alone would not read from ZIP archives, and libzip plus libtiff for a
read-only subset of both formats is more to install on every platform than the code it saves.

The studio gets the operation `import_tiff` (parameters like `import_raw` plus `folder`), and the
file browser marks TIFF files, ZIP archives and directories with TIFF slices as kind `tiff`.

## Consequences

- Datasets published as TIFF stacks can be sieved without unpacking or converting them.
- Unusual TIFF variants (JPEG, CCITT, planar colour, 12-bit packing) are refused with a clear
  message; they can be added to `decodeTiffChunk` when real data needs them.
- Proprietary raw headers remain the main input for industrial scans; vendor formats are only
  read from publicly documented layouts.
