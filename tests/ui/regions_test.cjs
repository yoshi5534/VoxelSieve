// Unit tests of the regions of a scan in the UI (resources/ui/regions.js), run with node.
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const regions = require(path.join(__dirname, '../../resources/ui/regions.js'));

const { cutTriangles, describeSolid, parseStl, regionsOf, solidFromDrag, solidsTriangles,
  solidTriangles } = regions;

/// Segments u0 v0 u1 v1 as a list of in-plane points.
const points = (segments) => {
  const out = [];
  for (let i = 0; i < segments.length; i += 2) out.push([segments[i], segments[i + 1]]);
  return out;
};

// A box of 2 x 4 x 6 mm cut at z = 3 mm is its rectangle 2 x 4 mm; voxels of 0.5 mm make it
// 4 x 8 voxels from voxel (2, 2).
{
  const box = solidTriangles({ box_mm: [[1, 1, 0], [2 + 1, 4 + 1, 6]] });
  assert.equal(box.length, 12 * 9);
  const cut = points(cutTriangles(box, 2, 3, [0.5, 0.5, 0.5], [0, 1]));
  assert.ok(cut.length >= 8);
  for (const [u, v] of cut) {
    assert.ok(Math.abs(u - 2) < 1e-6 || Math.abs(u - 6) < 1e-6 || Math.abs(v - 2) < 1e-6 ||
      Math.abs(v - 10) < 1e-6, `${u} ${v} is not on the rectangle`);
    assert.ok(u >= 2 - 1e-6 && u <= 6 + 1e-6 && v >= 2 - 1e-6 && v <= 10 + 1e-6);
  }
  // Outside the box, nothing.
  assert.equal(cutTriangles(box, 2, 7, [0.5, 0.5, 0.5], [0, 1]).length, 0);
}

// A sphere cut through its centre is a circle of its radius; off centre a smaller one.
{
  const sphere = solidTriangles({ sphere: { center_mm: [5, 5, 5], radius_mm: 2 } });
  const ring = (z) => points(cutTriangles(sphere, 2, z, [1, 1, 1], [0, 1]))
    .map(([u, v]) => Math.hypot(u - 5, v - 5));
  for (const r of ring(5)) assert.ok(r > 1.9 && r <= 2.0001, `radius ${r}`);
  const expected = Math.sqrt(4 - 1.5 * 1.5);
  for (const r of ring(6.5)) assert.ok(Math.abs(r - expected) < 0.15, `radius ${r} at z 6.5`);
}

// A cylinder along x seen along x is a circle, along z a rectangle.
{
  const cylinder = solidTriangles({ cylinder: { from_mm: [0, 5, 5], to_mm: [10, 5, 5],
    radius_mm: 1 } });
  for (const [u, v] of points(cutTriangles(cylinder, 0, 4, [1, 1, 1], [1, 2]))) {
    assert.ok(Math.abs(Math.hypot(u - 5, v - 5) - 1) < 0.01);
  }
  const side = points(cutTriangles(cylinder, 2, 5, [1, 1, 1], [0, 1]));
  assert.ok(side.length > 0);
  for (const [u, v] of side) assert.ok(u >= -1e-6 && u <= 10 + 1e-6 && Math.abs(v - 5) <= 1.0001);
}

// Several solids in one array; a mesh needs its STL.
{
  const both = solidsTriangles([{ box_mm: [[0, 0, 0], [1, 1, 1]] },
    { box_mm: [[2, 2, 2], [3, 3, 3]] }]);
  assert.equal(both.length, 2 * 12 * 9);
  assert.equal(solidsTriangles([{ mesh_object: 'o2' }]), null);
}

// A binary STL as writeStl writes it.
{
  const buffer = new ArrayBuffer(84 + 50);
  const view = new DataView(buffer);
  view.setUint32(80, 1, true);
  [0, 0, 0, 1, 0, 0, 0, 1, 0].forEach((value, k) => view.setFloat32(84 + 12 + 4 * k, value, true));
  assert.deepEqual([...parseStl(buffer)], [0, 0, 0, 1, 0, 0, 0, 1, 0]);
  assert.equal(parseStl(new ArrayBuffer(10)).length, 0);
}

// Drags in a slice normal to z of a scan of 10 x 10 x 20 voxels of 0.5 mm: positions are
// continuous voxel coordinates whose voxel centres lie half a voxel into each voxel.
{
  const info = { pitch: [0.5, 0.5, 0.5], dims: [10, 10, 20] };
  const box = solidFromDrag('box', 2, 7, [2.5, 3.5], [6.5, 1.5], info);
  // From voxel centre 2 to 6 along x, 1 to 3 along y, through the whole depth.
  assert.deepEqual(box, { box_mm: [[1, 0.5, -0.25], [3, 1.5, 9.75]] });
  const sphere = solidFromDrag('sphere', 2, 7, [4.5, 4.5], [7.5, 8.5], info);
  assert.deepEqual(sphere, { sphere: { center_mm: [2, 2, 3.5], radius_mm: 2.5 } });
  const cylinder = solidFromDrag('cylinder', 0, 3, [4.5, 4.5], [4.5, 8.5], info);
  assert.deepEqual(cylinder, { cylinder: { from_mm: [-0.25, 2, 2], to_mm: [4.75, 2, 2],
    radius_mm: 2 } });
  // A click is no solid.
  assert.equal(solidFromDrag('box', 2, 7, [2.5, 3.5], [2.6, 3.6], info), null);
  assert.equal(solidFromDrag('sphere', 2, 7, [2.5, 3.5], [2.6, 3.6], info), null);
}

// Descriptions for the list of solids.
{
  assert.equal(describeSolid({ box_mm: [[0, 0, 0], [1, 2.5, 3]] }), 'Box 1 × 2.5 × 3 mm');
  assert.equal(describeSolid({ sphere: { center_mm: [0, 0, 0], radius_mm: 1.5 } }),
    'Sphere Ø 3 mm');
  assert.equal(describeSolid({ mesh_object: 'o2' }, () => 'Part model'), 'Inside Part model');
}

// The regions of a scan from its steps: the latest per name, removed ones gone, other objects'
// regions left out.
{
  const step = (id, params, object = 'o1') => ({ id, operation: 'define_region', status: 'done',
    object, params, summary: { description: 'region ' + id } });
  const list = regionsOf([
    step(1, { name: 'A', include: [{ box_mm: [[0, 0, 0], [1, 1, 1]] }] }),
    step(2, { name: 'B', include: [{ sphere: { center_mm: [0, 0, 0], radius_mm: 1 } }] }),
    step(3, { name: 'A', include: [{ box_mm: [[0, 0, 0], [2, 2, 2]] }], color: '#ff0000' }),
    step(4, { name: 'B', remove: true }),
    step(5, { name: 'C', include: [] }, 'o2'),
    { id: 6, operation: 'porosity', status: 'done', object: 'o1', params: {} },
  ], 'o1');
  assert.equal(list.length, 1);
  assert.equal(list[0].name, 'A');
  assert.equal(list[0].step, 3);
  assert.equal(list[0].color, '#ff0000');
  assert.deepEqual(regions.regionParameter(list[0]),
    { name: 'A', include: [{ box_mm: [[0, 0, 0], [2, 2, 2]] }], exclude: [] });
}

console.log('regions tests passed');
