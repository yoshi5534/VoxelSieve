// Brick streaming of the 3D view (review step 6.2, ADR 0035). The coarse level of the volume is
// one texture; finer levels come as bricks of 32^3 voxels with two voxels around them (enough for
// the gradients of the shading, which read a voxel further than filtering), loaded where
// the picture needs them into slots of a fixed atlas texture whose size the VRAM budget sets.
// A page table holds one texel per brick of every finer level, and per 8^3 cell of the coarse
// level, stacked along z: where the brick lies in the atlas, whether it is resident, absent
// (removed air only) or not loaded, whether it holds pores or zones, and its largest grey value,
// so the ray caster can skip what cannot show. Which bricks the picture needs comes back from the
// GPU: a small feedback pass writes per pixel the brick it wanted finer, or one it used.
'use strict';

const BRICK = 32;
const APRON = 2;
const SLOT = BRICK + 2 * APRON;
const COARSE_CELL = 8;
const STATE_NONE = 0;
const STATE_RESIDENT = 1;
const STATE_ABSENT = 2;
const STATE_COARSE = 3;
const FLAG_PORES = 4;
/// Feedback passes a resident brick may go unused before its slot can be taken.
const KEEP_EPOCHS = 10;

/// The page table: the bricks of levels 0 to coarseLevel - 1 and the cells of the coarse level,
/// for a volume texture that spans `extent` level-0 voxels per axis.
function pageLayout(coarseLevel, extent) {
  const levels = [];
  let depth = 0;
  let width = 1;
  let height = 1;
  for (let level = 0; level <= coarseLevel; level += 1) {
    const size = (level < coarseLevel ? BRICK : COARSE_CELL) * 2 ** level;
    const grid = extent.map((e) => Math.max(1, Math.ceil(e / size)));
    levels.push({ level, size, grid, origin: [0, 0, depth] });
    depth += grid[2];
    width = Math.max(width, grid[0]);
    height = Math.max(height, grid[1]);
  }
  return { coarseLevel, levels, dims: [width, height, depth] };
}

/// Slots per axis of an atlas within `budgetBytes`, for `bytesPerVoxel` bytes of textures per
/// voxel and 3D textures of at most `max3d` voxels per axis. Slot numbers fit 16 bits.
function atlasLayout(budgetBytes, bytesPerVoxel, max3d) {
  const perAxis = Math.max(1, Math.floor(max3d / SLOT));
  const count = Math.min(65535, Math.max(8, Math.floor(budgetBytes / (SLOT ** 3 * bytesPerVoxel))));
  const a = Math.max(1, Math.min(perAxis, Math.floor(Math.cbrt(count))));
  const z = Math.max(1, Math.min(perAxis, Math.floor(count / (a * a))));
  return [a, a, z];
}

/// A brick as the server sends it (`dims` voxels from `origin`, in level voxels, clipped to the
/// level) padded to a slot by repeating the edge voxels, with its largest grey value and whether
/// it holds pores or zones (overlay classes 1 and 2).
function padBrick(grey, overlay, origin, dims, brick) {
  const outGrey = new Uint8Array(SLOT ** 3);
  const outOverlay = new Uint8Array(SLOT ** 3);
  const first = brick.map((b) => b * BRICK - APRON);
  const source = (k, i) => Math.min(Math.max(first[k] + i, origin[k]), origin[k] + dims[k] - 1) -
    origin[k];
  const xs = Array.from({ length: SLOT }, (_, i) => source(0, i));
  let maxGrey = 0;
  let pores = false;
  for (let z = 0; z < SLOT; z += 1) {
    const sz = source(2, z);
    for (let y = 0; y < SLOT; y += 1) {
      const row = (sz * dims[1] + source(1, y)) * dims[0];
      const out = (z * SLOT + y) * SLOT;
      for (let x = 0; x < SLOT; x += 1) {
        const g = grey[row + xs[x]];
        const o = overlay[row + xs[x]];
        outGrey[out + x] = g;
        outOverlay[out + x] = o;
        if (g > maxGrey) maxGrey = g;
        if (o === 1 || o === 2) pores = true;
      }
    }
  }
  return { grey: outGrey, overlay: outOverlay, maxGrey, pores };
}

/// Largest grey value and pores or zones per coarse cell, over the cell and one voxel around it
/// (what linear filtering reads), as page texels of the coarse level.
function coarseCells(grey, overlay, dims, grid) {
  const out = new Uint8Array(4 * grid[0] * grid[1] * grid[2]);
  for (let cz = 0; cz < grid[2]; cz += 1) {
    for (let cy = 0; cy < grid[1]; cy += 1) {
      for (let cx = 0; cx < grid[0]; cx += 1) {
        const lo = [cx, cy, cz].map((c) => Math.max(0, c * COARSE_CELL - 1));
        const hi = [cx, cy, cz].map((c, k) => Math.min(dims[k], (c + 1) * COARSE_CELL + 1));
        let maxGrey = 0;
        let pores = false;
        for (let z = lo[2]; z < hi[2]; z += 1) {
          for (let y = lo[1]; y < hi[1]; y += 1) {
            const row = (z * dims[1] + y) * dims[0];
            for (let x = lo[0]; x < hi[0]; x += 1) {
              maxGrey = Math.max(maxGrey, grey[row + x]);
              pores = pores || overlay[row + x] === 1 || overlay[row + x] === 2;
            }
          }
        }
        const i = 4 * ((cz * grid[1] + cy) * grid[0] + cx);
        out[i + 2] = STATE_COARSE | (pores ? FLAG_PORES : 0);
        out[i + 3] = maxGrey;
      }
    }
  }
  return out;
}

class BrickPager {
  constructor(layout, slots) {
    this.layout = layout;
    this.slotCount = slots;
    const [w, h, d] = layout.dims;
    this.pages = new Uint8Array(4 * w * h * d);
    this.free = Array.from({ length: slots }, (_, i) => slots - 1 - i);
    this.resident = new Map();  // page texel -> {slot, used, level, brick}
    this.inFlight = new Set();  // page texels requested and not placed yet
    this.wanted = [];           // [{level, brick, key, count}] from the last feedback
    this.epoch = 0;
    this.full = false;          // the last brick found no slot
    this.dirty = null;          // [first, last] z of the page texels changed
  }

  /// Texel of brick `brick` at `level` in the page table, or -1 outside the level's grid.
  key(level, brick) {
    const { grid, origin } = this.layout.levels[level];
    if (brick.some((b, k) => b < 0 || b >= grid[k])) return -1;
    const [w, h] = this.layout.dims;
    return ((origin[2] + brick[2]) * h + brick[1]) * w + brick[0];
  }

  markDirty(key) {
    const [w, h] = this.layout.dims;
    const z = Math.floor(key / (w * h));
    this.dirty = this.dirty ? [Math.min(this.dirty[0], z), Math.max(this.dirty[1], z)] : [z, z];
  }

  /// Marks the bricks of a level that the dataset does not hold (bit i of `bits` for brick i of
  /// a grid of `grid` bricks, x fastest) as absent. Bricks beyond that grid are absent too.
  setPresence(level, grid, bits) {
    const own = this.layout.levels[level].grid;
    for (let z = 0; z < own[2]; z += 1) {
      for (let y = 0; y < own[1]; y += 1) {
        for (let x = 0; x < own[0]; x += 1) {
          const inside = x < grid[0] && y < grid[1] && z < grid[2];
          const i = inside ? (z * grid[1] + y) * grid[0] + x : -1;
          const present = inside && (bits[i >> 3] >> (i & 7)) & 1;
          const key = this.key(level, [x, y, z]);
          if (!present && !this.resident.has(key)) {
            this.pages[4 * key + 2] = STATE_ABSENT;
            this.markDirty(key);
          }
        }
      }
    }
  }

  /// Sets the texels of the coarse level (see coarseCells).
  setCoarse(texels) {
    const { grid } = this.layout.levels[this.layout.coarseLevel];
    for (let z = 0; z < grid[2]; z += 1) {
      for (let y = 0; y < grid[1]; y += 1) {
        const from = 4 * ((z * grid[1] + y) * grid[0]);
        const key = this.key(this.layout.coarseLevel, [0, y, z]);
        this.pages.set(texels.subarray(from, from + 4 * grid[0]), 4 * key);
        this.markDirty(key);
      }
    }
  }

  state(key) {
    return this.pages[4 * key + 2] & 3;
  }

  /// Takes the pixels of a feedback pass (four 32-bit values each: level + 1, plus 128 for a
  /// brick used rather than wanted, then the brick) and returns how many bricks are wanted.
  feedback(pixels) {
    this.epoch += 1;
    const counts = new Map();
    for (let i = 0; i < pixels.length; i += 4) {
      const code = pixels[i];
      if (code === 0) continue;
      const level = (code & 127) - 1;
      if (level < 0 || level >= this.layout.coarseLevel) continue;
      const brick = [pixels[i + 1], pixels[i + 2], pixels[i + 3]];
      const key = this.key(level, brick);
      if (key < 0) continue;
      if (code & 128) {
        const entry = this.resident.get(key);
        if (entry) entry.used = this.epoch;
      } else if (this.state(key) === STATE_NONE && !this.inFlight.has(key)) {
        const entry = counts.get(key);
        if (entry) entry.count += 1;
        else counts.set(key, { level, brick, key, count: 1 });
      }
    }
    // Coarser first: one of them improves more of the picture; then the most pixels.
    this.wanted = [...counts.values()].sort((a, b) => b.level - a.level || b.count - a.count);
    return this.wanted.length;
  }

  /// The next `count` bricks to request.
  takeRequests(count) {
    const out = [];
    if (this.full) return out;
    while (out.length < count && this.wanted.length) {
      const next = this.wanted.shift();
      if (this.state(next.key) !== STATE_NONE || this.inFlight.has(next.key)) continue;
      this.inFlight.add(next.key);
      out.push(next);
    }
    return out;
  }

  /// A request that came to nothing; the brick may be asked for again.
  failed(key) {
    this.inFlight.delete(key);
  }

  /// Gives a loaded brick a slot, taking the one of the brick unused longest if none is free.
  /// Returns the slot, or -1 when every slot holds a brick in use (the budget is full).
  place(key, level, brick, maxGrey, pores) {
    this.inFlight.delete(key);
    if (this.state(key) !== STATE_NONE) return -1;
    let slot = this.free.pop();
    if (slot === undefined) slot = this.evict();
    this.full = slot < 0;
    if (slot < 0) return -1;
    this.resident.set(key, { slot, used: this.epoch, level, brick });
    this.pages.set([slot & 255, slot >> 8, STATE_RESIDENT | (pores ? FLAG_PORES : 0), maxGrey],
      4 * key);
    this.markDirty(key);
    return slot;
  }

  evict() {
    let oldest = null;
    let oldestKey = -1;
    for (const [key, entry] of this.resident) {
      if (entry.used < this.epoch - KEEP_EPOCHS && (!oldest || entry.used < oldest.used)) {
        oldest = entry;
        oldestKey = key;
      }
    }
    if (!oldest) return -1;
    this.resident.delete(oldestKey);
    this.pages.fill(0, 4 * oldestKey, 4 * oldestKey + 4);
    this.markDirty(oldestKey);
    return oldest.slot;
  }

  /// Drops every resident brick, for example when the bricks no longer match what is shown.
  clear() {
    for (const key of this.resident.keys()) {
      this.pages.fill(0, 4 * key, 4 * key + 4);
      this.markDirty(key);
    }
    this.resident.clear();
    this.inFlight.clear();
    this.wanted = [];
    this.free = Array.from({ length: this.slotCount }, (_, i) => this.slotCount - 1 - i);
    this.full = false;
  }

  /// The finest level resident, or the coarse level when no brick is.
  finestLevel() {
    let finest = this.layout.coarseLevel;
    for (const entry of this.resident.values()) finest = Math.min(finest, entry.level);
    return finest;
  }
}

const BRICKS = {
  BRICK, APRON, SLOT, COARSE_CELL, STATE_NONE, STATE_RESIDENT, STATE_ABSENT, STATE_COARSE,
  FLAG_PORES, KEEP_EPOCHS, pageLayout, atlasLayout, padBrick, coarseCells, BrickPager,
};
if (typeof window !== 'undefined') window.BRICKS = BRICKS;
if (typeof module !== 'undefined') module.exports = BRICKS;
