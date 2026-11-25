// Lighting and looks of the 3D view of VoxelSieve (ADR 0043). A lighting is a key light given by
// azimuth and elevation, either following the camera (a headlight to one side) or fixed in the
// scene, a weaker fill light from the other side, a rim light on the silhouette, ambient light,
// specular highlights, shadows and ambient occlusion. A look is a whole rendering in one click:
// the curve and colours of the transfer function, the lighting and the background. Runs in the
// browser and, for its tests, in node.
'use strict';

/// Everything a lighting holds, with the values of the headlight the view always had.
const DEFAULT_LIGHTING = {
  preset: 'headlight',
  follow: true,       // the key light turns with the camera; else it is fixed in the scene
  azimuth: 16.7,      // degrees: following, to the right of the view; fixed, about z from +x
  elevation: 21,      // degrees above the view's horizon, or above the xy plane
  intensity: 0.75,    // diffuse part of the key light
  ambient: 0.25,
  specular: 0.25,
  shininess: 24,
  fill: 0,            // a second light from the other side, relative to the key light
  rim: 0,             // light on the silhouette, where the surface turns away
  shadows: 0,         // 0..1: how dark the shadow of the key light is
  occlusion: 0,       // 0..1: how much creases and pores darken in ambient light
};

const LIGHTING_PRESETS = {
  headlight: { name: 'Headlight', description: 'One light from the camera, as before' },
  studio: { name: 'Studio', description: 'Key, fill and rim light with soft shadows',
    azimuth: -35, elevation: 35, intensity: 0.85, ambient: 0.18, specular: 0.35, shininess: 40,
    fill: 0.35, rim: 0.25, shadows: 0.7, occlusion: 0.6 },
  top: { name: 'Top light', description: 'Soft light from above, fixed in the scene',
    follow: false, azimuth: -60, elevation: 70, intensity: 0.8, ambient: 0.3, specular: 0.2,
    shininess: 30, fill: 0.2, rim: 0, shadows: 0.5, occlusion: 0.8 },
  dramatic: { name: 'Dramatic', description: 'Low side light, deep shadows and a bright rim',
    azimuth: 70, elevation: 15, intensity: 1, ambient: 0.06, specular: 0.5, shininess: 60,
    fill: 0.08, rim: 0.6, shadows: 0.9, occlusion: 0.5 },
  backlight: { name: 'Back light', description: 'Light from behind: the outline glows',
    azimuth: 160, elevation: 30, intensity: 0.6, ambient: 0.2, specular: 0.15, shininess: 20,
    fill: 0.3, rim: 0.9, shadows: 0.3, occlusion: 0.4 },
  flat: { name: 'Flat', description: 'Even light without highlights, for measuring by eye',
    azimuth: 0, elevation: 0, intensity: 0.15, ambient: 0.85, specular: 0, shininess: 8,
    fill: 0, rim: 0, shadows: 0, occlusion: 0 },
};

/// The lighting of a preset; unknown names give the headlight.
function lightingPreset(name) {
  const { name: _, description: __, ...values } = LIGHTING_PRESETS[name] ?? {};
  return { ...DEFAULT_LIGHTING, ...values, preset: name in LIGHTING_PRESETS ? name : 'headlight' };
}

/// A saved lighting completed by the defaults; numbers outside their range are clamped, so a
/// hand-edited project cannot break the shader.
function normalizeLighting(saved) {
  const l = { ...DEFAULT_LIGHTING, ...(saved ?? {}) };
  const clamp = (v, low, high, fallback) => (Number.isFinite(Number(v))
    ? Math.min(Math.max(Number(v), low), high) : fallback);
  return {
    preset: typeof l.preset === 'string' ? l.preset : 'eigen',
    follow: l.follow !== false,
    azimuth: clamp(l.azimuth, -180, 180, DEFAULT_LIGHTING.azimuth),
    elevation: clamp(l.elevation, -90, 90, DEFAULT_LIGHTING.elevation),
    intensity: clamp(l.intensity, 0, 2, DEFAULT_LIGHTING.intensity),
    ambient: clamp(l.ambient, 0, 1, DEFAULT_LIGHTING.ambient),
    specular: clamp(l.specular, 0, 1, DEFAULT_LIGHTING.specular),
    shininess: clamp(l.shininess, 1, 200, DEFAULT_LIGHTING.shininess),
    fill: clamp(l.fill, 0, 1, 0),
    rim: clamp(l.rim, 0, 1, 0),
    shadows: clamp(l.shadows, 0, 1, 0),
    occlusion: clamp(l.occlusion, 0, 1, 0),
  };
}

function lightNormalize(v) {
  const length = Math.hypot(...v) || 1;
  return v.map((c) => c / length);
}

/// Unit vectors in world space towards the key and the fill light, for a camera {forward, right,
/// up} (orbitCamera). Following the camera, azimuth turns from the view direction to the right
/// and elevation lifts towards the camera's up; fixed, they are the angles about z from +x and
/// above the xy plane. The fill light comes from the mirrored azimuth, lower.
function lightDirections(lighting, camera) {
  const l = normalizeLighting(lighting);
  const rad = Math.PI / 180;
  const towards = (azimuth, elevation) => {
    const a = azimuth * rad;
    const e = elevation * rad;
    if (l.follow) {
      const { forward, right, up } = camera;
      return lightNormalize([0, 1, 2].map((k) => -forward[k] * Math.cos(e) * Math.cos(a) +
        right[k] * Math.cos(e) * Math.sin(a) + up[k] * Math.sin(e)));
    }
    return [Math.cos(e) * Math.cos(a), Math.cos(e) * Math.sin(a), Math.sin(e)];
  };
  const fillAzimuth = l.follow ? -l.azimuth - 30 * Math.sign(l.azimuth || 1) : l.azimuth + 150;
  return { key: towards(l.azimuth, l.elevation),
    fill: towards(fillAzimuth, Math.max(l.elevation * 0.3, 5)) };
}

/// Looks: a whole rendering in one click. Each names the curve preset of the transfer function
/// (transferPreset in transfer.js) and its colour map, the lighting, the background (BACKGROUNDS
/// in volume.js) and the edge emphasis; `mode` 0 is the lit surface, 1 the transfer function.
const LOOKS = {
  studio: { name: 'Studio', description: 'The part solid in steel under studio light',
    mode: 1, curve: 'massiv', colorMap: 'stahl', lighting: 'studio', background: 'soft', edges: 0 },
  ton: { name: 'Clay', description: 'A matt white surface with soft shadows, shows the form',
    mode: 0, surfaceColor: [226, 222, 214], lighting: 'top', background: 'soft', edges: 0 },
  glas: { name: 'Glass', description: 'Faint surface, pores and inclusions clear inside',
    mode: 1, curve: 'glas', colorMap: 'eis', lighting: 'backlight', background: 'softDark',
    edges: 0.6 },
  gold: { name: 'Gold', description: 'Solid gold under a low side light',
    mode: 1, curve: 'massiv', colorMap: 'gold', lighting: 'dramatic', background: 'softDark',
    edges: 0 },
  roentgen: { name: 'X-ray', description: 'Translucent in bone tones, like a radiograph',
    mode: 1, curve: 'durchsicht', colorMap: 'knochen', lighting: 'flat', background: 'schwarz',
    edges: 0.3, shading: false },
  glut: { name: 'Glow', description: 'Density in Inferno colours, dense parts glow',
    mode: 1, curve: 'innen', colorMap: 'inferno', lighting: 'headlight', background: 'schwarz',
    edges: 0.2 },
};

/// What a look sets, for a volume whose air/material threshold is at `threshold` (0..1):
/// {volume: settings of the VolumeViewer, transfer: the transfer function state}.
function lookSettings(name, threshold, presets = transferPresetFunction()) {
  const look = LOOKS[name] ?? LOOKS.studio;
  const volume = { mode: look.mode, shading: look.shading ?? true,
    lighting: lightingPreset(look.lighting), edges: look.edges, look: name };
  if (look.surfaceColor) volume.surfaceColor = [...look.surfaceColor];
  const transfer = { preset: look.curve ?? 'massiv', colorMap: look.colorMap ?? 'stahl',
    colorSource: 'map', reverse: false, colorRange: null,
    points: presets(look.curve ?? 'massiv', threshold) };
  return { volume, background: look.background, transfer };
}

function transferPresetFunction() {
  if (typeof transferPreset === 'function') return transferPreset;
  return require('./transfer.js').transferPreset;  // in node
}

if (typeof window !== 'undefined') {
  Object.assign(window, { DEFAULT_LIGHTING, LIGHTING_PRESETS, LOOKS, lightDirections,
    lightingPreset, lookSettings, normalizeLighting });
}
if (typeof module !== 'undefined') {
  module.exports = { DEFAULT_LIGHTING, LIGHTING_PRESETS, LOOKS, lightDirections, lightingPreset,
    lookSettings, normalizeLighting };
}
