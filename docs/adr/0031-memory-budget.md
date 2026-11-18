# 0031: Execution context and memory budget

Status: accepted (2026-11-18)

## Context

VoxelSieve is meant to analyse scans of several hundred GB on a workstation with 32–64 GB of RAM
(ADR 0004). Each part of the process sized its memory on its own:

- every `Dataset` cache, the GUI's view cache, the material volume's cache and the per-thread
  buffers of the algorithms had a fixed default or a share of the physical memory;
- nothing added these up, so two analyses in the GUI, or one analysis beside the view cache,
  could ask for more than the machine has;
- the brick cache dropped bricks that callers still held, so a halo read of T threads kept
  T × 27 bricks alive that it did not count, and loaded them again when asked for;
- bricks loaded on access (`BrickLoading::kOnAccess`) grew without being counted until the next
  miss, which then measured every cached brick under the lock;
- `forEachBrick` was a plain `parallel_for` over the bricks in index order, and each algorithm
  that needed fewer bricks in flight (materials, models, orientation, the surface writer) built
  its own pipeline and guessed its own share of the memory.

The review of November 2026 (`docs/review-large-data-2026-11.md`, findings A1–A9) asked for one
budget for the whole process before the analyses are made to stay within it.

## Decision

**One memory budget per process** (`resources.hpp`). `memoryBudgetBytes()` is three quarters of
the usable memory (the cgroup limit on Linux), or what `--memory <MB>` / `setMemoryBudget` set.
It is split into:

- **Brick caches.** Every `BrickCache` registers its budget with the process; the shared cache
  of the datasets defaults to a third of the budget (at least 2 GiB, at most half), the GUI's
  view cache to a sixteenth (at least 512 MB). `--cache` still sets the shared cache alone.
- **Work memory** (`workMemoryBytes()`): the budget less all cache budgets, at least an eighth of
  the budget, split evenly between the operations admitted at the moment.

The "execution context" of the plan is the process itself, not an object passed through every
call: tools run one analysis, and the GUI runs its operations in the same process, which the
admission below divides. Threads are limited process-wide as before (`setThreadLimit`,
`--threads`); cancel and progress stay in the operation context (`OperationContext`).

**Tasks in flight come from the work memory.** An algorithm that processes bricks or tiles
declares what one task needs; `tasksInMemory(task_bytes)` says how many fit, between 1 and the
thread limit. `Dataset::forEachBrick` takes a `BrickSweep` with that cost and a halo:

- bricks are visited along a Morton curve, so the bricks in use are a small window;
- at most `tasksInMemory` calls run at once, each isolated so that a thread waiting for its own
  nested parallel work starts no other brick (`detail::runInFlight`);
- the bricks the halo reaches are pinned for the call, so its neighbours find them cached.

Segmentation, learned segmentation, orientation, the surface writer and `writeDerivedDataset`
use the same helper or the same sizing instead of their own guesses.

**Pinned bricks.** A brick that a caller holds (`Dataset::BrickPtr`) is pinned: the cache does
not evict it, and it counts against the budget as long as it is held. Asking for it again is a
hit, never a second copy. `CacheStats` reports the pinned bricks and bytes. A brick loaded on
access is charged its full size (all leaves) when it is loaded, so the cache no longer measures
its bricks again.

**Typed region reads.** `Dataset::readRegion` has overloads for `uint16_t` and `uint8_t` output
beside `float`, for callers that need integer grey values only (the DICONDE export); a wider
dataset into a narrower type throws. All overloads copy leaf by leaf instead of probing each
voxel.

**Admission of operations.** `Operation::estimate` declares what a run needs: work memory and
the disk space of its outputs, roughly per active voxel of its input dataset for the built-in
operations. The project checks the free space of the step directory before the run
(`requireFreeSpace`), and `admitOperation` lets the run start when no other operation runs or
when its memory fits beside theirs; otherwise it waits, and stops waiting when it is cancelled.
An estimate larger than the work memory still runs alone.

**Resumable derived datasets.** `writeDerivedDataset` calls the values of one brick after the
other and builds, compresses and writes the bricks in parallel behind them. With a
`resume_key` it keeps a journal (`derived.json`, `bricks.log`, as the import of ADR 0004 does)
and an interrupted run resumes; the thickness map uses it and adds the values of the bricks it
kept to its statistics again.

## Consequences

- With the default budget a tool behaves as before on a machine of its own: the shared cache
  keeps its size (a third of three quarters is the quarter it had), and tasks in flight are the
  thread count unless a task is large.
- `--memory` bounds caches and work buffers together, but not the memory an algorithm keeps per
  voxel of its result (pore statistics, meshes); those are bounded by phase 3 of the review.
- The estimates of the operations are coarse figures from the 2048³ benchmark; an estimate that
  is too high can fail a step for lack of disk space it would not have used, one that is too low
  only lets operations overlap more.
- Plugins see `Operation::estimate` as a new virtual function: the plugin API version is 5.
