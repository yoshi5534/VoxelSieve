# AI assistants over MCP

`vs-gui --mcp` serves the GUI over the Model Context Protocol (stdio), so an AI assistant can
drive VoxelSieve: create or open a project, run operations, undo and redo, and read results. It
runs on your machine: scans stay where they are, and the assistant only receives results such as
pore lists, measurements, slice images and reports.

**Install.** Every [release](https://github.com/yoshi5534/VoxelSieve/releases) has packages for
Linux (x64), Windows (x64) and macOS (Apple silicon), built from the same sources:

- `voxelsieve-<version>-<platform>.mcpb`: an MCP bundle. Open it with Claude Desktop (double-click it, or
  drag it onto the Extensions page of the settings) and choose the directories with your scans.
- `voxelsieve-<version>-<platform>.tar.gz` / `.zip`: all tools with their libraries. Unpack it
  anywhere; nothing else needs to be installed.
- `voxelsieve-<version>-samples.zip`: a synthetic scan of a cast housing with its CAD model,
  inspection order and the ground truth of its defects, for the [quickstart](quickstart.md).
- `ghcr.io/yoshi5534/voxelsieve`: a Docker image built from the Linux package with
  `packaging/Dockerfile`: the browser UI by default, MCP with `--mcp` (see the
  [quickstart](quickstart.md#docker)).

The packages are not signed yet. On macOS, clear the download flag once with
`xattr -dr com.apple.quarantine voxelsieve-*`; on Windows, SmartScreen may ask for confirmation.
The Linux build needs glibc 2.39 or newer (Ubuntu 24.04, Debian 13, Fedora 40).

**Register** the unpacked server with any MCP client as a stdio server. Directories after the
options are the only places VoxelSieve reads data from and keeps projects in:

```sh
claude mcp add voxelsieve -- /opt/voxelsieve/bin/vs-gui --mcp /data/scans   # Claude Code
```

```json
{"mcpServers": {"voxelsieve": {"command": "/opt/voxelsieve/bin/vs-gui",
                               "args": ["--mcp", "/data/scans"]}}}
```

The server keeps the project used last and opens it again at its next start (unless
`--project` names another or `--no-reopen` is given), so a client that starts a new server for
every chat finds its project open.

Then ask, for example: "Import /data/scans/housing.raw, analyse the porosity and write an
inspection report with the example order", or "Show me slice 400 of the latest dataset with the
pores". A client can cancel a running import or analysis; it stops at its next progress report.
Imports and analyses of large scans can take longer than a client waits for one call: with
`background: true` a `run_<operation>` tool returns at once, `wait` follows the operation (up to
50 s per call) and gives its step when it is done, and `cancel` stops it. `pipeline` runs several
operations one after the other in one call, for example import, `isolate_part`,
`segment_materials` and `material_meshes`.

Tools: `project_create`, `project_open`, `project_status` (the steps in brief; `step` gives one
in full), `project_save_as`, `undo`, `redo`,
`objects`, `results`, `object_select`, `dataset_info`, `histogram` (grey values with the
threshold and a suggested number of materials), `cad_features` (planes, cylinders and spheres of
a CAD model with a picture, for inspection plans), `view_slice` (a slice as an image, with pores and
zones), `view_objects` (also in the colours of the materials), `import_preview`, `step_telemetry`, `list_files`, `read_file`
(text files, and PNG pictures as images), `browse`, `wait`, `cancel`, `pipeline` and one `run_<operation>` per
operation, including plugins (`--plugins <dir>` or `VOXELSIEVE_PLUGIN_PATH`). To keep the choice
small for a model, the methods that keep the browser UI's saved views (`view_set`, `view_save`,
`view_list`, `view_image`, `view_rename`, `view_delete`), `list_operations` (tools/list already
describes every operation) and `object_add` (the same as `run_open_dataset` and `run_add_mesh`)
are left out unless `--tools all` is given. Tools carry annotations, so a client
knows which only read (`readOnlyHint`) and which delete something (`destructiveHint`). Parameter
schemas come from the operations, errors come back as tool errors with the reason, and long
operations report progress.

Resources: the result files of the project's steps (`report.html`, `report.json`,
`porosity.json`, the projection pictures, comparison and measurement results) are listed as
`voxelsieve://step/<id>/<output>/<file>`, so a client can attach them to the conversation.
Datasets are never sent over the protocol, only their metadata and these results.

Prompts: `porosity_check` (porosity against an inspection order, with report),
`compare_with_cad` (nominal-actual comparison), `wall_thickness_check` (walls against the minimum
of the drawing), `first_article_inspection` (characteristics of a drawing measured on the scan,
with the first article report) and `first_look` (size, voxel size, three slices) walk a model
through the usual inspections.
