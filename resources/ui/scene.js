// 3D scene of VoxelSieve Studio (ADR 0018): every object of the project where it lies in the
// global coordinate system, each in its own colour. Meshes are drawn as their triangles, volumes
// as the display mesh of their latest surface step or, before one, as the outline of the box
// they fill. The global axes are drawn at the origin. A click on an object picks the point under
// the pointer (for point-pair alignment); picked points are drawn as dots.
'use strict';

/// Colours of the objects in the order they were created, as kObjectColors in src/studio.cpp.
const OBJECT_COLORS = [[196, 200, 207], [230, 150, 60], [90, 160, 230], [120, 190, 110],
  [200, 110, 170], [220, 200, 80]];

function objectColor(index) {
  return OBJECT_COLORS[index % OBJECT_COLORS.length];
}

// ---------------------------------------------------------------------------------------------
// Geometry, also used by tests/ui/scene_test.cjs

/// Applies a row-major 4x4 rigid transform to a point.
function applyPose(pose, p) {
  return [0, 1, 2].map((r) =>
    pose[4 * r] * p[0] + pose[4 * r + 1] * p[1] + pose[4 * r + 2] * p[2] + pose[4 * r + 3]);
}

/// The inverse of a row-major 4x4 rigid transform: the transposed rotation, turned translation.
function invertPose(pose) {
  const inverse = new Array(16).fill(0);
  for (let r = 0; r < 3; r += 1) {
    for (let c = 0; c < 3; c += 1) inverse[4 * r + c] = pose[4 * c + r];
    inverse[4 * r + 3] = -(pose[r] * pose[3] + pose[4 + r] * pose[7] + pose[8 + r] * pose[11]);
  }
  inverse[15] = 1;
  return inverse;
}

/// Bounds {min, max} of points (flat xyz array) in their own coordinates.
function pointBounds(points) {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < points.length; i += 3) {
    for (let k = 0; k < 3; k += 1) {
      min[k] = Math.min(min[k], points[i + k]);
      max[k] = Math.max(max[k], points[i + k]);
    }
  }
  return { min, max };
}

/// The eight corners of a box {min, max}.
function boxCorners({ min, max }) {
  const corners = [];
  for (let i = 0; i < 8; i += 1) {
    corners.push([i & 1 ? max[0] : min[0], i & 2 ? max[1] : min[1], i & 4 ? max[2] : min[2]]);
  }
  return corners;
}

/// Global bounds of objects [{pose, scale, bounds}]: their boxes, scaled and placed.
function sceneBounds(objects) {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (const object of objects) {
    for (const corner of boxCorners(object.bounds)) {
      const p = applyPose(object.pose, corner.map((c, k) => c * object.scale[k]));
      for (let k = 0; k < 3; k += 1) {
        min[k] = Math.min(min[k], p[k]);
        max[k] = Math.max(max[k], p[k]);
      }
    }
  }
  return { min, max };
}

/// Nearest hit of the ray origin + t * direction (t > 0) with indexed triangles, by the
/// Möller-Trumbore test: {t, point} or null. The ray and the points share one coordinate system.
function intersectMesh(origin, direction, points, indices) {
  let best = null;
  const [ox, oy, oz] = origin;
  const [dx, dy, dz] = direction;
  for (let i = 0; i < indices.length; i += 3) {
    const a = 3 * indices[i];
    const b = 3 * indices[i + 1];
    const c = 3 * indices[i + 2];
    const e1x = points[b] - points[a];
    const e1y = points[b + 1] - points[a + 1];
    const e1z = points[b + 2] - points[a + 2];
    const e2x = points[c] - points[a];
    const e2y = points[c + 1] - points[a + 1];
    const e2z = points[c + 2] - points[a + 2];
    const px = dy * e2z - dz * e2y;
    const py = dz * e2x - dx * e2z;
    const pz = dx * e2y - dy * e2x;
    const det = e1x * px + e1y * py + e1z * pz;
    if (Math.abs(det) < 1e-12) continue;
    const inv = 1 / det;
    const tx = ox - points[a];
    const ty = oy - points[a + 1];
    const tz = oz - points[a + 2];
    const u = (tx * px + ty * py + tz * pz) * inv;
    if (u < 0 || u > 1) continue;
    const qx = ty * e1z - tz * e1y;
    const qy = tz * e1x - tx * e1z;
    const qz = tx * e1y - ty * e1x;
    const v = (dx * qx + dy * qy + dz * qz) * inv;
    if (v < 0 || u + v > 1) continue;
    const t = (e2x * qx + e2y * qy + e2z * qz) * inv;
    if (t > 0 && (!best || t < best.t)) best = { t };
  }
  if (best) best.point = origin.map((o, k) => o + direction[k] * best.t);
  return best;
}

// ---------------------------------------------------------------------------------------------
// Rendering

const SCENE_VERTEX = `#version 300 es
in vec3 position;
uniform mat4 model;   // object points to scene units: pose, scale and the fit of the scene
uniform vec3 eye;
uniform vec3 right;
uniform vec3 up;
uniform vec3 forward;
uniform float aspect;
uniform float pointSize;
out vec3 world;
void main() {
  world = (model * vec4(position, 1.0)).xyz;
  vec3 d = world - eye;
  float z = dot(d, forward);
  const float near = 0.01;
  const float far = 50.0;
  gl_Position = vec4(dot(d, right) / (aspect * 0.35), dot(d, up) / 0.35,
                     z * (far + near) / (far - near) - 2.0 * far * near / (far - near), z);
  gl_PointSize = pointSize;
}`;

const SCENE_FRAGMENT = `#version 300 es
precision highp float;
in vec3 world;
out vec4 color;
uniform vec3 eye;
uniform vec3 right;
uniform vec3 up;
uniform vec3 baseColor;
uniform int shaded;   // 0: lines and points in their plain colour
void main() {
  if (shaded == 0) {
    color = vec4(baseColor, 1.0);
    return;
  }
  vec3 normal = normalize(cross(dFdx(world), dFdy(world)));
  vec3 direction = normalize(world - eye);
  vec3 light = normalize(-direction + up * 0.4 + right * 0.3);
  float diffuse = abs(dot(normal, light));
  vec3 halfway = normalize(light - direction);
  float specular = pow(abs(dot(normal, halfway)), 24.0) * 0.25;
  color = vec4(baseColor * (0.25 + 0.75 * diffuse) + vec3(specular), 1.0);
}`;

class SceneViewer {
  constructor() {
    this.objects = [];        // [{id, name, kind, index, pose, key}] from the project
    this.meshes = new Map();  // key -> {points, indices, scale, shape, bounds, buffers}
    this.loading = new Map(); // key -> the request on its way
    this.hidden = new Set();  // ids of objects not shown
    this.picks = [];          // [{point (global mm), color}]
    this.yaw = 0.8;
    this.pitch = 0.45;
    this.distance = 2.2;
    this.target = [0, 0, 0];
    this.canvas = null;
    this.gl = null;
    this.pending = false;
    this.fit = null;          // {center, size}: global mm to scene units
    this.onInteract = () => {};
    this.onPick = null;       // (object id, global point in mm) after a click on an object
    this.onLoad = () => {};
  }

  getState() {
    return { yaw: this.yaw, pitch: this.pitch, distance: this.distance, target: this.target,
      hidden: [...this.hidden] };
  }

  setState(saved) {
    for (const key of ['yaw', 'pitch', 'distance', 'target']) {
      if (saved?.[key] !== undefined) this[key] = saved[key];
    }
    if (Array.isArray(saved?.hidden)) this.hidden = new Set(saved.hidden);
    this.requestDraw();
  }

  /// Takes the objects of the project and loads the meshes it does not have yet.
  setObjects(objects) {
    this.objects = objects;
    const wanted = new Set(objects.map((object) => object.key));
    for (const [key, mesh] of this.meshes) {
      if (!wanted.has(key)) {
        mesh.buffers?.forEach((buffer) => this.gl?.deleteBuffer(buffer));
        this.meshes.delete(key);
      }
    }
    const loads = objects.filter((object) => !this.meshes.has(object.key)).map((object) => {
      if (!this.loading.has(object.key)) {
        this.loading.set(object.key, this.load(object).finally(() => {
          this.loading.delete(object.key);
        }));
      }
      return this.loading.get(object.key);
    });
    this.updateFit();
    this.requestDraw();
    return Promise.all(loads);
  }

  async load(object) {
    const params = new URLSearchParams({ object: object.id.slice(1), max_triangles: 1500000 });
    const response = await fetch('api/object_mesh?' + params);
    if (!response.ok) throw new Error((await response.json()).error);
    const vertices = Number(response.headers.get('X-Vertices'));
    const triangles = Number(response.headers.get('X-Triangles'));
    const buffer = await response.arrayBuffer();
    const points = new Float32Array(buffer, 0, vertices * 3);
    // Undone while on its way: not wanted any more.
    if (!this.objects.some((o) => o.key === object.key)) return;
    const shape = response.headers.get('X-Shape');
    const bounds = pointBounds(points);
    const box = shape === 'box';
    this.meshes.set(object.key, {
      // A box is drawn as its twelve edges.
      points: box ? new Float32Array(boxCorners(bounds).flat()) : points,
      indices: box ? new Uint32Array([0, 1, 2, 3, 4, 5, 6, 7, 0, 2, 1, 3, 4, 6, 5, 7, 0, 4, 1, 5,
        2, 6, 3, 7]) : new Uint32Array(buffer, vertices * 12, triangles * 3),
      shape, bounds,
      scale: response.headers.get('X-Scale').split(',').map(Number),
      buffers: null,
    });
    this.updateFit();
    this.requestDraw();
    this.onLoad();
  }

  /// Visible objects with their mesh: [{object, mesh}].
  visible() {
    return this.objects.filter((object) => !this.hidden.has(object.id) &&
      this.meshes.has(object.key)).map((object) => ({ object, mesh: this.meshes.get(object.key) }));
  }

  /// Fits the scene units to the global bounds of all objects, shown or not, so hiding one
  /// does not move the others.
  updateFit() {
    const placed = this.objects.filter((object) => this.meshes.has(object.key))
      .map((object) => ({ ...this.meshes.get(object.key), pose: object.pose }));
    if (!placed.length) {
      this.fit = null;
      return;
    }
    const { min, max } = sceneBounds(placed);
    this.fit = {
      center: min.map((m, k) => (m + max[k]) / 2),
      size: Math.max(...max.map((m, k) => m - min[k]), 1e-9),
    };
  }

  /// Back to looking at the whole scene.
  reset() {
    this.yaw = 0.8;
    this.pitch = 0.45;
    this.distance = 2.2;
    this.target = [0, 0, 0];
    this.requestDraw();
    this.onInteract();
  }

  toScene(p) {
    return p.map((c, k) => (c - this.fit.center[k]) / this.fit.size);
  }

  fromScene(p) {
    return p.map((c, k) => c * this.fit.size + this.fit.center[k]);
  }

  attach(canvas) {
    this.canvas = canvas;
    this.gl = canvas.getContext('webgl2');
    if (!this.gl) {
      throw new Error('The 3D view needs WebGL2, which this browser has turned off. Try Edge or ' +
        'Chrome; in Firefox set webgl.force-enabled to true in about:config, or update the ' +
        'graphics driver.');
    }
    for (const mesh of this.meshes.values()) mesh.buffers = null;
    this.axes = null;
    this.program = this.createProgram();
    let drag = null;
    canvas.addEventListener('contextmenu', (event) => event.preventDefault());
    canvas.addEventListener('pointerdown', (event) => {
      drag = {
        x: event.clientX, y: event.clientY, yaw: this.yaw, pitch: this.pitch,
        target: [...this.target], pan: event.button === 2 || event.shiftKey, moved: false,
      };
      canvas.setPointerCapture(event.pointerId);
    });
    canvas.addEventListener('pointermove', (event) => {
      if (!drag) return;
      const dx = event.clientX - drag.x;
      const dy = event.clientY - drag.y;
      if (Math.abs(dx) + Math.abs(dy) > 3) drag.moved = true;
      if (!drag.moved) return;
      if (drag.pan) {
        const { right, up } = this.camera();
        const scale = 0.7 * this.distance / Math.max(canvas.clientHeight, 1);
        this.target = drag.target.map((c, i) => c - (right[i] * dx - up[i] * dy) * scale);
      } else {
        this.yaw = drag.yaw - dx * 0.01;
        this.pitch = Math.min(Math.max(drag.pitch + dy * 0.01, -1.5), 1.5);
      }
      this.requestDraw();
    });
    canvas.addEventListener('pointerup', (event) => {
      if (drag?.moved) this.onInteract();
      else if (drag && this.onPick && event.button === 0) this.pick(event);
      drag = null;
    });
    canvas.addEventListener('wheel', (event) => {
      event.preventDefault();
      this.distance = Math.min(Math.max(this.distance * Math.exp(event.deltaY * 0.001), 0.02), 20);
      this.requestDraw();
      this.onInteract();
    }, { passive: false });
    new ResizeObserver(() => this.requestDraw()).observe(canvas);
    this.requestDraw();
  }

  createProgram() {
    const gl = this.gl;
    const compile = (type, source) => {
      const shader = gl.createShader(type);
      gl.shaderSource(shader, source);
      gl.compileShader(shader);
      if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        throw new Error(gl.getShaderInfoLog(shader));
      }
      return shader;
    };
    const program = gl.createProgram();
    gl.attachShader(program, compile(gl.VERTEX_SHADER, SCENE_VERTEX));
    gl.attachShader(program, compile(gl.FRAGMENT_SHADER, SCENE_FRAGMENT));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      throw new Error(gl.getProgramInfoLog(program));
    }
    return program;
  }

  /// Eye position and view axes of the camera, orbiting `target` with z up.
  camera() {
    const offset = [
      this.distance * Math.cos(this.pitch) * Math.cos(this.yaw),
      this.distance * Math.cos(this.pitch) * Math.sin(this.yaw),
      this.distance * Math.sin(this.pitch),
    ];
    const eye = offset.map((c, i) => c + this.target[i]);
    const forward = sceneNormalize(offset.map((c) => -c));
    const right = sceneNormalize(sceneCross(forward, [0, 0, 1]));
    const up = sceneCross(right, forward);
    return { eye, forward, right, up };
  }

  /// Picks the point of a shown object under the pointer and hands it to onPick.
  pick(event) {
    if (!this.fit) return;
    const rect = this.canvas.getBoundingClientRect();
    const x = 2 * (event.clientX - rect.left) / rect.width - 1;
    const y = 1 - 2 * (event.clientY - rect.top) / rect.height;
    const { eye, forward, right, up } = this.camera();
    const aspect = rect.width / Math.max(rect.height, 1);
    const d = forward.map((f, k) => f + x * aspect * 0.35 * right[k] + y * 0.35 * up[k]);
    // The ray in global mm: through the eye, one scene unit is fit.size mm.
    const origin = this.fromScene(eye);
    const direction = d.map((c) => c * this.fit.size);
    let best = null;
    for (const { object, mesh } of this.visible()) {
      if (mesh.shape === 'box') continue;
      // Into the object's points: the inverse pose, then the scale per axis.
      const inverse = invertPose(object.pose);
      const o = applyPose(inverse, origin).map((c, k) => c / mesh.scale[k]);
      const toward = applyPose(inverse, origin.map((c, k) => c + direction[k]))
        .map((c, k) => c / mesh.scale[k] - o[k]);
      const hit = intersectMesh(o, toward, mesh.points, mesh.indices);
      if (hit && (!best || hit.t < best.t)) {
        best = { t: hit.t, id: object.id,
          point: applyPose(object.pose, hit.point.map((c, k) => c * mesh.scale[k])) };
      }
    }
    if (best) this.onPick(best.id, best.point);
  }

  requestDraw() {
    if (this.pending || !this.gl) return;
    this.pending = true;
    requestAnimationFrame(() => {
      this.pending = false;
      this.draw();
    });
  }

  capture() {
    this.draw();
    return this.canvas.toDataURL('image/png');
  }

  upload(mesh) {
    const gl = this.gl;
    const points = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, points);
    gl.bufferData(gl.ARRAY_BUFFER, mesh.points, gl.STATIC_DRAW);
    const indices = gl.createBuffer();
    mesh.vao = gl.createVertexArray();
    gl.bindVertexArray(mesh.vao);
    const location = gl.getAttribLocation(this.program, 'position');
    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location, 3, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indices);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, mesh.indices, gl.STATIC_DRAW);
    gl.bindVertexArray(null);
    mesh.buffers = [points, indices];
  }

  /// Column-major matrix for WebGL: object points to scene units.
  modelMatrix(pose, scale) {
    const s = 1 / this.fit.size;
    const m = new Float32Array(16);
    for (let r = 0; r < 3; r += 1) {
      for (let c = 0; c < 3; c += 1) m[4 * c + r] = pose[4 * r + c] * scale[c] * s;
      m[12 + r] = (pose[4 * r + 3] - this.fit.center[r]) * s;
    }
    m[15] = 1;
    return m;
  }

  /// Draws lines or points given in scene units, in one colour.
  drawLines(mode, vertices, color, size = 1) {
    const gl = this.gl;
    const buffer = gl.createBuffer();
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(vertices), gl.STREAM_DRAW);
    const location = gl.getAttribLocation(this.program, 'position');
    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location, 3, gl.FLOAT, false, 0, 0);
    const uniform = (name) => gl.getUniformLocation(this.program, name);
    gl.uniformMatrix4fv(uniform('model'), false,
      new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]));
    gl.uniform3fv(uniform('baseColor'), color.map((c) => c / 255));
    gl.uniform1i(uniform('shaded'), 0);
    gl.uniform1f(uniform('pointSize'), size);
    gl.drawArrays(mode, 0, vertices.length / 3);
    gl.bindVertexArray(null);
    gl.deleteVertexArray(vao);
    gl.deleteBuffer(buffer);
  }

  draw() {
    const gl = this.gl;
    const canvas = this.canvas;
    if (!gl) return;
    const ratio = window.devicePixelRatio || 1;
    const width = Math.round(canvas.clientWidth * ratio);
    const height = Math.round(canvas.clientHeight * ratio);
    if (width === 0 || height === 0) return;
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    gl.viewport(0, 0, width, height);
    gl.clearColor(0.11, 0.12, 0.14, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    if (!this.fit) return;
    const { eye, forward, right, up } = this.camera();
    gl.useProgram(this.program);
    const uniform = (name) => gl.getUniformLocation(this.program, name);
    gl.uniform3fv(uniform('eye'), eye);
    gl.uniform3fv(uniform('right'), right);
    gl.uniform3fv(uniform('up'), up);
    gl.uniform3fv(uniform('forward'), forward);
    gl.uniform1f(uniform('aspect'), width / height);
    gl.uniform1f(uniform('pointSize'), 1);
    gl.enable(gl.DEPTH_TEST);
    for (const { object, mesh } of this.visible()) {
      if (!mesh.buffers) this.upload(mesh);
      gl.uniformMatrix4fv(uniform('model'), false, this.modelMatrix(object.pose, mesh.scale));
      gl.uniform3fv(uniform('baseColor'), objectColor(object.index).map((c) => c / 255));
      const box = mesh.shape === 'box';
      gl.uniform1i(uniform('shaded'), box ? 0 : 1);
      gl.bindVertexArray(mesh.vao);
      gl.drawElements(box ? gl.LINES : gl.TRIANGLES, mesh.indices.length, gl.UNSIGNED_INT, 0);
      gl.bindVertexArray(null);
    }
    // The global axes at the origin: x red, y green, z blue, a tenth of the scene long.
    const origin = this.toScene([0, 0, 0]);
    [[1, 0, 0], [0, 1, 0], [0, 0, 1]].forEach((axis, k) => {
      const end = origin.map((c, i) => c + 0.1 * axis[i]);
      this.drawLines(gl.LINES, [...origin, ...end],
        [[230, 70, 60], [90, 200, 90], [80, 130, 240]][k]);
    });
    gl.disable(gl.DEPTH_TEST);
    for (const pick of this.picks) {
      this.drawLines(gl.POINTS, this.toScene(pick.point), pick.color, 9 * ratio);
    }
  }
}

function sceneNormalize(v) {
  const length = Math.hypot(...v);
  return v.map((c) => c / length);
}

function sceneCross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

if (typeof window !== 'undefined') {
  Object.assign(window, { OBJECT_COLORS, SceneViewer, objectColor });
}
if (typeof module !== 'undefined') {
  module.exports = { OBJECT_COLORS, applyPose, boxCorners, intersectMesh, invertPose,
    pointBounds, sceneBounds };
}
