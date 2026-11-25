# 0043: Colours, light and video of the 3D view

Status: accepted (2026-11-25)

## Context

The 3D view (ADR 0008, ADR 0035) drew good working pictures, but not pictures to show: five
colour maps that only recoloured the few control points of the transfer function (so a map with
nine stops became a ramp between three or four colours), one fixed headlight with a fixed
highlight, no shadows, and no way to make a video of the part. A CT scan of a casting or an
assembly is often shown to people who never open the software — in a report, a talk or a mail —
and a short turntable video says more there than a dozen screenshots.

Encoding video is a format question (ADR 0016: established libraries only), and a video encoder
would be a large new dependency for one button.

## Decision

**Colours from a map at every grey value.** A transfer function has a colour source: the colours
of its points (as before, and still the default of the suggestions), or a colour map that gives
every one of the 256 grey values its colour, spread between two markers that are dragged on a
colour strip under the histogram (default: the grey values the curve lets be seen). The opacity
always comes from the points. There are sixteen maps in groups: neutral (grey, steel, bone),
perceptual (viridis, inferno, magma, plasma, cividis; samples of the published scales), material
looks (copper, gold, jade, ice, ember), turbo and a diverging cool-to-warm map, and an own
gradient of three colours. Every map can be reversed. `transferTable` in `transfer.js` builds the
table; the curve editor shows the colours of the table, also on its points.

**Light as a few named parts** (`lighting.js`): a key light given by azimuth and elevation that
either follows the camera or stays in the scene (so a turntable shows the part turning under the
light), a fill light from the other side, a rim light on the silhouette, ambient light, the
strength and size of the highlights, shadows of the key light and ambient occlusion. Presets
(headlight, studio, top light, dramatic, back light, flat) set them all; changing one makes the
lighting "own". The default is the headlight the view always had, so saved views look the same.
Normals are turned to the viewer and lit one-sided, so light from behind is light from behind.

**Shadows and occlusion from the coarse level.** The ray caster and the meshes share one piece of
GLSL. A shadow ray marches from the surface towards the key light through the coarse level
(at most 256 voxels per axis, always complete in VRAM), with steps that grow by 4 % so the far
side of the part is reached in at most 96 samples and the shadow softens with distance. Ambient
occlusion takes 8 samples around the normal at two radii. In the surface view and on meshes both
are computed where the ray stops; in the translucent view once per pixel, at the sample that
gives most to it, and scale the whole pixel. What blocks the light is the same thing that is
seen: the grey threshold and the shown materials in the surface view, the opacity of the
transfer function in the translucent one. The cut opens the part for the light as for the eye.

**Edge emphasis** scales the opacity of the transfer function by the gradient: surfaces, pore
walls and boundaries between materials stay, the inside clears. **Per-material opacity** and
**palettes** (vivid, pastel, metals, colour-blind safe) make a segmented part readable in the
translucent view.

**Looks**: a look is a curve preset, a colour map, a lighting, a background and an edge emphasis
in one click (studio, clay, glass, gold, X-ray, glow), each shown as a small rendering of the
scan itself. The previews are drawn at their own small size, one per frame, so a slow graphics
card or a software renderer does not stall the page.

**Video by the browser.** `video.js` moves the camera along a motion — turntable, swing, orbit,
a sweep of the cut into the part and back, a zoom, or a flight through the saved views of the
volume — as a pure function of time, so it is tested in node. The recording has three phases:
the motion is run through once so the bricks it needs are streamed in; every frame is drawn at
the chosen size (up to 3840 × 2160) and kept as a JPEG; then the frames are played at their rate
into the browser's own encoder (`MediaRecorder`: MP4 with H.264 where the browser writes it,
else WebM). The encoder stamps frames with the time they arrive, which is why the drawing and the
encoding are separate: a view that needs a second per frame still gives a video of the right
length and steady motion. A short recording that is thrown away first takes the pause an encoder
needs to start. The video is downloaded; no encoder or muxer is part of VoxelSieve.

Lighting, edge emphasis, material opacities and the colour settings of the transfer function are
part of the view state, so they are saved with the project and with named views.

## Consequences

- No new dependency. The video format is the browser's: MP4 in current Chrome and Edge, WebM in
  Firefox. A WebM can be converted with any video tool where MP4 is needed.
- The frames of a video are held in memory as JPEGs until it is encoded: about 0.3 MB per frame
  at 1920 × 1080, so a minute at 30 fps is about 0.5 GB of browser memory; 4K is four times that.
- Shadows and occlusion follow the coarse level, so they are soft and miss details finer than a
  coarse voxel; a small pore casts no shadow. They cost up to about a hundred texture reads per
  pixel, once per pixel, and are off in the headlight preset.
- The translucent view takes its shadow at one sample per pixel: right for opaque-looking curves,
  an approximation for very translucent ones, where shadows are faint anyway.
- Video and looks are features of the browser's WebGL view. MCP clients keep `view_objects`
  (the CPU renderer) for pictures; a video needs a browser.
