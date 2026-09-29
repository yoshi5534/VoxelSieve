# 0014: Learned segmentation models

Status: accepted (2026-09-29)

## Context

The threshold segmentation (ADR 0013) decides every voxel from grey values around it. On the
Me 163 scan it leaves a rim of one voxel around thin sheets (the labels are tighter than any
threshold) and misses weld and rivet lines. A network trained on the labels learns these
boundaries. It has to run on datasets larger than RAM, in the studio and through MCP like every
other operation, and the core should stay free of new dependencies (CLAUDE.md). ONNX Runtime is
not packaged for Ubuntu 24.04 and would be a large dependency for a few convolutions.

## Decision

- **VoxelSieve runs the models itself** (`include/voxelsieve/model.hpp`). A model is a small
  graph of 3D layers: convolution (stride 1, zero padding to the same size, optional ReLU),
  2×2×2 max pooling, nearest-neighbour upsampling and concatenation. That covers U-Nets, the
  usual architecture for volume segmentation. The convolution computes four output channels per
  pass over the input rows and runs in parallel over planes; results match PyTorch.
- **File format `.vsm`**: the bytes `VSMODEL1`, the length of a JSON header (uint64, little
  endian), the header, then the weights and biases of the conv layers in layer order as float32.
  The header holds the layers, the input normalisation `(grey - offset) * scale`, the tile
  divisor (2^pooling steps), the halo in voxels and the materials: output channel 0 is air,
  channel k the material k with name, colour and the grey value `lower` from which a labelled
  component counted as that material in training (used for scoring, ADR 0013).
- **Tiles with a halo.** `segmentMaterialsWithModel` walks the dataset's stored bricks in tiles
  (default 128³) and gives each tile the model's halo of context, read through `Dataset`, so
  memory depends on the tile, not the volume (ADR 0004). Tiles in which no voxel reaches the
  first material's `lower` are air without running the network. The output is the material
  volume of ADR 0013, with the model's name in `materials.json`, so the slice view, volumes and
  scoring work unchanged.
- **Training outside the core.** `tools/models/unet.py` defines the U-Net and the export,
  `tools/models/train_unet.py` trains it on a grey volume and a class volume with PyTorch on the
  CPU and leaves slabs out (`--holdout`) for honest scoring. Neither is needed to build or run
  VoxelSieve.
- **Tools:** `vs-segment --model <file.vsm>` and the studio operation `segment_model`
  ("Gelernte Segmentierung"); `vs-segment --region` and `scoreMaterials(..., region)` score only
  a box, such as the held-out part of a scan.

## Consequences

- Me 163 (512 × 3584 × 512, sieved with threshold 2500 and `--min-material 8`): a U-Net with
  8/16/32 channels (89 000 parameters, 356 kB) trained 90 minutes on 4 CPU cores on V1, V3, V4,
  V5 and V7, with classes from the target components (median grey value, split at 12762).
  Scored on V2 and V6, which it never saw (y 544–992 and 2592–3040: training patches reach at most
  32 voxels into V2 and V6, so that band is left out):

  | Held out | Threshold (ADR 0013) | U-Net |
  | --- | --- | --- |
  | V2 material/air | 0.861 | 0.898 |
  | V6 material/air | 0.843 | 0.922 |
  | V6 light | 0.766 | 0.907 |
  | V6 dense | 0.960 | 0.905 |

  The rim around the sheets is gone. The dense class got worse on V6: the network marks some
  air and light voxels next to dense parts as dense (0.38 and 0.10 million voxels). Over the
  whole scan, training data included, Dice is 0.928 material/air, 0.920 light, 0.930 dense.
- Running the model over the whole scan takes 18 minutes on 4 cores (the threshold
  segmentation 30 s); the full Me 163 (6144 × 9600 × 5186) would take days on such a machine.
  Faster kernels (SIMD beyond SSE2, a GPU backend) or a smaller network are the next step when
  this matters.
- Only the listed layer types load; other architectures need new layer types in `model.hpp`.
  An ONNX importer can convert to `.vsm` later without changing the runtime.
