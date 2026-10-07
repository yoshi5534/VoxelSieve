# 0017: Telemetry of every operation

Status: accepted (2026-09-30)

## Context

Every performance problem so far was found by reading console output by hand: a TIFF import that
used few cores because pass 2 decoded the same slices again and again, and a pass 2 that slowed
from 22 s to 101 s because the pages of a memory-mapped ZIP pushed the staging file out of the
page cache. `vs-sieve` printed stage times and the process's peak memory; the other tools printed
one or two times, and the studio recorded nothing but start and end time. To make operations
faster and leaner we need the same numbers from every operation, per phase, kept with the result.

## Decision

- `Telemetry` (`telemetry.hpp`) records a run per phase: wall time, CPU time and the cores it kept
  busy, peak resident memory split into the process's own pages and mapped file pages, bytes
  read and written (all calls, and what reached the disk), and major and minor page faults. A
  background thread samples the same counters once a second into a timeline (at most 600 points;
  longer runs keep every other point and double the interval).
- The counters are the process's own, read from `getrusage`, `/proc/self/io` and
  `/proc/self/status`; the peak memory of each phase comes from resetting the high-water mark at
  each phase boundary (`/proc/self/clear_refs`, Linux 4.0+). Where that is not allowed the peaks
  count from process start, and the record says so. No new dependency.
- Amendment (2026-10-07, Windows and macOS builds): macOS reads `getrusage`, `task_info` (memory
  footprint as own pages, the rest of the resident set as mapped files) and `proc_pid_rusage`
  (bytes to and from the device; logical writes). Windows reads `GetProcessTimes`,
  `GetProcessMemoryInfo` (the whole working set counts as own pages, all page faults as minor)
  and `GetProcessIoCounters` (bytes read and written, no device counters). On both the peaks
  count from process start.
- Library code marks its phases with `TelemetryPhase`, which goes to the calling thread's current
  recorder (`TelemetryScope`) and does nothing without one. So `writeDataset`,
  `analyzePorosity`, `segmentMaterials` and `compareToCad` report the same phases in the studio
  and on the command line, and plugins get a total without any change of the plugin API.
- `Project::run` records every step: the record without its timeline goes into the protocol
  (`Step::telemetry` in `project.json`), the whole record into `telemetry.json` in the step
  directory. A failed step keeps its summary. The studio method `step_telemetry` (and so the MCP
  tool of the same name) returns the whole record; the UI shows a line per step and, under
  "Run time and resources", the phases, hints and the timeline.
- Command-line tools print a table of the phases to stderr at the end and write the whole record
  with `--telemetry <file.json>`.
- A record carries hints for phases that take at least 5 s: few cores busy with many major page
  faults (waiting for data that is not in the page cache), few cores busy without them (serial
  work or a lock), or a peak close to the installed memory.
- Telemetry stays local. Nothing is sent anywhere.

## Consequences

- The counters belong to the whole process. The studio runs one operation at a time, so a step
  gets its own numbers; its HTTP threads add a little CPU time.
- Reading `/proc` at each phase boundary and once a second costs well below a millisecond.
- Only Linux has these counters; elsewhere the record keeps wall time and CPU time.
