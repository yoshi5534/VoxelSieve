"""VoxelSieve for Python: numpy access to sieved CT datasets and VoxelSieve's analyses.

Arrays follow numpy's order for VoxelSieve's x-fastest volumes: a volume of dims (nx, ny, nz) is
an array of shape (nz, ny, nx), indexed ``volume[z, y, x]``. Metadata keeps VoxelSieve's own
order, as in its JSON files: ``dims``, voxel sizes, brick indices and boxes are (x, y, z).

    import voxelsieve as vs

    ds = vs.Dataset.open("scan.vsieve")       # reads only index.json
    slab = ds[100:110, :, :]                  # ten slices as a numpy array, bricks loaded lazily
    coarse = ds.level(2)[:, 512, :]           # an xz slice of the 4x coarser level

    project = vs.Project.create("inspection", "Housing 4711")
    project.run("import_raw", path="scan.raw", dims=[512, 512, 512], voxel_size_mm=0.1)
    pores = project.run("porosity")           # inputs come from the latest step
    print(pores.summary)

See docs/python.md for more.
"""

from __future__ import annotations

import json
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterator, Optional, Sequence, Union

if sys.platform == "win32":
    # In a build tree the DLLs of VoxelSieve and vcpkg are in build/<preset>/bin; in a wheel they
    # are next to _core and found without this.
    _dll_dir = os.environ.get("VOXELSIEVE_DLL_DIR")
    if _dll_dir and os.path.isdir(_dll_dir):
        os.add_dll_directory(_dll_dir)

import numpy as np

from . import _core

__all__ = [
    "Dataset",
    "Level",
    "MaterialVolume",
    "Project",
    "Step",
    "operations",
    "phantom",
    "write_dataset",
    "write_derived_dataset",
    "__version__",
]

__version__ = _core.version()

PathLike = Union[str, os.PathLike]
VoxelSizeLike = Union[float, Sequence[float]]

_NUMPY_TYPES = {"uint8": np.uint8, "uint16": np.uint16, "float": np.float32}


def _pitch(voxel_size_mm: VoxelSizeLike) -> list[float]:
    if np.isscalar(voxel_size_mm):
        return [float(voxel_size_mm)] * 3
    pitch = [float(v) for v in voxel_size_mm]  # type: ignore[union-attr]
    if len(pitch) != 3:
        raise ValueError("voxel_size_mm must be a number or [x, y, z]")
    return pitch


def _box_from_key(key: Any, shape: tuple[int, int, int]):
    """numpy-style key over (z, y, x) -> (min xyz, max xyz, post-index into the read box)."""
    if not isinstance(key, tuple):
        key = (key,)
    if any(k is Ellipsis for k in key):
        index = key.index(Ellipsis)
        key = key[:index] + (slice(None),) * (3 - len(key) + 1) + key[index + 1 :]
    if len(key) > 3:
        raise IndexError("A volume has three axes (z, y, x)")
    key = key + (slice(None),) * (3 - len(key))
    lo: list[int] = []
    hi: list[int] = []
    post: list[Any] = []
    for k, size in zip(key, shape):
        if isinstance(k, slice):
            start, stop, step = k.indices(size)
            if step < 0:
                raise IndexError("Negative steps are not supported; use numpy on the result")
            stop = max(stop, start)
            lo.append(start)
            hi.append(stop)
            post.append(slice(None, None, step))
        elif isinstance(k, (int, np.integer)):
            i = int(k)
            if i < 0:
                i += size
            if not 0 <= i < size:
                raise IndexError(f"Index {int(k)} is out of range for an axis of size {size}")
            lo.append(i)
            hi.append(i + 1)
            post.append(0)
        else:
            raise IndexError("Only integers, slices and ... index a dataset")
    # (z, y, x) -> (x, y, z)
    return lo[::-1], hi[::-1], tuple(post)


class Level:
    """One resolution level of a dataset, indexed like a numpy array of shape (nz, ny, nx).

    Level L holds voxels 2^L times as large per axis as level 0; a level-L voxel is the mean of
    the active level-0 voxels it covers. Removed outside air reads as ``fill`` (default 0).
    """

    def __init__(self, dataset: "Dataset", level: int):
        self._dataset = dataset
        self.index = level
        info = dataset.info["levels"][level]
        self.dims: tuple[int, int, int] = tuple(info["dims"])  # (x, y, z)
        self.voxel_size_mm: tuple[float, float, float] = tuple(info["voxel_size"]["pitch_mm"])

    @property
    def shape(self) -> tuple[int, int, int]:
        """(nz, ny, nx)."""
        return (self.dims[2], self.dims[1], self.dims[0])

    @property
    def dtype(self) -> np.dtype:
        return self._dataset.dtype

    def __getitem__(self, key: Any) -> np.ndarray:
        lo, hi, post = _box_from_key(key, self.shape)
        return self.read(lo, hi)[post]

    def read(
        self,
        min_xyz: Sequence[int],
        max_xyz: Sequence[int],
        fill: float = 0,
        dtype: Optional[Any] = None,
    ) -> np.ndarray:
        """Voxels of the box [min, max) given in (x, y, z), as an array of shape (z, y, x).

        The result has the dataset's value type unless ``dtype`` is given; ``fill`` is written where
        no voxel is active (outside air). With ``fill=numpy.nan`` the result is float32.
        """
        values = self._dataset._core.read_region(
            self.index, list(map(int, min_xyz)), list(map(int, max_xyz)), float(fill)
        )
        if dtype is None:
            dtype = np.float32 if np.isnan(fill) else self.dtype
        return values if np.dtype(dtype) == np.float32 else values.astype(dtype)

    def active(self, min_xyz: Sequence[int], max_xyz: Sequence[int]) -> np.ndarray:
        """Boolean mask of the voxels the sieve kept (the part and its internal voids)."""
        return ~np.isnan(self.read(min_xyz, max_xyz, fill=np.nan))

    def bricks(self) -> list[tuple[int, int, int]]:
        """Indices (x, y, z) of the stored bricks; bricks of only outside air are not stored."""
        return [tuple(b) for b in self._dataset._core.bricks(self.index)]

    def brick_box(self, brick: Sequence[int]) -> tuple[tuple[int, ...], tuple[int, ...]]:
        """Voxel box (min, max) in (x, y, z) of a brick, clipped to the level."""
        lo, hi = self._dataset._core.brick_box(self.index, list(brick))
        return tuple(lo), tuple(hi)

    def iter_bricks(
        self, fill: float = 0, dtype: Optional[Any] = None
    ) -> Iterator[tuple[tuple[int, int, int], tuple[slice, slice, slice], np.ndarray]]:
        """Yields (brick, slices, values) for every stored brick, one after the other, so that a
        volume much larger than memory can be processed brick by brick. ``slices`` places the
        values in the level: ``volume[slices] = values``."""
        for brick in self.bricks():
            lo, hi = self.brick_box(brick)
            slices = tuple(slice(lo[a], hi[a]) for a in (2, 1, 0))
            yield brick, slices, self.read(lo, hi, fill=fill, dtype=dtype)  # type: ignore[misc]


class Dataset:
    """A bricked, multi-resolution dataset written by the sieve (``.vsieve`` directory).

    Opening reads only index.json; bricks are loaded on demand into a cache of ``cache_bytes``.
    ``on_access=True`` loads the values of a brick only where they are read, which suits sparse
    access such as single slices. Indexing a dataset reads level 0:
    ``dataset[z0:z1, y0:y1, x0:x1]``.
    """

    def __init__(self, core: Any):
        self._core = core
        self.info: dict = json.loads(core.info())
        self.levels = [Level(self, level) for level in range(len(self.info["levels"]))]

    @classmethod
    def open(cls, path: PathLike, cache_bytes: int = 1 << 30, on_access: bool = False) -> "Dataset":
        return cls(_core.Dataset.open(os.fspath(path), int(cache_bytes), bool(on_access)))

    @property
    def path(self) -> Path:
        return Path(self._core.dir())

    @property
    def dims(self) -> tuple[int, int, int]:
        """(nx, ny, nz) of level 0."""
        return tuple(self.info["dims"])  # type: ignore[return-value]

    @property
    def shape(self) -> tuple[int, int, int]:
        """(nz, ny, nx) of level 0, the shape of the volume as a numpy array."""
        return self.levels[0].shape

    @property
    def voxel_size_mm(self) -> tuple[float, float, float]:
        """Voxel edge lengths (x, y, z) in mm; voxels need not be cubes."""
        return tuple(self.info["voxel_size"]["pitch_mm"])  # type: ignore[return-value]

    @property
    def value_type(self) -> str:
        """"uint16", "uint8" or "float": the stored grey values, lossless in the input's width."""
        return self.info["value_type"]

    @property
    def dtype(self) -> np.dtype:
        return np.dtype(_NUMPY_TYPES[self.value_type])

    @property
    def value_mapping(self) -> tuple[float, float]:
        """(offset, scale): scan value = offset + scale * grey value (float scans, ADR 0015)."""
        mapping = self.info["value_mapping"]
        return mapping["offset"], mapping["scale"]

    def to_values(self, grey: np.ndarray) -> np.ndarray:
        """Grey values as values of the scan (identity unless the input was float)."""
        offset, scale = self.value_mapping
        if offset == 0 and scale == 1:
            return grey
        return offset + scale * grey.astype(np.float64)

    @property
    def threshold(self) -> float:
        return self.info["threshold"]

    def level(self, level: int) -> Level:
        return self.levels[level]

    def __getitem__(self, key: Any) -> np.ndarray:
        return self.levels[0][key]

    def read(self, min_xyz, max_xyz, fill: float = 0, dtype=None, level: int = 0) -> np.ndarray:
        return self.levels[level].read(min_xyz, max_xyz, fill=fill, dtype=dtype)

    def active(self, min_xyz, max_xyz, level: int = 0) -> np.ndarray:
        return self.levels[level].active(min_xyz, max_xyz)

    def bricks(self, level: int = 0) -> list[tuple[int, int, int]]:
        return self.levels[level].bricks()

    def iter_bricks(self, level: int = 0, fill: float = 0, dtype=None):
        return self.levels[level].iter_bricks(fill=fill, dtype=dtype)

    def cache_stats(self) -> dict:
        return json.loads(self._core.cache_stats())

    def __repr__(self) -> str:
        return (
            f"Dataset({str(self.path)!r}, dims={self.dims}, value_type={self.value_type!r}, "
            f"levels={len(self.levels)})"
        )


def write_dataset(
    volume: np.ndarray,
    path: PathLike,
    voxel_size_mm: VoxelSizeLike = 1.0,
    *,
    slice_thickness_mm: float = 0.0,
    threshold: Optional[float] = None,
    margin_voxels: int = 3,
    brick_size: int = 256,
    min_material_voxels: int = 1,
    value_type: Optional[str] = None,
) -> Dataset:
    """Sieves a volume of shape (nz, ny, nx) into a dataset at ``path`` and opens it.

    Only the outside air is removed; the part and its internal voids are kept (ADR 0003). The
    threshold between air and material is estimated from the histogram unless given.

    uint16 and uint8 arrays are stored losslessly. A ``numpy.memmap`` of a raw file is read through
    the operating system's page cache, so the volume need not fit in memory. Float arrays are
    mapped linearly onto 16 bit (their minimum to 0, their maximum to 65535) and the mapping is
    recorded in the dataset (``Dataset.value_mapping``, ADR 0015); that needs a 16-bit copy in
    memory. ``value_type="float"`` stores float grids, for viewers that know no integer grids.
    """
    volume = np.asarray(volume)
    if volume.ndim != 3:
        raise ValueError("The volume must have three axes (z, y, x)")
    options = _core.WriteOptions()
    options.pitch_mm = _pitch(voxel_size_mm)
    options.slice_thickness_mm = float(slice_thickness_mm)
    options.margin_voxels = int(margin_voxels)
    options.brick_size = int(brick_size)
    options.min_material_voxels = int(min_material_voxels)
    options.value_type = value_type
    if np.issubdtype(volume.dtype, np.floating):
        finite = volume[np.isfinite(volume)]
        if finite.size == 0:
            raise ValueError("The volume holds no finite value")
        low, high = float(finite.min()), float(finite.max())
        scale = (high - low) / 65535.0 if high > low else 1.0
        grey = np.clip(np.rint((np.nan_to_num(volume, nan=low) - low) / scale), 0, 65535)
        options.mapping_offset = low
        options.mapping_scale = scale
        if threshold is not None:
            threshold = (threshold - low) / scale
        volume = grey.astype(np.uint16)
    options.threshold = None if threshold is None else float(threshold)
    path = os.fspath(path)
    if volume.dtype == np.uint8:
        _core.write_dataset_u8(np.ascontiguousarray(volume), path, options)
    elif volume.dtype == np.uint16:
        _core.write_dataset_u16(np.ascontiguousarray(volume), path, options)
    elif np.issubdtype(volume.dtype, np.integer):
        if volume.min() < 0 or volume.max() > 65535:
            raise ValueError("Integer volumes must hold values from 0 to 65535")
        _core.write_dataset_u16(np.ascontiguousarray(volume, dtype=np.uint16), path, options)
    else:
        raise TypeError(f"Unsupported volume type {volume.dtype}")
    return Dataset.open(path)


def write_derived_dataset(
    dataset: Dataset,
    path: PathLike,
    function: Callable[[np.ndarray], np.ndarray],
    *,
    halo: int = 0,
    value_type: Optional[str] = None,
) -> Dataset:
    """Writes a dataset with the part of ``dataset`` and new grey values computed brick by brick.

    ``function(values)`` gets the level-0 values of one brick, grown by ``halo`` voxels on every
    side for neighbourhood filters (clipped at the volume's border), as a float32 array of shape
    (z, y, x) with NaN where no voxel is active, and returns an array of the same shape. Only the
    brick's own voxels that are active in the input are kept, and a NaN result removes a voxel.
    The coarser levels are rebuilt from the new values. Memory use is a few bricks, so this works
    on scans much larger than memory. Integer datasets store the results rounded.
    """

    def per_brick(_brick, _lo, _hi, values):
        result = np.asarray(function(values), dtype=np.float32)
        return np.ascontiguousarray(result)

    _core.write_derived_dataset(dataset._core, os.fspath(path), per_brick, int(halo), value_type)
    return Dataset.open(path)


class MaterialVolume:
    """Material labels of a segmentation (ADR 0013): 0 is air, 1 and up the materials of
    ``materials``. Indexed like a dataset: ``labels[z0:z1, y0:y1, x0:x1]`` as uint8."""

    def __init__(self, core: Any):
        self._core = core
        self.info: dict = json.loads(core.info())

    @classmethod
    def open(cls, path: PathLike) -> "MaterialVolume":
        return cls(_core.MaterialVolume.open(os.fspath(path)))

    @property
    def materials(self) -> list[dict]:
        return self.info["materials"]

    @property
    def dims(self) -> tuple[int, int, int]:
        return tuple(self.info["dims"])  # type: ignore[return-value]

    @property
    def shape(self) -> tuple[int, int, int]:
        return (self.dims[2], self.dims[1], self.dims[0])

    def read(self, min_xyz: Sequence[int], max_xyz: Sequence[int]) -> np.ndarray:
        return self._core.read_region(list(map(int, min_xyz)), list(map(int, max_xyz)))

    def __getitem__(self, key: Any) -> np.ndarray:
        lo, hi, post = _box_from_key(key, self.shape)
        return self.read(lo, hi)[post]


@dataclass
class Step:
    """A step of a project's protocol: the operation, its parameters and what it produced."""

    id: int
    operation: str
    title: str
    status: str
    params: dict
    summary: dict
    outputs: dict  # port -> {"path", "type"}
    inputs: dict = field(default_factory=dict)  # port -> {"step", "output"}
    messages: list = field(default_factory=list)
    object: str = ""
    telemetry: dict = field(default_factory=dict)
    started: str = ""
    finished: str = ""

    @classmethod
    def _from(cls, data: dict) -> "Step":
        names = cls.__dataclass_fields__.keys()
        return cls(**{k: v for k, v in data.items() if k in names})

    def path(self, output: Optional[str] = None) -> Path:
        """Path of an output; the only one when ``output`` is not given."""
        if output is None:
            if len(self.outputs) != 1:
                raise ValueError(f"Step {self.id} has outputs {sorted(self.outputs)}; name one")
            output = next(iter(self.outputs))
        return Path(self.outputs[output]["path"])

    def dataset(self, output: str = "dataset", **open_args) -> Dataset:
        return Dataset.open(self.path(output), **open_args)

    def materials(self, output: str = "materials") -> MaterialVolume:
        return MaterialVolume.open(self.path(output))


class Project:
    """A VoxelSieve project (ADR 0008): every operation runs as a step of a protocol that can be
    undone and redone and is saved at once, the same projects the GUI and the MCP server open.

    ``run(operation, **params)`` runs any operation (``operations()`` lists them with their
    parameter schemas). Inputs not given are taken from the latest step with an output of the
    required type, of the active object first, as in the GUI.
    """

    def __init__(self, core: Any):
        self._core = core

    @classmethod
    def create(cls, path: PathLike, name: str = "") -> "Project":
        path = os.fspath(path)
        return cls(_core.Project.create(path, name or Path(path).name))

    @classmethod
    def open(cls, path: PathLike) -> "Project":
        return cls(_core.Project.open(os.fspath(path)))

    @property
    def path(self) -> Path:
        return Path(self._core.dir())

    @property
    def name(self) -> str:
        return self._core.name()

    def run(
        self,
        operation: str,
        *,
        inputs: Optional[dict] = None,
        object: str = "",
        name: str = "",
        progress: Optional[Callable[[float], None]] = None,
        **params: Any,
    ) -> Step:
        """Runs ``operation`` with ``params`` as a new step and returns it.

        ``inputs`` maps input ports to outputs of earlier steps, as a Step (its only output, or
        the output of the port's name) or (step id, output name). ``object`` names the object
        (volume or mesh) the step works on; ``name`` the name of an object the step creates.
        Paths among the parameters may be ``pathlib.Path``. Raises when the operation fails; the
        failed step stays in the protocol.
        """
        refs = {}
        for port, ref in (inputs or {}).items():
            if isinstance(ref, Step):
                output = port if port in ref.outputs else None
                if output is None:
                    if len(ref.outputs) != 1:
                        raise ValueError(f"Name the output of step {ref.id} for input {port!r}")
                    output = next(iter(ref.outputs))
                refs[port] = (ref.id, output)
            else:
                refs[port] = (int(ref[0]), str(ref[1]))
        text = json.dumps(params, default=os.fspath)
        return Step._from(json.loads(self._core.run(operation, text, refs, object, name, progress)))

    @property
    def steps(self) -> list[Step]:
        return [Step._from(s) for s in json.loads(self._core.steps())]

    @property
    def objects(self) -> list[dict]:
        """Volumes and meshes of the project with their poses (ADR 0018)."""
        return json.loads(self._core.objects())

    def results(self, object: str = "") -> list[dict]:
        """Analysis results of the active steps (ADR 0023), of one object when given."""
        return json.loads(self._core.results(object))

    def select_object(self, object_id: str) -> None:
        self._core.select_object(object_id)

    def undo(self) -> bool:
        return self._core.undo()

    def redo(self) -> bool:
        return self._core.redo()

    def latest(self, output_type: str, object: str = "") -> Optional[Path]:
        """Path of the latest active output of a type ("dataset", "porosity", "materials", ...)."""
        path = self._core.latest(output_type, object)
        return None if path is None else Path(path)

    def dataset(self, object: str = "", **open_args) -> Dataset:
        """The latest dataset, of one object when given."""
        path = self.latest("dataset", object)
        if path is None:
            raise LookupError("The project has no dataset")
        return Dataset.open(path, **open_args)

    def operations(self) -> list[dict]:
        """Operations this project can run, built-in and from loaded plugins."""
        return json.loads(self._core.operations())

    def load_plugins(self, directory: PathLike) -> list[str]:
        """Loads operation plugins (shared libraries) from a directory; one message per file."""
        return self._core.load_plugins(os.fspath(directory))

    def __repr__(self) -> str:
        return f"Project({str(self.path)!r}, steps={len(self.steps)})"


def operations() -> list[dict]:
    """The built-in operations: id, title, description, inputs, outputs and the JSON schema of
    their parameters."""
    return json.loads(_core.operations())


def phantom(
    dims: Sequence[int] = (128, 128, 128),
    voxel_size_mm: float = 0.1,
    *,
    pores: Optional[Sequence[tuple[Sequence[float], float]]] = None,
    air_value: int = 1000,
    material_value: int = 20000,
    noise_sigma: float = 0.0,
    seed: int = 42,
    outer_size_mm: Sequence[float] = (8.0, 8.0, 8.0),
    wall_thickness_mm: float = 1.5,
) -> tuple[np.ndarray, float]:
    """The synthetic phantom of VoxelSieve's tests: a hollow box centred in the volume with
    spherical pores in its walls, as a uint16 array of shape (nz, ny, nx), and its analytic
    material volume in mm^3. ``pores`` are ((x, y, z) centre in mm from the volume centre,
    radius in mm); default: the three pores of VoxelSieve's default phantom."""
    if pores is None:
        pores = [((3.25, 0.0, 0.0), 0.40), ((-3.25, 1.5, -1.0), 0.30), ((0.5, -0.5, 3.25), 0.20)]
    flat = [[float(c[0]), float(c[1]), float(c[2]), float(r)] for c, r in pores]
    return _core.phantom(
        [int(d) for d in dims],
        float(voxel_size_mm),
        int(air_value),
        int(material_value),
        float(noise_sigma),
        int(seed),
        [float(s) for s in outer_size_mm],
        float(wall_thickness_mm),
        flat,
    )
