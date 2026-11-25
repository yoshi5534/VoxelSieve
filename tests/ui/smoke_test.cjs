// The inspection of the sample part (docs/quickstart.md) in a browser, as a user clicks through
// it: opening the scan (a project next to it, the import, the 3D view and the suggested
// analyses), the porosity analysis, the report, a region with the porosity in it, the slice and
// the 3D view with a look and a short video of it, a raw file without a sidecar, undo and redo.
// The scan is synthetic, so the answers are known: three shrinkage cavities and one zone of
// loosened microstructure. Nothing else clicks through resources/ui.
//
//   node smoke_test.cjs <vs-gui> <vs-synth> <inspection_order.json> <work dir>
//
// Needs the package playwright or playwright-core. The browser is the one of the package, the
// program in VOXELSIEVE_BROWSER, or the installed Chrome.

const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

function loadPlaywright() {
  for (const name of ['playwright', 'playwright-core']) {
    try {
      return require(name);
    } catch (error) {
      if (error.code !== 'MODULE_NOT_FOUND') throw error;
    }
  }
  throw new Error('Neither playwright nor playwright-core is installed');
}

function expect(condition, what) {
  if (!condition) throw new Error('Not as expected: ' + what);
}

/// Starts vs-gui on a free port, for the files below `root`; resolves to the process and the
/// address it prints.
function startServer(program, root) {
  const server = spawn(program, ['--port', '0', root], { stdio: ['ignore', 'ignore', 'pipe'] });
  return new Promise((resolve, reject) => {
    let text = '';
    server.stderr.on('data', (chunk) => {
      text += chunk;
      const address = /http:\/\/\S+/.exec(text);
      if (address) resolve({ server, address: address[0] });
    });
    server.on('error', reject);
    server.on('exit', (code) => reject(new Error('vs-gui ended with ' + code + ': ' + text)));
  });
}

async function launch(chromium) {
  // WebGL without a graphics card, for the 3D view.
  const args = ['--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'];
  if (process.env.VOXELSIEVE_BROWSER) {
    return chromium.launch({ args, executablePath: process.env.VOXELSIEVE_BROWSER });
  }
  try {
    return await chromium.launch({ args });
  } catch {
    return chromium.launch({ args, channel: 'chrome' });
  }
}

/// The share of bright pixels in the picture of `canvas`, as the browser shows it. The part is
/// bright in every view, the background dark.
async function brightShare(page, canvas) {
  const png = (await canvas.screenshot()).toString('base64');
  return page.evaluate(async (data) => {
    const image = new Image();
    image.src = 'data:image/png;base64,' + data;
    await image.decode();
    const copy = document.createElement('canvas');
    copy.width = image.width;
    copy.height = image.height;
    const context = copy.getContext('2d');
    context.drawImage(image, 0, 0);
    const pixels = context.getImageData(0, 0, copy.width, copy.height).data;
    let bright = 0;
    for (let i = 0; i < pixels.length; i += 4) {
      if (pixels[i] + pixels[i + 1] + pixels[i + 2] > 3 * 120) bright += 1;
    }
    return bright / (pixels.length / 4);
  }, png);
}

async function walk(page, address, work) {
  const stage = page.locator('#stage');
  const steps = page.locator('#protocol > li');
  // The end of an operation: `locator` appears, or the page shows the error of the step.
  const finished = async (locator) => {
    const error = page.locator('#error');
    const options = { timeout: 600000 };
    const waits = [locator.waitFor(options), error.waitFor(options)];
    for (const wait of waits) wait.catch(() => {});  // the one that does not happen
    await Promise.race(waits);
    expect(await error.isHidden(), 'the step runs: ' + await error.textContent());
  };

  await page.goto(address);
  // 1. Open a scan (ADR 0039): the sample with its sidecar opens at once, in a project next to it,
  // and shows the part in 3D with the analyses that suit it.
  await page.locator('#welcome-scan').click();
  await page.locator('#browser-list > li', { hasText: 'housing.raw' }).dblclick();
  await finished(stage.locator('.next-step').first());
  expect((await page.locator('#project-name').textContent()).startsWith('housing'),
    'the project of the scan is open');
  expect(await steps.count() === 1, 'the import is the first step of the protocol');
  await finished(stage.getByText('drag rotates'));
  await expectPicture(page, stage.locator('canvas:not(.transfer)').first(), 'the first 3D view');
  const suggested = await stage.locator('.next-steps').innerText();
  expect(/Porosity analysis/.test(suggested), 'porosity is suggested, in: ' + suggested);
  expect(/Nominal-actual comparison/.test(suggested),
    'the comparison with the CAD model next to the scan is suggested, in: ' + suggested);

  // 2. Analysis: the porosity of the part, run from its suggestion; the scan is synthetic, so the
  // answer is known. Its pores become layers of the workspace (ADR 0040).
  await stage.locator('.next-step', { hasText: 'Porosity analysis' })
    .getByRole('button', { name: 'Run', exact: true }).click();
  await finished(stage.locator('.layer', { hasText: 'Pores' }));
  const porosity = stage.locator('details.analysis', { hasText: 'Porosity analysis' });
  await porosity.locator('> summary').click();
  await porosity.getByText('Result of step').waitFor();
  const figures = await porosity.locator('.analysis-result .figures').innerText();
  expect(/pores\s+3\b/i.test(figures), 'three pores, in: ' + figures);
  expect(/zones\s+1\b/i.test(figures), 'one loosened zone, in: ' + figures);
  await stage.locator('.layer', { hasText: 'Pores' }).locator('.layer-name').click();
  expect(await stage.locator('.layer-properties tbody tr').count() === 3,
    'the properties of the pores list three of them');

  // 3. History: the analysis is listed.
  await stage.locator('.side-tabs').getByRole('button', { name: 'History', exact: true }).click();
  await stage.getByText('Pores and loosened zones').first().waitFor();

  // 4. Report: with the order of the examples, the part passes.
  await page.locator('.stages button[data-stage="report"]').click();
  await stage.locator('input[name="order_path"]').fill(path.join(work, 'inspection_order.json'));
  await stage.getByRole('button', { name: 'Create report' }).click();
  await finished(stage.locator('iframe.report'));
  const report = page.frameLocator('iframe.report');
  await report.locator('h1').first().waitFor();
  expect((await report.locator('body').innerText()).length > 500, 'the report has its text');
  await report.locator('img').first().waitFor();  // the report has its pictures

  // 4b. A region (ADR 0041), after the report, which takes the latest porosity: a box drawn in the slice view around two of the pores (lunkers at
  // voxels x 175 and 179, y 32 and 73; the third at x 141, y 126 lies outside), then the porosity
  // in it.
  await page.locator('.stages button[data-stage="workspace"]').click();
  await stage.getByRole('button', { name: 'New region' }).click();
  const sliceCanvas = stage.locator('.viewer canvas');
  await sliceCanvas.waitFor();
  await page.waitForFunction(() => state.viewer?.info && state.viewer.canvas?.clientWidth > 0);
  await page.waitForTimeout(500);  // the view fits the slice on its first frame
  const frame = await sliceCanvas.boundingBox();
  const toScreen = (u, v) => page.evaluate(([u, v]) => {
    const viewer = state.viewer;
    const [su, sv] = viewer.stretch();
    return [(u - viewer.center[0]) * viewer.zoom * su + viewer.canvas.clientWidth / 2,
      (v - viewer.center[1]) * viewer.zoom * sv + viewer.canvas.clientHeight / 2];
  }, [u, v]);
  const [x0, y0] = await toScreen(160, 18);
  const [x1, y1] = await toScreen(200, 92);
  await page.mouse.move(frame.x + x0, frame.y + y0);
  await page.mouse.down();
  await page.mouse.move(frame.x + x1, frame.y + y1, { steps: 8 });
  await page.mouse.up();
  await stage.locator('ul.solids li', { hasText: 'Box' }).waitFor();
  await stage.getByRole('button', { name: 'Save region' }).click();
  await finished(stage.locator('.layer', { hasText: 'Region 1' }));
  await stage.getByRole('button', { name: 'Analyse here' }).click();
  const inRegion = stage.locator('details.analysis', { hasText: 'Porosity analysis' });
  await inRegion.getByText('Runs in the region Region 1').waitFor();
  await inRegion.getByRole('button', { name: 'Run', exact: true }).click();
  await finished(inRegion.locator('.analysis-result', { hasText: 'Region 1' }));
  const regionFigures = await inRegion.locator('.analysis-result .figures').innerText();
  expect(/pores\s+2\b/i.test(regionFigures), 'two pores in the region, in: ' + regionFigures);

  // 5. View: a slice, then the volume in 3D.
  await page.locator('.stages button[data-stage="workspace"]').click();
  await stage.locator('.view-modes').getByRole('button', { name: 'Slice', exact: true }).click();
  const slice = stage.locator('canvas').first();
  await slice.waitFor();
  await expectPicture(page, slice, 'the slice view');
  await stage.locator('.view-modes').getByRole('button', { name: '3D', exact: true }).click();
  await finished(stage.getByText('drag rotates'));
  // The porosity result showed the part translucent, for the pores inside; as a solid again.
  await stage.locator('.layer', { hasText: 'Grey values' }).click();
  await stage.locator('select[title="How the grey values are drawn"]').selectOption('0');
  await expectPicture(page, stage.locator('canvas:not(.transfer)').first(), 'the 3D view');

  // 5b. A look in one click (ADR 0043), and a short video of it, downloaded.
  // The video settings are read when the view is drawn anew, as the look does.
  await page.evaluate(() => localStorage.setItem('voxelsieve.video',
    JSON.stringify({ motion: 'turntable', seconds: 0.5, fps: 8, size: 'view' })));
  const view3d = stage.locator('canvas:not(.transfer)').first();
  const plain = await view3d.screenshot();
  await stage.locator('.looks button[data-look="gold"]').click();
  let changed = false;
  for (let attempt = 0; attempt < 60 && !changed; attempt += 1) {
    await page.waitForTimeout(500);
    changed = !(await view3d.screenshot()).equals(plain);
  }
  expect(changed, 'the look "Gold" changes the picture');
  const download = page.waitForEvent('download', { timeout: 600000 });
  await stage.getByRole('button', { name: 'Record video' }).click();
  const video = await download;
  const videoFile = path.join(work, 'video-' + video.suggestedFilename());
  await video.saveAs(videoFile);
  expect(/\.(mp4|webm)$/.test(videoFile) && fs.statSync(videoFile).size > 1000,
    'the video is downloaded: ' + videoFile);

  // 6. A raw file without a sidecar: its layout is detected and shown for a check.
  await page.locator('.stages button[data-stage="dataset"]').click();
  await stage.getByRole('button', { name: 'Open another scan' }).click();
  await page.locator('#browser-list > li', { hasText: 'nosidecar' }).click();
  await page.locator('#browser-list > li', { hasText: 'casting.raw' }).dblclick();
  const check = stage.locator('.scan-check');
  await check.waitFor();
  const size = await check.locator('input[type=number]').evaluateAll(
    (inputs) => inputs.slice(0, 3).map((input) => input.value).join(' x '));
  expect(size === '208 x 160 x 88', 'the size is detected in the grey values: ' + size);
  await check.getByRole('button', { name: 'Cancel', exact: true }).click();
  await check.waitFor({ state: 'detached' });

  // Undo takes the last step back, redo brings it again.
  const done = await steps.count();
  await page.locator('#btn-undo').click();
  // The protocol starts hidden on screens narrower than 1600 pixels; show it.
  if (await page.locator('body.protocol-hidden').count()) await page.locator('#btn-protocol').click();
  await page.locator('#protocol > li.undone').first().waitFor();
  await page.locator('#btn-redo').click();
  await page.locator('#protocol > li.undone').first().waitFor({ state: 'detached' });
  expect(await steps.count() === done, 'undo and redo keep the steps');
  expect(await page.locator('#error').isHidden(), 'no error is shown: ' +
    await page.locator('#error').textContent());
}

/// Waits until `canvas` shows the part: neither an empty nor a flooded picture.
async function expectPicture(page, canvas, what) {
  let share = 0;
  for (let attempt = 0; attempt < 60 && share < 0.02; attempt += 1) {
    await page.waitForTimeout(500);
    share = await brightShare(page, canvas);
  }
  expect(share >= 0.02 && share <= 0.7,
    what + ' shows the part (' + (100 * share).toFixed(1) + ' % bright pixels)');
}

async function main() {
  const [gui, synth, order, workArgument] = process.argv.slice(2);
  const work = path.resolve(workArgument);
  fs.rmSync(work, { recursive: true, force: true });
  fs.mkdirSync(work, { recursive: true });
  // The sample data of a release (packaging/make-samples.cmake).
  const made = spawnSync(synth, ['--part', 'housing', '--voxel-size', '0.25', '--lunker', '3',
    '--loosening', '1', '--noise', '400', '--blur', '0.1', '--cupping', '0.05', '--stl',
    'housing.stl', '--out', 'housing'], { cwd: work, stdio: 'ignore' });
  expect(made.status === 0, 'vs-synth makes the sample');
  // The server reads only below its directory, so the order goes there too.
  fs.copyFileSync(order, path.join(work, 'inspection_order.json'));
  // The scan once more without its sidecar, as many scanners write it.
  fs.mkdirSync(path.join(work, 'nosidecar'));
  fs.copyFileSync(path.join(work, 'housing.raw'), path.join(work, 'nosidecar', 'casting.raw'));

  const { chromium } = loadPlaywright();
  const { server, address } = await startServer(gui, work);
  let browser = null;
  try {
    browser = await launch(chromium);
    const page = await browser.newPage({ viewport: { width: 1400, height: 1000 } });
    const errors = [];
    page.on('pageerror', (error) => errors.push(String(error)));
    page.on('console', (message) => {
      // A browser asks for an icon of the page, which the server does not have.
      const url = message.location().url ?? '';
      if (message.type() === 'error' && !url.endsWith('/favicon.ico')) {
        errors.push(message.text() + (url ? ' (' + url + ')' : ''));
      }
    });
    // The 3D view draws with software rendering here; on a slow runner one frame with shadows,
    // or the pictures of the looks, keeps the page from taking a click for over a minute.
    page.setDefaultTimeout(300000);
    try {
      await walk(page, address, work);
    } catch (error) {
      await page.screenshot({ path: path.join(work, 'failure.png'), fullPage: true })
        .catch(() => {});
      throw new Error(error.message + (errors.length ? '\nErrors of the page:\n' +
        errors.join('\n') : ''));
    }
    expect(errors.length === 0, 'the page reports no errors:\n' + errors.join('\n'));
  } finally {
    if (browser) await browser.close();
    server.removeAllListeners('exit');
    server.kill();
  }
  console.log('The walk through the browser UI went as expected');
}

main().catch((error) => {
  console.error(error.message);
  process.exit(1);
});
