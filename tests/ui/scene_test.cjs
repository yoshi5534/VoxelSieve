// Unit tests of the geometry of the object scene (resources/ui/scene.js), run with node.
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const scene = require(path.join(__dirname, '../../resources/ui/scene.js'));

const { applyPose, boxCorners, intersectMesh, invertPose, pointBounds, sceneBounds } = scene;

const near = (a, b, tolerance = 1e-9) =>
  assert.ok(a.every((v, i) => Math.abs(v - b[i]) <= tolerance), `${a} != ${b}`);

// 90 degrees about z, then a shift: the inverse brings every point back.
const pose = [0, -1, 0, 5, 1, 0, 0, -2, 0, 0, 1, 3, 0, 0, 0, 1];
{
  near(applyPose(pose, [1, 0, 0]), [5, -1, 3]);
  const inverse = invertPose(pose);
  for (const p of [[0, 0, 0], [1, 2, 3], [-4, 0.5, 7]]) near(applyPose(inverse, applyPose(pose, p)), p);
}

// Bounds of points and of placed, scaled objects.
{
  const bounds = pointBounds(new Float32Array([0, 0, 0, 2, 1, 4, -1, 3, 1]));
  assert.deepEqual(bounds, { min: [-1, 0, 0], max: [2, 3, 4] });
  assert.equal(boxCorners(bounds).length, 8);
  // Voxels of 0.5 mm, the box turned about z and shifted.
  const placed = sceneBounds([{ bounds, scale: [0.5, 0.5, 0.5], pose }]);
  near(placed.min, [3.5, -2.5, 3]);
  near(placed.max, [5, -1, 5]);
}

// The nearest hit of a ray with a mesh: two squares at z = 1 and z = 3, seen from below.
{
  const points = new Float32Array([0, 0, 1, 2, 0, 1, 0, 2, 1, 2, 2, 1,
    0, 0, 3, 2, 0, 3, 0, 2, 3, 2, 2, 3]);
  const indices = new Uint32Array([0, 1, 2, 1, 3, 2, 4, 5, 6, 5, 7, 6]);
  const hit = intersectMesh([0.5, 1.5, -1], [0, 0, 2], points, indices);
  assert.ok(hit);
  near(hit.point, [0.5, 1.5, 1]);
  assert.ok(Math.abs(hit.t - 1) < 1e-12);
  assert.equal(intersectMesh([3, 1, -1], [0, 0, 1], points, indices), null);  // beside both
  assert.equal(intersectMesh([1, 1, 4], [0, 0, 1], points, indices), null);   // pointing away
}

console.log('scene.js ok');
