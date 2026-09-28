// Unit tests of the transfer function of the 3D view (resources/ui/transfer.js), run with node.
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const transfer = require(path.join(__dirname, '../../resources/ui/transfer.js'));

const { applyColorMap, colorMapAt, hexColor, histogramPeaks, lookupTable, otsuThreshold,
  parseHexColor, suggestTransfers, transferPreset } = transfer;

// The table interpolates linearly between points and holds the end values outside them.
{
  const table = lookupTable([
    { x: 0.2, a: 0, color: [0, 0, 0] },
    { x: 0.6, a: 1, color: [200, 100, 0] },
  ]);
  assert.equal(table.length, 256 * 4);
  const at = (i) => Array.from(table.slice(i * 4, i * 4 + 4));
  assert.deepEqual(at(0), [0, 0, 0, 0]);
  assert.deepEqual(at(255), [200, 100, 0, 255]);
  const middle = Math.round(0.4 * 255);   // halfway between the points
  const [r, g, , a] = at(middle);
  assert.ok(Math.abs(r - 100) <= 1 && Math.abs(g - 50) <= 1 && Math.abs(a - 128) <= 1,
    `halfway ${at(middle)}`);
}

// Several segments: every entry lies between its neighbouring points.
{
  const points = transferPreset('rand', 0.5);
  const table = lookupTable(points);
  const peak = points.reduce((best, p) => (p.a > best.a ? p : best));
  // The peak falls between two entries, so the highest entry lies within one entry's rise of it.
  const highest = Math.max(...table.filter((_, i) => i % 4 === 3));
  assert.ok(highest <= Math.round(peak.a * 255) && highest >= Math.round(peak.a * 255) - 7,
    `peak ${highest}`);
  assert.equal(table[0 * 4 + 3], 0, 'air stays transparent');
  assert.equal(table[255 * 4 + 3], 0, 'solid material transparent in the edge preset');
}

// Every preset is sorted, inside 0..1 and transparent in the air below the threshold.
for (const name of ['durchsicht', 'dichte', 'rand', 'massiv']) {
  for (const threshold of [0.02, 0.3, 0.5, 0.98]) {
    const points = transferPreset(name, threshold);
    points.forEach((p, i) => {
      assert.ok(p.x >= 0 && p.x <= 1 && p.a >= 0 && p.a <= 1, `${name} point ${i}`);
      if (i > 0) assert.ok(p.x >= points[i - 1].x, `${name} sorted`);
      p.color.forEach((c) => assert.ok(Number.isInteger(c) && c >= 0 && c <= 255));
    });
    const table = lookupTable(points);
    // Every preset rises from zero within 0.12 below the (clamped) threshold.
    const clamped = Math.min(Math.max(threshold, 0.05), 0.95);
    const air = Math.floor(Math.max(clamped - 0.13, 0) * 255);
    assert.equal(table[air * 4 + 3], 0, `${name} at ${threshold}: air is transparent`);
  }
}

// Colour maps hit their end stops and spread over the points.
assert.deepEqual(colorMapAt('viridis', 0), [68, 1, 84]);
assert.deepEqual(colorMapAt('viridis', 1), [253, 231, 37]);
assert.deepEqual(colorMapAt('unbekannt', 1), [235, 235, 235]);
{
  const points = applyColorMap([{ x: 0.3, a: 0, color: [0, 0, 0] },
    { x: 0.9, a: 1, color: [0, 0, 0] }], 'grau');
  assert.deepEqual(points[0].color, [40, 40, 40]);
  assert.deepEqual(points[1].color, [235, 235, 235]);
}

// Colours survive the round trip through the colour input.
assert.equal(hexColor([255, 8, 160]), '#ff08a0');
assert.deepEqual(parseHexColor('#ff08a0'), [255, 8, 160]);

// A synthetic histogram: air, a broad material peak and optionally a denser inclusion phase.
function histogramOf(peaks) {
  return Array.from({ length: 256 }, (_, i) => Math.round(peaks.reduce((sum, [centre, sigma, count]) =>
    sum + count * Math.exp(-0.5 * ((i - centre) / sigma) ** 2), 0)));
}
const alphaAt = (points, bin) => lookupTable(points)[bin * 4 + 3];

{
  const histogram = histogramOf([[20, 4, 1e6], [180, 8, 3e5], [235, 3, 800]]);
  const split = otsuThreshold(histogram);
  assert.ok(split > 30 && split < 170, `otsu ${split}`);
  const peaks = histogramPeaks(histogram).map((p) => p.bin);
  for (const expected of [20, 180, 235]) {
    assert.ok(peaks.some((bin) => Math.abs(bin - expected) <= 2), `peak ${expected} in ${peaks}`);
  }

  const suggestions = suggestTransfers(histogram);
  assert.deepEqual(suggestions.map((s) => s.id), ['dichte', 'locker', 'phasen']);
  for (const suggestion of suggestions) {
    suggestion.points.forEach((p, i) => {
      assert.ok(p.x >= 0 && p.x <= 1 && p.a >= 0 && p.a <= 1, `${suggestion.id} point ${i}`);
      if (i > 0) assert.ok(p.x > suggestion.points[i - 1].x, `${suggestion.id} sorted`);
    });
    assert.ok(suggestion.name && suggestion.description);
    assert.ok(transfer.COLOR_MAPS[suggestion.colorMap], suggestion.colorMap);
    assert.equal(alphaAt(suggestion.points, 20), 0, `${suggestion.id}: air is transparent`);
  }
  const [density, loose, phase] = suggestions;
  // The density rendering covers the material peak, the phase rendering stands out at 235.
  assert.ok(density.range[0] * 255 < 180 && density.range[1] * 255 > 180);
  assert.ok(alphaAt(density.points, 180) > 0);
  assert.ok(alphaAt(phase.points, 235) > 200, 'inclusion opaque');
  assert.ok(alphaAt(phase.points, 180) < 30, 'material faint');
  // Lower densities than the material are highlighted, the material itself is faint.
  const middle = Math.round((loose.range[0] + loose.range[1]) / 2 * 255);
  assert.ok(alphaAt(loose.points, middle) > 100 && alphaAt(loose.points, 180) < 40);
}

// Without a further phase the third suggestion shows the part as a body.
{
  const suggestions = suggestTransfers(histogramOf([[30, 5, 5e5], [200, 10, 2e5]]));
  assert.deepEqual(suggestions.map((s) => s.id), ['dichte', 'locker', 'koerper']);
  assert.equal(alphaAt(suggestions[2].points, 250), 255);
}
assert.deepEqual(suggestTransfers(new Array(256).fill(0)), []);

console.log('transfer.js ok');
