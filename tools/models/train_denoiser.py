#!/usr/bin/env python3
"""Trains the 3D U-Net of unet.py to denoise a scan from the noisy scan alone (blind spots).

No noise-free data is needed (Noise2Void, Krull et al. 2019): the network sees the scan with a
few voxels hidden and learns to predict each hidden voxel from its surroundings. Noise that is
independent from voxel to voxel cannot be predicted from the surroundings, so the network learns
the structure and leaves the noise out. The reconstruction filter of a CT scan correlates the
noise of neighbouring voxels, though, and a network that saw the neighbours would learn to copy
their noise; so not only the voxel but the cube of --mask-radius voxels around it is hidden,
replaced by voxels from random places of the same patch (structured blind spots). Choose the
radius from the noise correlation vs-denoise reports: 1 when the neighbours' correlation is
clearly below 0.2 at a distance of 2 voxels, else 2. The network cannot see what the cube hides:
structures no wider than the cube, such as pores of three voxels, are smoothed away (ADR 0025);
for those, non-local means keeps more.

The input is a headerless uint16 volume, x fastest (with --dims), such as vs-synth writes or a
staged scan, or a VoxelSieve dataset (.vsieve), read through the Python package. Patches are
read as training goes, never the whole scan (volumes.py). Writes a VoxelSieve model (.vsm) with
the task "denoising" for vs-denoise --model and the GUI operation "denoise". Needs PyTorch and
NumPy; trains on the GPU when there is one.

Example (the benchmark scan of vs-denoise, see docs/adr/0025-denoising.md):
  vs-denoise --benchmark bench
  train_denoiser.py --grey bench/scan.raw --dims 216 176 112 --minutes 30 --out bench.vsm
  vs-denoise --benchmark bench --model bench.vsm
"""

import argparse
import math
import os
import sys
import time

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unet import UNet, export_vsm  # noqa: E402
import volumes  # noqa: E402


def parse():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--grey", required=True,
                   help="Noisy volume: headerless uint16, x fastest, or a dataset directory")
    p.add_argument("--dims", type=int, nargs=3, metavar=("X", "Y", "Z"),
                   help="Dimensions of a raw volume")
    p.add_argument("--mask-radius", type=int, default=1,
                   help="Half edge of the hidden cube around each blind spot (default 1)")
    p.add_argument("--mask-fraction", type=float, default=0.005,
                   help="Share of the voxels of a patch that are blind spots (default 0.005)")
    p.add_argument("--material", type=float, default=None,
                   help="Grey value above which a voxel is material (default: Otsu)")
    p.add_argument("--patch", type=int, default=32)
    p.add_argument("--batch", type=int, default=8)
    p.add_argument("--minutes", type=float, default=30.0)
    p.add_argument("--channels", type=int, nargs=3, default=[16, 32, 64])
    p.add_argument("--threads", type=int, default=0, help="PyTorch threads (default: all)")
    p.add_argument("--device", default="auto", help="auto (GPU when there is one), cpu, cuda, mps")
    p.add_argument("--name", default="Noise2Void U-Net")
    p.add_argument("--out", required=True)
    return p.parse_args()


def otsu(values):
    hist, edges = np.histogram(values, bins=1024)
    centres = (edges[:-1] + edges[1:]) / 2
    w0 = np.cumsum(hist)
    w1 = w0[-1] - w0
    m0 = np.cumsum(hist * centres)
    mean0 = m0 / np.maximum(w0, 1)
    mean1 = (m0[-1] - m0) / np.maximum(w1, 1)
    between = w0 * w1 * (mean0 - mean1) ** 2
    return float(centres[np.argmax(between)])


def main():
    a = parse()
    if a.threads:
        torch.set_num_threads(a.threads)
    torch.manual_seed(0)
    rng = np.random.default_rng(0)
    grey = volumes.open_grey(a.grey, a.dims)
    P = a.patch

    # Normalisation from a sample of patches: air level to 0, material level to 1.
    sample = volumes.sample_values(grey, rng, regions=grey.regions)
    split = a.material if a.material is not None else otsu(sample)
    air = float(np.median(sample[sample < split]))
    material = float(np.median(sample[sample >= split]))
    offset, scale = air, 1.0 / max(material - air, 1.0)
    print(f"air {air:.0f}, material {material:.0f} (split {split:.0f})", flush=True)

    # Patches are centred on material most of the time: that is where the structure is.
    sampler = volumes.PatchSampler(grey.shape, P, rng, focus=0.8, regions=grey.regions)

    def batch():
        out = []
        for _ in range(a.batch):
            corner = sampler.corner()
            values = grey.read(corner, P)
            sampler.observe(corner, {"material": values >= split})
            g = (values.astype(np.float32) - offset) * scale
            for axis in (0, 1, 2):
                if rng.random() < 0.5:
                    g = np.flip(g, axis)
            out.append(g.copy())
        return torch.from_numpy(np.stack(out))[:, None]

    r = a.mask_radius
    cube = torch.stack(torch.meshgrid(*[torch.arange(-r, r + 1)] * 3, indexing="ij"), -1)
    cube = cube.reshape(-1, 3)
    spots = max(1, int(a.mask_fraction * P ** 3))

    def mask(clean):
        """The batch with blind spots hidden, and the flat indices of the blind spots."""
        b = clean.shape[0]
        flat = clean.reshape(b, -1)
        hidden = flat.clone()
        centre = torch.randint(0, P, (b, spots, 3))
        cells = (centre[:, :, None, :] + cube[None, None]).clamp(0, P - 1).reshape(b, -1, 3)
        index = (cells[..., 0] * P + cells[..., 1]) * P + cells[..., 2]
        source = torch.randint(0, P ** 3, index.shape)
        hidden.scatter_(1, index, torch.gather(flat, 1, source))
        spot = (centre[..., 0] * P + centre[..., 1]) * P + centre[..., 2]
        return hidden.reshape(clean.shape), spot

    dev = volumes.device(a.device)
    print("training on", dev, flush=True)
    net = UNet(1, a.channels).to(dev)
    opt = torch.optim.AdamW(net.parameters(), 1e-3, weight_decay=1e-5)
    batches = volumes.prefetch(batch)
    budget = a.minutes * 60
    t0, step, smooth = time.time(), 0, None
    while time.time() - t0 < budget:
        for group in opt.param_groups:
            group["lr"] = 1e-3 * 0.5 * (1 + math.cos(math.pi * (time.time() - t0) / budget))
        clean = next(batches)
        hidden, spot = mask(clean)
        clean, hidden, spot = clean.to(dev), hidden.to(dev), spot.to(dev)
        out = net(hidden).reshape(clean.shape[0], -1)
        target = clean.reshape(clean.shape[0], -1)
        loss = ((torch.gather(out, 1, spot) - torch.gather(target, 1, spot)) ** 2).mean()
        opt.zero_grad()
        loss.backward()
        opt.step()
        step += 1
        smooth = loss.item() if smooth is None else 0.98 * smooth + 0.02 * loss.item()
        if step % 100 == 0:
            print(f"step {step} {time.time() - t0:.0f} s loss {smooth:.4f}", flush=True)

    net = net.cpu().eval()
    export_vsm(a.out, net, a.name, [], offset, scale, task="denoising",
               description=f"Noise2Void U-Net {a.channels}, mask radius {r}, {step} steps on "
                           f"{grey.name}")
    print("wrote", a.out)


if __name__ == "__main__":
    main()
