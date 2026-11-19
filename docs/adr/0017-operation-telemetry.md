# 0017: Telemetry of every operation

Status: accepted (2026-09-30)

## Context

Every performance problem so far was found by reading console output by hand: a TIFF import that
used few cores because pass 2 decoded the same slices again and again, and a pass 2 that slowed
from 22 s to 101 s because the pages of a memory-mapped ZIP pushed the staging file out of the
page cache. `vs-sieve` printed stage times and the process's peak memory; the other tools printed
one or two times, and the GUI recorded nothing but start and end time. To make operations
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
- Amendment (2026-10-10): on Windows the device reads and writes and the major page faults come
  from the system-wide performance counters (PDH, part of Windows: `PhysicalDisk(_Total)` read
  and write bytes, `Memory\Page Reads/sec` for hard faults), read raw so they are cumulative.
  Reads of memory-mapped files appear in no per-process counter there, so without them every
  import read zero bytes from disk and the hints blamed too few parallel parts where the work
  waited for the disk. They include other processes.
- Amendment (2026-11-17, numbers for scans larger than memory):
  - **Peaks per phase on every system.** A sampler reads only the memory counters every 50 ms
    (`TelemetryOptions::peak_interval_s`). Where there is no high-water mark per phase
    (Windows, macOS), phases no longer take the process's lifetime peak, which carried the
    peak of an earlier phase into every later one.
  - **Own and mapped memory on Windows.** The private working set counts as the process's own
    pages, the rest of the working set as mapped files (`PROCESS_MEMORY_COUNTERS_EX2`). The
    working set of a sieve of a 16 GB raw file was 18.7 GB, of which only 2.5 GB was its own.
    `system.memory_split` says whether the split was available.
  - **Reads the library counts itself** (`countRead`), the same on every system:
    - `mapped_read_mb`: bytes copied out of memory-mapped raw files and staging copies, which
      no per-process counter sees.
    - `brick_read_mb`: brick files of datasets loaded (grey values and material labels). Set
      against the dataset's size on disk, it shows how often bricks are read again: 34× for
      the wall thickness of a 2048³ scan with the default cache.
  - The table and the UI show own memory and mapped pages apart.
  - `tools/bench/scale.cmake` runs every tool on phantoms of growing size and collects these
    numbers. The nightly scale test checks them against `tests/scale_limits.json`.
- Library code marks its phases with `TelemetryPhase`, which goes to the calling thread's current
  recorder (`TelemetryScope`) and does nothing without one. So `writeDataset`,
  `analyzePorosity`, `segmentMaterials` and `compareToCad` report the same phases in the GUI
  and on the command line, and plugins get a total without any change of the plugin API.
- `Project::run` records every step: the record without its timeline goes into the protocol
  (`Step::telemetry` in `project.json`), the whole record into `telemetry.json` in the step
  directory. A failed step keeps its summary. The GUI method `step_telemetry` (and so the MCP
  tool of the same name) returns the whole record; the UI shows a line per step and, under
  "Run time and resources", the phases, hints and the timeline.
- Command-line tools print a table of the phases to stderr at the end and write the whole record
  with `--telemetry <file.json>`.
- A record carries hints for phases that take at least 5 s: few cores busy with many major page
  faults (waiting for data that is not in the page cache), few cores busy without them (serial
  work or a lock), or a peak close to the installed memory.
- Telemetry stays local. Nothing is sent anywhere.

## Consequences

- The counters belong to the whole process. The GUI runs one operation at a time, so a step
  gets its own numbers; its HTTP threads add a little CPU time.
- Reading the counters at each phase boundary, once a second, and the memory every 50 ms costs
  well below a millisecond each time.
- Only Linux has these counters; elsewhere the record keeps wall time and CPU time.
