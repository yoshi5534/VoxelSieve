// 3D view of VoxelSieve (ADR 0008, ADR 0035): ray casting of the dataset in WebGL2. The coarse
// level (at most 256 voxels per axis) is always there; finer levels are streamed as bricks into
// an atlas within a VRAM budget, where the picture needs them (bricks.js), down to level 0.
// "Surface" shades the part surface at a threshold, "Transfer function" composites colour and
// opacity per grey value (transfer.js) and "Maximum intensity projection" shows the densest
// value along each ray. Pores and zones of a porosity analysis are drawn in their own colours.
// A cut along x opens the part. "Extracted surface" draws the mesh of a surface step (ADR 0009)
// instead of the grey values, "Nominal-actual deviation" the surface of a nominal-actual
// comparison coloured by its deviation from the CAD model and "Wall thickness" the surface of a
// wall thickness step coloured by the thickness (ADR 0030).
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
  soft: { name: 'Soft light', style: 2, colors: [[232, 234, 237], [150, 157, 166]] },
  softDark: { name: 'Soft dark', style: 2, colors: [[70, 76, 86], [14, 16, 19]] },
  verlauf: { name: 'Gradient blue', style: 1, colors: [[58, 72, 96], [10, 12, 18]] },
  verlaufGrau: { name: 'Gradient grey', style: 1, colors: [[96, 100, 106], [26, 28, 31]] },
  schwarz: { name: 'Black', style: 0, colors: [[18, 18, 18], [18, 18, 18]] },
  weiss: { name: 'White', style: 0, colors: [[255, 255, 255], [255, 255, 255]] },
};

// The surface mesh in level-0 voxel coordinates, projected like the rays of VOLUME_FRAGMENT.
const MESH_VERTEX = `#version 300 es
in vec3 position;
in float deviation;   // mm: the deviation in mode 4, the wall thickness in mode 5
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
// 0: surface colour; 1: colour by the deviation, as deviationColor in compare.cpp; 2: by the wall
// thickness, as thicknessColor in thickness.cpp
uniform int valueColors;
uniform float tolerance;
uniform float range;
uniform float minWall;  // 0: no limit
uniform float maxWall;
uniform float alpha;    // below 1 for the translucent regions (ADR 0041)
vec3 deviationColor(float d) {
  float a = abs(d);
  if (a > 1e29) return vec3(150.0) / 255.0;  // no value
  if (a <= tolerance) return vec3(60.0, 190.0, 90.0) / 255.0;
  float t = clamp((a - tolerance) / max(range - tolerance, 1e-6), 0.0, 1.0);
  if (d > 0.0) return mix(vec3(240.0, 225.0, 40.0), vec3(215.0, 30.0, 30.0), t) / 255.0;
  return mix(vec3(40.0, 205.0, 240.0), vec3(40.0, 60.0, 215.0), t) / 255.0;
}
vec3 thicknessColor(float t) {
  if (t < 0.0) return vec3(150.0) / 255.0;  // not measured
  if (minWall > 0.0 && t < minWall) return vec3(200.0, 40.0, 170.0) / 255.0;
  if (maxWall > 0.0 && t > maxWall) return vec3(110.0, 50.0, 170.0) / 255.0;
  float x = clamp(t / max(range, 1e-6), 0.0, 1.0) * 4.0;
  vec3 stops[5] = vec3[5](vec3(215.0, 40.0, 40.0), vec3(240.0, 220.0, 50.0),
                          vec3(60.0, 190.0, 90.0), vec3(40.0, 190.0, 230.0),
                          vec3(40.0, 70.0, 210.0));
  int i = min(int(x), 3);
  return mix(stops[i], stops[i + 1], x - float(i)) / 255.0;
}
void main() {
  if (tex.x > cut) discard;
  vec3 normal = normalize(cross(dFdx(world), dFdy(world)));
  vec3 direction = normalize(world - eye);
  vec3 light = normalize(-direction + up * 0.4 + right * 0.3);
  float diffuse = abs(dot(normal, light));
  vec3 halfway = normalize(light - direction);
  float specular = pow(abs(dot(normal, halfway)), 24.0) * 0.25;
  vec3 base = valueColors == 1 ? deviationColor(dev)
             : valueColors == 2 ? thicknessColor(dev) : surfaceColor;
  color = vec4(base * (0.25 + 0.75 * diffuse) + vec3(specular), alpha);
}`;

/// The ray caster. Each sample takes the finest level loaded where it lies, down to the level a
/// pixel at its distance needs: bricks of the finer levels from the atlas through the page table
/// (bricks.js), else the coarse textures. Bricks and coarse cells that cannot show in the current
/// mode are skipped whole. Compiled a second time with FEEDBACK for the pass that writes per pixel
/// the first brick along the ray it wanted finer and a random one of those further on, or else
/// one it used.
function volumeFragment(feedback) {
  return `#version 300 es
${feedback ? '#define FEEDBACK' : ''}
precision highp float;
precision highp int;
precision highp sampler3D;
precision highp usampler3D;
in vec2 ndc;
#ifdef FEEDBACK
layout(location = 0) out uvec4 color;
layout(location = 1) out uvec4 further;
#else
out vec4 color;
#endif
uniform sampler3D grey;     // the coarse level
uniform sampler3D overlay;
uniform sampler2D transfer; // 256 x 1 RGBA: colour and opacity per grey value
// What the material colouring lets be seen, filtered linearly so its edges are smooth surfaces
// and not the faces of voxels: red 1 where the surface may lie (a material shown, or a pore),
// green 1 where a material is not left out. Of the coarse level and of the bricks.
uniform sampler3D visibility;
uniform sampler3D atlasGrey;
uniform sampler3D atlasOverlay;
uniform sampler3D atlasVisibility;
uniform highp usampler3D pages;
uniform int coarseLevel;       // the level of the coarse textures; bricks are of the finer ones
uniform ivec3 pageOrigin[16];  // where each level starts in the page table
uniform ivec3 pageGrid[16];    // its bricks (the coarse level: its cells) per axis
uniform vec3 extent;           // level-0 voxels the volume spans in texture coordinates 0..1
uniform vec3 coarseDims;
uniform ivec3 atlasSlots;
uniform vec3 atlasSize;
uniform float pixelSize;       // a pixel at distance 1, in world units
uniform float voxel0;          // a level-0 voxel along its shortest axis, in world units
uniform float firstOpaque;     // lowest grey value (0..1) the transfer function shows; 2 if none
uniform float seed;
uniform vec3 eye;
uniform vec3 right;
uniform vec3 up;
uniform vec3 forward;
uniform float aspect;
uniform vec3 box;        // half extent of the volume, largest axis 0.5
uniform float threshold; // material threshold on the 0..1 grey scale
uniform int mode;        // 0 surface, 1 transfer function, 2 maximum intensity projection,
                         // 3 and up background only (the mesh is drawn on top)
uniform bool shading;
uniform bool pores;        // pores of a porosity analysis
uniform bool zones;        // its zones of loosened microstructure
uniform bool materials;    // colour the material classes of a segmentation
uniform int hiddenMaterials; // bit m - 1 set: material class m is left out
uniform vec3 materialColors[8];
uniform float cut;       // texture x beyond which the part is cut away
uniform vec3 surfaceColor;
uniform vec3 poreColor;
uniform vec3 zoneColor;
uniform vec3 background;   // top or centre colour
uniform vec3 background2;  // bottom or edge colour
uniform int backgroundStyle; // 0 plain, 1 vertical gradient, 2 soft light

// Opacities of the transfer function hold for this path length (1/128 of the largest axis), so
// the picture does not depend on the level shown.
const float kReferenceLength = 1.0 / 128.0;
const int kBrick = ${BRICKS.BRICK};
const float kSlot = ${BRICKS.SLOT}.0;
const float kApron = ${BRICKS.APRON}.0;
const float kCell = ${BRICKS.COARSE_CELL}.0;

// Where a sample lies: in a brick of the atlas (state 1), in a brick without voxels (2) or in the
// coarse textures (3).
struct Where {
  int state;
  int level;
  ivec3 brick;      // the brick, or the cell of the coarse level
  vec3 coord;       // texture coordinates in the atlas or the coarse textures
  vec3 texel;       // one voxel in those coordinates
  vec3 voxelTex;    // one voxel of the level in volume texture coordinates
  vec3 cellMin;     // the brick or cell in volume texture coordinates
  vec3 cellMax;
  float maxGrey;    // largest grey value in it and one voxel around
  bool porous;      // holds pores or zones
  int wantedLevel;  // a finer level that is not loaded there, or -1
  ivec3 wantedBrick;
};

Where locate(vec3 t, float distance) {
  Where w;
  w.wantedLevel = -1;
  float footprint = distance * pixelSize / voxel0;
  int desired = footprint <= 1.0 ? 0 : min(int(floor(log2(footprint))), coarseLevel);
  vec3 p0 = t * extent;
  for (int level = desired; level < coarseLevel; ++level) {
    float scale = exp2(float(level));
    vec3 p = p0 / scale;
    ivec3 b = clamp(ivec3(floor(p / float(kBrick))), ivec3(0), pageGrid[level] - 1);
    uvec4 e = texelFetch(pages, pageOrigin[level] + b, 0);
    int state = int(e.b & 3u);
    if (state == 0) {
      // The level just finer than the one found is asked for, so the picture refines in steps.
      w.wantedLevel = level;
      w.wantedBrick = b;
      continue;
    }
    w.state = state;
    w.level = level;
    w.brick = b;
    w.voxelTex = vec3(scale) / extent;
    w.cellMin = vec3(b * kBrick) * w.voxelTex;
    w.cellMax = vec3((b + 1) * kBrick) * w.voxelTex;
    w.maxGrey = float(e.a) / 255.0;
    w.porous = (e.b & 4u) != 0u;
    if (state == 1) {
      int slot = int(e.r) | (int(e.g) << 8);
      vec3 s = vec3(slot % atlasSlots.x, (slot / atlasSlots.x) % atlasSlots.y,
                    slot / (atlasSlots.x * atlasSlots.y));
      // Past the apron at the start of the slot.
      w.coord = (s * kSlot + kApron + (p - vec3(b * kBrick))) / atlasSize;
      w.texel = 1.0 / atlasSize;
    }
    return w;
  }
  vec3 c = t * coarseDims;
  ivec3 cell = clamp(ivec3(floor(c / kCell)), ivec3(0), pageGrid[coarseLevel] - 1);
  uvec4 e = texelFetch(pages, pageOrigin[coarseLevel] + cell, 0);
  w.state = 3;
  w.level = coarseLevel;
  w.brick = cell;
  w.coord = t;
  w.texel = 1.0 / coarseDims;
  w.voxelTex = w.texel;
  w.cellMin = vec3(cell) * kCell / coarseDims;
  w.cellMax = vec3(cell + 1) * kCell / coarseDims;
  w.maxGrey = float(e.a) / 255.0;
  w.porous = (e.b & 4u) != 0u;
  return w;
}

// Grey value (which 0) or visibility (which 1) one voxel offset from the sample.
vec4 fetch(Where w, int which, vec3 offset) {
  vec3 c = w.coord + offset * w.texel;
  if (w.state == 1) {
    return which == 0 ? textureLod(atlasGrey, c, 0.0) : textureLod(atlasVisibility, c, 0.0);
  }
  return which == 0 ? textureLod(grey, c, 0.0) : textureLod(visibility, c, 0.0);
}

float greyAt(Where w) {
  return fetch(w, 0, vec3(0.0)).r;
}

vec2 visibilityAt(Where w) {
  return fetch(w, 1, vec3(0.0)).rg;
}

int classAt(Where w) {
  float value = w.state == 1 ? textureLod(atlasOverlay, w.coord, 0.0).r
                             : textureLod(overlay, w.coord, 0.0).r;
  return int(value * 255.0 + 0.5);
}

// Differences of a channel one voxel apart along each axis.
vec3 differences(Where w, int which, vec4 channel) {
  return vec3(dot(fetch(w, which, vec3(1, 0, 0)) - fetch(w, which, vec3(-1, 0, 0)), channel),
              dot(fetch(w, which, vec3(0, 1, 0)) - fetch(w, which, vec3(0, -1, 0)), channel),
              dot(fetch(w, which, vec3(0, 0, 1)) - fetch(w, which, vec3(0, 0, -1)), channel));
}

// The same per world unit, so voxels that are not cubes shade right.
vec3 worldGradient(Where w, int which, vec4 channel) {
  return differences(w, which, channel) / (box * w.voxelTex);
}

const vec4 kRed = vec4(1.0, 0.0, 0.0, 0.0);
const vec4 kGreen = vec4(0.0, 1.0, 0.0, 0.0);

vec3 normalAt(Where w) {
  vec3 g = worldGradient(w, 0, kRed);
  return length(g) > 1e-5 ? -normalize(g) : vec3(0.0);
}

// Normal of the surface, where the grey value is above the threshold and the visibility above
// one half: of the grey values, or of the edge of what the materials let be seen where that edge
// is the nearer of the two.
vec3 surfaceNormal(Where w, float value, float visible) {
  vec3 g = worldGradient(w, 0, kRed);
  if (materials) {
    vec3 v = worldGradient(w, 1, kGreen + kRed);
    // Distances to both edges, estimated from value and gradient.
    float toGrey = (value - threshold) / max(length(g), 1e-5);
    float toVisible = (visible - 0.5) / max(length(v), 1e-5);
    if (toVisible < toGrey && length(v) > 1e-5) g = v;
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

// Material class 1 to 8 of an overlay class (SliceOverlay::kMaterial + m), 0 for none.
int materialOf(int kind) {
  return materials && kind > 16 ? kind - 16 : 0;
}

bool materialShown(int material) {
  return material > 0 && ((hiddenMaterials >> (material - 1)) & 1) == 0;
}

// Opacity of one step of the given length for an opacity per reference length.
float stepOpacity(float opacity, float stepSize) {
  return 1.0 - pow(1.0 - min(opacity, 0.999), stepSize / kReferenceLength);
}

// Whether nothing in the brick or cell can show in the current mode (review step 6.3).
bool skippable(Where w, float highest) {
  if (w.state == 2) return true;
  if ((pores || zones) && w.porous) return false;
  if (mode == 0) return w.maxGrey < threshold;
  if (mode == 1) return w.maxGrey < firstOpaque;
  return w.maxGrey <= highest;
}

// Ray parameter where the ray leaves the brick or cell.
float exitOf(Where w, vec3 direction) {
  vec3 lo = (w.cellMin - 0.5) * 2.0 * box;
  vec3 hi = (w.cellMax - 0.5) * 2.0 * box;
  vec3 inverse = 1.0 / direction;
  vec3 far = max((lo - eye) * inverse, (hi - eye) * inverse);
  return min(min(far.x, far.y), far.z);
}

// Step of about 0.6 voxels of the sample's level.
float stepOf(Where w) {
  vec3 v = w.voxelTex * box;
  return min(min(v.x, v.y), v.z) * 1.2;
}

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

vec3 backgroundAt() {
  if (backgroundStyle == 1) return mix(background2, background, ndc.y * 0.5 + 0.5);
  if (backgroundStyle == 2) {
    // Soft light: in the middle falling off to the edges, the lower part a little darker
    // like a floor.
    float r = length(vec2(ndc.x * aspect * 0.55, ndc.y * 0.8 - 0.1));
    vec3 c = mix(background, background2, smoothstep(0.05, 1.05, r));
    return c * (1.0 - 0.15 * smoothstep(-0.1, -1.0, ndc.y));
  }
  return background;
}

float random(vec3 v) {
  return fract(sin(dot(v, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}

void main() {
  vec3 background = backgroundAt();
  vec3 direction = normalize(forward + ndc.x * aspect * 0.35 * right + ndc.y * 0.35 * up);
  float near;
  float far;
  if (mode >= 3 || !hitBox(eye, direction, near, far)) {
#ifdef FEEDBACK
    color = uvec4(0u);
    further = uvec4(0u);
#else
    color = vec4(background, 1.0);
#endif
    return;
  }
  near = max(near, 0.0);
  float stepSize = stepOf(locate((eye + direction * near) / (2.0 * box) + 0.5, near));
  // A per-pixel offset of the first sample turns the rings of regular sampling into fine noise.
  near += stepSize * fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
  vec3 accumulated = vec3(0.0);
  float alpha = 0.0;
  float highest = 0.0;
  bool porous = false;
  bool entered = false;
  bool done = false;
  int wantedLevel = -1;
  ivec3 wantedBrick = ivec3(0);
  int usedLevel = -1;
  ivec3 usedBrick = ivec3(0);
  float used = 0.0;
  int furtherLevel = -1;
  ivec3 furtherBrick = ivec3(0);
  float missing = 0.0;
  float t = near;
  for (int i = 0; i < 8192 && t < far; ++i) {
    vec3 tex = (eye + direction * t) / (2.0 * box) + 0.5;
    Where w = locate(tex, t);
    stepSize = stepOf(w);
#ifdef FEEDBACK
    if (w.wantedLevel >= 0) {
      if (wantedLevel < 0) {
        wantedLevel = w.wantedLevel;
        wantedBrick = w.wantedBrick;
      } else if (w.wantedLevel != wantedLevel || w.wantedBrick != wantedBrick) {
        missing += 1.0;
        if (random(vec3(gl_FragCoord.yx, float(i) + seed + 0.5)) * missing < 1.0) {
          furtherLevel = w.wantedLevel;
          furtherBrick = w.wantedBrick;
        }
      }
    }
    if (w.state == 1) {
      // One of the bricks used along the ray, each as likely (reservoir sampling).
      used += 1.0;
      if (random(vec3(gl_FragCoord.xy, float(i) + seed)) * used < 1.0) {
        usedLevel = w.level;
        usedBrick = w.brick;
      }
    }
#endif
    if (tex.x > cut) {
      entered = false;
      t += stepSize;
      continue;
    }
    if (skippable(w, highest)) {
      t = max(exitOf(w, direction), t) + 0.05 * stepSize;
      entered = true;
      continue;
    }
    int kind = classAt(w);
    float value = greyAt(w);
    int material = materialOf(kind);
    // A material left out is seen through like air, and in the surface view so is what the
    // segmentation counts as air. The visibility is interpolated, so these edges are as smooth
    // as the grey surface instead of showing the voxels of the segmentation.
    vec2 visible = materials ? visibilityAt(w) : vec2(1.0);
    if (mode == 0) {
      if (pores && kind == 1) {
#ifndef FEEDBACK
        color = vec4(shade(poreColor, normalAt(w), direction), 1.0);
#endif
        done = true;
        break;
      }
      if (value >= threshold && visible.r >= 0.5) {
#ifndef FEEDBACK
        // On the cut face the grey gradient says nothing; light it as the plane it is.
        bool onCut = cut < 1.0 && tex.x > cut - 1.5 * w.voxelTex.x && !entered;
        vec3 n = onCut ? vec3(1.0, 0.0, 0.0) : surfaceNormal(w, value, visible.r);
        // The interpolated edge can lie in a voxel of air or of a material left out; its colour
        // is that of the material shown just behind it.
        for (int k = 1; materials && !materialShown(material) && kind != 2 && k <= 3; ++k) {
          float behind = t + stepSize * 0.5 * float(k);
          kind = classAt(locate((eye + direction * behind) / (2.0 * box) + 0.5, behind));
          material = materialOf(kind);
        }
        vec3 base = (zones && kind == 2) ? zoneColor
            : materialShown(material) ? materialColors[(material - 1) % 8] : surfaceColor;
        color = vec4(shade(base, n, direction), 1.0);
#endif
        done = true;
        break;
      }
      entered = true;
    } else if (mode == 2) {
      if (visible.g >= 0.5) {
        highest = max(highest, value);
        porous = porous || (pores && kind == 1);
        if (highest >= 1.0) break;
      }
    } else {
      vec4 sample_;
      if (pores && kind == 1) {
        sample_ = vec4(poreColor, 0.6);
      } else if (zones && kind == 2) {
        sample_ = vec4(zoneColor, 0.1);
      } else {
        sample_ = texture(transfer, vec2(value, 0.5));
        // Quadratic, so the low opacities that matter for looking through get room on the curve.
        sample_.a *= sample_.a;
        if (material > 0) sample_.rgb = materialColors[(material - 1) % 8];
      }
      sample_.a *= visible.g;
      float a = stepOpacity(sample_.a, stepSize);
      if (a >= 0.0005) {
        vec3 rgb = sample_.rgb;
        if (shading) {
          vec3 g = differences(w, 0, kRed);
          float strength = smoothstep(0.02, 0.12, length(g));
          if (strength > 0.0) rgb = mix(rgb, shade(rgb, -normalize(g), direction), strength);
        }
        accumulated += (1.0 - alpha) * a * rgb;
        alpha += (1.0 - alpha) * a;
        if (alpha > 0.98) break;
      }
    }
    t += stepSize;
  }
#ifdef FEEDBACK
  if (wantedLevel >= 0) {
    color = uvec4(uint(wantedLevel + 1), uvec3(wantedBrick));
  } else if (usedLevel >= 0) {
    color = uvec4(uint(usedLevel + 1) | 128u, uvec3(usedBrick));
  } else {
    color = uvec4(0u);
  }
  further = furtherLevel >= 0 ? uvec4(uint(furtherLevel + 1), uvec3(furtherBrick)) : uvec4(0u);
#else
  if (done) return;
  if (mode == 2) {
    vec3 mapped = mix(background, texture(transfer, vec2(highest, 0.5)).rgb, highest);
    color = vec4(porous ? mix(mapped, poreColor, 0.65) : mapped, 1.0);
    return;
  }
  color = vec4(accumulated + (1.0 - alpha) * background, 1.0);
#endif
}`;
}

/// VRAM budget of the brick atlas in MB, kept per browser; WebGL cannot tell how much is free.
const DEFAULT_VRAM_MB = 512;

function savedVramBudget() {
  try {
    const value = Number(localStorage.getItem('voxelsieve.vramMB'));
    return value >= 64 ? value : DEFAULT_VRAM_MB;
  } catch {
    return DEFAULT_VRAM_MB;
  }
}

/// Lowest grey value (0..1) whose opacity the transfer function lets show, 2 for none.
function firstOpaque(table) {
  if (!table) return 2;
  for (let v = 0; v < 256; v += 1) if (table[4 * v + 3] > 0) return v / 255;
  return 2;
}

class VolumeViewer {
  constructor() {
    this.volume = null;       // {dims, level, voxelSize, window, key}
    this.mode = 0;
    this.pores = true;
    this.zones = true;
    this.materials = true;      // colour the classes of a material segmentation, if loaded
    this.hiddenMaterials = [];  // ids of the material classes left out
    this.materialColors = MATERIAL_COLORS;  // colour of material m at m - 1
    this.cut = 1;
    this.threshold = 0.5;
    this.shading = true;
    this.surfaceColor = [209, 214, 219];
    // Regions of the scan, drawn translucent over it: {key, color [r, g, b], triangles} with the
    // triangles in level-0 voxel coordinates.
    this.regions = [];
    this.regionBuffers = new Map();  // key -> {buffer, vao, count}
    this.poreColor = [230, 38, 31];
    this.zoneColor = [242, 204, 77];
    this.background = { preset: 'softDark', ...BACKGROUNDS.softDark };
    this.histogram = new Array(256).fill(0);
    this.transfer = null;       // lookup table of 256 RGBA bytes
    this.surface = null;        // {key, points, triangles} of the mesh of a surface step
    this.yaw = 0.8;
    this.pitch = 0.45;
    this.distance = 2.2;
    this.target = [0, 0, 0];  // the point the camera orbits, moved by panning
    this.moving = false;      // the camera moves: draw at half resolution meanwhile
    this.vramMB = savedVramBudget();
    this.brickIndex = null;   // which bricks hold voxels (api/volume_bricks)
    this.pager = null;        // the bricks in the atlas (bricks.js)
    this.arrived = [];        // bricks loaded, waiting for their upload
    this.requests = new Map();  // page texel -> AbortController
    this.changed = true;      // something changed since the last feedback pass
    this.lastFeedback = 0;
    this.onDetail = () => {}; // what is loaded changed (streamStatus)
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
      mode: this.mode, shading: this.shading, pores: this.pores, zones: this.zones,
      materials: this.materials,
      cut: this.cut,
      threshold: this.threshold, yaw: this.yaw, pitch: this.pitch, distance: this.distance,
      target: this.target, surfaceColor: this.surfaceColor, poreColor: this.poreColor, zoneColor: this.zoneColor,
      background: this.background,
    };
  }

  setState(saved) {
    const keys = ['mode', 'shading', 'pores', 'zones', 'materials', 'cut', 'threshold', 'yaw', 'pitch', 'distance',
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

  /// Loads the coarse level of a dataset step (with the overlay of a porosity step and of a
  /// material segmentation step) and which of its finer bricks hold voxels.
  async load(step, porosity, materials = null) {
    const key = step + '/' + (porosity ?? '-') + (materials === null ? '' : '/' + materials);
    if (this.volume?.key === key) return;
    this.source = { step, porosity, materials };
    const params = new URLSearchParams({ step, max: 256 });
    if (porosity !== null) params.set('porosity', porosity);
    if (materials !== null) params.set('materials', materials);
    const [response, index] = await Promise.all([
      fetch('api/volume?' + params),
      fetch('api/volume_bricks?' + new URLSearchParams({ step }))
        .then((r) => (r.ok ? r.json() : null)).catch(() => null),
    ]);
    if (!response.ok) throw new Error((await response.json()).error);
    const dims = response.headers.get('X-Dims').split(',').map(Number);
    const window = response.headers.get('X-Window').split(',').map(Number);
    const buffer = new Uint8Array(await response.arrayBuffer());
    const count = dims[0] * dims[1] * dims[2];
    this.dropBricks();
    this.volume = {
      key, dims, window,
      level: Number(response.headers.get('X-Level')),
      // Edge lengths x, y, z in mm; voxels need not be cubes (ADR 0012).
      voxelSize: response.headers.get('X-Voxel-Size').split(',').map(Number),
      grey: buffer.subarray(0, count),
      overlay: buffer.subarray(count, 2 * count),
    };
    this.brickIndex = index?.brick === BRICKS.BRICK ? index : null;
    this.pager = null;
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

  /// Takes the canvas of the view. The same canvas keeps its context across re-renders of the
  /// page (review step 6.4); only a new one gets set up.
  attach(canvas) {
    if (this.canvas === canvas && this.gl) {
      this.requestDraw();
      return;
    }
    this.canvas = canvas;
    this.gl = attachContext(canvas, (gl) => this.setup(gl), () => this.lose());
    attachOrbitInput(canvas, this, {
      clampTarget: (t) => t.map((c) => Math.min(Math.max(c, -0.6), 0.6)),
    });
    this.requestDraw();
  }

  /// Everything on a new or restored context; textures are uploaded again when drawn.
  setup(gl) {
    this.gl = gl;
    this.lost = false;
    this.uploaded = false;
    this.textures = null;
    this.visibilityTextures = null;
    this.visibilityKey = null;
    this.transferUploaded = false;
    this.transferTexture = null;
    this.atlas = null;
    this.pageTexture = null;
    this.feedbackTarget = null;
    this.meshBuffers = null;
    this.regionBuffers = new Map();
    this.locations = new Map();
    this.program = compileProgram(gl, VOLUME_VERTEX, volumeFragment(false));
    this.feedbackProgram = compileProgram(gl, VOLUME_VERTEX, volumeFragment(true));
    this.meshProgram = compileProgram(gl, MESH_VERTEX, MESH_FRAGMENT);
    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    this.vao = gl.createVertexArray();
    gl.bindVertexArray(this.vao);
    for (const program of [this.program, this.feedbackProgram]) {
      const location = gl.getAttribLocation(program, 'position');
      gl.enableVertexAttribArray(location);
      gl.vertexAttribPointer(location, 2, gl.FLOAT, false, 0, 0);
    }
    gl.bindVertexArray(null);
    this.pager = null;
    this.dropBricks();
    this.requestDraw();
  }

  /// The context is gone: nothing can be drawn until it is restored.
  lose() {
    this.lost = true;
    this.dropBricks();
    this.pager = null;
  }

  /// Stops the requests for bricks and forgets the ones waiting for their upload.
  dropBricks() {
    for (const controller of this.requests.values()) controller.abort();
    this.requests.clear();
    this.arrived = [];
  }

  /// Sets the VRAM budget of the bricks in MB and starts over with an atlas of that size.
  setVramBudget(mb) {
    this.vramMB = Math.max(64, Math.round(mb));
    try {
      localStorage.setItem('voxelsieve.vramMB', String(this.vramMB));
    } catch {
      // The budget then holds for this page only.
    }
    this.dropBricks();
    this.pager = null;
    this.requestDraw();
  }

  /// What is loaded, for the status line.
  streamStatus() {
    const pager = this.pager;
    if (!pager || !this.volume) return null;
    return {
      finest: pager.finestLevel(),
      bricks: pager.resident.size,
      slots: pager.slotCount,
      loading: this.requests.size + this.arrived.length,
      full: pager.full,
      budgetMB: this.atlasMB ?? this.vramMB,
    };
  }

  /// Loads the mesh of a surface step for mode 3.
  loadSurface(step) {
    return this.loadMesh('surface', step);
  }

  /// Loads the compared surface of a nominal-actual comparison step for mode 4, coloured by its
  /// deviation or, with `displacement`, by how far its non-rigid registration moved it.
  loadDeviation(step, displacement = false) {
    return this.loadMesh('deviation', step, displacement ? { displacement: 1 } : {});
  }

  /// Loads the measured surface of a wall thickness step for mode 5, coloured by the thickness
  /// or, with `nominal`, by its deviation from the nominal thickness of the CAD model.
  loadWallThickness(step, nominal = false) {
    return this.loadMesh('wall_thickness', step, nominal ? { nominal: 1 } : {});
  }

  async loadMesh(kind, step, extra = {}) {
    const query = new URLSearchParams({ step, ...extra });
    const key = kind + ':' + query;
    if (this.surface?.key === key) return this.surface;
    const response = await fetch('api/' + kind + '?' + query);
    if (!response.ok) throw new Error((await response.json()).error);
    const vertices = Number(response.headers.get('X-Vertices'));
    const triangles = Number(response.headers.get('X-Triangles'));
    const buffer = await response.arrayBuffer();
    const deviations = ['deviation', 'wall_thickness'].includes(kind) ? vertices : 0;
    const nominal = 'nominal' in extra;
    if (kind === 'wall_thickness') {
      // Unmeasured points come as NaN; the shader takes negative thicknesses or huge deviations
      // for them, since not every GPU keeps NaN through the interpolation.
      const values = new Float32Array(buffer, vertices * 12, vertices);
      const none = nominal ? 1e30 : -1;
      values.forEach((value, i) => { if (Number.isNaN(value)) values[i] = none; });
    }
    this.surface = {
      key, kind, triangles,
      points: new Float32Array(buffer, 0, vertices * 3),
      deviation: deviations ? new Float32Array(buffer, vertices * 12, vertices) : null,
      indices: new Uint32Array(buffer, vertices * 12 + deviations * 4, triangles * 3),
      tolerance: Number(response.headers.get('X-Tolerance') ?? 0),
      range: Number(response.headers.get('X-Range') ?? 0),
      minWall: Number(response.headers.get('X-Min-Wall') ?? 0),
      maxWall: Number(response.headers.get('X-Max-Wall') ?? 0),
      hasDisplacement: response.headers.get('X-Displacement') === '1',
      displacement: 'displacement' in extra,
      hasNominal: response.headers.get('X-Nominal') === '1',
      nominal,
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
    const uniform = (name) => this.uniform(this.meshProgram, name);
    gl.uniform3fv(uniform('extent'), dims.map((d) => d * scale));
    gl.uniform3fv(uniform('box'), box);
    gl.uniform3fv(uniform('eye'), eye);
    gl.uniform3fv(uniform('right'), right);
    gl.uniform3fv(uniform('up'), up);
    gl.uniform3fv(uniform('forward'), forward);
    gl.uniform1f(uniform('aspect'), aspect);
    gl.uniform1f(uniform('cut'), this.cut);
    gl.uniform3fv(uniform('surfaceColor'), this.surfaceColor.map((c) => c / 255));
    gl.uniform1i(uniform('valueColors'), !this.surface.deviation ? 0
      : this.surface.kind === 'wall_thickness' && !this.surface.nominal ? 2 : 1);
    gl.uniform1f(uniform('tolerance'), this.surface.tolerance);
    gl.uniform1f(uniform('range'), this.surface.range);
    gl.uniform1f(uniform('minWall'), this.surface.minWall);
    gl.uniform1f(uniform('maxWall'), this.surface.maxWall);
    gl.uniform1f(uniform('alpha'), 1);
    gl.enable(gl.DEPTH_TEST);
    gl.clear(gl.DEPTH_BUFFER_BIT);
    gl.bindVertexArray(this.meshVao);
    gl.drawElements(gl.TRIANGLES, this.surface.triangles * 3, gl.UNSIGNED_INT, 0);
    gl.bindVertexArray(null);
    gl.disable(gl.DEPTH_TEST);
  }

  setRegions(regions) {
    this.regions = regions;
    if (this.gl) {
      for (const [key, entry] of this.regionBuffers) {
        if (regions.some((region) => region.key === key)) continue;
        this.gl.deleteBuffer(entry.buffer);
        this.gl.deleteVertexArray(entry.vao);
        this.regionBuffers.delete(key);
      }
    }
    this.requestDraw();
  }

  /// The regions as translucent solids over the picture, without depth: a region shows where it
  /// lies around the part, also inside it.
  drawRegions(eye, right, up, forward, aspect, box) {
    const gl = this.gl;
    if (!this.regions.length) return;
    const { dims, level } = this.volume;
    const scale = 2 ** level;
    gl.useProgram(this.meshProgram);
    const uniform = (name) => this.uniform(this.meshProgram, name);
    gl.uniform3fv(uniform('extent'), dims.map((d) => d * scale));
    gl.uniform3fv(uniform('box'), box);
    gl.uniform3fv(uniform('eye'), eye);
    gl.uniform3fv(uniform('right'), right);
    gl.uniform3fv(uniform('up'), up);
    gl.uniform3fv(uniform('forward'), forward);
    gl.uniform1f(uniform('aspect'), aspect);
    gl.uniform1f(uniform('cut'), 2);
    gl.uniform1i(uniform('valueColors'), 0);
    gl.uniform1f(uniform('alpha'), 0.22);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    const position = gl.getAttribLocation(this.meshProgram, 'position');
    const deviation = gl.getAttribLocation(this.meshProgram, 'deviation');
    for (const region of this.regions) {
      let entry = this.regionBuffers.get(region.key);
      if (!entry) {
        const buffer = gl.createBuffer();
        const vao = gl.createVertexArray();
        gl.bindVertexArray(vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
        gl.bufferData(gl.ARRAY_BUFFER, region.triangles, gl.STATIC_DRAW);
        gl.enableVertexAttribArray(position);
        gl.vertexAttribPointer(position, 3, gl.FLOAT, false, 0, 0);
        if (deviation >= 0) {
          gl.disableVertexAttribArray(deviation);
          gl.vertexAttrib1f(deviation, 0);
        }
        gl.bindVertexArray(null);
        entry = { buffer, vao, count: region.triangles.length / 3 };
        this.regionBuffers.set(region.key, entry);
      }
      gl.uniform3fv(uniform('surfaceColor'), region.color.map((c) => c / 255));
      gl.bindVertexArray(entry.vao);
      gl.drawArrays(gl.TRIANGLES, 0, entry.count);
    }
    gl.bindVertexArray(null);
    gl.disable(gl.BLEND);
  }

  uniform(program, name) {
    const key = program === this.program ? 'v:' + name
      : program === this.feedbackProgram ? 'f:' + name : 'm:' + name;
    if (!this.locations.has(key)) this.locations.set(key, this.gl.getUniformLocation(program, name));
    return this.locations.get(key);
  }

  /// Eye position and view axes of the camera, orbiting `target` with z up.
  camera() {
    return orbitCamera(this);
  }

  /// Half extent of the volume box in world units, the largest axis 0.5.
  boxHalf() {
    const sizes = this.volume.dims.map((d, a) => d * this.volume.voxelSize[a]);
    const largest = Math.max(...sizes);
    return sizes.map((s) => s / largest / 2);
  }

  /// Level-0 voxels the volume texture spans: the coarse dims times 2^level.
  extent() {
    return this.volume.dims.map((d) => d * 2 ** this.volume.level);
  }

  texture3d(unit, format, dims, filter) {
    const gl = this.gl;
    const handle = gl.createTexture();
    gl.activeTexture(gl.TEXTURE0 + unit);
    gl.bindTexture(gl.TEXTURE_3D, handle);
    gl.texStorage3D(gl.TEXTURE_3D, 1, format, ...dims);
    gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, filter);
    for (const wrap of [gl.TEXTURE_WRAP_S, gl.TEXTURE_WRAP_T, gl.TEXTURE_WRAP_R]) {
      gl.texParameteri(gl.TEXTURE_3D, wrap, gl.CLAMP_TO_EDGE);
    }
    return handle;
  }

  upload() {
    const gl = this.gl;
    const dims = this.volume.dims;
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    if (this.textures) this.textures.forEach((t) => gl.deleteTexture(t));
    const grey = this.texture3d(0, gl.R8, dims, gl.LINEAR);
    gl.texSubImage3D(gl.TEXTURE_3D, 0, 0, 0, 0, ...dims, gl.RED, gl.UNSIGNED_BYTE,
      this.volume.grey);
    const overlay = this.texture3d(1, gl.R8, dims, gl.NEAREST);
    gl.texSubImage3D(gl.TEXTURE_3D, 0, 0, 0, 0, ...dims, gl.RED, gl.UNSIGNED_BYTE,
      this.volume.overlay);
    this.textures = [grey, overlay];
    this.uploaded = true;
    this.visibilityKey = null;
  }

  /// The atlas within the VRAM budget, its page table and the pager. A budget the GPU cannot
  /// give is halved until it can.
  createAtlas() {
    const gl = this.gl;
    if (this.atlas) this.atlas.textures.forEach((t) => gl.deleteTexture(t));
    if (this.pageTexture) gl.deleteTexture(this.pageTexture);
    this.atlas = null;
    const withMaterials = this.source?.materials != null;
    const bytesPerVoxel = withMaterials ? 4 : 2;
    const max3d = gl.getParameter(gl.MAX_3D_TEXTURE_SIZE);
    let budget = this.vramMB * 2 ** 20;
    while (gl.getError() !== gl.NO_ERROR) { /* clear earlier errors */ }
    for (let attempt = 0; attempt < 4 && !this.atlas; attempt += 1, budget /= 2) {
      const slots = BRICKS.atlasLayout(budget, bytesPerVoxel, max3d);
      const size = slots.map((s) => s * BRICKS.SLOT);
      const textures = [
        this.texture3d(3, gl.R8, size, gl.LINEAR),
        this.texture3d(4, gl.R8, size, gl.NEAREST),
        this.texture3d(6, gl.RG8, withMaterials ? size : [1, 1, 1], gl.LINEAR),
      ];
      if (gl.getError() === gl.NO_ERROR) {
        this.atlas = { slots, size, textures, withMaterials };
        this.atlasMB = Math.round(slots[0] * slots[1] * slots[2] * BRICKS.SLOT ** 3 *
          bytesPerVoxel / 2 ** 20);
      } else {
        textures.forEach((t) => gl.deleteTexture(t));
      }
    }
    if (!this.atlas) throw new Error('The GPU has no memory for the bricks of the 3D view');
    const layout = BRICKS.pageLayout(this.volume.level, this.extent());
    const [ax, ay, az] = this.atlas.slots;
    this.pager = new BRICKS.BrickPager(layout, ax * ay * az);
    const index = this.brickIndex;
    for (const level of index?.levels ?? []) {
      if (level.level >= this.volume.level) continue;
      const bits = new Uint8Array(level.present.length / 2);
      for (let i = 0; i < bits.length; i += 1) {
        bits[i] = parseInt(level.present.substr(2 * i, 2), 16);
      }
      this.pager.setPresence(level.level, level.grid, bits);
    }
    const coarse = layout.levels[this.volume.level];
    this.pager.setCoarse(BRICKS.coarseCells(this.volume.grey, this.volume.overlay,
      this.volume.dims, coarse.grid));
    this.pageTexture = gl.createTexture();
    gl.activeTexture(gl.TEXTURE7);
    gl.bindTexture(gl.TEXTURE_3D, this.pageTexture);
    gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texImage3D(gl.TEXTURE_3D, 0, gl.RGBA8UI, ...layout.dims, 0, gl.RGBA_INTEGER,
      gl.UNSIGNED_BYTE, this.pager.pages);
    this.pager.dirty = null;
    this.atlasKey = [this.vramMB, withMaterials, this.volume.key].join('|');
    this.changed = true;
  }

  /// Uploads the page texels that changed.
  uploadPages() {
    const gl = this.gl;
    const [first, last] = this.pager.dirty;
    const [w, h] = this.pager.layout.dims;
    gl.activeTexture(gl.TEXTURE7);
    gl.bindTexture(gl.TEXTURE_3D, this.pageTexture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texSubImage3D(gl.TEXTURE_3D, 0, 0, 0, first, w, h, last - first + 1, gl.RGBA_INTEGER,
      gl.UNSIGNED_BYTE, this.pager.pages.subarray(4 * w * h * first, 4 * w * h * (last + 1)));
    this.pager.dirty = null;
  }

  /// Asks for the bricks the last feedback pass wanted, six at a time (what browsers send to one
  /// host at once).
  requestBricks() {
    const pager = this.pager;
    for (const wanted of pager.takeRequests(Math.max(0, 6 - this.requests.size))) {
      const { key, level, brick } = wanted;
      const controller = new AbortController();
      this.requests.set(key, controller);
      const params = new URLSearchParams({
        step: this.source.step, level, i: brick[0], j: brick[1], k: brick[2],
        low: Math.round(this.volume.window[0]), high: Math.round(this.volume.window[1]),
      });
      if (this.source.porosity !== null) params.set('porosity', this.source.porosity);
      if (this.source.materials !== null) params.set('materials', this.source.materials);
      const volumeKey = this.volume.key;
      fetch('api/volume_brick?' + params, { signal: controller.signal })
        .then(async (response) => {
          if (!response.ok) throw new Error(response.statusText);
          const dims = response.headers.get('X-Dims').split(',').map(Number);
          const origin = response.headers.get('X-Origin').split(',').map(Number);
          const buffer = new Uint8Array(await response.arrayBuffer());
          const count = dims[0] * dims[1] * dims[2];
          if (this.pager !== pager || this.volume?.key !== volumeKey) return;
          const padded = BRICKS.padBrick(buffer.subarray(0, count), buffer.subarray(count, 2 * count),
            origin, dims, brick);
          this.arrived.push({ key, level, brick, ...padded });
          this.frame();
        })
        .catch(() => {
          if (this.pager === pager) pager.failed(key);
        })
        .finally(() => {
          if (this.requests.get(key) !== controller) return;
          this.requests.delete(key);
          // The next wanted brick right away, not only after the next feedback pass.
          if (this.pager === pager) this.requestBricks();
        });
    }
  }

  /// Uploads the bricks that arrived into their slots, as many as fit a few milliseconds of the
  /// frame (review finding G4).
  uploadBricks() {
    const gl = this.gl;
    const start = performance.now();
    const [ax, ay] = this.atlas.slots;
    const S = BRICKS.SLOT;
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    while (this.arrived.length && performance.now() - start < 4) {
      const brick = this.arrived.shift();
      const slot = this.pager.place(brick.key, brick.level, brick.brick, brick.maxGrey, brick.pores);
      if (slot < 0) continue;
      const at = [(slot % ax) * S, (Math.floor(slot / ax) % ay) * S, Math.floor(slot / (ax * ay)) * S];
      const put = (unit, texture, format, data) => {
        gl.activeTexture(gl.TEXTURE0 + unit);
        gl.bindTexture(gl.TEXTURE_3D, texture);
        gl.texSubImage3D(gl.TEXTURE_3D, 0, ...at, S, S, S, format, gl.UNSIGNED_BYTE, data);
      };
      put(3, this.atlas.textures[0], gl.RED, brick.grey);
      put(4, this.atlas.textures[1], gl.RED, brick.overlay);
      if (this.atlas.withMaterials) {
        put(6, this.atlas.textures[2], gl.RG, visibilityChannels(brick.overlay, this.hiddenMaterials));
      }
      this.changed = true;
    }
  }

  /// Uploads what the material colouring lets be seen of the coarse level, as two channels per
  /// voxel (see `visibility` in the shader). The shader filters them linearly, so the edges of the
  /// segmentation and of materials left out are smooth surfaces. The bricks hold theirs; they are
  /// loaded again.
  uploadVisibility() {
    const gl = this.gl;
    if (this.visibilityTextures) this.visibilityTextures.forEach((t) => gl.deleteTexture(t));
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const dims = this.volume.dims;
    const handle = this.texture3d(5, gl.RG8, dims, gl.LINEAR);
    gl.texSubImage3D(gl.TEXTURE_3D, 0, 0, 0, 0, ...dims, gl.RG, gl.UNSIGNED_BYTE,
      visibilityChannels(this.volume.overlay, this.hiddenMaterials));
    this.visibilityTextures = [handle];
    if (this.visibilityKey !== null && this.atlas?.withMaterials && this.pager) {
      this.dropBricks();
      this.pager.clear();
    }
    this.visibilityKey = this.hiddenMaterials.join(',');
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

  /// Draws on the next frame because something changed.
  requestDraw() {
    this.changed = true;
    this.frame();
  }

  /// Draws on the next frame.
  frame() {
    if (this.pending || !this.gl) return;
    this.pending = true;
    requestAnimationFrame(() => {
      this.pending = false;
      this.draw();
    });
  }

  /// The uniforms both passes of the ray caster share.
  setVolumeUniforms(program, width, height) {
    const gl = this.gl;
    const uniform = (name) => this.uniform(program, name);
    const [x, y, z] = this.volume.dims;
    const box = this.boxHalf();
    const { eye, forward, right, up } = this.camera();
    const units = { grey: 0, overlay: 1, transfer: 2, atlasGrey: 3, atlasOverlay: 4,
      visibility: 5, atlasVisibility: 6, pages: 7 };
    for (const [name, unit] of Object.entries(units)) gl.uniform1i(uniform(name), unit);
    const layout = this.pager.layout;
    const origins = new Int32Array(48);
    const grids = new Int32Array(48);
    layout.levels.slice(0, 16).forEach((level, i) => {
      origins.set(level.origin, 3 * i);
      grids.set(level.grid, 3 * i);
    });
    gl.uniform1i(uniform('coarseLevel'), Math.min(this.volume.level, 15));
    gl.uniform3iv(uniform('pageOrigin'), origins);
    gl.uniform3iv(uniform('pageGrid'), grids);
    const extent = this.extent();
    gl.uniform3fv(uniform('extent'), extent);
    gl.uniform3fv(uniform('coarseDims'), [x, y, z]);
    gl.uniform3iv(uniform('atlasSlots'), this.atlas.slots);
    gl.uniform3fv(uniform('atlasSize'), this.atlas.size);
    // The level a sample needs follows from the pixels of the picture at full resolution, in
    // CSS pixels: one voxel per CSS pixel at most.
    gl.uniform1f(uniform('pixelSize'), 0.7 / Math.max(this.canvas.clientHeight, 1));
    gl.uniform1f(uniform('voxel0'), Math.min(...box.map((b, k) => 2 * b / extent[k])));
    gl.uniform1f(uniform('firstOpaque'), firstOpaque(this.transfer));
    gl.uniform1f(uniform('seed'), (this.lastFeedback % 1000) / 7);
    gl.uniform3fv(uniform('eye'), eye);
    gl.uniform3fv(uniform('right'), right);
    gl.uniform3fv(uniform('up'), up);
    gl.uniform3fv(uniform('forward'), forward);
    gl.uniform1f(uniform('aspect'), width / height);
    gl.uniform3fv(uniform('box'), box);
    gl.uniform1f(uniform('threshold'), this.threshold);
    gl.uniform1i(uniform('mode'), this.mode);
    gl.uniform1i(uniform('pores'), this.pores ? 1 : 0);
    gl.uniform1i(uniform('zones'), this.zones ? 1 : 0);
    gl.uniform1i(uniform('materials'), this.materials && this.source?.materials != null ? 1 : 0);
    gl.uniform1i(uniform('hiddenMaterials'),
      this.hiddenMaterials.reduce((mask, id) => mask | (1 << (id - 1)), 0));
    gl.uniform3fv(uniform('materialColors'), MATERIAL_COLORS.map((fallback, m) =>
      this.materialColors[m] ?? fallback).flat().map((c) => c / 255));
    gl.uniform1f(uniform('cut'), this.cut);
    gl.uniform1i(uniform('shading'), this.shading ? 1 : 0);
    const color = (name, rgb) => gl.uniform3fv(uniform(name), rgb.map((c) => c / 255));
    color('surfaceColor', this.surfaceColor);
    color('poreColor', this.poreColor);
    color('zoneColor', this.zoneColor);
    color('background', this.background.colors[0]);
    color('background2', this.background.colors[1]);
    gl.uniform1i(uniform('backgroundStyle'), this.background.style);
  }

  /// The feedback pass at an eighth of the resolution: which bricks the picture wants, then
  /// requests for them.
  runFeedback(width, height) {
    const gl = this.gl;
    const w = Math.max(1, Math.ceil(width / 8));
    const h = Math.max(1, Math.ceil(height / 8));
    let target = this.feedbackTarget;
    if (!target || target.w !== w || target.h !== h) {
      if (target) {
        target.textures.forEach((t) => gl.deleteTexture(t));
        gl.deleteFramebuffer(target.framebuffer);
      }
      const framebuffer = gl.createFramebuffer();
      gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
      const attachments = [gl.COLOR_ATTACHMENT0, gl.COLOR_ATTACHMENT1];
      const textures = attachments.map((attachment) => {
        const texture = gl.createTexture();
        gl.activeTexture(gl.TEXTURE8);
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA32UI, w, h);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, attachment, gl.TEXTURE_2D, texture, 0);
        return texture;
      });
      gl.drawBuffers(attachments);
      target = this.feedbackTarget = {
        w, h, textures, framebuffer, attachments, pixels: new Uint32Array(8 * w * h),
      };
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, target.framebuffer);
    gl.viewport(0, 0, w, h);
    gl.useProgram(this.feedbackProgram);
    this.setVolumeUniforms(this.feedbackProgram, width, height);
    gl.bindVertexArray(this.vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    target.attachments.forEach((attachment, i) => {
      gl.readBuffer(attachment);
      gl.readPixels(0, 0, w, h, gl.RGBA_INTEGER, gl.UNSIGNED_INT, target.pixels, 4 * w * h * i);
    });
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);
    const wanted = this.pager.feedback(target.pixels);
    this.requestBricks();
    return wanted;
  }

  draw() {
    const gl = this.gl;
    const canvas = this.canvas;
    if (!gl || this.lost) return;
    // While the camera moves, half the resolution keeps the frames coming (review step 6.3).
    const ratio = (window.devicePixelRatio || 1) * (this.moving ? 0.5 : 1);
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
    const withMaterials = this.source?.materials != null;
    if (!this.pager || this.atlasKey !== [this.vramMB, withMaterials, this.volume.key].join('|')) {
      this.dropBricks();
      this.createAtlas();
    }
    if (this.visibilityKey !== this.hiddenMaterials.join(',')) this.uploadVisibility();
    this.uploadBricks();
    if (this.pager.dirty) this.uploadPages();
    gl.useProgram(this.program);
    this.setVolumeUniforms(this.program, width, height);
    gl.bindVertexArray(this.vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    const box = this.boxHalf();
    const { eye, forward, right, up } = this.camera();
    const meshKind = { 3: 'surface', 4: 'deviation', 5: 'wall_thickness' }[this.mode];
    if (meshKind && this.surface?.kind === meshKind) {
      this.drawMesh(eye, right, up, forward, width / height, box);
    }
    this.drawRegions(eye, right, up, forward, width / height, box);
    // Feedback after the picture, so a capture still finds it in the canvas; at most every
    // 50 ms, while something changed or bricks are on their way.
    const now = performance.now();
    // A full atlas takes no more bricks until the camera moves and frees some.
    if (this.changed) this.pager.full = false;
    const streaming = this.requests.size > 0 || this.arrived.length > 0 ||
      (this.pager.wanted.length > 0 && !this.pager.full);
    if (this.mode < 3 && (this.changed || streaming)) {
      if (now - this.lastFeedback >= 50) {
        this.lastFeedback = now;
        this.changed = false;
        this.runFeedback(width, height);
        this.onDetail(this.streamStatus());
      }
      this.frame();
    }
  }
}


/// The two visibility channels per voxel of an overlay (SliceOverlay classes, material m is
/// 16 + m) for the 3D view: red 255 where the surface may lie (a material shown, a pore or a
/// zone), green 255 where no material is left out.
function visibilityChannels(overlay, hiddenMaterials) {
  const hidden = new Set(hiddenMaterials);
  const lookup = new Uint8Array(512);
  for (let kind = 0; kind < 256; kind += 1) {
    const left = kind > 16 && hidden.has(kind - 16);
    lookup[2 * kind] = kind !== 0 && !left ? 255 : 0;
    lookup[2 * kind + 1] = left ? 0 : 255;
  }
  const out = new Uint8Array(2 * overlay.length);
  for (let i = 0; i < overlay.length; i += 1) {
    out[2 * i] = lookup[2 * overlay[i]];
    out[2 * i + 1] = lookup[2 * overlay[i] + 1];
  }
  return out;
}

if (typeof window !== 'undefined') {
  window.VolumeViewer = VolumeViewer;
  window.BACKGROUNDS = BACKGROUNDS;
}
if (typeof module !== 'undefined') {
  module.exports = { visibilityChannels, firstOpaque, volumeFragment };
}
