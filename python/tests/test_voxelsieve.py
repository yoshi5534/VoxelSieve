"""Tests of the Python package against the synthetic phantom, whose geometry is known analytically.

Run from a build with VOXELSIEVE_BUILD_PYTHON=ON (ctest -R python_package), or after
`pip install .` with `python -m unittest discover -s python/tests`.
"""

import math
import tempfile
import unittest
from pathlib import Path

import numpy as np

import voxelsieve as vs

# The default phantom: a hollow box of 8 mm with 1.5 mm walls in a 128^3 volume of 0.1 mm voxels,
# and three pores of radius 0.4, 0.3 and 0.2 mm in its walls.
PORES = [((3.25, 0.0, 0.0), 0.40), ((-3.25, 1.5, -1.0), 0.30), ((0.5, -0.5, 3.25), 0.20)]
DIMS = (128, 128, 128)
VOXEL = 0.1


def pore_voxel(centre_mm, dims=DIMS, voxel=VOXEL):
    """(z, y, x) index of the voxel at a position given in mm from the volume centre."""
    return tuple(int(round(centre_mm[a] / voxel + (dims[a] - 1) / 2)) for a in (2, 1, 0))


class TempDir(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()


class PhantomTest(unittest.TestCase):
    def test_phantom_is_a_numpy_volume_in_zyx_order(self):
        volume, material_mm3 = vs.phantom((64, 48, 32), 0.25, outer_size_mm=(8, 6, 4),
                                          wall_thickness_mm=1.0, pores=[])
        self.assertEqual(volume.shape, (32, 48, 64))
        self.assertEqual(volume.dtype, np.uint16)
        self.assertAlmostEqual(material_mm3, 8 * 6 * 4 - 6 * 4 * 2, places=6)
        # Air in the corners, material in the middle of a wall (x = +3.5 mm).
        self.assertEqual(volume[0, 0, 0], 1000)
        self.assertEqual(volume[16, 24, int(round(3.5 / 0.25 + 31.5))], 20000)


class DatasetTest(TempDir):
    def setUp(self):
        super().setUp()
        self.volume, self.material_mm3 = vs.phantom(DIMS, VOXEL, pores=PORES)
        self.dataset = vs.write_dataset(self.volume, self.tmp / "phantom.vsieve", VOXEL,
                                        brick_size=64)

    def test_metadata(self):
        ds = self.dataset
        self.assertEqual(ds.dims, DIMS)
        self.assertEqual(ds.shape, DIMS[::-1])
        self.assertEqual(ds.value_type, "uint16")
        self.assertEqual(ds.dtype, np.uint16)
        self.assertEqual(ds.voxel_size_mm, (VOXEL, VOXEL, VOXEL))
        self.assertGreater(len(ds.levels), 1)
        self.assertEqual(ds.level(1).shape, (64, 64, 64))
        self.assertTrue(1000 < ds.threshold < 20000)

    def test_part_and_internal_voids_are_kept_losslessly(self):
        ds = self.dataset
        values = ds[:, :, :]
        active = ds.active((0, 0, 0), DIMS)
        self.assertEqual(values.shape, self.volume.shape)
        # Every kept voxel has exactly the grey value of the input (ADR 0021).
        np.testing.assert_array_equal(values[active], self.volume[active])
        # Every material voxel is kept ...
        self.assertTrue(active[self.volume >= ds.threshold].all())
        # ... and so are the pores and the closed cavity, which are air inside the part.
        for centre, _ in PORES:
            self.assertTrue(active[pore_voxel(centre)], centre)
        self.assertTrue(active[64, 64, 64])
        # The outside air is gone, apart from the margin around the part.
        self.assertFalse(active[0, 0, 0])
        self.assertEqual(values[0, 0, 0], 0)

    def test_slicing_follows_numpy(self):
        ds = self.dataset
        np.testing.assert_array_equal(ds[64], ds[64:65, :, :][0])
        np.testing.assert_array_equal(ds[64, 10:20, -5], ds[64:65, 10:20, 123:124][0, :, 0])
        np.testing.assert_array_equal(ds[60:70:3, ..., 64], ds[60:70, :, 64][::3])
        self.assertEqual(ds[64, 64, 64].shape, ())
        with self.assertRaises(IndexError):
            ds[128]
        with self.assertRaises(IndexError):
            ds.read((0, 0, 0), (129, 1, 1))

    def test_fill_and_dtype(self):
        corner = self.dataset.read((0, 0, 0), (4, 4, 4), fill=np.nan)
        self.assertEqual(corner.dtype, np.float32)
        self.assertTrue(np.isnan(corner).all())
        corner = self.dataset.read((0, 0, 0), (4, 4, 4), fill=7)
        self.assertEqual(corner.dtype, np.uint16)
        self.assertTrue((corner == 7).all())

    def test_coarser_level_is_the_mean_of_its_children(self):
        ds = self.dataset
        fine = ds[60:64, 60:64, 0:128].astype(np.float64)
        coarse = ds.level(1)[30:32, 30:32, 0:64]
        # Inside the cavity and walls all eight children are active.
        expected = fine.reshape(2, 2, 2, 2, 64, 2).mean(axis=(1, 3, 5))
        np.testing.assert_allclose(coarse[:, :, 30:34], np.rint(expected[:, :, 30:34]), atol=0.5)

    def test_iterating_bricks_reassembles_the_volume(self):
        ds = self.dataset
        bricks = ds.bricks()
        self.assertTrue(0 < len(bricks) <= 8)
        assembled = np.zeros(ds.shape, dtype=ds.dtype)
        for brick, slices, values in ds.iter_bricks():
            self.assertIn(brick, bricks)
            assembled[slices] = values
        np.testing.assert_array_equal(assembled, ds[:, :, :])

    def test_reopen_and_cache(self):
        again = vs.Dataset.open(self.tmp / "phantom.vsieve", cache_bytes=1 << 20, on_access=True)
        np.testing.assert_array_equal(again[64], self.dataset[64])
        self.assertGreater(again.cache_stats()["misses"], 0)
        self.assertIn("phantom.vsieve", repr(again))

    def test_memmap_input(self):
        raw = self.tmp / "phantom.raw"
        self.volume.tofile(raw)
        mapped = np.memmap(raw, dtype="<u2", mode="r", shape=self.volume.shape)
        ds = vs.write_dataset(mapped, self.tmp / "mapped.vsieve", VOXEL)
        np.testing.assert_array_equal(ds[64], self.dataset[64])

    def test_uint8_and_float_input(self):
        small = (self.volume // 100).astype(np.uint8)
        ds8 = vs.write_dataset(small, self.tmp / "u8.vsieve", VOXEL)
        self.assertEqual(ds8.dtype, np.uint8)
        active = ds8.active((0, 0, 0), DIMS)
        np.testing.assert_array_equal(ds8[:, :, :][active], small[active])

        attenuation = self.volume.astype(np.float32) * 1e-4 - 0.05
        dsf = vs.write_dataset(attenuation, self.tmp / "float.vsieve", VOXEL)
        self.assertEqual(dsf.value_type, "uint16")
        offset, scale = dsf.value_mapping
        self.assertAlmostEqual(offset, attenuation.min(), places=6)
        values = dsf.to_values(dsf[64])
        kept = dsf.active((0, 0, 64), (128, 128, 65))[0]
        np.testing.assert_allclose(values[kept], attenuation[64][kept], atol=scale)

    def test_anisotropic_voxels(self):
        ds = vs.write_dataset(self.volume, self.tmp / "aniso.vsieve", (0.1, 0.1, 0.25),
                              slice_thickness_mm=0.2)
        self.assertEqual(ds.voxel_size_mm, (0.1, 0.1, 0.25))
        self.assertEqual(ds.info["voxel_size"]["slice_thickness_mm"], 0.2)

    def test_derived_dataset_keeps_the_part(self):
        calls = []

        def invert(values):
            calls.append(values.shape)
            return 65535 - values

        derived = vs.write_derived_dataset(self.dataset, self.tmp / "inverted.vsieve", invert,
                                           halo=2)
        self.assertEqual(len(calls), len(self.dataset.bricks()))
        self.assertTrue(all(shape[0] <= 64 + 4 for shape in calls))
        active = self.dataset.active((0, 0, 0), DIMS)
        np.testing.assert_array_equal(derived.active((0, 0, 0), DIMS), active)
        np.testing.assert_array_equal(derived[:, :, :][active], 65535 - self.volume[active])

    def test_derived_dataset_reports_errors_of_the_function(self):
        def broken(values):
            raise RuntimeError("broken filter")

        with self.assertRaisesRegex(RuntimeError, "broken filter"):
            vs.write_derived_dataset(self.dataset, self.tmp / "broken.vsieve", broken)
        with self.assertRaises(ValueError):
            vs.write_derived_dataset(self.dataset, self.tmp / "wrong.vsieve",
                                     lambda v: v[1:])


class ProjectTest(TempDir):
    def test_operations_are_listed_with_schemas(self):
        ops = {op["id"]: op for op in vs.operations()}
        for name in ("import_raw", "porosity", "segment_materials", "surface", "wall_thickness"):
            self.assertIn(name, ops)
        self.assertEqual(ops["porosity"]["parameters"]["type"], "object")
        self.assertIn("min_pore_voxels", ops["porosity"]["parameters"]["properties"])

    def test_porosity_finds_the_phantom_pores(self):
        # A solid box, so that the closed cavity of the default phantom is no pore.
        volume, material_mm3 = vs.phantom(DIMS, VOXEL, pores=PORES, wall_thickness_mm=0)
        dataset = vs.write_dataset(volume, self.tmp / "phantom.vsieve", VOXEL)
        project = vs.Project.create(self.tmp / "project", "Phantom")
        opened = project.run("open_dataset", path=dataset.path)
        self.assertEqual(opened.status, "done")
        self.assertEqual(len(project.objects), 1)

        pores = project.run("porosity", min_pore_voxels=3)
        self.assertEqual(pores.status, "done")
        self.assertEqual(pores.inputs["dataset"]["step"], opened.id)
        self.assertEqual(pores.summary["pores"], len(PORES))
        expected_mm3 = sum(4 / 3 * math.pi * r**3 for _, r in PORES)
        self.assertAlmostEqual(pores.summary["pore_volume_mm3"], expected_mm3,
                               delta=0.15 * expected_mm3)
        self.assertAlmostEqual(pores.summary["part_volume_mm3"], material_mm3 + expected_mm3,
                               delta=0.03 * material_mm3)
        self.assertTrue((pores.path() / "porosity.json").exists())
        self.assertEqual([r["type"] for r in project.results()], ["porosity"])

        # Undo, reopen: the protocol is the project's, as in the GUI.
        self.assertTrue(project.undo())
        reopened = vs.Project.open(self.tmp / "project")
        self.assertEqual([s.operation for s in reopened.steps], ["open_dataset", "porosity"])
        self.assertEqual(reopened.results(), [])

    def test_import_raw_and_segment_materials(self):
        volume, _ = vs.phantom((96, 96, 96), 0.1, pores=[], outer_size_mm=(6, 6, 6),
                               wall_thickness_mm=1.2)
        raw = self.tmp / "box.raw"
        volume.tofile(raw)
        project = vs.Project.create(self.tmp / "project")
        progress = []
        imported = project.run("import_raw", path=raw, dims=[96, 96, 96], voxel_size_mm=0.1,
                               progress=progress.append, name="Box")
        self.assertEqual(project.objects[0]["name"], "Box")
        self.assertTrue(progress)
        np.testing.assert_array_equal(project.dataset()[48], imported.dataset()[48])

        segmented = project.run("segment_materials", materials=1)
        labels = segmented.materials()
        self.assertEqual(len(labels.materials), 1)
        self.assertEqual(labels.shape, (96, 96, 96))
        cut = labels[48]
        self.assertEqual(cut.dtype, np.uint8)
        self.assertEqual(cut[48, 48], 0)      # the cavity is air
        self.assertEqual(cut[48, 48 + 25], 1)  # the middle of a wall is material
        material_mm3 = labels.materials[0]["volume_mm3"]
        self.assertAlmostEqual(material_mm3, 6**3 - 3.6**3, delta=0.05 * (6**3 - 3.6**3))

    def test_failing_operation_raises_and_stays_in_the_protocol(self):
        project = vs.Project.create(self.tmp / "project")
        # A path that does not exist is refused before a step starts.
        with self.assertRaises(Exception):
            project.run("open_dataset", path=self.tmp / "missing.vsieve")
        self.assertEqual(len(project.steps), 0)
        # A directory that is no dataset fails in the step, which stays in the protocol.
        (self.tmp / "empty.vsieve").mkdir()
        with self.assertRaises(Exception):
            project.run("open_dataset", path=self.tmp / "empty.vsieve")
        self.assertEqual(project.steps[-1].status, "failed")
        with self.assertRaises(Exception):
            project.run("no_such_operation")


if __name__ == "__main__":
    unittest.main()
