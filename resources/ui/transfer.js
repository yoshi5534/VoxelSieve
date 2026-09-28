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
    hexColor, lookupTable, parseHexColor, transferPreset });
}
if (typeof module !== 'undefined') {
  module.exports = { COLOR_MAPS, applyColorMap, colorMapAt, hexColor, lookupTable, parseHexColor,
    transferPreset };
}
