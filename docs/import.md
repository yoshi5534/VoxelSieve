# Importing scans

Every input becomes a sparse, bricked VDB dataset: raw volumes (with or without a vendor header
or sidecar), TIFF and DICOM stacks, also inside ZIP archives, gzip-compressed raw files and
DICONDE. In the [browser UI](gui.md) and over [MCP](mcp.md), "Open a scan" (`open_scan`) finds
out what a file is and imports it without a form ([ADR 0039](adr/0039-opening-a-scan.md)); the
command line is `vs-sieve`.

VoxelSieve writes either a bricked dataset (any size, streamed) or a single `.vdb` grid (small
volumes, in memory).

## Bricked dataset for large scans

```sh
vs-sieve scan.raw --out scan.vsieve
```

`scan.vsieve/` holds `index.json`, `overview.vdb` (the whole volume in one grid of at most 256³
voxels) and `level0/`, `level1/`, … with one VDB file per 256³ brick. Bricks that contain only
outside air are not written. The input is memory-mapped and processed in two streaming passes, so
the scan does not need to fit in RAM. `--phantom N` sieves a computed N³ phantom instead of a file.

Raw files from CT systems usually start with a proprietary header. VoxelSieve skips it: given the
dimensions, everything in the file beyond the voxel data is taken as the header, and the detected
size is printed. Use `--header <bytes>` when the file also has a footer, `--type uint8` for 8-bit
data and `--big-endian` for big-endian 16-bit samples. Check the printed header size: wrong
dimensions that are too small also leave extra bytes and would be misread as a header.

| Input | Raw size | Dataset | Time | Peak memory |
|---|---|---|---|---|
| 1024³ raw file (mmap) | 2 GB | 776 MB | 6.1 s | 2.5 GB, of which ~2 GB reclaimable mapped input pages |
| 1024³ computed phantom | (2 GB) | 776 MB | 35.5 s | 0.46 GB |
| 2048³ computed phantom | (16 GB) | 5.8 GB | 345 s | 0.87 GB |

The computed phantom is slower because every voxel is evaluated twice; its memory figure shows the
sieve's own footprint without mapped input pages.

Programs read a dataset through `voxelsieve::Dataset`, which loads bricks on demand into an LRU
cache with a memory budget:

```cpp
const auto dataset = voxelsieve::Dataset::open("scan.vsieve", /*cache_bytes=*/4ULL << 30U);
std::optional<float> value = dataset.sample(0, {512, 400, 300});  // nullopt for removed air
std::vector<float> slab(512 * 512 * 16);
dataset.readRegion(0, {{0, 0, 300}, {512, 512, 316}}, slab);      // across brick boundaries
dataset.forEachGrid(1, [](const auto& brick, const auto& grid) { /* parallel, uint16/uint8/float */ });
```

With "Open a scan" (`inspect_scan`, `open_scan`), a raw file needs no sidecar: its size, sample type, byte order and header are read from its name
(`part_512x512x400_16bit_0.05mm.raw`) or detected in the grey values; a missing voxel size is
assumed as 1 mm with a warning.

## TIFF stacks

```sh
vs-sieve slices/ --out scan.vsieve --voxel-size 0.05
vs-sieve scan.zip --out scan.vsieve        # read without extracting
vs-sieve scan.zip --folder target --threshold 0.5 --out labels.vsieve
vs-sieve recon.zip --value-range -0.05,0.3 --out scan.vsieve  # float
vs-sieve pipe.zip --air-from xy --out pipe.vsieve  # cut at both ends
```

A directory of slices, a multi-page TIFF or a ZIP archive of either is read directly
(`voxelsieve::TiffStackSource`, ADR 0011). Slices are sorted by name with numbers by value
(`slice2` before `slice10`). Supported are classic TIFF and BigTIFF, strips and tiles,
uncompressed, LZW, Deflate and PackBits, with or without horizontal predictor, and unsigned 8 or 16
bit samples; 32-bit samples are accepted as long as they fit into 16 bits (label volumes). Float
slices (32 or 64 bit, also with the floating-point predictor) are mapped linearly onto grey values
0 to 65535 over a value range, given with `--value-range` or estimated from 33 slices; the
mapping is printed and recorded in the dataset's `index.json` (`value = offset + scale * grey`),
and values outside the range are clipped and counted (ADR 0015). Colour and signed images are
refused. When the first and last slice cut through the part (a pipe, a drill core), `--air-from
xy` lets outside air in only from the sides, so the inside of the part is kept (ADR 0003). When
the input holds several folders of slices, the grey values are taken, not a folder named like labels or masks (`label*`, `mask*`,
`seg*`, `gt`, `target`); the others are printed and can be chosen with `--folder`. The voxel size
comes from an OME-XML image description (`PhysicalSizeX`, `Y`, `Z`), a centimetre resolution, an
inch resolution that is not a printing default such as 72 or 300 dpi, or a key=value image
description (including its slice `spacing`), otherwise 1 mm is assumed and printed; set it with
`--voxel-size`. In the GUI this is the operation `import_tiff`.
For a dataset, the slices are first decoded once, on all cores, into a copy of 2 bytes per
voxel: as many slices as fit into the memory budget (`--memory`) and the free memory stay in
memory, the others go to a temporary raw file in the output directory (elsewhere with
`--staging-dir`), which is removed at the end;
`--no-staging` reads the slices directly, which is much slower for large stacks.

## DICOM stacks

```sh
vs-sieve dicom/ --out scan.vsieve          # a directory of slices
vs-sieve dicom.zip --out scan.vsieve       # the same in a ZIP archive
```

A directory of DICOM slices, or a ZIP archive of them (read without extracting it, each entry
checked against its CRC), is read with DCMTK (`voxelsieve::DicomStackSource`, ADR 0019):
uncompressed or RLE, with or without preamble, sorted along the slice normal by their position.
Voxel size and pose come from the files. Signed samples are shifted by 32768 and, with the
rescale slope and intercept, recorded as the value mapping, so no value is lost. When a directory
holds several series, the largest is read and the others are printed. In the GUI this is the
operation `import_dicom`. When the slices are thinner than their spacing, or much farther apart
than the pixels, the import summary warns (`warnings`), and `interpolate_slices` (all imports)
fills the gaps with slices interpolated between the measured ones.

## DICONDE

```sh
vs-diconde scan.vsieve --porosity scan.porosity \
  --materials scan.materials --order examples/inspection_order.json --out scan.diconde
vs-diconde --info scan.diconde/volume
```

DICONDE (ASTM E2339) is DICOM for nondestructive evaluation (ADR 0024). VoxelSieve writes a
dataset as a series of CT images, one file per slice, with the exact grey values, the voxel size
and the position of the object; removed outside air gets the air level. The inspection order
becomes the component (part, serial number, drawing, material), the inspection (laboratory,
customer, report and order number, inspector, the evaluation against the acceptance limits) and
the scan settings. Pores and loosened zones become indications with their position, volume and
size, and a material segmentation a second, derived series of labels in the same coordinates.
DICONDE files are read like other DICOM slices (`vs-sieve`, `import_dicom`), and the import
reports their component and indications. In the GUI this is the operation `export_diconde`.

## First look while importing, also from a network share

```sh
vs-sieve /mnt/ct-share/part.zip --out part.vsieve --preview first.png
```

Before the passes, the import reads 64 whole slices spread evenly over the volume and shows them
at once: the central sections normal to z, y and x, averaged in-plane to at most 1024 voxels per
edge and 32 million voxels in all, and the histogram of those slices with the estimated threshold
(ADR 0020). While a staged import copies the rest, the preview gets sharper: it reads the other
preview slices coarse to fine, three times as many in each round, and shows a new preview after
every round, the last with the histogram of the whole volume. Slices are what every
input stores together (a file of a stack, a contiguous part of a raw file), so this is one
request per slice, also over SMB. The import goes on without reading them again: stacks and
files on a network share are copied once, slice by slice, to a local staging file, and the
preview's slices are copied first. Local raw files are read directly and get only the first
look; their import is limited by the passes, not by reading. Raw files on SMB or NFS shares and mapped network drives are
detected and staged too, instead of being read twice in small pieces. In the GUI the preview
appears in the progress bar of every import and stays with the step (`preview.png`); AI
assistants get it with `import_preview`.

gzip-compressed raw files (`scan.raw.gz` next to `scan.json`) are read too, with `vs-sieve`,
`import_raw` and the GUI's file browser. gzip can only be read forward, so such a file is
decompressed once, in slice order, into the staging file; its preview comes at the end of that
pass. Give `--header` when the uncompressed data has a header.

## Single grid

```sh
vs-sieve phantom.raw --out phantom.vdb
```

Dimensions and voxel size are read from `phantom.json`, or given with `--dims X Y Z` and
`--voxel-size MM`. Voxels need not be cubes (ADR 0012): `--voxel-size 0.33,0.33,0.6` gives the
edge along x, y and z, and `--slice-thickness 0.4` records slices thinner than their spacing
(a gap between slices). All measurements, VDB transforms, images and viewers use the pitch per
axis. The air/material threshold is estimated from the histogram unless `--threshold` is set:
the valley between the air peak and the next material, so light materials in a scan of several
count as material (Otsu's split when there is only one); `--margin` controls how many voxels of air stay around the part. `--dense` writes every
voxel without sieving, as a baseline. Grey values keep the input's 16 or 8 bits (ADR 0021); with
`--value-type float` the `.vdb` and every brick of a dataset open directly in Blender or Houdini,
which read only OpenVDB's standard grid types.

Phantom benchmark (hollow box with pores filling about a quarter of the volume, noise σ = 500,
4 cores):

| Volume | Raw | Dense VDB | Sieved VDB | Active voxels | Sieve time |
|---|---|---|---|---|---|
| 256³ | 32 MB | 43.6 MB | 11.8 MB | 27 % | 0.03 s |
| 512³ | 256 MB | 348.9 MB | 85.9 MB | 26 % | 0.23 s |

The table was measured with grey values stored as 32-bit float (ADR 0002), which is why the dense
baseline is larger than the raw file. Since ADR 0021 grids keep the input's 16 bits: a dense VDB
of the 256³ phantom takes 115 % of the raw file instead of 140 %.

Design decisions are recorded in [docs/adr](adr).
