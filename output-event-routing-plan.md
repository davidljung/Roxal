# Output Event Routing Implementation Plan

Branch: `codex/output-event-routing`

## Goals

- Make Roxal `print()` capturable by an embedding host without blocking a
  real-time `runFor()` loop.
- Preserve the standalone CLI's serialized stdout/stderr behavior by default.
- Use one logging-oriented event envelope so later structured logging can be
  added without creating a second transport and sink API.
- Preserve print output across remote-compute calls, including chained calls.
- Route ordinary Roxal-owned diagnostics that may arise during embedded
  execution through the same mechanism.

## Agreed Design

### Event model

`OutputEventView` is the non-owning hot-path envelope; `OutputEvent` is its
owning transport/consumer counterpart. The envelope contains:

- kind: `Print`, `Log`, or `Diagnostic`;
- severity: `None` for `Print`, with log-oriented levels reserved for later;
- channel: `stdout`, `stderr`, or an application-defined name;
- category: an independent logging/diagnostic namespace;
- text and flush intent;
- optional source name, 1-based line, and 0-based column;
- presentation flags, initially `SourceExcerpt`, asking the sink to add source
  text and a caret when it can resolve the source.

Source presentation is best effort. A sink must still emit the event if the
source is virtual, remote-only, stale, or otherwise unavailable.

### Sink contract

- Roxal has one primary, process-wide `OutputSink`; this matches the VM's
  current process-wide singleton and avoids broadcast callback semantics.
- The sink is a stable, non-owning raw pointer installed before execution and
  kept alive until VM-owned threads have stopped. It is not replaced while
  execution is active.
- Dispatch performs an atomic pointer load and one virtual call. The router
  catches sink exceptions and counts rejected/thrown events as drops.
- This is a bounded-dispatch contract, not an allocation-free VM contract;
  evaluating and stringifying a Roxal `print()` argument may allocate normally.
- A custom RT-capable sink must be bounded and non-blocking and perform no
  file, terminal, or network I/O. It copies retained view data into storage
  governed by its own queueing policy.
- Rejection never falls back to blocking console output.
- The built-in sink remains synchronous and serializes each complete event
  under one mutex. Custom channels use stdout by default; `stderr` uses stderr.
- Roxal does not create an output worker thread. An embedding host such as
  Future Controller owns its queue and low-priority consumer. If a core async
  sink is added later, its worker must explicitly leave inherited RT scheduling
  (for example, use `SCHED_OTHER`) and observe the host's RT-core exclusion.

`flush=true` is an urgency/delivery-boundary request, not permission for an RT
producer to wait for physical I/O. The default console sink flushes before
returning. An asynchronous host sink must preserve accepted record order and
make a flushed record visible to its target promptly, but the producer remains
non-blocking.

### Language surface

`sys.print(value='', end='\n', flush=false, here=false, channel='stdout')`
constructs one complete `Print` event. Severity and category are deliberately
not print parameters; future logging builtins will populate those fields while
using the same underlying route. The old implementation's apparent
positional-boolean compatibility branch is pre-existing unreachable code
because the typed `end:string` parameter converts the value before the native
builtin sees it; this change does not claim that syntax as supported.

### Execution and compute routing

- Generalize the former print-only dynamic target to `OutputRoute`.
- Capture and restore the route across queued local actor calls as well as
  remote calls.
- `here=true` bypasses the dynamic compute return route and submits to the
  machine-local sink.
- Diagnostics tee to the machine-local sink and the originating caller. The
  server-local copy omits client-source presentation; the forwarded copy keeps
  it so the client's sink can resolve the client-owned source file.
- The compute protocol carries the complete event envelope and has a protocol
  version bump. Chained remote calls retain their upstream route.

## Existing Output Migration Boundary

Confirmed and implemented with the following boundary.

Route through output events:

- dataflow period/tick and node overruns, RT-lint advisories, debug signal
  timing, and callback exceptions;
- actor/VM remote and native call failures, unawaited actor exceptions, and
  dataflow-engine termination;
- async file-I/O, NN, DDS, gRPC, and web runtime diagnostics;
- compiler/parser errors reached through the embedded API and bytecode-emitter
  warnings;
- VM opcode-profile warnings and the impossible-type `len()` diagnostic;
- core callback exception reporting;
- internal `_runtests` reports as ordinary `Print` records.

Keep direct:

- CLI/REPL argument, status, help/version, AST/disassembly, and debug dumps;
- compute-server process lifecycle output;
- fatal assertions and forensic paths, GC diagnostics/self-tests,
  signal-handler stack dumps, Qt fatal/message fallback, unit-test harness
  output, third-party miniaudio, and unused SGCL.

## Work Plan and Status

1. **Complete:** Inventory the existing print path, compute backchannel,
   embedding/RT entry points, callback registry, and direct console output.
2. **Complete:** Agree on event fields, sink ownership/lifetime, ordering/drop
   semantics, source-presentation model, and migration boundary.
3. **Complete:** Add the common event/view types, sink interface, router,
   default serialized console sink, and focused C++ tests.
4. **Complete:** Route `print()` through the event API, add `channel`, retain
   `flush`/`here` compatibility, and add language-level tests.
5. **Complete:** Generalize and propagate `OutputRoute` through local actors.
6. **Complete:** Replace compute `PrintOutput` with the versioned full-event
   protocol and verify direct and chained remote behavior.
7. **Complete:** Move runtime-error source-file lookup and caret rendering
   out of the execution path and into the sink.
8. **Complete:** Migrate the agreed ordinary diagnostics; leave the
   listed emergency, CLI, forensic, and service-administration output direct.
9. **Complete:** Document only the script-facing `print()` signature/channel
   behavior in `roxal-for-devs.md`. Document the embedding API, implementation
   architecture, sink contract, and RT constraints in `implementation-notes.md`.
10. **Complete:** Run focused unit/language/compute tests, then the normal build
    and regression suite; report any environmental or pre-existing failures
    separately.

## Checkpoints

- The all-feature build, a compute-disabled build, and a Debug build compile.
- The focused C++ output/callback tests pass in optimized and Debug builds.
- The complete source suite passes: 948 tests, zero unexpected failures.
- No output worker thread has been added.
