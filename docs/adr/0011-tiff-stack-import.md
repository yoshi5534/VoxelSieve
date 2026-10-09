# 0011: TIFF stacks and ZIP archives as input

Status: accepted (2026-09-28); the readers are libtiff and libzip since ADR 0016

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

**Amendment (staging).** The cache works for region reads, but not for the streaming sieve: pass
2 builds bricks of 256³ voxels in parallel, each brick needs its 256 slices, and the threads work
on different brick layers at once. As soon as those slices exceed the cache (at 1250² float
slices, 16 threads need about 13 GB), every slice is inflated and decoded again for each brick
that touches it: a 22 GB stack of 4000 slices took almost an hour on 16 cores. The source now
reports `slowRandomAccess()`, and `writeDataset` first copies such a source slice by slice, in
parallel, to a temporary raw file of 2 bytes per voxel (in the output directory, or
`DatasetOptions::staging_dir`), which both passes read memory-mapped like a raw scan; the file is
removed at the end. Every slice is decoded once, whatever the slice size, at the price of
temporary disk space the size of a uint16 raw volume; `writeDataset` checks that it is free first.
The copy is off with `stage_slow_sources = false` (`vs-sieve --no-staging`). Raw inputs are not
copied, since they are memory-mapped already.

**Amendment (memory and writes on Windows).** On Windows the 22 GB stack above peaked at 35 GB of
working set: the memory-mapped archive and the staging copy stayed resident, since `madvise` does
not exist there. `detail::releaseMappedPages` is the portable hint (`VirtualUnlock` on Windows,
`madvise(MADV_DONTNEED)` elsewhere): the archive lets its pages go every 64 entries it inflates,
staging lets each written slice go, and the passes let the staged copy go now and then (pass 1
every 1024 rows of blocks, pass 2 every 16 bricks). The pages stay in the file cache, so reading
them again costs a soft fault. The peak fell to 4.8 GB. Bricks are now serialised into memory and
written in one piece: OpenVDB's `io::File` writes leaf by leaf through a `std::ofstream`, and on
Windows those small writes left about half the cores waiting. Pass 2 fell from 118 s to 78 s, the
whole import from 175 s to 133 s. Blosc compression is now most of pass 2 and is kept, since
uncompressed bricks are 70 % larger and slower to write.

The TIFF and ZIP readers were first written in the library on top of zlib and CRC-32 from Boost.
Since ADR 0016 they are libtiff and libzip: `src/detail/tiff.cpp` feeds libtiff from a file or an
inflated archive entry through `TIFFClientOpen`, and `src/detail/zip.cpp` keeps one libzip handle
per concurrent reader. libzip reads the archive from a memory mapping
(`zip_source_buffer_create`), not through stdio, whose 4 KiB reads with a seek before each
made reading a cold archive slow.

The studio gets the operation `import_tiff` (parameters like `import_raw` plus `folder`), and the
file browser marks TIFF files, ZIP archives and directories with TIFF slices as kind `tiff`.

## Consequences

- Datasets published as TIFF stacks can be sieved without unpacking or converting them.
- Unusual TIFF variants (JPEG, CCITT, planar colour, 12-bit packing) are refused with a clear
  message; they can be added to `decodeTiffChunk` when real data needs them.
- Proprietary raw headers remain the main input for industrial scans; vendor formats are only
  read from publicly documented layouts.
