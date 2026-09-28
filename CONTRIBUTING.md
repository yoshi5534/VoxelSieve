# Contributing

Contributions are welcome: bug reports, sample data, fixes and new operations.

## Before you start

- For anything larger than a small fix, open an issue first so we can agree on the approach.
- Read the architecture decision records in `docs/adr/` before changing data types, file
  formats or the operation and plugin interfaces.
- New processing belongs in an operation (ADR 0008), so the studio, the command line and AI
  systems get it alike. Algorithms that must handle large scans read through `VolumeSource` or
  `Dataset` and never assume the volume fits in memory (ADR 0004).

## Build, test, lint

See [Build](README.md#build) for the dependencies. A pull request is mergeable when these pass,
as they do in CI:

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan
tools/lint.sh
```

Every algorithm gets a test against the synthetic phantom or a synthetic scan with known ground
truth (`vs-synth`), not against a hand-picked number.

## Data

Do not add scans of real parts unless you own them and may publish them. Test data is generated
synthetically; keep large files out of the repository. File format readers for CT vendors are
written from public documentation only.

## Developer Certificate of Origin

Contributions are accepted under the Apache License 2.0 (see [LICENSE](LICENSE)). Every commit
must be signed off to certify that you wrote it or otherwise have the right to submit it under
this license, as described in the [Developer Certificate of Origin](https://developercertificate.org/):

```sh
git commit -s
```

This adds a line like `Signed-off-by: Your Name <you@example.com>` to the commit message. Use
your real name or the name you are publicly known by.
