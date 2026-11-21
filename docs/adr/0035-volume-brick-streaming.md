# 0035: Brick streaming of the 3D view

Status: accepted (2026-11-19)

## Context

The 3D view of the GUI (`resources/ui/volume.js`, ADR 0008) ray casts a coarse level of the
dataset of at most 256 voxels per axis, plus one detail box of at most 256³ near the camera. On
a scan of 5300³ voxels that is a view of about 166³ voxels of the part; level 0 can never be
seen in 3D. The review of November 2026 (`docs/review-large-data-2026-11.md`, findings G1–G6,
phase 6) found besides:

- each re-render of the page made a new canvas and WebGL2 context, never released, and context
  loss was not handled (G3);
- each change of the detail box sent 33 MB and uploaded 76–100 MB in one frame (G4);
- the ray caster marched every pixel through empty space, without skipping or early stops (G5);
- `readVolumePreview` built a float copy of the whole preview (G6).

WebGL2 cannot report how much video memory is free, and browsers lose the context when a page
takes too much.

## Decision

**Bricks of 32³ voxels per level, streamed into a fixed atlas.** The coarse level stays one
texture that is always loaded. The finer levels are split into bricks of 32³ voxels of their
level; each comes from the server with two voxels around it (`GET /api/volume_brick`, 36³ at
most, 8-bit grey values in the window of the coarse level and the overlay; linear filtering reads
half a voxel beyond the brick, the gradients of the shading one and a half) and goes into a slot
of an atlas: a few 3D textures of fixed size (grey R8, overlay R8, and RG8 visibility when
materials are shown). Their size follows from a **VRAM budget**: 512 MB by default, set by the
user in the view and kept in the browser (`localStorage`). A budget the GPU refuses is halved
until it fits.

**A page table** (an RGBA8UI 3D texture) holds one texel per brick of every finer level, and one
per 8³ cell of the coarse level, the levels stacked along z: the slot of a resident brick, its
state (not loaded, resident, absent, coarse), whether it holds pores or zones, and its largest
grey value. The server tells once which bricks hold voxels at all (`GET /api/volume_bricks`, from
the brick list of the dataset without reading bricks); the others are absent and never asked for.

**Level of detail by distance, refined where the picture needs it.** Each sample of the ray
caster wants the coarsest level whose voxels are at most a CSS pixel at its distance, and takes
the finest level resident at or above it, else the coarse texture. Which bricks to load is not
guessed from the camera: a **feedback pass** renders the same rays at an eighth of the resolution
and writes per pixel the first brick along the ray that was wanted one level finer than what was
found, or else one brick it used (reservoir sampling along the ray). The UI reads it back, asks
for the wanted bricks coarsest first, at most six at a time, and uploads arrivals with
`texSubImage3D` for at most a few milliseconds per frame. A brick unused in the last ten feedback
passes gives its slot to a new one; when none can, the budget is full and loading pauses until
the camera moves. Only what the rays reach is loaded: bricks behind a surface are not.

**Skipping.** A brick or coarse cell that cannot show in the current mode is crossed in one step:
absent bricks always; in the surface view those whose largest value is below the threshold, in
the transfer function view those below its first opaque value, in the maximum intensity view
those below the maximum so far; never those with pores or zones while these are shown. The
largest value covers the voxels around the brick, which linear filtering reads. The transfer
function view stops at an opacity of 0.98, the maximum intensity view at the top grey value, and
while the camera moves the view draws at half the resolution.

**One context per view.** The canvas of each 3D view lives as long as its viewer, so re-renders
of the page keep the context, its textures and its bricks; a lost context is restored and set up
again. The orbit camera, its pointer input and shader compilation are shared by the volume and
the scene view (`orbit.js`).

**No float copy.** `readVolumePreview` takes the window from every 7th voxel of every 4th slice
first, unless given, and maps each slice to bytes as it reads it. `VolumeRequest::level` reads a
given level, which the brick endpoint uses.

## Consequences

- Level 0 can be seen in 3D anywhere in the scan; how much at once is set by the VRAM budget, not
  by a fixed box. The detail box and its status are gone; the status line names the finest level
  loaded, the bricks and the budget.
- Bricks at the border between two levels are not blended: where the level changes, a seam can
  show while bricks arrive.
- The bricks are 8-bit like the coarse level, in the window of the whole volume.
- Changing which materials are hidden loads the bricks again, since their visibility channel is
  computed in the browser from the overlay, which is not kept.
- The browser's HTTP cache is not used for bricks: a step can be computed again under the same
  number after an undo, so the responses are not marked cacheable.
- The page table takes 4 bytes per brick of level 0 and a seventh more for the coarser levels:
  about 21 MB of the GPU for 5300³. The levels shown are limited to 16.
