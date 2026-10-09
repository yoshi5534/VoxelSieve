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
- Not done: a finer preview while the import goes on (more slices as staging proceeds), and
  starting the import from the preview's threshold with a dialog in between. The import does not
  wait for the user.
