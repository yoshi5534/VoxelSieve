// Video of the 3D view of VoxelSieve (ADR 0043): a camera motion (turntable, swing, orbit, cut
// sweep, zoom, or a flight through the saved views) rendered frame by frame by the view itself and
// encoded by the browser (MediaRecorder: MP4 where the browser writes it, else WebM), so no
// encoder is part of VoxelSieve. The motions are pure functions of time and run in
// node for their tests.
'use strict';

const VIDEO_MOTIONS = {
  turntable: { name: 'Turntable', description: 'One full turn about the vertical axis' },
  swing: { name: 'Swing', description: 'Turns 40° to each side and back' },
  orbit: { name: 'Orbit', description: 'A full turn that looks from above and from below' },
  cut: { name: 'Cut sweep', description: 'The cut moves into the part and back, turning a little' },
  zoom: { name: 'Zoom in', description: 'Moves closer while turning a quarter' },
  views: { name: 'Saved views',
    description: 'Flies from one saved view of the project to the next' },
};

const VIDEO_SIZES = {
  view: { name: 'As the view', width: 0, height: 0 },
  hd: { name: '1280 × 720', width: 1280, height: 720 },
  fullhd: { name: '1920 × 1080', width: 1920, height: 1080 },
  square: { name: '1080 × 1080', width: 1080, height: 1080 },
  uhd: { name: '3840 × 2160', width: 3840, height: 2160 },
};

/// Smooth start and end of a motion: 0..1 to 0..1 with zero speed at both ends.
function ease(t) {
  const x = Math.min(Math.max(t, 0), 1);
  return x * x * (3 - 2 * x);
}

/// Angle a turned towards b the shorter way round, at fraction f.
function turnTowards(a, b, f) {
  let d = (b - a) % (2 * Math.PI);
  if (d > Math.PI) d -= 2 * Math.PI;
  if (d < -Math.PI) d += 2 * Math.PI;
  return a + d * f;
}

/// The camera (yaw, pitch, distance, target) and cut of `motion` at time t in 0..1, starting from
/// the camera `start`. `keyframes` are the view states the 'views' motion flies through; with
/// fewer than two it turns like the turntable.
function cameraAt(motion, start, t, keyframes = []) {
  const x = Math.min(Math.max(t, 0), 1);
  const camera = { yaw: start.yaw, pitch: start.pitch, distance: start.distance,
    target: [...(start.target ?? [0, 0, 0])], cut: start.cut ?? 1 };
  const maxPitch = 1.45;
  switch (motion) {
    case 'swing':
      camera.yaw = start.yaw + (40 * Math.PI / 180) * Math.sin(2 * Math.PI * x);
      break;
    case 'orbit':
      camera.yaw = start.yaw + 2 * Math.PI * x;
      camera.pitch = Math.min(Math.max(start.pitch + 0.45 * Math.sin(2 * Math.PI * x),
        -maxPitch), maxPitch);
      break;
    case 'cut': {
      // In to a tenth of the part and out again, so the inside shows layer by layer.
      const depth = Math.sin(Math.PI * x);
      camera.cut = (start.cut ?? 1) + (0.1 - (start.cut ?? 1)) * depth * depth;
      camera.yaw = start.yaw + (30 * Math.PI / 180) * Math.sin(Math.PI * x);
      break;
    }
    case 'zoom':
      camera.distance = start.distance * (1 - 0.5 * ease(x));
      camera.yaw = start.yaw + (Math.PI / 2) * ease(x);
      break;
    case 'views':
      if (keyframes.length >= 2) {
        const segments = keyframes.length - 1;
        const s = Math.min(Math.floor(x * segments), segments - 1);
        const f = ease(x * segments - s);
        const a = keyframes[s];
        const b = keyframes[s + 1];
        const mix = (p, q) => p + (q - p) * f;
        camera.yaw = turnTowards(a.yaw, b.yaw, f);
        camera.pitch = mix(a.pitch, b.pitch);
        camera.distance = mix(a.distance, b.distance);
        const ta = a.target ?? [0, 0, 0];
        const tb = b.target ?? [0, 0, 0];
        camera.target = ta.map((c, k) => mix(c, tb[k]));
        camera.cut = mix(a.cut ?? 1, b.cut ?? 1);
        break;
      }
      camera.yaw = start.yaw + 2 * Math.PI * x;
      break;
    default:  // turntable
      camera.yaw = start.yaw + 2 * Math.PI * x;
  }
  return camera;
}

/// The best format the browser records a canvas in: {mime, extension}, MP4 (H.264) first, or
/// null when it records none.
function videoFormat(isSupported = (type) => typeof MediaRecorder !== 'undefined' &&
  MediaRecorder.isTypeSupported(type)) {
  const candidates = [
    ['video/mp4;codecs=avc1.640033', 'mp4'], ['video/mp4;codecs=avc1', 'mp4'], ['video/mp4', 'mp4'],
    ['video/webm;codecs=vp9', 'webm'], ['video/webm;codecs=vp8', 'webm'], ['video/webm', 'webm'],
  ];
  const found = candidates.find(([mime]) => isSupported(mime));
  return found ? { mime: found[0], extension: found[1] } : null;
}

/// Bits per second for a clear picture of that size and frame rate (about 0.12 bits per pixel).
function videoBitrate(width, height, fps) {
  return Math.round(Math.min(Math.max(width * height * fps * 0.12, 2e6), 80e6));
}

/// Even pixel sizes of a video: `size` from VIDEO_SIZES, 'view' being the canvas as shown.
function videoSize(size, canvas) {
  const chosen = VIDEO_SIZES[size] ?? VIDEO_SIZES.fullhd;
  const ratio = typeof window !== 'undefined' ? window.devicePixelRatio || 1 : 1;
  const width = chosen.width || Math.round(canvas.clientWidth * ratio);
  const height = chosen.height || Math.round(canvas.clientHeight * ratio);
  return { width: Math.max(2, width - (width % 2)), height: Math.max(2, height - (height % 2)) };
}

const pause = (ms) => new Promise((resolve) => { setTimeout(resolve, ms); });
const nextFrame = () => new Promise((resolve) => { requestAnimationFrame(() => resolve()); });

/// Records a video of a view that has getState/setState, draw(), its canvas and, optionally,
/// streamStatus() (the bricks of the volume view) and `renderSize` (the picture's size in
/// pixels, else the canvas as shown). Three phases, reported to onProgress({phase, done}):
/// 'prepare' runs through the motion once so the bricks it needs are loaded; 'render' draws
/// every frame and keeps it as a JPEG; 'encode' plays the frames at their rate into the
/// browser's recorder, which stamps frames with the time they arrive, so a view that draws
/// slowly still gives a video of the right length. Resolves to {blob, extension, frames,
/// seconds}; `signal` (an AbortSignal) stops it early with the frames drawn until then. The
/// view is left as it was.
async function recordVideo(view, options) {
  const { motion = 'turntable', seconds = 8, fps = 30, size = 'fullhd', keyframes = [],
    onProgress = () => {}, signal = null } = options;
  const format = videoFormat();
  if (!format || !HTMLCanvasElement.prototype.captureStream) {
    throw new Error('This browser cannot record video from the view; Chrome, Edge and Firefox can');
  }
  const saved = view.getState();
  const start = { yaw: saved.yaw, pitch: saved.pitch, distance: saved.distance,
    target: saved.target, cut: saved.cut };
  const { width, height } = videoSize(size, view.canvas);
  const count = Math.max(2, Math.round(seconds * fps));
  const show = (t) => {
    Object.assign(view, cameraAt(motion, start, t, keyframes));
    view.changed = true;
    view.draw();
  };
  const frames = [];
  view.renderSize = { width, height };
  view.recording = true;
  try {
    // Warm up: the bricks along the motion come in while nothing is kept yet.
    const stops = view.streamStatus ? 12 : 0;
    for (let i = 0; i < stops && !signal?.aborted; i += 1) {
      show(i / stops);
      onProgress({ phase: 'prepare', done: i / stops });
      for (let wait = 0; wait < 40; wait += 1) {
        await nextFrame();
        view.draw();
        if (!view.streamStatus()?.loading) break;
        await pause(25);
      }
    }
    for (let i = 0; i < count && !signal?.aborted; i += 1) {
      show(i / (count - 1));
      // The canvas is copied when toBlob is called, right after it was drawn.
      frames.push(await new Promise((resolve) => {
        view.canvas.toBlob(resolve, 'image/jpeg', 0.95);
      }));
      onProgress({ phase: 'render', done: (i + 1) / count });
      await nextFrame();
    }
  } finally {
    view.renderSize = null;
    view.recording = false;
    view.setState(saved);
  }
  if (frames.length < 2) throw new Error('The video was stopped before it had two frames');

  // Encode: the frames at their rate on a canvas of their own, decoded a few ahead.
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  canvas.style.cssText = 'position: fixed; left: -100000px; top: 0;';
  document.body.append(canvas);
  const context = canvas.getContext('2d');
  const stream = canvas.captureStream(0);
  const [track] = stream.getVideoTracks();
  try {
    const options = { mimeType: format.mime, videoBitsPerSecond: videoBitrate(width, height, fps) };
    // The first frame an encoder of the browser gets can hold the page for most of a second
    // while it starts; a short recording that is thrown away takes that pause.
    {
      const warm = new MediaRecorder(stream, options);
      const done = new Promise((resolve) => { warm.onstop = resolve; });
      warm.start();
      for (let i = 0; i < 3; i += 1) {
        context.drawImage(await createImageBitmap(frames[0]), 0, 0, width, height);
        if (track.requestFrame) track.requestFrame();
        await pause(1000 / fps);
      }
      warm.stop();
      await done;
    }
    const recorder = new MediaRecorder(stream, options);
    const chunks = [];
    recorder.ondataavailable = (event) => {
      if (event.data.size > 0) chunks.push(event.data);
    };
    const stopped = new Promise((resolve) => { recorder.onstop = resolve; });
    const ahead = [];
    const decode = (i) => {
      if (i < frames.length) ahead.push(createImageBitmap(frames[i]));
    };
    for (let i = 0; i < 8; i += 1) decode(i);
    await Promise.all(ahead);
    recorder.start(1000);
    let begin = performance.now();
    for (let i = 0; i < frames.length; i += 1) {
      const bitmap = await ahead.shift();
      decode(i + 8);
      const left = begin + (i * 1000) / fps - performance.now();
      // A frame decoded late moves the rest back, so the frames keep their spacing.
      if (left > 1) await pause(left);
      else if (left < 0) begin -= left;
      context.drawImage(bitmap, 0, 0, width, height);
      bitmap.close();
      if (track.requestFrame) track.requestFrame();
      onProgress({ phase: 'encode', done: (i + 1) / frames.length });
    }
    // The last frame lasts its time too.
    await pause(1000 / fps);
    recorder.stop();
    await stopped;
    return { blob: new Blob(chunks, { type: format.mime.split(';')[0] }),
      extension: format.extension, frames: frames.length, seconds: frames.length / fps };
  } finally {
    track.stop();
    canvas.remove();
  }
}

if (typeof window !== 'undefined') {
  Object.assign(window, { VIDEO_MOTIONS, VIDEO_SIZES, cameraAt, recordVideo, videoFormat,
    videoSize });
}
if (typeof module !== 'undefined') {
  module.exports = { VIDEO_MOTIONS, VIDEO_SIZES, cameraAt, ease, turnTowards, videoBitrate,
    videoFormat, videoSize };
}
