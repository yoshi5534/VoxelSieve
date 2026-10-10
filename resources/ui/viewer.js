// Slice viewer of VoxelSieve Studio (ADR 0008). The volume is never loaded: the view requests
// tiles of 256 x 256 pixels from the resolution level whose voxels match the zoom, so a view of a
// scan of hundreds of GB reads only a few bricks. Grey values arrive as float32 and are windowed
// here, so contrast changes need no new requests.
//
// Other objects of the project show in the same plane (ADR 0018): other volumes as layers
// sampled by the server at the pixels of the tiles of the shown volume (api/object_tile), blended
// in the object's colour or as a checkerboard, and meshes as the lines where they cut the slice
// (api/cut_lines).
'use strict';

const TILE = 256;
const MAX_TILES = 160;          // cached tiles, about 0.5 MB each
const MAX_REQUESTS = 4;         // concurrent tile requests
const AXIS_NAMES = ['x', 'y', 'z'];
const IN_PLANE = [[1, 2], [0, 2], [0, 1]];
const CHECKER = 32;             // edge of the checkerboard squares in tile pixels

/// Voxel edge lengths [x, y, z] in mm; datasets store a number for cubic voxels (ADR 0012).
function voxelPitch(size) {
  return Array.isArray(size) ? size : [size, size, size];
}

// Overlay value of material m is MATERIAL_OVERLAY + m (SliceOverlay::kMaterial); the colours
// match those of the material volume (materials.cpp).
const MATERIAL_OVERLAY = 16;
const MATERIAL_COLORS = [[66, 146, 198], [230, 126, 34], [46, 160, 67], [196, 60, 80],
  [142, 99, 190], [214, 190, 40], [23, 170, 170], [140, 110, 80]];

// What a pixel of another volume hit (kPlaneVoid, kPlaneMaterial in slice.hpp): a kept voxel
// below the threshold of that volume (air near the part, a pore) or its material.
const PLANE_VOID = 1;
const PLANE_MATERIAL = 2;

/// Pixels of a tile of another volume, RGBA: in 'blend' mode its material in the object's colour,
/// brighter where it is denser, covering the slice by `opacity`; in 'checker' mode all its kept
/// voxels in grey in every other square of a checkerboard, the slice in the squares between.
/// The material edge fades in over a tenth of the window around `threshold`, so the
/// interpolated values show the surface instead of a staircase of voxels.
function layerPixels(data, inside, size, color, window, threshold, mode, opacity, out) {
  const [low, high] = window ?? [0, 1];
  const scale = 1 / (high - low);
  const ramp = 0.1 * (high - low);
  for (let i = 0; i < data.length; i += 1) {
    const p = i * 4;
    const t = Math.min(Math.max((data[i] - low) * scale, 0), 1);
    if (mode === 'checker') {
      const x = i % size;
      const y = Math.floor(i / size);
      const shown = inside[i] >= PLANE_VOID && (Math.floor(x / CHECKER) + Math.floor(y / CHECKER)) % 2 === 1;
      out[p] = out[p + 1] = out[p + 2] = t * 255;
      out[p + 3] = shown ? 255 : 0;
    } else {
      const shade = 0.3 + 0.7 * t;
      out[p] = shade * color[0];
      out[p + 1] = shade * color[1];
      out[p + 2] = shade * color[2];
      const cover = Math.min(Math.max((data[i] - threshold + ramp) / (2 * ramp), 0), 1);
      out[p + 3] = inside[i] >= PLANE_VOID ? cover * opacity * 255 : 0;
    }
  }
  return out;
}

/// Screen positions of cut segments (u0 v0 u1 v1 in level-0 voxel indices, whose centres lie on
/// whole numbers): voxel u covers the screen from u to u + 1 as the tiles draw it.
function cutsToScreen(segments, view) {
  const { center, zoom, stretch, width, height } = view;
  const out = new Float32Array(segments.length);
  for (let i = 0; i < segments.length; i += 2) {
    out[i] = (segments[i] + 0.5 - center[0]) * zoom * stretch[0] + width / 2;
    out[i + 1] = (segments[i + 1] + 0.5 - center[1]) * zoom * stretch[1] + height / 2;
  }
  return out;
}

/// Window from the 0.5 and 99.5 percentiles of the material of tiles of another volume, or null.
function insideWindow(tiles) {
  const values = [];
  for (const tile of tiles) {
    for (let i = 0; i < tile.data.length; i += 7) {
      if (tile.inside[i] === PLANE_MATERIAL) values.push(tile.data[i]);
    }
  }
  if (values.length < 16) return null;
  values.sort((a, b) => a - b);
  const low = values[Math.floor(values.length * 0.005)];
  const high = values[Math.floor(values.length * 0.995)];
  return [low, Math.max(high, low + 1)];
}

class SliceViewer {
  constructor() {
    this.tiles = new Map();     // key -> {data, overlay, canvas, rendered, used}
    this.loading = new Map();   // key -> AbortController of the request
    this.failed = new Set();
    this.queue = [];
    this.active = 0;
    this.info = null;           // dataset_info
    this.step = null;           // dataset step id
    this.porosity = null;       // porosity step id for the overlay
    this.materials = null;      // material segmentation step id for the overlay
    this.axis = 2;
    this.index = [0, 0, 0];
    this.zoom = 1;              // screen pixels per level-0 voxel along the finer in-plane axis
    this.center = [0, 0];       // level-0 voxel coordinates of the view centre (u, v)
    this.window = null;         // [low, high]
    this.showOverlay = true;
    this.hiddenMaterials = [];  // ids of the material classes left out of the overlay
    this.materialColors = MATERIAL_COLORS;  // colour of material m at m - 1
    // Other objects: {id, number, kind, color, version, show, mode, opacity, window}; `version`
    // changes with anything that moves the object relative to the slice.
    this.layers = [];
    this.layerSettings = {};    // object id -> {show, mode, opacity}, saved with the view
    this.cuts = new Map();      // object id -> {key, segments} of the cut lines shown
    this.cutLoading = new Map();  // object id -> {key, controller}
    this.canvas = null;
    this.drawPending = false;
    this.onChange = () => {};
  }

  /// Shows a dataset; keeps the view when the dataset stays the same.
  setDataset(info, step, porosity, materials = null) {
    const same = this.step === step && this.info !== null;
    this.info = info;
    this.step = step;
    if (this.porosity !== porosity || this.materials !== materials) {
      this.porosity = porosity;
      this.materials = materials;
      this.clearTiles();
    }
    if (!same) {
      this.clearTiles();
      this.window = null;
      this.index = info.dims.map((d) => Math.floor(d / 2));
      this.fitPending = true;
    }
  }

  /// Slice, zoom and window, for saving them with the project or as a named view.
  getState() {
    const saved = { step: this.step, axis: this.axis, index: [...this.index], window: this.window,
      showOverlay: this.showOverlay, layers: structuredClone(this.layerSettings) };
    // Before the first picture the zoom is not fitted yet; leave it to the next fit then.
    if (!this.fitPending) Object.assign(saved, { zoom: this.zoom, center: [...this.center] });
    return saved;
  }

  /// Restores a state of the same dataset; call after setDataset.
  setState(saved) {
    if (!this.info || saved.step !== this.step) return;
    this.axis = saved.axis;
    this.index = saved.index.map((i, a) => Math.min(Math.max(i, 0), this.info.dims[a] - 1));
    this.window = saved.window;
    this.showOverlay = saved.showOverlay;
    if (saved.layers) {
      this.layerSettings = structuredClone(saved.layers);
      for (const layer of this.layers) Object.assign(layer, this.layerSettings[layer.id] ?? {});
    }
    if (saved.zoom && saved.center) {
      this.zoom = saved.zoom;
      this.center = [...saved.center];
      this.fitPending = false;
    }
    this.requestDraw();
    this.onChange();
  }

  /// The current picture as a PNG data URL.
  capture() {
    this.draw();
    return this.canvas.toDataURL('image/png');
  }

  clearTiles() {
    for (const controller of this.loading.values()) controller.abort();
    this.tiles.clear();
    this.failed.clear();
    this.queue = [];
    this.frame = null;
    for (const { controller } of this.cutLoading.values()) controller.abort();
    this.cutLoading.clear();
    this.cuts.clear();
  }

  /// The other objects to show in the slice: [{id, kind, color, version}], kind 'volume' or
  /// 'mesh'. Volumes start blended at half opacity, meshes as lines; both start hidden.
  setLayers(objects) {
    this.layers = objects.map((object) => {
      const before = this.layers.find((layer) => layer.id === object.id);
      const layer = { show: false, mode: 'blend', opacity: 0.5, ...object,
        ...this.layerSettings[object.id] };
      layer.number = Number(object.id.replace(/^o/, ''));
      // The window of a volume stays as long as the volume does.
      layer.window = before && before.source === object.source ? before.window : null;
      return layer;
    });
    for (const id of this.cuts.keys()) {
      if (!this.layers.some((layer) => layer.id === id)) this.cuts.delete(id);
    }
    this.requestDraw();
  }

  /// Changes how an object shows: {show, mode, opacity}.
  setLayer(id, change) {
    const layer = this.layers.find((candidate) => candidate.id === id);
    if (!layer) return;
    Object.assign(layer, change);
    this.layerSettings[id] = { show: layer.show, mode: layer.mode, opacity: layer.opacity };
    this.requestDraw();
    this.onChange();
  }

  shownLayers(kind) {
    return this.layers.filter((layer) => layer.show && layer.kind === kind);
  }

  attach(canvas) {
    this.canvas = canvas;
    let drag = null;
    canvas.addEventListener('pointerdown', (event) => {
      drag = { x: event.clientX, y: event.clientY, center: [...this.center] };
      canvas.setPointerCapture(event.pointerId);
    });
    canvas.addEventListener('pointermove', (event) => {
      if (drag) {
        const [su, sv] = this.stretch();
        this.center = [drag.center[0] - (event.clientX - drag.x) / (this.zoom * su),
          drag.center[1] - (event.clientY - drag.y) / (this.zoom * sv)];
        this.requestDraw();
      }
      this.hover = this.voxelAt(event);
      this.onChange();
    });
    canvas.addEventListener('pointerup', () => { drag = null; });
    canvas.addEventListener('pointerleave', () => {
      this.hover = null;
      this.onChange();
    });
    canvas.addEventListener('wheel', (event) => {
      event.preventDefault();
      if (event.shiftKey) {
        this.moveSlice(event.deltaY > 0 ? 1 : -1);
        return;
      }
      const before = this.screenToVoxel(event.offsetX, event.offsetY);
      const factor = Math.exp(-event.deltaY * 0.0015);
      this.zoom = Math.min(Math.max(this.zoom * factor, this.minZoom()), 32);
      const after = this.screenToVoxel(event.offsetX, event.offsetY);
      this.center = [this.center[0] + before[0] - after[0], this.center[1] + before[1] - after[1]];
      this.requestDraw();
      this.onChange();
    }, { passive: false });
    new ResizeObserver(() => this.requestDraw()).observe(canvas);
    this.requestDraw();
  }

  planeAxes() {
    return IN_PLANE[this.axis];
  }

  planeDims() {
    const [u, v] = this.planeAxes();
    return [this.info.dims[u], this.info.dims[v]];
  }

  /// Screen size of a voxel along the in-plane axes relative to the finer one: voxels that are
  /// not cubes are drawn stretched, so the slice shows the part in its true proportions.
  stretch() {
    const pitch = voxelPitch(this.info.voxel_size_mm);
    const [u, v] = this.planeAxes();
    const finer = Math.min(pitch[u], pitch[v]);
    return [pitch[u] / finer, pitch[v] / finer];
  }

  maxLevel() {
    return this.info.levels.length - 1;
  }

  minZoom() {
    const [w, h] = this.planeDims();
    const [su, sv] = this.stretch();
    return Math.min(this.canvas.clientWidth / (w * su), this.canvas.clientHeight / (h * sv)) / 8;
  }

  fit() {
    const [w, h] = this.planeDims();
    const [su, sv] = this.stretch();
    this.zoom = Math.min(this.canvas.clientWidth / (w * su), this.canvas.clientHeight / (h * sv)) *
      0.95;
    this.center = [w / 2, h / 2];
  }

  setAxis(axis) {
    this.axis = axis;
    this.fit();
    this.requestDraw();
    this.onChange();
  }

  setSlice(index) {
    const max = this.info.dims[this.axis] - 1;
    this.index[this.axis] = Math.min(Math.max(Math.round(index), 0), max);
    this.requestDraw();
    this.onChange();
  }

  moveSlice(delta) {
    this.setSlice(this.index[this.axis] + delta * (1 << this.level()));
  }

  /// Centres the view on a voxel and shows its slice; with `extent` (voxels), zooms so that
  /// the extent fills about a third of the view.
  showVoxel(voxel, extent = 0) {
    const [u, v] = this.planeAxes();
    this.index[this.axis] = Math.round(voxel[this.axis]);
    this.center = [voxel[u], voxel[v]];
    if (extent > 0) {
      const size = Math.min(this.canvas.clientWidth, this.canvas.clientHeight);
      this.zoom = Math.min(Math.max(size / (3 * extent), this.minZoom()), 32);
    }
    this.requestDraw();
    this.onChange();
  }

  level() {
    const level = Math.floor(Math.log2(1 / this.zoom));
    return Math.min(Math.max(level, 0), this.maxLevel());
  }

  screenToVoxel(x, y) {
    const [su, sv] = this.stretch();
    return [this.center[0] + (x - this.canvas.clientWidth / 2) / (this.zoom * su),
      this.center[1] + (y - this.canvas.clientHeight / 2) / (this.zoom * sv)];
  }

  voxelAt(event) {
    const rect = this.canvas.getBoundingClientRect();
    const [pu, pv] = this.screenToVoxel(event.clientX - rect.left, event.clientY - rect.top);
    const [u, v] = this.planeAxes();
    const voxel = [0, 0, 0];
    voxel[u] = Math.floor(pu);
    voxel[v] = Math.floor(pv);
    voxel[this.axis] = this.index[this.axis];
    if (voxel.some((c, a) => c < 0 || c >= this.info.dims[a])) return null;
    return { voxel, value: this.valueAt(voxel) };
  }

  /// Grey value at a level-0 voxel from the finest cached tile, or undefined.
  valueAt(voxel) {
    const [u, v] = this.planeAxes();
    for (let level = this.level(); level <= this.maxLevel(); level += 1) {
      const lu = voxel[u] >> level;
      const lv = voxel[v] >> level;
      const tile = this.tiles.get(this.key(level, Math.floor(lu / TILE), Math.floor(lv / TILE)));
      if (tile) return tile.data[(lv % TILE) * TILE + (lu % TILE)];
    }
    return undefined;
  }

  key(level, tu, tv, index = this.index[this.axis], layer = null) {
    return this.slot(level, tu, tv, layer) + '@' + (index >> level);
  }

  /// A tile position independent of the slice, of the volume or of the layer of another object.
  slot(level, tu, tv, layer = null) {
    const content = layer ? ['object', layer.id, layer.version]
      : [this.porosity ?? '-', this.materials ?? '-'];
    return [this.step, ...content, this.axis, level, tu, tv].join('/');
  }

  /// Cached tile of the same position from the slice nearest to the current one, or undefined.
  nearestTile(level, tu, tv, layer = null) {
    const slot = this.slot(level, tu, tv, layer);
    const slice = this.index[this.axis] >> level;
    let best;
    for (const tile of this.tiles.values()) {
      if (tile.slot !== slot) continue;
      if (!best || Math.abs(tile.slice - slice) < Math.abs(best.slice - slice)) best = tile;
    }
    return best;
  }

  /// The slice (of the level) to show: the current one when all its visible tiles are there,
  /// otherwise the complete one nearest to it, or undefined when there is none.
  shownSlice(level, visible) {
    const slots = visible.map(([tu, tv]) => this.slot(level, tu, tv));
    if (!slots.length) return undefined;
    const complete = (slice) => slots.every((slot) =>
      this.tiles.has(slot + '@' + slice) || this.failed.has(slot + '@' + slice));
    const current = this.index[this.axis] >> level;
    if (complete(current)) return current;
    let best;
    for (const tile of this.tiles.values()) {
      if (tile.slot !== slots[0] || tile.slice === best) continue;
      if (best !== undefined && Math.abs(tile.slice - current) >= Math.abs(best - current)) continue;
      if (complete(tile.slice)) best = tile.slice;
    }
    return best;
  }

  requestDraw() {
    if (this.drawPending || !this.canvas) return;
    this.drawPending = true;
    requestAnimationFrame(() => {
      this.drawPending = false;
      this.draw();
    });
  }

  /// Visible tiles of a level: [tu, tv, x, y, w, h] with screen position and size.
  visibleTiles(level) {
    const [su, sv] = this.stretch();
    const scale = 1 << level;
    const extent = TILE * scale;
    const width = this.canvas.clientWidth;
    const height = this.canvas.clientHeight;
    const [u0, v0] = this.screenToVoxel(0, 0);
    const [u1, v1] = this.screenToVoxel(width, height);
    const [du, dv] = this.planeDims();
    // Tile edges snap to device pixels, so neighbouring tiles share an edge exactly. Fractional
    // edges are antialiased on their own for each tile and leave a faint seam between them.
    const ratio = window.devicePixelRatio || 1;
    const snap = (value) => Math.round(value * ratio) / ratio;
    const tiles = [];
    for (let tv = Math.max(0, Math.floor(v0 / extent)); tv * extent < Math.min(v1, dv); tv += 1) {
      for (let tu = Math.max(0, Math.floor(u0 / extent)); tu * extent < Math.min(u1, du); tu += 1) {
        const x0 = snap((tu * extent - this.center[0]) * this.zoom * su + width / 2);
        const y0 = snap((tv * extent - this.center[1]) * this.zoom * sv + height / 2);
        const x1 = snap(((tu + 1) * extent - this.center[0]) * this.zoom * su + width / 2);
        const y1 = snap(((tv + 1) * extent - this.center[1]) * this.zoom * sv + height / 2);
        tiles.push([tu, tv, x0, y0, x1 - x0, y1 - y0]);
      }
    }
    return tiles;
  }

  draw() {
    if (!this.canvas || !this.info) return;
    const canvas = this.canvas;
    const ratio = window.devicePixelRatio || 1;
    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    if (width === 0 || height === 0) return;
    if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
      canvas.width = Math.round(width * ratio);
      canvas.height = Math.round(height * ratio);
    }
    if (this.fitPending) {
      this.fitPending = false;
      this.fit();
    }
    const context = canvas.getContext('2d');
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    context.fillStyle = '#111';
    context.fillRect(0, 0, width, height);
    context.imageSmoothingEnabled = false;

    const level = this.level();
    const visible = this.visibleTiles(level);
    // Coarser tiles first as placeholders, then the wanted level on top. Until every visible tile
    // of the current slice is there, the nearest slice that is complete stands in for it, so
    // scrolling shows whole slices instead of a black flash per step or a mix of tiles from
    // different slices. Without a complete slice, each tile takes its nearest one.
    const shown = this.shownSlice(level, visible);
    // Tiles at the far edges reach past the volume; only the volume itself is drawn.
    const [du, dv] = this.planeDims();
    const [su, sv] = this.stretch();
    const outline = [(0 - this.center[0]) * this.zoom * su + width / 2,
      (0 - this.center[1]) * this.zoom * sv + height / 2, du * this.zoom * su, dv * this.zoom * sv];
    context.save();
    context.beginPath();
    context.rect(...outline);
    context.clip();
    for (let l = Math.min(level + 3, this.maxLevel()); l >= level; l -= 1) {
      for (const [tu, tv, x, y, w, h] of l === level ? visible : this.visibleTiles(l)) {
        let tile = this.tiles.get(this.key(l, tu, tv));
        if (l === level && !tile) {
          tile = shown === undefined ? this.nearestTile(l, tu, tv)
            : this.tiles.get(this.slot(l, tu, tv) + '@' + shown);
        }
        if (!tile) continue;
        tile.used = performance.now();
        context.drawImage(this.renderTile(tile), x, y, w, h);
      }
    }
    // Other volumes over it, at the slice the volume shows. Their values are interpolated, so
    // they are drawn smoothly too: they rarely run along the voxels of the shown volume.
    const layers = this.shownLayers('volume');
    context.imageSmoothingEnabled = true;
    for (const layer of layers) {
      for (const [tu, tv, x, y, w, h] of visible) {
        const slice = shown ?? this.index[this.axis] >> level;
        const tile = this.tiles.get(this.slot(level, tu, tv, layer) + '@' + slice) ??
          this.nearestTile(level, tu, tv, layer);
        if (!tile) continue;
        tile.used = performance.now();
        context.drawImage(this.renderLayerTile(tile, layer), x, y, w, h);
      }
    }
    context.imageSmoothingEnabled = false;
    context.restore();
    this.drawCuts(context, width, height);
    // Outline of the volume.
    context.strokeStyle = 'rgba(255,255,255,0.25)';
    context.strokeRect(...outline);

    // Tiles load slice by slice: a slice once started loads completely before the next one
    // starts, at the slice current by then. Scrolling thus updates whole slices and skips those
    // scrolled past, instead of starting every slice and finishing none.
    const frameContext = this.slot(level, 0, 0);
    const missing = (index) => visible.filter(([tu, tv]) => {
      const key = this.key(level, tu, tv, index);
      return !this.tiles.has(key) && !this.failed.has(key);
    });
    if (!this.frame || this.frame.context !== frameContext || !missing(this.frame.index).length) {
      this.frame = { context: frameContext, index: this.index[this.axis] };
    }
    const index = this.frame.index;
    const order = (a, b) => a.distance - b.distance;
    const wanted = (layer) => visible
      .filter(([tu, tv]) => {
        const key = this.key(level, tu, tv, index, layer);
        return !this.tiles.has(key) && !this.failed.has(key) && !this.loading.has(key);
      })
      .map(([tu, tv, x, y, w, h]) => ({ level, tu, tv, index, layer,
        distance: Math.hypot(x + w / 2 - width / 2, y + h / 2 - height / 2) }))
      .sort(order);
    // The volume first, then the other objects.
    this.queue = [wanted(null), ...layers.map(wanted)].flat();
    this.pump(new Set([null, ...layers].flatMap((layer) =>
      visible.map(([tu, tv]) => this.key(level, tu, tv, index, layer)))));
    this.loadCuts();
  }

  /// Requests the cut lines of the shown meshes at the current slice; the previous lines stay
  /// until the new ones arrive.
  loadCuts() {
    const index = this.index[this.axis];
    for (const layer of this.shownLayers('mesh')) {
      const key = [this.step, layer.version, this.axis, index].join('/');
      if (this.cuts.get(layer.id)?.key === key) continue;
      const loading = this.cutLoading.get(layer.id);
      if (loading?.key === key) continue;
      loading?.controller.abort();
      const controller = new AbortController();
      this.cutLoading.set(layer.id, { key, controller });
      const params = new URLSearchParams({ step: this.step, object: layer.number,
        axis: this.axis, index });
      fetch('api/cut_lines?' + params, { signal: controller.signal })
        .then(async (response) => {
          if (!response.ok) throw new Error((await response.json()).error);
          this.cuts.set(layer.id, { key, segments: new Float32Array(await response.arrayBuffer()) });
        })
        .catch((error) => {
          if (error.name !== 'AbortError') console.warn('Cut lines', layer.id, error);
        })
        .finally(() => {
          if (this.cutLoading.get(layer.id)?.controller === controller) {
            this.cutLoading.delete(layer.id);
          }
          this.requestDraw();
        });
    }
  }

  drawCuts(context, width, height) {
    const view = { center: this.center, zoom: this.zoom, stretch: this.stretch(), width, height };
    context.save();
    context.lineWidth = 1.5;
    context.lineCap = 'round';
    for (const layer of this.shownLayers('mesh')) {
      const cuts = this.cuts.get(layer.id);
      if (!cuts) continue;
      const screen = cutsToScreen(cuts.segments, view);
      context.strokeStyle = 'rgb(' + layer.color.join(',') + ')';
      context.beginPath();
      for (let i = 0; i + 3 < screen.length; i += 4) {
        context.moveTo(screen[i], screen[i + 1]);
        context.lineTo(screen[i + 2], screen[i + 3]);
      }
      context.stroke();
    }
    context.restore();
  }

  /// Starts queued requests; those not in `wanted` (after a zoom or pan) give their places up.
  pump(wanted) {
    for (const [key, controller] of this.loading) {
      if (!wanted.has(key)) controller.abort();
    }
    while (this.active < MAX_REQUESTS && this.queue.length) {
      const { level, tu, tv, index, layer } = this.queue.shift();
      this.fetchTile(level, tu, tv, index, layer);
    }
  }

  async fetchTile(level, tu, tv, index, layer = null) {
    const key = this.key(level, tu, tv, index, layer);
    const slot = this.slot(level, tu, tv, layer);
    const slice = index >> level;
    const controller = new AbortController();
    this.loading.set(key, controller);
    this.active += 1;
    const params = new URLSearchParams({
      step: this.step, axis: this.axis, index, level,
      u: tu * TILE, v: tv * TILE, size: TILE,
    });
    if (layer) params.set('object', layer.number);
    if (!layer && this.porosity !== null) params.set('porosity', this.porosity);
    if (!layer && this.materials !== null) params.set('materials', this.materials);
    try {
      const response = await fetch((layer ? 'api/object_tile?' : 'api/tile?') + params,
        { signal: controller.signal });
      if (!response.ok) throw new Error((await response.json()).error);
      const buffer = await response.arrayBuffer();
      const count = TILE * TILE;
      const owner = layer ? layer.id + '#' + layer.version : null;
      const tile = { data: new Float32Array(buffer, 0, count), slot, slice, owner, canvas: null,
        rendered: null, used: performance.now() };
      // The overlay of the volume, or what the tile hit of the other volume.
      tile[layer ? 'inside' : 'overlay'] = new Uint8Array(buffer, count * 4, count);
      if (layer) tile.threshold = Number(response.headers.get('X-Threshold'));
      this.tiles.set(key, tile);
      if (layer && !layer.window) {
        // Its own window, from its first tiles.
        layer.window = insideWindow([...this.tiles.values()].filter((t) => t.owner === owner));
      }
      this.evict();
    } catch (error) {
      if (error.name !== 'AbortError') {
        this.failed.add(key);
        console.warn('Tile', key, error);
      }
    } finally {
      if (this.loading.get(key) === controller) this.loading.delete(key);
      this.active -= 1;
      // The first window comes from the whole first view, air and material alike.
      if (!this.window && this.active === 0 && this.queue.length === 0) this.autoWindow();
      this.requestDraw();
    }
  }

  evict() {
    if (this.tiles.size <= MAX_TILES) return;
    const oldest = [...this.tiles.entries()].sort((a, b) => a[1].used - b[1].used);
    for (const [key] of oldest.slice(0, this.tiles.size - MAX_TILES)) this.tiles.delete(key);
  }

  /// Window from the 0.5 and 99.5 percentiles of the visible tiles of the current slice.
  autoWindow() {
    const values = [];
    const level = this.level();
    for (const [tu, tv] of this.visibleTiles(level)) {
      const tile = this.tiles.get(this.key(level, tu, tv));
      if (!tile) continue;
      for (let i = 0; i < tile.data.length; i += 7) values.push(tile.data[i]);
    }
    if (!values.length) return;
    values.sort((a, b) => a - b);
    const low = values[Math.floor(values.length * 0.005)];
    let high = values[Math.floor(values.length * 0.995)];
    if (high <= low) high = low + 1;
    this.setWindow(low, high);
  }

  setWindow(low, high) {
    this.window = [low, high];
    this.requestDraw();
    this.onChange();
  }

  setOverlay(show) {
    this.showOverlay = show;
    this.requestDraw();
  }

  /// Leaves the material classes with these ids out of the overlay.
  setHiddenMaterials(ids) {
    this.hiddenMaterials = [...ids];
    this.requestDraw();
  }

  /// Colours of the material classes (material m at m - 1), as segmented or chosen in the view.
  setMaterialColors(colors) {
    this.materialColors = colors.map((color) => [...color]);
    this.requestDraw();
  }

  renderLayerTile(tile, layer) {
    const state = [layer.window?.[0], layer.window?.[1], layer.mode, layer.opacity].join('/');
    if (tile.rendered === state) return tile.canvas;
    if (!tile.canvas) {
      tile.canvas = document.createElement('canvas');
      tile.canvas.width = TILE;
      tile.canvas.height = TILE;
    }
    const context = tile.canvas.getContext('2d');
    const image = context.createImageData(TILE, TILE);
    layerPixels(tile.data, tile.inside, TILE, layer.color, layer.window, tile.threshold, layer.mode,
      layer.opacity, image.data);
    context.putImageData(image, 0, 0);
    tile.rendered = state;
    return tile.canvas;
  }

  renderTile(tile) {
    const state = [this.window?.[0], this.window?.[1], this.showOverlay,
      this.hiddenMaterials.join(','), this.materialColors.flat().join(',')].join('/');
    if (tile.rendered === state) return tile.canvas;
    if (!tile.canvas) {
      tile.canvas = document.createElement('canvas');
      tile.canvas.width = TILE;
      tile.canvas.height = TILE;
    }
    const context = tile.canvas.getContext('2d');
    const image = context.createImageData(TILE, TILE);
    slicePixels(tile.data, tile.overlay, this.window ?? [0, 1],
      this.showOverlay ? this.hiddenMaterials : null, image.data, this.materialColors);
    context.putImageData(image, 0, 0);
    tile.rendered = state;
    return tile.canvas;
  }
}

/// Pixels of a tile of the volume, RGBA: the grey values between `low` and `high`, with the
/// overlay tinted on top unless `hiddenMaterials` (ids of material classes left out) is null;
/// material m in `materialColors[m - 1]`.
function slicePixels(data, overlay, [low, high], hiddenMaterials, out,
  materialColors = MATERIAL_COLORS) {
  const scale = 255 / (high - low);
  for (let i = 0; i < data.length; i += 1) {
    const grey = Math.min(Math.max((data[i] - low) * scale, 0), 255);
    let r = grey;
    let g = grey;
    let b = grey;
    if (hiddenMaterials) {
      const kind = overlay[i];
      // Tinted, so the grey values stay readable: pores red, zones yellow.
      if (kind === 1) {
        r = 0.35 * grey + 165; g = 0.35 * grey + 25; b = 0.35 * grey + 25;
      } else if (kind === 2) {
        r = 0.5 * grey + 125; g = 0.5 * grey + 100; b = 0.4 * grey;
      } else if (kind > MATERIAL_OVERLAY && !hiddenMaterials.includes(kind - MATERIAL_OVERLAY)) {
        // Materials in their colour, half covering the grey value.
        const id = kind - MATERIAL_OVERLAY;
        const [mr, mg, mb] = materialColors[id - 1] ?? MATERIAL_COLORS[(id - 1) % 8];
        r = 0.5 * grey + 0.5 * mr; g = 0.5 * grey + 0.5 * mg; b = 0.5 * grey + 0.5 * mb;
      }
    }
    const p = i * 4;
    out[p] = r;
    out[p + 1] = g;
    out[p + 2] = b;
    out[p + 3] = 255;
  }
}

if (typeof window !== 'undefined') {
  Object.assign(window, { SliceViewer, MATERIAL_COLORS, SLICE_AXIS_NAMES: AXIS_NAMES });
}
if (typeof module !== 'undefined') {
  module.exports = { cutsToScreen, insideWindow, layerPixels, slicePixels };
}
