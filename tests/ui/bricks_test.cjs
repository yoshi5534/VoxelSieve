// Unit tests of the brick streaming of the 3D view (resources/ui/bricks.js), run with node.
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const B = require(path.join(__dirname, '../../resources/ui/bricks.js'));
const { firstOpaque } = (() => {
  global.BRICKS = B;
  global.MATERIAL_COLORS = [];
  return require(path.join(__dirname, '../../resources/ui/volume.js'));
})();

// The page table stacks the levels along z: bricks of 32 voxels per level, cells of 8 voxels
// of the coarse level. A 256^3 texture at level 2 spans 1024 level-0 voxels.
{
  const layout = B.pageLayout(2, [1024, 1024, 512]);
  assert.equal(layout.levels.length, 3);
  assert.deepEqual(layout.levels[0].grid, [32, 32, 16]);
  assert.deepEqual(layout.levels[1].grid, [16, 16, 8]);
  assert.deepEqual(layout.levels[2].grid, [32, 32, 16]);  // 8-voxel cells of the coarse level
  assert.deepEqual(layout.levels.map((l) => l.origin[2]), [0, 16, 24]);
  assert.deepEqual(layout.dims, [32, 32, 40]);
}

// The atlas holds the budget's worth of slots, at most what the texture size allows.
{
  const slots = B.atlasLayout(512 * 2 ** 20, 2, 2048);
  const count = slots[0] * slots[1] * slots[2];
  assert.ok(count * B.SLOT ** 3 * 2 <= 512 * 2 ** 20);
  assert.ok(count > 0.8 * 512 * 2 ** 20 / (B.SLOT ** 3 * 2));
  assert.ok(slots.every((s) => s * B.SLOT <= 2048));
  const small = B.atlasLayout(16 * 2 ** 30, 4, 256);
  assert.ok(small.every((s) => s * B.SLOT <= 256));
  assert.ok(B.atlasLayout(1, 2, 2048).reduce((a, b) => a * b) >= 1);
}

// A brick at the volume border is padded by repeating its edge voxels; its largest grey value
// and pores come along.
{
  // Brick (1, 0, 0) of a level 40 voxels wide: x from 31 to 40 (clipped), y and z from 0 to 33.
  const dims = [9, 33, 33];
  const origin = [31, 0, 0];
  const grey = new Uint8Array(dims[0] * dims[1] * dims[2]);
  const overlay = new Uint8Array(grey.length);
  grey.forEach((_, i) => { grey[i] = i % dims[0]; });  // grey = x - 31
  overlay[5] = 1;
  const padded = B.padBrick(grey, overlay, origin, dims, [1, 0, 0]);
  assert.equal(padded.grey.length, B.SLOT ** 3);
  assert.equal(B.APRON, 2);
  assert.equal(padded.grey[0], 0);           // x = 30: before what was sent, x = 31 repeated
  assert.equal(padded.grey[1], 0);           // x = 31: the apron
  assert.equal(padded.grey[2], 1);           // x = 32: the first voxel of the brick
  assert.equal(padded.grey[9], 8);           // x = 39: the last of the level
  assert.equal(padded.grey[B.SLOT - 1], 8);  // past the level: the edge repeated
  // y = -2 and z = -2 repeat y = 0 and z = 0.
  assert.equal(padded.grey[B.SLOT * B.SLOT + B.SLOT + 3], padded.grey[3]);
  assert.equal(padded.maxGrey, 8);
  assert.equal(padded.pores, true);
  assert.equal(B.padBrick(grey, new Uint8Array(grey.length).fill(17), origin, dims, [1, 0, 0]).pores,
    false);
}

// The coarse cells take the largest grey value over the cell and one voxel around it.
{
  const dims = [16, 8, 8];
  const grey = new Uint8Array(16 * 8 * 8);
  grey[8] = 200;  // x = 8: the first voxel of cell 1, next to cell 0
  const overlay = new Uint8Array(grey.length);
  overlay[15] = 2;  // a zone in cell 1
  const cells = B.coarseCells(grey, overlay, dims, [2, 1, 1]);
  assert.equal(cells[2] & 3, B.STATE_COARSE);
  assert.equal(cells[3], 200);           // cell 0 reads one voxel into cell 1
  assert.equal(cells[4 + 3], 200);
  assert.equal(cells[2] & B.FLAG_PORES, 0);
  assert.equal(cells[4 + 2] & B.FLAG_PORES, B.FLAG_PORES);
}

// The pager: absent bricks, feedback, slots and eviction of the brick unused longest.
{
  const layout = B.pageLayout(1, [128, 64, 64]);  // level 0: 4 x 2 x 2 bricks
  const pager = new B.BrickPager(layout, 2);
  // Bricks 0, 1 and 4 of level 0 hold voxels, the others are absent.
  pager.setPresence(0, [4, 2, 2], new Uint8Array([0b10011, 0]));
  assert.equal(pager.state(pager.key(0, [0, 0, 0])), B.STATE_NONE);
  assert.equal(pager.state(pager.key(0, [2, 0, 0])), B.STATE_ABSENT);
  assert.equal(pager.state(pager.key(0, [0, 1, 1])), B.STATE_ABSENT);
  assert.equal(pager.key(0, [4, 0, 0]), -1);
  // Feedback: brick 0 wanted by two pixels, brick 1 by one, an absent one ignored.
  const pixels = new Uint32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 0, 0, 0, 0]);
  assert.equal(pager.feedback(pixels), 2);
  const requests = pager.takeRequests(6);
  assert.deepEqual(requests.map((r) => r.brick), [[0, 0, 0], [1, 0, 0]]);
  assert.equal(pager.takeRequests(6).length, 0);  // in flight, not wanted again
  const k0 = pager.key(0, [0, 0, 0]);
  const k1 = pager.key(0, [1, 0, 0]);
  assert.equal(pager.place(k0, 0, [0, 0, 0], 180, false), 0);
  assert.equal(pager.place(k1, 0, [1, 0, 0], 90, true), 1);
  assert.deepEqual([...pager.pages.subarray(4 * k1, 4 * k1 + 4)],
    [1, 0, B.STATE_RESIDENT | B.FLAG_PORES, 90]);
  assert.ok(pager.dirty);
  // A third brick finds no slot while both are in use.
  const k2 = pager.key(0, [0, 1, 0]);
  assert.equal(pager.place(k2, 0, [0, 1, 0], 10, false), -1);
  assert.equal(pager.full, true);
  assert.equal(pager.takeRequests(6).length, 0);
  // Brick 1 stays in use, brick 0 does not: after a while its slot goes to the new brick.
  const touch1 = new Uint32Array([129, 1, 0, 0]);
  for (let i = 0; i <= B.KEEP_EPOCHS; i += 1) pager.feedback(touch1);
  pager.full = false;
  assert.equal(pager.place(k2, 0, [0, 1, 0], 10, false), 0);
  assert.equal(pager.state(k0), B.STATE_NONE);
  assert.equal(pager.state(k1), B.STATE_RESIDENT);
  assert.equal(pager.finestLevel(), 0);
  pager.clear();
  assert.equal(pager.resident.size, 0);
  assert.equal(pager.state(k1), B.STATE_NONE);
  assert.equal(pager.finestLevel(), 1);
}

// The transfer function shows nothing below its first opaque grey value.
{
  const table = new Uint8Array(1024);
  assert.equal(firstOpaque(table), 2);
  table[4 * 51 + 3] = 1;
  assert.equal(firstOpaque(table), 51 / 255);
  assert.equal(firstOpaque(null), 2);
}

console.log('bricks tests passed');
