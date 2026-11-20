// Unit tests of how the slice viewer (resources/ui/viewer.js) loads and keeps its tiles: the level
// for the zoom, the request queue, loading slice by slice while scrolling, failed and aborted
// requests and the tile cache. Run with node; fetch and the canvas are stand-ins.
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');

global.window = { devicePixelRatio: 1 };
global.requestAnimationFrame = () => {};
const { SliceViewer, TILE, MAX_REQUESTS, MIN_TILES } = require(
  path.join(__dirname, '../../resources/ui/viewer.js'));

/// A 2D context that accepts every call.
function nullContext() {
  return new Proxy({}, {
    get: (target, name) => (name in target ? target[name] : () => {}),
    set: (target, name, value) => { target[name] = value; return true; },
  });
}

/// fetch that records requests and answers them when told to; aborted requests reject as the
/// browser's do.
function fakeFetch() {
  const requests = [];
  const fetch = (url, { signal }) => new Promise((resolve, reject) => {
    const params = new URL(url, 'http://host/').searchParams;
    const request = {
      url, signal, index: Number(params.get('index')), level: Number(params.get('level')),
      u: Number(params.get('u')), v: Number(params.get('v')),
      answer: () => resolve({ ok: true, headers: { get: () => null },
        arrayBuffer: async () => new ArrayBuffer(TILE * TILE * 5) }),
      fail: () => resolve({ ok: false, json: async () => ({ error: 'no such tile' }) }),
    };
    signal.addEventListener('abort', () => {
      const error = new Error('aborted');
      error.name = 'AbortError';
      reject(error);
    });
    requests.push(request);
  });
  return { fetch, requests };
}

const settle = async () => {
  for (let i = 0; i < 5; i += 1) await new Promise((resolve) => setImmediate(resolve));
};

/// A viewer of a 1024^3 dataset with three levels on a canvas of 1024 x 768, at level 0 over the
/// middle of the volume: 4 x 4 tiles visible.
function viewer() {
  const v = new SliceViewer();
  v.canvas = { clientWidth: 1024, clientHeight: 768, width: 0, height: 0,
    getContext: () => nullContext() };
  v.setDataset({ dims: [1024, 1024, 1024], voxel_size_mm: 0.1, levels: [{}, {}, {}] }, 7, null);
  v.fitPending = false;
  v.zoom = 1;
  v.center = [512, 512];
  v.renderTile = () => ({});
  v.window = [0, 1];  // no automatic window, which would draw again
  return v;
}

// The level whose voxels are a device pixel or a little larger, within the levels there are.
{
  const v = viewer();
  const levelAt = (zoom, ratio = 1) => {
    v.zoom = zoom;
    window.devicePixelRatio = ratio;
    return v.level();
  };
  assert.equal(levelAt(1), 0);
  assert.equal(levelAt(4), 0);
  assert.equal(levelAt(0.5), 1);
  assert.equal(levelAt(0.3), 1);
  assert.equal(levelAt(0.25), 2);
  assert.equal(levelAt(0.01), 2);       // the coarsest level there is
  assert.equal(levelAt(0.5, 2), 0);     // two device pixels per CSS pixel
  window.devicePixelRatio = 1;
}

// A draw asks for the visible tiles of the slice, nearest to the middle of the view first, at
// most MAX_REQUESTS at a time; the rest wait in the queue.
(async () => {
  {
    const { fetch, requests } = fakeFetch();
    global.fetch = fetch;
    const v = viewer();
    v.draw();
    assert.equal(v.visibleCount, 16);
    assert.equal(requests.length, MAX_REQUESTS);
    assert.equal(v.queue.length, 16 - MAX_REQUESTS);
    assert.ok(requests.every((r) => r.level === 0 && r.index === 512));
    // The first ones are the four around the centre of the view.
    assert.deepEqual(requests.map((r) => [r.u / TILE, r.v / TILE]).sort(),
      [[1, 1], [1, 2], [2, 1], [2, 2]]);
    // An answer frees a place for the next tile, asked for at the next frame.
    requests[0].answer();
    await settle();
    v.draw();
    assert.equal(v.tiles.size, 1);
    assert.equal(requests.length, MAX_REQUESTS + 1);
    const tile = v.tiles.get(v.key(0, requests[0].u / TILE, requests[0].v / TILE));
    assert.equal(tile.data.length, TILE * TILE);
    assert.equal(tile.overlay.length, TILE * TILE);
    // The value under the pointer comes from the tile; elsewhere there is none yet.
    assert.equal(v.valueAt([requests[0].u + 5, requests[0].v + 5, 512]), 0);
    const other = requests[1];
    assert.equal(v.valueAt([other.u + 5, other.v + 5, 512]), undefined);
  }

  // Scrolling while a slice loads: the slice started loads completely first, then the one
  // current by then; the slices scrolled past are never asked for.
  {
    const { fetch, requests } = fakeFetch();
    global.fetch = fetch;
    const v = viewer();
    v.draw();
    v.index[2] = 513;
    v.draw();
    v.index[2] = 514;
    v.draw();
    assert.ok(requests.every((r) => r.index === 512));
    while (requests.some((r) => !r.done)) {
      for (const r of requests.filter((x) => !x.done)) {
        r.done = true;
        r.answer();
      }
      await settle();
      v.draw();
    }
    const indices = [...new Set(requests.map((r) => r.index))];
    assert.deepEqual(indices, [512, 514]);
    assert.equal(requests.filter((r) => r.index === 514).length, 16);
    assert.equal(v.shownSlice(0, v.visibleTiles(0)), 514);
  }

  // The nearest complete slice stands in while the current one is incomplete.
  {
    const { fetch, requests } = fakeFetch();
    global.fetch = fetch;
    const v = viewer();
    v.draw();
    while (requests.some((r) => !r.done)) {
      for (const r of requests.filter((x) => !x.done)) {
        r.done = true;
        r.answer();
      }
      await settle();
      v.draw();
    }
    v.index[2] = 520;
    v.draw();
    assert.equal(v.shownSlice(0, v.visibleTiles(0)), 512);
    assert.equal(v.nearestTile(0, 1, 1).slice, 512);
  }

  // Zooming out to another level gives up the requests of the tiles no longer wanted.
  {
    const { fetch, requests } = fakeFetch();
    global.fetch = fetch;
    const v = viewer();
    v.draw();
    v.zoom = 0.5;
    v.draw();
    await settle();
    const first = requests.slice(0, MAX_REQUESTS);
    assert.ok(first.every((r) => r.signal.aborted));
    assert.equal(v.failed.size, 0);  // aborted, not failed
    assert.equal(v.active, 0);
    v.draw();
    const next = requests.slice(MAX_REQUESTS);
    assert.equal(next.length, 4);  // 2 x 2 tiles of level 1 cover the view
    assert.ok(next.every((r) => r.level === 1 && !r.signal.aborted));
  }

  // A tile the server cannot give is not asked for again, and the slice counts as complete.
  {
    const { fetch, requests } = fakeFetch();
    global.fetch = fetch;
    const v = viewer();
    v.draw();
    requests[0].fail();
    const failedKey = v.key(0, requests[0].u / TILE, requests[0].v / TILE);
    const warn = console.warn;
    console.warn = () => {};
    await settle();
    console.warn = warn;
    assert.ok(v.failed.has(failedKey));
    v.draw();
    assert.equal(requests.filter((r) => r.u === requests[0].u && r.v === requests[0].v).length, 1);
  }

  // The cache keeps three screens of tiles, at least MIN_TILES, and drops those used longest
  // ago, never those of the current frame.
  {
    const v = viewer();
    v.visibleCount = 16;
    assert.equal(v.tileCapacity(), MIN_TILES);
    v.visibleCount = 100;
    assert.equal(v.tileCapacity(), 300);
    v.visibleCount = 16;
    for (let i = 0; i < MIN_TILES + 10; i += 1) {
      v.tiles.set('t' + i, { used: i });
    }
    v.frameStart = MIN_TILES + 5;  // the last five were drawn in this frame
    v.evict();
    assert.equal(v.tiles.size, MIN_TILES);
    assert.ok(!v.tiles.has('t0') && !v.tiles.has('t9') && v.tiles.has('t10'));
    // More tiles in the frame than the capacity: they all stay.
    v.frameStart = 0;
    for (let i = 0; i < 20; i += 1) v.tiles.set('n' + i, { used: 1000 + i });
    v.evict();
    assert.equal(v.tiles.size, MIN_TILES + 20);
  }

  console.log('tile tests passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
