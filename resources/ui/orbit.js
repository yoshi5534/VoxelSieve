// What the two 3D views (volume.js, scene.js) share: the camera that orbits a point with z up,
// its pointer input, shader programs, and one WebGL2 context per canvas that is kept while the
// canvas lives and set up again when the browser takes it away (context loss).
'use strict';

/// Eye position and view axes of a camera with `yaw`, `pitch` and `distance` around `target`.
function orbitCamera({ yaw, pitch, distance, target }) {
  const offset = [
    distance * Math.cos(pitch) * Math.cos(yaw),
    distance * Math.cos(pitch) * Math.sin(yaw),
    distance * Math.sin(pitch),
  ];
  const eye = offset.map((c, i) => c + target[i]);
  const forward = orbitNormalize(offset.map((c) => -c));
  const right = orbitNormalize(orbitCross(forward, [0, 0, 1]));
  const up = orbitCross(right, forward);
  return { eye, forward, right, up };
}

/// Pointer input of an orbit view: left drag turns, right or Shift drag moves the target with
/// the picture, the wheel zooms. `view` holds yaw, pitch, distance and target and has
/// requestDraw() and onInteract(); `view.moving` is true while the camera moves, so the view can
/// draw coarser meanwhile. `options`: minDistance, maxDistance, clampTarget (a function of the
/// new target), onClick (a click without movement).
function attachOrbitInput(canvas, view, options = {}) {
  const { minDistance = 0.05, maxDistance = 8, clampTarget = (t) => t, onClick = null } = options;
  let drag = null;
  let rest = null;
  // Moving until the camera has rested for a moment.
  const moved = () => {
    view.moving = true;
    clearTimeout(rest);
    rest = setTimeout(() => {
      view.moving = false;
      view.requestDraw();
    }, 200);
    view.requestDraw();
  };
  canvas.addEventListener('contextmenu', (event) => event.preventDefault());
  canvas.addEventListener('pointerdown', (event) => {
    drag = {
      x: event.clientX, y: event.clientY, yaw: view.yaw, pitch: view.pitch,
      target: [...view.target], pan: event.button === 2 || event.shiftKey, moved: false,
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
      const { right, up } = orbitCamera(view);
      // The picture moves with the pointer: one canvas height spans 0.7 distance units.
      const scale = 0.7 * view.distance / Math.max(canvas.clientHeight, 1);
      view.target = clampTarget(drag.target.map((c, i) => c - (right[i] * dx - up[i] * dy) * scale));
    } else {
      view.yaw = drag.yaw - dx * 0.01;
      view.pitch = Math.min(Math.max(drag.pitch + dy * 0.01, -1.5), 1.5);
    }
    moved();
  });
  canvas.addEventListener('pointerup', (event) => {
    if (drag?.moved) view.onInteract();
    else if (drag && onClick && event.button === 0) onClick(event);
    drag = null;
  });
  canvas.addEventListener('wheel', (event) => {
    event.preventDefault();
    view.distance = Math.min(Math.max(view.distance * Math.exp(event.deltaY * 0.001), minDistance),
      maxDistance);
    moved();
    view.onInteract();
  }, { passive: false });
  new ResizeObserver(() => view.requestDraw()).observe(canvas);
}

/// The WebGL2 context of a canvas. `setup(gl)` creates everything the view needs on it, now and
/// again after a context loss; `lost()` drops what belonged to the old context.
function attachContext(canvas, setup, lost) {
  const gl = canvas.getContext('webgl2');
  if (!gl) {
    throw new Error('The 3D view needs WebGL2, which this browser has turned off. Try Edge or ' +
      'Chrome; in Firefox set webgl.force-enabled to true in about:config, or update the ' +
      'graphics driver.');
  }
  canvas.addEventListener('webglcontextlost', (event) => {
    // Asks the browser to restore the context.
    event.preventDefault();
    lost();
  });
  canvas.addEventListener('webglcontextrestored', () => setup(gl));
  setup(gl);
  return gl;
}

function compileProgram(gl, vertex, fragment) {
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
  gl.attachShader(program, compile(gl.VERTEX_SHADER, vertex));
  gl.attachShader(program, compile(gl.FRAGMENT_SHADER, fragment));
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    throw new Error(gl.getProgramInfoLog(program));
  }
  return program;
}

function orbitNormalize(v) {
  const length = Math.hypot(...v);
  return v.map((c) => c / length);
}

function orbitCross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

if (typeof window !== 'undefined') {
  Object.assign(window, { orbitCamera, attachOrbitInput, attachContext, compileProgram });
}
if (typeof module !== 'undefined') {
  module.exports = { orbitCamera };
}
