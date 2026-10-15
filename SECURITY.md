# Security

VoxelSieve reads files that may come from others: raw volumes and their sidecars, TIFF stacks
and ZIP archives, DICOM and DICONDE files, STL meshes, projects and plugins. Its servers listen on
the local machine only, without authentication (the browser UI on 127.0.0.1, MCP on
stdin/stdout), and the MCP server reads and writes only in the directories it is given.

## Reporting a vulnerability

Please report vulnerabilities privately through
[GitHub's security advisories](https://github.com/yoshi5534/VoxelSieve/security/advisories/new),
not as a public issue. Include the version, the platform and, if you can, a file or the steps
that trigger the problem. You will get an answer within a week; fixes go into the next release
and are named in the changelog.

## Supported versions

Only the latest release gets security fixes.

## Things that are not vulnerabilities

- Plugins run as native code with the rights of the user (`--plugins`, `VOXELSIEVE_PLUGIN_PATH`);
  load only plugins you trust.
- `vs-gui --host` with an address other than the local machine exposes the UI without
  authentication to that network. Do this only behind your own access control.
