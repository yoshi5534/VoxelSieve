#!/usr/bin/env python3
"""Trains the small 3D U-Net of unet.py on a labelled scan and writes a VoxelSieve model (.vsm).

Inputs are two headerless volumes of the same size, x fastest: the grey values (uint16) and
the class of every voxel (uint8, 0 = air, 1..n = materials). Slabs given with --holdout are left
out of training (no training patch touches them), so the model can be scored on them afterwards
with vs-segment --region. Needs PyTorch and NumPy; runs on the CPU.

Example (Me 163, see docs/adr/0014-learned-segmentation.md):
  train_unet.py --grey grey.u16 --classes classes.u8 --dims 512 3584 512 \\
      --holdout y:512:1024 --holdout y:2560:3072 \\
      --material "Leichtes Material:3000" --material "Dichtes Material:12762.25" \\
      --minutes 90 --out me163.vsm
"""

import argparse
import math
import os
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unet import UNet, export_vsm  # noqa: E402

COLORS = [[66, 146, 198], [230, 126, 34], [46, 160, 67], [196, 60, 80],
          [142, 99, 190], [214, 190, 40], [23, 170, 170], [140, 110, 80]]


def parse():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--grey", required=True)
    p.add_argument("--classes", required=True)
    p.add_argument("--dims", type=int, nargs=3, required=True, metavar=("X", "Y", "Z"))
    p.add_argument("--holdout", action="append", default=[], metavar="AXIS:START:END")
    p.add_argument("--material", action="append", required=True, metavar="NAME:LOWER",
                   help="Material of class 1, 2, ... and the grey value it starts at")
    p.add_argument("--offset", type=float, default=2000.0, help="Input offset (grey value)")
    p.add_argument("--scale", type=float, default=1.0 / 5000.0, help="Input scale")
    p.add_argument("--patch", type=int, default=64)
    p.add_argument("--batch", type=int, default=4)
    p.add_argument("--minutes", type=float, default=60.0)
    p.add_argument("--channels", type=int, nargs=3, default=[8, 16, 32])
    p.add_argument("--name", default="U-Net")
    p.add_argument("--out", required=True)
    return p.parse_args()


def main():
    a = parse()
    torch.manual_seed(0)
    rng = np.random.default_rng(0)
    X, Y, Z = a.dims
    grey = np.memmap(a.grey, np.uint16, "r", shape=(Z, Y, X))
    classes = np.memmap(a.classes, np.uint8, "r", shape=(Z, Y, X))
    n_classes = len(a.material) + 1
    # Held-out slabs as (array axis, start, end); array axes are z, y, x.
    holdout = []
    for h in a.holdout:
        axis, start, end = h.split(":")
        holdout.append(({"x": 2, "y": 1, "z": 0}[axis], int(start), int(end)))
    P = a.patch

    def outside_holdout(corner):
        return all(corner[ax] + P <= s or corner[ax] >= e for ax, s, e in holdout)

    # Patch centres on labelled voxels of every class (a sample of them, every 4th voxel).
    centres = {c: [] for c in range(1, n_classes)}
    for z in range(0, Z, 4):
        plane = classes[z, :, ::4]
        for c in centres:
            yy, xx = np.nonzero(plane == c)
            keep = rng.random(len(yy)) < 0.05
            centres[c].append(np.stack([np.full(keep.sum(), z), yy[keep], xx[keep] * 4], 1))
    centres = {c: np.concatenate(v) for c, v in centres.items() if sum(map(len, v))}

    def corner_of(centre):
        return [int(np.clip(centre[i] - P // 2, 0, (Z, Y, X)[i] - P)) for i in range(3)]

    def sample():
        while True:
            if rng.random() < 0.75:  # a labelled voxel, classes equally often
                c = list(centres)[rng.integers(len(centres))]
                corner = corner_of(centres[c][rng.integers(len(centres[c]))])
            else:
                corner = [int(rng.integers(0, d - P + 1)) for d in (Z, Y, X)]
            if outside_holdout(corner):
                return corner

    def batch():
        gs, ts = [], []
        for _ in range(a.batch):
            z, y, x = sample()
            g = (grey[z:z + P, y:y + P, x:x + P].astype(np.float32) - a.offset) * a.scale
            t = classes[z:z + P, y:y + P, x:x + P].astype(np.int64)
            for axis in (1, 2):
                if rng.random() < 0.5:
                    g, t = np.flip(g, axis), np.flip(t, axis)
            gs.append(g.copy())
            ts.append(t.copy())
        return torch.from_numpy(np.stack(gs))[:, None], torch.from_numpy(np.stack(ts))

    def dice_loss(logits, t):
        p = logits.softmax(1)
        oh = F.one_hot(t, n_classes).permute(0, 4, 1, 2, 3).float()
        inter = (p * oh).sum((0, 2, 3, 4))
        den = p.sum((0, 2, 3, 4)) + oh.sum((0, 2, 3, 4))
        return 1 - ((2 * inter + 1) / (den + 1))[1:].mean()

    net = UNet(n_classes, a.channels)
    opt = torch.optim.AdamW(net.parameters(), 2e-3, weight_decay=1e-4)
    weight = torch.tensor([1.0] + [2.0] * (n_classes - 1))
    budget = a.minutes * 60
    t0, step = time.time(), 0
    while time.time() - t0 < budget:
        for group in opt.param_groups:
            group["lr"] = 2e-3 * 0.5 * (1 + math.cos(math.pi * (time.time() - t0) / budget))
        g, t = batch()
        out = net(g)
        loss = F.cross_entropy(out, t, weight) + dice_loss(out, t)
        opt.zero_grad()
        loss.backward()
        opt.step()
        step += 1
        if step % 100 == 0:
            print(f"step {step} {time.time() - t0:.0f} s loss {loss.item():.3f}", flush=True)

    materials = []
    for i, m in enumerate(a.material):
        name, lower = m.rsplit(":", 1)
        materials.append({"id": i + 1, "name": name, "color": COLORS[i % len(COLORS)],
                          "lower": float(lower)})
    net.eval()
    export_vsm(a.out, net, a.name, materials, a.offset, a.scale,
               description=f"3D U-Net {a.channels}, {step} steps on {os.path.basename(a.grey)}")
    print("wrote", a.out)


if __name__ == "__main__":
    main()
