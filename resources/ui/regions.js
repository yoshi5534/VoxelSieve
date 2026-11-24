'use strict';

// Regions of a scan (ADR 0041): solids in mm in the dataset's coordinates (voxel index times
// voxel size). The views draw them from triangles: the slice view the lines where they cut the
// slice, the 3D view the translucent solids. Boxes, spheres and cylinders are tessellated here,
// as region.cpp does; meshes come from the region.stl the define_region step wrote.

/// Triangles of a box from corner `low` to `high`: 9 floats per triangle.
function boxTriangles(low, high) {
  const corner = (i) => [i & 1 ? high[0] : low[0], i & 2 ? high[1] : low[1],
    i & 4 ? high[2] : low[2]];
  // Two triangles per face, by the corners of the face.
  const faces = [[0, 2, 3, 1], [4, 5, 7, 6], [0, 1, 5, 4], [2, 6, 7, 3], [0, 4, 6, 2],
    [1, 3, 7, 5]];
  const out = [];
  for (const [a, b, c, d] of faces) {
    out.push(...corner(a), ...corner(b), ...corner(c), ...corner(a), ...corner(c), ...corner(d));
  }
  return new Float32Array(out);
}

function sphereTriangles(center, radius, rings = 16, segments = 32) {
  const point = (ring, segment) => {
    const theta = Math.PI * ring / rings;
    const phi = 2 * Math.PI * segment / segments;
    return [center[0] + radius * Math.sin(theta) * Math.cos(phi),
      center[1] + radius * Math.sin(theta) * Math.sin(phi), center[2] + radius * Math.cos(theta)];
  };
  const out = [];
  for (let r = 0; r < rings; r += 1) {
    for (let s = 0; s < segments; s += 1) {
      const a = point(r, s);
      const b = point(r + 1, s);
      const c = point(r + 1, s + 1);
      const d = point(r, s + 1);
      if (r > 0) out.push(...a, ...b, ...d);
      if (r < rings - 1) out.push(...b, ...c, ...d);
    }
  }
  return new Float32Array(out);
}

function cylinderTriangles(from, to, radius, segments = 32) {
  const axis = [to[0] - from[0], to[1] - from[1], to[2] - from[2]];
  const length = Math.hypot(...axis) || 1;
  const n = axis.map((c) => c / length);
  // Two directions across the axis.
  const helper = Math.abs(n[0]) < 0.9 ? [1, 0, 0] : [0, 1, 0];
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0]];
  const u = cross(n, helper);
  const ul = Math.hypot(...u);
  const e1 = u.map((c) => c / ul);
  const e2 = cross(n, e1);
  const rim = (base, s) => {
    const phi = 2 * Math.PI * s / segments;
    return [0, 1, 2].map((k) => base[k] + radius * (Math.cos(phi) * e1[k] + Math.sin(phi) * e2[k]));
  };
  const out = [];
  for (let s = 0; s < segments; s += 1) {
    const a = rim(from, s);
    const b = rim(from, s + 1);
    const c = rim(to, s + 1);
    const d = rim(to, s);
    out.push(...a, ...b, ...c, ...a, ...c, ...d, ...from, ...b, ...a, ...to, ...d, ...c);
  }
  return new Float32Array(out);
}

/// Triangles of a solid of a region; null for a mesh, whose triangles come from the step.
function solidTriangles(solid) {
  if (solid.box_mm) return boxTriangles(solid.box_mm[0], solid.box_mm[1]);
  if (solid.sphere) return sphereTriangles(solid.sphere.center_mm, solid.sphere.radius_mm);
  if (solid.cylinder) {
    return cylinderTriangles(solid.cylinder.from_mm, solid.cylinder.to_mm, solid.cylinder.radius_mm);
  }
  return null;
}

/// Triangles of several solids in one array; null when one of them is a mesh.
function solidsTriangles(solids) {
  const parts = solids.map(solidTriangles);
  if (parts.some((part) => part === null)) return null;
  const out = new Float32Array(parts.reduce((sum, part) => sum + part.length, 0));
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.length;
  }
  return out;
}

/// Triangles of a binary STL file.
function parseStl(buffer) {
  const view = new DataView(buffer);
  if (buffer.byteLength < 84) return new Float32Array(0);
  const count = view.getUint32(80, true);
  const out = new Float32Array(count * 9);
  for (let t = 0; t < count && 84 + t * 50 + 50 <= buffer.byteLength; t += 1) {
    for (let k = 0; k < 9; k += 1) out[t * 9 + k] = view.getFloat32(84 + t * 50 + 12 + k * 4, true);
  }
  return out;
}

/// Where triangles (mm) cut the plane `axis` = `planeMm`: segments u0 v0 u1 v1 in level-0 voxel
/// indices of the in-plane axes `u`, `v`, the cut lines of the slice view.
function cutTriangles(triangles, axis, planeMm, pitch, [u, v]) {
  const out = [];
  const point = [];
  for (let t = 0; t + 8 < triangles.length; t += 9) {
    point.length = 0;
    for (let e = 0; e < 3; e += 1) {
      const a = t + 3 * e;
      const b = t + 3 * ((e + 1) % 3);
      const da = triangles[a + axis] - planeMm;
      const db = triangles[b + axis] - planeMm;
      // Half-open, so a vertex on the plane counts for one of its edges only.
      if ((da < 0) === (db < 0)) continue;
      const f = da / (da - db);
      point.push((triangles[a + u] + f * (triangles[b + u] - triangles[a + u])) / pitch[u],
        (triangles[a + v] + f * (triangles[b + v] - triangles[a + v])) / pitch[v]);
    }
    if (point.length === 4) out.push(...point);
  }
  return new Float32Array(out);
}

/// The solid a drag in the slice view draws, from `start` to `end` (continuous level-0 voxel
/// coordinates of the in-plane axes as SliceViewer.screenToVoxel gives them) on the slice
/// `index` normal to `axis`: a box through the whole depth of the scan, a sphere around the start
/// or a cylinder around it along the slice normal through the whole depth. Null when too small.
function solidFromDrag(kind, axis, index, start, end, info) {
  const pitch = info.pitch;
  const [u, v] = [[1, 2], [0, 2], [0, 1]][axis];
  // Voxel centres lie on whole numbers; the screen draws voxel u from u to u + 1.
  const mm = (value, a) => (value - 0.5) * pitch[a];
  const depth = [-0.5 * pitch[axis], (info.dims[axis] - 0.5) * pitch[axis]];
  const round = (value) => Math.round(value * 1000) / 1000;
  const center = [0, 0, 0];
  center[u] = mm(start[0], u);
  center[v] = mm(start[1], v);
  center[axis] = index * pitch[axis];
  const radius = Math.hypot(mm(end[0], u) - center[u], mm(end[1], v) - center[v]);
  if (kind === 'box') {
    const low = [0, 0, 0];
    const high = [0, 0, 0];
    low[u] = Math.min(mm(start[0], u), mm(end[0], u));
    high[u] = Math.max(mm(start[0], u), mm(end[0], u));
    low[v] = Math.min(mm(start[1], v), mm(end[1], v));
    high[v] = Math.max(mm(start[1], v), mm(end[1], v));
    [low[axis], high[axis]] = depth;
    if (high[u] - low[u] < pitch[u] || high[v] - low[v] < pitch[v]) return null;
    return { box_mm: [low.map(round), high.map(round)] };
  }
  if (radius < Math.min(pitch[u], pitch[v])) return null;
  if (kind === 'sphere') return { sphere: { center_mm: center.map(round), radius_mm: round(radius) } };
  const from = [...center];
  const to = [...center];
  [from[axis], to[axis]] = depth;
  return { cylinder: { from_mm: from.map(round), to_mm: to.map(round), radius_mm: round(radius) } };
}

function formatMm(value) {
  return String(Math.round(value * 100) / 100);
}

/// "Box 10 x 5 x 2 mm", for the list of solids.
function describeSolid(solid, objectName = (id) => id) {
  if (solid.box_mm) {
    const [low, high] = solid.box_mm;
    return 'Box ' + [0, 1, 2].map((a) => formatMm(high[a] - low[a])).join(' × ') + ' mm';
  }
  if (solid.sphere) return 'Sphere Ø ' + formatMm(2 * solid.sphere.radius_mm) + ' mm';
  if (solid.cylinder) {
    const { from_mm: from, to_mm: to, radius_mm: radius } = solid.cylinder;
    const length = Math.hypot(to[0] - from[0], to[1] - from[1], to[2] - from[2]);
    return 'Cylinder Ø ' + formatMm(2 * radius) + ' mm, ' + formatMm(length) + ' mm long';
  }
  if (solid.mesh_object) return 'Inside ' + objectName(solid.mesh_object);
  if (solid.mesh) return 'Mesh ' + String(solid.mesh).split(/[\\/]/).pop();
  return 'Solid';
}

/// The regions of a scan object from the steps of the project: the latest done define_region
/// step per name, unless it removed the region. In the order they were first defined.
function regionsOf(steps, object) {
  const regions = new Map();
  for (const step of steps) {
    if (step.operation !== 'define_region' || step.status !== 'done') continue;
    if ((step.object ?? '') !== (object ?? '')) continue;
    const name = step.params.name;
    if (step.params.remove) {
      regions.delete(name);
      continue;
    }
    regions.set(name, {
      name, step: step.id, color: step.params.color ?? null,
      include: step.params.include ?? [], exclude: step.params.exclude ?? [],
      description: step.summary?.description ?? '', meshes: step.summary?.meshes ?? {},
    });
  }
  return [...regions.values()];
}

/// The `region` parameter of an analysis for a region.
function regionParameter(region) {
  return { name: region.name, include: region.include, exclude: region.exclude };
}

const REGION_COLORS = ['#2f80ed', '#27ae60', '#9b51e0', '#f2994a', '#00b8d9', '#eb5aa6'];

if (typeof window !== 'undefined') {
  Object.assign(window, { solidTriangles, solidsTriangles, parseStl, cutTriangles, solidFromDrag,
    describeSolid, regionsOf, regionParameter, REGION_COLORS });
}
if (typeof module !== 'undefined') {
  module.exports = { boxTriangles, sphereTriangles, cylinderTriangles, solidTriangles,
    solidsTriangles, parseStl, cutTriangles, solidFromDrag, describeSolid, regionsOf,
    regionParameter, REGION_COLORS };
}
