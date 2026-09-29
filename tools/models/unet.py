"""Small 3D U-Net for material segmentation and its export to a VoxelSieve model (.vsm).

The network matches the layer types VoxelSieve runs itself (model.hpp, ADR 0014): 3x3x3
convolutions with ReLU, 2x2x2 max pooling, nearest-neighbour upsampling and concatenation.
Tensors are (batch, channel, z, y, x), like the volumes VoxelSieve stores x fastest.
"""

import json
import struct

import torch
import torch.nn as nn
import torch.nn.functional as F


def _block(i, o):
    return nn.Sequential(nn.Conv3d(i, o, 3, padding=1), nn.ReLU(),
                         nn.Conv3d(o, o, 3, padding=1), nn.ReLU())


class UNet(nn.Module):
    """Two pooling steps; channels[0..2] per resolution; `classes` scores (0 = air)."""

    def __init__(self, classes=3, channels=(8, 16, 32)):
        super().__init__()
        c = channels
        self.channels, self.classes = tuple(c), classes
        self.e1, self.e2, self.b = _block(1, c[0]), _block(c[0], c[1]), _block(c[1], c[2])
        self.d2, self.d1 = _block(c[2] + c[1], c[1]), _block(c[1] + c[0], c[0])
        self.head = nn.Conv3d(c[0], classes, 1)

    def forward(self, x):
        a = self.e1(x)
        b = self.e2(F.max_pool3d(a, 2))
        c = self.b(F.max_pool3d(b, 2))
        b = self.d2(torch.cat([F.interpolate(c, scale_factor=2, mode="nearest"), b], 1))
        a = self.d1(torch.cat([F.interpolate(b, scale_factor=2, mode="nearest"), a], 1))
        return self.head(a)

    def layers(self):
        """The layer graph in VoxelSieve terms: (description, conv module or None)."""
        def block(prefix, source, seq):
            return [({"op": "conv", "name": f"{prefix}a", "inputs": [source], "relu": True}, seq[0]),
                    ({"op": "conv", "name": prefix, "inputs": [f"{prefix}a"], "relu": True}, seq[2])]
        out = block("e1", "input", self.e1)
        out += [({"op": "maxpool", "name": "p1", "inputs": ["e1"]}, None)]
        out += block("e2", "p1", self.e2)
        out += [({"op": "maxpool", "name": "p2", "inputs": ["e2"]}, None)]
        out += block("b", "p2", self.b)
        out += [({"op": "upsample", "name": "u2", "inputs": ["b"]}, None),
                ({"op": "concat", "name": "c2", "inputs": ["u2", "e2"]}, None)]
        out += block("d2", "c2", self.d2)
        out += [({"op": "upsample", "name": "u1", "inputs": ["d2"]}, None),
                ({"op": "concat", "name": "c1", "inputs": ["u1", "e1"]}, None)]
        out += block("d1", "c1", self.d1)
        out += [({"op": "conv", "name": "scores", "inputs": ["d1"], "relu": False}, self.head)]
        return out


def export_vsm(path, net, name, materials, input_offset, input_scale, halo=16, description=""):
    """Writes `net` as a .vsm file. `materials`: [{"id", "name", "color", "lower"}], id 1, 2, ..."""
    layers, blobs = [], []
    for entry, conv in net.layers():
        entry = dict(entry)
        if conv is not None:
            w = conv.weight.detach().float().contiguous()
            entry.update({"in": w.shape[1], "out": w.shape[0], "kernel": w.shape[2]})
            blobs += [w.flatten(), conv.bias.detach().float()]
        layers.append(entry)
    header = json.dumps({
        "format": "voxelsieve-model", "version": 1, "name": name, "description": description,
        "input": {"offset": input_offset, "scale": input_scale}, "divisor": 4, "halo": halo,
        "materials": materials, "layers": layers}).encode()
    with open(path, "wb") as f:
        f.write(b"VSMODEL1")
        f.write(struct.pack("<Q", len(header)))
        f.write(header)
        for blob in blobs:
            f.write(blob.numpy().astype("<f4").tobytes())
