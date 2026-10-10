# 0020: A first look at the volume before the import is done

Status: accepted (2026-10-09)

## Context

Importing a large scan takes minutes: a 3072 x 3072 x 2000 volume of 16-bit voxels is 38 GB, and
on a Samba share behind 1 Gbit/s Ethernet (about 110 MB/s) reading it alone takes six minutes.
Until the dataset was written, the studio showed a progress bar and nothing else, so a wrong
volume, a wrong folder or a threshold that would keep the noise was found only at the end.

What a first look needs is a picture of the part and the histogram, from a small part of the data
that is cheap to read on every input and does not have to be read again by the import:

- Every input stores a slice together: a file of a TIFF or DICOM stack, an entry of a ZIP archive,
  a page of a multi-page TIFF (whose page offsets are read when the stack is opened), a contiguous
  run of bytes in a raw file. Reading a whole slice is one request; reading every n-th row of a
  slice is a request per row, which on SMB costs a round trip each (about 0.5 ms) and saves little,
  since the import needs those rows anyway. Compressed slices (ZIP, Deflate, LZW) must be read
  whole in any case.
- Slices that are read early are not wasted: inputs with slow random access (`slowRandomAccess`)
  are copied once, slice by slice, to a local staging file before the passes (ADR 0011), and the
  order of that copy is free.
- Raw files were memory-mapped and read directly by both passes, pass 2 brick by brick. On a
  network share every page that misses the cache is a small request of its own, and the file is
  read twice.

## Decision

- `readImportPreview` (`preview.hpp`) reads `PreviewOptions::slices` (64) whole slices, the middle
  slice of each of 64 equal parts along z, in parallel, so several requests are in flight on a
  share. Each slice is averaged over square blocks to at most `PreviewOptions::size` (256) voxels
  per edge. The histogram counts every voxel of those slices at full resolution, and the
  threshold is estimated from it as the import estimates it from the whole volume (ADR 0004). For
  2000 slices that is 3.2 % of the voxels.
- `DatasetOptions::preview` makes `writeDataset` hand over the same preview before the passes. A
  staged source copies the preview's slices first, builds the preview from the staging file
  while the slices are still in memory, calls back, and then copies the remaining slices; every
  slice is read exactly once, as before. Other sources (local raw files, phantoms) are read
  directly, which costs a few milliseconds.
- `MappedRawSource` reports slow random access when its file lies on a network file system (SMB,
  CIFS, NFS, AFS, Ceph by the file system type on Linux, a non-local mount on macOS, a UNC path or
  a remote drive on Windows; `detail::onNetworkShare`). Such a file is staged like a stack: read
  once, slice by slice, and the passes read the local copy.
- `renderPreview` draws the central sections normal to z, y and x in true proportions (ADR 0012)
  and the histogram on a logarithmic scale with the threshold in red; `previewSummary` gives the
  numbers and a binned histogram.
- Every import operation keeps the preview with its step (`preview.png`, `preview.json`), logs
  when it came ("Preview after 0.8 s: 64 of 512 slices ..."), and hands it to the studio through
  `OperationContext::preview`. The studio shows it in `project_status` while the step runs and
  gives it, with the picture, through the API method `import_preview`, so the browser UI and AI
  assistants get it alike; the UI shows it under the progress bar. `vs-sieve --preview <png>`
  writes the picture as soon as it is read. The new member is the last of `OperationContext`, so
  plugins built against plugin API version 3 keep working and the version stays.

**Amendment (gzip-compressed raw files).** `GzipRawSource` reads `<name>.raw.gz` (sidecar
`<name>.json`) with zlib, which is already a dependency (ADR 0019); concatenated gzip members
(pigz) are read as one stream. gzip can only be decompressed forward, so the source reports
`sequentialAccess()`: `writeDataset` always stages it (also with staging off), in one pass in
slice order on one thread, and takes the preview's slices as the copy passes them. The preview
therefore comes only at the end of that pass, not after 3 % of the reading; reading the 64 slices
first would mean decompressing everything up to the last of them anyway. `readImportPreview`
reads such a source in order. The header size is not known without decompressing everything, so
it is 0 unless given. `import_raw`, `vs-sieve` and the studio's file browser take `.raw.gz`.

**Measured (explicit reads of whole slices).** A share was emulated with sshfs over loopback
through a proxy adding 0.25 ms each way and capping the bandwidth (sequential `cat`: 86 MB/s at
the 1 Gbit/s cap). Reading a 512 MB raw file slice by slice on 4 threads, page cache dropped:

| link | memory map | `read` per slice, file kept open | `read` per slice, opened each time |
|------|-----------:|---------------------------------:|-----------------------------------:|
| 1 Gbit/s | 81–83 MB/s | 81–83 MB/s | 65–70 MB/s |
| 10 Gbit/s | 369–446 MB/s | 437–483 MB/s | 311–359 MB/s |

Reading slice by slice through the memory map lets the kernel read ahead and fills the link.
Explicit reads with the file kept open gain about 10 % at 10 Gbit/s and nothing at 1 Gbit/s, and
lose when the file is opened per slice. Not adopted: the memory map stays. What matters on a
share is reading in large pieces in order, which staging ensures. A 500 MB phantom imported from the emulated 1 Gbit/s share took 22.8 s
read directly and 23.2 s staged, since it fits the page cache and pass 2 reads it from there;
staging pays off for volumes larger than memory, where the passes read the share twice and pass 2
in bricks. The same phantom gzip-compressed (430 MB) took 25.1 s, its preview came after 7.8 s.

**Amendment (a preview that gets sharper, 2026-10-10).** The first look stays as it is, but
it no longer has to be all: a staged import copies every slice anyway, so it may as well copy the
slices that make the preview sharper first and show them as they arrive.

- The preview has one resolution from the start: blocks of `pixel_stride` voxels in-plane, as
  small as `PreviewOptions::size` (1024 per edge) and `PreviewOptions::voxels` (32 million in
  all) allow, and slices about as far apart as a block is wide, but at least the 64 of the first
  look. Each preview slice shows one input slice, the middle of its part of the volume
  (`previewSlices`).
- The first look reads the middles of 64 equal parts of those preview slices; every preview slice
  shows the slice read nearest to it. A staged import then copies the preview slices in rounds:
  the middles of three times as many parts each round (they include the earlier ones), until
  all. After each round `DatasetOptions::preview` is called again with the sharper preview. Then
  the copy takes the remaining slices, counts them in the histogram only, and calls a last time,
  with the histogram of the whole volume: its threshold is the one the import uses.
  `ImportPreview::slices_read` and `complete()` say how far it is.
- The picture gets about a pixel per preview voxel, 256 to 1024 pixels along the longest edge.
  The studio replaces it under the progress bar as each preview arrives (the UI watches
  `slices_read` in `project_status`); the step keeps the last one.
- Sources read directly (local raw files, phantoms) get only the first look: reading more there
  would read the volume twice, and their import is bound by the passes, not by reading. A stream
  read in one pass (gzip) still gives its one preview at the end of the copy, now complete.
- Example: 2000 slices of 3072 x 3072 voxels give blocks of 9 voxels and 223 preview slices
  (342 x 342 x 223, 26 million voxels, 52 MB). Previews come after 64, 192 and 223 slices and at
  the end; the first look still costs 3 % of the reading, the sharp preview 11 %.

## Consequences

- The first picture comes after about 3 % of the reading: on 1 Gbit/s Ethernet after about 11 s
  for the volume above, on 10 Gbit/s after one or two seconds, while the whole import still
  reads every byte once. The seven Me 163 archives (512 x 3584 x 512 voxels, ZIP on a FUSE
  mount) showed their preview after 0.8 s of an 18 s import.
- The slice spacing of the preview is coarse (about 31 slices for 2000); thin features along z
  can fall between the slices read. The preview is for orientation and the threshold, not for
  measuring.
- Raw files on a share now need local disk space for the staging copy (2 bytes per voxel), as
  stacks already do; `--no-staging` keeps reading them directly.
- Not done: starting the import from the preview's threshold with a dialog in between. The
  import does not wait for the user.
