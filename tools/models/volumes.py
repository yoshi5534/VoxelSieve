"""Streamed patches of scans for the training scripts (train_unet.py, train_denoiser.py).

A scan is read patch by patch, never as a whole and without a pass over it before training:
from a headerless raw volume (x fastest) through a memory map, or from a VoxelSieve dataset
(.vsieve directory) or material volume brick by brick through the Python package
(``pip install .``), so a scan of hundreds of GB trains as well as a small one. Where to take
patches from is learned while training: each patch read adds a few positions of every class (or
of the material) it holds to a bounded reservoir, from which later patches are centred. Batches
are put together in a background thread while the network trains, on the GPU when there is one.

Arrays are indexed [z, y, x] like the volumes; corners and sizes are (z, y, x) too.
"""

import os
import queue
import threading

import numpy as np


def _is_dataset(path):
    return os.path.isdir(path)


class RawVolume:
    """A headerless raw volume, x fastest, memory-mapped."""

    def __init__(self, path, dims, dtype):
        x, y, z = dims
        self.shape = (z, y, x)
        self.array = np.memmap(path, dtype, "r", shape=self.shape)
        self.regions = None  # patches may come from anywhere
        self.name = os.path.basename(path)

    def read(self, corner, size):
        z, y, x = corner
        return np.asarray(self.array[z:z + size, y:y + size, x:x + size])


class DatasetVolume:
    """A VoxelSieve dataset: the grey values of level 0, the removed outside air at its air
    level. Patches come from around the bricks that hold voxels."""

    def __init__(self, path):
        import voxelsieve  # the Python package of this repository

        self.dataset = voxelsieve.Dataset.open(path)
        self.shape = self.dataset.shape
        self.air = float(self.dataset.info.get("air_level", 0.0))
        level = self.dataset.level(0)
        self.regions = []
        for brick in level.bricks():
            lo, hi = level.brick_box(brick)  # (x, y, z)
            self.regions.append((tuple(lo[::-1]), tuple(hi[::-1])))
        self.name = os.path.basename(os.path.normpath(path))

    def read(self, corner, size):
        z, y, x = corner
        return self.dataset.read([x, y, z], [x + size, y + size, z + size], fill=self.air)


class MaterialLabels:
    """A material volume of VoxelSieve (vs-segment): class 0 air, 1.. the materials."""

    def __init__(self, path):
        import voxelsieve

        self.volume = voxelsieve.MaterialVolume.open(path)
        x, y, z = self.volume.dims
        self.shape = (z, y, x)

    def read(self, corner, size):
        z, y, x = corner
        return self.volume.read([x, y, z], [x + size, y + size, z + size])


def open_grey(path, dims=None, dtype=np.uint16):
    """A dataset directory, or a raw file of `dims` (x, y, z)."""
    if _is_dataset(path):
        return DatasetVolume(path)
    if dims is None:
        raise SystemExit(f"{path}: a raw volume needs --dims")
    return RawVolume(path, dims, dtype)


def open_labels(path, dims=None):
    """A material volume directory, or a raw uint8 file of `dims` (x, y, z)."""
    if _is_dataset(path):
        return MaterialLabels(path)
    if dims is None:
        raise SystemExit(f"{path}: raw labels need --dims")
    return RawVolume(path, dims, np.uint8)


class PatchSampler:
    """Corners of patches of `size` voxels in a volume of `shape` (z, y, x).

    With probability `focus`, a patch is centred on a position of one of the kinds seen so far
    (each kind equally often); otherwise it lies anywhere, or around a region of `regions` (boxes
    (lo, hi) in z, y, x), such as the bricks of a dataset that hold voxels. `allowed(corner)` may
    refuse corners, such as those touching held-out slabs. `observe` fills the reservoirs of
    positions from the patches read.
    """

    def __init__(self, shape, size, rng, focus=0.75, regions=None, allowed=None, reservoir=4096,
                 per_patch=16):
        if min(shape) < size:
            raise SystemExit("The volume is smaller than a patch")
        self.shape = shape
        self.size = size
        self.rng = rng
        self.focus = focus
        self.regions = regions or None
        self.allowed = allowed or (lambda corner: True)
        self.capacity = reservoir
        self.per_patch = per_patch
        self.positions = {}  # kind -> array of positions (n, 3)
        self.seen = {}       # kind -> how many positions were offered

    def _clip(self, centre):
        return [int(np.clip(centre[i] - self.size // 2, 0, self.shape[i] - self.size))
                for i in range(3)]

    def _anywhere(self):
        if self.regions:
            lo, hi = self.regions[self.rng.integers(len(self.regions))]
            return self._clip([self.rng.integers(lo[i], hi[i]) for i in range(3)])
        return [int(self.rng.integers(0, d - self.size + 1)) for d in self.shape]

    def corner(self):
        for _ in range(10000):
            kinds = [k for k, p in self.positions.items() if len(p)]
            if kinds and self.rng.random() < self.focus:
                positions = self.positions[kinds[self.rng.integers(len(kinds))]]
                corner = self._clip(positions[self.rng.integers(len(positions))])
            else:
                corner = self._anywhere()
            if self.allowed(corner):
                return corner
        raise SystemExit("No patch lies outside the held-out regions")

    def observe(self, corner, kinds):
        """Adds positions of the patch at `corner`: `kinds` is {kind: boolean mask of the patch}."""
        for kind, mask in kinds.items():
            local = np.argwhere(mask)
            if not len(local):
                continue
            take = local[self.rng.integers(len(local), size=min(self.per_patch, len(local)))]
            found = take + np.asarray(corner)
            stored = self.positions.get(kind, np.empty((0, 3), np.int64))
            seen = self.seen.get(kind, 0)
            for position in found:
                seen += 1
                if len(stored) < self.capacity:
                    stored = np.vstack([stored, position])
                else:  # reservoir sampling: every position offered stays with equal chance
                    slot = self.rng.integers(seen)
                    if slot < self.capacity:
                        stored[slot] = position
            self.positions[kind] = stored
            self.seen[kind] = seen


def sample_values(volume, rng, count=64, size=32, regions=None):
    """Grey values of `count` random patches, for the normalisation of the input."""
    sampler = PatchSampler(volume.shape, min(size, *volume.shape), rng, focus=0.0,
                           regions=regions)
    return np.concatenate([volume.read(sampler.corner(), sampler.size).ravel()
                           for _ in range(count)]).astype(np.float32)


def prefetch(make, depth=4):
    """Yields make() forever, made ahead in a background thread (reading patches releases the
    GIL, so the reads overlap the training)."""
    ready = queue.Queue(depth)
    failure = []

    def work():
        try:
            while True:
                ready.put(make())
        except BaseException as error:  # noqa: BLE001 - passed to the training loop
            failure.append(error)
            ready.put(None)

    threading.Thread(target=work, daemon=True).start()
    while True:
        item = ready.get()
        if item is None and failure:
            raise failure[0]
        yield item


def device(name="auto"):
    """The torch device: `auto` takes CUDA, then Apple's MPS, else the CPU."""
    import torch

    if name != "auto":
        return torch.device(name)
    if torch.cuda.is_available():
        return torch.device("cuda")
    mps = getattr(torch.backends, "mps", None)
    if mps is not None and mps.is_available():
        return torch.device("mps")
    return torch.device("cpu")
