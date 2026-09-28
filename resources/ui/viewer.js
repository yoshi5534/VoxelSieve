// Slice viewer of VoxelSieve Studio (ADR 0008). The volume is never loaded: the view requests
// tiles of 256 x 256 pixels from the resolution level whose voxels match the zoom, so a view of a
// scan of hundreds of GB reads only a few bricks. Grey values arrive as float32 and are windowed
// here, so contrast changes need no new requests.
'use strict';

const TILE = 256;
const MAX_TILES = 160;          // cached tiles, about 0.5 MB each
const MAX_REQUESTS = 4;         // concurrent tile requests
const AXIS_NAMES = ['x', 'y', 'z'];
const IN_PLANE = [[1, 2], [0, 2], [0, 1]];

class SliceViewer {
  constructor() {
    this.tiles = new Map();     // key -> {data, overlay, canvas, rendered, used}
    this.loading = new Set();
    this.failed = new Set();
    this.queue = [];
    this.active = 0;
    this.info = null;           // dataset_info
    this.step = null;           // dataset step id
    this.porosity = null;       // porosity step id for the overlay
    this.axis = 2;
    this.index = [0, 0, 0];
    this.zoom = 1;              // screen pixels per level-0 voxel
    this.center = [0, 0];       // level-0 voxel coordinates of the view centre (u, v)
    this.window = null;         // [low, high]
    this.showOverlay = true;
    this.canvas = null;
    this.drawPending = false;
    this.onChange = () => {};
  }

  /// Shows a dataset; keeps the view when the dataset stays the same.
  setDataset(info, step, porosity) {
    const same = this.step === step && this.info !== null;
    this.info = info;
    this.step = step;
    if (this.porosity !== porosity) {
      this.porosity = porosity;
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
      showOverlay: this.showOverlay };
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
    this.tiles.clear();
    this.failed.clear();
    this.queue = [];
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
        this.center = [drag.center[0] - (event.clientX - drag.x) / this.zoom,
          drag.center[1] - (event.clientY - drag.y) / this.zoom];
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

  maxLevel() {
    return this.info.levels.length - 1;
  }

  minZoom() {
    const [w, h] = this.planeDims();
    return Math.min(this.canvas.clientWidth / w, this.canvas.clientHeight / h) / 8;
  }

  fit() {
    const [w, h] = this.planeDims();
    this.zoom = Math.min(this.canvas.clientWidth / w, this.canvas.clientHeight / h) * 0.95;
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
    return [this.center[0] + (x - this.canvas.clientWidth / 2) / this.zoom,
      this.center[1] + (y - this.canvas.clientHeight / 2) / this.zoom];
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

  key(level, tu, tv) {
    const slice = this.index[this.axis] >> level;
    return [this.step, this.porosity ?? '-', this.axis, level, slice, tu, tv].join('/');
  }

  requestDraw() {
    if (this.drawPending || !this.canvas) return;
    this.drawPending = true;
    requestAnimationFrame(() => {
      this.drawPending = false;
      this.draw();
    });
  }

  /// Visible tiles of a level: [tu, tv, x, y, size] with screen position and size.
  visibleTiles(level) {
    const scale = 1 << level;
    const extent = TILE * scale;
    const width = this.canvas.clientWidth;
    const height = this.canvas.clientHeight;
    const [u0, v0] = this.screenToVoxel(0, 0);
    const [u1, v1] = this.screenToVoxel(width, height);
    const [du, dv] = this.planeDims();
    const tiles = [];
    for (let tv = Math.max(0, Math.floor(v0 / extent)); tv * extent < Math.min(v1, dv); tv += 1) {
      for (let tu = Math.max(0, Math.floor(u0 / extent)); tu * extent < Math.min(u1, du); tu += 1) {
        const x = (tu * extent - this.center[0]) * this.zoom + width / 2;
        const y = (tv * extent - this.center[1]) * this.zoom + height / 2;
        tiles.push([tu, tv, x, y, extent * this.zoom]);
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
    // Coarser tiles first as placeholders, then the wanted level on top.
    for (let l = Math.min(level + 3, this.maxLevel()); l >= level; l -= 1) {
      for (const [tu, tv, x, y, size] of this.visibleTiles(l)) {
        const tile = this.tiles.get(this.key(l, tu, tv));
        if (!tile) continue;
        tile.used = performance.now();
        context.drawImage(this.renderTile(tile), x, y, size, size);
      }
    }
    // Outline of the volume.
    const [du, dv] = this.planeDims();
    context.strokeStyle = 'rgba(255,255,255,0.25)';
    context.strokeRect((0 - this.center[0]) * this.zoom + width / 2,
      (0 - this.center[1]) * this.zoom + height / 2, du * this.zoom, dv * this.zoom);

    this.queue = this.visibleTiles(level)
      .filter(([tu, tv]) => {
        const key = this.key(level, tu, tv);
        return !this.tiles.has(key) && !this.loading.has(key) && !this.failed.has(key);
      })
      .map(([tu, tv, x, y, size]) => ({ level, tu, tv,
        distance: Math.hypot(x + size / 2 - width / 2, y + size / 2 - height / 2) }))
      .sort((a, b) => a.distance - b.distance);
    this.pump();
  }

  pump() {
    while (this.active < MAX_REQUESTS && this.queue.length) {
      const { level, tu, tv } = this.queue.shift();
      this.fetchTile(level, tu, tv);
    }
  }

  async fetchTile(level, tu, tv) {
    const key = this.key(level, tu, tv);
    this.loading.add(key);
    this.active += 1;
    const params = new URLSearchParams({
      step: this.step, axis: this.axis, index: this.index[this.axis], level,
      u: tu * TILE, v: tv * TILE, size: TILE,
    });
    if (this.porosity !== null) params.set('porosity', this.porosity);
    try {
      const response = await fetch('api/tile?' + params);
      if (!response.ok) throw new Error((await response.json()).error);
      const buffer = await response.arrayBuffer();
      const count = TILE * TILE;
      this.tiles.set(key, {
        data: new Float32Array(buffer, 0, count),
        overlay: new Uint8Array(buffer, count * 4, count),
        canvas: null, rendered: null, used: performance.now(),
      });
      this.evict();
    } catch (error) {
      this.failed.add(key);
      console.warn('Tile', key, error);
    } finally {
      this.loading.delete(key);
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

  renderTile(tile) {
    const state = [this.window?.[0], this.window?.[1], this.showOverlay].join('/');
    if (tile.rendered === state) return tile.canvas;
    if (!tile.canvas) {
      tile.canvas = document.createElement('canvas');
      tile.canvas.width = TILE;
      tile.canvas.height = TILE;
    }
    const context = tile.canvas.getContext('2d');
    const image = context.createImageData(TILE, TILE);
    const pixels = image.data;
    const [low, high] = this.window ?? [0, 1];
    const scale = 255 / (high - low);
    for (let i = 0; i < tile.data.length; i += 1) {
      const grey = Math.min(Math.max((tile.data[i] - low) * scale, 0), 255);
      let r = grey;
      let g = grey;
      let b = grey;
      if (this.showOverlay) {
        const overlay = tile.overlay[i];
        // Tinted, so the grey values stay readable: pores red, zones yellow.
        if (overlay === 1) {
          r = 0.35 * grey + 165; g = 0.35 * grey + 25; b = 0.35 * grey + 25;
        } else if (overlay === 2) {
          r = 0.5 * grey + 125; g = 0.5 * grey + 100; b = 0.4 * grey;
        }
      }
      const p = i * 4;
      pixels[p] = r;
      pixels[p + 1] = g;
      pixels[p + 2] = b;
      pixels[p + 3] = 255;
    }
    context.putImageData(image, 0, 0);
    tile.rendered = state;
    return tile.canvas;
  }
}

window.SliceViewer = SliceViewer;
window.SLICE_AXIS_NAMES = AXIS_NAMES;
