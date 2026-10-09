# Quickstart: your first CT analysis with an AI assistant

This guide takes you from download to a porosity report and a comparison with the CAD model in
about ten minutes, without building anything. You need a computer with Windows (x64), macOS
(Apple silicon) or Linux (x64), and an MCP client such as Claude Desktop or Claude Code. The
browser UI works without any AI client.

Your scans stay on your computer. VoxelSieve runs locally and sends the assistant only results:
pore lists, measurements, slice pictures and reports, never the volume data
([what the assistant sees](#what-the-assistant-sees)).

## 1. Install

Download from the [latest release](https://github.com/yoshi5534/VoxelSieve/releases/latest):

| You use | Download | Then |
| --- | --- | --- |
| Claude Desktop | `voxelsieve-<version>-<platform>.mcpb` | Double-click it, or drag it into Settings > Extensions. Choose the directories with your scans when asked. Done; go to step 3. |
| Claude Code or another MCP client | `voxelsieve-<version>-<platform>.tar.gz` (Linux, macOS) or `.zip` (Windows) | Unpack it anywhere, for example to `/opt/voxelsieve` or `C:\Tools\voxelsieve`. The programs are in `bin/`. |
| Docker | `voxelsieve-<version>-linux-x64.tar.gz` | Build the image, see [Docker](#docker). |

The programs are not signed yet. On macOS, unpacked programs from the archive are blocked until
you remove the quarantine flag: `xattr -dr com.apple.quarantine /opt/voxelsieve`. On Windows,
SmartScreen may warn on the first start; choose "More info" and "Run anyway".

## 2. Get the sample data

Download `voxelsieve-<version>-samples.zip` from the same release and unpack it, for example to
`~/ct/voxelsieve-samples`. It holds a synthetic CT scan of a cast housing with three shrinkage
cavities and a zone of loosened microstructure, so you can check every result:

| File | Content |
| --- | --- |
| `housing.raw` | the scan: 208 x 160 x 88 voxels of 0.25 mm, 16 bit |
| `housing.json` | its sidecar: dimensions, voxel size and the true position and volume of every defect |
| `housing.stl` | the CAD model of the housing in mm |
| `inspection_order.json` | an example inspection order with acceptance limits after BDG P 202 |

Your own scans work the same way: raw volumes with a JSON sidecar, TIFF stacks (also inside a
ZIP) or VoxelSieve datasets (`.vsieve`).

## 3. Connect your AI client

VoxelSieve reads data and keeps its projects only in the directories you give it. Give it the
directory with your scans.

**Claude Desktop** with the `.mcpb` bundle: nothing more to do. To change the directories later,
open Settings > Extensions > VoxelSieve.

**Claude Code:**

```sh
claude mcp add voxelsieve -- /opt/voxelsieve/bin/vs-studio --mcp ~/ct
```

**Other MCP clients** (Claude Desktop without the bundle, Cursor, ...) take a stdio server; on
Windows the command is `"C:\\Tools\\voxelsieve\\bin\\vs-studio.exe"` in JSON:

```json
{
  "mcpServers": {
    "voxelsieve": {
      "command": "/opt/voxelsieve/bin/vs-studio",
      "args": ["--mcp", "/home/me/ct"]
    }
  }
}
```

## 4. First analyses

Ask in your own words. These three requests use the sample data; replace the paths with yours.

**Porosity against the inspection order:**

> Analyse the porosity of ~/ct/voxelsieve-samples/housing.raw with the inspection order
> inspection_order.json next to it, and tell me whether the part passes.

The assistant creates a project next to the scan, imports it, runs the porosity analysis and
writes the report. Expect 3 pores with together about 3.6 mm³ (the sidecar says 1.33, 1.13 and
1.16 mm³), and a failed part: zone A, the sealing face, permits no loosened microstructure, and the
zone of loosened microstructure lies in it. The report itself is `report.html` in the project
directory; the assistant can also attach it to the conversation.

**Comparison with the CAD model:**

> Compare the scan with housing.stl and show me where it deviates by more than 0.1 mm.

Expect about 99.9 % of the surface within 0.1 mm: the synthetic scan matches its model except at
the noise level.

**A look inside:**

> Show me the slice through the largest pore.

The assistant renders the slice with the pores in red and the loosened zone in yellow.

VoxelSieve also offers these as prompts, *porosity_check*, *compare_with_cad* and *first_look*:
clients that list MCP prompts let you pick one, fill in the paths and have the assistant walk
through the steps.

## 5. The browser UI

Every project the assistant creates also opens in the browser UI, with the step protocol,
undo/redo, slices and the 3D view:

```sh
/opt/voxelsieve/bin/vs-studio ~/ct
```

Then open <http://localhost:8410> and open the project directory the assistant named. The 3D view needs WebGL2; if your browser has it turned off, the
view says how to turn it on.

## Docker

The image runs the same server on Ubuntu 24.04. Build it from the Linux package and the
Dockerfile in this repository:

```sh
tar xzf voxelsieve-<version>-linux-x64.tar.gz
curl -LO https://raw.githubusercontent.com/yoshi5534/VoxelSieve/main/packaging/Dockerfile
docker build --build-arg PACKAGE=voxelsieve-<version>-linux-x64 -t voxelsieve .
```

Browser UI for the data in `~/ct`, on <http://localhost:8410>:

```sh
docker run --rm -p 8410:8410 -v ~/ct:/data --user "$(id -u):$(id -g)" voxelsieve
```

MCP over stdin/stdout, for an MCP client (`-i` keeps stdin open):

```json
{
  "mcpServers": {
    "voxelsieve": {
      "command": "docker",
      "args": ["run", "-i", "--rm", "-v", "/home/me/ct:/data", "--user", "1000:1000",
               "voxelsieve", "--mcp", "/data"]
    }
  }
}
```

Inside the container the data is at `/data`, so ask for `/data/voxelsieve-samples/housing.raw`.
`--user` with your user and group id lets VoxelSieve write projects into the mounted directory;
without it, it runs as its own user, which may not write there. The UI has no login: publish the
port only on a network you trust.

## What the assistant sees

VoxelSieve answers the assistant's tool calls with results, not data:

- step summaries such as the number of pores, porosity, deviations and the verdict per zone,
- files of the results it asks for, up to 1 MB of text (`porosity.json`, `report.json`) or 8 MB
  per picture (slices, projections),
- directory listings inside the directories you allowed.

The scans themselves, datasets and meshes never leave your computer. The assistant's model does
see the results and pictures above, so treat them as you would treat sending them by email.

The report from VoxelSieve is the result of an inspection, not the assistant's summary of it.
VoxelSieve is not a certified inspection system; check results that matter.

## Troubleshooting

- **"Outside the allowed directories"**: give the directory with the scan to `vs-studio` (or
  choose it in the bundle's settings).
- **The tools of the saved views are missing**: they are left out for AI clients; start with
  `--tools all` to get them.
- **Long imports**: a large scan takes minutes to hours. The assistant sees the progress and can
  cancel it.
