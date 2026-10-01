// Simple 3D view of VoxelSieve Studio (ADR 0008): ray casting of a coarse level of the dataset
// (at most 256 voxels per axis) in WebGL2. When the camera comes close, the part of the volume in
// front of it is loaded again at a finer level and drawn from that detail texture wherever it
// covers the ray, fading in at its border. "Surface" shades the part surface at a threshold,
// "Transfer function" composites colour and opacity per grey value (transfer.js) and
// "Maximum intensity projection" shows the densest value along each ray. Pores and zones of a porosity
// analysis are drawn in their own colours. A cut along x opens the part. "Extracted surface"
// draws the mesh of a surface step (ADR 0009) instead of the grey values, "Nominal-actual deviation"
// the surface of a nominal-actual comparison coloured by its deviation from the CAD model.
'use strict';

const VOLUME_VERTEX = `#version 300 es
in vec2 position;
out vec2 ndc;
void main() {
  ndc = position;
  gl_Position = vec4(position, 0.0, 1.0);
}`;

/// Background styles: colours are top/centre and bottom/edge.
const BACKGROUNDS = {
  studio: { name: 'Studio light', style: 2, colors: [[232, 234, 237], [150, 157, 166]] },
  studioDunkel: { name: 'Studio dark', style: 2, colors: [[70, 76, 86], [14, 16, 19]] },
  verlauf: { name: 'Gradient blue', style: 1, colors: [[58, 72, 96], [10, 12, 18]] },
  verlaufGrau: { name: 'Gradient grey', style: 1, colors: [[96, 100, 106], [26, 28, 31]] },
  schwarz: { name: 'Black', style: 0, colors: [[18, 18, 18], [18, 18, 18]] },
  weiss: { name: 'White', style: 0, colors: [[255, 255, 255], [255, 255, 255]] },
};

// The surface mesh in level-0 voxel coordinates, projected like the rays of VOLUME_FRAGMENT.
const MESH_VERTEX = `#version 300 es
in vec3 position;
in float deviation;   // mm, for mode 4
uniform vec3 extent;  // level-0 voxels that span the texture (preview dims times 2^level)
uniform vec3 box;
uniform vec3 eye;
uniform vec3 right;
uniform vec3 up;
uniform vec3 forward;
uniform float aspect;
out vec3 world;
out vec3 tex;
out float dev;
void main() {
  dev = deviation;
  tex = (position + 0.5) / extent;
  world = (tex - 0.5) * 2.0 * box;
  vec3 d = world - eye;
  float z = dot(d, forward);
  const float near = 0.01;
  const float far = 20.0;
  gl_Position = vec4(dot(d, right) / (aspect * 0.35), dot(d, up) / 0.35,
                     z * (far + near) / (far - near) - 2.0 * far * near / (far - near), z);
}`;

const MESH_FRAGMENT = `#version 300 es
precision highp float;
in vec3 world;
in vec3 tex;
in float dev;
out vec4 color;
uniform vec3 eye;
uniform vec3 right;
uniform vec3 up;
uniform float cut;
uniform vec3 surfaceColor;
uniform int deviationColors;  // 1: colour by the deviation, as deviationColor in compare.cpp
uniform float tolerance;
uniform float range;
vec3 deviationColor(float d) {
  float a = abs(d);
  if (a <= tolerance) return vec3(60.0, 190.0, 90.0) / 255.0;
  float t = clamp((a - tolerance) / max(range - tolerance, 1e-6), 0.0, 1.0);
  if (d > 0.0) return mix(vec3(240.0, 225.0, 40.0), vec3(215.0, 30.0, 30.0), t) / 255.0;
  return mix(vec3(40.0, 205.0, 240.0), vec3(40.0, 60.0, 215.0), t) / 255.0;
}
void main() {
  if (tex.x > cut) discard;
  vec3 normal = normalize(cross(dFdx(world), dFdy(world)));
  vec3 direction = normalize(world - eye);
  vec3 light = normalize(-direction + up * 0.4 + right * 0.3);
  float diffuse = abs(dot(normal, light));
  vec3 halfway = normalize(light - direction);
  float specular = pow(abs(dot(normal, halfway)), 24.0) * 0.25;
  vec3 base = deviationColors == 1 ? deviationColor(dev) : surfaceColor;
  color = vec4(base * (0.25 + 0.75 * diffuse) + vec3(specular), 1.0);
}`;

const VOLUME_FRAGMENT = `#version 300 es
precision highp float;
precision highp sampler3D;
in vec2 ndc;
out vec4 color;
uniform sampler3D grey;
uniform sampler3D overlay;
uniform sampler2D transfer; // 256 x 1 RGBA: colour and opacity per grey value
uniform sampler3D detailGrey;    // a finer level of the part near the camera
uniform sampler3D detailOverlay;
uniform bool hasDetail;
uniform vec3 detailMin;          // the detail region in texture coordinates of the volume
uniform vec3 detailMax;
uniform vec3 detailVoxel;        // one voxel in texture coordinates of the detail
uniform vec3 eye;
uniform vec3 right;
uniform vec3 up;
uniform vec3 forward;
uniform float aspect;
uniform vec3 box;        // half extent of the volume, largest axis 0.5
uniform vec3 voxel;      // one voxel in texture coordinates
uniform float threshold; // material threshold on the 0..1 grey scale
uniform int mode;        // 0 surface, 1 transfer function, 2 maximum intensity projection,
                         // 3 background only (the mesh is drawn on top)
uniform bool shading;
uniform bool pores;
uniform float cut;       // texture x beyond which the part is cut away
uniform vec3 surfaceColor;
uniform vec3 poreColor;
uniform vec3 zoneColor;
uniform vec3 background;   // top or centre colour
uniform vec3 background2;  // bottom or edge colour
uniform int backgroundStyle; // 0 plain, 1 vertical gradient, 2 studio

// Opacities of the transfer function hold for this path length (1/128 of the largest axis), so
// the picture does not depend on the level shown.
const float kReferenceLength = 1.0 / 128.0;

bool hitBox(vec3 origin, vec3 direction, out float near, out float far) {
  vec3 inverse = 1.0 / direction;
  vec3 t0 = (-box - origin) * inverse;
  vec3 t1 = (box - origin) * inverse;
  vec3 low = min(t0, t1);
  vec3 high = max(t0, t1);
  near = max(max(low.x, low.y), low.z);
  far = min(min(high.x, high.y), high.z);
  return far > max(near, 0.0);
}

bool inDetail(vec3 t) {
  return hasDetail && all(greaterThanEqual(t, detailMin)) && all(lessThanEqual(t, detailMax));
}

// Texture coordinates of the detail for volume texture coordinates t.
vec3 detailAt(vec3 t) {
  return (t - detailMin) / (detailMax - detailMin);
}

// Share of the detail at t: it fades in over the outer voxels of the detail, so its border does
// not show as a step.
float detailWeight(vec3 t) {
  if (!inDetail(t)) return 0.0;
  vec3 d = detailAt(t);
  vec3 edge = min(d, 1.0 - d) / detailVoxel;
  return clamp(min(min(edge.x, edge.y), edge.z) / 4.0, 0.0, 1.0);
}

float greyAt(vec3 t) {
  float w = detailWeight(t);
  float coarse = w < 1.0 ? texture(grey, t).r : 0.0;
  float fine = w > 0.0 ? texture(detailGrey, detailAt(t)).r : 0.0;
  return mix(coarse, fine, w);
}

// Differences of the grey values one voxel apart.
vec3 coarseGradient(vec3 t) {
  return vec3(
    texture(grey, t + vec3(voxel.x, 0, 0)).r - texture(grey, t - vec3(voxel.x, 0, 0)).r,
    texture(grey, t + vec3(0, voxel.y, 0)).r - texture(grey, t - vec3(0, voxel.y, 0)).r,
    texture(grey, t + vec3(0, 0, voxel.z)).r - texture(grey, t - vec3(0, 0, voxel.z)).r);
}

vec3 detailGradient(vec3 t) {
  vec3 d = detailAt(t);
  vec3 v = detailVoxel;
  return vec3(
    texture(detailGrey, d + vec3(v.x, 0, 0)).r - texture(detailGrey, d - vec3(v.x, 0, 0)).r,
    texture(detailGrey, d + vec3(0, v.y, 0)).r - texture(detailGrey, d - vec3(0, v.y, 0)).r,
    texture(detailGrey, d + vec3(0, 0, v.z)).r - texture(detailGrey, d - vec3(0, 0, v.z)).r);
}

vec3 gradientAt(vec3 t) {
  float w = detailWeight(t);
  if (w <= 0.0) return coarseGradient(t);
  if (w >= 1.0) return detailGradient(t);
  return mix(coarseGradient(t), detailGradient(t), w);
}

vec3 normalAt(vec3 t) {
  // Per voxel to per world unit, so voxels that are not cubes shade right.
  float w = detailWeight(t);
  vec3 g = w < 1.0 ? coarseGradient(t) / (box * voxel) : vec3(0.0);
  if (w > 0.0) {
    g = mix(g, detailGradient(t) / (box * (detailMax - detailMin) * detailVoxel), w);
  }
  return length(g) > 1e-5 ? -normalize(g) : vec3(0.0);
}

vec3 shade(vec3 base, vec3 normal, vec3 direction) {
  vec3 light = normalize(-direction + up * 0.4 + right * 0.3);
  float diffuse = abs(dot(normal, light));
  vec3 halfway = normalize(light - direction);
  float specular = pow(abs(dot(normal, halfway)), 24.0) * 0.25;
  return base * (0.25 + 0.75 * diffuse) + vec3(specular);
}

int classAt(vec3 t) {
  float value = detailWeight(t) > 0.5 ? texture(detailOverlay, detailAt(t)).r : texture(overlay, t).r;
  return int(value * 255.0 + 0.5);
}

// Opacity of one step of the given length for an opacity per reference length.
float stepOpacity(float opacity, float stepSize) {
  return 1.0 - pow(1.0 - min(opacity, 0.999), stepSize / kReferenceLength);
}

vec3 backgroundAt() {
  if (backgroundStyle == 1) return mix(background2, background, ndc.y * 0.5 + 0.5);
  if (backgroundStyle == 2) {
    // Studio: a soft light in the middle falling off to the edges, the lower part a little darker
    // like a floor.
    float r = length(vec2(ndc.x * aspect * 0.55, ndc.y * 0.8 - 0.1));
    vec3 c = mix(background, background2, smoothstep(0.05, 1.05, r));
    return c * (1.0 - 0.15 * smoothstep(-0.1, -1.0, ndc.y));
  }
  return background;
}

void main() {
  vec3 background = backgroundAt();
  if (mode >= 3) {
    color = vec4(background, 1.0);
    return;
  }
  vec3 direction = normalize(forward + ndc.x * aspect * 0.35 * right + ndc.y * 0.35 * up);
  float near;
  float far;
  if (!hitBox(eye, direction, near, far)) {
    color = vec4(background, 1.0);
    return;
  }
  near = max(near, 0.0);
  // Steps of about 0.6 voxels, of the detail where the ray is in it.
  vec3 coarseVoxel = voxel * box;
  vec3 fineVoxel = box * (detailMax - detailMin) * detailVoxel;
  float coarseStep = min(min(coarseVoxel.x, coarseVoxel.y), coarseVoxel.z) * 1.2;
  float fineStep = hasDetail ? min(min(fineVoxel.x, fineVoxel.y), fineVoxel.z) * 1.2 : coarseStep;
  float stepSize = inDetail((eye + direction * near) / (2.0 * box) + 0.5) ? fineStep : coarseStep;
  // A per-pixel offset of the first sample turns the rings of regular sampling into fine noise.
  near += stepSize * fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
  vec3 accumulated = vec3(0.0);
  float alpha = 0.0;
  float highest = 0.0;
  bool porous = false;
  bool entered = false;
  for (float t = near; t < far; t += stepSize) {
    vec3 p = eye + direction * t;
    vec3 tex = p / (2.0 * box) + 0.5;
    stepSize = inDetail(tex) ? fineStep : coarseStep;
    if (tex.x > cut) {
      entered = false;
      continue;
    }
    int kind = classAt(tex);
    float value = greyAt(tex);
    if (mode == 0) {
      if (pores && kind == 1) {
        color = vec4(shade(poreColor, normalAt(tex), direction), 1.0);
        return;
      }
      if (value >= threshold) {
        // On the cut face the grey gradient says nothing; light it as the plane it is.
        bool onCut = cut < 1.0 && tex.x > cut - 1.5 * voxel.x && !entered;
        vec3 n = onCut ? vec3(1.0, 0.0, 0.0) : normalAt(tex);
        vec3 base = (pores && kind == 2) ? zoneColor : surfaceColor;
        color = vec4(shade(base, n, direction), 1.0);
        return;
      }
      entered = true;
    } else if (mode == 2) {
      highest = max(highest, value);
      porous = porous || (pores && kind == 1);
    } else {
      vec4 sample_;
      if (pores && kind == 1) {
        sample_ = vec4(poreColor, 0.6);
      } else if (pores && kind == 2) {
        sample_ = vec4(zoneColor, 0.1);
      } else {
        sample_ = texture(transfer, vec2(value, 0.5));
        // Quadratic, so the low opacities that matter for looking through get room on the curve.
        sample_.a *= sample_.a;
      }
      float a = stepOpacity(sample_.a, stepSize);
      if (a < 0.0005) continue;
      vec3 rgb = sample_.rgb;
      if (shading) {
        vec3 g = gradientAt(tex);
        float strength = smoothstep(0.02, 0.12, length(g));
        if (strength > 0.0) rgb = mix(rgb, shade(rgb, -normalize(g), direction), strength);
      }
      accumulated += (1.0 - alpha) * a * rgb;
      alpha += (1.0 - alpha) * a;
      if (alpha > 0.98) break;
    }
  }
  if (mode == 2) {
    vec3 mapped = mix(background, texture(transfer, vec2(highest, 0.5)).rgb, highest);
    color = vec4(porous ? mix(mapped, poreColor, 0.65) : mapped, 1.0);
    return;
  }
  color = vec4(accumulated + (1.0 - alpha) * background, 1.0);
}`;

class VolumeViewer {
  constructor() {
    this.volume = null;       // {dims, level, voxelSize, window, key}
    this.mode = 0;
    this.pores = true;
    this.cut = 1;
    this.threshold = 0.5;
    this.shading = true;
    this.surfaceColor = [209, 214, 219];
    this.poreColor = [230, 38, 31];
    this.zoneColor = [242, 204, 77];
    this.background = { preset: 'studioDunkel', ...BACKGROUNDS.studioDunkel };
    this.histogram = new Array(256).fill(0);
    this.transfer = null;       // lookup table of 256 RGBA bytes
    this.surface = null;        // {key, points, triangles} of the mesh of a surface step
    this.yaw = 0.8;
    this.pitch = 0.45;
    this.distance = 2.2;
    this.target = [0, 0, 0];  // the point the camera orbits, moved by panning
    this.detail = null;       // finer level of the part near the camera, see updateDetail
    this.detailWanted = null; // key of the region requested last
    this.detailPending = null; // the region on its way
    this.detailTimer = null;
    this.onDetail = () => {}; // the detail changed
    this.canvas = null;
    this.gl = null;
    this.pending = false;
    this.onChange = () => {};
    this.onInteract = () => {};  // camera moved by the user
  }

  /// Everything that makes up the picture, for saving it with the project or as a named view.
  getState() {
    return {
      key: this.volume?.key ?? null,
      mode: this.mode, shading: this.shading, pores: this.pores, cut: this.cut,
      threshold: this.threshold, yaw: this.yaw, pitch: this.pitch, distance: this.distance,
      target: this.target, surfaceColor: this.surfaceColor, poreColor: this.poreColor, zoneColor: this.zoneColor,
      background: this.background,
    };
  }

  setState(saved) {
    const keys = ['mode', 'shading', 'pores', 'cut', 'threshold', 'yaw', 'pitch', 'distance',
      'target', 'surfaceColor', 'poreColor', 'zoneColor', 'background'];
    for (const key of keys) if (saved[key] !== undefined) this[key] = saved[key];
    this.requestDraw();
  }

  /// The current picture as a PNG data URL, drawn right now so the buffer is still there.
  capture() {
    this.draw();
    return this.canvas.toDataURL('image/png');
  }

  /// Renders with other settings and returns a small PNG data URL, leaving the view unchanged.
  renderPreview(settings, table, width = 240) {
    const saved = this.getState();
    const savedTable = this.transfer;
    Object.assign(this, settings);
    this.transfer = table;
    this.transferUploaded = false;
    this.draw();
    const scaled = document.createElement('canvas');
    scaled.width = width;
    scaled.height = Math.round(width * this.canvas.height / Math.max(this.canvas.width, 1));
    scaled.getContext('2d').drawImage(this.canvas, 0, 0, scaled.width, scaled.height);
    this.setState(saved);
    this.transfer = savedTable;
    this.transferUploaded = false;
    this.draw();
    return scaled.toDataURL('image/png');
  }

  /// Loads the volume preview of a dataset step (with the overlay of a porosity step).
  async load(step, porosity) {
    const key = step + '/' + (porosity ?? '-');
    if (this.volume?.key === key) return;
    this.source = { step, porosity };
    const params = new URLSearchParams({ step, max: 256 });
    if (porosity !== null) params.set('porosity', porosity);
    const response = await fetch('api/volume?' + params);
    if (!response.ok) throw new Error((await response.json()).error);
    const dims = response.headers.get('X-Dims').split(',').map(Number);
    const window = response.headers.get('X-Window').split(',').map(Number);
    const buffer = new Uint8Array(await response.arrayBuffer());
    const count = dims[0] * dims[1] * dims[2];
    this.volume = {
      key, dims, window,
      level: Number(response.headers.get('X-Level')),
      // Edge lengths x, y, z in mm; voxels need not be cubes (ADR 0012).
      voxelSize: response.headers.get('X-Voxel-Size').split(',').map(Number),
      grey: buffer.subarray(0, count),
      overlay: buffer.subarray(count, 2 * count),
    };
    this.detail = null;
    this.detailWanted = null;
    this.detailPending = null;
    this.detailUploaded = false;
    this.histogram = new Array(256).fill(0);
    for (let i = 0; i < count; i += 1) this.histogram[this.volume.grey[i]] += 1;
    // Default threshold halfway between air and material.
    this.threshold = this.suggestThreshold();
    this.uploaded = false;
    this.requestDraw();
    this.onChange();
  }

  /// Middle between the two main peaks of the grey histogram (air and material).
  suggestThreshold() {
    const histogram = new Array(256).fill(0);
    this.histogram.forEach((count, v) => { histogram[v] = count; });
    let air = 0;
    for (let v = 0; v < 128; v += 1) if (histogram[v] > histogram[air]) air = v;
    let material = 128;
    for (let v = 128; v < 256; v += 1) if (histogram[v] > histogram[material]) material = v;
    return (air + material) / 2 / 255;
  }

  attach(canvas) {
    this.canvas = canvas;
    this.gl = canvas.getContext('webgl2');
    if (!this.gl) throw new Error('The browser does not support WebGL2');
    // A new canvas has a new context; textures of the old one are gone with it.
    this.uploaded = false;
    this.textures = null;
    this.detailUploaded = false;
    this.detailTextures = null;
    this.transferUploaded = false;
    this.transferTexture = null;
    this.program = this.createProgram();
    this.meshProgram = this.createMeshProgram();
    this.meshBuffers = null;
    let drag = null;
    canvas.addEventListener('contextmenu', (event) => event.preventDefault());
    canvas.addEventListener('pointerdown', (event) => {
      // Left drag turns; right drag or shift drag moves the point the camera looks at.
      drag = {
        x: event.clientX, y: event.clientY, yaw: this.yaw, pitch: this.pitch,
        target: [...this.target], pan: event.button === 2 || event.shiftKey,
      };
      canvas.setPointerCapture(event.pointerId);
    });
    canvas.addEventListener('pointermove', (event) => {
      if (!drag) return;
      const dx = event.clientX - drag.x;
      const dy = event.clientY - drag.y;
      if (drag.pan) {
        const { right, up } = this.camera();
        // The picture moves with the pointer: one canvas height spans 0.7 distance units.
        const scale = 0.7 * this.distance / Math.max(canvas.clientHeight, 1);
        this.target = drag.target.map((c, i) =>
          Math.min(Math.max(c - (right[i] * dx - up[i] * dy) * scale, -0.6), 0.6));
      } else {
        this.yaw = drag.yaw - dx * 0.01;
        this.pitch = Math.min(Math.max(drag.pitch + dy * 0.01, -1.5), 1.5);
      }
      this.requestDraw();
    });
    canvas.addEventListener('pointerup', () => {
      if (drag) this.onInteract();
      drag = null;
    });
    canvas.addEventListener('wheel', (event) => {
      event.preventDefault();
      this.distance = Math.min(Math.max(this.distance * Math.exp(event.deltaY * 0.001), 0.05), 8);
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
    gl.attachShader(program, compile(gl.VERTEX_SHADER, VOLUME_VERTEX));
    gl.attachShader(program, compile(gl.FRAGMENT_SHADER, VOLUME_FRAGMENT));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      throw new Error(gl.getProgramInfoLog(program));
    }
    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    this.vao = gl.createVertexArray();
    gl.bindVertexArray(this.vao);
    const location = gl.getAttribLocation(program, 'position');
    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location, 2, gl.FLOAT, false, 0, 0);
    return program;
  }

  createMeshProgram() {
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
    gl.attachShader(program, compile(gl.VERTEX_SHADER, MESH_VERTEX));
    gl.attachShader(program, compile(gl.FRAGMENT_SHADER, MESH_FRAGMENT));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      throw new Error(gl.getProgramInfoLog(program));
    }
    return program;
  }

  /// Loads the mesh of a surface step for mode 3.
  loadSurface(step) {
    return this.loadMesh('surface', step);
  }

  /// Loads the compared surface of a nominal-actual comparison step for mode 4.
  loadDeviation(step) {
    return this.loadMesh('deviation', step);
  }

  async loadMesh(kind, step) {
    const key = kind + ':' + step;
    if (this.surface?.key === key) return this.surface;
    const response = await fetch('api/' + kind + '?' + new URLSearchParams({ step }));
    if (!response.ok) throw new Error((await response.json()).error);
    const vertices = Number(response.headers.get('X-Vertices'));
    const triangles = Number(response.headers.get('X-Triangles'));
    const buffer = await response.arrayBuffer();
    const deviations = kind === 'deviation' ? vertices : 0;
    this.surface = {
      key, kind, triangles,
      points: new Float32Array(buffer, 0, vertices * 3),
      deviation: deviations ? new Float32Array(buffer, vertices * 12, vertices) : null,
      indices: new Uint32Array(buffer, vertices * 12 + deviations * 4, triangles * 3),
      tolerance: Number(response.headers.get('X-Tolerance') ?? 0),
      range: Number(response.headers.get('X-Range') ?? 0),
    };
    if (this.meshBuffers && this.gl) {
      this.meshBuffers.forEach((b) => this.gl.deleteBuffer(b));
    }
    this.meshBuffers = null;
    this.requestDraw();
    return this.surface;
  }

  uploadMesh() {
    const gl = this.gl;
    const points = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, points);
    gl.bufferData(gl.ARRAY_BUFFER, this.surface.points, gl.STATIC_DRAW);
    const indices = gl.createBuffer();
    this.meshVao = gl.createVertexArray();
    gl.bindVertexArray(this.meshVao);
    const location = gl.getAttribLocation(this.meshProgram, 'position');
    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location, 3, gl.FLOAT, false, 0, 0);
    this.meshBuffers = [points, indices];
    const deviation = gl.getAttribLocation(this.meshProgram, 'deviation');
    if (this.surface.deviation) {
      const values = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, values);
      gl.bufferData(gl.ARRAY_BUFFER, this.surface.deviation, gl.STATIC_DRAW);
      gl.enableVertexAttribArray(deviation);
      gl.vertexAttribPointer(deviation, 1, gl.FLOAT, false, 0, 0);
      this.meshBuffers.push(values);
    } else if (deviation >= 0) {
      gl.disableVertexAttribArray(deviation);
      gl.vertexAttrib1f(deviation, 0);
    }
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indices);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, this.surface.indices, gl.STATIC_DRAW);
    gl.bindVertexArray(null);
  }

  drawMesh(eye, right, up, forward, aspect, box) {
    const gl = this.gl;
    if (!this.meshBuffers) this.uploadMesh();
    const { dims, level } = this.volume;
    const scale = 2 ** level;
    gl.useProgram(this.meshProgram);
    const uniform = (name) => gl.getUniformLocation(this.meshProgram, name);
    gl.uniform3fv(uniform('extent'), dims.map((d) => d * scale));
    gl.uniform3fv(uniform('box'), box);
    gl.uniform3fv(uniform('eye'), eye);
    gl.uniform3fv(uniform('right'), right);
    gl.uniform3fv(uniform('up'), up);
    gl.uniform3fv(uniform('forward'), forward);
    gl.uniform1f(uniform('aspect'), aspect);
    gl.uniform1f(uniform('cut'), this.cut);
    gl.uniform3fv(uniform('surfaceColor'), this.surfaceColor.map((c) => c / 255));
    gl.uniform1i(uniform('deviationColors'), this.surface.deviation ? 1 : 0);
    gl.uniform1f(uniform('tolerance'), this.surface.tolerance);
    gl.uniform1f(uniform('range'), this.surface.range);
    gl.enable(gl.DEPTH_TEST);
    gl.clear(gl.DEPTH_BUFFER_BIT);
    gl.bindVertexArray(this.meshVao);
    gl.drawElements(gl.TRIANGLES, this.surface.triangles * 3, gl.UNSIGNED_INT, 0);
    gl.bindVertexArray(null);
    gl.disable(gl.DEPTH_TEST);
  }

  /// Eye position and view axes of the camera, orbiting `target` with z up.
  camera() {
    const offset = [
      this.distance * Math.cos(this.pitch) * Math.cos(this.yaw),
      this.distance * Math.cos(this.pitch) * Math.sin(this.yaw),
      this.distance * Math.sin(this.pitch),
    ];
    const eye = offset.map((c, i) => c + this.target[i]);
    const forward = normalize(offset.map((c) => -c));
    const right = normalize(cross(forward, [0, 0, 1]));
    const up = cross(right, forward);
    return { eye, forward, right, up };
  }

  /// Half extent of the volume box in world units, the largest axis 0.5.
  boxHalf() {
    const sizes = this.volume.dims.map((d, a) => d * this.volume.voxelSize[a]);
    const largest = Math.max(...sizes);
    return sizes.map((s) => s / largest / 2);
  }

  /// Whether the coarse voxel with this grey value and overlay class shows in the current mode.
  visible(grey, kind) {
    if (this.pores && kind === 1) return true;
    if (this.mode === 1) return (this.transfer?.[grey * 4 + 3] ?? 0) > 8;
    return grey >= this.threshold * 255;
  }

  /// The level-0 voxels the camera sees nearest: rays through a grid over the picture are
  /// marched through the coarse volume to the first voxel that shows. The level is as fine as a
  /// pixel at the nearest hit needs, and coarse enough that the hits up to half again as far fit
  /// the voxel budget. Null when that needs no finer level than the coarse volume. `seen` is the
  /// part around those hits, `region` the part to load: grown to the budget of the level, so the
  /// border of the detail stays out of the picture where it can.
  detailRegion() {
    const { eye, forward, right, up } = this.camera();
    const box = this.boxHalf();
    const { dims, grey, overlay } = this.volume;
    const aspect = this.canvas.clientWidth / Math.max(this.canvas.clientHeight, 1);
    // One coarse voxel along the ray, in world units.
    const step = Math.min(...box.map((b, k) => 2 * b / dims[k]));
    const hits = [];
    for (let j = 0; j <= 8; j += 1) {
      for (let i = 0; i <= 8; i += 1) {
        const d = normalize(forward.map((f, k) =>
          f + (i / 4 - 1) * aspect * 0.35 * right[k] + (j / 4 - 1) * 0.35 * up[k]));
        let near = 0;
        let far = Infinity;
        for (let k = 0; k < 3; k += 1) {
          const t0 = (-box[k] - eye[k]) / d[k];
          const t1 = (box[k] - eye[k]) / d[k];
          near = Math.max(near, Math.min(t0, t1));
          far = Math.min(far, Math.max(t0, t1));
        }
        for (let t = near; t < far; t += step) {
          const tex = eye.map((e, k) => (e + d[k] * t) / (2 * box[k]) + 0.5);
          if (tex[0] > this.cut) continue;
          const v = tex.map((c, k) => Math.min(Math.max(Math.floor(c * dims[k]), 0), dims[k] - 1));
          const index = v[0] + dims[0] * (v[1] + dims[1] * v[2]);
          if (this.visible(grey[index], overlay[index])) {
            hits.push({ t, d, near, far });
            break;
          }
        }
      }
    }
    if (!hits.length) return null;
    const nearest = Math.min(...hits.map((h) => h.t));
    // World to level-0 voxels: the texture spans the preview dims times 2^level.
    const scale = 2 ** this.volume.level;
    const extent = dims.map((n) => n * scale);
    const toVoxel = (p, k) => (p / (2 * box[k]) + 0.5) * extent[k];
    // What the nearest hits show: from just in front of them a little into the part.
    const seen = [Infinity, -Infinity, Infinity, -Infinity, Infinity, -Infinity];
    for (const h of hits.filter((hit) => hit.t <= 1.5 * nearest)) {
      for (const t of [Math.max(h.near, h.t - 2 * step), Math.min(h.far, h.t + 0.2 * h.t)]) {
        for (let k = 0; k < 3; k += 1) {
          const v = toVoxel(eye[k] + h.d[k] * t, k);
          seen[2 * k] = Math.min(seen[2 * k], Math.max(0, Math.floor(v)));
          seen[2 * k + 1] = Math.max(seen[2 * k + 1], Math.min(extent[k], Math.ceil(v)));
        }
      }
    }
    if ([0, 1, 2].some((k) => seen[2 * k + 1] <= seen[2 * k])) return null;
    // The level a pixel at the nearest hit needs: the picture is 0.7 distance units high.
    const pixel = 0.7 * nearest / Math.max(this.canvas.height, 1);
    const voxel = Math.min(...box.map((b, k) => 2 * b / extent[k]));
    let level = Math.max(0, Math.floor(Math.log2(pixel / voxel)));
    // The level the server reads for the seen part: the finest at which the voxels covering it
    // are at most the budget per axis (readVolumePreview).
    const budget = 256;
    const fits = (r, l) => [0, 1, 2].every((k) =>
      ((r[2 * k + 1] - 1) >> l) - (r[2 * k] >> l) + 1 <= budget);
    while (!fits(seen, level)) level += 1;
    if (level >= this.volume.level) return null;
    // Grow the region around what is seen to the budget of that level, inside the volume.
    const region = [];
    for (let k = 0; k < 3; k += 1) {
      const size = Math.min((budget - 1) << level, extent[k]);
      const centre = (seen[2 * k] + seen[2 * k + 1]) / 2;
      const a = Math.max(0, Math.min(Math.round(centre - size / 2), extent[k] - size));
      region.push(a, a + size);
    }
    return { seen, region, level };
  }

  /// Loads the part near the camera at a finer level, once the camera has rested for a moment.
  scheduleDetail() {
    clearTimeout(this.detailTimer);
    this.detailTimer = setTimeout(() => this.updateDetail().catch(() => {}), 350);
  }

  async updateDetail() {
    if (!this.volume || !this.canvas || this.mode >= 3) return;
    const wanted = this.detailRegion();
    const key = wanted ? this.volume.key + ':' + wanted.region.join(',') : null;
    if (key === this.detailWanted) return;
    // A detail that already covers what is seen at the level wanted, loaded or on its way, stays.
    const covers = (d) => d && d.key.startsWith(this.volume.key + ':') && d.level === wanted.level &&
      [0, 1, 2].every((k) => d.min[k] <= wanted.seen[2 * k] && d.max[k] >= wanted.seen[2 * k + 1]);
    if (wanted && (covers(this.detail) || covers(this.detailPending))) return;
    this.detailWanted = key;
    this.detailPending = wanted && {
      key, level: wanted.level,
      min: [0, 2, 4].map((i) => wanted.region[i]),
      max: [1, 3, 5].map((i) => wanted.region[i]),
    };
    if (!wanted) {
      this.detail = null;
      this.requestDraw();
      this.onDetail(null);
      return;
    }
    const [x0, x1, y0, y1, z0, z1] = wanted.region;
    const params = new URLSearchParams({
      step: this.source.step, max: 256, x0, y0, z0, x1, y1, z1,
      low: Math.round(this.volume.window[0]), high: Math.round(this.volume.window[1]),
    });
    if (this.source.porosity !== null) params.set('porosity', this.source.porosity);
    // Only the latest region matters; a request for an earlier one stops.
    this.detailAbort?.abort();
    this.detailAbort = new AbortController();
    let response;
    try {
      response = await fetch('api/volume?' + params, { signal: this.detailAbort.signal });
    } finally {
      // A failed request is asked again when the camera moves.
      if (!response?.ok && this.detailPending?.key === key) this.detailPending = null;
    }
    if (!response.ok || this.detailWanted !== key) return;
    const dims = response.headers.get('X-Dims').split(',').map(Number);
    const origin = response.headers.get('X-Origin').split(',').map(Number);
    const level = Number(response.headers.get('X-Level'));
    const buffer = new Uint8Array(await response.arrayBuffer());
    if (this.detailWanted !== key || level >= this.volume.level) return;
    const count = dims[0] * dims[1] * dims[2];
    const scale = 2 ** level;
    this.detail = {
      key, dims, level,
      min: origin.map((o) => o * scale),                // level-0 voxels
      max: origin.map((o, k) => (o + dims[k]) * scale),
      grey: buffer.subarray(0, count),
      overlay: buffer.subarray(count, 2 * count),
    };
    this.detailPending = null;
    this.detailUploaded = false;
    this.requestDraw();
    this.onDetail(this.detail);
  }

  upload() {
    const gl = this.gl;
    const [x, y, z] = this.volume.dims;
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const texture = (unit, data, filter) => {
      const handle = gl.createTexture();
      gl.activeTexture(gl.TEXTURE0 + unit);
      gl.bindTexture(gl.TEXTURE_3D, handle);
      gl.texImage3D(gl.TEXTURE_3D, 0, gl.R8, x, y, z, 0, gl.RED, gl.UNSIGNED_BYTE, data);
      gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, filter);
      gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, filter);
      for (const wrap of [gl.TEXTURE_WRAP_S, gl.TEXTURE_WRAP_T, gl.TEXTURE_WRAP_R]) {
        gl.texParameteri(gl.TEXTURE_3D, wrap, gl.CLAMP_TO_EDGE);
      }
      return handle;
    };
    if (this.textures) this.textures.forEach((t) => gl.deleteTexture(t));
    this.textures = [texture(0, this.volume.grey, gl.LINEAR),
      texture(1, this.volume.overlay, gl.NEAREST)];
    this.uploaded = true;
  }

  uploadDetail() {
    const gl = this.gl;
    if (this.detailTextures) this.detailTextures.forEach((t) => gl.deleteTexture(t));
    this.detailUploaded = true;
    // Without a detail, a single voxel keeps the samplers complete.
    const [x, y, z] = this.detail ? this.detail.dims : [1, 1, 1];
    const empty = new Uint8Array(1);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const texture = (unit, data, filter) => {
      const handle = gl.createTexture();
      gl.activeTexture(gl.TEXTURE0 + unit);
      gl.bindTexture(gl.TEXTURE_3D, handle);
      gl.texImage3D(gl.TEXTURE_3D, 0, gl.R8, x, y, z, 0, gl.RED, gl.UNSIGNED_BYTE, data);
      gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, filter);
      gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, filter);
      for (const wrap of [gl.TEXTURE_WRAP_S, gl.TEXTURE_WRAP_T, gl.TEXTURE_WRAP_R]) {
        gl.texParameteri(gl.TEXTURE_3D, wrap, gl.CLAMP_TO_EDGE);
      }
      return handle;
    };
    this.detailTextures = [texture(3, this.detail?.grey ?? empty, gl.LINEAR),
      texture(4, this.detail?.overlay ?? empty, gl.NEAREST)];
  }

  /// Sets the transfer function as 256 RGBA bytes (see lookupTable in transfer.js).
  setTransfer(table) {
    this.transfer = table;
    this.transferUploaded = false;
    this.requestDraw();
  }

  uploadTransfer() {
    const gl = this.gl;
    if (!this.transferTexture) this.transferTexture = gl.createTexture();
    gl.activeTexture(gl.TEXTURE2);
    gl.bindTexture(gl.TEXTURE_2D, this.transferTexture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const table = this.transfer ?? new Uint8Array(256 * 4);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, table);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    this.transferUploaded = true;
  }

  set(settings) {
    Object.assign(this, settings);
    this.requestDraw();
  }

  requestDraw() {
    if (this.pending || !this.gl) return;
    this.pending = true;
    requestAnimationFrame(() => {
      this.pending = false;
      this.draw();
    });
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
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    if (!this.volume || !this.program) return;
    if (!this.uploaded) this.upload();
    if (!this.detailUploaded) this.uploadDetail();
    if (!this.transferUploaded) this.uploadTransfer();
    const [x, y, z] = this.volume.dims;
    // The box in true proportions: voxel counts times edge lengths.
    const [sx, sy, sz] = this.volume.dims.map((d, a) => d * this.volume.voxelSize[a]);
    const largest = Math.max(sx, sy, sz);
    const { eye, forward, right, up } = this.camera();
    gl.useProgram(this.program);
    const uniform = (name) => gl.getUniformLocation(this.program, name);
    gl.uniform1i(uniform('grey'), 0);
    gl.uniform1i(uniform('overlay'), 1);
    gl.uniform1i(uniform('transfer'), 2);
    gl.uniform1i(uniform('detailGrey'), 3);
    gl.uniform1i(uniform('detailOverlay'), 4);
    const detail = this.detail;
    gl.uniform1i(uniform('hasDetail'), detail ? 1 : 0);
    if (detail) {
      // Level-0 voxels to texture coordinates of the volume.
      const extent = this.volume.dims.map((d) => d * 2 ** this.volume.level);
      gl.uniform3fv(uniform('detailMin'), detail.min.map((m, k) => m / extent[k]));
      gl.uniform3fv(uniform('detailMax'), detail.max.map((m, k) => m / extent[k]));
      gl.uniform3fv(uniform('detailVoxel'), detail.dims.map((d) => 1 / d));
    } else {
      gl.uniform3fv(uniform('detailMin'), [0, 0, 0]);
      gl.uniform3fv(uniform('detailMax'), [1, 1, 1]);
      gl.uniform3fv(uniform('detailVoxel'), [1 / x, 1 / y, 1 / z]);
    }
    gl.uniform3fv(uniform('eye'), eye);
    gl.uniform3fv(uniform('right'), right);
    gl.uniform3fv(uniform('up'), up);
    gl.uniform3fv(uniform('forward'), forward);
    gl.uniform1f(uniform('aspect'), width / height);
    gl.uniform3fv(uniform('box'), [sx / largest / 2, sy / largest / 2, sz / largest / 2]);
    gl.uniform3fv(uniform('voxel'), [1 / x, 1 / y, 1 / z]);
    gl.uniform1f(uniform('threshold'), this.threshold);
    gl.uniform1i(uniform('mode'), this.mode);
    gl.uniform1i(uniform('pores'), this.pores ? 1 : 0);
    gl.uniform1f(uniform('cut'), this.cut);
    gl.uniform1i(uniform('shading'), this.shading ? 1 : 0);
    const color = (name, rgb) => gl.uniform3fv(uniform(name), rgb.map((c) => c / 255));
    color('surfaceColor', this.surfaceColor);
    color('poreColor', this.poreColor);
    color('zoneColor', this.zoneColor);
    color('background', this.background.colors[0]);
    color('background2', this.background.colors[1]);
    gl.uniform1i(uniform('backgroundStyle'), this.background.style);
    gl.bindVertexArray(this.vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    this.scheduleDetail();
    const meshKind = { 3: 'surface', 4: 'deviation' }[this.mode];
    if (meshKind && this.surface?.kind === meshKind) {
      this.drawMesh(eye, right, up, forward, width / height,
        [sx / largest / 2, sy / largest / 2, sz / largest / 2]);
    }
  }
}

function normalize(v) {
  const length = Math.hypot(...v);
  return v.map((c) => c / length);
}

function cross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

window.VolumeViewer = VolumeViewer;
window.BACKGROUNDS = BACKGROUNDS;
