# Where pspocket stands

2026-10-04, after the round 15 commits (runs 022 to 030). Read this first in a new session, then `CLAUDE.md`, `docs/decisions.md`
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
| lv2 kernel | Host threads, sync objects, memory, `sys_vm`, event queues and ports, timers, filesystem (host directories and disc images read in place, D-043), `sys_fs_fcntl`/chmod/utime, SPU init (D-031), the RSX device (`sys_rsx`: local memory, context, IO map, display buffers, tiles; D-045), a report of what every thread waits on (D-053) |
| HLE modules | sysPrxForUser, cellSysmodule, cellSpurs (native, checked against libsre: initialize, workloads, tasksets, tasks, event flags, lv2 queue ports, barriers, job chains; D-047, D-054; host SPU workers run SPU tasks, job chains and a game's own policy module, D-050, D-051, D-055), cellAudio (a host thread, ports, notify queues, no sound out yet; D-052), cellSheap (libsre's allocator), cellGcmSys (20, with the vblank interrupt thread; D-045), cellSysutil and AvconfExt (18; D-048), sys_io (virtual controller, no keyboard; D-049), sceNp/sceNp2 (offline: callbacks and handlers kept, never called), cellNetCtl, sys_net, sys_fs (20 cellFs functions), cellGame (6) |
| Loader | `pspocket-boot` runs a decrypted EBOOT under the interpreter on the Mac. Crash reports show r3..r10 and the caller's code (D-042). `--bdvd IMAGE.iso`, `--probe-disc` |
| RSX | Portable front end (FIFO methods, state, textures, shader translation to MSL) and Metal backend (D-032). Driven by GTA V since run 016, through lv2 with a null backend on the Mac (commands run, nothing drawn) |
| iOS app | XcodeGen test host with device self-tests. On an iPhone 17 Pro Max, MetalFX 30 Hz runs steady and lifted PPU code runs 12× faster than the interpreter (runs 001–003). `-extract-iso` mode. The GTA V disc image (18.7 GB, decrypted at disc level) is in the app's Documents |

## GTA V bring-up so far (docs/runs 004–030)

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
- its first frame: it kicks the GraphicsMain job chain, then waits. Run 019 is the first run without a crash (60 s);
- its SPU tasks (dependency tasks, zlib inflate, Bink) and its own SPU policy module, on native SPURS workers (runs
  020, 021);
- cellAudio: two ports and the 5.33 ms audio period. Run 021 calls no unimplemented function (153 of 362 imports);
- its job chains (runs 022 to 026): the Secondary chain (its audio mixer, whose jobs use 232 of the 234 LS pages),
  GraphicsMain and EDGE/GFX;
- frames: from run 026 the RSX works through about 2 million command words a minute and flips about 30 times a second;
- loading its data (run 029, after the event flag direction fix, D-058) and **the first picture: the Rockstar logo
  screen**, drawn by the Metal backend and saved as PNG (`pspocket-boot --frames DIR`, D-057);
- PSN set up offline (D-059), a CellSpursTaskset2.

## Right now

The game shows its Rockstar logo screen, loads its data and sets up its systems. It stops at `_cellSpursQueueInitialize`
(its own assert when that fails): SPURS queues are next, with libsre as the reference (RPCS3 has none). Also called and
missing: save data (`cellSaveDataAutoLoad2`), trophies (`sceNpTrophyInit`), `cellNetCtlGetInfo`, background music
playback, `cellVoiceInit`, `sys_net_free_thread_context`. The game's audio mix thread faults if the game stays stuck for
a minute or more (a voice object never set up); that should go once the game gets further.
See the frames with `scripts/boot.sh --frames inventory/gta5/frames --frame-every 30`.

## Next steps, in order

1. **The boot loop.**
   1. Run `scripts/boot.sh`.
   2. Read the whole log, `inventory/boot-NNN.log`.
   3. Implement what it shows, following RPCS3 and citing it, with a test for each.
   4. Write a `docs/runs/0NN` entry (next is 031) and record decisions as D-060 onwards.
   5. Commit, push, repeat.

   A run that doesn't crash ends at its timeout with the thread report: every guest thread, what it waits on and its
   backtrace, and the RSX's state (D-053; `pspocket-boot --report-every S` prints it during the run too). Likely next
   modules, from the imports: cellRtc (7), sceNpTrophy (12), save data (in cellSysutil).
2. **Graphics.** The Metal backend draws the logo screen. As the game gets further, its frames need more of the RSX:
   render targets, tiled surfaces, shaders, textures. Look at the saved frames every round.
3. **The libsre reference from the firmware** (D-046): decrypted in `inventory/firmware/elf/` (libsre, libspurs_jq,
   the two SPU kernels); `inventory/firmware/tools/fw_unself.py` remakes it from the PUP and RPCS3's source. libsre
   also holds the SPU modules as raw code: the system service workload (libsre 0x21480), the taskset policy module
   (0x23680, loaded at LS 0xA00, syscall entry 0xA70), two task exit handlers (0x25500, 0x25B80) and the two job chain
   policy modules (0x26300, 0x2A180). Use it for SPURS queues and the SPU-side contract; the job chain policy is
   done (D-055). It is a reference only: never
   committed, never shipped, and no disassembly of it goes into the repository.
4. **Later:**
   - RSX → Metal up to the first frame;
   - sound out of cellAudio (a host sink: Core Audio) and input from a real controller;
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
