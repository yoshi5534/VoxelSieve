// The inspection of the sample part (docs/quickstart.md) in a browser, as a user clicks through
// it: a new project, the import, the porosity analysis, the report, the slice and the 3D view,
// undo and redo. The scan is synthetic, so the answers are known: three shrinkage cavities and
// one zone of loosened microstructure. Nothing else clicks through resources/ui.
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
  await page.locator('#welcome-new').click();
  // The file browser opens in the directory the server was given; the project gets a name there.
  await page.locator('#browser-list > li').first().waitFor();
  await page.locator('#browser-name').fill('smoke');
  await page.locator('#browser-ok').click();
  await page.locator('#wizard').waitFor();
  expect((await page.locator('#project-name').textContent()).startsWith('smoke'),
    'the new project is open');

  // 1. Dataset: the raw file of the sample, imported with the values of its sidecar.
  await stage.getByRole('button', { name: 'Choose a dataset' }).click();
  await page.locator('#browser-list > li', { hasText: 'housing.raw' }).dblclick();
  await stage.getByRole('button', { name: 'Import', exact: true }).click();
  await finished(stage.getByText('Current dataset'));
  expect(await steps.count() === 1, 'the import is the first step of the protocol');

  // 2. Analysis: the porosity of the part, known from how the scan was made.
  await page.locator('.stages button[data-stage="analysis"]').click();
  const porosity = stage.locator('.card', {
    has: page.getByRole('heading', { name: 'Porosity analysis', exact: true }) });
  await porosity.getByRole('button', { name: 'Run', exact: true }).click();
  await finished(porosity.getByText('Result (step'));
  const figures = await porosity.locator('table').last().innerText();
  expect(/pores\s+3\b/i.test(figures), 'three pores, in: ' + figures);
  expect(/zones\s+1\b/i.test(figures), 'one loosened zone, in: ' + figures);

  // 3. Results: the analysis is listed.
  await page.locator('.stages button[data-stage="results"]').click();
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

  // 5. View: a slice, then the volume in 3D.
  await page.locator('.stages button[data-stage="view"]').click();
  const slice = stage.locator('canvas').first();
  await slice.waitFor();
  await expectPicture(page, slice, 'the slice view');
  await stage.locator('.view-modes').getByRole('button', { name: '3D', exact: true }).click();
  await finished(stage.getByText('drag rotates'));
  await expectPicture(page, stage.locator('canvas:not(.transfer)').first(), 'the 3D view');

  // Undo takes the report back, redo brings it again.
  const done = await steps.count();
  await page.locator('#btn-undo').click();
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
    page.setDefaultTimeout(60000);
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
