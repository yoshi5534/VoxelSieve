# 0015: Float input mapped onto 16-bit grey values

Status: accepted (2026-09-29)

## Context

Reconstruction software often writes float slices instead of integers: attenuation coefficients
in 1/mm or 1/pixel, with negative values from noise and reconstruction artefacts in the air. The
first such dataset a user brought is a ZIP archive of 32-bit float TIFF slices of 1250 x 1250
(a drill core in a tube), with values from -0.027 to 0.247 and a noise level of about 0.007.

Until now `TiffStackSource` refused float images (ADR 0011), because every `VolumeSource` delivers
16-bit grey values and ADR 0002 forbids silent quantisation. The streaming sieve, the Otsu
threshold, the material segmentation and the histograms all work on the 65536 grey levels of
16-bit data. Carrying float through all of them (a second `readRegion`, float histograms with
chosen bins, float block maxima) touches every algorithm for one input format.

## Decision

Float slices are mapped linearly onto grey values 0 to 65535 while they are read, and the mapping
is kept wherever the data goes:

- `ValueMapping` (`source.hpp`): `value = offset + scale * grey`. `VolumeSource::valueMapping()`
  is the identity for integer sources; `ConcatSource` requires all parts to map alike.
- The value range (the values that become grey 0 and 65535) is given (`--value-range`,
  `value_range`) or estimated from up to 9 slices spread over the stack, first and last included,
  widened by 5 % of the range on each side. Values outside the range are clipped and not-a-number
  becomes 0; each clipped value is counted once and reported, so that nothing is lost silently.
- `writeDataset` records the mapping in `index.json` as `value_mapping` (`offset`, `scale`), only
  when it is not the identity, so integer datasets keep their format. A single `.vdb` from
  `vs-sieve` carries `value_offset` and `value_scale` as grid metadata.
- 32 and 64-bit float TIFFs are read, uncompressed or compressed as before, with the
  floating-point predictor (3) as tifffile and libtiff write it.

The grids hold the grey values, as for every other input; `value_mapping` converts them back.

## Consequences

- The quantisation step is the range divided by 65535: 4.6e-6 for the example, about 1/1500 of
  its noise. The largest error after mapping back is half a step, checked against tifffile.
- The quantisation is explicit: the range is printed and logged, the mapping is stored, clipping is
  counted. A too narrow given range shows up as clipped values; an estimated range can miss
  extremes in slices that were not sampled, which the clip count reports after the run.
- All algorithms work unchanged. Thresholds, pore statistics and histograms are in grey values;
  showing them in the units of the scan (the slice view, the report) is a later step on top of
  `value_mapping`.
- Float output (float grids with the original values) remains possible later, e.g. when a
  dataset should hold values of a physical unit directly.
