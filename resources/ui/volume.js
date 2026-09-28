// Simple 3D view of VoxelSieve Studio (ADR 0008): ray casting of a coarse level of the dataset
// (at most 256 voxels per axis) in WebGL2. "Oberfläche" shades the part surface at a threshold,
// "Transferfunktion" composites colour and opacity per grey value (transfer.js) and
// "Maximumprojektion" shows the densest value along each ray. Pores and zones of a porosity
// analysis are drawn in their own colours. A cut along x opens the part. "Extrahierte Oberfläche"
// draws the mesh of a surface step (ADR 0009) instead of the grey values, "Soll-Ist-Abweichung"
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
  studio: { name: 'Studio hell', style: 2, colors: [[232, 234, 237], [150, 157, 166]] },
  studioDunkel: { name: 'Studio dunkel', style: 2, colors: [[70, 76, 86], [14, 16, 19]] },
  verlauf: { name: 'Verlauf blau', style: 1, colors: [[58, 72, 96], [10, 12, 18]] },
  verlaufGrau: { name: 'Verlauf grau', style: 1, colors: [[96, 100, 106], [26, 28, 31]] },
  schwarz: { name: 'Schwarz', style: 0, colors: [[18, 18, 18], [18, 18, 18]] },
  weiss: { name: 'Weiß', style: 0, colors: [[255, 255, 255], [255, 255, 255]] },
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

vec3 gradientAt(vec3 t) {
  return vec3(
    texture(grey, t + vec3(voxel.x, 0, 0)).r - texture(grey, t - vec3(voxel.x, 0, 0)).r,
    texture(grey, t + vec3(0, voxel.y, 0)).r - texture(grey, t - vec3(0, voxel.y, 0)).r,
    texture(grey, t + vec3(0, 0, voxel.z)).r - texture(grey, t - vec3(0, 0, voxel.z)).r);
}

vec3 normalAt(vec3 t) {
  vec3 g = gradientAt(t);
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
  return int(texture(overlay, t).r * 255.0 + 0.5);
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
  float stepSize = min(min(voxel.x * box.x, voxel.y * box.y), voxel.z * box.z) * 1.2;
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
    if (tex.x > cut) {
      entered = false;
      continue;
    }
    int kind = classAt(tex);
    float value = texture(grey, tex).r;
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
      surfaceColor: this.surfaceColor, poreColor: this.poreColor, zoneColor: this.zoneColor,
      background: this.background,
    };
  }

  setState(saved) {
    const keys = ['mode', 'shading', 'pores', 'cut', 'threshold', 'yaw', 'pitch', 'distance',
      'surfaceColor', 'poreColor', 'zoneColor', 'background'];
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
      voxelSize: Number(response.headers.get('X-Voxel-Size')),
      grey: buffer.subarray(0, count),
      overlay: buffer.subarray(count, 2 * count),
    };
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
    if (!this.gl) throw new Error('Der Browser unterstützt kein WebGL2');
    // A new canvas has a new context; textures of the old one are gone with it.
    this.uploaded = false;
    this.textures = null;
    this.transferUploaded = false;
    this.transferTexture = null;
    this.program = this.createProgram();
    this.meshProgram = this.createMeshProgram();
    this.meshBuffers = null;
    let drag = null;
    canvas.addEventListener('pointerdown', (event) => {
      drag = { x: event.clientX, y: event.clientY, yaw: this.yaw, pitch: this.pitch };
      canvas.setPointerCapture(event.pointerId);
    });
    canvas.addEventListener('pointermove', (event) => {
      if (!drag) return;
      this.yaw = drag.yaw - (event.clientX - drag.x) * 0.01;
      this.pitch = Math.min(Math.max(drag.pitch + (event.clientY - drag.y) * 0.01, -1.5), 1.5);
      this.requestDraw();
    });
    canvas.addEventListener('pointerup', () => {
      if (drag) this.onInteract();
      drag = null;
    });
    canvas.addEventListener('wheel', (event) => {
      event.preventDefault();
      this.distance = Math.min(Math.max(this.distance * Math.exp(event.deltaY * 0.001), 0.6), 8);
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
    if (!this.transferUploaded) this.uploadTransfer();
    const [x, y, z] = this.volume.dims;
    const largest = Math.max(x, y, z);
    // Camera orbiting the centre, z up.
    const eye = [
      this.distance * Math.cos(this.pitch) * Math.cos(this.yaw),
      this.distance * Math.cos(this.pitch) * Math.sin(this.yaw),
      this.distance * Math.sin(this.pitch),
    ];
    const forward = normalize(eye.map((c) => -c));
    const right = normalize(cross(forward, [0, 0, 1]));
    const up = cross(right, forward);
    gl.useProgram(this.program);
    const uniform = (name) => gl.getUniformLocation(this.program, name);
    gl.uniform1i(uniform('grey'), 0);
    gl.uniform1i(uniform('overlay'), 1);
    gl.uniform1i(uniform('transfer'), 2);
    gl.uniform3fv(uniform('eye'), eye);
    gl.uniform3fv(uniform('right'), right);
    gl.uniform3fv(uniform('up'), up);
    gl.uniform3fv(uniform('forward'), forward);
    gl.uniform1f(uniform('aspect'), width / height);
    gl.uniform3fv(uniform('box'), [x / largest / 2, y / largest / 2, z / largest / 2]);
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
    const meshKind = { 3: 'surface', 4: 'deviation' }[this.mode];
    if (meshKind && this.surface?.kind === meshKind) {
      this.drawMesh(eye, right, up, forward, width / height,
        [x / largest / 2, y / largest / 2, z / largest / 2]);
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
