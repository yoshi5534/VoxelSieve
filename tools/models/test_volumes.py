"""Tests of the streamed patches of the training scripts (volumes.py); need NumPy only."""

import os
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import volumes  # noqa: E402


class VolumesTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        # 40 x 30 x 20 voxels (x, y, z), grey value 100 * x + y; a small bright block of class 1.
        z, y, x = np.mgrid[0:20, 0:30, 0:40]
        grey = (100 * x + y).astype(np.uint16)
        labels = np.zeros((20, 30, 40), np.uint8)
        labels[14:17, 3:6, 30:33] = 1
        self.grey_path = os.path.join(self.dir.name, "grey.u16")
        self.labels_path = os.path.join(self.dir.name, "labels.u8")
        grey.tofile(self.grey_path)
        labels.tofile(self.labels_path)

    def tearDown(self):
        self.dir.cleanup()

    def test_raw_patches_are_read_at_their_corner(self):
        grey = volumes.open_grey(self.grey_path, [40, 30, 20])
        self.assertEqual(grey.shape, (20, 30, 40))
        patch = grey.read([2, 3, 4], 8)
        self.assertEqual(patch.shape, (8, 8, 8))
        self.assertEqual(patch[0, 0, 0], 100 * 4 + 3)
        self.assertEqual(patch[1, 2, 3], 100 * 7 + 5)
        with self.assertRaises(SystemExit):
            volumes.open_grey(self.grey_path)  # a raw volume needs its dims

    def test_patches_lie_inside_and_outside_held_out_slabs(self):
        rng = np.random.default_rng(1)
        held = lambda corner: corner[2] + 8 <= 10 or corner[2] >= 20  # noqa: E731
        sampler = volumes.PatchSampler((20, 30, 40), 8, rng, allowed=held)
        for _ in range(500):
            corner = sampler.corner()
            self.assertTrue(all(0 <= corner[i] <= (20, 30, 40)[i] - 8 for i in range(3)))
            self.assertTrue(held(corner))

    def test_a_class_once_seen_is_found_again(self):
        rng = np.random.default_rng(2)
        labels = volumes.open_labels(self.labels_path, [40, 30, 20])
        sampler = volumes.PatchSampler(labels.shape, 8, rng, focus=0.75)
        hits = []
        for _ in range(400):
            corner = sampler.corner()
            patch = labels.read(corner, 8)
            sampler.observe(corner, {1: patch == 1})
            hits.append(bool((patch == 1).any()))
        # 27 voxels of 24000: by chance a patch of 8^3 holds one in about 4 of 100; once found,
        # three in four patches are centred on one.
        self.assertGreater(np.mean(hits[200:]), 0.6)
        stored = sampler.positions[1]
        self.assertTrue(np.all(labels.array[tuple(stored.T)] == 1))

    def test_the_reservoir_stays_within_its_capacity(self):
        rng = np.random.default_rng(3)
        sampler = volumes.PatchSampler((20, 30, 40), 8, rng, reservoir=50)
        for _ in range(100):
            sampler.observe([0, 0, 0], {"material": np.ones((8, 8, 8), bool)})
        self.assertEqual(len(sampler.positions["material"]), 50)
        self.assertEqual(sampler.seen["material"], 100 * sampler.per_patch)

    def test_patches_come_from_the_regions_given(self):
        rng = np.random.default_rng(4)
        regions = [((0, 0, 32), (8, 8, 40))]
        sampler = volumes.PatchSampler((20, 30, 40), 8, rng, focus=0.0, regions=regions)
        for _ in range(100):
            z, y, x = sampler.corner()
            # Centred on a voxel of the region, clipped to the volume.
            self.assertTrue(28 <= x <= 32)
            self.assertLessEqual(z, 3)

    def test_values_are_sampled_from_patches(self):
        grey = volumes.open_grey(self.grey_path, [40, 30, 20])
        values = volumes.sample_values(grey, np.random.default_rng(5), count=10, size=8)
        self.assertEqual(len(values), 10 * 8 ** 3)
        self.assertTrue(np.all(values <= 100 * 39 + 29))

    def test_prefetch_makes_ahead_and_passes_errors_on(self):
        count = iter(range(3))

        def make():
            value = next(count, None)
            if value is None:
                raise ValueError("no more")
            return value

        made = volumes.prefetch(make)
        self.assertEqual([next(made) for _ in range(3)], [0, 1, 2])
        with self.assertRaises(ValueError):
            next(made)


if __name__ == "__main__":
    unittest.main()
