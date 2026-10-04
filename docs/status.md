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
| lv2 kernel | Host threads, sync objects, memory, `sys_vm`, event queues and ports, timers, filesystem (host directories and disc images read in place, D-043), `sys_fs_fcntl`/chmod/utime, SPU init (D-031) |
| HLE modules | sysPrxForUser, cellSysmodule, cellSpurs (native: initialize, workloads, tasksets, tasks, event flags, lv2 queue ports, barriers, job chain creation), cellSheap (initialize), sceNp/sceNp2 (offline), cellNetCtl, sys_net, sys_fs (20 cellFs functions), cellGame (6) |
| Loader | `pspocket-boot` runs a decrypted EBOOT under the interpreter on the Mac. Crash reports show r3..r10 and the caller's code (D-042). `--bdvd IMAGE.iso`, `--probe-disc` |
| RSX | Portable front end (FIFO methods, state, textures, shader translation to MSL) and Metal backend (D-032); not yet driven by a game |
| iOS app | XcodeGen test host with device self-tests. On an iPhone 17 Pro Max, MetalFX 30 Hz runs steady and lifted PPU code runs 12× faster than the interpreter (runs 001–003). `-extract-iso` mode. The GTA V disc image (18.7 GB, decrypted at disc level) is in the app's Documents |

## GTA V bring-up so far (docs/runs 004–014)

The EBOOT boots under the interpreter on the M4 Mac. It gets through:
- the CRT, TLS, lwmutexes, cellSysmodule;
- two SPURS instances ("Primary" with 5 SPUs, "Secondary" with 1);
- `sys_vm`, threads, networking (offline);
- SPURS setup: 7 job chains (DefaultShort, DefaultLong, ShapeTests, AudioEngine, GraphicsMain, GraphicsOther,
  Secondary), the "dependency_spu" taskset with 4 tasks and an event flag, and Bink's "RAD Game Tools Task" tasksets;
- `sceNp2Init`, cellGame;
- its first file access, which needs the disc. Round 12 made the disc image readable.

## Right now

The next `scripts/boot.sh` run copies the disc image from the iPhone app to `inventory/gta5/disc.iso`, once
(`scripts/ios.sh pull-iso`). That copy hasn't been tried on a device. If `devicectl device info files` rejects its
flags or its JSON shape differs, fix `cmd_pull_iso`; or copy by hand (Finder → iPhone → Files → PSPocket → drag the
.iso into ~/Downloads, where boot.sh looks). After that the game reads its files, and the log shows the next gaps.

## Next steps, in order

1. **The boot loop.**
   1. Run `scripts/boot.sh`.
   2. Read the whole log, `inventory/boot-NNN.log`.
   3. Implement what it shows, following RPCS3 and citing it, with a test for each.
   4. Write a `docs/runs/0NN` entry (next is 015) and record decisions as D-045 onwards.
   5. Commit, push, repeat.

   Likely next modules, from the imports: cellSysutil (32), sys_io/cellPad (14), cellGcmSys (20, the start of RSX),
   cellAudio (11), cellRtc (7), sceNpTrophy (12), cellSaveData.
2. **Native SPURS stage 3b: running SPU tasks** (`docs/spurs-native.md`).
   - Host workers run task ELFs through `runtime/spu`, with the local-store layout and the task syscalls (0xA70) that
     Sony's SPU-side libraries expect. RPCS3 `cellSpursSpu.cpp` is a draft reference; libsre is the authority.
   - Today, waking SPU tasks from `cellSpursEventFlagSet` is a deliberate `PS_FATAL`.
3. **The libsre reference from the firmware.**
   - `inventory/firmware/PS3UPDAT.PUP` is on the Mac. Install it in RPCS3 (or extract dev_flash), then
     `rpcs3 --decrypt .../dev_flash/sys/external/libsre.sprx`.
   - Use it to check the assumptions below, then to build job chains (GTA V's main SPU work: 7 chains created, none
     run), SPURS queues (`_cellSpursQueueInitialize`, `cellSpursQueuePopBody`, ...) and `cellSheapAllocate`/`Free`.
   - It is a reference only: never committed, never shipped.
4. **Later:**
   - RSX → Metal up to the first frame;
   - audio and input;
   - lift the whole EBOOT (`ppu-lift`, 6.1 M instructions, already lifts in 10 s) instead of interpreting;
   - SPU lifting;
   - the iOS app booting the game from the ISO in its Documents.

## Assumptions to check against libsre

- D-039: `cellSheapInitialize` semantics (host-side heap, nothing written to the guest).
- D-041:
  - LS pattern helpers: loadable = blocks touched; read-only = blocks fully covered; context = 0x400 + 2 KB per block.
    GTA V's use matched in run 014.
  - Our own layout inside the opaque `CellSpursTaskAttribute`.
- D-042: the `_cellSpursTaskAttributeInitialize` signature. In run 013, r9 or r10 held 0x40; every call is now logged
  with its arguments.
- RPCS3 runs Sony's libsre by default (`g_prx_list`), so its `cellSpurs.cpp` HLE is a partial, little-exercised
  reference.

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
