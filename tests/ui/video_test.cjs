// Unit tests of the camera motions and formats of the video of the 3D view
// (resources/ui/video.js), run with node.
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const { VIDEO_MOTIONS, cameraAt, ease, turnTowards, videoBitrate, videoFormat, videoSize } =
  require(path.join(__dirname, '../../resources/ui/video.js'));

const near = (a, b, what, tolerance = 1e-9) => assert.ok(Math.abs(a - b) <= tolerance,
  `${what}: ${a} is not ${b}`);
const start = { yaw: 0.8, pitch: 0.45, distance: 2.2, target: [0.1, 0, 0], cut: 1 };

// Every motion starts where the view is, and the closed ones end there too, so a video loops.
for (const motion of Object.keys(VIDEO_MOTIONS)) {
  const first = cameraAt(motion, start, 0);
  near(first.yaw, start.yaw, motion + ' starts at the view');
  near(first.pitch, start.pitch, motion + ' pitch');
  near(first.cut, 1, motion + ' cut');
  assert.deepEqual(first.target, start.target);
  if (motion === 'zoom') continue;
  const last = cameraAt(motion, start, 1);
  near(Math.cos(last.yaw), Math.cos(start.yaw), motion + ' ends where it began', 1e-9);
  near(Math.sin(last.yaw), Math.sin(start.yaw), motion + ' ends where it began', 1e-9);
  near(last.pitch, start.pitch, motion + ' pitch at the end', 1e-9);
  near(last.cut, 1, motion + ' cut at the end', 1e-9);
}

// The turntable turns once at a steady rate.
near(cameraAt('turntable', start, 0.25).yaw - start.yaw, Math.PI / 2, 'a quarter turn');
// The cut sweep goes to a tenth of the part halfway.
near(cameraAt('cut', start, 0.5).cut, 0.1, 'the cut at its deepest');
// Zooming ends at half the distance, a quarter turn on.
near(cameraAt('zoom', start, 1).distance, 1.1, 'zoomed');
near(cameraAt('zoom', start, 1).yaw - start.yaw, Math.PI / 2, 'turned');
// The orbit stays away from the poles.
for (let t = 0; t <= 1; t += 0.05) {
  assert.ok(Math.abs(cameraAt('orbit', { ...start, pitch: 1.3 }, t).pitch) <= 1.45);
}

// Through saved views: at the views at t = 0, 1/2 and 1, the shorter way round between them.
{
  const views = [
    { yaw: 3.0, pitch: 0.2, distance: 2, target: [0, 0, 0], cut: 1 },
    { yaw: -3.0, pitch: 0.6, distance: 1, target: [0.2, 0, 0], cut: 0.5 },
    { yaw: -2.0, pitch: 0.4, distance: 3, target: [0, 0.2, 0] },
  ];
  const at = (t) => cameraAt('views', start, t, views);
  near(at(0).yaw, 3.0, 'first view');
  near(at(0.5).distance, 1, 'second view');
  near(at(0.5).cut, 0.5, 'its cut');
  near(at(1).pitch, 0.4, 'third view');
  near(at(1).cut, 1, 'a view without cut is uncut');
  // From 3.0 to -3.0 the short way is about 0.28 rad across ±π, not 6 rad back.
  const middle = at(0.25).yaw;
  assert.ok(Math.abs(Math.cos(middle) + 1) < 0.02, 'across the back: ' + middle);
  // With one view the flight turns like the turntable.
  near(cameraAt('views', start, 0.25, [views[0]]).yaw - start.yaw, Math.PI / 2, 'fallback');
}

near(ease(0), 0, 'ease start');
near(ease(1), 1, 'ease end');
near(ease(0.5), 0.5, 'ease middle');
near(turnTowards(3, -3, 1) % (2 * Math.PI), 3 + (2 * Math.PI - 6), 'short way');

// Formats: MP4 first where the browser records it, else WebM, else none.
assert.deepEqual(videoFormat(() => true),
  { mime: 'video/mp4;codecs=avc1.640033', extension: 'mp4' });
assert.deepEqual(videoFormat((type) => type.startsWith('video/webm')),
  { mime: 'video/webm;codecs=vp9', extension: 'webm' });
assert.equal(videoFormat(() => false), null);

// Sizes are even, as encoders need; the view's own size comes from the canvas.
assert.deepEqual(videoSize('fullhd', null), { width: 1920, height: 1080 });
assert.deepEqual(videoSize('view', { clientWidth: 801, clientHeight: 601 }),
  { width: 800, height: 600 });
assert.ok(videoBitrate(1920, 1080, 30) >= 2e6 && videoBitrate(3840, 2160, 60) <= 80e6);
console.log('video tests passed');
