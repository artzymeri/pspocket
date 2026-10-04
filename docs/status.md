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
| HLE modules | sysPrxForUser, cellSysmodule, cellSpurs (native, checked against libsre: initialize, workloads, tasksets, tasks, event flags, lv2 queue ports, barriers, job chains, SPURS queues; D-047, D-054, D-063; host SPU workers run SPU tasks, job chains and a game's own policy module with libsre's priorities, D-050, D-051, D-055, D-064), cellAudio (a host thread, ports, notify queues, no sound out yet; D-052), cellSheap (libsre's allocator), cellGcmSys (20, with the vblank interrupt thread; D-045), cellSysutil and AvconfExt (D-048), sys_io (virtual controller, no keyboard; D-049), sceNp/sceNp2 (offline, D-059), sceNpTrophy (trophy packs installed as RPCS3, D-065), cellSaveData (D-062), cellRtc (D-060), cellVoice, cellNetCtl, sys_net, sys_fs (20 cellFs functions), cellGame (6). 244 of GTA V's 364 imports; the rest are online, keyboard dialogs and shutdown |
| Loader | `pspocket-boot` runs a decrypted EBOOT under the interpreter on the Mac. Crash reports show r3..r10 and the caller's code (D-042), or for SPU code the host thread, pc, MFC parameters and registers (D-067). `--bdvd IMAGE.iso`, `--probe-disc`, `--flash DIR` (/dev_flash from the user's firmware, D-067), `--frames DIR`, `--dump-on-fault FILE` |
| RSX | Portable front end (FIFO methods, state, textures, shader translation to MSL with condition codes, D-068) and Metal backend (D-032). Driven by GTA V since run 016; its frames are saved as PNG (D-057): the logo, the legal screen and the Display Calibration screen draw as on the PS3 |
| iOS app | XcodeGen test host with device self-tests. On an iPhone 17 Pro Max, MetalFX 30 Hz runs steady and lifted PPU code runs 12× faster than the interpreter (runs 001–003). `-extract-iso` mode. The GTA V disc image (18.7 GB, decrypted at disc level) is in the app's Documents |

## GTA V bring-up so far (docs/runs 004–040)

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
- PSN set up offline (D-059), a CellSpursTaskset2;
- save data, the clock, voice, trophies, background music (D-060 to D-062, D-065), SPURS queues (D-063);
- four fixes found in runs 031 to 038: SPURS priorities were upside down (D-064), PPU `stwcx.` was not atomic so two
  threads could take one lock-free slot (D-066), the MP3 codec comes from /dev_flash (the user's firmware, D-067), and
  fragment programs need condition codes (D-068);
- **runs 039 and 040 end at their timeout without a crash**: the opening animation, the legal screen and the **Display
  Calibration** screen, which waits for the X button.

## Right now

The game shows its Display Calibration screen and waits for a button press (run 040, 150 s, no crash, no failed
draws). Nothing presses buttons yet: `pspocket-boot` needs a way to script the virtual controller (press X at a given
time) so runs can go on into the game's menus and loading. The two trophy calls in run 040's log are implemented now
(D-065); the next run will register the trophy set.
See the frames with `scripts/boot.sh --frames inventory/gta5/frames --frame-every 30`.

## Next steps, in order

1. **The boot loop.**
   1. Run `scripts/boot.sh`.
   2. Read the whole log, `inventory/boot-NNN.log`.
   3. Implement what it shows, following RPCS3 and citing it, with a test for each.
   4. Write a `docs/runs/0NN` entry (next is 041) and record decisions as D-069 onwards.
   5. Commit, push, repeat.

   A run that doesn't crash ends at its timeout with the thread report: every guest thread, what it waits on and its
   backtrace, and the RSX's state (D-053; `pspocket-boot --report-every S` prints it during the run too). A fault in
   SPU code can be read offline with `--dump-on-fault inventory/dumps/NAME.bin` (guest memory and the SPU's local
   store). Next: scripted controller input, then whatever the menus and loading need.
2. **Graphics.** The Metal backend draws the logo, legal and calibration screens. As the game gets further, its
   frames need more of the RSX: render targets, tiled surfaces, shaders, textures. Look at the saved frames every round.
3. **The libsre reference from the firmware** (D-046): decrypted in `inventory/firmware/elf/` (libsre, libspurs_jq,
   the two SPU kernels); `inventory/firmware/tools/fw_unself.py` remakes it from the PUP and RPCS3's source. libsre
   also holds the SPU modules as raw code: the system service workload (libsre 0x21480), the taskset policy module
   (0x23680, loaded at LS 0xA00, syscall entry 0xA70), two task exit handlers (0x25500, 0x25B80) and the two job chain
   policy modules (0x26300, 0x2A180). Use it for the SPU-side contract; the job chain policy (D-055) and SPURS queues
   (D-063) are done. It is a reference only: never committed, never shipped, and no disassembly of it goes into the
   repository. Separately, /dev_flash files a game loads itself come from the user's firmware (D-067):
   `fw_unself.py --raw NAME` copies them into `inventory/firmware/dev_flash`; so far only flashMP3.pic.
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
