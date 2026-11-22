# 0036: One process per project

Status: accepted (2026-11-23)

## Context

A project is a directory with `project.json` and the outputs of its steps (ADR 0008). Nothing
kept two programs from opening the same one. `Project::open` takes a step that is `running` in
the file for one that was interrupted, marks it failed and removes its directory. When the step
was in fact running in another process, that removed the files the other process was writing:
the review of November 2026 (`docs/review-2026-11-b.md`, A3) reproduced it with a browser UI
that imports and an MCP server that starts meanwhile and reopens the project used last. Both
would also have written `project.json` from their own copy of the protocol.

## Decision

**A project is open in one process at a time.** `Project::create`, `Project::open` and
`Project::saveAs` take a lock of the operating system on `project.lock` in the project directory
(`boost::interprocess::file_lock`: `fcntl` on POSIX, `LockFileEx` on Windows). A second process
gets the error "The project … is open in another VoxelSieve program; close it there first" and
touches nothing. `vs-gui --mcp` then starts without a project instead of reopening the last one.

- The lock belongs to the process and ends with it, so a crash leaves no stale lock; the file
  itself stays and means nothing.
- Within a process the copies of a project (the working copy of a running operation) and every
  further opening of the same directory share the one lock, held by reference count.
- On a file system without locks, or a read-only project, the project opens unlocked, as
  before.

Boost.Interprocess is header-only and one more port of the Boost that asio, beast and iostreams
already bring (ADR 0016); it is the only part used.

## Not done

Opening a locked project read-only, to look at results while another program computes. Every
change of the view saves the project, so this needs a project that never saves; it can follow
when someone needs it.

## Consequences

- The browser UI and an AI client cannot work on one project at the same time: they are two
  programs, as one `vs-gui` serves either the browser or MCP. Serving both from one process
  would lift this and is not part of this decision.
- Tests that open a project twice in one process keep working; the test of the lock itself needs
  a second process (a death test, POSIX only).
