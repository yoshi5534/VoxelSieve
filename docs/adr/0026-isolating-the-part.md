# 0026: Isolating the part: region and largest objects

Status: accepted (2026-10-15)

## Context

The sieve (ADR 0003, 0004) removes the outside air and keeps everything else. A scan often shows
more than the part: the holder or fixture it stands in, a support, a tube, loose debris, a second
part. Everything after the sieve then counts these too. The material segmentation (ADR 0013)
gives a foam or plastic holder its own class or puts it into the lightest class of the part,
which distorts that class and the volumes; the porosity analysis and the surface see the holder
as part of the object. There was no way to say "only this", neither a crop nor a step that keeps
the largest object.

Scans are larger than RAM (ADR 0004), so labelling connected objects voxel by voxel over the whole
volume at once is out of reach for the largest ones.

## Decision

An operation `isolate_part` (`isolatePart` in `isolate.hpp`) writes a new dataset of the same
object with what is not the part removed like outside air; the input stays in the project and
later steps take the new dataset, as after denoising (ADR 0025).

- **Region.** A box of level-0 voxels (`region_min`, `region_max`); voxels outside it are removed.
  The dimensions and coordinates of the dataset stay, so poses and other objects are unaffected.
- **Objects.** Within the region, objects are found on a grid of cells of 2^k level-0 voxels per
  axis, the finest grid with at most 2^27 cells (one cell per voxel up to about 500^3 voxels).
  The material voxels per cell are counted in one pass over the bricks; a cell is material when at
  least 2^k of its voxels are above the threshold, so a sheet one voxel thin crossing a cell still
  counts and lone noise voxels do not join objects. Material cells connected across faces are an
  object, its size the material voxels it holds. The largest `keep_objects` objects (default 1)
  are kept, grown by the dataset's margin of air plus one voxel (in whole cells) so that the voxels
  of partial volume at their surface stay. Everything the volume's border reaches without crossing
  a kept cell is removed; what the kept objects enclose (pores, cavities) stays.
- **Writing.** `writeDerivedDataset` takes NaN as "remove this voxel" and leaves out bricks that
  end up empty, so the new dataset is smaller and the removed voxels are outside air for every
  later analysis.

## Consequences

- Holders and debris that do not touch the part disappear in one step; objects that touch it are
  one object, and a region crops them apart first. A holder closer to the part than the grown
  margin keeps a stub there.
- On large scans the cells are coarser: objects closer than about a cell stay together. The cell
  size is reported in the step's summary.
- Memory is about 3 bytes per cell (counts and marks), at most about 400 MB.
- Masks of other shapes (cylinders, painted regions) and splitting touching objects (watershed)
  are not covered; they would add further ways of choosing cells to keep.
