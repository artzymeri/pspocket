# Where pspocket stands

2026-10-04, commit 5b29087 and later. Read this first in a new session, then `CLAUDE.md`, `docs/decisions.md`
(D-033 onwards matter most now) and the latest `docs/runs/` entries. Keep this file current when the picture changes.

## The project

A static recompiler that turns PS3 games into native, signed iOS apps: the game's PPU and SPU code is translated to
ARM64 ahead of time and its graphics go to Metal. It is not an emulator. One Mac-side build per game (disc image in,
app out); the runtime is shared by all games (D-036). GTA V (BLES01807) is the driving title (D-025); the goal is any
PS3 game. SPURS (the PS3's SPU job system) is our own native C++ implementation, with no Sony code in the app (D-037).

## Working with the user

- Not a programmer, and keen to see the game run. Don't ask design questions: decide sensibly, record the decision in
  `docs/decisions.md`, and give one command to run per round, saying in plain words what changed and what to expect.
- Be honest about the size of the job. They have been told GTA V on the iPhone is months of work at least.
- Work on `main` only and push to `main` only.
- Never commit game data (CLAUDE.md rule 1). The disc image, the decrypted EBOOT, boot logs and the firmware all stay
  in `inventory/`, which is gitignored.

## What exists

| Area | State |
|---|---|
| Toolchain | `ps3inspect` (inventory), ELF/SELF/PRX loader, NID database from RPCS3 (D-019), PPU interpreter and lifter with differential fuzzing (D-023, D-028), SPU interpreter, runtime and lifter (D-029), one PPU/SPU reservation system (D-030), ISO 9660 disc reader |
| lv2 kernel | Host threads, sync objects, memory, `sys_vm`, event queues and ports, timers, filesystem (host directories and disc images read in place, D-043), `sys_fs_fcntl`/chmod/utime, SPU init (D-031), the RSX device (`sys_rsx`: local memory, context, IO map, display buffers, tiles; D-045) |
| HLE modules | sysPrxForUser, cellSysmodule, cellSpurs (native, checked against libsre: initialize, workloads, tasksets, tasks, event flags, lv2 queue ports, barriers, job chain creation and run; D-047), cellSheap (libsre's allocator), cellGcmSys (20, with the vblank interrupt thread; D-045), cellSysutil and AvconfExt (18; D-048), sys_io (virtual controller, no keyboard; D-049), sceNp/sceNp2 (offline), cellNetCtl, sys_net, sys_fs (20 cellFs functions), cellGame (6) |
| Loader | `pspocket-boot` runs a decrypted EBOOT under the interpreter on the Mac. Crash reports show r3..r10 and the caller's code (D-042). `--bdvd IMAGE.iso`, `--probe-disc` |
| RSX | Portable front end (FIFO methods, state, textures, shader translation to MSL) and Metal backend (D-032). Driven by GTA V since run 016, through lv2 with a null backend on the Mac (commands run, nothing drawn) |
| iOS app | XcodeGen test host with device self-tests. On an iPhone 17 Pro Max, MetalFX 30 Hz runs steady and lifted PPU code runs 12× faster than the interpreter (runs 001–003). `-extract-iso` mode. The GTA V disc image (18.7 GB, decrypted at disc level) is in the app's Documents |

## GTA V bring-up so far (docs/runs 004–019)

The EBOOT boots under the interpreter on the M4 Mac. It gets through:
- the CRT, TLS, lwmutexes, cellSysmodule;
- two SPURS instances ("Primary" with 5 SPUs, "Secondary" with 1);
- `sys_vm`, threads, networking (offline);
- SPURS setup: 7 job chains (DefaultShort, DefaultLong, ShapeTests, AudioEngine, GraphicsMain, GraphicsOther,
  Secondary), the "dependency_spu" taskset with 4 tasks and an event flag, and Bink's "RAD Game Tools Task" tasksets;
- `sceNp2Init`, cellGame;
- its archives, read in place from the disc image (copied once from the iPhone, `inventory/gta5/disc.iso`);
- graphics setup: GCM, 720p video output, two display buffers, tiled render targets, the vblank handler;
- the system cache, controller and keyboard setup, the EDGE/GFX job chain;
- its first frame: it kicks the GraphicsMain job chain, then waits. Run 019 is the first run without a crash (60 s).

## Right now

The game waits on SPU work that nothing runs yet: the dependency tasks, zlib inflate, Bink, the EDGE and graphics job
chains, and a custom workload with GTA V's own SPU policy module (`cellSpursAddWorkload`, unimplemented). Running SPU
code natively is the critical path (step 2 below). Also unimplemented and called: `cellAudioInit`, `cellAudioPortOpen`.
The phone is no longer needed for boot rounds.

## Next steps, in order

1. **The boot loop.**
   1. Run `scripts/boot.sh`.
   2. Read the whole log, `inventory/boot-NNN.log`.
   3. Implement what it shows, following RPCS3 and citing it, with a test for each.
   4. Write a `docs/runs/0NN` entry (next is 020) and record decisions as D-050 onwards.
   5. Commit, push, repeat.

   Now that the game no longer crashes, a round ends at the timeout: `pspocket-boot` needs a report of what each guest
   thread is waiting for. Likely next modules, from the imports: cellAudio (11, needs an audio thread), cellRtc (7),
   sceNpTrophy (12), save data (in cellSysutil).
2. **Native SPURS stage 3b: running SPU tasks** (`docs/spurs-native.md`).
   - Host workers run task ELFs through `runtime/spu`, with the local-store layout and the task syscalls (0xA70) that
     Sony's SPU-side libraries expect. RPCS3 `cellSpursSpu.cpp` is a draft reference; libsre is the authority.
   - Today, waking SPU tasks from `cellSpursEventFlagSet` is a deliberate `PS_FATAL`.
   - A scheduler first: host workers that pick workloads by SPURS' rules (RPCS3 `spursKernel1SelectWorkload`) and sleep
     when idle. Then the taskset policy, then job chains (stage 5) and custom workloads (GTA V's own policy module).
3. **The libsre reference from the firmware** (D-046): decrypted in `inventory/firmware/elf/` (libsre, libspurs_jq,
   the two SPU kernels); `inventory/firmware/tools/fw_unself.py` remakes it from the PUP and RPCS3's source. libsre
   also holds the SPU modules as raw code: the system service workload (libsre 0x21480), the taskset policy module
   (0x23680, loaded at LS 0xA00, syscall entry 0xA70), two task exit handlers (0x25500, 0x25B80) and the two job chain
   policy modules (0x26300, 0x2A180). Use it for job chains (8 created by GTA V, none run), SPURS queues and the
   SPU-side contract. It is a reference only: never
   committed, never shipped, and no disassembly of it goes into the repository.
4. **Later:**
   - RSX → Metal up to the first frame;
   - audio and input;
   - lift the whole EBOOT (`ppu-lift`, 6.1 M instructions, already lifts in 10 s) instead of interpreting;
   - SPU lifting;
   - the iOS app booting the game from the ISO in its Documents.

## Checked against libsre

D-039, D-041 and D-042 were checked against libsre in round 13 (D-047). Task creation, the task attribute, the LS
pattern helpers, taskset shutdown and cellSheap now follow it. RPCS3 runs Sony's libsre by default (`g_prx_list`), so
its `cellSpurs.cpp` HLE is a partial, little-exercised reference: check new SPURS functions against libsre too.

## Practical notes

- The Mac checkout is `~/Desktop/Private/pspocket`.
  - Decrypted EBOOT: `inventory/gta5/decrypt/EBOOT.elf`.
  - Game's `/dev_hdd0`: `inventory/gta5/hdd0`.
  - Boot logs: `inventory/boot-NNN.log`.
- Build and test on the Mac:

  ```sh
  cmake -S . -B build/host -G Ninja -DPSPOCKET_WERROR=ON
  ninja -C build/host
  ctest --test-dir build/host
  ```

  Clang only. The AArch64 qemu and sanitizer trees only matter on Linux hosts.
- The iPhone: `scripts/ios.sh` (bundle `com.artzymeri.pspocket.testhost`, team M7XJA39ZV6). The phone has to be
  connected and unlocked.
- Pre-commit hook, once per clone: `git config core.hooksPath .githooks`.
