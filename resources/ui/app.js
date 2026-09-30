// VoxelSieve Studio: browser UI over the studio API (ADR 0008). Plain JavaScript, no build step.
// Everything goes through POST api/call/<method>, the same methods MCP clients use.
'use strict';

const $ = (id) => document.getElementById(id);

const state = {
  status: { open: false },
  operations: [],
  stage: 'dataset',
  rawPath: null,
  rawOperation: null,
  busy: false,
  viewer: null,
  volume: null,
  viewMode: 'slice',
  pores: { step: null, list: [] },
  projectDir: null,     // directory of the open project, to notice when another one is opened
  transfer: null,       // transfer function of the 3D view
  restore: null,        // parts of a saved view state still to apply: {slice, volume, transfer}
  savedState: {},       // view state last sent to the studio
  suggestions: null,    // renderings suggested for the loaded volume: {key, items, images}
  capture: null,        // returns the picture of the current view as a PNG data URL
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
  rotation_deg: 'Rotation CAD → scan (°)',
  dropped_components: 'Omitted internal surfaces',
};

// ---------------------------------------------------------------------------------------------
// Helpers

function el(tag, attributes = {}, ...children) {
  const element = document.createElement(tag);
  for (const [key, value] of Object.entries(attributes)) {
    if (value === undefined || value === null || value === false) continue;
    if (key.startsWith('on')) element.addEventListener(key.slice(2), value);
    else if (key === 'className') element.className = value;
    else element.setAttribute(key, value === true ? '' : value);
  }
  for (const child of children.flat()) {
    if (child === undefined || child === null || child === false) continue;
    element.append(child instanceof Node ? child : document.createTextNode(String(child)));
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
  return latestOutput('dataset') ? 'analysis' : 'dataset';
}

function projectOpened() {
  state.projectDir = state.status.dir;
  state.viewer = null;
  state.volume = null;
  state.transfer = null;
  state.suggestions = null;
  state.pores = { step: null, list: [] };
  state.savedState = state.status.view ?? {};
  applyViewState(state.savedState);
}

/// Shows a view state: stage and view mode now, viewer settings once their data is loaded.
function applyViewState(view) {
  const stages = ['dataset', 'analysis', 'report', 'view'];
  state.stage = stages.includes(view?.stage) ? view.stage : defaultStage();
  if (view?.viewMode === 'slice' || view?.viewMode === '3d') state.viewMode = view.viewMode;
  state.restore = { slice: view?.slice ?? null, volume: view?.volume ?? null,
    transfer: view?.transfer ?? null };
}

function collectViewState() {
  const previous = state.savedState ?? {};
  const pending = state.restore ?? {};
  const transfer = state.transfer?.points
    ? { key: state.transfer.key, preset: state.transfer.preset,
      colorMap: state.transfer.colorMap, points: state.transfer.points }
    : pending.transfer ?? previous.transfer ?? null;
  return {
    stage: state.stage,
    viewMode: state.viewMode,
    slice: state.viewer?.info ? state.viewer.getState() : pending.slice ?? previous.slice ?? null,
    volume: state.volume?.volume ? state.volume.getState()
      : pending.volume ?? previous.volume ?? null,
    transfer,
  };
}

let viewSaveTimer = null;

/// Sends the view state to the studio shortly after the last change.
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
    el('button', { onclick: save, title: 'Save the current view with a picture in the project' },
      'Save view'),
    el('button', {
      onclick: () => state.capture &&
        downloadDataUrl(state.capture(), fileName(name.value.trim() || 'view')),
      title: 'Download the current view as PNG',
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
      title: 'Download image' }, '⤓') : null,
    el('button', { className: 'icon', title: 'Rename', onclick: () => renameSavedView(view) },
      '✎'),
    el('button', { className: 'icon', title: 'Delete', onclick: () => deleteSavedView(view) },
      '×'))))));
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

async function runStep(operation, params) {
  clearError();
  state.busy = true;
  const title = operationInfo(operation)?.title ?? operation;
  $('busy-text').textContent = title + ' running …';
  $('busy-progress').removeAttribute('value');
  $('busy').hidden = false;
  render();
  const poll = setInterval(async () => {
    try {
      const status = await api('project_status');
      if (status.running && status.running.progress > 0) {
        $('busy-progress').value = status.running.progress;
      }
    } catch {
      // The next poll or the result will show the problem.
    }
  }, 500);
  let step = null;
  try {
    step = await api('run_' + operation, params);
  } catch (error) {
    showError(title + ': ' + error.message);
  } finally {
    clearInterval(poll);
    state.busy = false;
    $('busy').hidden = true;
  }
  await refresh();
  return step;
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
        el('span', { className: 'kind' }, kindLabel(entry.kind)),
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
        // A directory of TIFF slices is chosen by a click and the button; a double click opens it.
        if (selectable && entry.kind === 'tiff' && entry.directory) browserLoad(entry.path);
        else if (selectable) browserFinish(entry);
      });
      list.append(item);
    }
    if (!listing.entries.length) list.append(el('li', { className: 'disabled' }, 'empty'));
  } catch (error) {
    showError(error);
  }
}

function kindLabel(kind) {
  return { dir: 'Folder', project: 'Project', dataset: 'Dataset', raw: 'Raw data',
    tiff: 'TIFF', file: 'File' }[kind] ?? kind;
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
// Forms from JSON schema

function buildForm(schema, initial = {}, skip = ['inputs']) {
  const form = el('div', { className: 'form' });
  const readers = [];
  const required = schema.required ?? [];
  // Known parameters in the order of LABELS, then the others; schemas arrive sorted by name.
  const known = Object.keys(LABELS);
  const rank = (name) => (known.includes(name) ? known.indexOf(name) : known.length);
  const properties = Object.entries(schema.properties ?? {})
    .sort(([a], [b]) => rank(a) - rank(b) || a.localeCompare(b));
  for (const [name, property] of properties) {
    if (skip.includes(name)) continue;
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
            const chosen = await browse({ title: LABELS[name] ?? name, kinds: ['file', 'raw'] });
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

function summaryTable(summary) {
  const rows = Object.entries(summary ?? {}).map(([key, value]) => el('tr', {},
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
    (threads ? ' of ' + threads : '') + ' cores, ' + formatMb(total.peak_rss_mb) + ' peak';
}

function telemetryChart(timeline, system) {
  const points = timeline.t_s ?? [];
  if (points.length < 2) return el('p', { className: 'meta' }, 'Too short for a timeline');
  const width = 320;
  const height = 90;
  const end = points[points.length - 1];
  const memory = points.map((_, i) => timeline.rss_anon_mb[i] + timeline.rss_file_mb[i]);
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
      el('td', {}, formatMb(phase.peak_rss_mb))));
  const table = el('table', { className: 'summary telemetry' },
    el('tr', {}, ['Phase', 'Time', 'Cores', 'Memory'].map((h) => el('th', {}, h))),
    rows);
  const total = telemetry.total;
  const disk = el('div', { className: 'meta' }, 'Disk: ' + formatMb(total.disk_read_mb) + ' read, ' +
    formatMb(total.disk_write_mb) + ' written, ' + formatNumber(total.major_faults) + ' page faults');
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
    const classes = [step.status, step.active ? '' : 'undone'].join(' ');
    const statusText = { done: '✓', failed: '✗', running: '…' }[step.status] ?? step.status;
    const item = el('li', { className: classes },
      el('div', { className: 'title' }, statusText + ' ' + step.id + '. ' + step.title),
      el('div', { className: 'meta' }, formatTime(step.started),
        step.size_bytes ? ' · ' + formatBytes(step.size_bytes) : '',
        telemetryLine(step.telemetry),
        step.active ? '' : ' · undone'),
      step.messages.slice(-2).map((message) => el('div', { className: 'message' }, message)),
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
    analysis: Boolean(latestOutput('porosity')),
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
  const dataset = latestOutput('dataset');
  if (dataset) {
    panel.append(el('div', { className: 'card' },
      el('h3', {}, 'Current dataset (step ' + dataset.step.id + ')'),
      el('p', {}, dataset.step.outputs[dataset.output].path),
      summaryTable(dataset.step.summary),
      el('div', { className: 'row' }, nextButton('On to the analysis', 'analysis'))));
  } else {
    panel.append(el('p', { className: 'hint' },
      'Choose a sieved dataset (.vsieve), a raw file or a TIFF stack (folder, multi-page TIFF ' +
      'or ZIP). On import, raw data is freed from the air around the part and stored as a ' +
      'dataset with resolution levels; the original stays unchanged.'));
  }
  panel.append(el('div', { className: 'row' }, el('button', {
    disabled: state.busy,
    onclick: async () => {
      const chosen = await browse({ title: 'Choose a dataset or raw data',
        kinds: ['dataset', 'raw', 'tiff', 'file'] });
      if (!chosen) return;
      if (chosen.kind === 'dataset') {
        state.rawPath = null;
        await runStep('open_dataset', { path: chosen.path });
      } else {
        state.rawPath = chosen.path;
        state.rawOperation = chosen.kind === 'tiff' ? 'import_tiff' : 'import_raw';
        render();
      }
    },
  }, dataset ? 'Choose another dataset …' : 'Choose a dataset …')));

  const importOperation = state.rawOperation ?? 'import_raw';
  const importInfo = operationInfo(importOperation);
  if (state.rawPath && importInfo) {
    const form = buildForm(importInfo.parameters, { path: state.rawPath });
    panel.append(el('div', { className: 'card' },
      el('h3', {}, importInfo.title),
      el('p', {}, importOperation === 'import_tiff'
        ? 'The slices are sorted by name (numbers by value) and read directly from the folder or ' +
          'ZIP. With several folders, the grey values are chosen over label or mask folders. ' +
          'Without a voxel size in the files, 1 mm is assumed.'
        : 'Dimensions and voxel size come from the JSON file next to the raw data when they are ' +
          'not given. Without a threshold, it is found automatically (valley after the air peak).'),
      form.element,
      el('div', { className: 'row' },
        runButton('Import', importOperation, () => form.values(), () => {
          state.rawPath = null;
          render();
        }),
        el('button', { onclick: () => { state.rawPath = null; render(); } }, 'Cancel'))));
  }
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

function renderAnalysisStage(panel) {
  panel.append(el('h1', {}, 'Analysis'));
  const dataset = latestOutput('dataset');
  if (!dataset) {
    panel.append(el('p', { className: 'hint' }, 'Choose a dataset first.'),
      nextButton('To the dataset', 'dataset'));
    return;
  }
  panel.append(el('p', { className: 'hint' },
    'Operations work on the dataset of step ' + dataset.step.id +
    ' and write their result as a new step. Plugins appear here as well.'));
  const operations = state.operations.filter((operation) =>
    operation.inputs.some((input) => input.type === 'dataset'));
  for (const operation of operations) {
    const form = buildForm(operation.parameters);
    const last = latestStepOf(operation.id);
    const card = el('div', { className: 'card' },
      el('h3', {}, operation.title),
      el('p', {}, operation.description),
      el('details', {}, el('summary', {}, 'Parameters'), form.element),
      el('div', { className: 'row' }, runButton('Run', operation.id, () => form.values())));
    if (last) {
      const result = el('div', {}, el('h3', {}, 'Result (step ' + last.id + ')'),
        summaryTable(last.summary));
      appendImages(result, last);
      card.append(result);
    }
    panel.append(card);
  }
  if (latestOutput('porosity')) {
    panel.append(el('div', { className: 'row' }, nextButton('Check in the view', 'view'),
      nextButton('On to the report', 'report')));
  }
}

function renderReportStage(panel) {
  panel.append(el('h1', {}, 'Test report'));
  const porosity = latestOutput('porosity');
  const info = operationInfo('report');
  if (!porosity || !info) {
    panel.append(el('p', { className: 'hint' }, 'Run a porosity analysis first.'),
      nextButton('To the analysis', 'analysis'));
    return;
  }
  const last = latestStepOf('report');
  const form = buildForm(info.parameters, last ? last.params : {});
  panel.append(el('div', { className: 'card' },
    el('p', {}, 'The inspection order holds laboratory, customer, part, scan parameters and the ' +
      'acceptance limits per inspection zone (BDG P 202). Entries in the JSON field add to or ' +
      'override the file. An example is in examples/inspection_order.json.'),
    form.element,
    el('div', { className: 'row' }, runButton('Create report', 'report', () => form.values()))));
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
// Slice view

/// Latest porosity step computed from the dataset of `datasetStep`.
function porosityOf(datasetStep) {
  const steps = activeSteps().filter((step) => step.operation === 'porosity' &&
    step.inputs.dataset?.step === datasetStep);
  return steps.length ? steps[steps.length - 1].id : null;
}

/// Latest material segmentation computed from the dataset of `datasetStep`.
function materialsOf(datasetStep) {
  const steps = activeSteps().filter((step) =>
    ['segment_materials', 'segment_model'].includes(step.operation) &&
    step.inputs.dataset?.step === datasetStep);
  return steps.length ? steps[steps.length - 1] : null;
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
  if (state.stage === 'view') render();
}

function renderViewStage(panel) {
  const dataset = latestOutput('dataset');
  if (!dataset) {
    panel.append(el('h1', {}, 'View'),
      el('p', { className: 'hint' }, 'Choose a dataset first.'),
      nextButton('To the dataset', 'dataset'));
    return;
  }
  const modes = el('div', { className: 'group view-modes' },
    [['slice', 'Slice'], ['3d', '3D']].map(([mode, label]) => el('button', {
      className: state.viewMode === mode ? 'on' : null,
      onclick: () => { state.viewMode = mode; render(); },
    }, label)));
  renderViewBar(panel, modes);
  if (state.viewMode === '3d') {
    renderVolumeView(panel, dataset);
    return;
  }
  const viewer = state.viewer ?? (state.viewer = new SliceViewer());
  const porosity = porosityOf(dataset.step.id);
  const materials = materialsOf(dataset.step.id);
  loadPores(porosity);
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
  const overlay = el('input', { type: 'checkbox', checked: viewer.showOverlay,
    disabled: porosity === null && materials === null });
  overlay.addEventListener('change', () => viewer.setOverlay(overlay.checked));
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
    el('div', { className: 'group' }, 'Window', low, high,
      el('button', { onclick: () => viewer.autoWindow() }, 'Auto')),
    el('label', { className: 'group' }, overlay,
      materials === null ? 'Pores and zones' : 'Pores, zones, materials'),
    el('button', { onclick: () => { viewer.fit(); viewer.requestDraw(); viewer.onChange(); } },
      'Fit'));

  const poreRows = state.pores.list.slice(0, 500).map((pore) => el('tr', {
    onclick: () => viewer.showVoxel(pore.center_voxels, Math.max(8,
      ...pore.bounds_max.map((max, a) => max - pore.bounds_min[a] + 1))),
    title: 'Jump to the pore',
  }, el('td', {}, pore.id), el('td', {}, formatNumber(pore.equivalent_diameter_mm)),
  el('td', {}, formatNumber(pore.volume_mm3))));
  const side = el('div', { className: 'pores' },
    el('h3', {}, porosity === null ? 'No porosity analysis'
      : 'Pores (step ' + porosity + ')'),
    poreRows.length ? el('table', {},
      el('thead', {}, el('tr', {}, el('th', {}, 'No.'), el('th', {}, 'Ø mm'), el('th', {}, 'mm³'))),
      el('tbody', {}, poreRows))
      : el('p', { className: 'hint' }, porosity === null
        ? 'After a porosity analysis the pores appear here; a click jumps to the pore.'
        : 'No pores found.'));

  if (materials !== null) {
    // Legend: the classes with the grey value they start at and their volume.
    const rows = (materials.summary?.materials ?? []).map((material) => el('tr', {},
      el('td', {}, el('span', { className: 'swatch', style: 'background: rgb(' +
        MATERIAL_COLORS[(material.id - 1) % 8].join(',') + ')' }), ' ' + material.id),
      el('td', {}, formatNumber(Math.round(material.from_grey_value))),
      el('td', {}, formatNumber(material.volume_mm3))));
    side.append(el('h3', {}, 'Materials (step ' + materials.id + ')'),
      el('table', {}, el('thead', {}, el('tr', {}, el('th', {}, 'No.'), el('th', {}, 'from grey value'),
        el('th', {}, 'mm³'))), el('tbody', {}, rows)));
  }
  panel.append(el('div', { className: 'viewer' },
    el('div', {}, tools, canvas, status), side));

  state.capture = () => viewer.capture();
  api('dataset_info', { step: dataset.step.id }).then((info) => {
    viewer.setDataset(info, dataset.step.id, porosity, materials?.id ?? null);
    viewer.attach(canvas);
    if (state.restore?.slice) {
      viewer.setState(state.restore.slice);
      state.restore.slice = null;
    }
    viewer.onChange();
  }).catch(showError);
}

const IN_PLANE_NAMES = [['y', 'z'], ['x', 'z'], ['x', 'y']];

const VOLUME_MODES = ['Surface', 'Transfer function', 'Maximum intensity projection',
  'Extracted surface', 'Nominal-actual deviation'];

/// Latest surface step computed from the dataset of `datasetStep`.
function surfaceOf(datasetStep) {
  const steps = activeSteps().filter((step) => step.operation === 'surface' &&
    step.inputs.dataset?.step === datasetStep);
  return steps.length ? steps[steps.length - 1].id : null;
}

/// Latest nominal-actual comparison of a surface of the dataset of `datasetStep`.
function comparisonOf(datasetStep) {
  const surfaces = activeSteps().filter((step) => step.operation === 'surface' &&
    step.inputs.dataset?.step === datasetStep).map((step) => step.id);
  const steps = activeSteps().filter((step) => step.operation === 'compare_cad' &&
    surfaces.includes(step.inputs.surface?.step));
  return steps.length ? steps[steps.length - 1].id : null;
}

/// Colour bar of the deviation colours (deviationColor in compare.cpp) with its limits.
function deviationLegend(tolerance, range) {
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
    el('div', { className: 'legend-note' },
      'Blue: material missing · green: within tolerance · red: excess material'));
}

function renderVolumeView(panel, dataset) {
  const volume = state.volume ?? (state.volume = new VolumeViewer());
  // The transfer function outlives re-rendering; it is reset when another volume is loaded.
  const transfer = state.transfer ??
    (state.transfer = { key: null, preset: null, colorMap: 'viridis', points: null });
  const porosity = porosityOf(dataset.step.id);
  const surfaceStep = surfaceOf(dataset.step.id);
  const comparisonStep = comparisonOf(dataset.step.id);
  // Without a surface or comparison step its mesh cannot be shown; fall back to the grey values.
  if ((volume.mode === 3 && surfaceStep === null) ||
      (volume.mode === 4 && comparisonStep === null)) volume.mode = 0;
  const canvas = el('canvas');
  const histogram = el('canvas', { className: 'transfer', tabindex: 0,
    title: 'Click sets a point, drag moves it, double click or Delete removes it' });
  const editor = new TransferEditor(histogram);
  const status = el('div', { className: 'viewer-status' }, 'Loading overview …');
  const hint = el('div', { className: 'viewer-status' });
  const legend = el('div', { hidden: true });
  const suggestionRow = el('div', { className: 'suggestions' });
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
  const mode = el('select', { title: 'Rendering' },
    VOLUME_MODES.map((name, i) => {
      const missing = (i === 3 && surfaceStep === null) || (i === 4 && comparisonStep === null);
      return el('option', { value: i, selected: volume.mode === i, disabled: missing,
        title: missing ? 'Run the operation "' + (i === 3 ? 'Surface' : 'Nominal-actual comparison') +
          '" first' : null }, name);
    }));
  const shading = el('input', { type: 'checkbox', checked: volume.shading });
  shading.addEventListener('change', () => update({ shading: shading.checked }));
  const surface = colorInput('Surface colour', () => volume.surfaceColor,
    (rgb) => update({ surfaceColor: rgb }));
  const cut = el('input', { type: 'range', min: 0, max: 1, step: 0.005, value: volume.cut,
    title: 'Cut along x' });
  cut.addEventListener('input', () => update({ cut: Number(cut.value) }));
  const pores = el('input', { type: 'checkbox', checked: volume.pores,
    disabled: porosity === null });
  pores.addEventListener('change', () => update({ pores: pores.checked }));
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
  const colorMap = el('select', { title: 'Lay a colour map over the points' },
    Object.entries(COLOR_MAPS).map(([key, map]) => el('option', {
      value: key, selected: key === transfer.colorMap }, map.name)));
  const pointColor = el('input', { type: 'color', title: 'Colour of the selected point' });
  const pointOpacity = el('input', { type: 'number', min: 0, max: 100, step: 1,
    title: 'Opacity of the selected point in percent' });
  const removePoint = el('button', { onclick: () => editor.removeSelected(),
    title: 'Delete the selected point' }, 'Delete');
  const curveTools = el('div', { className: 'group' }, preset, colorMap,
    el('span', { className: 'sep' }), 'Point', pointColor, pointOpacity, '%', removePoint);
  const surfaceTools = el('label', { className: 'group' }, 'Colour', surface);

  preset.addEventListener('change', () => {
    if (preset.value) {
      transfer.preset = preset.value;
      transfer.colorMap = { dichte: 'viridis', rand: 'kupfer' }[preset.value] ?? 'stahl';
      colorMap.value = transfer.colorMap;
      editor.setPoints(transferPreset(preset.value, volume.threshold));
    }
    preset.value = '';
  });
  colorMap.addEventListener('change', () => {
    transfer.colorMap = colorMap.value;
    const selected = editor.selected;
    editor.setPoints(applyColorMap(editor.points, colorMap.value));
    editor.selected = selected;
    editor.onSelect();
  });
  pointColor.addEventListener('input', () => {
    if (editor.selected < 0) return;
    editor.points[editor.selected].color = parseHexColor(pointColor.value);
    editor.changed();
  });
  pointOpacity.addEventListener('change', () => {
    if (editor.selected < 0) return;
    editor.points[editor.selected].a = Math.min(Math.max(Number(pointOpacity.value) / 100, 0), 1);
    editor.changed();
  });

  editor.onChange = () => {
    transfer.points = editor.points;
    volume.set({ threshold: editor.threshold });
    volume.setTransfer(lookupTable(editor.points));
    scheduleViewSave();
  };
  const valueAt = (x) => {
    const w = volume.volume?.window;
    return w ? w[0] + x * (w[1] - w[0]) : x * 255;
  };
  const label = (x) => formatNumber(Math.round(valueAt(x)));
  editor.label = label;
  editor.onSelect = () => {
    const point = editor.points[editor.selected];
    pointColor.disabled = !point;
    pointOpacity.disabled = !point;
    removePoint.disabled = !point || editor.points.length <= 2;
    if (point) {
      pointColor.value = hexColor(point.color);
      if (document.activeElement !== pointOpacity) pointOpacity.value = Math.round(point.a * 100);
    }
    if (editor.hover !== null) {
      const bin = Math.min(Math.round(editor.hover * 255), 255);
      hint.textContent = 'Grey value ' + label(editor.hover) + ' · ' +
        formatNumber(volume.histogram[bin]) + ' voxels';
    } else {
      hint.textContent = editor.mode === 'threshold'
        ? 'Air/material threshold: ' + label(editor.threshold) + ' · drag in the histogram'
        : 'Curve: opacity per grey value · click sets a point, drag moves it, ' +
          'double click removes it';
    }
  };
  const applyMode = () => {
    update({ mode: Number(mode.value) });
    const surfaceMode = volume.mode === 0 || volume.mode >= 3;
    editor.setMode(surfaceMode ? 'threshold' : 'curve');
    curveTools.hidden = surfaceMode;
    surfaceTools.hidden = !surfaceMode;
    shading.parentElement.hidden = volume.mode !== 1;
    pores.parentElement.hidden = volume.mode >= 3;
    legend.hidden = volume.mode !== 4;
    if (volume.mode === 3) showSurface();
    if (volume.mode === 4) showDeviation();
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
  /// Loads the compared surface of the latest nominal-actual comparison of this dataset.
  const showDeviation = () => {
    status.textContent = 'Loading nominal-actual comparison of step ' + comparisonStep + ' …';
    volume.loadDeviation(comparisonStep).then((mesh) => {
      if (volume.mode !== 4) return;
      legend.replaceChildren(deviationLegend(mesh.tolerance, mesh.range));
      status.textContent = 'Nominal-actual deviation of step ' + comparisonStep + ' · ' +
        formatNumber(mesh.triangles) + ' triangles · drag rotates, mouse wheel zooms';
    }).catch((error) => {
      status.textContent = error.message;
    });
  };
  mode.addEventListener('change', applyMode);

  /// Uses a suggested rendering: transfer function mode with its curve and lighting.
  const useSuggestion = (suggestion) => {
    transfer.preset = null;
    transfer.colorMap = suggestion.colorMap;
    transfer.points = suggestion.points.map((p) => ({ ...p, color: [...p.color] }));
    volume.set({ mode: 1, shading: suggestion.shading });
    render();
  };
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
      const first = state.suggestions.items[0];
      transfer.key = v.key;
      transfer.preset = first ? null : 'durchsicht';
      transfer.colorMap = first ? first.colorMap : 'stahl';
      transfer.points = first ? first.points : transferPreset('durchsicht', volume.threshold);
      colorMap.value = transfer.colorMap;
    }
    editor.threshold = volume.threshold;
    editor.points = transfer.points;
    editor.setHistogram(volume.histogram);
    volume.setTransfer(lookupTable(editor.points));
    editor.onSelect();
    renderSuggestions();
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
    const detail = volume.detail;
    if (detail) {
      text += ' · near the camera level ' + detail.level + ' (' + detail.dims.join(' × ') +
        ' voxels of ' + size(detail.level) + ' mm)';
    }
    status.textContent = text + ' · drag rotates, right or Shift drag pans, ' +
      'mouse wheel zooms';
  };
  volume.onDetail = showVolumeStatus;
  panel.append(el('div', { className: 'viewer-tools' }, mode,
    el('label', { className: 'group' }, shading, 'Lighting'),
    el('label', { className: 'group' }, 'Cut x', cut),
    el('label', { className: 'group' }, pores, 'Pores', poreColor, zoneColor),
    el('div', { className: 'group' }, backgroundStyle, backgroundColors)),
  canvas, legend, status, suggestionRow,
  el('div', { className: 'transfer-editor' },
    el('div', { className: 'viewer-tools' }, el('b', {}, 'Histogram'), curveTools, surfaceTools),
    histogram, hint));
  applyMode();
  try {
    volume.attach(canvas);
  } catch (error) {
    status.textContent = error.message;
    return;
  }
  volume.load(dataset.step.id, porosity).then(() => {
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
  }).catch((error) => {
    status.textContent = error.message;
  });
  volume.onChange();
}

function render() {
  renderHeader();
  renderProtocol();
  const open = state.status.open;
  $('welcome').hidden = open;
  $('wizard').hidden = !open;
  if (!open) return;
  renderStages();
  const panel = $('stage');
  panel.replaceChildren();
  state.capture = null;
  if (state.stage === 'dataset') renderDatasetStage(panel);
  else if (state.stage === 'analysis') renderAnalysisStage(panel);
  else if (state.stage === 'view') renderViewStage(panel);
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
  setupBrowser();
  $('btn-new').addEventListener('click', newProject);
  $('welcome-new').addEventListener('click', newProject);
  $('btn-open').addEventListener('click', openProject);
  $('welcome-open').addEventListener('click', openProject);
  $('btn-save-as').addEventListener('click', saveAs);
  $('btn-undo').addEventListener('click', undo);
  $('btn-redo').addEventListener('click', redo);
  for (const button of document.querySelectorAll('.stages button')) {
    button.addEventListener('click', () => {
      state.stage = button.dataset.stage;
      render();
    });
  }
  document.addEventListener('keydown', (event) => {
    if (['INPUT', 'TEXTAREA', 'SELECT'].includes(document.activeElement?.tagName)) return;
    if (state.stage === 'view' && state.viewer?.info) {
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
  } catch (error) {
    showError(error);
  }
}

start();
