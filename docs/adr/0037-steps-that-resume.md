# 0037: Steps that resume and projects that carry their inputs

Status: accepted (2026-11-23)

## Context

`vs-sieve` resumes an interrupted import from the journal it leaves in the output directory
(ADR 0034). In a project that never happened: a step that failed lost its directory at once, and
a step found `running` when the project was opened was taken for interrupted and lost it then.
A 300 GB import that was killed at 90 % started from zero in the GUI. The review of November 2026
(`docs/review-2026-11-b.md`, A4, A6, A7, C2, C3) found four more places where a project depended
on something outside itself or trusted a file too far.

## Decision

**Steps of operations that can resume keep their files until the next step.**

- An operation says that it continues work found in its output directory with
  `OperationInfo::resumable` (the imports do). The plugin API version is 6.
- When such a step fails or the program ends while it runs, its directory stays and the step is
  marked `resumable`. The same operation run again with the same parameters and inputs gets the
  directory and continues; in the UI the step has a "Resume" button that does this.
- Any other step that runs removes the kept files first, so they cost disk space only until the
  next step. A step the user cancelled keeps nothing.
- An import in a project owns its directory: a journal there of another input or other options
  is replaced (`DatasetOptions::replace_other_import`). On the command line that stays an
  error, because the directory is the user's.

**The journal of an import names its input.** `VolumeSource::identity` gives the path, size and
time of the last change of the file or directory a source reads; it is part of the key that must
match to resume. Scans of a series have one size, and the size alone let the next scan continue
the import of the last one.

**A comparison keeps its CAD model.** `compare_cad` copies the STL beside `compare.json`
(`cad_file`); the nominal wall thickness and the dimensional measurement read the copy. The
project no longer depends on where the file was, and measures against the model the alignment
was made with.

**Files named inside files pass the directory boundary.** `OperationContext::check_path` is the
check of the caller (a server limited to some directories); operations call it for the mesh of a
region in an inspection order and the volume files of a gzipped XML project before they read
them. Parameters that are paths were checked already.

**The files everything hangs on reach the disk.** `writeFile` and `writeJson` take `to_disk`:
`project.json`, the `index.json` of a dataset and the journal of an import are flushed before
they are renamed into place, so a power cut leaves the old or the new file. Bricks are not
flushed one by one; a resumed import instead writes again every brick its log lists whose file
is missing or empty.

## Consequences

- A failed import can leave a staging copy of the scan in the project until the next step; the
  step shows its size and says that it can be resumed.
- Projects written before have comparisons without a copy; they read the CAD file from its
  path as before.
- A plugin built against API version 5 is rejected and must be rebuilt.
