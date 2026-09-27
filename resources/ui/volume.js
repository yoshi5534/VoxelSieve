// Simple 3D view of VoxelSieve Studio (ADR 0008): ray casting of a coarse level of the dataset
// (at most 256 voxels per axis) in WebGL2. "Oberfläche" shades the part surface, "Durchsicht"
// shows the part translucent with its pores and zones inside. A cut along x opens the part.
'use strict';

const VOLUME_VERTEX = `#version 300 es
in vec2 position;
out vec2 ndc;
void main() {
  ndc = position;
  gl_Position = vec4(position, 0.0, 1.0);
}`;

const VOLUME_FRAGMENT = `#version 300 es
precision highp float;
precision highp sampler3D;
in vec2 ndc;
out vec4 color;
uniform sampler3D grey;
uniform sampler3D overlay;
uniform vec3 eye;
uniform vec3 right;
uniform vec3 up;
uniform vec3 forward;
uniform float aspect;
uniform vec3 box;        // half extent of the volume, largest axis 0.5
uniform vec3 voxel;      // one voxel in texture coordinates
uniform float threshold; // material threshold on the 0..1 grey scale
uniform int mode;        // 0 surface, 1 translucent
uniform bool pores;
uniform float cut;       // texture x beyond which the part is cut away

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

vec3 normalAt(vec3 t) {
  vec3 g = vec3(
    texture(grey, t + vec3(voxel.x, 0, 0)).r - texture(grey, t - vec3(voxel.x, 0, 0)).r,
    texture(grey, t + vec3(0, voxel.y, 0)).r - texture(grey, t - vec3(0, voxel.y, 0)).r,
    texture(grey, t + vec3(0, 0, voxel.z)).r - texture(grey, t - vec3(0, 0, voxel.z)).r);
  return length(g) > 1e-5 ? -normalize(g) : vec3(0.0);
}

vec3 shade(vec3 base, vec3 normal, vec3 direction) {
  vec3 light = normalize(-direction + up * 0.4 + right * 0.3);
  float diffuse = abs(dot(normal, light));
  return base * (0.25 + 0.75 * diffuse);
}

int classAt(vec3 t) {
  return int(texture(overlay, t).r * 255.0 + 0.5);
}

void main() {
  vec3 direction = normalize(forward + ndc.x * aspect * 0.35 * right + ndc.y * 0.35 * up);
  float near;
  float far;
  vec3 background = vec3(0.07);
  if (!hitBox(eye, direction, near, far)) {
    color = vec4(background, 1.0);
    return;
  }
  near = max(near, 0.0);
  float stepSize = min(min(voxel.x * box.x, voxel.y * box.y), voxel.z * box.z) * 1.2;
  vec3 accumulated = vec3(0.0);
  float alpha = 0.0;
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
        vec3 n = normalAt(tex);
        color = vec4(shade(vec3(0.9, 0.15, 0.12), n, direction), 1.0);
        return;
      }
      if (value >= threshold) {
        // On the cut face the grey gradient says nothing; light it as the plane it is.
        bool onCut = cut < 1.0 && tex.x > cut - 1.5 * voxel.x && !entered;
        vec3 n = onCut ? vec3(1.0, 0.0, 0.0) : normalAt(tex);
        vec3 base = (pores && kind == 2) ? vec3(0.95, 0.8, 0.3) : vec3(0.82, 0.84, 0.86);
        color = vec4(shade(base, n, direction), 1.0);
        return;
      }
      entered = true;
    } else {
      vec4 sample_ = vec4(0.0);
      if (pores && kind == 1) {
        sample_ = vec4(0.95, 0.15, 0.1, 0.5);
      } else if (pores && kind == 2) {
        sample_ = vec4(1.0, 0.8, 0.2, 0.08);
      } else if (value >= threshold) {
        sample_ = vec4(0.85, 0.88, 0.92, 0.012);
      }
      accumulated += (1.0 - alpha) * sample_.a * sample_.rgb;
      alpha += (1.0 - alpha) * sample_.a;
      if (alpha > 0.97) break;
    }
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
    this.yaw = 0.8;
    this.pitch = 0.45;
    this.distance = 2.2;
    this.canvas = null;
    this.gl = null;
    this.pending = false;
    this.onChange = () => {};
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
    // Default threshold halfway between air and material.
    this.threshold = this.suggestThreshold();
    this.uploaded = false;
    this.requestDraw();
    this.onChange();
  }

  /// Middle between the two main peaks of the grey histogram (air and material).
  suggestThreshold() {
    const histogram = new Array(256).fill(0);
    for (let i = 0; i < this.volume.grey.length; i += 3) histogram[this.volume.grey[i]] += 1;
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
    this.uploaded = false;
    this.program = this.createProgram();
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
    canvas.addEventListener('pointerup', () => { drag = null; });
    canvas.addEventListener('wheel', (event) => {
      event.preventDefault();
      this.distance = Math.min(Math.max(this.distance * Math.exp(event.deltaY * 0.001), 0.6), 8);
      this.requestDraw();
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
    const ratio = window.devicePixelRatio || 1;
    const width = Math.round(canvas.clientWidth * ratio);
    const height = Math.round(canvas.clientHeight * ratio);
    if (width === 0 || height === 0) return;
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    gl.viewport(0, 0, width, height);
    gl.clearColor(0.07, 0.07, 0.07, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    if (!this.volume) return;
    if (!this.uploaded) this.upload();
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
    gl.bindVertexArray(this.vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
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
