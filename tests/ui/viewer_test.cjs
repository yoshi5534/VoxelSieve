// Unit tests of the slice viewer's drawing of other objects (resources/ui/viewer.js), run with node.
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const { cutsToScreen, insideWindow, layerPixels, slicePixels } = require(
  path.join(__dirname, '../../resources/ui/viewer.js'));

// A cut through the centre of voxel 10 lies in the middle of its pixels: voxel u spans the screen
// from u to u + 1. Voxels twice as long along v are drawn twice as high.
{
  const view = { center: [10, 20], zoom: 4, stretch: [1, 2], width: 200, height: 100 };
  const screen = cutsToScreen(new Float32Array([10, 20, 12, 19.5]), view);
  assert.deepEqual([...screen], [102, 54, 110, 50]);
}

// The window of a volume comes only from its material, not from the air kept around it.
{
  const data = new Float32Array(700).map((_, i) => (i < 350 ? -1000 : 100 + (i % 50)));
  const inside = new Uint8Array(700).map((_, i) => (i < 350 ? 1 : 2));
  const [low, high] = insideWindow([{ data, inside }]);
  assert.ok(low >= 100 && high <= 149 && high > low, `${low} ${high}`);
  assert.equal(insideWindow([{ data, inside: new Uint8Array(700).fill(1) }]), null);
}

// Blended: the object's colour, darker for lower values, transparent outside its material.
// Checkerboard: grey values of everything kept in every other square of 32 pixels.
{
  const size = 64;
  const data = new Float32Array(size * size).fill(10);
  const inside = new Uint8Array(size * size).fill(2);
  inside[1] = 1;  // air kept near the part
  inside[size * 40 + 1] = 1;
  inside[5] = 0;  // removed air
  const out = new Uint8ClampedArray(size * size * 4);
  layerPixels(data, inside, size, [200, 100, 50], [0, 10], 5, 'blend', 0.5, out);
  assert.deepEqual([...out.slice(0, 4)], [200, 100, 50, 128]);
  assert.equal(out[5 * 4 + 3], 0);
  // The edge: half covered at the threshold, nothing a tenth of the window below it.
  data[2] = 5;
  data[3] = 4;
  layerPixels(data, inside, size, [200, 100, 50], [0, 10], 5, 'blend', 0.5, out);
  assert.equal(out[2 * 4 + 3], 64);
  assert.equal(out[3 * 4 + 3], 0);
  data[2] = data[3] = 10;
  layerPixels(data, inside, size, [200, 100, 50], [0, 20], 5, 'checker', 0.5, out);
  const pixel = (x, y) => [...out.slice((y * size + x) * 4, (y * size + x) * 4 + 4)];
  assert.equal(pixel(0, 0)[3], 0);              // the slice shows through
  assert.deepEqual(pixel(32, 0), [128, 128, 128, 255]);
  assert.deepEqual(pixel(0, 40), [128, 128, 128, 255]);
  assert.equal(pixel(1, 40)[3], 255);
  assert.equal(pixel(40, 40)[3], 0);
}

// Materials in their colour over the grey value, pores tinted red; a hidden material and the whole
// overlay switched off leave the grey value.
{
  const data = new Float32Array([0, 100, 100, 100, 100]);
  const overlay = new Uint8Array([0, 0, 1, 17, 18]);
  const out = new Uint8ClampedArray(5 * 4);
  const pixel = (i) => [...out.slice(i * 4, i * 4 + 4)];
  slicePixels(data, overlay, [0, 100], [], out);
  assert.deepEqual(pixel(0), [0, 0, 0, 255]);
  assert.deepEqual(pixel(1), [255, 255, 255, 255]);
  assert.ok(pixel(2)[0] > pixel(2)[1]);
  assert.deepEqual(pixel(3), [160, 200, 226, 255]);  // material 1: half white, half its colour
  assert.deepEqual(pixel(4), [242, 190, 144, 255]);  // material 2
  slicePixels(data, overlay, [0, 100], [2], out);
  assert.deepEqual(pixel(3), [160, 200, 226, 255]);
  assert.deepEqual(pixel(4), [255, 255, 255, 255]);
  slicePixels(data, overlay, [0, 100], null, out);
  assert.deepEqual(pixel(2), [255, 255, 255, 255]);
  assert.deepEqual(pixel(3), [255, 255, 255, 255]);
}
