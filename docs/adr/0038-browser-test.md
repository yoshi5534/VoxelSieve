# 0038: A browser test of the UI, with Playwright as a test tool

Status: accepted (2026-11-23)

## Context

The browser UI in `resources/ui` is plain JavaScript without a build step. Node tests cover its
modules (tile cache, transfer function, brick streaming), and the operations behind it have their
own tests, but nothing clicked through the page: a broken wizard, file browser or 3D view passed
every test (`docs/review-2026-11-b.md`, F5). Such a test needs a browser that a program drives,
which is a new dependency (ADR 0016 asks for a record of each).

## Decision

**One test walks through the inspection of the sample part in a headless browser**
(`tests/ui/smoke_test.cjs`, CTest `ui_smoke`): a new project by the file browser, the import, the
porosity analysis, the report, the slice view and the 3D view, undo and redo. It checks the
known answers of the synthetic scan, that both views show the part, and that the page reports no
error.

- The browser is driven with **Playwright** (`playwright-core`, Apache-2.0), installed with npm
  in the release job of CI at a pinned version. It is a tool of this one test: nothing of it is
  built into or shipped with VoxelSieve, and it is not in `vcpkg.json`.
- No browser is downloaded in CI: the test uses the Chrome of the runner. Elsewhere it takes
  the browser of a full `playwright` package, or the program in `VOXELSIEVE_BROWSER`.
- CMake adds the test only where node finds the package at configure time, so a build without
  it loses nothing but this test. To run it locally: `npm install playwright-core` somewhere,
  point `NODE_PATH` at its `node_modules`, configure again.
- It stays one walk. Behaviour is tested at the operations and the API (the UI gets its features
  from API methods), not by more browser tests.

## Consequences

- A change that breaks the path a first user takes through the UI fails CI, on Linux.
- The 3D view renders with the software WebGL of the headless browser; the test asks only that
  the part is in the picture, not how it looks.
- The pinned version needs a raise now and then, when the Chrome of the runners outgrows it.
