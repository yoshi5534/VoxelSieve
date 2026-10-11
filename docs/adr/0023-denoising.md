# 0023: Denoising with non-local means or a self-trained model

Status: accepted (2026-10-11)

## Context

Industrial CT scans are noisy, and the noise decides what an analysis can still see: a pore of a
few voxels in a thick casting has a contrast of a few noise sigmas, the threshold of the sieve
cuts through noise at every edge, and the porosity analysis (ADR 0006) finds false pores in noise
and misses true ones. Simple filters fail exactly at the fine structures that matter. A Gaussian
or median filter that removes the noise also blurs a pore of three voxels radius into the material
around it, rounds thin walls and shifts edges.

Two properties of real scans make it harder than the textbook case:

- **Correlated noise.** The reconstruction filter (filtered back projection) correlates the noise
  of neighbouring voxels: in the Me 163 scan neighbours along x correlate by 0.53. A filter that
  assumes independent voxels underestimates how much two neighbours may differ by noise alone.
- **Size.** Scans are larger than RAM (ADR 0004). A filter has to work tile by tile on the
  bricked dataset, and its result must not depend on where the tiles start.

There is no noise-free version of a real scan to learn from or to measure against. The synthetic
scans (ADR 0005) can be made with and without noise, so a benchmark on them can.

## Decision

Two methods behind one interface, `denoiseDataset` (`denoise.hpp`), the operation `denoise` and
the tool `vs-denoise`. Both read level 0 of a dataset tile by tile with a halo and write a new
dataset with the same bricks, active voxels and value type (`writeDerivedDataset` in
`dataset.hpp`); the coarse levels are rebuilt from the result. The input stays as it is. In a
project the denoised dataset becomes the object's latest source, so the analyses after it read it.

### Noise model

`estimateNoise` measures the noise from 8³ blocks of level 0 that are fully active, above the
threshold and whose six neighbour blocks are material too, so no edge falls into them. Sigma is
the square root of the median block variance. For every offset d up to three voxels per axis, the
pair variance (the mean squared difference of voxels d apart) is the median over a hashed sample
of 4000 blocks. White noise gives 2σ² at every offset; correlated noise gives less near the
centre. `vs-denoise` and the operation report sigma and the neighbour correlations.

### Non-local means (default)

Every voxel becomes the weighted mean of the voxels in its search window (7³ by default), weighted
by how alike their surrounding patches (3³) are, not by how close they lie. Edges, thin walls and
small pores average with voxels of the same structure elsewhere and keep their contrast.

- **Distance:** the mean squared patch difference at offset d, divided by the noise pair variance
  at d. Two patches of the same structure give 1 on average, whatever the correlation of the
  noise at that offset. With white-noise normalisation correlated neighbours always looked
  similar, and NLM blurred small pores like a box filter.
- **Weight:** exp(-r / strength), and no weight at all above `similarity_limit` (1.3). Without the
  cutoff, the hundreds of material voxels around a pore of a few voxels outweigh the few that look
  like it, however small each weight, and fill the pore in: the small-pore contrast dropped to
  0.38. A smaller search window, a centre weight of 1 and a second guided pass did not help.
- **Centre weight:** the largest weight of the other voxels, as usual.
- **Exactness:** patch distances are box sums in double precision and therefore exact, so the
  result is the same for every tile size (tested). Tiles of 64³ run in parallel; the weights use
  a vectorisable exp.

### Learned model (Noise2Void)

A denoising model is a `.vsm` (ADR 0014) with `"task": "denoising"`: no materials, one output
channel, read back as grey value through the model's input normalisation. `train_denoiser.py`
trains the U-Net of `unet.py` on the noisy scan alone (Noise2Void, Krull et al. 2019): it hides
blind spots and learns to predict them from their surroundings, which cannot predict
independent noise. Because the noise of a CT scan is correlated, the whole cube of
`--mask-radius` voxels around each blind spot is hidden (structured blind spots), so the network
cannot copy the noise from the neighbours. It runs like a segmentation model, tile by tile on the
CPU.

### Benchmark

`vs-denoise --benchmark <dir>` makes a synthetic scan of the housing (ADR 0005) with 16 lunkers
of three voxels radius and 60 % spread, a loosened zone, unsharpness and correlated noise
(`SyntheticSpec::noise_correlation_mm`, new), sieves it with and without noise, denoises it with
NLM and, with `--model`, with a model, and measures each against the noise-free scan:

- RMSE in all, at edges, in material and in air (`scoreDenoising`);
- the contrast of the lunkers kept, all and the smallest third, and the contrast-to-noise ratio of
  the small ones;
- lunkers found by the porosity analysis, false pores and zones.

It writes `benchmark.json`, `benchmark.md` and `slices.png` (the slice through the smallest
lunker, each variant side by side).

## Measurements

Benchmark scan of `vs-denoise --benchmark` (208 × 160 × 88 voxels of 0.25 mm, air 2000, material
12000, noise correlation 0.7 voxels), 4 cores, 2026-10-11. RMSE in grey values; contrast kept as a
share of the noise-free contrast; CNR of the smallest third of the lunkers.

Noise sigma 2000 (a fifth of the contrast):

| Variant | Seconds | RMSE | Edges | Material | Air | Lunker contrast | Small lunker contrast | Small lunker CNR | Lunkers found | False pores | Zones |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| noise-free | | 0 | 0 | 0 | 0 | 1.00 | 1.00 | | 12 / 16 | 0 | 1 |
| noisy | | 1881 | 1935 | 1999 | 1795 | 1.05 | 1.10 | 2.4 | 14 / 16 | 320 | 31 |
| non-local means | 5.7 | 663 | 941 | 679 | 457 | 0.65 | 0.42 | 2.7 | 8 / 16 | 0 | 19 |
| Noise2Void, 25 min training | 33.7 | 468 | 741 | 452 | 251 | 0.14 | 0.12 | 1.2 | 0 / 16 | 0 | 14 |

- NLM cuts the noise by three and keeps the lunkers that stand out of the noise; the false pores
  go away. A search radius of 4 raises the small-lunker CNR to 3.0 at twice the time.
- The model removes more noise but also the lunkers: they are a tiny share of the training
  patches, so the network learns that material is flat. Its loss reaches the noise floor long
  before it would gain anything from the lunkers. A model is the better choice for structure that
  is everywhere in the scan (fibres, foams, lattices), not for rare defects.

Me 163 (training volume V4, 512³ voxels of 0.33 × 0.33 × 0.6 mm, 17 % active): measured noise
sigma 1010, neighbour correlation 0.53 (x), 0.26 (y), 0.18 (z); after NLM the noise is 301.

## Consequences

- Denoising is a step of its own and writes a new dataset; it doubles the disk space of a scan
  for as long as both are kept.
- NLM needs no training and no parameters for a new scan; it is the default. The model needs a
  training run per scanner and setting, PyTorch, and a GPU for large training sets.
- The porosity analysis finds many false zones in correlated noise, also after denoising (19 to
  31 against one in the noise-free scan). Its zone detection assumes white noise and should take
  the noise model next.
- `ModelSpec` has a `task` now; models without it are segmentation models, as before.
