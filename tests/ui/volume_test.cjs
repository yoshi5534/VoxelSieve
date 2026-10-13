// Unit tests of the 3D view's material visibility (resources/ui/volume.js), run with node.
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const { visibilityChannels } = require(path.join(__dirname, '../../resources/ui/volume.js'));

// Air, a pore, a zone, material 1 and material 2 with material 2 left out: the surface may lie
// in the pore, the zone and material 1; only material 2 is seen through in every view.
{
  const overlay = new Uint8Array([0, 1, 2, 17, 18]);
  assert.deepEqual([...visibilityChannels(overlay, [2])],
    [0, 255, 255, 255, 255, 255, 255, 255, 0, 0]);
  assert.deepEqual([...visibilityChannels(overlay, [])],
    [0, 255, 255, 255, 255, 255, 255, 255, 255, 255]);
}
