# Native SPURS (D-037)

SPURS is the PS3's SPU task system: the game hands it work (tasks, job chains, custom workloads) and SPURS keeps up
to six SPUs busy with it. On the PS3, Sony's library (libsre) runs a small scheduler kernel on every SPU. pspocket does
not run that kernel. It implements SPURS on the host, in `runtime/hle/src/cellSpurs*.cpp`, and runs only the game's own
SPU code, recompiled to ARM64 like the rest of the game.

## Layers

| Layer | What it is | Reference |
|---|---|---|
| PPU API | The 43 cellSpurs functions GTA V imports (more as other games need them), as HLE functions working on the game's own SPURS structures in guest memory | RPCS3 `cellSpurs.cpp`; structure layouts from `cellSpurs.h`, kept byte for byte because games and their SPU code read them |
| Scheduler | One host worker per SPU the game asked for, each with an SPU context and 256 KB local store. Workers pick workloads by SPURS' rules (per-SPU priority, min/max contention, ready counts and signals in the guest `CellSpurs`), and **sleep on a host condition variable when idle** instead of spinning like the SPU kernel does | RPCS3 `spursKernel1SelectWorkload` (the selection rules) |
| Taskset policy | Loads task ELFs into local store, saves and restores task contexts, and services task syscalls (yield, exit, wait signal, event flags, queues) by catching the task's call to the policy module's syscall address | Sony's libsre taskset policy module (from the user's firmware, read on the Mac); RPCS3 `cellSpursSpu.cpp` has an unwired draft |
| Job chain policy | Walks the job chain's command list (JOB, NEXT, CALL, RET, SYNC, GUARD, ...), DMAs each job's binary and input/output buffers, and runs it | Sony's libsre job chain module only: RPCS3's `spursJobChainEntry` is empty |
| Custom workloads | `cellSpursAddWorkload` with the game's own policy module: that module is game code, so it runs (recompiled) on a worker with the SPURS kernel interface it expects in local store | RPCS3 `spursKernelRunWorkload` (local-store layout, entry registers) |
| Sync objects | SPURS event flags, semaphores, barriers, queues (PPU and SPU sides) | RPCS3 for event flags, semaphores, barriers; libsre for queues (RPCS3: UNIMPLEMENTED) |

SPU code always runs through the SPU runtime (`runtime/spu`): lifted where the build recompiled it, interpreted (and
logged) where it didn't.

## Why native is the fast path on iPhone

The PS3's SPURS kernel polls for work on every SPU. Six polling SPU threads would occupy all six of the phone's
cores. The native scheduler runs SPU work only when there is some, on as many workers as the host has cores to spare,
and hands the cores back to the PPU threads and the renderer otherwise.

## Testing

- Unit tests on synthetic SPU programs (built in code, no game data) for each policy.
- On the Mac, once the user's firmware file is there: differential runs of the same workload under libsre (as a
  reference only, never in the app or the repository) and under the native scheduler.

## Stages

1. Attributes, initialize, finalize (done).
2. Workload registry (done for SPURS1: the guest tables as RPCS3 fills them) and the scheduler (ReadyCountStore,
   WakeUp, workload flag), custom workloads.
3. Tasksets (PPU side done) and tasks (GTA V's physics tasks, Bink, inflate).
4. Event flags, semaphores, barriers, lv2 event queue attachment.
5. Job chains and SPURS queues (need libsre as the reference).
