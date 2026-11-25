// Unit tests of the lighting and looks of the 3D view (resources/ui/lighting.js), run with node.
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const { DEFAULT_LIGHTING, LIGHTING_PRESETS, LOOKS, lightDirections, lightingPreset, lookSettings,
  normalizeLighting } = require(path.join(__dirname, '../../resources/ui/lighting.js'));
const { orbitCamera } = require(path.join(__dirname, '../../resources/ui/orbit.js'));
const { COLOR_MAPS } = require(path.join(__dirname, '../../resources/ui/transfer.js'));

const near = (a, b, what, tolerance = 1e-9) => assert.ok(Math.abs(a - b) <= tolerance,
  `${what}: ${a} is not ${b}`);
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

// The default is the headlight the view always had: light from the camera, a little to the right
// and above (the old shader took -view + 0.4 up + 0.3 right).
{
  const camera = orbitCamera({ yaw: 0.8, pitch: 0.45, distance: 2.2, target: [0, 0, 0] });
  const { key } = lightDirections(DEFAULT_LIGHTING, camera);
  const old = [0, 1, 2].map((k) => -camera.forward[k] + 0.4 * camera.up[k] + 0.3 * camera.right[k]);
  const length = Math.hypot(...old);
  near(dot(key, old.map((c) => c / length)), 1, 'headlight as before', 2e-3);
  near(Math.hypot(...key), 1, 'unit vector');
}

// Following the camera, the light turns with it; fixed, it stays.
{
  const a = orbitCamera({ yaw: 0, pitch: 0.3, distance: 2, target: [0, 0, 0] });
  const b = orbitCamera({ yaw: 1.2, pitch: 0.3, distance: 2, target: [0, 0, 0] });
  const follow = { ...DEFAULT_LIGHTING, azimuth: 40, elevation: 20 };
  near(dot(lightDirections(follow, a).key, a.forward), dot(lightDirections(follow, b).key,
    b.forward), 'the same angle to the view');
  const fixed = { ...follow, follow: false };
  assert.deepEqual(lightDirections(fixed, a).key, lightDirections(fixed, b).key);
  // Fixed: azimuth about z from +x, elevation above the xy plane.
  const up = lightDirections({ ...fixed, azimuth: 0, elevation: 90 }, a).key;
  near(up[2], 1, 'straight above');
  const x = lightDirections({ ...fixed, azimuth: 0, elevation: 0 }, a).key;
  near(x[0], 1, 'along +x');
  // The fill light comes from the other side of the view.
  const { key, fill } = lightDirections(follow, a);
  assert.ok(dot(key, a.right) > 0 && dot(fill, a.right) < 0, 'fill from the other side');
}

// Presets: complete and inside the ranges; unknown names give the headlight.
{
  for (const name of Object.keys(LIGHTING_PRESETS)) {
    const lighting = lightingPreset(name);
    assert.equal(lighting.preset, name);
    assert.deepEqual(normalizeLighting(lighting), lighting, name + ' within its ranges');
  }
  assert.equal(lightingPreset('bogus').preset, 'headlight');
  // A hand-edited lighting is clamped and completed.
  const odd = normalizeLighting({ ambient: 7, shadows: -1, shininess: 'x', azimuth: 500 });
  assert.equal(odd.ambient, 1);
  assert.equal(odd.shadows, 0);
  assert.equal(odd.shininess, DEFAULT_LIGHTING.shininess);
  assert.equal(odd.azimuth, 180);
  assert.equal(odd.intensity, DEFAULT_LIGHTING.intensity);
  assert.deepEqual(normalizeLighting(undefined), DEFAULT_LIGHTING);
}

// Looks name presets, colour maps and lightings that exist, and set the transfer function.
{
  for (const name of Object.keys(LOOKS)) {
    const { volume, background, transfer } = lookSettings(name, 0.4);
    assert.ok([0, 1].includes(volume.mode), name + ' mode');
    assert.ok(volume.lighting.preset in LIGHTING_PRESETS, name + ' lighting');
    assert.ok(transfer.colorMap in COLOR_MAPS, name + ' colour map');
    assert.equal(transfer.colorSource, 'map');
    assert.ok(transfer.points.length >= 2 && transfer.points[0].a === 0, name + ' curve');
    assert.equal(typeof background, 'string');
    assert.equal(volume.look, name);
  }
}
console.log('lighting tests passed');
