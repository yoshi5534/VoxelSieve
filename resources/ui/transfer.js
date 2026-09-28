// Transfer function of the 3D view of VoxelSieve Studio: control points that map the grey value
// of the volume preview (0..1 over its window) to colour and opacity, a histogram editor for them
// and a few presets and colour maps. Runs in the browser and, for its tests, in node.
'use strict';

/// Colour maps as stops [t, r, g, b] with t in 0..1 and colours in 0..255.
const COLOR_MAPS = {
  grau: { name: 'Grau', stops: [[0, 40, 40, 40], [1, 235, 235, 235]] },
  stahl: { name: 'Stahl', stops: [[0, 70, 90, 120], [0.5, 150, 170, 195], [1, 225, 232, 240]] },
  viridis: { name: 'Viridis', stops: [[0, 68, 1, 84], [0.25, 59, 82, 139], [0.5, 33, 145, 140],
    [0.75, 94, 201, 98], [1, 253, 231, 37]] },
  heiss: { name: 'Glut', stops: [[0, 90, 0, 0], [0.4, 220, 40, 0], [0.75, 255, 190, 0],
    [1, 255, 255, 220]] },
  kupfer: { name: 'Kupfer', stops: [[0, 60, 30, 15], [0.6, 200, 120, 70], [1, 255, 205, 150]] },
};

/// Colour of a colour map at t in 0..1.
function colorMapAt(name, t) {
  const stops = (COLOR_MAPS[name] ?? COLOR_MAPS.grau).stops;
  const x = Math.min(Math.max(t, 0), 1);
  let i = 0;
  while (i < stops.length - 2 && x > stops[i + 1][0]) i += 1;
  const [t0, ...c0] = stops[i];
  const [t1, ...c1] = stops[i + 1];
  const f = t1 > t0 ? (x - t0) / (t1 - t0) : 0;
  return c0.map((c, k) => Math.round(c + (c1[k] - c) * f));
}

/// 256 RGBA entries (bytes) interpolated linearly between the control points, which are sorted by
/// x. Left of the first and right of the last point their values hold.
function lookupTable(points) {
  const table = new Uint8Array(256 * 4);
  let segment = 0;
  for (let i = 0; i < 256; i += 1) {
    const x = i / 255;
    while (segment < points.length - 2 && x > points[segment + 1].x) segment += 1;
    const p = points[segment];
    const q = points[Math.min(segment + 1, points.length - 1)];
    const f = q.x > p.x ? Math.min(Math.max((x - p.x) / (q.x - p.x), 0), 1) : (x > p.x ? 1 : 0);
    for (let k = 0; k < 3; k += 1) {
      table[i * 4 + k] = Math.round(p.color[k] + (q.color[k] - p.color[k]) * f);
    }
    table[i * 4 + 3] = Math.round(255 * (p.a + (q.a - p.a) * f));
  }
  return table;
}

/// Recolours the points with a colour map spread over their x range.
function applyColorMap(points, name) {
  const low = points[0].x;
  const high = points[points.length - 1].x;
  return points.map((p) => ({ ...p,
    color: colorMapAt(name, high > low ? (p.x - low) / (high - low) : 0) }));
}

const TRANSFER_PRESETS = {
  durchsicht: 'Bauteil durchscheinend',
  dichte: 'Dichte farbig',
  rand: 'Randschichten und Porenwände',
  massiv: 'Bauteil massiv',
};

/// Control points of a preset for a volume whose air/material threshold is at `threshold`.
function transferPreset(name, threshold) {
  const t = Math.min(Math.max(threshold, 0.05), 0.95);
  const point = (x, a) => ({ x: Math.min(Math.max(x, 0), 1), a, color: [0, 0, 0] });
  switch (name) {
    case 'dichte':
      return applyColorMap([point(0, 0), point(t - 0.04, 0), point(t + 0.04, 0.12),
        point(1, 0.3)], 'viridis');
    case 'rand':
      return applyColorMap([point(0, 0), point(t - 0.12, 0), point(t, 0.8), point(t + 0.12, 0),
        point(1, 0)], 'kupfer');
    case 'massiv':
      return applyColorMap([point(0, 0), point(t - 0.02, 0), point(t + 0.02, 1), point(1, 1)],
        'stahl');
    default:
      return applyColorMap([point(0, 0), point(t - 0.04, 0), point(t + 0.06, 0.12),
        point(1, 0.2)], 'stahl');
  }
}

/// Histogram smoothed with a box filter of 2 * radius + 1 bins.
function smoothHistogram(histogram, radius) {
  return histogram.map((_, i) => {
    let sum = 0;
    let count = 0;
    for (let k = Math.max(i - radius, 0); k <= Math.min(i + radius, histogram.length - 1); k += 1) {
      sum += histogram[k];
      count += 1;
    }
    return sum / count;
  });
}

/// Otsu threshold: the bin that best separates the histogram into two classes.
function otsuThreshold(histogram) {
  const total = histogram.reduce((a, b) => a + b, 0);
  const sum = histogram.reduce((a, count, i) => a + i * count, 0);
  let below = 0;
  let belowSum = 0;
  let best = 0;
  let bestVariance = -1;
  for (let t = 0; t < histogram.length - 1; t += 1) {
    below += histogram[t];
    belowSum += t * histogram[t];
    const above = total - below;
    if (below === 0 || above === 0) continue;
    const difference = belowSum / below - (sum - belowSum) / above;
    const variance = below * above * difference * difference;
    if (variance > bestVariance) {
      bestVariance = variance;
      best = t;
    }
  }
  return best + 0.5;
}

/// Prominent peaks of the histogram (on a log scale, so a small second material next to a
/// large air peak still counts), strongest first: {bin, prominence}.
function histogramPeaks(histogram) {
  const log = smoothHistogram(histogram, 4).map((count) => Math.log1p(count));
  const peaks = [];
  for (let i = 0; i < log.length; i += 1) {
    let top = true;
    for (let k = Math.max(i - 6, 0); k <= Math.min(i + 6, log.length - 1) && top; k += 1) {
      top = log[k] < log[i] || (log[k] === log[i] && k >= i);
    }
    if (!top || log[i] <= 0) continue;
    // Prominence: height above the higher of the two valleys towards the next higher peak.
    let left = log[i];
    for (let k = i - 1; k >= 0 && log[k] <= log[i]; k -= 1) left = Math.min(left, log[k]);
    let right = log[i];
    for (let k = i + 1; k < log.length && log[k] <= log[i]; k += 1) right = Math.min(right, log[k]);
    peaks.push({ bin: i, prominence: log[i] - Math.max(left, right) });
  }
  return peaks.filter((p) => p.prominence >= 0.7).sort((a, b) => b.prominence - a.prominence);
}

/// Two or three renderings that bring out interesting ranges of the histogram: the density
/// spread of the material, lower densities inside the part (pores, loosened structure) and
/// further phases (inclusions, a second material) or else the part as a solid body. Each is
/// {id, name, description, points, colorMap, shading, range: [low, high] in 0..1}.
function suggestTransfers(histogram) {
  const bins = histogram.length;
  const total = histogram.reduce((a, b) => a + b, 0);
  if (total === 0) return [];
  const split = otsuThreshold(histogram);
  const smooth = smoothHistogram(histogram, 2);
  const argmax = (from, to) => {
    let best = from;
    for (let i = from; i < to; i += 1) if (smooth[i] > smooth[best]) best = i;
    return best;
  };
  const air = argmax(0, Math.ceil(split));
  const material = argmax(Math.ceil(split), bins);
  // Half width at half maximum of the material peak, at least two bins.
  let low = material;
  while (low > split && smooth[low] > smooth[material] / 2) low -= 1;
  let high = material;
  while (high < bins - 1 && smooth[high] > smooth[material] / 2) high += 1;
  const spread = Math.max((high - low) / 2, 2);
  const threshold = (air + material) / 2;
  const x = (bin) => Math.min(Math.max(bin / (bins - 1), 0), 1);
  const point = (bin, a) => ({ x: x(bin), a, color: [0, 0, 0] });
  const sorted = (points) => points.sort((p, q) => p.x - q.x)
    .filter((p, i, all) => i === 0 || p.x > all[i - 1].x);

  const suggestions = [];
  const densityLow = Math.max(material - 3 * spread, threshold);
  const densityHigh = Math.min(material + 3 * spread, bins - 1);
  suggestions.push({
    id: 'dichte',
    name: 'Dichteverteilung',
    description: 'Das Material farbig nach Dichte',
    // The colour map spans only the material range, so its whole scale shows density.
    points: (() => {
      const range = applyColorMap(sorted([point(densityLow, 0.06), point(material, 0.1),
        point(densityHigh, 0.16)]), 'viridis');
      const first = range[0].color;
      return sorted([{ ...point(0, 0), color: first }, { ...point(densityLow - 2, 0), color: first },
        ...range, { ...point(bins - 1, 0.16), color: range[range.length - 1].color }]);
    })(),
    colorMap: 'viridis',
    shading: false,
    range: [x(densityLow), x(densityHigh)],
  });

  // Lower densities between the surface and the material peak: pores and loosened structure,
  // with the material itself faint.
  const looseLow = threshold + 0.25 * (material - threshold);
  const looseHigh = Math.max(material - 2 * spread, looseLow + 2);
  if (looseHigh < material) {
    const faint = [150, 160, 172];
    suggestions.push({
      id: 'locker',
      name: 'Geringere Dichte',
      description: 'Bereiche unter der Materialdichte rot, das Material blass',
      points: sorted([point(0, 0), point(threshold, 0),
        { ...point(looseLow, 0.55), color: [230, 60, 30] },
        { ...point(looseHigh, 0.55), color: [255, 170, 40] },
        { ...point(material - spread, 0.08), color: faint },
        { ...point(bins - 1, 0.08), color: faint }]).map((p) => (p.a === 0 ? { ...p,
        color: [230, 60, 30] } : p)),
      colorMap: 'heiss',
      shading: true,
      range: [x(looseLow), x(looseHigh)],
    });
  }

  // Further phases: prominent peaks inside the part (above the surface threshold) away from the
  // material peak.
  const phases = histogramPeaks(histogram).filter((p) => p.bin > threshold + 2 * spread &&
    Math.abs(p.bin - material) > 3 * spread).slice(0, 2);
  if (phases.length > 0) {
    const colors = [[40, 200, 255], [255, 80, 200]];
    const points = [point(0, 0), point(threshold, 0), point(threshold + 2, 0.04),
      point(bins - 1, 0.04)];
    for (const [i, phase] of phases.entries()) {
      const width = Math.max(spread, 3);
      points.push(point(phase.bin - width, 0.04), { ...point(phase.bin, 0.9), color: colors[i] },
        point(phase.bin + width, 0.04));
    }
    const faint = sorted(points).map((p) => (p.a >= 0.9 ? p : { ...p, color: [150, 160, 172] }));
    suggestions.push({
      id: 'phasen',
      name: phases.length > 1 ? 'Weitere Phasen' : 'Weitere Phase',
      description: 'Eigene Maxima im Histogramm farbig, etwa Einschlüsse',
      points: faint,
      colorMap: 'grau',
      shading: true,
      range: [x(phases[0].bin - spread), x(phases[0].bin + spread)],
    });
  } else {
    suggestions.push({
      id: 'koerper',
      name: 'Bauteil',
      description: 'Das Bauteil als beleuchteter Körper',
      points: applyColorMap(sorted([point(0, 0), point(threshold - 2, 0), point(threshold + 2, 1),
        point(bins - 1, 1)]), 'stahl'),
      colorMap: 'stahl',
      shading: true,
      range: [x(threshold), 1],
    });
  }
  return suggestions.slice(0, 3);
}

function hexColor(color) {
  return '#' + color.map((c) => c.toString(16).padStart(2, '0')).join('');
}

function parseHexColor(hex) {
  return [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16));
}

/// Histogram editor on a 2D canvas. In 'curve' mode the control points of the transfer function
/// are dragged (click adds a point, double click or Delete removes it); in 'threshold' mode the
/// air/material threshold of the surface view.
class TransferEditor {
  constructor(canvas) {
    this.canvas = canvas;
    this.histogram = new Array(256).fill(0);
    this.points = transferPreset('durchsicht', 0.5);
    this.threshold = 0.5;
    this.mode = 'curve';
    this.selected = -1;
    this.hover = null;
    this.label = (x) => (x * 255).toFixed(0);
    this.onChange = () => {};   // points or threshold changed
    this.onSelect = () => {};   // selection or hover changed
    this.attach();
  }

  setHistogram(histogram) {
    this.histogram = histogram;
    this.draw();
  }

  setPoints(points) {
    this.points = points.map((p) => ({ ...p, color: [...p.color] }));
    this.selected = -1;
    this.changed();
  }

  setMode(mode) {
    this.mode = mode;
    this.draw();
    this.onSelect();
  }

  changed() {
    this.draw();
    this.onChange();
    this.onSelect();
  }

  // Layout: plot area inside the canvas, in CSS pixels.
  area() {
    const width = this.canvas.clientWidth;
    const height = this.canvas.clientHeight;
    return { left: 8, top: 8, width: Math.max(width - 16, 1), height: Math.max(height - 32, 1) };
  }

  toCanvas(x, a) {
    const r = this.area();
    return [r.left + x * r.width, r.top + (1 - a) * r.height];
  }

  fromEvent(event) {
    const box = this.canvas.getBoundingClientRect();
    const r = this.area();
    const x = (event.clientX - box.left - r.left) / r.width;
    const a = 1 - (event.clientY - box.top - r.top) / r.height;
    return { x: Math.min(Math.max(x, 0), 1), a: Math.min(Math.max(a, 0), 1),
      px: event.clientX - box.left, py: event.clientY - box.top };
  }

  pointAt(px, py) {
    let best = -1;
    let bestDistance = 9;
    this.points.forEach((p, i) => {
      const [cx, cy] = this.toCanvas(p.x, p.a);
      const distance = Math.hypot(cx - px, cy - py);
      if (distance < bestDistance) {
        best = i;
        bestDistance = distance;
      }
    });
    return best;
  }

  removeSelected() {
    if (this.selected < 0 || this.points.length <= 2) return;
    this.points.splice(this.selected, 1);
    this.selected = -1;
    this.changed();
  }

  attach() {
    const canvas = this.canvas;
    let drag = null;
    canvas.addEventListener('pointerdown', (event) => {
      if (event.button !== 0) return;
      const at = this.fromEvent(event);
      canvas.setPointerCapture(event.pointerId);
      canvas.focus();
      if (this.mode === 'threshold') {
        this.threshold = at.x;
        drag = { threshold: true };
        this.changed();
        return;
      }
      let index = this.pointAt(at.px, at.py);
      if (index < 0) {
        // A new point on the curve's colour at that grey value.
        const table = lookupTable(this.points);
        const i = Math.round(at.x * 255) * 4;
        const point = { x: at.x, a: at.a, color: [table[i], table[i + 1], table[i + 2]] };
        index = this.points.findIndex((p) => p.x > at.x);
        if (index < 0) index = this.points.length;
        this.points.splice(index, 0, point);
      }
      this.selected = index;
      drag = { index };
      this.changed();
    });
    canvas.addEventListener('pointermove', (event) => {
      const at = this.fromEvent(event);
      this.hover = at.x;
      if (drag?.threshold) {
        this.threshold = at.x;
        this.changed();
      } else if (drag) {
        const i = drag.index;
        const low = i > 0 ? this.points[i - 1].x : 0;
        const high = i < this.points.length - 1 ? this.points[i + 1].x : 1;
        this.points[i].x = Math.min(Math.max(at.x, low), high);
        this.points[i].a = at.a;
        this.changed();
      } else {
        this.draw();
        this.onSelect();
      }
    });
    canvas.addEventListener('pointerup', () => { drag = null; });
    canvas.addEventListener('pointerleave', () => {
      this.hover = null;
      this.draw();
      this.onSelect();
    });
    canvas.addEventListener('dblclick', (event) => {
      const at = this.fromEvent(event);
      const index = this.pointAt(at.px, at.py);
      if (index >= 0) {
        this.selected = index;
        this.removeSelected();
      }
    });
    canvas.addEventListener('keydown', (event) => {
      if (event.key === 'Delete' || event.key === 'Backspace') {
        event.preventDefault();
        this.removeSelected();
      }
    });
    new ResizeObserver(() => this.draw()).observe(canvas);
  }

  draw() {
    const canvas = this.canvas;
    const ratio = window.devicePixelRatio || 1;
    const width = Math.round(canvas.clientWidth * ratio);
    const height = Math.round(canvas.clientHeight * ratio);
    if (width === 0 || height === 0) return;
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    const g = canvas.getContext('2d');
    g.setTransform(ratio, 0, 0, ratio, 0, 0);
    g.fillStyle = '#1b1b1b';
    g.fillRect(0, 0, canvas.clientWidth, canvas.clientHeight);
    const r = this.area();

    // Histogram, logarithmic.
    const peak = Math.log1p(Math.max(...this.histogram, 1));
    const bar = r.width / 256;
    g.fillStyle = '#4a4a4a';
    this.histogram.forEach((count, i) => {
      const h = (Math.log1p(count) / peak) * r.height;
      g.fillRect(r.left + i * bar, r.top + r.height - h, Math.ceil(bar), h);
    });

    if (this.mode === 'curve') {
      // Area under the curve in the colour of the transfer function.
      const table = lookupTable(this.points);
      for (let i = 0; i < 256; i += 1) {
        const a = table[i * 4 + 3] / 255;
        if (a <= 0) continue;
        g.fillStyle = `rgba(${table[i * 4]},${table[i * 4 + 1]},${table[i * 4 + 2]},0.75)`;
        g.fillRect(r.left + i * bar, r.top + (1 - a) * r.height, Math.ceil(bar), a * r.height);
      }
      g.strokeStyle = '#eee';
      g.lineWidth = 1.5;
      g.beginPath();
      const first = this.toCanvas(0, this.points[0].a);
      g.moveTo(...first);
      this.points.forEach((p) => g.lineTo(...this.toCanvas(p.x, p.a)));
      g.lineTo(...this.toCanvas(1, this.points[this.points.length - 1].a));
      g.stroke();
      this.points.forEach((p, i) => {
        const [cx, cy] = this.toCanvas(p.x, p.a);
        g.beginPath();
        g.arc(cx, cy, i === this.selected ? 6 : 4.5, 0, 2 * Math.PI);
        g.fillStyle = hexColor(p.color);
        g.fill();
        g.lineWidth = i === this.selected ? 2.5 : 1.5;
        g.strokeStyle = i === this.selected ? '#4fa3ff' : '#fff';
        g.stroke();
      });
    } else {
      const [tx] = this.toCanvas(this.threshold, 0);
      g.fillStyle = 'rgba(200, 210, 220, 0.18)';
      g.fillRect(tx, r.top, r.left + r.width - tx, r.height);
      g.strokeStyle = '#ff9f1c';
      g.lineWidth = 2;
      g.beginPath();
      g.moveTo(tx, r.top);
      g.lineTo(tx, r.top + r.height);
      g.stroke();
    }

    // Grey value axis.
    g.fillStyle = '#999';
    g.font = '11px system-ui, sans-serif';
    g.textBaseline = 'bottom';
    const bottom = canvas.clientHeight - 3;
    g.textAlign = 'left';
    g.fillText(this.label(0), r.left, bottom);
    g.textAlign = 'right';
    g.fillText(this.label(1), r.left + r.width, bottom);
    const marker = this.mode === 'threshold' ? this.threshold : this.hover;
    if (marker !== null) {
      g.textAlign = 'center';
      g.fillStyle = '#ddd';
      g.fillText(this.label(marker), Math.min(Math.max(r.left + marker * r.width, 60),
        r.left + r.width - 60), bottom);
    }
  }
}

if (typeof window !== 'undefined') {
  Object.assign(window, { COLOR_MAPS, TRANSFER_PRESETS, TransferEditor, applyColorMap, colorMapAt,
    hexColor, lookupTable, parseHexColor, suggestTransfers, transferPreset });
}
if (typeof module !== 'undefined') {
  module.exports = { COLOR_MAPS, applyColorMap, colorMapAt, hexColor, histogramPeaks, lookupTable,
    otsuThreshold, parseHexColor, suggestTransfers, transferPreset };
}
