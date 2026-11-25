// VoxelSieve: browser UI over the GUI API (ADR 0008). Plain JavaScript, no build step.
// Everything goes through POST api/call/<method>, the same methods MCP clients use.
'use strict';

const $ = (id) => document.getElementById(id);

const state = {
  status: { open: false },
  operations: [],
  stage: 'dataset',
  busy: false,
  viewer: null,
  volume: null,
  viewMode: '3d',
  pores: { step: null, list: [] },
  projectDir: null,     // directory of the open project, to notice when another one is opened
  transfer: null,       // transfer function of the 3D view
  restore: null,        // parts of a saved view state still to apply: {slice, volume, transfer}
  savedState: {},       // view state last sent to the GUI
  suggestions: null,    // renderings suggested for the loaded volume: {key, items, images}
  capture: null,        // returns the picture of the current view as a PNG data URL
  scene: null,          // 3D view of all objects (scene.js)
  align: null,          // alignment form of the scene: moving, target, picking, picked points
  shownResults: {},     // result step shown in the views, by result type (ADR 0023)
  pendingScan: null,    // a scan whose detected values are shown for a check (inspect_scan)
  nextSteps: null,      // analyses suggested for the shown dataset: {key, list, error}
  layers: null,         // what the views show (layerState)
  layerSelected: 'grey',  // entry of the layer tree whose properties are shown
  sideTab: 'analyses',  // right column of the workspace: 'analyses' or 'history'
  openAnalysis: null,   // operation whose card is open
  openGroups: null,     // analysis group key -> open, as the user left it
  jumpTo: null,         // voxel the slice view shows once loaded: {voxel, size}
};

// Labels of known parameters; forms show them in this order.
const LABELS = {
  path: 'File',
  dims: 'Dimensions (voxels)',
  voxel_size_mm: 'Voxel size (mm)',
  slice_thickness_mm: 'Slice thickness (mm)',
  folder: 'Folder',
  sample_type: 'Data type',
  big_endian: 'Big Endian',
  header_bytes: 'Header (bytes)',
  threshold: 'Threshold',
  air_threshold: 'Air threshold',
  materials: 'Materials',
  margin_voxels: 'Air margin (voxels)',
  brick_size: 'Brick size (voxels)',
  min_pore_voxels: 'Smallest pore (voxels)',
  zone_sigma: 'Zone threshold (σ)',
  min_zone_void_fraction: 'Min. void fraction per zone',
  order_path: 'Inspection order (file)',
  order: 'Inspection order (JSON)',
  template_path: 'Report template (file)',
  bins: 'Bins',
  level: 'Resolution level',
  bits: 'Bits per voxel',
  band_voxels: 'Band width (± voxels)',
  iso_value: 'Surface grey value',
  stl: 'Write STL mesh',
  vdb: 'Write VDB level set',
  cad_path: 'CAD model (STL)',
  model_path: 'Learned model (.vsm)',
  tile: 'Tile edge (voxels)',
  alignment: 'Alignment',
  tolerance_mm: 'Tolerance (± mm)',
  outer_surface_only: 'Compare the outer skin only',
  aligned_stl: 'Write the aligned CAD model as STL',
  method: 'Method',
  min_wall_mm: 'Minimum wall (mm)',
  max_wall_mm: 'Maximum wall (mm)',
  max_thickness_mm: 'Measure walls up to (mm)',
  search_angle_deg: 'Search angle (°)',
  max_triangles: 'Triangle budget of the surface',
  volume_map: 'Also map the thickness inside the part',
  map_max_thickness_mm: 'Thickness map up to (mm)',
  nominal_thickness: 'Compare with the nominal thickness of the CAD model',
  nominal_tolerance_mm: 'Tolerance against nominal (± mm, 0: from the comparison)',
};

const SUMMARY_LABELS = {
  dims: 'Dimensions',
  voxel_size_mm: 'Voxel size (mm)',
  slice_thickness_mm: 'Slice thickness (mm)',
  levels: 'Resolution levels',
  bricks: 'Bricks',
  active_voxels: 'Active voxels',
  threshold: 'Threshold',
  air_threshold: 'Air threshold',
  model: 'Model',
  materials: 'Materials',
  header_bytes: 'Header (bytes)',
  pores: 'Pores',
  region: 'Region',
  pore_volume_mm3: 'Pore volume (mm³)',
  zones: 'Loosened zones',
  zone_void_volume_mm3: 'Void volume in zones (mm³)',
  part_volume_mm3: 'Part volume (mm³)',
  porosity: 'Porosity',
  evaluated_zones: 'Evaluated inspection zones',
  missing_fields: 'Missing fields',
  passed: 'Result',
  surface_blocks: 'Surface blocks (8³)',
  band_voxels: 'Voxels in the distance band',
  file_bytes: 'File size (bytes)',
  bits_per_voxel: 'Bits per voxel',
  compression_vs_raw: 'Compression vs. 16-bit raw data',
  step_voxels: 'Distance step (voxels)',
  iso_value: 'Surface grey value',
  surface_volume_mm3: 'Volume from the surface (mm³)',
  deviation_mean_mm: 'Mean deviation (mm)',
  deviation_rms_mm: 'RMS of the deviation (mm)',
  deviation_min_mm: 'Smallest deviation (mm)',
  deviation_max_mm: 'Largest deviation (mm)',
  within_tolerance_percent: 'Within tolerance (% of area)',
  above_tolerance_percent: 'Above tolerance (% of area)',
  below_tolerance_percent: 'Below tolerance (% of area)',
  tolerance_mm: 'Tolerance (± mm)',
  fit_rms_mm: 'Residual alignment error (mm)',
  rotation_deg: 'Rotation (°)',
  translation_mm: 'Translation (mm)',
  moved_objects: 'Moved objects',
  target: 'Aligned to',
  start: 'Start',
  pairs: 'Point pairs',
  residuals_mm: 'Distance left per pair (mm)',
  rms_mm: 'RMS of the distances left (mm)',
  max_residual_mm: 'Largest distance left (mm)',
  fit_inliers: 'Share of points in the fit',
  fit_iterations: 'Iterations',
  resolution_mm: 'Resolution of the fit (mm)',
  nominal: 'Nominal object',
  dropped_components: 'Omitted internal surfaces',
  displacement_max_mm: 'Non-rigid: largest displacement (mm)',
  displacement_mean_mm: 'Non-rigid: mean displacement (mm)',
  rigid_deviation_rms_mm: 'Rigid alignment alone: RMS of the deviation (mm)',
  rigid_within_tolerance_percent: 'Rigid alignment alone: within tolerance (% of area)',
  method: 'Method',
  thickness_min_mm: 'Thinnest wall (mm)',
  thickness_median_mm: 'Median wall thickness (mm)',
  thickness_mean_mm: 'Mean wall thickness (mm)',
  thickness_max_mm: 'Thickest wall (mm)',
  measured_percent: 'Measured (% of the surface)',
  max_thickness_mm: 'Measured up to (mm)',
  min_wall_mm: 'Minimum wall (mm)',
  below_min_percent: 'Thinner than the minimum (% of the surface)',
  thin_regions: 'Thin regions',
  max_wall_mm: 'Maximum wall (mm)',
  above_max_percent: 'Thicker than the maximum (% of the surface)',
  thick_regions: 'Thick regions',
  map_median_mm: 'Thickness map: median (mm)',
  map_max_mm: 'Thickness map: thickest spot (mm)',
  map_range_mm: 'Thickness map: colour range (mm)',
  map_below_min_percent: 'Thickness map: thinner than the minimum (% of the material)',
  map_above_max_percent: 'Thickness map: thicker than the maximum (% of the material)',
  map_thick_regions: 'Thickness map: thick regions',
  nominal_tolerance_mm: 'Tolerance against nominal (± mm)',
  nominal_compared_percent: 'Compared with nominal (% of the measured surface)',
  nominal_deviation_min_mm: 'Largest shortfall against nominal (mm)',
  nominal_deviation_max_mm: 'Largest excess over nominal (mm)',
  thinner_than_nominal_percent: 'Thinner than nominal (% of the compared surface)',
  thicker_than_nominal_percent: 'Thicker than nominal (% of the compared surface)',
};

// ---------------------------------------------------------------------------------------------
// Helpers

/// An element with attributes and children. `icon` names an icon of icons.js drawn before the
/// children; a button that has only the icon needs a `title`, which also names it for screen
/// readers.
function el(tag, attributes = {}, ...children) {
  const element = document.createElement(tag);
  for (const [key, value] of Object.entries(attributes)) {
    if (value === undefined || value === null || value === false) continue;
    if (key.startsWith('on')) element.addEventListener(key.slice(2), value);
    else if (key === 'className') element.className = value;
    else if (key === 'icon') element.append(icon(value));
    else element.setAttribute(key, value === true ? '' : value);
  }
  const shown = children.flat().filter((child) => child !== undefined && child !== null &&
    child !== false && child !== '');
  for (const child of shown) {
    element.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  if (attributes.icon && !shown.length) {
    element.classList.add('icon-only');
    if (attributes.title && !attributes['aria-label']) {
      element.setAttribute('aria-label', attributes.title);
    }
  }
  return element;
}

function formatNumber(value) {
  if (typeof value !== 'number') return String(value);
  if (Number.isInteger(value)) return value.toLocaleString('en-US');
  return value.toLocaleString('en-US', { maximumSignificantDigits: 4 });
}

/// A voxel size: a number for cubes, [x, y, z] otherwise (ADR 0012).
function formatVoxelSize(value) {
  return Array.isArray(value) ? value.map(formatNumber).join(' × ') : formatNumber(value);
}

function formatValue(key, value) {
  if (key === 'porosity' && typeof value === 'number') return formatNumber(value * 100) + ' %';
  if (key === 'passed') return value ? 'passed' : 'failed';
  if (key.endsWith('_percent') && typeof value === 'number') {
    return value.toLocaleString('en-US', { maximumFractionDigits: 1 }) + ' %';
  }
  if (Array.isArray(value) && value.some((item) => item !== null && typeof item === 'object')) {
    return value.map((item) => JSON.stringify(item)).join('; ');
  }
  // Lists and positions, not sizes.
  if (Array.isArray(value) && ['translation_mm', 'residuals_mm', 'moved_objects', 'bounds_min_mm',
    'bounds_max_mm'].includes(key)) return value.map(formatNumber).join(', ');
  if (Array.isArray(value)) return value.map(formatNumber).join(' × ');
  if (value !== null && typeof value === 'object') return JSON.stringify(value);
  return formatNumber(value);
}

function formatBytes(bytes) {
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return formatNumber(unit === 0 ? value : Math.round(value * 10) / 10) + ' ' + units[unit];
}

function formatTime(iso) {
  if (!iso) return '';
  const date = new Date(iso);
  return Number.isNaN(date.getTime()) ? iso : date.toLocaleString('en-US');
}

function storageGet(key) {
  try {
    return localStorage.getItem(key);
  } catch {
    return null;
  }
}

function storageSet(key, value) {
  try {
    localStorage.setItem(key, value);
  } catch {
    // Storage is a convenience only.
  }
}

function fileUrl(step, output, file = '') {
  const parts = [step, output, ...file.split('/')].map((part) => encodeURIComponent(part));
  return 'files/' + parts.join('/');
}

// ---------------------------------------------------------------------------------------------
// API

async function api(method, params = {}) {
  const response = await fetch('api/call/' + method, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(params),
  });
  let body;
  try {
    body = await response.json();
  } catch {
    throw new Error(response.statusText || 'No answer from the server');
  }
  if (!response.ok) throw new Error(body.error || response.statusText);
  return body;
}

function showError(error) {
  $('error').textContent = error instanceof Error ? error.message : String(error);
  $('error').hidden = false;
}

function clearError() {
  $('error').hidden = true;
}

async function refresh() {
  state.status = await api('project_status');
  if (!state.status.open) {
    state.projectDir = null;
  } else if (state.status.dir !== state.projectDir) {
    projectOpened();
  }
  render();
}

// ---------------------------------------------------------------------------------------------
// View state: saved with the project, so it opens as it was left, and as named views

/// Stage for a project without a saved view state.
function defaultStage() {
  if (latestOutput('report')) return 'report';
  return latestOutput('dataset') ? 'workspace' : 'dataset';
}

function projectOpened() {
  state.projectDir = state.status.dir;
  state.viewer = null;
  state.volume = null;
  state.scene = null;
  // Their canvases go with them: a canvas keeps the context and input of its viewer.
  state.volumeCanvas = null;
  state.sceneCanvas = null;
  state.align = null;
  state.transfer = null;
  state.suggestions = null;
  state.pores = { step: null, list: [] };
  state.savedState = state.status.view ?? {};
  applyViewState(state.savedState);
}

/// Shows a view state: stage and view mode now, viewer settings once their data is loaded.
function applyViewState(view) {
  // Analysis, results and view were stages of their own before the workspace (ADR 0040).
  const stage = ['analysis', 'results', 'view'].includes(view?.stage) ? 'workspace' : view?.stage;
  state.stage = ['dataset', 'workspace', 'report'].includes(stage) ? stage : defaultStage();
  if (view?.stage === 'results') state.sideTab = 'history';
  if (['slice', '3d', 'scene'].includes(view?.viewMode)) state.viewMode = view.viewMode;
  state.restore = { slice: view?.slice ?? null, volume: view?.volume ?? null,
    transfer: view?.transfer ?? null, scene: view?.scene ?? null };
  // Both name the step they belong to, so they only apply to it.
  state.materialDraft = view?.materialDraft ?? null;
  state.materialColors = view?.materialColors ?? null;
  state.shownResults = view?.shownResults ?? {};
  state.layers = view?.layers ?? layersFromVolume(view?.volume);
}

function collectViewState() {
  const previous = state.savedState ?? {};
  const pending = state.restore ?? {};
  const transfer = state.transfer?.points
    ? { key: state.transfer.key, preset: state.transfer.preset,
      colorMap: state.transfer.colorMap, points: state.transfer.points,
      colorSource: state.transfer.colorSource ?? 'points', reverse: state.transfer.reverse ?? false,
      colorRange: state.transfer.colorRange ?? null,
      customStops: state.transfer.customStops ?? null }
    : pending.transfer ?? previous.transfer ?? null;
  return {
    stage: state.stage,
    viewMode: state.viewMode,
    slice: state.viewer?.info ? state.viewer.getState() : pending.slice ?? previous.slice ?? null,
    volume: state.volume?.volume ? state.volume.getState()
      : pending.volume ?? previous.volume ?? null,
    scene: state.scene?.fit ? state.scene.getState() : pending.scene ?? previous.scene ?? null,
    transfer,
    materialDraft: state.materialDraft ?? null,
    materialColors: state.materialColors ?? null,
    shownResults: state.shownResults ?? {},
    layers: state.layers,
  };
}

let viewSaveTimer = null;

/// Sends the view state to the GUI shortly after the last change.
function scheduleViewSave() {
  if (!state.status.open) return;
  clearTimeout(viewSaveTimer);
  viewSaveTimer = setTimeout(async () => {
    const view = collectViewState();
    if (JSON.stringify(view) === JSON.stringify(state.savedState)) return;
    try {
      await api('view_set', { state: view });
      state.savedState = view;
    } catch {
      // Not worth an error message; the next change tries again.
    }
  }, 700);
}

function downloadDataUrl(url, name) {
  const link = el('a', { href: url, download: name });
  document.body.append(link);
  link.click();
  link.remove();
}

function fileName(name) {
  return (name.replace(/[\\/:*?"<>|]+/g, '_').trim() || 'view') + '.png';
}

async function saveNamedView(name) {
  clearError();
  try {
    const image = state.capture ? state.capture() : undefined;
    await api('view_save', { name, state: collectViewState(), image_base64: image });
    await refresh();
  } catch (error) {
    showError(error);
  }
}

function showSavedView(view) {
  applyViewState(view.state);
  render();
}

async function renameSavedView(view) {
  const name = window.prompt('New name of the view', view.name);
  if (!name || name === view.name) return;
  await action(() => api('view_rename', { id: view.id, name }));
}

async function deleteSavedView(view) {
  if (!window.confirm('Delete the view "' + view.name + '"?')) return;
  await action(() => api('view_delete', { id: view.id }));
}

/// Toolbar of the view stage: save the current view under a name, export it as a picture, and
/// the saved views as pictures to click.
function renderViewBar(panel, modes) {
  const views = state.status.saved_views ?? [];
  const name = el('input', { type: 'text', placeholder: 'View ' + (views.length + 1),
    title: 'Name of the view', className: 'view-name' });
  const save = () => saveNamedView(name.value.trim() || name.placeholder);
  name.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') save();
  });
  panel.append(el('div', { className: 'viewer-tools view-bar' }, modes,
    el('span', { className: 'sep' }), name,
    el('button', { onclick: save, icon: 'image',
      title: 'Save the current view with a picture in the project' }, 'Save view'),
    el('button', {
      onclick: () => state.capture &&
        downloadDataUrl(state.capture(), fileName(name.value.trim() || 'view')),
      title: 'Download the current view as PNG', icon: 'download',
    }, 'Export image')));
  if (!views.length) return;
  panel.append(el('div', { className: 'saved-views' }, views.map((view) => el('div', {
    className: 'saved-view', title: 'Show view',
  },
  el('button', { className: 'thumb', onclick: () => showSavedView(view) },
    view.has_image ? el('img', { src: 'views/' + view.id + '.png', alt: view.name, loading: 'lazy' })
      : el('span', { className: 'hint' }, 'no image')),
  el('div', { className: 'saved-view-row' },
    el('span', { className: 'saved-view-name' }, view.name),
    view.has_image ? el('a', { href: 'views/' + view.id + '.png', download: fileName(view.name),
      title: 'Download image', icon: 'download' }) : null,
    el('button', { className: 'icon', title: 'Rename', icon: 'edit',
      onclick: () => renameSavedView(view) }),
    el('button', { className: 'icon', title: 'Delete', icon: 'delete',
      onclick: () => deleteSavedView(view) }))))));
}

async function action(work) {
  clearError();
  try {
    await work();
  } catch (error) {
    showError(error);
  }
  try {
    await refresh();
  } catch (error) {
    showError(error);
  }
}

function operationInfo(id) {
  return state.operations.find((operation) => operation.id === id);
}

/// Runs an operation in the background (review step 5.3): the call returns at once with a run id,
/// and the busy dialog follows the run through `wait` until it ends, so a reload of the page finds
/// the run again (resumeRun) instead of losing its progress and Cancel.
async function runStep(operation, params) {
  clearError();
  const title = operationInfo(operation)?.title ?? operation;
  showBusy(title);
  let step = null;
  try {
    const started = await api('run_' + operation, { ...params, background: true });
    step = await followRun(started.run_id);
  } catch (error) {
    showError(title + ': ' + error.message);
  } finally {
    hideBusy();
  }
  await refresh();
  return step;
}

function showBusy(title) {
  state.busy = true;
  $('busy-text').textContent = title + ' running …';
  $('busy-progress').removeAttribute('value');
  $('busy-preview').hidden = true;
  $('busy-cancel').disabled = false;
  $('busy').hidden = false;
  render();
}

function hideBusy() {
  state.busy = false;
  $('busy').hidden = true;
  $('busy-preview').hidden = true;
}

/// Waits for the background run `id`, showing its progress and the preview of an import; returns
/// its step (or the steps of a pipeline), or throws its error.
async function followRun(id) {
  // Slices read by the preview shown; a staged import sends a sharper one after each round.
  let previewSlices = 0;
  for (;;) {
    const waited = await api('wait', { seconds: 0.5 });
    if (waited.run_id !== id) throw new Error('Another run started');
    if (waited.done) {
      const result = waited.result;
      if (result && result.status === 'failed' && result.error) throw new Error(result.error);
      return result;
    }
    if (waited.running?.progress > 0) $('busy-progress').value = waited.running.progress;
    try {
      const status = await api('project_status');
      const slicesRead = status.running?.preview?.slices_read ?? 0;
      if (slicesRead > previewSlices) {
        previewSlices = slicesRead;
        await showImportPreview();
      }
    } catch {
      // The next round or the result shows the problem.
    }
  }
}

/// After a reload: shows a run that is still going on and follows it to its end.
async function resumeRun() {
  const running = state.status?.running;
  if (!running || !running.background || state.busy) return;
  const title = operationInfo(running.operation)?.title ?? running.operation;
  showBusy(title);
  try {
    await followRun(running.run_id);
  } catch (error) {
    showError(title + ': ' + error.message);
  } finally {
    hideBusy();
  }
  await refresh();
}
/// Shows the preview of a volume while its import runs (ADR 0020): first a few slices spread over
/// the volume, read first, with their histogram and the estimated threshold; then, while a
/// staged import copies the rest, sharper ones, until the histogram holds the whole volume.
async function showImportPreview() {
  const preview = await api('import_preview');
  if (!state.busy) return;
  $('busy-preview-image').src = 'data:' + preview.image.mime_type + ';base64,' +
    preview.image.base64;
  const mapping = preview.value_mapping;
  const threshold = mapping
    ? (mapping.offset + mapping.scale * preview.threshold).toPrecision(4)
    : Math.round(preview.threshold);
  const stride = preview.pixel_stride > 1
    ? ', averaged over ' + preview.pixel_stride + ' × ' + preview.pixel_stride + ' voxels'
    : '';
  const what = preview.complete
    ? 'every slice read'
    : preview.slices_read + ' of ' + preview.slices + ' slices read (' +
      (100 * preview.fraction_read).toFixed(1) + ' % of the voxels)';
  const then = preview.complete
    ? 'The import goes on with the histogram and the bricks.'
    : 'The import goes on and does not read these slices again; the preview gets sharper.';
  $('busy-preview-text').textContent = 'Preview after ' + preview.seconds.toFixed(1) + ' s, ' +
    what + stride + ': sections normal to z, y and x. Estimated threshold ' + threshold +
    ' (red line in the histogram). ' + then;
  $('busy-preview').hidden = false;
}

// ---------------------------------------------------------------------------------------------
// Project state

function activeSteps() {
  return (state.status.steps ?? []).filter((step) => step.active && step.status === 'done');
}

/// Latest active output of a type: {step, output}.
function latestOutput(type) {
  const steps = activeSteps();
  for (let i = steps.length - 1; i >= 0; i -= 1) {
    for (const [name, output] of Object.entries(steps[i].outputs)) {
      if (output.type === type) return { step: steps[i], output: name };
    }
  }
  return null;
}

/// The dataset the slice and 3D views show: the latest of the active object when that is a
/// volume, otherwise the latest of the project. {step, output} or null.
function shownDataset() {
  const active = state.status.active_object;
  const steps = activeSteps().filter((step) => active && step.object === active);
  for (let i = steps.length - 1; i >= 0; i -= 1) {
    for (const [name, output] of Object.entries(steps[i].outputs)) {
      if (output.type === 'dataset') return { step: steps[i], output: name };
    }
  }
  return latestOutput('dataset');
}

function latestStepOf(operation) {
  const steps = activeSteps().filter((step) => step.operation === operation);
  return steps.length ? steps[steps.length - 1] : null;
}

// ---------------------------------------------------------------------------------------------
// File browser

let browserResolve = null;
let browserOptions = {};
let browserSelection = null;

async function browserLoad(path) {
  try {
    const listing = await api('browse', path ? { path } : {});
    $('browser-path').value = listing.path;
    $('browser-up').dataset.parent = listing.parent;
    storageSet('voxelsieve.dir', listing.path);
    browserSelection = null;
    const list = $('browser-list');
    list.replaceChildren();
    for (const entry of listing.entries) {
      const selectable = browserOptions.kinds.includes(entry.kind);
      const navigable = entry.kind === 'dir' || (entry.directory && !selectable);
      const item = el('li', { className: selectable || navigable ? '' : 'disabled' },
        el('span', { className: 'kind', icon: KIND_ICONS[entry.kind] ?? 'file' },
          kindLabel(entry.kind)),
        el('span', {}, entry.name),
        entry.size_bytes !== undefined ? el('span', { className: 'size' },
          formatBytes(entry.size_bytes)) : null);
      item.addEventListener('click', () => {
        if (selectable) {
          for (const other of list.children) other.classList.remove('selected');
          item.classList.add('selected');
          browserSelection = entry;
        } else if (navigable) {
          browserLoad(entry.path);
        }
      });
      item.addEventListener('dblclick', () => {
        // A directory of TIFF, PNG or DICOM slices is chosen by a click and the button; a double
        // click opens it.
        if (selectable && ['tiff', 'png', 'dicom'].includes(entry.kind) && entry.directory) {
          browserLoad(entry.path);
        } else if (selectable) browserFinish(entry);
      });
      list.append(item);
    }
    if (!listing.entries.length) list.append(el('li', { className: 'disabled' }, 'empty'));
  } catch (error) {
    showError(error);
  }
}

const KIND_ICONS = { dir: 'folder', project: 'open', dataset: 'scan', raw: 'scan', tiff: 'scan',
  dicom: 'scan', gzipped_xml: 'scan', mesh: 'cad' };

function kindLabel(kind) {
  return { dir: 'Folder', project: 'Project', dataset: 'Dataset', raw: 'Raw data',
    tiff: 'TIFF', png: 'PNG', dicom: 'DICOM', metaimage: 'MetaImage', nrrd: 'NRRD', hdf5: 'HDF5',
    gzipped_xml: 'Gzipped XML', mesh: 'CAD model', file: 'File' }[kind] ?? kind;
}

function browserFinish(entry) {
  const resolve = browserResolve;
  browserResolve = null;
  $('browser').close();
  if (resolve) resolve(entry);
}

/// Opens the file browser. `kinds`: selectable entry kinds; `name`: ask for a new name in the
/// current directory instead. Resolves to {path, kind} or null.
function browse({ title, kinds = [], name = null }) {
  browserOptions = { kinds, name };
  $('browser-title').textContent = title;
  $('browser-name-row').hidden = name === null;
  $('browser-name').value = name ?? '';
  $('browser').showModal();
  browserLoad(storageGet('voxelsieve.dir'));
  return new Promise((resolve) => {
    browserResolve = resolve;
  });
}

function setupBrowser() {
  $('browser-up').addEventListener('click', () => browserLoad($('browser-up').dataset.parent));
  $('browser-path').addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      browserLoad($('browser-path').value);
    }
  });
  // Enter in the name field confirms; otherwise the form's implicit submission would press the
  // first button, Cancel, and only close the dialog.
  $('browser-name').addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      $('browser-ok').click();
    }
  });
  $('browser-ok').addEventListener('click', () => {
    if (browserOptions.name !== null) {
      const name = $('browser-name').value.trim();
      if (!name) return;
      browserFinish({ path: $('browser-path').value.replace(/\/$/, '') + '/' + name, kind: 'new' });
    } else if (browserSelection) {
      browserFinish(browserSelection);
    }
  });
  $('browser').addEventListener('close', () => {
    if (browserResolve) browserFinish(null);
  });
}

// ---------------------------------------------------------------------------------------------
// Opening a scan in one go (ADR 0039): what it is, a project next to it, the import, the 3D view
// and the analyses that suit it.

/// Entry kinds the scan browser offers.
const SCAN_KINDS = ['dataset', 'raw', 'tiff', 'png', 'dicom', 'metaimage', 'nrrd', 'hdf5',
  'gzipped_xml', 'project', 'mesh', 'file'];

const FROM_LABELS = {
  sidecar: 'from the sidecar file',
  files: 'from the files',
  dataset: 'from the dataset',
  'file name': 'from the file name',
  'grey values': 'detected in the grey values',
  parameters: 'given',
  assumed: 'not known: 1 mm assumed',
};

/// Asks for a scan and opens it. `settings`: always show the values and all import settings.
async function chooseScan(settings = false) {
  const chosen = await browse({ title: 'Choose a CT scan', kinds: SCAN_KINDS });
  if (!chosen) return;
  await inspectAndOpen(chosen.path, settings);
}

/// Opens the scan at `path` at once when everything about it is known; otherwise shows what was
/// detected, for a check, first.
async function inspectAndOpen(path, settings = false) {
  clearError();
  let inspected;
  try {
    inspected = await api('inspect_scan', { path });
  } catch (error) {
    showError(error);
    return;
  }
  if (inspected.kind === 'project') {
    await action(() => api('project_open', { path }));
    return;
  }
  if (settings || inspected.check || !inspected.ready) {
    state.pendingScan = { ...inspected, settings };
    render();
    return;
  }
  await openScan(inspected, {});
}

/// Imports the scan (into a new project next to it when none is open), then shows it in 3D.
async function openScan(inspected, params) {
  clearError();
  state.pendingScan = null;
  const title = inspected.kind === 'mesh' ? 'Adding ' + inspected.name
    : 'Import of ' + inspected.name;
  showBusy(title);
  let opened = null;
  try {
    opened = await api('open_scan', { path: inspected.path, params, background: true });
    if (opened.run) await followRun(opened.run.run_id);
  } catch (error) {
    showError(title + ': ' + error.message);
    opened = null;
  } finally {
    hideBusy();
  }
  await refresh();
  if (opened && inspected.kind !== 'mesh') {
    // The first look: the part in 3D, with the analyses that suit it beside it.
    state.stage = 'workspace';
    state.viewMode = '3d';
    render();
  }
}

/// The values found for a scan, for a check before it is imported: size, grey values and voxel
/// size with where each comes from, editable where they were guessed, and all import settings.
function scanCheckCard(inspected) {
  const found = inspected.found ?? {};
  const from = inspected.from ?? {};
  const raw = inspected.kind === 'raw';
  const fromLabel = (key) => FROM_LABELS[from[key]] ?? '';
  const dims = [0, 1, 2].map((axis) => el('input', { type: 'number', min: 1, step: 1,
    value: found.dims?.[axis] ?? '', title: 'xyz'[axis] + ' in voxels', disabled: !raw }));
  const pitchValue = Array.isArray(found.voxel_size_mm) ? found.voxel_size_mm[0]
    : found.voxel_size_mm;
  const pitch = el('input', { type: 'number', min: 0, step: 'any', placeholder: '1',
    value: from.voxel_size === 'assumed' ? '' : (pitchValue ?? '') });
  const sampleLabel = { uint8: '8 bit', uint16: '16 bit', int8: '8 bit signed',
    int16: '16 bit signed', uint32: '32 bit', int32: '32 bit signed', float32: '32 bit float',
    float64: '64 bit float' };
  const bits = found.sample_type ? (sampleLabel[found.sample_type] ?? found.sample_type) +
    (found.byte_order && !found.sample_type.endsWith('8') ? ', ' + found.byte_order + ' endian'
      : '') +
    (found.header_bytes ? ', header of ' + formatNumber(found.header_bytes) + ' bytes' : '')
    : found.bits ? found.bits + ' bit' + (found.float ? ' float' : '') : null;
  const rows = [
    el('tr', {}, el('td', {}, 'Size'), el('td', {}, dims[0], ' × ', dims[1], ' × ', dims[2],
      ' voxels'), el('td', {}, fromLabel('dims'))),
    bits ? el('tr', {}, el('td', {}, 'Grey values'), el('td', {}, bits),
      el('td', {}, fromLabel('sample_type'))) : null,
    el('tr', {}, el('td', {}, 'Voxel size'), el('td', {}, pitch, ' mm'),
      el('td', {}, fromLabel('voxel_size'))),
  ];
  const info = inspected.operation ? operationInfo(inspected.operation) : null;
  const form = info ? buildForm(info.parameters, inspected.params ?? { path: inspected.path })
    : null;
  const advanced = form ? el('details', { open: Boolean(inspected.settings) },
    el('summary', {}, 'All import settings'), form.element) : null;
  const readQuick = () => {
    const params = {};
    if (raw) {
      const values = dims.map((input) => Number(input.value));
      if (values.some((value) => !Number.isInteger(value) || value < 1)) {
        throw new Error('Enter the size in voxels along x, y and z');
      }
      params.dims = values;
    }
    if (pitch.value !== '') {
      const value = Number(pitch.value);
      if (!(value > 0)) throw new Error('The voxel size must be above 0 mm');
      params.voxel_size_mm = value;
    }
    return params;
  };
  return el('div', { className: 'card scan-check' },
    el('h3', {}, inspected.name),
    el('p', {}, inspected.path),
    el('table', { className: 'found' }, el('tbody', {}, rows)),
    (inspected.notes ?? []).map((note) => el('p', {
      className: 'note' + (/assumed|grey values|Enter/.test(note) ? ' guess' : '') }, note)),
    inspected.project_dir && !state.status.open
      ? el('p', { className: 'hint' }, 'The project is saved in ' + inspected.project_dir + '.')
      : null,
    advanced,
    el('div', { className: 'row' },
      el('button', { className: 'primary', disabled: state.busy || !inspected.operation,
        onclick: () => {
          let params;
          try {
            params = advanced?.open ? form.values() : readQuick();
          } catch (error) {
            showError(error);
            return;
          }
          if (advanced?.open) delete params.path;
          openScan(inspected, params);
        } }, 'Open'),
      el('button', { onclick: () => { state.pendingScan = null; render(); } }, 'Cancel')));
}

/// The analyses suggested for the shown dataset, loaded once per dataset and state of the
/// protocol; each runs with one click and shows its result.
function nextStepsRow(dataset) {
  const key = dataset.step.id + ':' + state.status.cursor + ':' + (state.status.steps ?? []).length;
  const row = el('div', { className: 'next-steps' });
  if (state.nextSteps?.key !== key) {
    state.nextSteps = { key, list: null, row: null };
    api('suggest_analyses', { step: dataset.step.id, output: dataset.output }).then((answer) => {
      if (state.nextSteps?.key !== key) return;
      state.nextSteps.list = answer.suggestions;
      // The row of the latest rendering.
      if (state.nextSteps.row) fillNextSteps(state.nextSteps.row);
    }).catch(() => {
      if (state.nextSteps?.key === key) state.nextSteps.list = [];
    });
  }
  state.nextSteps.row = row;
  fillNextSteps(row);
  return row;
}

function fillNextSteps(row) {
  const list = state.nextSteps?.list;
  if (!list?.length) {
    row.hidden = true;
    return;
  }
  row.hidden = false;
  // Beside the analyses, the done ones are in their groups already.
  const shown = row.classList.contains('open-only') ? list.filter((s) => !s.done) : list;
  if (!shown.length) {
    row.hidden = true;
    return;
  }
  row.replaceChildren(el('span', { className: 'label' }, 'Suggested analyses'),
    ...shown.slice(0, 5).map((suggestion) => el('div', {
      className: 'next-step' + (suggestion.done ? ' done' : ''), title: suggestion.why },
    el('b', { icon: OPERATION_ICONS[suggestion.operation] ?? 'analysis' }, suggestion.title),
    el('p', {}, suggestion.why),
    el('div', { className: 'row' }, suggestion.done
      ? [el('span', { className: 'done-mark', icon: 'check' }, 'done'), el('button', {
        icon: 'show', onclick: () => showSuggestionResult(suggestion.operation) }, 'Show')]
      : el('button', { className: 'primary', icon: 'run', disabled: state.busy, onclick: async () => {
        const step = await runStep(suggestion.operation, suggestion.params);
        if (step) showSuggestionResult(suggestion.operation);
      } }, 'Run')))));
}

/// Shows the result of an analysis in the view that suits it.
function showSuggestionResult(operation) {
  showResultOf(operation);
}

// ---------------------------------------------------------------------------------------------
// Forms from JSON schema

/// A form of the parameters of `schema`, without those in `skip`; with `only`, of those alone.
function buildForm(schema, initial = {}, skip = ['inputs'], only = null) {
  const form = el('div', { className: 'form' });
  const readers = [];
  const required = schema.required ?? [];
  // Known parameters in the order of LABELS, then the others; schemas arrive sorted by name.
  const known = Object.keys(LABELS);
  const rank = (name) => (known.includes(name) ? known.indexOf(name) : known.length);
  const properties = Object.entries(schema.properties ?? {})
    .sort(([a], [b]) => rank(a) - rank(b) || a.localeCompare(b));
  for (const [name, property] of properties) {
    if (skip.includes(name) || (only && !only.includes(name))) continue;
    const value = initial[name] ?? property.default;
    const label = (LABELS[name] ?? name) + (required.includes(name) ? ' *' : '');
    const field = el('div', { className: 'field' });
    let read;
    if (Array.isArray(property.type) && property.type.includes('number') &&
        property.type.includes('array')) {
      // One value for all axes, or x, y and z (voxel sizes, ADR 0012).
      const values = Array.isArray(value) ? value : [value, undefined, undefined];
      const inputs = ['x (or all)', 'y', 'z'].map((placeholder, i) => el('input', {
        type: 'number', step: 'any', min: 0, value: values[i] ?? '', placeholder,
        name: name + i }));
      field.append(...inputs);
      read = () => {
        const given = inputs.map((input) => input.value !== '');
        if (!given.some(Boolean)) return undefined;
        if (given[0] && !given[1] && !given[2]) return Number(inputs[0].value);
        if (given.every(Boolean)) return inputs.map((input) => Number(input.value));
        throw new Error((LABELS[name] ?? name) + ': give one value for all axes or x, y and z');
      };
    } else if (property.type === 'integer' || property.type === 'number') {
      const input = el('input', {
        type: 'number', step: property.type === 'integer' ? 1 : 'any',
        min: property.minimum, max: property.maximum, value: value ?? '', name,
      });
      field.append(input);
      read = () => (input.value === '' ? undefined : Number(input.value));
    } else if (property.type === 'boolean') {
      const input = el('input', { type: 'checkbox', checked: Boolean(value), name });
      field.append(input);
      read = () => input.checked;
    } else if (property.type === 'string' && property.enum) {
      const select = el('select', { name },
        property.enum.map((option) => el('option', { value: option, selected: option === value },
          option)));
      field.append(select);
      read = () => select.value;
    } else if (property.type === 'array' && property.minItems &&
               property.minItems === property.maxItems) {
      const inputs = [];
      for (let i = 0; i < property.minItems; i += 1) {
        const input = el('input', { type: 'number', step: 'any', value: value?.[i] ?? '',
          name: name + i });
        inputs.push(input);
        field.append(input);
      }
      read = () => (inputs.every((input) => input.value === '') ? undefined
        : inputs.map((input) => Number(input.value)));
    } else if (property.type === 'array' &&
               ['number', 'integer', 'string'].includes(property.items?.type)) {
      // A list such as thresholds, names or colours: comma-separated.
      const numbers = property.items.type !== 'string';
      const input = el('input', { type: 'text', value: Array.isArray(value) ? value.join(', ') : '',
        name, spellcheck: 'false', placeholder: numbers ? '1, 2, …' : 'a, b, …' });
      field.append(input);
      read = () => {
        const items = input.value.split(',').map((item) => item.trim()).filter(Boolean);
        if (!items.length) return undefined;
        if (!numbers) return items;
        const values = items.map(Number);
        if (values.some(Number.isNaN)) {
          throw new Error((LABELS[name] ?? name) + ': give numbers separated by commas');
        }
        return values;
      };
    } else if (property.type === 'object') {
      const text = el('textarea', { rows: 6, name, placeholder: '{ }' });
      text.value = value ? JSON.stringify(value, null, 2) : '';
      field.append(text);
      read = () => {
        if (!text.value.trim()) return undefined;
        try {
          return JSON.parse(text.value);
        } catch (error) {
          throw new Error((LABELS[name] ?? name) + ': not valid JSON (' + error.message + ')');
        }
      };
    } else {
      const input = el('input', { type: 'text', value: value ?? '', name, spellcheck: 'false' });
      field.append(input);
      if (name === 'path' || name.endsWith('_path')) {
        field.append(el('button', {
          type: 'button',
          onclick: async () => {
            const chosen = await browse({ title: LABELS[name] ?? name,
              kinds: ['file', 'raw', 'mesh', 'tiff', 'png', 'dicom', 'metaimage', 'nrrd',
                'hdf5', 'gzipped_xml'] });
            if (chosen) input.value = chosen.path;
          },
        }, '…'));
      }
      read = () => (input.value === '' ? undefined : input.value);
    }
    form.append(el('label', {}, label), field);
    if (property.description) form.append(el('div', { className: 'help' }, property.description));
    readers.push([name, read]);
  }
  return {
    element: form,
    values() {
      const values = {};
      for (const [name, read] of readers) {
        const value = read();
        if (value !== undefined) values[name] = value;
      }
      return values;
    },
  };
}

// ---------------------------------------------------------------------------------------------
// Rendering

/// Key figures as tiles of large numbers, which read faster than a table.
function figureTiles(figures) {
  const tiles = Object.entries(figures ?? {}).filter(([key]) => key !== 'motion')
    .map(([key, value]) => el('div', { className: 'figure' },
      el('div', { className: 'figure-label' }, SUMMARY_LABELS[key] ?? key),
      el('div', { className: 'figure-value' + (key === 'passed' ? (value ? ' passed' : ' not-passed')
        : '') }, formatValue(key, value))));
  return tiles.length ? el('div', { className: 'figures' }, tiles) : null;
}

function summaryTable(summary) {
  // The motion matrix of an alignment is shown as its rotation and translation.
  const rows = Object.entries(summary ?? {}).filter(([key]) => key !== 'motion').map(([key, value]) => el('tr', {},
    el('td', {}, SUMMARY_LABELS[key] ?? key),
    el('td', { className: key === 'passed' ? (value ? 'passed' : 'not-passed') : null },
      formatValue(key, value))));
  return rows.length ? el('table', { className: 'summary' }, rows) : null;
}

// Telemetry: time and resources of a step, per phase (step_telemetry for the timeline).

const telemetryTimelines = new Map();

function formatSeconds(seconds) {
  if (seconds >= 90) return formatNumber(Math.round(seconds / 6) / 10) + ' min';
  return formatNumber(Math.round(seconds * 10) / 10) + ' s';
}

function formatMb(mb) {
  return formatBytes(Math.round(mb * 1024 * 1024));
}

function telemetryLine(telemetry) {
  const total = telemetry?.total;
  if (!total) return '';
  const threads = telemetry.system?.threads;
  return ' · ' + formatSeconds(total.wall_s) + ', ' + formatNumber(total.cores_used) +
    (threads ? ' of ' + threads : '') + ' cores, ' + formatMb(total.peak_anon_mb ?? total.peak_rss_mb) + ' peak';
}

function telemetryChart(timeline, system) {
  const points = timeline.t_s ?? [];
  if (points.length < 2) return el('p', { className: 'meta' }, 'Too short for a timeline');
  const width = 320;
  const height = 90;
  const end = points[points.length - 1];
  // The process's own memory; mapped file pages can be dropped by the system at any time.
  const memory = points.map((_, i) => timeline.rss_anon_mb[i]);
  const series = [
    { name: 'Cores', values: timeline.cores_used, max: system?.threads || Math.max(...timeline.cores_used), color: 'var(--accent)' },
    { name: 'Memory', values: memory, max: Math.max(...memory, 1), color: 'var(--good)' },
    { name: 'Disk read', values: timeline.disk_read_mb_s, max: Math.max(...timeline.disk_read_mb_s, 1), color: 'var(--bad)' },
  ];
  const lines = series.map((line) => {
    const coordinates = line.values.map((value, i) =>
      (points[i] / end * width).toFixed(1) + ',' + (height - Math.min(value / line.max, 1) * (height - 2) - 1).toFixed(1));
    return '<polyline fill="none" stroke-width="1.5" stroke="' + line.color + '" points="' + coordinates.join(' ') + '"/>';
  });
  const chart = el('div', { className: 'telemetry-chart' });
  chart.innerHTML = '<svg viewBox="0 0 ' + width + ' ' + height + '" preserveAspectRatio="none">' + lines.join('') + '</svg>';
  const legend = el('div', { className: 'meta' }, series.map((line) => el('span', {},
    el('span', { className: 'swatch', style: 'background:' + line.color }),
    line.name + ' (max ' + (line.name === 'Cores' ? formatNumber(line.max)
      : line.name === 'Memory' ? formatMb(line.max) : formatMb(line.max) + '/s') + ') ')),
    ' · ' + formatSeconds(end));
  return el('div', {}, chart, legend);
}

function telemetryDetails(step) {
  const telemetry = step.telemetry;
  if (!telemetry?.total) return null;
  const rows = [...(telemetry.phases ?? []), { ...telemetry.total, name: 'Total', depth: -1 }].map((phase) =>
    el('tr', { className: phase.depth < 0 ? 'total' : null },
      el('td', { style: 'padding-left:' + (Math.max(phase.depth, 0) * 12) + 'px' }, phase.name),
      el('td', {}, formatSeconds(phase.wall_s)),
      el('td', {}, formatNumber(phase.cores_used)),
      el('td', {}, formatMb(phase.peak_anon_mb ?? phase.peak_rss_mb)),
      el('td', {}, formatMb(phase.peak_file_mb ?? 0))));
  const table = el('table', { className: 'summary telemetry' },
    el('tr', {}, ['Phase', 'Time', 'Cores', 'Memory', 'Mapped'].map((h) => el('th', {}, h))),
    rows);
  const total = telemetry.total;
  const disk = el('div', { className: 'meta' }, 'Disk: ' + formatMb(total.disk_read_mb) + ' read, ' +
    formatMb(total.disk_write_mb) + ' written, ' + formatNumber(total.major_faults) + ' page faults' +
    (total.brick_read_mb ? ' · bricks loaded: ' + formatMb(total.brick_read_mb) : ''));
  const chart = el('div', {});
  const key = step.id + '/' + step.started;
  const load = async () => {
    if (!telemetryTimelines.has(key)) {
      telemetryTimelines.set(key, api('step_telemetry', { step: step.id }).catch(() => null));
    }
    const record = await telemetryTimelines.get(key);
    chart.replaceChildren(record?.timeline ? telemetryChart(record.timeline, record.system) : '');
  };
  return el('details', { ontoggle: (event) => { if (event.target.open) load(); } },
    el('summary', {}, 'Run time and resources'),
    table,
    disk,
    (telemetry.hints ?? []).map((hint) => el('div', { className: 'message' }, hint)),
    chart);
}

function renderHeader() {
  const status = state.status;
  $('project-name').textContent = status.open ? status.name + ' — ' + status.dir : 'No project';
  $('btn-save-as').disabled = !status.open || state.busy;
  $('btn-undo').disabled = !status.can_undo || state.busy;
  $('btn-redo').disabled = !status.can_redo || state.busy;
  $('btn-new').disabled = state.busy;
  $('btn-open').disabled = state.busy;
}

function renderProtocol() {
  const list = $('protocol');
  list.replaceChildren();
  const steps = state.status.steps ?? [];
  $('protocol-empty').hidden = steps.length > 0;
  for (const step of steps) {
    const undone = !step.active && step.status !== 'failed';
    const classes = [step.status, undone ? 'undone' : ''].join(' ');
    const statusText = { done: '✓', failed: '✗', running: '…' }[step.status] ?? step.status;
    const item = el('li', { className: classes },
      el('div', { className: 'title' }, statusText + ' ' + step.id + '. ' + step.title),
      el('div', { className: 'meta' }, formatTime(step.started),
        step.size_bytes ? ' · ' + formatBytes(step.size_bytes) : '',
        telemetryLine(step.telemetry),
        undone ? ' · undone' : ''),
      step.messages.slice(-2).map((message) => el('div', { className: 'message' }, message)),
      // A step that was interrupted and kept its files continues when it runs again as it was.
      step.resumable
        ? el('button', {
          disabled: state.busy,
          onclick: () => runStep(step.operation,
            { ...step.params, ...(step.object_name ? { object_name: step.object_name } : {}) }),
        }, 'Resume')
        : '',
      el('details', {}, el('summary', {}, 'Details'),
        summaryTable(step.summary),
        el('pre', {}, JSON.stringify({ params: step.params, inputs: step.inputs }, null, 1))),
      telemetryDetails(step));
    list.append(item);
  }
  list.lastElementChild?.scrollIntoView({ block: 'nearest' });
}

function renderStages() {
  const complete = {
    dataset: Boolean(latestOutput('dataset')),
    workspace: (state.status.results ?? []).length > 0,
    report: Boolean(latestOutput('report')),
  };
  for (const button of document.querySelectorAll('.stages button')) {
    const stage = button.dataset.stage;
    button.classList.toggle('current', stage === state.stage);
    button.classList.toggle('complete', complete[stage]);
  }
}

function runButton(label, operation, readParams, onDone) {
  return el('button', {
    className: 'primary',
    icon: 'run',
    disabled: state.busy,
    onclick: async () => {
      let params;
      try {
        params = readParams();
      } catch (error) {
        showError(error);
        return;
      }
      const step = await runStep(operation, params);
      if (step && onDone) onDone(step);
    },
  }, label);
}

function nextButton(label, stage) {
  return el('button', { onclick: () => { state.stage = stage; render(); } }, label + ' →');
}

function renderDatasetStage(panel) {
  panel.append(el('h1', {}, 'Dataset'));
  renderObjectsCard(panel);
  const dataset = latestOutput('dataset');
  if (dataset) {
    panel.append(el('div', { className: 'card' },
      el('h3', {}, 'Current dataset (step ' + dataset.step.id + ')'),
      el('p', {}, dataset.step.outputs[dataset.output].path),
      summaryTable(dataset.step.summary),
      el('div', { className: 'row' }, nextButton('On to the analysis', 'workspace'))));
  } else {
    panel.append(el('p', { className: 'hint' },
      'Open a raw file, a TIFF stack (folder, multi-page TIFF or ZIP), a folder of DICOM ' +
      'slices, a gzipped XML project or a sieved dataset (.vsieve). Size, grey values and voxel ' +
      'size are found in the files, or in the name and grey values of a raw file without a ' +
      'sidecar. On import the air around the part is removed and the volume stored with ' +
      'resolution levels; the original stays unchanged.'));
  }
  if (state.pendingScan) panel.append(scanCheckCard(state.pendingScan));
  panel.append(el('div', { className: 'row' },
    el('button', { className: dataset ? null : 'primary', disabled: state.busy,
      onclick: () => chooseScan() }, dataset ? 'Open another scan …' : 'Open a scan …'),
    el('button', { disabled: state.busy, title: 'Shows the detected values and every setting of ' +
      'the import (threshold, air margin, staging, value type) before it starts',
    onclick: () => chooseScan(true) }, 'Import with settings …')));
}

function appendImages(container, step) {
  const [output] = Object.keys(step.outputs);
  if (!output) return;
  api('list_files', { step: step.id, output }).then((listing) => {
    const images = listing.files.filter((file) => file.file.endsWith('.png'));
    if (!images.length) return;
    container.append(el('div', { className: 'images' }, images.map((file) =>
      el('a', { href: fileUrl(step.id, output, file.file), target: '_blank' },
        el('img', { src: fileUrl(step.id, output, file.file), alt: file.file, title: file.file })))));
  }).catch(() => {});
}

/// The regions of the shown scan that an inspection zone can name, or nothing.
function regionsOfOrderHint() {
  const regions = scanRegions(shownDataset());
  if (!regions.length) return null;
  const example = JSON.stringify({ name: regions[0].name, region: regions[0].name,
    max_pore_count: 0 });
  return el('p', { className: 'hint' }, 'A zone of the order can name a region of the scan (' +
    regions.map((region) => region.name).join(', ') + '), for instance ', el('code', {}, example),
    '. The report then evaluates the pores in it, also those of an analysis of the whole part.');
}

function renderReportStage(panel) {
  panel.append(el('h1', {}, 'Test report'));
  const porosity = latestOutput('porosity');
  const info = operationInfo('report');
  if (!porosity || !info) {
    panel.append(el('p', { className: 'hint' }, 'Run a porosity analysis first.'),
      nextButton('To the analysis', 'workspace'));
    return;
  }
  const last = latestStepOf('report');
  const form = buildForm(info.parameters, last ? last.params : {});
  panel.append(el('div', { className: 'card' },
    el('p', {}, 'The inspection order holds laboratory, customer, part, scan parameters and the ' +
      'acceptance limits per inspection zone: BDG P 202 limits or the BDG P 203 porosity key ' +
      'of the drawing, in a region of boxes, spheres, cylinders or meshes of the project. ' +
      'Entries in the JSON field add to or override the file. Examples are in ' +
      'examples/inspection_order.json and examples/inspection_order_p203.json.'),
    regionsOfOrderHint(),
    form.element,
    el('div', { className: 'row' },
      runButton('Show inspection regions', 'inspection_regions', () => {
        // The regions of the same order, as mesh objects in the slice and 3D views.
        const { order, order_path: orderPath } = form.values();
        return Object.fromEntries(Object.entries({ order, order_path: orderPath })
          .filter(([, value]) => value !== undefined && value !== ''));
      }),
      runButton('Create report', 'report', () => form.values()))));
  if (last) {
    const url = fileUrl(last.id, 'report', 'report.html');
    panel.append(el('div', { className: 'card' },
      el('h3', {}, 'Report (step ' + last.id + ')'),
      summaryTable(last.summary),
      last.messages.length ? el('p', {}, last.messages.length + ' notes in the protocol') : null,
      el('div', { className: 'row' },
        el('a', { href: url, target: '_blank' }, 'Open in a new window (to print as PDF)')),
      el('iframe', { className: 'report', src: url, title: 'Test report' })));
  }
}

// ---------------------------------------------------------------------------------------------
// Results: every analysis result of the project per scanned object (ADR 0023)

/// Result types the GUI knows: a name, the summary keys that sum a result up, and where it is
/// shown (a view mode, with the rendering of the 3D view; reports open on their own). Plugin
/// results have none.
const RESULT_TYPES = {
  porosity: { label: 'Pores and loosened zones', view: 'slice',
    keys: ['region', 'pores', 'porosity', 'pore_volume_mm3', 'zones'] },
  materials: { label: 'Materials', view: 'slice', keys: ['materials'] },
  surface: { label: 'Surface', view: '3d',
    keys: ['surface_volume_mm3', 'compression_vs_raw'] },
  comparison: { label: 'Nominal-actual comparison', view: '3d',
    keys: ['deviation_rms_mm', 'within_tolerance_percent', 'tolerance_mm',
      'displacement_max_mm'] },
  wall_thickness: { label: 'Wall thickness', view: '3d',
    keys: ['region', 'thickness_min_mm', 'thickness_median_mm', 'below_min_percent', 'thin_regions',
      'measured_percent', 'thinner_than_nominal_percent', 'map_max_mm', 'map_thick_regions'] },
  report: { label: 'Test report', opens: true, keys: ['passed', 'evaluated_zones'] },
  inspection_regions: { label: 'Inspection regions', keys: ['zones'] },
  region: { label: 'Region', keys: ['name', 'description', 'removed'] },
  diconde: { label: 'DICONDE export', keys: ['slices', 'indications', 'series', 'path'] },
};

/// The summary figures that sum up a result of `type`: its key figures when it has them, else
/// all; lists of materials by their names.
function keyFigures(summary, type) {
  const all = summary ?? {};
  const keys = (RESULT_TYPES[type]?.keys ?? []).filter((key) => key in all);
  const figures = {};
  for (const key of keys.length ? keys : Object.keys(all)) {
    const value = all[key];
    figures[key] = key === 'materials' && Array.isArray(value)
      ? value.map((material) => material.name ?? material.id).join(', ') : value;
  }
  return figures;
}

/// The step of a result, or undefined when it is not among the active steps.
function resultStep(result) {
  return activeSteps().find((step) => step.id === result.step);
}

/// Whether the views show `result` for its dataset.
function isShown(result) {
  const source = result.source?.step;
  if (source === undefined) return false;
  if (result.type === 'porosity') return porosityOf(source) === result.step;
  if (result.type === 'materials') return materialsOf(source)?.id === result.step;
  if (result.type === 'surface') return surfaceOf(source) === result.step;
  if (result.type === 'comparison') return comparisonOf(source) === result.step;
  if (result.type === 'wall_thickness') return wallThicknessOf(source) === result.step;
  return false;
}

/// Shows a result in the slice or 3D view of its object.
async function showResult(result) {
  const info = RESULT_TYPES[result.type];
  if (!info?.view) return;
  state.shownResults = { ...state.shownResults, [result.type]: result.step };
  if (result.object && result.object !== state.status.active_object &&
      (state.status.objects ?? []).some((object) => object.id === result.object)) {
    await action(() => api('object_select', { id: result.object }));
  }
  state.viewMode = info.view;
  // The layer of the result on.
  const layers = layerState();
  if (result.type === 'porosity') Object.assign(layers, { pores: true, zones: true });
  if (result.type === 'materials') layers.colorBy = 'materials';
  if (result.type === 'comparison') layers.colorBy = 'deviation';
  if (result.type === 'wall_thickness') layers.colorBy = 'wall_thickness';
  if (result.type === 'surface') {
    layers.greyStyle = 3;
    if (['deviation', 'wall_thickness'].includes(layers.colorBy)) layers.colorBy = 'grey';
  }
  state.stage = 'workspace';
  render();
}

/// Pictures of a result output, as links to the full size.
function resultImages(result) {
  const container = el('div', { className: 'images' });
  api('list_files', { step: result.step, output: result.output }).then((listing) => {
    for (const file of listing.files.filter((entry) => entry.file.endsWith('.png'))) {
      const url = fileUrl(result.step, result.output, file.file);
      container.append(el('a', { href: url, target: '_blank' },
        el('img', { src: url, alt: file.file, title: file.file })));
    }
  }).catch(() => {});
  return container;
}

/// All figures of a result and its files to download, loaded when opened.
function resultDetails(result) {
  const files = el('ul', { className: 'result-files' });
  const details = el('details', {}, el('summary', {}, 'All figures and files'),
    summaryTable(result.summary), files);
  details.addEventListener('toggle', () => {
    if (!details.open || files.childElementCount) return;
    api('list_files', { step: result.step, output: result.output }).then((listing) => {
      files.replaceChildren(...listing.files.map((file) => el('li', {},
        el('a', { href: fileUrl(result.step, result.output, file.file), target: '_blank' },
          file.file), ' ', el('span', { className: 'hint' }, formatBytes(file.size_bytes)))));
    }).catch(showError);
  });
  return details;
}

/// Where a result comes from: the step, when, the dataset and the results it builds on.
function resultOrigin(result) {
  const step = resultStep(result);
  const parts = ['Step ' + result.step, step?.title ?? result.title, formatTime(result.finished)];
  if (result.source) parts.push('from the dataset of step ' + result.source.step);
  if (result.based_on.length) {
    parts.push('builds on step ' + result.based_on.map((ref) => ref.step).join(', '));
  }
  return parts.filter(Boolean).join(' · ');
}

function resultLabel(result) {
  return RESULT_TYPES[result.type]?.label ?? result.title;
}

function showButton(result) {
  const info = RESULT_TYPES[result.type];
  if (!info || (!info.opens && !info.view)) return null;
  if (info.opens) {
    return el('a', { href: fileUrl(result.step, result.output, 'report.html'), target: '_blank' },
      'Open the report');
  }
  return el('button', { disabled: state.busy, onclick: () => showResult(result),
    title: 'Show this result in the ' + (info.view === 'slice' ? 'slice' : '3D') + ' view' },
  'Show in the view');
}

function resultCard(result) {
  const shown = isShown(result);
  const card = el('div', { className: 'card result' + (state.focusResult === result.step
    ? ' focus' : '') },
  el('h3', {}, resultLabel(result),
    shown ? el('span', { className: 'badge', title: 'The slice and 3D views show this result' },
      'shown') : null),
  el('p', {}, resultOrigin(result)),
  summaryTable(keyFigures(result.summary, result.type)),
  resultImages(result),
  resultDetails(result),
  el('div', { className: 'row' }, showButton(result)));
  return card;
}

/// A replaced result in one line, to show or open it again.
function earlierResultRow(result) {
  const figures = Object.entries(keyFigures(result.summary, result.type)).slice(0, 2)
    .map(([key, value]) => (SUMMARY_LABELS[key] ?? key) + ' ' + formatValue(key, value));
  return el('tr', {},
    el('td', {}, resultLabel(result)),
    el('td', {}, 'Step ' + result.step),
    el('td', {}, formatTime(result.finished)),
    el('td', { className: 'hint' }, figures.join(' · ')),
    el('td', {}, showButton(result)));
}

/// All results of the project by object, the current ones and the earlier ones.
function renderHistory(panel) {
  const results = state.status.results ?? [];
  if (!results.length) {
    panel.append(el('p', { className: 'hint' }, 'No results yet. Every analysis, segmentation, ' +
      'surface, comparison and report appears here, sorted by the scanned object.'));
    return;
  }
  panel.append(el('p', { className: 'hint' }, 'All results of the project by object. The views ' +
    'show the current result of each kind unless you choose an earlier one here.'));
  const objects = state.status.objects ?? [];
  const groups = objects.map((object, index) => ({ object, index,
    results: results.filter((result) => result.object === object.id) }));
  const loose = results.filter((result) =>
    !objects.some((object) => object.id === result.object));
  if (loose.length) groups.push({ object: null, index: -1, results: loose });
  for (const group of groups.filter((candidate) => candidate.results.length)) {
    const current = group.results.filter((result) => result.current).reverse();
    const earlier = group.results.filter((result) => !result.current).reverse();
    const active = group.object && group.object.id === state.status.active_object;
    const section = el('section', { className: 'result-group' },
      el('h2', {}, group.object ? [objectSwatch(group.index, group.object.color), ' ', group.object.name] : 'Project',
        active ? el('span', { className: 'badge' }, 'active') : null,
        el('span', { className: 'hint' }, ' · ' + group.results.length +
          (group.results.length === 1 ? ' result' : ' results'))),
      current.map(resultCard));
    if (earlier.length) {
      section.append(el('details', { className: 'card' },
        el('summary', {}, 'Earlier results (' + earlier.length + ')'),
        el('table', { className: 'summary earlier' }, earlier.map(earlierResultRow))));
    }
    panel.append(section);
  }
  if (state.focusResult) {
    panel.querySelector('.result.focus')?.scrollIntoView({ block: 'nearest' });
    state.focusResult = null;
  }
}

// ---------------------------------------------------------------------------------------------
// Slice view

/// The step of `steps` (in order) the views show for results of `type`: the one chosen in the
/// results, else the latest.
function shownStep(type, steps) {
  const chosen = state.shownResults?.[type];
  return steps.find((step) => step.id === chosen) ?? steps[steps.length - 1] ?? null;
}

/// Porosity step computed from the dataset of `datasetStep` that the views show.
function porosityOf(datasetStep) {
  const steps = activeSteps().filter((step) => step.operation === 'porosity' &&
    step.inputs.dataset?.step === datasetStep);
  return shownStep('porosity', steps)?.id ?? null;
}

/// Material segmentation computed from the dataset of `datasetStep` that the views show.
function materialsOf(datasetStep) {
  const steps = activeSteps().filter((step) =>
    ['segment_materials', 'segment_model'].includes(step.operation) &&
    step.inputs.dataset?.step === datasetStep);
  return shownStep('materials', steps);
}

/// Material classes left out of the views, for the material segmentation step `step`; a new
/// segmentation shows all of them again.
function hiddenMaterials(step) {
  if (state.hiddenMaterials?.step !== step) state.hiddenMaterials = { step, ids: [] };
  return state.hiddenMaterials.ids;
}

/// Colours of the material classes of the segmentation step `materials` (material m at m - 1):
/// the ones chosen in the view, else those of the segmentation, else the default ones.
function materialColorsOf(materials) {
  const list = materials?.summary?.materials ?? [];
  const chosen = materials && state.materialColors?.step === materials.id
    ? state.materialColors.colors : [];
  return MATERIAL_COLORS.map((fallback, m) => chosen[m] ??
    list.find((material) => material.id === m + 1)?.color ?? fallback);
}

/// Legend of a material segmentation: per class its colour (to change for the views), name,
/// the grey value it starts at, its volume and share of all material, and a box that shows or
/// hides it in the slice and 3D views.
function materialLegend(materials, onChange, onColors, opacity = null) {
  const list = materials.summary?.materials ?? [];
  const total = list.reduce((sum, material) => sum + material.volume_mm3, 0);
  const hidden = hiddenMaterials(materials.id);
  const colors = materialColorsOf(materials);
  // A palette recolours every material at once.
  const palette = el('select', { title: 'Colour all materials from a palette' },
    el('option', { value: '' }, 'Palette …'),
    Object.entries(MATERIAL_PALETTES).map(([key, p]) => el('option', { value: key }, p.name)));
  palette.addEventListener('change', () => {
    const chosen = MATERIAL_PALETTES[palette.value];
    if (!chosen) return;
    const next = MATERIAL_COLORS.map((_, m) => [...chosen.colors[m % chosen.colors.length]]);
    state.materialColors = { step: materials.id, colors: next };
    onColors(next);
    scheduleViewSave();
    render();
  });
  const rows = list.map((material) => {
    const color = el('input', { type: 'color', className: 'swatch-input',
      value: hexColor(colors[material.id - 1]), title: 'Colour of this material in the views' });
    color.addEventListener('input', () => {
      const chosen = materialColorsOf(materials);
      chosen[material.id - 1] = parseHexColor(color.value);
      state.materialColors = { step: materials.id, colors: chosen };
      onColors(chosen);
      scheduleViewSave();
    });
    const show = el('input', { type: 'checkbox', checked: !hidden.includes(material.id),
      title: 'Show this material' });
    show.addEventListener('change', () => {
      const ids = hiddenMaterials(materials.id).filter((id) => id !== material.id);
      if (!show.checked) ids.push(material.id);
      state.hiddenMaterials.ids = ids.sort((a, b) => a - b);
      onChange(state.hiddenMaterials.ids);
    });
    let shown = null;
    if (opacity) {
      shown = el('input', { type: 'number', min: 0, max: 100, step: 5, className: 'opacity-input',
        value: Math.round(100 * (opacity.get()[material.id - 1] ?? 1)),
        title: 'Opacity of this material in the translucent 3D view, in percent' });
      shown.addEventListener('change', () => {
        const values = [...opacity.get()];
        values[material.id - 1] = Math.min(Math.max(Number(shown.value) / 100, 0), 1);
        opacity.set(values);
      });
    }
    return el('tr', {},
      el('td', {}, el('label', {}, show, color, ' ' + (material.name ?? material.id))),
      el('td', {}, material.from_grey_value === undefined ? '–'
        : formatNumber(Math.round(material.from_grey_value))),
      el('td', {}, formatNumber(material.volume_mm3)),
      el('td', {}, total > 0 ? (100 * material.volume_mm3 / total).toFixed(1) : '–'),
      opacity ? el('td', {}, shown) : null);
  });
  return [el('h3', {}, 'Materials (step ' + materials.id + ')'),
    el('div', { className: 'field-row' }, palette),
    el('table', { className: 'materials' }, el('thead', {}, el('tr', {}, el('th', {}, 'Material'),
      el('th', { title: 'Grey value the material starts at' }, 'from'), el('th', {}, 'mm³'), el('th', { title: 'Share of all material' }, '%'),
      opacity ? el('th', { title: 'Opacity in the translucent 3D view' }, 'Opacity %') : null)),
    el('tbody', {}, rows)),
    el('p', { className: 'hint' }, 'Untick a material to look past it, for instance the base ' +
      'material to see the inclusions in it. Materials are defined in the histogram of the 3D ' +
      'view.')];
}

/// dataset_info of a dataset step, asked once.
function datasetInfo(step) {
  state.datasetInfos ??= new Map();
  if (!state.datasetInfos.has(step)) {
    const request = api('dataset_info', { step });
    request.catch(() => state.datasetInfos.delete(step));
    state.datasetInfos.set(step, request);
  }
  return state.datasetInfos.get(step);
}

async function loadPores(step) {
  if (state.pores.step === step) return;
  state.pores = { step, list: [] };
  if (step === null) return;
  try {
    const file = await api('read_file', { step, output: 'porosity', file: 'porosity.json' });
    state.pores.list = JSON.parse(file.text).pores ?? [];
  } catch {
    state.pores.list = [];  // larger than read_file allows; the view still shows the overlay
  }
  if (state.stage === 'workspace') render();
}

// ---------------------------------------------------------------------------------------------
// Workspace (ADR 0040): objects and layers on the left, the view in the middle, the analyses and
// the history of results on the right.

/// How the grey values of the volume are drawn in 3D (VolumeViewer modes 0 to 3).
const GREY_STYLES = [[0, 'Solid surface'], [1, 'Translucent (transfer function)'],
  [2, 'Maximum intensity projection'], [3, 'Extracted surface mesh']];

/// What colours the part, one at a time: the step of the result it needs (null: not there).
const COLORINGS = [
  { key: 'grey', label: 'Grey value', step: () => undefined,
    title: 'The grey values alone' },
  { key: 'materials', label: 'Materials', step: (sources) => sources.materials?.id ?? null,
    title: 'Every material class of the segmentation in its colour' },
  { key: 'deviation', label: 'Nominal-actual deviation', step: (sources) => sources.comparison,
    title: 'The surface of the scan coloured by its deviation from the CAD model (3D view)' },
  { key: 'wall_thickness', label: 'Wall thickness', step: (sources) => sources.thickness,
    title: 'The surface coloured by its wall thickness (3D view); with a thickness map the ' +
      'material in the slice too' },
];

/// What the views show: how the grey values are drawn, what colours the part (null: the
/// materials when there is a segmentation) and which overlays are drawn on top.
function layerState() {
  if (!state.layers) state.layers = { greyStyle: 0, colorBy: null, pores: true, zones: true };
  return state.layers;
}

/// Layers of views saved before there were layers, from the settings of their 3D view.
function layersFromVolume(volume) {
  if (!volume) return null;
  const mode = volume.mode ?? 0;
  const colorBy = { 4: 'deviation', 5: 'wall_thickness' }[mode] ??
    (volume.materials === false ? 'grey' : null);
  return { greyStyle: mode <= 3 ? mode : 0, colorBy, pores: volume.pores ?? true,
    zones: volume.pores ?? true };
}

/// The results of the dataset of `datasetStep` that the views can show.
function layerSources(datasetStep) {
  return {
    porosity: porosityOf(datasetStep),
    materials: materialsOf(datasetStep),
    surface: surfaceOf(datasetStep),
    comparison: comparisonOf(datasetStep),
    thickness: wallThicknessOf(datasetStep),
    thicknessMap: thicknessMapOf(datasetStep),
  };
}

/// The colouring the views use: the chosen one while its result exists, else the materials of a
/// segmentation, else the grey values.
function activeColoring(sources) {
  const has = (key) => {
    const coloring = COLORINGS.find((candidate) => candidate.key === key);
    return coloring !== undefined && coloring.step(sources) !== null;
  };
  const chosen = layerState().colorBy;
  if (chosen && has(chosen)) return chosen;
  return sources.materials ? 'materials' : 'grey';
}

/// The mode of the 3D view for the layers: a coloured surface replaces the grey values.
function volumeModeOf(sources) {
  const coloring = activeColoring(sources);
  if (coloring === 'deviation') return 4;
  if (coloring === 'wall_thickness') return 5;
  const style = layerState().greyStyle;
  return style === 3 && sources.surface === null ? 0 : style;
}

function applyLayersToVolume(volume, sources) {
  const layers = layerState();
  volume.set({ mode: volumeModeOf(sources), pores: layers.pores, zones: layers.zones,
    materials: activeColoring(sources) === 'materials' });
}

function applyLayersToSlice(viewer, sources) {
  const layers = layerState();
  const coloring = activeColoring(sources);
  viewer.setShow({ pores: layers.pores, zones: layers.zones, materials: coloring === 'materials',
    thickness: coloring === 'wall_thickness' });
}

/// Shows the result of `operation` in the views: its layer on, in the view that suits it; a
/// result without a layer in the history.
function showResultOf(operation) {
  const layers = layerState();
  state.stage = 'workspace';
  if (operation === 'porosity') {
    // Pores lie inside the part: in 3D it is shown translucent, so that they show through.
    Object.assign(layers, { pores: true, zones: true, greyStyle: 1 });
    if (['deviation', 'wall_thickness'].includes(layers.colorBy)) layers.colorBy = null;
    state.layerSelected = 'pores';
    if (state.viewMode === 'scene') state.viewMode = '3d';
    const volume = state.volume;
    const pores = latestStepOf('porosity');
    if (volume?.volume && state.transfer && pores) {
      // The view loads the volume with its pores anew unless it has them already; that volume
      // starts with this preset.
      if (volume.volume.key.split('/')[1] === String(pores.id)) {
        Object.assign(state.transfer, { preset: 'durchsicht', colorMap: 'stahl',
          points: transferPreset('durchsicht', volume.threshold) });
      } else {
        state.transfer.next = { preset: 'durchsicht', after: volume.volume.key };
      }
    }
  } else if (['segment_materials', 'segment_model'].includes(operation)) {
    layers.colorBy = 'materials';
    state.layerSelected = 'materials';
  } else if (['compare_cad', 'compare_objects'].includes(operation)) {
    layers.colorBy = 'deviation';
    state.layerSelected = 'deviation';
    state.viewMode = '3d';
  } else if (operation === 'wall_thickness') {
    layers.colorBy = 'wall_thickness';
    state.layerSelected = 'wall_thickness';
    state.viewMode = '3d';
  } else if (operation === 'surface') {
    layers.greyStyle = 3;
    if (['deviation', 'wall_thickness'].includes(layers.colorBy)) layers.colorBy = 'grey';
    state.layerSelected = 'grey';
    state.viewMode = '3d';
  } else {
    const step = latestStepOf(operation);
    if (step) state.focusResult = step.id;
    state.sideTab = 'history';
  }
  render();
}

/// The pores of a porosity step as a table; a click shows the pore in the slice view.
function poreTable() {
  const rows = state.pores.list.slice(0, 500).map((pore) => el('tr', {
    onclick: () => {
      const size = Math.max(8, ...pore.bounds_max.map((max, a) => max - pore.bounds_min[a] + 1));
      if (state.viewMode === 'slice' && state.viewer?.info) {
        state.viewer.showVoxel(pore.center_voxels, size);
      } else {
        state.jumpTo = { voxel: pore.center_voxels, size };
        state.viewMode = 'slice';
        render();
      }
    },
    title: 'Show the pore in the slice view',
  }, el('td', {}, pore.id), el('td', {}, formatNumber(pore.equivalent_diameter_mm)),
  el('td', {}, formatNumber(pore.volume_mm3))));
  if (!rows.length) return el('p', { className: 'hint' }, 'No pores found.');
  return el('div', { className: 'pore-list' }, el('table', {},
    el('thead', {}, el('tr', {}, el('th', {}, 'No.'), el('th', {}, 'Ø mm'), el('th', {}, 'mm³'))),
    el('tbody', {}, rows)));
}

/// The tree of the shown scan: its grey values, the colouring, the overlays and other objects,
/// each with a switch; below it the properties of the selected entry, which the view filled in
/// `properties` (key -> elements).
function renderLayers(container, dataset, sources, properties) {
  const layers = layerState();
  const coloring = activeColoring(sources);
  const objects = state.status.objects ?? [];
  const object = objects.find((candidate) => candidate.id === dataset?.step.object);
  const select = (key) => {
    state.layerSelected = key;
    render();
  };
  // Each kind of layer has its icon, so the tree reads at a glance.
  const kinds = { grey: 'grey', 'grey-coloring': 'palette', materials: 'materials',
    deviation: 'deviation', wall_thickness: 'thickness', pores: 'pores', zones: 'zones',
    objects: 'shapes', 'new-region': 'edit' };
  const row = (key, control, label, step, title) => el('div', {
    className: 'layer' + (state.layerSelected === key ? ' selected' : ''), title,
    onclick: (event) => {
      if (!['INPUT', 'SELECT', 'BUTTON'].includes(event.target.tagName)) select(key);
    },
  }, control, el('span', { className: 'layer-icon',
    icon: key.startsWith('region:') ? 'region' : kinds[key] ?? 'palette' }),
  el('span', { className: 'layer-name' }, label),
  step !== null && step !== undefined
    ? el('span', { className: 'step-badge', title: 'Result of step ' + step }, '#' + step) : null);
  const changed = () => {
    scheduleViewSave();
    render();
  };

  container.append(el('h3', {}, 'Objects and layers'));
  if (!dataset) {
    container.append(el('p', { className: 'hint' }, 'The project holds no scan yet.'));
  } else {
    const style = GREY_STYLES.find(([mode]) => mode === volumeModeOf(sources))?.[1];
    container.append(
      el('div', { className: 'layer-object' }, 'Scan: ', el('b', {}, object?.name ?? 'dataset'),
        el('span', { className: 'hint' }, ' · step ' + dataset.step.id)),
      row('grey', null, 'Grey values',
        null, 'How the grey values are drawn: ' + (style ?? 'coloured surface')));
    const colorings = COLORINGS.filter((candidate) => candidate.step(sources) !== null);
    container.append(el('div', { className: 'layer-group' }, 'Colour by'),
      ...colorings.map((candidate) => {
        const radio = el('input', { type: 'radio', name: 'coloring',
          checked: candidate.key === coloring });
        // "Grey values" above is the entry of the grey values themselves.
        const key = candidate.key === 'grey' ? 'grey-coloring' : candidate.key;
        radio.addEventListener('change', () => {
          layers.colorBy = candidate.key;
          state.layerSelected = key;
          changed();
        });
        return row(key, radio, candidate.label, candidate.step(sources),
          candidate.title);
      }));
    const overlays = [];
    if (sources.porosity !== null) {
      for (const [key, label, title] of [['pores', 'Pores', 'Pores of the porosity analysis'],
        ['zones', 'Loosened zones', 'Zones of loosened microstructure of the porosity analysis']]) {
        const box = el('input', { type: 'checkbox', checked: layers[key] });
        box.addEventListener('change', () => {
          layers[key] = box.checked;
          changed();
        });
        overlays.push(row(key, box, label, sources.porosity, title));
      }
    }
    const regions = scanRegions(dataset);
    const hidden = layers.hiddenRegions ?? [];
    const regionRows = regions.map((region, i) => {
      const box = el('input', { type: 'checkbox', checked: !hidden.includes(region.name),
        title: 'Show this region in the views' });
      box.addEventListener('change', () => {
        layers.hiddenRegions = (layers.hiddenRegions ?? []).filter((n) => n !== region.name);
        if (!box.checked) layers.hiddenRegions.push(region.name);
        changed();
      });
      const swatch = el('span', { className: 'region-swatch' });
      swatch.style.background = regionColor(region, i);
      return row('region:' + region.name, box, el('span', {}, swatch, region.name), region.step,
        region.description);
    });
    const drawing = regionDraft(dataset);
    const regionGroup = [el('div', { className: 'layer-group' }, 'Regions'), ...regionRows,
      drawing ? row('new-region', null,
        (drawing.editing ? 'Editing ' : 'Drawing ') + drawing.name, null,
        'The region being drawn')
        : el('button', { className: 'link add-region', title: 'Draw a region in the slice view ' +
          'or take a CAD model, to run analyses in it only',
        icon: 'plus', onclick: () => startRegionDraft(dataset) }, 'New region')];
    for (const region of regions) {
      properties['region:' + region.name] = regionProperties(dataset, region);
    }
    if (drawing) properties['new-region'] = draftProperties(dataset);
    if (objects.some((candidate) => candidate.id !== dataset.step.object)) {
      overlays.push(row('objects', null, 'Other objects',
        null, 'Other volumes and CAD models of the project'));
    }
    if (overlays.length) container.append(el('div', { className: 'layer-group' }, 'Overlays'), ...overlays);
    container.append(...regionGroup);
    if (colorings.length === 1 && sources.porosity === null) {
      container.append(el('p', { className: 'hint' }, 'Run an analysis on the right: its result ' +
        'appears here as a layer to switch on and off.'));
    }
  }
  const selected = state.layerSelected ?? 'grey';
  const content = properties[selected];
  const names = { ...Object.fromEntries(COLORINGS.map((c) => [c.key, c.label])),
    grey: 'Grey values', pores: 'Pores', zones: 'Loosened zones', objects: 'Other objects',
    'new-region': 'New region' };
  if (selected.startsWith('region:')) names[selected] = 'Region ' + selected.slice(7);
  if (content?.length) {
    container.append(el('div', { className: 'layer-properties' },
      el('h4', {}, names[selected] ?? selected), ...content));
  }
}

// ---------------------------------------------------------------------------------------------
// Regions of a scan (ADR 0041): solids drawn in the slice view or taken from a CAD model, saved
// as define_region steps; analyses that take a region report only what lies in it.

/// The regions of the scan object of `dataset`.
function scanRegions(dataset) {
  return dataset ? regionsOf(activeSteps(), dataset.step.object) : [];
}

/// Analyses that take a `region` parameter.
function takesRegion(operation) {
  return Boolean(operation?.parameters?.properties?.region);
}

function regionColor(region, index) {
  return region.color ?? REGION_COLORS[index % REGION_COLORS.length];
}

/// The region being drawn, for the scan of `dataset`.
function regionDraft(dataset) {
  const draft = state.regionDraft;
  return draft && draft.object === (dataset?.step.object ?? '') ? draft : null;
}

function startRegionDraft(dataset, from = null) {
  const regions = scanRegions(dataset);
  let name = from?.name;
  for (let n = regions.length + 1; !name; n += 1) {
    if (!regions.some((region) => region.name === 'Region ' + n)) name = 'Region ' + n;
  }
  state.regionDraft = { object: dataset.step.object ?? '', name, editing: from?.name ?? null,
    color: from ? regionColor(from, regions.indexOf(from)) : REGION_COLORS[regions.length %
      REGION_COLORS.length], include: structuredClone(from?.include ?? []),
    exclude: structuredClone(from?.exclude ?? []), mode: 'include' };
  state.regionTool = from ? null : 'box';
  state.layerSelected = 'new-region';
  if (state.viewMode !== 'slice' && !from) state.viewMode = 'slice';
  render();
}

function stopRegionDraft() {
  state.regionDraft = null;
  state.regionTool = null;
  state.viewer?.setTool(null);
}

/// Triangles of the regions to show (mm), loaded from the steps where a region holds a mesh.
/// Calls `done` once missing meshes have arrived.
function regionTriangles(region, done) {
  const included = solidsTriangles(region.include);
  const excluded = solidsTriangles(region.exclude);
  if (included !== null && excluded !== null) return { included, excluded };
  if (!region.step) return { included: included ?? new Float32Array(0), excluded };
  state.regionMeshes ??= new Map();
  const cached = state.regionMeshes.get(region.step);
  if (cached?.included) return cached;
  if (!cached) {
    state.regionMeshes.set(region.step, {});
    const load = (file) => (file ? fetch(fileUrl(region.step, 'region', file))
      .then((response) => (response.ok ? response.arrayBuffer() : null))
      .then((buffer) => (buffer ? parseStl(buffer) : null)) : Promise.resolve(null));
    Promise.all([load(region.meshes.included), load(region.meshes.excluded)])
      .then(([inside, outside]) => {
        state.regionMeshes.set(region.step, { included: inside ?? new Float32Array(0),
          excluded: outside });
        done();
      }).catch(showError);
  }
  return { included: new Float32Array(0), excluded: null };
}

/// The regions the views draw: the shown saved ones and the one being drawn.
function shownRegions(dataset, done) {
  const layers = layerState();
  const regions = scanRegions(dataset);
  const shown = [];
  regions.forEach((region, i) => {
    if ((layers.hiddenRegions ?? []).includes(region.name)) return;
    if (regionDraft(dataset)?.editing === region.name) return;
    const triangles = regionTriangles(region, done);
    shown.push({ name: region.name, color: regionColor(region, i), triangles: triangles.included,
      excluded: triangles.excluded, key: region.step + ':' + region.name });
  });
  const draft = regionDraft(dataset);
  if (draft) {
    // Meshes of a draft (a CAD model) show through their objects' own cut lines.
    const plain = (solids) => solids.filter((solid) => solidTriangles(solid) !== null);
    shown.push({ name: draft.name, color: draft.color, draft: true,
      triangles: solidsTriangles(plain(draft.include)),
      excluded: solidsTriangles(plain(draft.exclude)), key: 'draft:' + JSON.stringify(draft) });
  }
  return shown;
}

/// The shown regions for the 3D view: triangles in level-0 voxel coordinates.
function regionsInVoxels(regions, pitch) {
  return regions.filter((region) => region.triangles?.length).map((region) => {
    const triangles = new Float32Array(region.triangles.length);
    for (let i = 0; i < triangles.length; i += 1) {
      triangles[i] = region.triangles[i] / pitch[i % 3];
    }
    return { key: region.key, color: parseHexColor(region.color), triangles };
  });
}

/// The properties of a saved region in the layer tree.
function regionProperties(dataset, region) {
  const solids = (list, label) => (list.length ? [el('div', { className: 'hint' }, label),
    el('ul', { className: 'solids' }, list.map((solid) => el('li', {},
      describeSolid(solid, objectName))))] : []);
  return [
    el('p', { className: 'hint' }, region.description),
    ...solids(region.include, 'Inside'), ...solids(region.exclude, 'Cut out'),
    el('div', { className: 'row' },
      el('button', { className: 'primary', title: 'Choose this region for the analyses on the ' +
        'right', onclick: () => {
        state.analysisRegion = region.name;
        state.sideTab = 'analyses';
        render();
      }, icon: 'analysis' }, 'Analyse here'),
      el('button', { icon: 'edit', onclick: () => startRegionDraft(dataset, region) }, 'Edit'),
      el('button', { icon: 'delete',
        title: 'Delete the region; a step of the protocol, so it can be undone',
        onclick: async () => {
          if (state.analysisRegion === region.name) state.analysisRegion = null;
          await runStep('define_region', { name: region.name, remove: true });
          state.layerSelected = 'grey';
          render();
        } }, 'Delete')),
    el('p', { className: 'hint' }, 'Step ' + region.step + '. Analyses in a region report only ' +
      'the pores and wall thickness in it; the regions follow their scan when it is moved.')];
}

/// The properties of the region being drawn: its name and colour, the tools that draw solids
/// in the slice view, a CAD model as a solid, the solids so far and Save.
function draftProperties(dataset) {
  const draft = regionDraft(dataset);
  if (!draft) return [];
  const name = el('input', { value: draft.name, spellcheck: false });
  name.addEventListener('input', () => { draft.name = name.value; });
  const color = el('input', { type: 'color', value: draft.color, title: 'Colour of the region' });
  color.addEventListener('input', () => {
    draft.color = color.value;
    render();
  });
  const tool = (kind, label, title) => el('button', {
    className: state.regionTool === kind ? 'on' : null, title, icon: kind,
    onclick: () => {
      state.regionTool = state.regionTool === kind ? null : kind;
      if (state.regionTool) state.viewMode = 'slice';
      render();
    },
  }, label);
  const mode = (key, label, title) => el('button', {
    className: draft.mode === key ? 'on' : null, title, icon: key === 'include' ? 'plus' : 'isolate',
    onclick: () => { draft.mode = key; render(); },
  }, label);
  const meshes = (state.status.objects ?? []).filter((object) => object.kind === 'mesh');
  const cad = el('select', { title: 'The inside of a CAD model or another mesh object' },
    el('option', { value: '' }, 'Inside a CAD model …'),
    meshes.map((object) => el('option', { value: object.id }, object.name)));
  cad.addEventListener('change', () => {
    if (!cad.value) return;
    draft[draft.mode].push({ mesh_object: cad.value });
    render();
  });
  const list = (solids, key, label) => (solids.length ? [el('div', { className: 'hint' }, label),
    el('ul', { className: 'solids' }, solids.map((solid, i) => el('li', {},
      describeSolid(solid, objectName), ' ', el('button', { className: 'link',
        title: 'Remove this solid', icon: 'delete',
        onclick: () => { draft[key].splice(i, 1); render(); } }))))] : []);
  const save = el('button', { className: 'primary', disabled: !draft.include.length &&
    !draft.exclude.length || state.busy, onclick: async () => {
    const params = { name: draft.name.trim(), color: draft.color, include: draft.include,
      exclude: draft.exclude };
    if (!params.name) {
      showError('The region needs a name');
      return;
    }
    const renamed = draft.editing && draft.editing !== params.name ? draft.editing : null;
    const step = await runStep('define_region', params);
    if (!step) return;
    if (renamed) await runStep('define_region', { name: renamed, remove: true });
    if (state.analysisRegion === renamed) state.analysisRegion = params.name;
    stopRegionDraft();
    state.layerSelected = 'region:' + params.name;
    render();
  }, icon: 'save' }, 'Save region');
  return [
    el('div', { className: 'form region-form' }, el('label', {}, 'Name', name),
      el('label', {}, 'Colour', color)),
    el('div', { className: 'group' }, mode('include', 'Add', 'Drawn solids add to the region'),
      mode('exclude', 'Cut out', 'Drawn solids are cut out of the region')),
    el('div', { className: 'group' },
      tool('box', 'Box', 'Drag a rectangle in the slice: a box through the whole scan along the ' +
        'slice normal'),
      tool('sphere', 'Sphere', 'Drag from the centre in the slice: a sphere'),
      tool('cylinder', 'Cylinder', 'Drag from the centre in the slice: a cylinder along the ' +
        'slice normal through the whole scan')),
    meshes.length ? cad : null,
    el('p', { className: 'hint' }, state.regionTool
      ? 'Drag in the slice view to draw. Other slices and axes show where the solid lies.'
      : 'Choose a tool, then drag in the slice view.'),
    ...list(draft.include, 'include', 'Inside'), ...list(draft.exclude, 'exclude', 'Cut out'),
    el('div', { className: 'row' }, save,
      el('button', { onclick: () => { stopRegionDraft(); state.layerSelected = 'grey'; render(); } },
        'Cancel'))];
}

/// The region an analysis runs in: a select of the whole part and the regions of the scan.
function regionChooser(dataset) {
  const regions = scanRegions(dataset);
  if (!regions.some((region) => region.name === state.analysisRegion)) state.analysisRegion = null;
  const select = el('select', { title: 'Where the analyses that take a region look' },
    el('option', { value: '' }, 'Whole part'),
    regions.map((region) => el('option', { value: region.name,
      selected: region.name === state.analysisRegion }, region.name)));
  select.addEventListener('change', () => {
    state.analysisRegion = select.value || null;
    render();
  });
  return el('div', { className: 'region-chooser' }, el('label', {}, 'Where', select),
    el('button', { title: 'Draw a new region in the slice view', icon: 'plus',
      onclick: () => startRegionDraft(dataset) }, 'Region'));
}

/// The `region` parameter of a run of an analysis that takes one, or nothing.
function analysisRegionParams(operation, dataset) {
  if (!takesRegion(operation) || !state.analysisRegion) return {};
  const region = scanRegions(dataset).find((candidate) => candidate.name === state.analysisRegion);
  return region ? { region: regionParameter(region) } : {};
}

// ---------------------------------------------------------------------------------------------
// Analyses in groups, in the order of the work

const ANALYSIS_GROUPS = [
  { key: 'prepare', icon: 'prepare', title: 'Prepare', hint: 'Writes a new dataset; later steps use it',
    ids: ['isolate_part', 'denoise', 'beam_hardening'] },
  { key: 'segment', icon: 'materials', title: 'Segment', hint: 'Materials and objects',
    ids: ['segment_materials', 'segment_model', 'measure_objects', 'material_meshes'] },
  { key: 'defects', icon: 'pores', title: 'Defects', hint: 'Pores, fibres, battery cells',
    ids: ['porosity', 'fibre_orientation', 'battery_inspection'] },
  { key: 'geometry', icon: 'surface', title: 'Geometry', hint: 'Surface, nominal-actual, wall thickness, dimensions',
    ids: ['surface', 'compare_cad', 'wall_thickness', 'measure_dimensions'] },
  { key: 'output', icon: 'export', title: 'Reports and export', hint: 'The test report has its own stage',
    ids: ['first_article_report', 'battery_report', 'export_diconde', 'inspection_regions'] },
];

/// The icon of each analysis on its card.
const OPERATION_ICONS = {
  isolate_part: 'isolate', denoise: 'denoise', beam_hardening: 'beam',
  segment_materials: 'materials', segment_model: 'model', measure_objects: 'list',
  material_meshes: 'shapes', porosity: 'pores', fibre_orientation: 'fibres',
  battery_inspection: 'battery', surface: 'surface', compare_cad: 'deviation',
  wall_thickness: 'thickness', measure_dimensions: 'target', first_article_report: 'checklist',
  battery_report: 'battery-full', export_diconde: 'export', inspection_regions: 'zones',
  report: 'report',
};

/// Operations that are no analysis of a scan: imports, objects (the Objects view) and the test
/// report (its stage).
const NOT_ANALYSES = ['open_dataset', 'import_raw', 'import_tiff', 'import_dicom',
  'import_gzipped_xml', 'add_mesh', 'move', 'align_points', 'align_surfaces', 'compare_objects',
  'report', 'battery_images', 'define_region'];

/// The settings shown on the card of an analysis; the others are under "More settings".
const MAIN_PARAMETERS = {
  isolate_part: ['keep_objects'],
  denoise: ['method', 'strength'],
  beam_hardening: ['method'],
  segment_materials: ['materials'],
  segment_model: ['model_path'],
  measure_objects: ['material_ids', 'min_voxels'],
  material_meshes: ['material_ids'],
  porosity: ['min_pore_voxels'],
  fibre_orientation: ['families', 'block_size'],
  battery_inspection: ['cell_type', 'axis'],
  surface: ['stl'],
  compare_cad: ['cad_path', 'tolerance_mm', 'alignment'],
  wall_thickness: ['method', 'min_wall_mm', 'max_wall_mm'],
  measure_dimensions: ['plan_path', 'cad_path'],
  first_article_report: ['order_path'],
  battery_report: ['order_path'],
  export_diconde: ['path'],
  inspection_regions: ['order_path'],
};

const INPUT_NAMES = { dataset: 'a scan', materials: 'a material segmentation',
  surface: 'a surface', comparison: 'a nominal-actual comparison', porosity: 'a porosity analysis',
  battery: 'a battery cell inspection', dimensional_measurement: 'a dimensional measurement' };

/// What an operation needs that the project does not have yet; a missing surface is made first.
function missingInputs(operation) {
  return operation.inputs.filter((input) => !input.optional && !latestOutput(input.type) &&
    !(input.type === 'surface' && latestOutput('dataset')))
    .map((input) => INPUT_NAMES[input.type] ?? input.type);
}

function firstSentence(text) {
  const end = text.search(/[.;:](\s|$)/);
  return end > 0 ? text.slice(0, end) : text;
}

/// The card of an analysis: title and what it does; opened, its main settings, the others
/// folded, Run and the latest result.
function analysisCard(operation, dataset) {
  const last = latestStepOf(operation.id);
  const card = el('details', { className: 'analysis', open: state.openAnalysis === operation.id },
    el('summary', { title: firstSentence(operation.description) },
      el('span', { className: 'analysis-icon', icon: OPERATION_ICONS[operation.id] ?? 'plugins' }),
      el('span', { className: 'analysis-title' }, operation.title),
      last ? el('span', { className: 'done-mark', title: 'Result of step ' + last.id,
        icon: 'check' }, '#' + last.id) : null));
  const fill = () => {
    if (card.childElementCount > 1) return;
    const main = MAIN_PARAMETERS[operation.id] ??
      (operation.parameters.required ?? []).filter((name) => name !== 'inputs');
    const initial = last ? last.params : {};
    const primary = buildForm(operation.parameters, initial, ['inputs', 'region'], main);
    const others = Object.keys(operation.parameters.properties ?? {})
      .filter((name) => !main.includes(name) && !['inputs', 'region'].includes(name));
    const secondary = buildForm(operation.parameters, initial, ['inputs', 'region', ...main]);
    const missing = missingInputs(operation);
    card.append(el('details', { className: 'about' }, el('summary', {}, 'What it does'),
      el('p', {}, operation.description)), primary.element);
    if (others.length) {
      card.append(el('details', { className: 'more' },
        el('summary', {}, 'More settings (' + others.length + ')'), secondary.element));
    }
    if (takesRegion(operation)) {
      card.append(el('p', { className: 'hint where' }, state.analysisRegion
        ? 'Runs in the region ' + state.analysisRegion + ' (chosen above).'
        : 'Runs on the whole part; choose a region above to look into part of it.'));
    }
    const run = runButton('Run', operation.id, () => ({ ...primary.values(),
      ...secondary.values(), ...analysisRegionParams(operation, dataset) }),
    () => showResultOf(operation.id));
    if (missing.length) {
      run.disabled = true;
      run.title = 'Needs ' + missing.join(' and ') + ' first';
    }
    card.append(el('div', { className: 'row' }, run,
      missing.length ? el('span', { className: 'hint' }, 'Needs ' + missing.join(' and ')) : null));
    if (last) {
      card.append(el('div', { className: 'analysis-result' },
        el('div', { className: 'hint' }, 'Result of step ' + last.id),
        figureTiles(keyFigures(last.summary, Object.values(last.outputs)[0]?.type)),
        el('div', { className: 'row' },
          el('button', { icon: 'show', onclick: () => showResultOf(operation.id) }, 'Show'),
          el('button', { icon: 'info', onclick: () => {
            state.focusResult = last.id;
            state.sideTab = 'history';
            render();
          } }, 'Details'))));
    }
  };
  card.addEventListener('toggle', () => {
    if (card.open) {
      state.openAnalysis = operation.id;
      fill();
    } else if (state.openAnalysis === operation.id) {
      state.openAnalysis = null;
    }
  });
  if (card.open) fill();
  return card;
}

function renderAnalyses(container, dataset) {
  if (!dataset) {
    container.append(el('p', { className: 'hint' }, 'Open a scan first.'));
    return;
  }
  container.append(regionChooser(dataset));
  const suggested = nextStepsRow(dataset);
  suggested.classList.add('open-only');
  fillNextSteps(suggested);
  container.append(suggested);
  const known = ANALYSIS_GROUPS.flatMap((group) => group.ids).concat(NOT_ANALYSES);
  const groups = [...ANALYSIS_GROUPS, { key: 'plugins', title: 'Plugins', hint: 'Operations of plugins',
    icon: 'plugins',
    ids: state.operations.filter((operation) => !known.includes(operation.id))
      .map((operation) => operation.id) }];
  state.openGroups ??= {};
  for (const group of groups) {
    const operations = group.ids.map(operationInfo).filter(Boolean);
    if (!operations.length) continue;
    const used = operations.some((operation) => latestStepOf(operation.id));
    const open = state.openGroups[group.key] ?? (used ||
      operations.some((operation) => operation.id === state.openAnalysis));
    const section = el('details', { className: 'analysis-group', open },
      el('summary', { title: group.hint }, el('span', { className: 'group-icon', icon: group.icon }),
        el('b', {}, group.title), el('span', { className: 'count' }, String(operations.length))),
      operations.map((operation) => analysisCard(operation, dataset)));
    section.addEventListener('toggle', () => { state.openGroups[group.key] = section.open; });
    container.append(section);
  }
}

/// The right column: the analyses, or all results of the project.
function renderSidePanel(container, dataset) {
  const tab = state.sideTab ?? 'analyses';
  container.append(el('div', { className: 'group side-tabs' },
    [['analyses', 'Analyses', 'analysis'], ['history', 'History', 'history']]
      .map(([key, label, name]) => el('button', {
      className: tab === key ? 'on' : null, icon: name,
      title: key === 'history' ? 'All results of the project, also earlier ones' : null,
      onclick: () => { state.sideTab = key; render(); },
    }, label))));
  if (tab === 'history') renderHistory(container);
  else renderAnalyses(container, dataset);
}

/// The workspace: objects and layers, the view, the analyses.
function renderWorkspace(panel) {
  const dataset = shownDataset();
  const objects = state.status.objects ?? [];
  const left = el('div', { className: 'ws-left' });
  const center = el('div', { className: 'ws-center' });
  const right = el('div', { className: 'ws-right' });
  panel.append(el('div', { className: 'workspace' }, left, center, right));
  const sources = dataset ? layerSources(dataset.step.id) : null;
  if (sources) loadPores(sources.porosity);
  const ctx = { sources, properties: {} };
  if (!dataset && !objects.length) {
    center.append(el('p', { className: 'hint' }, 'Choose a dataset first.'),
      nextButton('To the dataset', 'dataset'));
  } else {
    renderViewArea(center, dataset, ctx);
  }
  if (dataset) renderLayers(left, dataset, sources, ctx.properties);
  else left.append(el('h3', {}, 'Objects and layers'), el('p', { className: 'hint' }, 'No scan yet.'));
  renderSidePanel(right, dataset);
}

/// The middle column: slice, 3D or all objects, with the saved views.
function renderViewArea(panel, dataset, ctx) {
  const objects = state.status.objects ?? [];
  // Without a dataset (only meshes), the objects are all there is to see.
  if (!dataset) state.viewMode = 'scene';
  if (!objects.length && state.viewMode === 'scene') state.viewMode = 'slice';
  const available = [['slice', 'Slice'], ['3d', '3D'], ['scene', 'Objects']].filter(([mode]) =>
    (mode === 'scene' ? objects.length > 0 : Boolean(dataset)));
  const modes = el('div', { className: 'group view-modes' },
    available.map(([mode, label]) => el('button', {
      className: state.viewMode === mode ? 'on' : null,
      icon: { slice: 'slice', '3d': '3d', scene: 'objects' }[mode],
      title: mode === 'scene' ? 'All objects where they lie, for aligning and comparing them' : null,
      onclick: () => { state.viewMode = mode; render(); },
    }, label)));
  renderViewBar(panel, modes);
  if (state.viewMode === 'scene') {
    renderSceneView(panel);
    ctx.properties.objects = [el('p', { className: 'hint' }, 'The Objects view shows all ' +
      'volumes and CAD models where they lie.')];
  } else if (state.viewMode === '3d') {
    renderVolumeView(panel, dataset, ctx);
  } else {
    renderSliceView(panel, dataset, ctx);
  }
}

function renderSliceView(panel, dataset, ctx) {
  const viewer = state.viewer ?? (state.viewer = new SliceViewer());
  const { sources } = ctx;
  const porosity = sources.porosity;
  const materials = sources.materials;
  const thicknessMap = sources.thicknessMap;
  const coloring = activeColoring(sources);
  loadPores(porosity);
  applyLayersToSlice(viewer, sources);
  const canvas = el('canvas', { tabindex: 0 });
  const status = el('div', { className: 'viewer-status' });
  const slider = el('input', { type: 'range', min: 0, step: 1 });
  const sliceNumber = el('input', { type: 'number', min: 0, step: 1 });
  const low = el('input', { type: 'number', step: 'any', title: 'Grey value for black' });
  const high = el('input', { type: 'number', step: 'any', title: 'Grey value for white' });
  const axisButtons = [0, 1, 2].map((axis) => el('button', {
    onclick: () => viewer.setAxis(axis),
    title: 'Slice normal to the ' + SLICE_AXIS_NAMES[axis] + ' axis',
  }, SLICE_AXIS_NAMES[axis].toUpperCase()));
  slider.addEventListener('input', () => viewer.setSlice(Number(slider.value)));
  sliceNumber.addEventListener('change', () => viewer.setSlice(Number(sliceNumber.value)));
  const applyWindow = () => {
    if (low.value !== '' && high.value !== '' && Number(high.value) > Number(low.value)) {
      viewer.setWindow(Number(low.value), Number(high.value));
    }
  };
  low.addEventListener('change', applyWindow);
  high.addEventListener('change', applyWindow);

  viewer.onChange = () => {
    const axis = viewer.axis;
    axisButtons.forEach((button, a) => button.classList.toggle('on', a === axis));
    slider.max = viewer.info.dims[axis] - 1;
    sliceNumber.max = slider.max;
    slider.value = viewer.index[axis];
    if (document.activeElement !== sliceNumber) sliceNumber.value = viewer.index[axis];
    if (viewer.window) {
      if (document.activeElement !== low) low.value = Math.round(viewer.window[0]);
      if (document.activeElement !== high) high.value = Math.round(viewer.window[1]);
    }
    const [u, v] = IN_PLANE_NAMES[axis];
    const level = viewer.level();
    let text = SLICE_AXIS_NAMES[axis] + ' = ' + viewer.index[axis] + ' · ' + u + ' to the right, ' +
      v + ' down · level ' + level + ' (' +
      formatVoxelSize(viewer.info.levels[level].voxel_size_mm) + ' mm)';
    if (viewer.hover) {
      const value = viewer.hover.value;
      text += ' · voxel ' + viewer.hover.voxel.join(', ') +
        (value === undefined ? '' : ' · grey value ' + formatNumber(Math.round(value)));
    }
    status.textContent = text;
    scheduleViewSave();
  };

  const tools = el('div', { className: 'viewer-tools' },
    el('div', { className: 'group' }, axisButtons),
    slider, sliceNumber,
    el('button', { onclick: () => { viewer.fit(); viewer.requestDraw(); viewer.onChange(); } },
      'Fit'));

  ctx.properties.grey = [el('div', { className: 'viewer-tools' }, 'Window', low, high,
    el('button', { onclick: () => viewer.autoWindow() }, 'Auto')),
  el('p', { className: 'hint' }, 'Grey values from black to white. The 3D view draws them as ' +
    'a solid surface, translucent or as maximum intensity projection.')];
  if (porosity !== null) {
    ctx.properties.pores = [poreTable()];
    ctx.properties.zones = [el('p', { className: 'hint' }, 'Blocks whose grey value is lowered ' +
      'by voids too small to resolve, in yellow.')];
  }
  ctx.properties.objects = sliceObjectsCard(viewer, dataset).slice(1);
  if (materials !== null) {
    viewer.setHiddenMaterials(hiddenMaterials(materials.id));
    viewer.setMaterialColors(materialColorsOf(materials));
    ctx.properties.materials = materialLegend(materials, (ids) => viewer.setHiddenMaterials(ids),
      (colors) => viewer.setMaterialColors(colors)).slice(1);
  }
  const legend = el('div', { className: 'view-legend' });
  if (coloring === 'wall_thickness' && thicknessMap !== null) {
    legend.append(thicknessLegend(thicknessMap.params.min_wall_mm ?? 0,
      thicknessMap.params.max_wall_mm ?? 0, thicknessMap.summary?.map_range_mm ?? 10,
      'the material'));
  } else if (coloring === 'wall_thickness' || coloring === 'deviation') {
    legend.append(el('p', { className: 'hint' }, coloring === 'deviation'
      ? 'The nominal-actual deviation lies on the surface: the 3D view shows it.'
      : 'The wall thickness lies on the surface: the 3D view shows it. With the setting ' +
        '"volume map" the analysis also colours the material in the slice.'));
  }
  panel.append(tools, el('div', { className: 'viewer' }, canvas), legend, status);

  viewer.setRegions(shownRegions(dataset, () => render()));
  const draft = regionDraft(dataset);
  viewer.setTool(draft && state.regionTool ? { kind: state.regionTool, onDrawn: (solid) => {
    draft[draft.mode].push(solid);
    render();
  } } : null);
  state.capture = () => viewer.capture();
  api('dataset_info', { step: dataset.step.id }).then((info) => {
    viewer.setDataset(info, dataset.step.id, porosity, materials?.id ?? null,
      thicknessMap?.id ?? null);
    viewer.attach(canvas);
    if (state.restore?.slice) {
      viewer.setState(state.restore.slice);
      state.restore.slice = null;
      applyLayersToSlice(viewer, sources);
    }
    if (state.jumpTo) {
      viewer.showVoxel(state.jumpTo.voxel, state.jumpTo.size);
      state.jumpTo = null;
    }
    viewer.onChange();
  }).catch(showError);
}

const IN_PLANE_NAMES = [['y', 'z'], ['x', 'z'], ['x', 'y']];

/// The other objects of the project for the slice view: volumes blended over the slice or as a
/// checkerboard, meshes as the lines where they cut it. Sets the viewer's layers and returns the
/// controls, or nothing when the shown volume is the only object.
function sliceObjectsCard(viewer, dataset) {
  const objects = state.status.objects ?? [];
  const base = objects.find((object) => object.id === dataset.step.object);
  const others = objects.map((object, index) => ({ object, index }))
    .filter(({ object }) => object !== base);
  // A view being restored brings its settings of the objects along.
  const restore = state.restore?.slice;
  if (restore?.layers && restore.step === dataset.step.id) {
    viewer.layerSettings = structuredClone(restore.layers);
  }
  // Tiles and lines are fetched again when either object moves or the other's data changes.
  viewer.setLayers(others.map(({ object, index }) => ({
    id: object.id, kind: object.kind, color: objectColor(index, object.color), source: object.source.step,
    version: [objectKey(object), object.pose.join(','), base?.pose.join(',') ?? ''].join('|'),
  })));
  if (!others.length) return [];
  const rows = others.map(({ object, index }) => {
    const layer = viewer.layers.find((candidate) => candidate.id === object.id);
    const show = el('input', { type: 'checkbox', checked: layer.show,
      title: object.kind === 'volume' ? 'Show this volume in the slice'
        : 'Show where this mesh cuts the slice' });
    const item = el('div', { className: 'slice-object' },
      el('label', { title: object.id }, show, objectSwatch(index, object.color), ' ', object.name,
        object.kind === 'mesh' ? el('span', { className: 'hint' }, ' cut lines') : null));
    if (object.kind === 'volume') {
      const mode = el('select', { title: 'Blended in its colour, or every other square' },
        el('option', { value: 'blend', selected: layer.mode === 'blend' }, 'Blend'),
        el('option', { value: 'checker', selected: layer.mode === 'checker' }, 'Checkerboard'));
      const opacity = el('input', { type: 'range', min: 0, max: 1, step: 0.05,
        value: layer.opacity, disabled: layer.mode !== 'blend', title: 'Opacity' });
      mode.addEventListener('change', () => {
        opacity.disabled = mode.value !== 'blend';
        viewer.setLayer(object.id, { mode: mode.value });
      });
      opacity.addEventListener('input', () =>
        viewer.setLayer(object.id, { opacity: Number(opacity.value) }));
      item.append(el('div', {}, mode, opacity));
    }
    show.addEventListener('change', () => viewer.setLayer(object.id, { show: show.checked }));
    return item;
  });
  return [el('h3', {}, 'Other objects'), ...rows];
}

/// Names offered for materials defined in the histogram.
const MATERIAL_NAMES = ['Air inclusion', 'Aluminium', 'Steel', 'Cast iron', 'Titanium', 'Copper',
  'Magnesium', 'Plastic', 'Fibre', 'Ceramic', 'Glass', 'Inclusion', 'Coating'];


/// Surface step computed from the dataset of `datasetStep` that the views show.
function surfaceOf(datasetStep) {
  const steps = activeSteps().filter((step) => step.operation === 'surface' &&
    step.inputs.dataset?.step === datasetStep);
  return shownStep('surface', steps)?.id ?? null;
}

/// Nominal-actual comparison of a surface of the dataset of `datasetStep` that the views show.
function comparisonOf(datasetStep) {
  const surfaces = activeSteps().filter((step) => step.operation === 'surface' &&
    step.inputs.dataset?.step === datasetStep).map((step) => step.id);
  const steps = activeSteps().filter((step) =>
    ['compare_cad', 'compare_objects'].includes(step.operation) &&
    surfaces.includes(step.inputs.surface?.step));
  return shownStep('comparison', steps)?.id ?? null;
}

/// Wall thickness step computed from the dataset of `datasetStep` that the views show.
function wallThicknessOf(datasetStep) {
  const steps = activeSteps().filter((step) => step.operation === 'wall_thickness' &&
    step.inputs.dataset?.step === datasetStep);
  return shownStep('wall_thickness', steps)?.id ?? null;
}

/// The wall thickness step of the dataset of `datasetStep` that the views show, when it has a
/// thickness map: the slice view colours the material by it.
function thicknessMapOf(datasetStep) {
  const id = wallThicknessOf(datasetStep);
  const step = activeSteps().find((candidate) => candidate.id === id);
  return step?.params?.volume_map ? step : null;
}

/// Colour bar of the deviation colours (deviationColor in compare.cpp) with its limits.
function deviationLegend(tolerance, range,
  note = 'Blue: material missing · green: within tolerance · red: excess material') {
  const edge = (100 * (range - tolerance) / (2 * range)).toFixed(2);
  const inner = (100 - edge).toFixed(2);
  const bar = el('div', { className: 'legend-bar' });
  bar.style.background = 'linear-gradient(to right, rgb(40,60,215) 0%, rgb(40,205,240) ' +
    edge + '%, rgb(60,190,90) ' + edge + '%, rgb(60,190,90) ' + inner +
    '%, rgb(240,225,40) ' + inner + '%, rgb(215,30,30) 100%)';
  const mm = (value) => (value > 0 ? '+' : '') + formatNumber(value) + ' mm';
  const label = (text, percent, align) => {
    const span = el('span', {}, text);
    span.style.left = percent + '%';
    span.style.transform = 'translateX(' + align + '%)';
    return span;
  };
  return el('div', { className: 'deviation-legend' }, bar,
    el('div', { className: 'legend-labels' },
      label('≤ ' + mm(-range), 0, 0), label(mm(-tolerance), edge, -50), label('0', 50, -50),
      label(mm(tolerance), inner, -50), label('≥ ' + mm(range), 100, -100)),
    el('div', { className: 'legend-note' }, note));
}

/// Colour bar of the displacement of a non-rigid registration: green up to the tolerance, then
/// yellow to red.
function displacementLegend(tolerance, range) {
  const edge = (100 * Math.min(tolerance / range, 1)).toFixed(2);
  const bar = el('div', { className: 'legend-bar' });
  bar.style.background = 'linear-gradient(to right, rgb(60,190,90) 0%, rgb(60,190,90) ' + edge +
    '%, rgb(240,225,40) ' + edge + '%, rgb(215,30,30) 100%)';
  const label = (text, percent, align) => {
    const span = el('span', {}, text);
    span.style.left = percent + '%';
    span.style.transform = 'translateX(' + align + '%)';
    return span;
  };
  return el('div', { className: 'deviation-legend' }, bar,
    el('div', { className: 'legend-labels' }, label('0', 0, 0),
      label(formatNumber(tolerance) + ' mm', edge, -50),
      label('≥ ' + formatNumber(range) + ' mm', 100, -100)),
    el('div', { className: 'legend-note' },
      'How far the non-rigid registration moved the surface to fit the nominal geometry'));
}

/// Colour bar of the wall thickness colours (thicknessColor in thickness.cpp) from 0 to `range`,
/// with the limits of the analysis.
function thicknessLegend(minWall, maxWall, range, measured = null) {
  const at = (mm) => (100 * Math.min(mm / range, 1)).toFixed(2);
  const stops = [[215, 40, 40], [240, 220, 50], [60, 190, 90], [40, 190, 230], [40, 70, 210]];
  const parts = stops.map((c, i) => 'rgb(' + c.join(',') + ') ' + (25 * i) + '%');
  const bar = el('div', { className: 'legend-bar' });
  const layers = [];
  if (minWall > 0) {
    layers.push('linear-gradient(to right, rgb(200,40,170) 0%, rgb(200,40,170) ' + at(minWall) +
      '%, transparent ' + at(minWall) + '%)');
  }
  if (maxWall > 0 && maxWall < range) {
    layers.push('linear-gradient(to right, transparent ' + at(maxWall) + '%, rgb(110,50,170) ' +
      at(maxWall) + '%)');
  }
  layers.push('linear-gradient(to right, ' + parts.join(', ') + ')');
  bar.style.background = layers.join(', ');
  const label = (text, percent, align) => {
    const span = el('span', {}, text);
    span.style.left = percent + '%';
    span.style.transform = 'translateX(' + align + '%)';
    return span;
  };
  // Labels of the limits replace the ends of the scale they would cover.
  const nearEnd = maxWall > 0 && maxWall < range && at(maxWall) > 80;
  const labels = [];
  if (!(minWall > 0 && at(minWall) < 15)) labels.push(label('0', 0, 0));
  if (!nearEnd) labels.push(label('≥ ' + formatNumber(range) + ' mm', 100, -100));
  if (minWall > 0) labels.push(label(formatNumber(minWall) + ' mm', at(minWall), -50));
  if (maxWall > 0 && maxWall < range) {
    labels.push(label(formatNumber(maxWall) + ' mm', at(maxWall), nearEnd ? -100 : -50));
  }
  return el('div', { className: 'deviation-legend' }, bar,
    el('div', { className: 'legend-labels' }, labels),
    el('div', { className: 'legend-note' },
      'Thin red to thick blue' + (minWall > 0 ? ' · magenta: thinner than the minimum' : '') +
      (maxWall > 0 ? ' · violet: thicker than the maximum' : '') +
      (measured ? ' · on ' + measured : ' · grey: not measured')));
}

// ---------------------------------------------------------------------------------------------
// Lighting, looks and video of the 3D view (ADR 0043)

/// A labelled slider of the properties column: its value from get(), each move to set(value).
function sliderRow(label, title, { min, max, step }, get, set) {
  const output = el('span', { className: 'slider-value' });
  const input = el('input', { type: 'range', min, max, step, value: get(), title });
  const show = () => { output.textContent = formatNumber(Number(Number(input.value).toFixed(2))); };
  input.addEventListener('input', () => {
    show();
    set(Number(input.value));
  });
  show();
  return el('label', { className: 'slider-row', title }, el('span', {}, label), input, output);
}

/// Lighting of the 3D view (lighting.js): a preset and, opened, every part of it. A change of a
/// part makes the lighting the user's own.
function lightingControls(volume, update) {
  const lighting = () => normalizeLighting(volume.lighting);
  const preset = el('select', { title: 'Lighting' },
    Object.entries(LIGHTING_PRESETS).map(([key, p]) => el('option', {
      value: key, selected: key === lighting().preset, title: p.description }, p.name)),
    el('option', { value: 'eigen', selected: !(lighting().preset in LIGHTING_PRESETS) }, 'Own'));
  const details = el('details', { className: 'lighting-details' },
    el('summary', {}, 'Light, shadows and highlights'));
  const fill = () => {
    const change = (key) => (value) => {
      update({ lighting: { ...lighting(), [key]: value, preset: 'eigen' } });
      preset.value = 'eigen';
    };
    const follow = el('input', { type: 'checkbox', checked: lighting().follow });
    follow.addEventListener('change', () => change('follow')(follow.checked));
    const row = (label, title, key, range) => sliderRow(label, title, range,
      () => lighting()[key], change(key));
    details.replaceChildren(details.firstChild,
      el('label', { className: 'group', title: 'The light turns with the camera; else it stays ' +
        'in the scene and the part turns under it' }, follow, 'Light follows the camera'),
      row('Direction', 'Azimuth of the key light in degrees', 'azimuth',
        { min: -180, max: 180, step: 1 }),
      row('Height', 'Elevation of the key light in degrees', 'elevation',
        { min: -90, max: 90, step: 1 }),
      row('Key light', 'Strength of the key light', 'intensity', { min: 0, max: 2, step: 0.01 }),
      row('Fill light', 'A weaker light from the other side', 'fill',
        { min: 0, max: 1, step: 0.01 }),
      row('Ambient', 'Light from everywhere', 'ambient', { min: 0, max: 1, step: 0.01 }),
      row('Rim light', 'Light on the outline, where the surface turns away', 'rim',
        { min: 0, max: 1, step: 0.01 }),
      row('Highlights', 'Strength of the specular highlights', 'specular',
        { min: 0, max: 1, step: 0.01 }),
      row('Gloss', 'Size of the highlights: higher is smaller and glossier', 'shininess',
        { min: 1, max: 120, step: 1 }),
      row('Shadows', 'How dark the shadows of the key light are', 'shadows',
        { min: 0, max: 1, step: 0.01 }),
      row('Occlusion', 'How much creases, corners and pores darken', 'occlusion',
        { min: 0, max: 1, step: 0.01 }));
  };
  preset.addEventListener('change', () => {
    if (!(preset.value in LIGHTING_PRESETS)) return;
    update({ lighting: lightingPreset(preset.value) });
    fill();
  });
  fill();
  return [el('div', { className: 'field-row' }, icon('sun'), preset), details];
}

/// The video of the 3D view in one click (video.js), with its motion, length, frame rate and
/// size remembered per browser. `keyframes` gives the saved views of the volume for the flight.
function videoControls(volume, keyframes, baseName) {
  const saved = (() => {
    try {
      return JSON.parse(localStorage.getItem('voxelsieve.video') ?? '{}') ?? {};
    } catch {
      return {};
    }
  })();
  const settings = { motion: 'turntable', seconds: 8, fps: 30, size: 'fullhd', ...saved };
  const remember = () => {
    try {
      localStorage.setItem('voxelsieve.video', JSON.stringify(settings));
    } catch {
      // Private window: the settings hold for this page only.
    }
  };
  const motion = el('select', { title: 'Camera motion of the video' },
    Object.entries(VIDEO_MOTIONS).map(([key, m]) => el('option', {
      value: key, selected: key === settings.motion, title: m.description }, m.name)));
  const seconds = el('input', { type: 'number', min: 2, max: 120, step: 1, value: settings.seconds,
    title: 'Length in seconds' });
  const fps = el('select', { title: 'Frames per second' },
    [24, 30, 60].map((n) => el('option', { value: n, selected: n === settings.fps }, n + ' fps')));
  const size = el('select', { title: 'Size of the video in pixels' },
    Object.entries(VIDEO_SIZES).map(([key, s]) => el('option', {
      value: key, selected: key === settings.size }, s.name)));
  for (const input of [motion, seconds, fps, size]) {
    input.addEventListener('change', () => {
      Object.assign(settings, { motion: motion.value, size: size.value,
        seconds: Math.min(Math.max(Number(seconds.value) || 8, 2), 120), fps: Number(fps.value) });
      remember();
    });
  }
  const button = el('button', { icon: 'video', title: 'Record a video of the view with the ' +
    'motion set under "Video settings" and download it' }, 'Record video');
  const progress = el('div', { className: 'video-progress', hidden: true });
  let controller = null;
  button.addEventListener('click', async () => {
    if (controller) {
      controller.abort();
      return;
    }
    const frames = settings.motion === 'views' ? keyframes() : [];
    if (settings.motion === 'views' && frames.length < 2) {
      showError(new Error('The flight through saved views needs at least two saved views of ' +
        'this volume; save them with "Save view"'));
      return;
    }
    controller = new AbortController();
    let result = '';
    button.replaceChildren(icon('stop'), 'Stop');
    button.classList.add('on');
    progress.hidden = false;
    try {
      const video = await recordVideo(volume, { ...settings, keyframes: frames,
        signal: controller.signal,
        onProgress: ({ phase, done }) => {
          progress.textContent = { prepare: 'Loading the detail along the motion … ',
            render: 'Drawing the frames … ', encode: 'Encoding … ' }[phase] +
            Math.round(100 * done) + ' %';
          progress.style.setProperty('--done', (100 * done).toFixed(1) + '%');
        } });
      const url = URL.createObjectURL(video.blob);
      downloadDataUrl(url, fileName(baseName() + '-' + settings.motion).replace(/\.png$/,
        '.' + video.extension));
      setTimeout(() => URL.revokeObjectURL(url), 60000);
      result = 'Video of ' + video.seconds.toFixed(1) + ' s (' + video.frames + ' frames, ' +
        video.extension.toUpperCase() + ', ' + formatNumber(Math.round(video.blob.size / 1024)) +
        ' kB) downloaded';
    } catch (error) {
      showError(error);
    } finally {
      controller = null;
      button.replaceChildren(icon('video'), 'Record video');
      button.classList.remove('on');
      progress.textContent = result;
      progress.style.setProperty('--done', '100%');
      progress.hidden = !result;
      if (result) setTimeout(() => { progress.hidden = true; }, 8000);
    }
  });
  const options = el('details', { className: 'view-settings' }, el('summary', {}, 'Video settings'),
    el('div', { className: 'viewer-tools' }, motion, el('label', { className: 'group' }, seconds,
      's'), fps, size));
  return { controls: [button, options], progress };
}

/// Colour maps as options of a select, in their groups.
function colorMapOptions(selected) {
  const groups = new Map();
  for (const [key, map] of Object.entries(COLOR_MAPS)) {
    if (!groups.has(map.group)) groups.set(map.group, []);
    groups.get(map.group).push(el('option', { value: key, selected: key === selected }, map.name));
  }
  return [...groups].map(([group, options]) => el('optgroup', { label: group }, options));
}

function renderVolumeView(panel, dataset, ctx) {
  const volume = state.volume ?? (state.volume = new VolumeViewer());
  // The transfer function outlives re-rendering; it is reset when another volume is loaded.
  const transfer = state.transfer ??
    (state.transfer = { key: null, preset: null, colorMap: 'viridis', points: null });
  const { sources } = ctx;
  const porosity = sources.porosity;
  const materials = sources.materials;
  const surfaceStep = sources.surface;
  const comparisonStep = sources.comparison;
  const thicknessStep = sources.thickness;
  const layers = layerState();
  applyLayersToVolume(volume, sources);
  // One canvas for the view's life, so re-rendering keeps its WebGL context and textures.
  const canvas = state.volumeCanvas ?? (state.volumeCanvas = el('canvas'));
  const histogram = el('canvas', { className: 'transfer', tabindex: 0,
    title: 'Click sets a point, drag moves it, double click or Delete removes it' });
  const editor = new TransferEditor(histogram);
  const status = el('div', { className: 'viewer-status' }, 'Loading overview …');
  const hint = el('div', { className: 'viewer-status' });
  const legend = el('div', { hidden: true });
  const suggestionRow = el('div', { className: 'suggestions' });
  const looksRow = el('div', { className: 'suggestions looks' });
  // Every change of the picture is also kept in the project.
  const update = (settings) => {
    volume.set(settings);
    scheduleViewSave();
  };
  volume.onInteract = scheduleViewSave;
  state.capture = () => volume.capture();

  const colorInput = (title, get, set) => {
    const input = el('input', { type: 'color', title, value: hexColor(get()) });
    input.addEventListener('input', () => set(parseHexColor(input.value)));
    return input;
  };
  // How the grey values are drawn: a property of the grey values in the layer tree.
  const style = el('select', { title: 'How the grey values are drawn' },
    GREY_STYLES.filter(([i]) => i !== 3 || surfaceStep !== null).map(([i, name]) => el('option',
      { value: i, selected: layers.greyStyle === i }, name)));
  style.addEventListener('change', () => {
    layers.greyStyle = Number(style.value);
    scheduleViewSave();
    render();
  });
  const shading = el('input', { type: 'checkbox', checked: volume.shading });
  shading.addEventListener('change', () => update({ shading: shading.checked }));
  const surface = colorInput('Surface colour', () => volume.surfaceColor,
    (rgb) => update({ surfaceColor: rgb }));
  const cut = el('input', { type: 'range', min: 0, max: 1, step: 0.005, value: volume.cut,
    title: 'Cut along x' });
  cut.addEventListener('input', () => update({ cut: Number(cut.value) }));
  volume.hiddenMaterials = materials === null ? [] : [...hiddenMaterials(materials.id)];
  volume.materialColors = materialColorsOf(materials);
  if (materials !== null) {
    ctx.properties.materials = materialLegend(materials,
      (ids) => update({ hiddenMaterials: [...ids] }),
      (colors) => update({ materialColors: colors }),
      { get: () => volume.materialOpacity, set: (values) => update({ materialOpacity: values }) })
      .slice(1);
  }
  const poreColor = colorInput('Pore colour', () => volume.poreColor,
    (rgb) => update({ poreColor: rgb }));
  const zoneColor = colorInput('Colour of the loosened zones', () => volume.zoneColor,
    (rgb) => update({ zoneColor: rgb }));

  // Background: a style with two colours (top or centre, bottom or edge) that can be changed.
  const backgroundStyle = el('select', { title: 'Background' },
    Object.entries(BACKGROUNDS).map(([key, background]) => el('option', {
      value: key, selected: key === volume.background.preset }, background.name)),
    el('option', { value: 'eigen', selected: !(volume.background.preset in BACKGROUNDS) },
      'Own colours'));
  const backgroundColors = [0, 1].map((i) => colorInput(
    i === 0 ? 'Background top or centre' : 'Background bottom or edge',
    () => volume.background.colors[i],
    (rgb) => {
      const colors = [...volume.background.colors];
      colors[i] = rgb;
      const style = volume.background.style === 0 ? 1 : volume.background.style;
      update({ background: { preset: 'eigen', name: 'Own colours', style, colors } });
      backgroundStyle.value = 'eigen';
    }));
  backgroundStyle.addEventListener('change', () => {
    const preset = BACKGROUNDS[backgroundStyle.value];
    if (!preset) return;
    update({ background: { preset: backgroundStyle.value, ...preset } });
    backgroundColors.forEach((input, i) => { input.value = hexColor(preset.colors[i]); });
  });

  const preset = el('select', { title: 'Transfer function preset' },
    el('option', { value: '' }, 'Preset …'),
    Object.entries(TRANSFER_PRESETS).map(([key, name]) => el('option', { value: key }, name)));
  const colorMap = el('select', { title: 'Colour map of the grey values' },
    colorMapOptions(transfer.colorMap));
  const colorSwatch = el('span', { className: 'colormap-swatch', title: 'The colour map' });
  const reverse = el('input', { type: 'checkbox', checked: Boolean(transfer.reverse) });
  const colorSource = el('select', { title: 'Where the colours come from: every grey value from ' +
    'the colour map, spread between the two markers under the histogram, or the colours of the ' +
    'points' },
  el('option', { value: 'map', selected: transfer.colorSource === 'map' }, 'Colours from the map'),
  el('option', { value: 'points', selected: transfer.colorSource !== 'map' },
    'Colours of the points'));
  const fitRange = el('button', { title: 'Spread the colour map over the grey values that are ' +
    'seen again' }, 'Fit');
  // The own gradient: three colours, low to high.
  const customStops = () => transfer.customStops ?? COLOR_MAPS.eigen.stops;
  const customColors = [0, 1, 2].map((i) => {
    const input = el('input', { type: 'color', title: ['Low', 'Middle', 'High'][i] +
      ' colour of the own gradient', value: hexColor(customStops()[i]?.slice(1) ?? [0, 0, 0]) });
    input.addEventListener('input', () => {
      const stops = customStops().map((stop) => [...stop]);
      stops[i] = [[0, 0.5, 1][i], ...parseHexColor(input.value)];
      transfer.customStops = stops;
      colorsChanged();
    });
    return input;
  });
  const customRow = el('span', { className: 'group' }, customColors);
  const pointColor = el('input', { type: 'color', title: 'Colour of the selected point' });
  const pointOpacity = el('input', { type: 'number', min: 0, max: 100, step: 1,
    title: 'Opacity of the selected point in percent' });
  const removePoint = el('button', { onclick: () => editor.removeSelected(),
    title: 'Delete the selected point' }, 'Delete');
  const curveTools = el('div', { className: 'group' }, preset, colorMap, colorSwatch, customRow,
    el('label', { className: 'group', title: 'Run the colour map the other way' }, reverse,
      'Reverse'), colorSource, fitRange,
    el('span', { className: 'sep' }), 'Point', pointColor, pointOpacity, '%', removePoint);
  const surfaceTools = el('label', { className: 'group' }, 'Colour', surface);

  // Materials defined in the histogram for the material segmentation (ADR 0013): where each
  // starts in grey values, its name and colour, and its opacity in the 3D preview.
  if (state.materialDraft?.step !== dataset.step.id) {
    state.materialDraft = { step: dataset.step.id, editing: false, preview: false, materials: null };
  }
  const draft = state.materialDraft;
  const toX = (grey) => {
    const w = volume.volume?.window;
    return Math.min(Math.max(w ? (grey - w[0]) / (w[1] - w[0]) : grey / 255, 0), 1);
  };
  const editMaterials = el('button', { className: draft.editing ? 'on' : null,
    title: 'Define materials in the histogram for the material segmentation' }, 'Materials');
  const materialCount = el('select', { title: 'Number of materials for the automatic split' },
    [1, 2, 3, 4, 5, 6, 7, 8].map((n) => el('option', { value: n }, n)));
  const preview = el('input', { type: 'checkbox', checked: draft.preview });
  const segmentButton = el('button', { className: 'primary',
    title: 'Run the material segmentation with these materials' }, 'Segment');
  const materialTools = el('div', { className: 'group' },
    el('button', { onclick: () => autoMaterials(Number(materialCount.value)),
      title: 'Split the grey values above the first boundary by multi-level Otsu' }, 'Auto split'),
    materialCount,
    el('button', { onclick: () => addMaterial(), title: 'Split the selected material in two' },
      '+ Material'),
    el('button', { onclick: () => snapAll(),
      title: 'Move every boundary into the nearest valley of the histogram' }, 'Snap to valleys'),
    el('label', { className: 'group', title: 'Show the grey values in the colours of their ' +
      'material in 3D, before segmenting' }, preview, 'Preview in 3D'),
    segmentButton);
  const materialRows = el('div', { className: 'material-editor' });
  // The histogram belongs to the grey values; a coloured surface of a result replaces them.
  const transferEditor = el('div', { className: 'transfer-editor' },
    el('div', { className: 'viewer-tools' }, el('b', {}, 'Histogram'), editMaterials, curveTools,
      surfaceTools, materialTools),
    histogram, hint, materialRows);

  /// The draft's materials in the editor, at the grey values of the loaded volume.
  const syncEditor = () => {
    editor.materials = (draft.materials ?? []).map((m) => ({ ...m, color: [...m.color],
      x: toX(m.from) }));
    if (editor.selectedMaterial >= editor.materials.length) editor.selectedMaterial = -1;
  };
  /// Back from the editor into the draft, in grey values.
  const syncDraft = () => {
    draft.materials = editor.materials.map((m) => ({ name: m.name, color: [...m.color],
      opacity: m.opacity, from: Math.round(valueAt(m.x)) }));
  };
  const materialsChanged = () => {
    syncDraft();
    applyTransfer();
    renderMaterialRows();
    scheduleViewSave();
  };
  const autoMaterials = (count) => {
    const air = editor.materials[0]?.x ?? editor.threshold;
    editor.materials = suggestMaterials(volume.histogram, air, count, editor.materials);
    editor.selectedMaterial = -1;
    editor.changed();
  };
  const addMaterial = () => {
    const list = editor.materials;
    if (list.length >= 8) return;
    const i = editor.selectedMaterial >= 0 ? editor.selectedMaterial : list.length - 1;
    const low = list[i]?.x ?? editor.threshold;
    const high = list[i + 1]?.x ?? 1;
    if (high - low < 2 / 255) return;
    const used = list.map((m) => hexColor(m.color));
    const color = MATERIAL_PALETTE.find((c) => !used.includes(hexColor(c))) ?? MATERIAL_PALETTE[0];
    list.splice(i + 1, 0, { name: 'Material ' + (list.length + 1), color: [...color],
      x: snapToValley(volume.histogram, (low + high) / 2, Math.floor((high - low) * 255 / 3)),
      opacity: 0.6 });
    list.sort((a, b) => a.x - b.x);
    editor.selectedMaterial = i + 1;
    editor.changed();
  };
  const snapAll = () => {
    editor.materials.forEach((m, i) => editor.moveBoundary(i, snapToValley(volume.histogram, m.x)));
    editor.changed();
  };
  /// One row per material: colour, name, grey value it starts at, opacity in the preview, its
  /// share of the material voxels of the overview, and a button that removes it.
  const renderMaterialRows = () => {
    const list = editor.materials;
    const counts = materialCounts(volume.histogram, list).slice(1);
    const total = counts.reduce((a, b) => a + b, 0);
    materialRows.replaceChildren(el('table', { className: 'materials' },
      el('thead', {}, el('tr', {}, el('th', {}, ''), el('th', {}, 'Name'),
        el('th', {}, 'from grey value'), el('th', { title: 'Opacity in the 3D preview' }, 'Opacity %'),
        el('th', { title: 'Share of the material voxels in the overview' }, '%'), el('th', {}, ''))),
      el('tbody', {}, list.map((material, i) => {
        const color = el('input', { type: 'color', value: hexColor(material.color),
          title: 'Colour of the material' });
        color.addEventListener('input', () => {
          material.color = parseHexColor(color.value);
          editor.draw();
          syncDraft();
          applyTransfer();
          scheduleViewSave();
        });
        const name = el('input', { type: 'text', value: material.name, list: 'material-names',
          spellcheck: 'false' });
        name.addEventListener('change', () => {
          material.name = name.value.trim() || 'Material ' + (i + 1);
          editor.draw();
          materialsChanged();
        });
        const from = el('input', { type: 'number', step: 'any', value: Math.round(valueAt(material.x)),
          title: i === 0 ? 'Air threshold: below it is air' : 'Grey value the material starts at' });
        from.addEventListener('change', () => {
          editor.moveBoundary(i, toX(Number(from.value)));
          editor.changed();
        });
        const opacity = el('input', { type: 'number', min: 0, max: 100, step: 1,
          value: Math.round(material.opacity * 100) });
        opacity.addEventListener('change', () => {
          material.opacity = Math.min(Math.max(Number(opacity.value) / 100, 0), 1);
          materialsChanged();
        });
        const remove = el('button', { className: 'icon', title: 'Remove this material',
          disabled: list.length <= 1, onclick: () => {
            editor.selectedMaterial = i;
            editor.removeSelected();
          } }, '×');
        return el('tr', { className: i === editor.selectedMaterial ? 'selected' : null,
          onclick: (event) => {
            if (editor.selectedMaterial === i || event.target.tagName === 'BUTTON') return;
            editor.selectedMaterial = i;
            editor.draw();
            editor.onSelect();
          } },
        el('td', {}, color), el('td', {}, name), el('td', {}, from), el('td', {}, opacity),
        el('td', {}, total > 0 ? (100 * counts[i] / total).toFixed(1) : '–'), el('td', {}, remove));
      }))),
    el('datalist', { id: 'material-names' }, MATERIAL_NAMES.map((n) => el('option', { value: n }))),
    el('p', { className: 'hint' }, 'Drag a boundary in the histogram, double click puts it into ' +
      'the nearest valley. The first boundary is the air threshold. "Segment" runs the material ' +
      'segmentation with these materials; the shares are estimated from the overview.'));
  };
  /// The transfer function: the materials' colours in the preview, else the curve.
  const curveTable = () => transferTable({ ...transfer, points: editor.points });
  editor.tableOf = curveTable;
  const applyTransfer = () => {
    volume.setTransfer(draft.editing && draft.preview && editor.materials.length
      ? materialTable(editor.materials) : curveTable());
  };
  /// The colour controls and the editor's colour range as the transfer function says.
  const syncColors = () => {
    const map = transfer.colorSource === 'map';
    editor.colorRange = map ? (transfer.colorRange ?? visibleRange(editor.points)) : null;
    colorSwatch.style.background = colorMapGradient(transfer.colorMap,
      { reverse: Boolean(transfer.reverse), custom: transfer.customStops ?? null });
    customRow.hidden = transfer.colorMap !== 'eigen';
    fitRange.hidden = !map;
    colorMap.value = transfer.colorMap;
    colorSource.value = map ? 'map' : 'points';
    reverse.checked = Boolean(transfer.reverse);
  };
  /// The colours changed: on the points too when they carry them.
  const colorsChanged = () => {
    if (transfer.colorSource !== 'map') {
      const selected = editor.selected;
      editor.setPoints(applyColorMap(editor.points, transfer.colorMap,
        { reverse: Boolean(transfer.reverse), custom: transfer.customStops ?? null }));
      editor.selected = selected;
    }
    volume.look = null;
    syncColors();
    editor.draw();
    editor.onSelect();
    applyTransfer();
    scheduleViewSave();
  };
  editor.onRange = (range) => {
    transfer.colorRange = range;
    applyTransfer();
    scheduleViewSave();
  };
  const setEditing = (editing) => {
    draft.editing = editing;
    editMaterials.classList.toggle('on', editing);
    if (!editing) setPreview(false);
    applyMode();
    applyTransfer();
    scheduleViewSave();
  };
  const setPreview = (on) => {
    draft.preview = on;
    preview.checked = on;
    if (on && (volume.mode !== 1 || volume.materials)) {
      // The preview is a transfer function, and the colours of a segmentation would cover it.
      layers.greyStyle = 1;
      if (activeColoring(sources) !== 'grey') layers.colorBy = 'grey';
      scheduleViewSave();
      render();
      return;
    }
    applyTransfer();
  };
  editMaterials.addEventListener('click', () => setEditing(!draft.editing));
  preview.addEventListener('change', () => {
    setPreview(preview.checked);
    scheduleViewSave();
  });
  segmentButton.addEventListener('click', async () => {
    const list = draft.materials ?? [];
    if (!list.length) return;
    const step = await runStep('segment_materials', {
      materials: list.length,
      air_threshold: list[0].from,
      material_thresholds: list.slice(1).map((m) => m.from),
      material_names: list.map((m) => m.name),
      material_colors: list.map((m) => hexColor(m.color)),
    });
    if (step?.status !== 'done') return;
    // The result in its colours, as a lit surface.
    draft.editing = false;
    draft.preview = false;
    Object.assign(layers, { colorBy: 'materials', greyStyle: 0 });
    state.layerSelected = 'materials';
    scheduleViewSave();
    render();
  });

  preset.addEventListener('change', () => {
    if (preset.value) {
      transfer.preset = preset.value;
      transfer.colorMap = { dichte: 'viridis', rand: 'kupfer', glas: 'eis', innen: 'inferno' }[
        preset.value] ?? 'stahl';
      transfer.colorRange = null;
      editor.setPoints(transferPreset(preset.value, volume.threshold));
      syncColors();
    }
    preset.value = '';
  });
  colorMap.addEventListener('change', () => {
    transfer.colorMap = colorMap.value;
    // A colour map is seen whole when it colours every grey value.
    transfer.colorSource = 'map';
    colorsChanged();
  });
  reverse.addEventListener('change', () => {
    transfer.reverse = reverse.checked;
    colorsChanged();
  });
  colorSource.addEventListener('change', () => {
    transfer.colorSource = colorSource.value;
    colorsChanged();
  });
  fitRange.addEventListener('click', () => {
    transfer.colorRange = null;
    colorsChanged();
  });
  pointColor.addEventListener('input', () => {
    if (editor.selected < 0) return;
    editor.points[editor.selected].color = parseHexColor(pointColor.value);
    // An own colour of a point holds only when the points carry the colours.
    if (transfer.colorSource === 'map') {
      transfer.colorSource = 'points';
      syncColors();
    }
    editor.changed();
  });
  pointOpacity.addEventListener('change', () => {
    if (editor.selected < 0) return;
    editor.points[editor.selected].a = Math.min(Math.max(Number(pointOpacity.value) / 100, 0), 1);
    editor.changed();
  });

  editor.onChange = () => {
    if (editor.mode === 'materials') {
      materialsChanged();
      return;
    }
    transfer.points = editor.points;
    if (transfer.colorSource === 'map' && !transfer.colorRange) {
      editor.colorRange = visibleRange(editor.points);
    }
    volume.set({ threshold: editor.threshold });
    applyTransfer();
    scheduleViewSave();
  };
  const valueAt = (x) => {
    const w = volume.volume?.window;
    return w ? w[0] + x * (w[1] - w[0]) : x * 255;
  };
  const label = (x) => formatNumber(Math.round(valueAt(x)));
  editor.label = label;
  editor.onSelect = () => {
    if (editor.mode === 'materials') {
      materialRows.querySelectorAll('tbody tr').forEach((row, i) =>
        row.classList.toggle('selected', i === editor.selectedMaterial));
    }
    const point = editor.points[editor.selected];
    pointColor.disabled = !point;
    pointOpacity.disabled = !point;
    removePoint.disabled = !point || editor.points.length <= 2;
    if (point) {
      pointColor.value = hexColor(point.color);
      if (document.activeElement !== pointOpacity) pointOpacity.value = Math.round(point.a * 100);
    }
    if (editor.mode === 'materials' && editor.hover === null) {
      hint.textContent = 'Materials: drag a boundary, click selects a material, double click ' +
        'on a boundary snaps it into the valley';
    } else if (editor.hover !== null) {
      const bin = Math.min(Math.round(editor.hover * 255), 255);
      const material = editor.mode === 'materials'
        ? editor.materials[materialAt(editor.materials, editor.hover)] : undefined;
      hint.textContent = 'Grey value ' + label(editor.hover) + ' · ' +
        formatNumber(volume.histogram[bin]) + ' voxels' + (editor.mode !== 'materials' ? ''
        : ' · ' + (material ? material.name : 'air'));
    } else {
      hint.textContent = editor.mode === 'threshold'
        ? 'Air/material threshold: ' + label(editor.threshold) + ' · drag in the histogram'
        : 'Curve: opacity per grey value · click sets a point, drag moves it, ' +
          'double click removes it';
    }
  };
  const applyMode = () => {
    const surfaceMode = volume.mode === 0 || volume.mode >= 3;
    if (volume.mode !== 1 && draft.preview) setPreview(false);
    editor.setMode(draft.editing ? 'materials' : surfaceMode ? 'threshold' : 'curve');
    curveTools.hidden = draft.editing || surfaceMode;
    surfaceTools.hidden = draft.editing || !surfaceMode;
    materialTools.hidden = !draft.editing;
    materialRows.hidden = !draft.editing;
    legend.hidden = volume.mode !== 4 && volume.mode !== 5;
    transferEditor.hidden = volume.mode >= 4;
    suggestionRow.classList.toggle('off', volume.mode >= 4);
    if (volume.mode === 3) showSurface();
    if (volume.mode === 4) showDeviation();
    if (volume.mode === 5) showThickness();
  };
  /// Loads the mesh of the latest surface step of this dataset and says what is shown.
  const showSurface = () => {
    status.textContent = 'Loading surface of step ' + surfaceStep + ' …';
    volume.loadSurface(surfaceStep).then((mesh) => {
      if (volume.mode !== 3) return;
      status.textContent = 'Extracted surface of step ' + surfaceStep + ' · ' +
        formatNumber(mesh.triangles) + ' triangles · drag rotates, mouse wheel zooms';
    }).catch((error) => {
      status.textContent = error.message;
    });
  };
  /// Loads the compared surface of the latest nominal-actual comparison of this dataset, coloured
  /// by its deviation or by the displacement of its non-rigid registration.
  const showDeviation = (displacement = false) => {
    status.textContent = 'Loading nominal-actual comparison of step ' + comparisonStep + ' …';
    volume.loadDeviation(comparisonStep, displacement).then((mesh) => {
      if (volume.mode !== 4) return;
      const toggle = mesh.hasDisplacement ? el('label', {},
        el('input', { type: 'checkbox', checked: mesh.displacement,
          onchange: (event) => showDeviation(event.target.checked) }),
        ' Show how far the non-rigid registration moved the surface') : [];
      legend.replaceChildren(mesh.displacement ? displacementLegend(mesh.tolerance, mesh.range)
        : deviationLegend(mesh.tolerance, mesh.range), toggle);
      status.textContent = (mesh.displacement ? 'Non-rigid displacement' :
        mesh.hasDisplacement ? 'Nominal-actual deviation after non-rigid registration' :
          'Nominal-actual deviation') + ' of step ' + comparisonStep + ' · ' +
        formatNumber(mesh.triangles) + ' triangles · drag rotates, mouse wheel zooms';
    }).catch((error) => {
      status.textContent = error.message;
    });
  };
  /// Loads the measured surface of the latest wall thickness step of this dataset, coloured by
  /// the thickness or by its deviation from the nominal thickness of the CAD model.
  const showThickness = (nominal = false) => {
    status.textContent = 'Loading wall thickness of step ' + thicknessStep + ' …';
    volume.loadWallThickness(thicknessStep, nominal).then((mesh) => {
      if (volume.mode !== 5) return;
      const toggle = mesh.hasNominal ? el('label', {},
        el('input', { type: 'checkbox', checked: mesh.nominal,
          onchange: (event) => showThickness(event.target.checked) }),
        ' Show the deviation from the nominal thickness of the CAD model') : [];
      legend.replaceChildren(mesh.nominal ? deviationLegend(mesh.tolerance, mesh.range,
        'Blue: thinner than nominal · green: within tolerance · red: thicker · grey: no value')
        : thicknessLegend(mesh.minWall, mesh.maxWall, mesh.range), toggle);
      status.textContent = (mesh.nominal ? 'Wall thickness against nominal' : 'Wall thickness') +
        ' of step ' + thicknessStep + ' · ' +
        formatNumber(mesh.triangles) + ' triangles · drag rotates, mouse wheel zooms';
    }).catch((error) => {
      status.textContent = error.message;
    });
  };

  /// Uses a suggested rendering: transfer function mode with its curve and lighting.
  const useSuggestion = (suggestion) => {
    Object.assign(transfer, { preset: null, colorMap: suggestion.colorMap, colorSource: 'points',
      reverse: false, colorRange: null });
    transfer.points = suggestion.points.map((p) => ({ ...p, color: [...p.color] }));
    layers.greyStyle = 1;
    if (activeColoring(sources) !== 'materials') layers.colorBy = 'grey';
    volume.set({ shading: suggestion.shading });
    scheduleViewSave();
    render();
  };
  /// A look (lighting.js): curve, colours, lighting and background in one click.
  const useLook = (name) => {
    const look = lookSettings(name, volume.threshold);
    Object.assign(transfer, look.transfer);
    layers.greyStyle = look.volume.mode;
    if (activeColoring(sources) !== 'materials') layers.colorBy = 'grey';
    volume.set({ ...look.volume,
      background: { preset: look.background, ...BACKGROUNDS[look.background] } });
    scheduleViewSave();
    render();
  };
  /// The settings of a look for a small picture of it.
  const lookPreview = (name) => {
    const look = lookSettings(name, volume.threshold);
    return [{ ...look.volume, background: { preset: look.background,
      ...BACKGROUNDS[look.background] } }, transferTable(look.transfer)];
  };
  const renderLooks = () => {
    const images = state.lookImages?.key === volume.volume?.key ? state.lookImages.images : {};
    looksRow.replaceChildren(el('b', { title: 'Curve, colours, light and background in one ' +
      'click' }, 'Looks'),
    ...Object.entries(LOOKS).map(([name, look]) => el('button', {
      className: 'suggestion' + (volume.look === name ? ' on' : ''), 'data-look': name,
      onclick: () => useLook(name), title: look.description,
    }, images[name] ? el('img', { src: images[name], alt: '' }) : null, el('b', {}, look.name))));
  };
  state.renderLooks = renderLooks;
  const renderSuggestions = () => {
    const suggestions = state.suggestions;
    if (!suggestions?.items.length) {
      suggestionRow.hidden = true;
      return;
    }
    suggestionRow.hidden = false;
    suggestionRow.replaceChildren(el('b', {}, 'Suggestions'),
      ...suggestions.items.map((suggestion, i) => el('button', {
        className: 'suggestion', onclick: () => useSuggestion(suggestion),
        title: suggestion.description + ' (grey values ' + label(suggestion.range[0]) + ' to ' +
          label(suggestion.range[1]) + ')',
      }, suggestions.images[i] ? el('img', { src: suggestions.images[i], alt: '' }) : null,
      el('span', {}, el('b', {}, suggestion.name), el('br'), suggestion.description))));
  };

  volume.onChange = () => {
    const v = volume.volume;
    if (!v) return;
    if (state.suggestions?.key !== v.key) {
      state.suggestions = { key: v.key, items: suggestTransfers(volume.histogram), images: [] };
    }
    if (transfer.key !== v.key || !transfer.points) {
      // A new volume starts with the first suggestion, or a preset when there is none.
      // A preset asked for the next volume, such as the one with the pores of an analysis.
      const asked = transfer.next && transfer.next.after !== v.key;
      const first = asked ? null : state.suggestions.items[0];
      if (asked) transfer.next = null;
      transfer.key = v.key;
      transfer.preset = first ? null : 'durchsicht';
      transfer.colorMap = first ? first.colorMap : 'stahl';
      transfer.points = first ? first.points : transferPreset('durchsicht', volume.threshold);
      Object.assign(transfer, { colorSource: 'points', reverse: false, colorRange: null });
    }
    editor.threshold = volume.threshold;
    editor.points = transfer.points;
    syncColors();
    if (!draft.materials) {
      // The materials of the latest segmentation, else a split of the histogram.
      const segmented = (materials?.summary?.materials ?? [])
        .filter((m) => m.from_grey_value !== undefined);
      const colors = materialColorsOf(materials);
      draft.materials = segmented.length ? segmented.map((m) => ({ name: m.name ?? 'Material ' +
        m.id, color: colors[m.id - 1], from: Math.round(m.from_grey_value), opacity: 0.6 }))
        : null;
      if (draft.materials) {
        syncEditor();
        const counts = materialCounts(volume.histogram, editor.materials).slice(1);
        if (counts.length > 1) draft.materials[counts.indexOf(Math.max(...counts))].opacity = 0.1;
      } else {
        editor.materials = suggestMaterials(volume.histogram, volume.threshold, 2);
        syncDraft();
      }
    }
    syncEditor();
    materialCount.value = String(editor.materials.length);
    editor.setHistogram(volume.histogram);
    renderMaterialRows();
    applyTransfer();
    editor.onSelect();
    renderSuggestions();
    renderLooks();
    showVolumeStatus();
  };
  /// Says which level is shown, and which finer one near the camera.
  const showVolumeStatus = () => {
    const v = volume.volume;
    if (!v || (volume.mode >= 3 && volume.surface)) return;  // the mesh status stays
    const size = (level) => {
      const pitch = v.voxelSize.map((s) => s * 2 ** (level - v.level));
      return formatVoxelSize(pitch.every((s) => s === pitch[0]) ? pitch[0] : pitch);
    };
    let text = 'Level ' + v.level + ' · ' + v.dims.join(' × ') + ' voxels of ' + size(v.level) + ' mm';
    const stream = volume.streamStatus();
    if (stream && stream.bricks > 0) {
      text += ' · near the camera level ' + stream.finest + ' (voxels of ' + size(stream.finest) +
        ' mm), ' + stream.bricks + ' of ' + stream.slots + ' bricks in ' + stream.budgetMB +
        ' MB of VRAM' + (stream.full ? ', budget full' : '') +
        (stream.loading ? ', loading …' : '');
    }
    status.textContent = text + ' · drag rotates, right or Shift drag pans, ' +
      'mouse wheel zooms';
  };
  volume.onDetail = showVolumeStatus;
  // WebGL cannot tell how much video memory is free; the user sets what the bricks may take.
  const vram = el('input', { type: 'number', min: 64, max: 16384, step: 64, value: volume.vramMB,
    title: 'Video memory for the bricks of finer levels, in MB' });
  vram.addEventListener('change', () => {
    if (Number(vram.value) >= 64) volume.setVramBudget(Number(vram.value));
  });
  const edges = sliderRow('Edges only', 'Opacity only where the grey value changes: surfaces, ' +
    'pore walls and boundaries between materials stand out, the inside clears',
  { min: 0, max: 1, step: 0.01 }, () => volume.edges, (value) => update({ edges: value }));
  ctx.properties.grey = [el('div', { className: 'field-row' }, style),
    volume.mode === 1 ? el('label', { className: 'group' }, shading, 'Lighting') : null,
    volume.mode === 1 ? edges : null,
    volume.mode !== 2 ? el('h4', {}, 'Light') : null,
    ...(volume.mode !== 2 ? lightingControls(volume, update) : []),
    el('p', { className: 'hint' }, volume.mode >= 4
      ? 'The coloured surface of the result replaces the grey values; choose "Grey value" ' +
        'under "Colour by" to see them again.'
      : 'Threshold, transfer function and colours are set in the histogram below the view.')]
    .filter(Boolean);
  if (porosity !== null) {
    ctx.properties.pores = [el('label', { className: 'group' }, 'Colour', poreColor),
      poreTable()];
    ctx.properties.zones = [el('label', { className: 'group' }, 'Colour', zoneColor),
      el('p', { className: 'hint' }, 'Blocks whose grey value is lowered by voids too small to ' +
        'resolve.')];
  }
  ctx.properties.objects = [el('p', { className: 'hint' }, 'Other volumes and CAD models show ' +
    'in the slice view and in the Objects view.')];
  const video = videoControls(volume, () => (state.status.saved_views ?? [])
    .map((view) => view.state?.volume).filter((v) => v && v.key === volume.volume?.key),
  () => state.status.name ?? 'voxelsieve');
  panel.append(el('div', { className: 'viewer-tools' },
    el('label', { className: 'group' }, 'Cut x', cut), ...video.controls,
    el('details', { className: 'view-settings' }, el('summary', {}, 'View settings'),
      el('div', { className: 'viewer-tools' },
        el('div', { className: 'group' }, 'Background', backgroundStyle, backgroundColors),
        el('label', { className: 'group' }, 'VRAM', vram, 'MB')))),
  canvas, video.progress, legend, status, looksRow, suggestionRow,
  transferEditor);
  applyMode();
  try {
    volume.attach(canvas);
  } catch (error) {
    status.textContent = error.message;
    return;
  }
  const regions = shownRegions(dataset, () => render());
  datasetInfo(dataset.step.id).then((info) => {
    volume.setRegions(regionsInVoxels(regions, voxelPitch(info.voxel_size_mm)));
  }).catch(showError);
  volume.load(dataset.step.id, porosity, materials?.id ?? null).then(() => {
    const key = volume.volume.key;
    const restore = state.restore;
    if (restore?.volume || restore?.transfer) {
      // A saved view of this volume: its settings replace the ones the controls were built with.
      if (restore.volume?.key === key) volume.setState(restore.volume);
      if (restore.transfer?.key === key && restore.transfer.points?.length >= 2) {
        Object.assign(transfer, restore.transfer);
      }
      restore.volume = null;
      restore.transfer = null;
      render();
      return;
    }
    volume.onChange();
    if (state.suggestions && !state.suggestions.images.length) {
      // Small pictures of the suggestions, rendered once per volume.
      state.suggestions.images = state.suggestions.items.map((suggestion) => volume.renderPreview(
        { mode: 1, shading: suggestion.shading }, lookupTable(suggestion.points)));
      renderSuggestions();
    }
    if (state.lookImages?.key !== key) {
      // One picture per frame, so the page keeps answering meanwhile.
      const images = {};
      state.lookImages = { key, images };
      (async () => {
        for (const name of Object.keys(LOOKS)) {
          await new Promise((resolve) => { requestAnimationFrame(resolve); });
          if (state.lookImages?.key !== key || volume.volume?.key !== key || volume.recording) {
            return;
          }
          images[name] = volume.renderPreview(...lookPreview(name));
          state.renderLooks?.();
        }
      })();
    }
  }).catch((error) => {
    status.textContent = error.message;
  });
  volume.onChange();
}

// ---------------------------------------------------------------------------------------------
// Objects: volumes and meshes in one global coordinate system (ADR 0018)

/// Rotation angle of a row-major 4x4 pose in degrees.
function poseAngle(pose) {
  const trace = pose[0] + pose[5] + pose[10];
  return Math.acos(Math.min(Math.max((trace - 1) / 2, -1), 1)) * 180 / Math.PI;
}

function objectName(id) {
  return (state.status.objects ?? []).find((object) => object.id === id)?.name ?? id;
}

/// Latest active surface step of an object, or null.
function surfaceStepOf(id) {
  const steps = activeSteps().filter((step) => step.object === id &&
    Object.values(step.outputs).some((output) => output.type === 'surface'));
  return steps.length ? steps[steps.length - 1].id : null;
}

/// What the scene shows of an object changes when a surface step is added or undone.
function objectKey(object) {
  return object.id + ':' + object.source.step + ':' +
    (object.kind === 'volume' ? surfaceStepOf(object.id) ?? 'box' : 'mesh');
}

function objectSwatch(index, color) {
  return el('span', { className: 'swatch', style: 'background: rgb(' +
    objectColor(index, color).join(',') + ')' });
}

/// The objects with their place in the global coordinate system; a click chooses the active
/// object, whose data new steps work on.
function objectTable(objects) {
  const active = state.status.active_object;
  return el('table', { className: 'objects' },
    el('thead', {}, el('tr', {}, el('th', {}, ''), el('th', {}, 'Object'), el('th', {}, 'Kind'),
      el('th', {}, 'Position (mm)'), el('th', {}, 'Rotation'), el('th', {}, 'Active'))),
    el('tbody', {}, objects.map((object, index) => el('tr', {
      className: object.id === active ? 'active' : null,
    },
    el('td', {}, objectSwatch(index, object.color)),
    el('td', { title: object.id }, object.name),
    el('td', {}, object.kind === 'volume' ? 'Volume' : 'Mesh'),
    el('td', {}, [3, 7, 11].map((i) => formatNumber(Math.round(object.pose[i] * 1000) / 1000))
      .join(', ')),
    el('td', {}, formatNumber(Math.round(poseAngle(object.pose) * 100) / 100) + '°'),
    el('td', {}, el('input', {
      type: 'radio', name: 'active-object', checked: object.id === active,
      disabled: state.busy,
      title: 'New steps work on the data of this object',
      onchange: () => action(() => api('object_select', { id: object.id })),
    }))))));
}

function renderObjectsCard(panel) {
  const objects = state.status.objects ?? [];
  const add = el('button', {
    disabled: state.busy,
    onclick: async () => {
      const chosen = await browse({ title: 'Choose a CAD model (STL)', kinds: ['mesh', 'file'] });
      if (chosen) await runStep('add_mesh', { path: chosen.path });
    },
  }, 'Add CAD model (STL) …');
  const card = el('div', { className: 'card' }, el('h3', {}, 'Objects'));
  if (objects.length) {
    card.append(el('p', {}, 'Every scan and CAD model is an object in one global coordinate ' +
      'system. Choosing another dataset adds an object; the view "Objects" aligns and compares ' +
      'them.'), objectTable(objects));
  } else {
    card.append(el('p', {}, 'Scans and CAD models become objects in one global coordinate ' +
      'system, which can be aligned to each other and compared.'));
  }
  const row = el('div', { className: 'row' }, add);
  if (objects.length > 1) {
    row.append(el('button', {
      onclick: () => { state.stage = 'workspace'; state.viewMode = 'scene'; render(); },
    }, 'Align and compare →'));
  }
  card.append(row);
  panel.append(card);
}

/// A select of objects; `filter` limits the choice.
function objectSelect(value, onchange, filter = () => true) {
  const objects = state.status.objects ?? [];
  return el('select', { onchange: (event) => onchange(event.target.value) },
    objects.filter(filter).map((object) => el('option', {
      value: object.id, selected: object.id === value,
    }, object.name + ' (' + object.id + ')')));
}

/// Default choice of the alignment forms: the active object is the target, the next one moves.
function alignState() {
  const objects = state.status.objects ?? [];
  const ids = objects.map((object) => object.id);
  const align = state.align ?? (state.align = { picking: false, points: { moving: [], target: [] },
    tolerance: 0.1, start: 'current', translation: [0, 0, 0], axis: 'z', degrees: 0 });
  if (!ids.includes(align.target)) {
    align.target = ids.includes(state.status.active_object) ? state.status.active_object : ids[0];
  }
  if (!ids.includes(align.moving) || align.moving === align.target) {
    align.moving = ids.find((id) => id !== align.target) ?? ids[0];
  }
  const volumes = objects.filter((object) => object.kind === 'volume').map((object) => object.id);
  if (!volumes.includes(align.actual)) align.actual = volumes[0];
  if (!ids.includes(align.nominal) || align.nominal === align.actual) {
    align.nominal = ids.find((id) => id !== align.actual);
  }
  return align;
}

/// Runs surface steps on the volumes among `ids` that have none, so they can be fitted.
async function ensureSurfaces(ids) {
  for (const id of ids) {
    const object = (state.status.objects ?? []).find((o) => o.id === id);
    if (object?.kind === 'volume' && surfaceStepOf(id) === null) {
      const step = await runStep('surface', { object: id });
      if (!step) return false;
    }
  }
  return true;
}

function lastStepCard(operation, title) {
  const last = latestStepOf(operation);
  if (!last) return null;
  return el('details', {}, el('summary', {}, title + ' (step ' + last.id + ')'),
    summaryTable(last.summary));
}

function renderSceneView(panel) {
  const scene = state.scene ?? (state.scene = new SceneViewer());
  const objects = state.status.objects ?? [];
  const align = alignState();
  const canvas = state.sceneCanvas ?? (state.sceneCanvas = el('canvas', { tabindex: 0 }));
  const status = el('div', { className: 'viewer-status' });
  if (state.restore?.scene) {
    scene.setState(state.restore.scene);
    state.restore.scene = null;
  }
  scene.onInteract = scheduleViewSave;
  state.capture = () => scene.capture();

  const picked = () => align.points.moving.length + align.points.target.length;
  const showPicks = () => {
    scene.picks = [
      ...align.points.moving.map((point) => ({ point, color: [255, 220, 0] })),
      ...align.points.target.map((point) => ({ point, color: [0, 220, 255] }))];
    scene.requestDraw();
  };
  const describe = () => {
    const shown = objects.length - objects.filter((o) => scene.hidden.has(o.id)).length;
    let text = shown + ' of ' + objects.length + ' objects shown · drag turns, right drag moves, ' +
      'wheel zooms · axes x red, y green, z blue';
    if (align.picking) {
      text = 'Picking: click points on ' + objectName(align.moving) + ' (yellow) and the same ' +
        'points on ' + objectName(align.target) + ' (cyan), in the same order';
    }
    status.textContent = text;
  };
  showPicks();

  const visibility = objects.map((object, index) => {
    const box = el('input', { type: 'checkbox', checked: !scene.hidden.has(object.id) });
    box.addEventListener('change', () => {
      if (box.checked) scene.hidden.delete(object.id);
      else scene.hidden.add(object.id);
      scene.requestDraw();
      describe();
      scheduleViewSave();
    });
    return el('label', { className: 'group', title: object.id }, box,
      objectSwatch(index, object.color), object.name);
  });
  const tools = el('div', { className: 'viewer-tools' }, visibility,
    el('button', { onclick: () => scene.reset() }, 'Reset view'));

  // Point pairs
  const pairCount = () => Math.min(align.points.moving.length, align.points.target.length);
  const pickedHint = el('p', { className: 'hint' });
  const clear = el('button', {
    onclick: () => {
      align.points = { moving: [], target: [] };
      showPicks();
      updatePairs();
    },
  }, 'Clear');
  const alignPairs = runButton('Align', 'align_points', () => {
    if (pairCount() < 3) throw new Error('Pick at least three point pairs');
    return {
      objects: [align.moving], target: align.target,
      pairs: align.points.moving.slice(0, pairCount()).map((moving, i) =>
        ({ moving, target: align.points.target[i] })),
    };
  }, () => {
    align.points = { moving: [], target: [] };
    align.picking = false;
    render();
  });
  // Picking changes only these, so the scene is not built again for every point.
  const updatePairs = () => {
    pickedHint.textContent = align.points.moving.length + ' points on ' +
      objectName(align.moving) + ', ' + align.points.target.length + ' on ' +
      objectName(align.target);
    alignPairs.textContent = 'Align (' + pairCount() + ' pairs)';
    alignPairs.disabled = state.busy || pairCount() < 3;
    clear.disabled = picked() === 0;
  };
  scene.onPick = align.picking ? (id, point) => {
    if (id === align.moving) align.points.moving.push(point);
    else if (id === align.target) align.points.target.push(point);
    else return;
    showPicks();
    updatePairs();
  } : null;
  const pairs = el('div', { className: 'card' }, el('h3', {}, 'Align by point pairs'),
    el('p', {}, 'Moves one object so that points picked on it meet the same points on the ' +
      'target. At least three pairs, not on one line.'),
    el('label', {}, 'Move ', objectSelect(align.moving, (id) => {
      align.moving = id;
      align.points.moving = [];
      render();
    }, (o) => o.id !== align.target)),
    el('label', {}, ' onto ', objectSelect(align.target, (id) => {
      align.target = id;
      align.points.target = [];
      render();
    })),
    el('div', { className: 'row' },
      el('button', {
        className: align.picking ? 'on' : null,
        onclick: () => { align.picking = !align.picking; render(); },
      }, align.picking ? 'Stop picking' : 'Pick points'),
      clear, alignPairs),
    pickedHint,
    lastStepCard('align_points', 'Last point alignment'));
  updatePairs();

  // Best fit
  const startSelect = el('select', { onchange: (event) => { align.start = event.target.value; } },
    [['current', 'from where they lie'], ['principal_axes', 'find the orientation first']]
      .map(([value, label]) => el('option', { value, selected: align.start === value }, label)));
  const fit = el('div', { className: 'card' }, el('h3', {}, 'Best fit of the surfaces'),
    el('p', {}, 'Moves ' + objectName(align.moving) + ' so that its surface fits the surface of ' +
      objectName(align.target) + ' best. Volumes get a surface step first if they have none.'),
    el('label', {}, 'Start ', startSelect),
    el('div', { className: 'row' }, el('button', {
      className: 'primary',
      disabled: state.busy || align.moving === align.target,
      onclick: async () => {
        if (!(await ensureSurfaces([align.moving, align.target]))) return;
        await runStep('align_surfaces', {
          objects: [align.moving], target: align.target, start: align.start,
        });
      },
    }, 'Fit')),
    lastStepCard('align_surfaces', 'Last best fit'));

  // Move by hand
  const translation = [0, 1, 2].map((k) => el('input', {
    type: 'number', step: 'any', value: align.translation[k], title: 'xyz'[k] + ' in mm',
    onchange: (event) => { align.translation[k] = Number(event.target.value); },
  }));
  const axisSelect = el('select', { onchange: (event) => { align.axis = event.target.value; } },
    ['x', 'y', 'z'].map((axis) => el('option', { value: axis, selected: align.axis === axis },
      axis)));
  const degrees = el('input', { type: 'number', step: 'any', value: align.degrees,
    onchange: (event) => { align.degrees = Number(event.target.value); } });
  const move = el('div', { className: 'card' }, el('h3', {}, 'Move'),
    el('p', {}, 'Turns ' + objectName(align.moving) + ' about its centre, then shifts it.'),
    el('label', {}, 'Rotation about ', axisSelect, ' by ', degrees, '°'),
    el('label', {}, 'Translation (mm) ', translation),
    el('div', { className: 'row' }, runButton('Move', 'move', () => {
      const object = objects.find((o) => o.id === align.moving);
      const mesh = object && scene.meshes.get(objectKey(object));
      const params = { objects: [align.moving], translation_mm: align.translation,
        rotation_axis: { x: [1, 0, 0], y: [0, 1, 0], z: [0, 0, 1] }[align.axis],
        rotation_deg: align.degrees };
      if (mesh) {
        const centre = mesh.bounds.min.map((m, k) => (m + mesh.bounds.max[k]) / 2 * mesh.scale[k]);
        params.center_mm = applyPose(object.pose, centre);
      }
      return params;
    })));

  // Comparison
  const volumes = objects.filter((o) => o.kind === 'volume');
  const tolerance = el('input', { type: 'number', step: 'any', min: 0.001, value: align.tolerance,
    onchange: (event) => { align.tolerance = Number(event.target.value); } });
  const compare = el('div', { className: 'card' }, el('h3', {}, 'Nominal-actual comparison'),
    volumes.length && align.nominal ? [
      el('p', {}, 'Deviation of the scanned surface from the nominal object where both lie now.'),
      el('label', {}, 'Scan ', objectSelect(align.actual, (id) => { align.actual = id; render(); },
        (o) => o.kind === 'volume')),
      el('label', {}, ' against ', objectSelect(align.nominal, (id) => {
        align.nominal = id;
        render();
      }, (o) => o.id !== align.actual)),
      el('label', {}, 'Tolerance (± mm) ', tolerance),
      el('label', { title: 'For flexible parts: bend the scanned surface onto the nominal one ' +
        'before measuring; how far it was moved is shown beside the deviation' },
      el('input', { type: 'checkbox', checked: Boolean(align.nonRigid),
        onchange: (event) => { align.nonRigid = event.target.checked; } }),
      ' Non-rigid registration (flexible part)'),
      el('div', { className: 'row' }, el('button', {
        className: 'primary',
        disabled: state.busy,
        onclick: async () => {
          if (!(await ensureSurfaces([align.actual]))) return;
          await runStep('compare_objects', { object: align.actual, nominal: align.nominal,
            tolerance_mm: align.tolerance, non_rigid: Boolean(align.nonRigid) });
        },
      }, 'Compare')),
      lastStepCard('compare_objects', 'Last comparison')]
      : el('p', { className: 'hint' }, 'Needs a scanned volume and a second object.'));

  const side = el('div', { className: 'pores scene-tools' },
    el('h3', {}, 'Objects'), objectTable(objects),
    objects.length > 1 ? [pairs, fit, move, compare] : el('p', { className: 'hint' },
      'Add a second object to align and compare.'));
  panel.append(el('div', { className: 'viewer scene' }, el('div', {}, tools, canvas, status),
    side));
  describe();
  try {
    scene.attach(canvas);
  } catch (error) {
    status.textContent = error.message;
    return;
  }
  scene.setObjects(objects.map((object, index) => ({ ...object, index,
    key: objectKey(object) }))).catch((error) => { status.textContent = error.message; });
}

function render() {
  renderHeader();
  renderProtocol();
  const open = state.status.open;
  $('welcome').hidden = open;
  $('wizard').hidden = !open;
  if (!open) {
    const pending = state.pendingScan;
    $('welcome-start').hidden = Boolean(pending);
    $('welcome-check').hidden = !pending;
    $('welcome-check').replaceChildren(...(pending ? [scanCheckCard(pending)] : []));
    return;
  }
  renderStages();
  const panel = $('stage');
  panel.replaceChildren();
  state.capture = null;
  if (state.stage === 'dataset') renderDatasetStage(panel);
  else if (state.stage === 'workspace') renderWorkspace(panel);
  else renderReportStage(panel);
  scheduleViewSave();
}

// ---------------------------------------------------------------------------------------------
// Commands

async function newProject() {
  const chosen = await browse({ title: 'Create a new project', name: 'project' });
  if (!chosen) return;
  await action(async () => {
    await api('project_create', { path: chosen.path });
  });
}

async function openProject() {
  const chosen = await browse({ title: 'Open project', kinds: ['project'] });
  if (!chosen) return;
  await action(async () => {
    await api('project_open', { path: chosen.path });
  });
}

async function saveAs() {
  const chosen = await browse({ title: 'Save project as', name: state.status.name + '-copy' });
  if (!chosen) return;
  await action(() => api('project_save_as', { path: chosen.path }));
}

function undo() {
  if (!state.busy && state.status.can_undo) action(() => api('undo'));
}

function redo() {
  if (!state.busy && state.status.can_redo) action(() => api('redo'));
}

async function start() {
  for (const element of document.querySelectorAll('[data-icon]')) {
    element.prepend(icon(element.dataset.icon));
  }
  setupBrowser();
  $('btn-new').addEventListener('click', newProject);
  $('welcome-new').addEventListener('click', newProject);
  $('welcome-scan').addEventListener('click', () => chooseScan());
  $('btn-open').addEventListener('click', openProject);
  $('welcome-open').addEventListener('click', openProject);
  $('btn-save-as').addEventListener('click', saveAs);
  $('btn-undo').addEventListener('click', undo);
  // The protocol takes room the workspace needs on smaller screens; it starts hidden there.
  const showProtocol = (show) => {
    document.body.classList.toggle('protocol-hidden', !show);
    $('btn-protocol').classList.toggle('on', show);
  };
  const savedProtocol = storageGet('protocol');
  showProtocol(savedProtocol === null ? window.innerWidth >= 1600 : savedProtocol === 'shown');
  $('btn-protocol').addEventListener('click', () => {
    const show = document.body.classList.contains('protocol-hidden');
    showProtocol(show);
    storageSet('protocol', show ? 'shown' : 'hidden');
    render();
  });
  $('btn-redo').addEventListener('click', redo);
  $('busy-cancel').addEventListener('click', async () => {
    // The operation stops at its next progress report; runStep then shows it as failed.
    $('busy-cancel').disabled = true;
    $('busy-text').textContent = $('busy-text').textContent.replace(' running …', ' stopping …');
    try {
      await api('cancel');
    } catch (error) {
      showError(error);
    }
  });
  for (const button of document.querySelectorAll('.stages button')) {
    button.addEventListener('click', () => {
      state.stage = button.dataset.stage;
      render();
    });
  }
  document.addEventListener('keydown', (event) => {
    if (['INPUT', 'TEXTAREA', 'SELECT'].includes(document.activeElement?.tagName)) return;
    if (state.stage === 'workspace' && state.viewMode === 'slice' && state.viewer?.info) {
      const steps = { ArrowUp: 1, ArrowDown: -1, PageUp: 10, PageDown: -10 };
      if (event.key in steps) {
        event.preventDefault();
        state.viewer.moveSlice(steps[event.key]);
        return;
      }
    }
    if (!(event.ctrlKey || event.metaKey) || event.key.toLowerCase() !== 'z') return;
    event.preventDefault();
    if (event.shiftKey) redo();
    else undo();
  });
  try {
    state.operations = (await api('list_operations')).operations;
    await refresh();
    await resumeRun();
  } catch (error) {
    showError(error);
  }
}

start();
