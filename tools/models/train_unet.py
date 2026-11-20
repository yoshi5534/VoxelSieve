#!/usr/bin/env python3
"""Trains the small 3D U-Net of unet.py on a labelled scan and writes a VoxelSieve model (.vsm).

Inputs are the grey values and the class of every voxel (0 = air, 1..n = materials) of the same
scan: headerless volumes, x fastest (uint16 grey values, uint8 classes, with --dims), or a
VoxelSieve dataset (.vsieve) and material volume, read through the Python package. Patches are
read as training goes, never the whole scan (volumes.py). Slabs given with --holdout are left
out of training (no training patch touches them), so the model can be scored on them afterwards
with vs-segment --region. Needs PyTorch and NumPy; trains on the GPU when there is one.

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
import volumes  # noqa: E402

COLORS = [[66, 146, 198], [230, 126, 34], [46, 160, 67], [196, 60, 80],
          [142, 99, 190], [214, 190, 40], [23, 170, 170], [140, 110, 80]]


def parse():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--grey", required=True, help="Raw uint16 volume or dataset directory")
    p.add_argument("--classes", required=True, help="Raw uint8 volume or material volume")
    p.add_argument("--dims", type=int, nargs=3, metavar=("X", "Y", "Z"),
                   help="Dimensions of raw volumes")
    p.add_argument("--holdout", action="append", default=[], metavar="AXIS:START:END")
    p.add_argument("--material", action="append", required=True, metavar="NAME:LOWER",
                   help="Material of class 1, 2, ... and the grey value it starts at")
    p.add_argument("--offset", type=float, default=2000.0, help="Input offset (grey value)")
    p.add_argument("--scale", type=float, default=1.0 / 5000.0, help="Input scale")
    p.add_argument("--patch", type=int, default=64)
    p.add_argument("--batch", type=int, default=4)
    p.add_argument("--minutes", type=float, default=60.0)
    p.add_argument("--channels", type=int, nargs=3, default=[8, 16, 32])
    p.add_argument("--device", default="auto", help="auto (GPU when there is one), cpu, cuda, mps")
    p.add_argument("--name", default="U-Net")
    p.add_argument("--out", required=True)
    return p.parse_args()


def main():
    a = parse()
    torch.manual_seed(0)
    rng = np.random.default_rng(0)
    grey = volumes.open_grey(a.grey, a.dims)
    classes = volumes.open_labels(a.classes, a.dims)
    if grey.shape != classes.shape:
        sys.exit(f"The grey values {grey.shape} and classes {classes.shape} differ in size")
    n_classes = len(a.material) + 1
    # Held-out slabs as (array axis, start, end); array axes are z, y, x.
    holdout = []
    for h in a.holdout:
        axis, start, end = h.split(":")
        holdout.append(({"x": 2, "y": 1, "z": 0}[axis], int(start), int(end)))
    P = a.patch

    def outside_holdout(corner):
        return all(corner[ax] + P <= s or corner[ax] >= e for ax, s, e in holdout)

    # Three patches in four centred on a voxel of a class seen so far, the classes equally often.
    sampler = volumes.PatchSampler(grey.shape, P, rng, focus=0.75, regions=grey.regions,
                                   allowed=outside_holdout)

    def batch():
        gs, ts = [], []
        for _ in range(a.batch):
            corner = sampler.corner()
            g = (grey.read(corner, P).astype(np.float32) - a.offset) * a.scale
            t = classes.read(corner, P).astype(np.int64)
            sampler.observe(corner, {c: t == c for c in range(1, n_classes)})
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

    dev = volumes.device(a.device)
    print("training on", dev, flush=True)
    net = UNet(n_classes, a.channels).to(dev)
    opt = torch.optim.AdamW(net.parameters(), 2e-3, weight_decay=1e-4)
    weight = torch.tensor([1.0] + [2.0] * (n_classes - 1), device=dev)
    batches = volumes.prefetch(batch)
    budget = a.minutes * 60
    t0, step = time.time(), 0
    while time.time() - t0 < budget:
        for group in opt.param_groups:
            group["lr"] = 2e-3 * 0.5 * (1 + math.cos(math.pi * (time.time() - t0) / budget))
        g, t = next(batches)
        g, t = g.to(dev, non_blocking=True), t.to(dev, non_blocking=True)
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
    net = net.cpu().eval()
    export_vsm(a.out, net, a.name, materials, a.offset, a.scale,
               description=f"3D U-Net {a.channels}, {step} steps on {grey.name}")
    print("wrote", a.out)


if __name__ == "__main__":
    main()
