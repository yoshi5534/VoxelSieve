# Review: correctness, robustness and consistency after v0.7.0 (November 2026)

A second review of the code base (`main` at `58577d8`, v0.7.0 plus #119–#121). The first one,
[review-large-data-2026-11.md](review-large-data-2026-11.md), asked what breaks when a scan is much
larger than RAM; its phases 0–7 are done and are not repeated here. This one asks: where can a
user lose work, get a wrong inspection result without notice, or get stuck, and what makes the
code harder to keep correct? It ends in a plan to choose from. Nothing here is fixed yet, except
three stale texts (section G).

## Method

- Reading the code along the paths a result takes: operation → project step → files → report,
  and the GUI/MCP layer around it.
- Black-box runs of the v0.7.0 Linux release package: the command-line tools and `vs-gui --mcp`
  driven over stdio, with synthetic parts (`vs-synth`, `vs-phantom`), a hand-made DICOM series and
  damaged files.

Marks on each finding:

- **[T]** reproduced with the release package; the steps are in the appendix.
- **[V]** read in the cited code, not run.
- **[L]** a lead, not checked further.

Effort: **S** up to 1 day, **M** 2–5 days, **L** 1–3 weeks.

## Summary

The operation layer, the server and the file writers are in good shape (see "What is good").
The serious findings sit in two places:

1. **The project around a step.** A project save that fails leaves the GUI stuck for good (A1), a
   second `vs-gui` on the same project deletes the output of a running step (A3), an interrupted
   import in a project starts from zero (A4), and a mistyped path in the next step deletes undone
   results (A5).
2. **Files that say what to inspect.** A misspelled key in an inspection order drops a limit and
   the part passes (B1).

Five findings are of high or critical severity (A1–A4, B1); four of them were reproduced, A4
was read in the code.

## 1. Findings

### A. Lost work and stuck states

| id | finding | where | sev | mark | effort |
|----|---------|-------|-----|------|--------|
| A1 | **A failed project save leaves the GUI stuck.** After a step, `executeRun` installs the working copy, which saves the project. When that save throws, `releaseRun()` is never reached: every later call answers "Operation '…' is still running", `cancel` does nothing, and only a restart helps. On disk the step is still `running`, so the next open removes its output. Triggers: a full disk while `project.json` is written, or text that is not UTF-8 in a step summary (A2). | `src/gui.cpp:2188-2211`, `src/project.cpp:718-723` | **critical** | [T] | S |
| A2 | **Text from input files reaches JSON unchecked.** A DICOM or DICONDE series in Latin-1 (`ISO_IR 100`) with a component name such as "Gehäuse" ends the import, after all its work, with `[json.exception.type_error.316] invalid UTF-8 byte`, and then A1. The strings are taken as raw bytes and `writeJson` dumps strictly. File names with invalid bytes and TIFF descriptions can take the same path. | `src/diconde.cpp:122`, `src/builtin_operations.cpp:529`, `src/io.cpp:188` | high | [T] | S–M |
| A3 | **A second process deletes the output of a running step.** `Project::open` takes every step with status `running` for an interrupted one, marks it failed and removes its directory. There is no lock between processes. `vs-gui --mcp` reopens the project used last, so starting an AI client while the browser UI runs an import is enough: the import then fails with "Cannot write …/level0/1_0_0.vdb". | `src/project.cpp:161-168`, `src/gui.cpp:853` | high | [T] | M |
| A4 | **An interrupted import in a project never resumes.** The journal that lets `vs-sieve` resume is removed with the step directory when the project is opened again. A 300 GB import that was killed at 90 % starts from zero in the GUI. | `src/project.cpp:164-167`, `src/dataset.cpp:1112` | high | [V] | M |
| A5 | **A new step discards the undone steps before it has succeeded.** Undo `compare_cad`, then run it again with a mistyped CAD path: the step fails with "Cannot open …", and the output of the undone step is already deleted, so redo has nothing left. With an undone import that is hours of work. | `src/project.cpp:509-513` | medium | [T] | S–M |
| A6 | **The resume key does not identify the input.** It holds the dimensions and the options, not the file. Importing another scan of the same size into the output directory of an interrupted import resumes it and mixes the bricks of two scans. Series inspections have exactly this: many scans of one size. | `src/dataset.cpp:838-850` | medium | [V] | S |
| A7 | **Files are renamed into place without being flushed to disk.** A crash of the process is covered; a power cut can leave `project.json` or `index.json` empty, or a brick log that lists bricks whose data never reached the disk. | `src/io.cpp:153-186` | low | [V] | S |

### B. Inspection results that can be wrong without notice

| id | finding | where | sev | mark | effort |
|----|---------|-------|-----|------|--------|
| B1 | **Unknown keys in orders and plans are ignored.** With `max_pore_size_mm: 0.3` the sample fails; with `max_pore_size: 0.3` the zone has no limit and passes. With `acceptence` instead of `acceptance` the report has no evaluation at all, and no warning. The order code looks keys up one by one (113 lookups in `report.cpp`) and never checks the rest. The same holds for regions, measurement plans and battery orders. | `src/report.cpp`, `src/region.cpp`, `src/dimensional_plan.cpp` | high | [T] | M |
| B2 | **The command-line tools accept values the operations reject.** `vs-porosity --min-pore -5 --zone-sigma -3` runs and writes a result; `run_porosity` refuses both. The ranges exist only in the operation schemas. | `apps/vs-porosity/main.cpp:50-54`, `src/builtin_operations.cpp:725` | medium | [T] | with D1 |
| B3 | `segment_materials` suggests two materials for the sample housing, which has one (seen in the MCP test of the release on 2026-11-22; edge and loosening voxels form the second peak). | `src/materials.cpp:429` | low | [L] | M |

### C. Inputs and the directory boundary

| id | finding | where | sev | mark | effort |
|----|---------|-------|-----|------|--------|
| C1 | **The voxel count of a raw volume is multiplied without an overflow check.** With `dims` 2³² × 2³² × 1 the product wraps to 0, the size check passes ("header 11440000 bytes skipped"), `vs-sieve` ends with `std::bad_alloc` and `vs-gui --mcp` dies with a segmentation fault, taking the session with it. With 10⁸ per axis the message names a nonsense byte count. | `src/source.cpp:150` | medium | [T] | S |
| C2 | **Files named inside files are read outside the allowed directories.** The boundary checks parameters marked as paths. A region of an inspection order (also an inline one) that names an STL outside is read and copied into the project, where `read_file` returns it. A gzipped XML project that names its volume files by absolute path is followed the same way. | `src/region.cpp:374`, `src/gzipped_xml.cpp:159`, `src/gui.cpp:2214` | medium | [T] | S–M |
| C3 | **Later steps read the CAD model again from where it once was.** `compare.json` stores the absolute path; `wall_thickness` with nominal thickness and `measure_dimensions` then fail with "Cannot open …" once the file has moved, and silently measure against another model when the file has changed. A project copied to another machine loses these steps. `add_mesh` already copies its STL into the project. | `src/builtin_operations.cpp:1350`, `:3035` | medium | [T] | S–M |
| C4 | Damaged datasets give raw messages instead of naming the file: "cannot create std::vector larger than max_size()", "std::bad_alloc", "[json.exception.out_of_range.403] key 'voxel_size_mm' not found". None of the damaged files crashed a tool. | `src/dataset_reader.cpp` | low | [T] | S |
| C5 | Binary STL vertices are not checked for NaN or infinity; a `.vss` file with damaged header bytes is accepted. | `src/mesh.cpp:27`, `src/surface.cpp` | low | [L] | S |

### D. Maintainability

| id | finding | where | effort |
|----|---------|-------|--------|
| D1 | **Every parameter is written down three times:** the default in the option struct, the default and range in the operation schema (91 defaults in `builtin_operations.cpp`), and the parsing and help text of the tool (`apps/*/main.cpp`, 3700 lines). Only the sieve shares one definition (`sieveParameters`). B2 is the visible result. | [V] | L |
| D2 | `builtin_operations.cpp` holds 30 operations in 3570 lines. `gui.cpp` (2564), `report.cpp` (2227) and `resources/ui/app.js` (2749) follow. | [V] | M |
| D3 | **A missing surface is handled in two ways.** `wall_thickness` extracts a private surface into its own output, which no later step can use and which every run repeats. `compare_cad`, `compare_objects`, `align_surfaces` and `measure_dimensions` fail with "No surface available for input 'surface'". | `src/builtin_operations.cpp:1484-1491`, `src/project.cpp:474` [T] | M |
| D4 | `report` needs a porosity result. An inspection of deviations or wall thickness alone gets no report. | `src/builtin_operations.cpp:1758` [T] | S–M |
| D5 | **The plugin interface is a C++ ABI** (`std::function`, `nlohmann::json` in `OperationContext`) guarded by one version number. A plugin built with another compiler, standard library or JSON version is loaded and then crashes. The release packages contain no headers, so a plugin can only be built from a checkout of the same tag. | `include/voxelsieve/operation.hpp:60-84` [V], package listing [T] | S–M |

### E. API and MCP

| id | finding | where | effort |
|----|---------|-------|--------|
| E1 | **`project_status` returns everything, indented.** A project of six steps answers with 34 KB: every step with all parameters (an inline order is repeated in each report step) and its telemetry. `project_open`, `undo` and `redo` answer the same. An AI client pays for this on every call, and it grows with every step. | `src/gui.cpp:888` [T] | S–M |
| E2 | A step that failed stays in the protocol with `"active": true`. Steps that fail on a check a moment after they start (file not found, bad dimensions) are stored for good and use up object ids. A cheap check before the step is created would also remove most cases of A5. | `src/project.cpp:428` [T] | M |
| E3 | No tool has `--version`. A build from source calls itself 0.3.1, also as the MCP server version. | `CMakeLists.txt:5`, `vcpkg.json:4` [T] | S |
| E4 | The UTF-8 manifest covers VoxelSieve's programs on Windows, not `python.exe`: paths with umlauts in the Python package are untested. | `resources/windows/utf8.manifest` [L] | S |

Operation ids mix nouns and verbs (`porosity`, `surface`, `segment_materials`, `move`). They are
stored in project files and used by the plugin's skills; renaming them costs more than it gives.

### F. Tests and CI

- **F1. No test makes a write fail.** None of A1–A5 was caught, because no test saves into a
  directory that refuses the write, opens one project twice, or interrupts a step of a project.
- **F2. The own parsers are not fuzzed:** STL, `.vss`, the import journal, raw sidecars, orders,
  plans, gzipped XML projects. Clang's libFuzzer needs no new dependency.
- **F3. No test checks that orders are strict** (comes with B1): change each key of the example
  orders and expect an error.
- **F4. MCP is tested end to end only by hand.** CTest runs `tools/list` and `browse` through the
  stdio server; the full walk through all tools was done manually after the release.
  `battery_images` has no test at operation level.
- **F5. Nothing clicks through the browser UI.** The node tests cover its modules. One browser
  smoke test (import the sample, porosity, report, the 3D view shows a picture) would cover the
  wizard. It needs a headless browser in CI as a development dependency.
- **F6.** Pull requests that change only documents run the whole CI.

### G. Documentation

- **G1. `CHANGELOG.md` has had no version heading since 0.2.1:** 412 lines stand under
  "Unreleased", and 0.3.0 to 0.7.0 are missing. [V]
- **G2. The texts about the server contradicted the code.** `README.md`, `SECURITY.md` and the
  help of `vs-gui` said the server has no authentication; since phase 5 it requires the access
  token of each start (`src/http_server.cpp:270`). **Corrected in this pull request** for the two
  documents; the help text (`apps/vs-gui/main.cpp:59`) still says it, and it lists four of the
  six prompts (`:48`).
- **G3. `README.md` is the manual:** 756 lines, beside the quickstart and the Python page.
- **G4. Orders and plans have no reference.** Their keys are documented only by the examples.
- **G5.** ADR 0002 called the integer grids "proposed" although ADR 0021 is accepted.
  **Corrected in this pull request.**

### What is good

- Operations validate their parameters: unknown names, types, ranges and enumerations are
  refused with a message that names the parameter. [T]
- The HTTP server checks the Host header and a token per start, limits bodies and connections;
  report templates and the UI escape what they insert. [V]
- Truncated and damaged surface files, bricks and datasets end in an error, not in a crash. [T]
- Result files go through a temporary file; steps run on a copy of the project while status and
  views are answered; MCP calls can be cancelled and report progress. [V]
- Every operation but `battery_images` has a test, and a failing import leaves the project
  usable. [V][T]

## 2. Plan

Six steps, each a small number of pull requests, in the order I recommend. Steps 1 and 2 remove
every finding of high or critical severity but A4 (step 3) and take about two weeks together.

### Step 1: A project never gets stuck and never loses a running step (about 1 week)

1. **A1:** release the run in a scope guard, so no exception can skip it. A save that fails
   after a step keeps the step in memory, reports the error and tries again on the next change.
2. **A2:** convert DICOM text to UTF-8 with DCMTK's own character set conversion when it is read
   (the port is built without optional features today; if the conversion needs one, that is a
   short ADR). As a net for every other source, `writeJson` replaces invalid bytes instead of
   throwing.
3. **A3:** a lock file in the project directory, held with an advisory file lock
   (Boost.Interprocess is header-only and one more port of the Boost already in use). Only the
   holder cleans up interrupted steps. A
   second process gets "the project is open in another VoxelSieve" and may open it read-only.
4. **A5:** undone steps are removed when the new step is done, not when it starts.
5. **F1:** tests for each: a save that fails, two openers, non-UTF-8 text, a failing step after
   undo.

**Status (2026-11-22): done** (ADR 0036). The second process gets the error and does not open the
project; opening read-only is left out until something needs it.

### Step 2: Orders are strict; dimensions are checked (about 1 week)

1. **B1, G4, F3:** every key of an order, a region, a plan and a battery order is known or an
   error that names it and the nearest known key. A zone without a criterion is an error. One
   JSON schema per file type in `docs/`, used by the check, the documentation and editors.
2. **C1:** checked multiplication for dimensions wherever they come from a parameter or a file.
3. **C4:** `Dataset::open` checks `index.json` and names the file in its message.

**Status (2026-11-22): done.** The schemas are `docs/schema/inspection_order.schema.json` and
`docs/schema/inspection_plan.schema.json`. The check keeps its key lists in the code where the
keys are read (a schema validator would be a new dependency); a test compares them with the
schemas in both directions. The sections of an order that only describe the inspection stay free,
because report templates show whatever they hold.

### Step 3: Projects are self-contained and imports resume (1–2 weeks)

1. **A4:** an interrupted import keeps its directory; the step is offered as "resume" and runs
   again with the same parameters into the same directory.
2. **A6:** the resume key gets the identity of the input (path, size, modification time).
3. **C3:** `compare_cad` and `measure_dimensions` copy the CAD model into their output and later
   steps read that copy.
4. **C2:** operations resolve every file named inside a file through one function of the
   context, which applies the directory boundary.
5. **A7:** flush `project.json`, `index.json` and the journal before the rename.

**Status (2026-11-23): done** (ADR 0037). Only the imports resume; the thickness map has a
journal too and can follow the same way.

### Step 4: The API answers briefly and treats prerequisites one way (about 1 week)

1. **E1:** over MCP, the status lists steps as id, operation, status, outputs and summary, without
   indentation; a `step` method returns one step in full.
2. **D3:** the project runs a missing prerequisite as a step of its own, visible in the protocol
   and reusable, and names it in the answer (`preceded_by`, as `followed_by` exists). First for
   the surface of all five operations; `wall_thickness` loses its private surface.
3. **D4:** the porosity input of `report` becomes optional.
4. **E2:** operations get a cheap check that runs before the step is created; failed steps are
   not active.
5. **E3:** `--version` in `cli::runTool`; the version of source builds from `git describe`.

**Status (2026-11-23): done.** Prerequisites are declared per input (`PortInfo::made_by`) and
run by `Project::run`, so the Python package gets them too; `align_surfaces` reads the surfaces
of objects, not an input, and still asks for a surface step. The check before the step is
`Operation::check`. The help of `vs-gui` (G2) is corrected with it.

### Step 5: One definition per parameter (2–3 weeks)

1. **D1, B2:** the tools build their options, ranges and help from the operation schema, as
   `vs-sieve` does for the sieve. The option structs keep the defaults; a test compares them with
   the schema.
2. **D2:** split `builtin_operations.cpp` by domain (import, porosity and report, surface and
   comparison, materials, objects, battery). Mechanical, best right after 5.1.
3. **D5:** ship the headers and a CMake package file in the release packages; the plugin
   handshake also compares a build string (compiler, standard library, JSON version).

**Status (2026-11-23): done.** The options that set a parameter of an operation
read it with `cli::Arguments::number(operation, parameter)` and take its limits from the schema;
the help texts stay written by hand. `exclusiveMinimum`, which the schemas used and nothing
checked, is checked. The build text is `VOXELSIEVE_PLUGIN_BUILD` (standard library, JSON and
OpenVDB version; plugin API 7). The operations are in `src/operations/` (import, inspection, surface,
materials, objects, battery; `common.hpp` holds what they share), moved without a change.

### Step 6: Tests that would have found this (about 1 week, any time)

1. **F4:** the manual MCP walk of 2026-11-22 becomes a CTest on the sample part.
2. **F2:** fuzz targets for the own parsers in the ASan preset, a few minutes each in the nightly.
3. **F5:** one browser smoke test of the wizard. Needs a yes for the development dependency.
4. **F6:** skip the build jobs for pull requests that touch only documents.

**Status (2026-11-23): 1, 2 and 4 done, 3 open.** The walk is `tests/mcp_walk.py` (CTest
`mcp_walk`): the program over standard input and output, one request after the answer to the one
before. For F2 the readers of the own formats (STL, surface mask, model, dataset index, sidecar,
project file, gzipped XML project) read some hundred damaged copies of a valid file in every test
run (`tests/damaged_files.hpp`), also under ASan; that needs no second compiler, which fuzz
targets with libFuzzer would. The browser test (F5) waits for the yes to its dependency.

### Small things for any pull request

- **G1:** split the changelog by the release tags, and let the release workflow fail when the
  tag has no heading. (S)
  *Done:* the changelog is split by the tags. The workflow warns instead of failing: the tag of
  a published release cannot be moved to a commit with the heading.
- **G2:** the two texts left in the help of `vs-gui`. (S)
- **G3:** move the tool chapters of the README into pages under `docs/`. (M)
- **C5, E4, B3** as they come up.

## 3. Not recommended

- **Renaming operation ids** for consistency. Projects and the plugin's skills use them.
- **A stable C ABI for plugins.** One example plugin exists; shipping headers and comparing a
  build string (D5) is enough until others write plugins.
- **A database for projects.** The JSON protocol with atomic saves is adequate once one process
  holds a lock (A3).

## Appendix: how the [T] findings were reproduced

All with `VoxelSieve-0.7.0-linux-x64.tar.gz`, `vs-gui --mcp` driven by JSON-RPC lines on stdin.

- **A1, A2:** a DICOM series of 48 slices with `SpecificCharacterSet = ISO_IR 100` and
  `PatientName = Gehäuse`; `project_create`, `run_import_dicom` → `type_error.316`; then
  `run_surface` → "Operation 'import_dicom' is still running".
- **A3:** process 1: `run_denoise` on a 512³ solid phantom with `--threads 1`; process 2:
  `project_open` of the same project after 10 s → `steps/2-denoise` is gone, process 1 answers
  "Cannot write …/2-denoise/dataset/level0/1_0_0.vdb".
- **A5:** `run_import_raw`, `run_surface`, `run_compare_cad`, `undo`,
  `run_compare_cad {cad_path: typo.stl}`, `redo` → the project's `steps/` holds only the import
  and the surface.
- **B1:** `run_report` with an inline order whose only zone has `max_pore_size_mm: 0.3` →
  `"passed": false`; with `max_pore_size: 0.3` → `"passed": true`; with `acceptence` → no
  evaluation and no error.
- **B2:** `vs-porosity ds --out p --min-pore -5 --zone-sigma -3` → exit code 0.
- **C1:** `vs-sieve part.raw --dims 4294967296 4294967296 1 --out d` → `std::bad_alloc`;
  `run_import_raw` with the same `dims` → the server ends with signal 11.
- **C2:** `vs-gui --mcp <allowed>`; `run_inspection_regions` with a region
  `{"mesh": "<outside>/secret.stl"}` → `regions/zone_3.stl` has the bytes of the outside file.
- **C3:** `run_compare_cad`, delete the STL, `run_wall_thickness {nominal_thickness: true}` →
  "Cannot open …".
- **C4:** `index.json` with `dims` of 10¹² per axis, or without `voxel_size_mm`; a brick cut to
  3000 bytes; a `.vss` cut to 20 kB.
- **D3, D4:** `run_compare_cad` and `run_report` in a project with only an import.
- **E1:** `project_open` of a project with import, porosity and four reports → 33 638 bytes.
