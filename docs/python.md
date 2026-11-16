# VoxelSieve for Python

The `voxelsieve` package gives Python and numpy access to VoxelSieve: read sieved CT datasets of
any size as numpy arrays, write numpy volumes as datasets, process them brick by brick, and run
every analysis of VoxelSieve (porosity, material segmentation, surface, nominal-actual comparison,
wall thickness, fibre orientation, battery cells, reports, ...) as steps of a project, the same
projects the GUI and the MCP server open. Design: [ADR 0031](adr/0031-python-package.md).

## Install

The package is built from source with pip; CMake builds libvoxelsieve and the extension module.
The C++ dependencies come from vcpkg, as for the command-line tools ([Build](../README.md#build)):

```sh
tools/setup-vcpkg.sh && export VCPKG_ROOT=~/vcpkg
pip install .
```

Without `VCPKG_ROOT`, CMake looks for the dependencies on the system. The first build compiles
OpenVDB, Boost and TBB and takes a while; later builds reuse vcpkg's binary cache. Python 3.10 or
newer and numpy are needed.

From a CMake build tree, configure with `-DVOXELSIEVE_BUILD_PYTHON=ON` (after
`pip install nanobind`) and put `build/<preset>/python` on `PYTHONPATH`; on Windows also set
`VOXELSIEVE_DLL_DIR` to `build/<preset>/bin`.

## Axis order

VoxelSieve stores volumes x fastest, then y, then z. As numpy arrays these are C-ordered arrays
of shape `(nz, ny, nx)`, indexed `volume[z, y, x]`, which is how `tifffile` and `scikit-image`
see image stacks too, and no copy is needed. Arrays use this order everywhere. Metadata keeps
VoxelSieve's own order, as in its JSON files and the other tools: `dims`, `voxel_size_mm`, brick
indices and the boxes of `read` are `(x, y, z)`.

## Datasets

```python
import numpy as np
import voxelsieve as vs

ds = vs.Dataset.open("scan.vsieve")      # reads only index.json
ds.dims, ds.shape, ds.voxel_size_mm, ds.value_type   # (nx, ny, nz), (nz, ny, nx), mm, "uint16"

slab = ds[400:410]                       # ten slices; only the bricks they touch are loaded
row = ds[512, 300, :]                    # numpy-style integers and slices, also ... and steps
box = ds.read((0, 0, 400), (256, 256, 410))           # box [min, max) in (x, y, z)
kept = ds.active((0, 0, 400), (256, 256, 410))        # voxels the sieve kept: part and voids
overview = ds.level(3)[:, :, :]          # the 8x coarser level, for a quick look at a huge scan
```

Removed outside air reads as `fill` (default 0); `fill=np.nan` returns float32 with NaN there.
Grey values come in the dataset's type (`uint16`, `uint8` or `float32`); float scans were mapped
onto 16 bit on import, and `ds.to_values(grey)` maps grey values back to the scan's values.
Bricks are kept in a cache of `cache_bytes` (default 1 GiB, `Dataset.open(path, cache_bytes=...)`);
`on_access=True` reads only the voxels that are accessed, which suits single slices.

A scan larger than memory is processed brick by brick:

```python
histogram = np.zeros(65536, np.int64)
for brick, slices, values in ds.iter_bricks(fill=np.nan, dtype=np.float32):
    grey = values[~np.isnan(values)].astype(np.uint16)
    histogram += np.bincount(grey, minlength=65536)
```

## Writing datasets

`write_dataset` sieves a numpy volume: it removes only the outside air and keeps the part and its
internal voids (ADR 0003), estimating the threshold from the histogram unless one is given.

```python
volume = tifffile.imread("stack.tif")                 # (nz, ny, nx) uint16
ds = vs.write_dataset(volume, "part.vsieve", voxel_size_mm=(0.05, 0.05, 0.08))

raw = np.memmap("scan.raw", dtype="<u2", mode="r", shape=(2000, 3000, 3000))
ds = vs.write_dataset(raw, "scan.vsieve", 0.02)       # read through the page cache, not into RAM
```

uint16 and uint8 volumes are stored losslessly. Float volumes are mapped linearly onto 16 bit
and the mapping is recorded (`ds.value_mapping`, ADR 0015). Raw files, TIFF stacks, DICOM and
other formats are better imported with the operations below, which read them out of core.

`write_derived_dataset` applies your own processing to every brick and writes a new dataset with
the same part, for example a filter from SciPy:

```python
from scipy import ndimage

smooth = vs.write_derived_dataset(
    ds, "smooth.vsieve",
    lambda values: ndimage.gaussian_filter(np.nan_to_num(values, nan=ds.info["air_level"]), 1.5),
    halo=6)
```

The function gets the brick's values grown by `halo` voxels per side, as float32 with NaN outside
the part, and returns an array of the same shape. Only the brick's own voxels are kept, and a
NaN result removes a voxel. Memory use stays at a few bricks.

## Projects and analyses

Every analysis is an operation; `vs.operations()` lists them with their parameters as JSON schema.
A project runs them as steps with a protocol, undo and redo, and is saved at once:

```python
project = vs.Project.create("housing-4711", "Housing 4711")
project.run("import_raw", path="scan.raw", dims=[1024, 1024, 800], voxel_size_mm=0.08)
project.run("segment_materials")
pores = project.run("porosity", min_pore_voxels=8)
print(pores.summary)            # pores, pore volume, porosity, ...
pores.path()                    # directory with porosity.json, images and porosity.vdb

labels = project.run("segment_materials").materials()
labels[400]                     # material ids of slice 400 as uint8 (0 = air)

project.run("report", order_path="examples/inspection_order.json")
project.undo()
```

Inputs not given are taken from the latest step with an output of the required type, of the
active object first, as in the GUI. `inputs={"dataset": step}` picks one explicitly. Projects
with several volumes and meshes (ADR 0018) work the same: `object=` names the object a step
works on, `name=` the object an import creates, and `project.objects` lists them with their
poses. A failing operation raises an exception and stays in the protocol as a failed step.
`project.load_plugins(dir)` adds the operations of plugins.

## Tests

`python -m unittest discover -s python/tests` after `pip install .`, or `ctest -R python_package`
in a CMake build with `-DVOXELSIEVE_BUILD_PYTHON=ON`. The tests use `vs.phantom(...)`, the
synthetic phantom of VoxelSieve's own tests, whose geometry is known analytically.
