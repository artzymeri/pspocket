# Where pspocket stands

2026-10-05, after run 069 (D-099). Read this first in a new session, then `CLAUDE.md`, `docs/decisions.md` (D-076
onwards matter most now) and the latest `docs/runs/` entries. Keep this file current when the picture changes.

## The project

A static recompiler that turns PS3 games into native, signed iOS apps: the game's PPU and SPU code is translated to
ARM64 ahead of time and its graphics go to Metal. It is not an emulator. One Mac-side build per game (disc image in,
app out); the runtime is shared by all games (D-036). GTA V (BLES01807) is the driving title (D-025); the goal is any
PS3 game through the same automatic pipeline (D-090). SPURS (the PS3's SPU job system) is our own native C++
implementation, with no Sony code in the app (D-037).

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
| Toolchain | `ps3inspect` (inventory), ELF/SELF/PRX loader, NID database from RPCS3 (D-019), ISO 9660 disc reader. PPU: interpreter (the oracle, with a decode cache, D-091), analyzer and lifter with differential fuzzing (D-023, D-028), whole-game translation built into the host (D-092 to D-096, D-098), `ppu-dis`. SPU: interpreter, runtime and lifter (D-029), relocatable programs (D-081), `scripts/lift-spu.sh` (D-076, D-099). One PPU/SPU reservation system (D-030, D-066) |
| lv2 kernel | Host threads, sync objects, memory, `sys_vm`, event queues and ports, timers, filesystem (host directories and disc images read in place, D-043), SPU threads (D-031), the RSX device (`sys_rsx`, D-045), a report of what every thread waits on (D-053) |
| HLE modules | sysPrxForUser, cellSysmodule, cellSpurs (native, checked against libsre: workloads, tasksets, tasks, job chains, SPURS queues and semaphores, a game's own policy module; the modules programs load themselves, D-079, D-082, D-099), cellAudio (D-052, D-072 to D-075), cellGcmSys, cellSysutil, sys_io (D-049), sceNp/sceNp2 (offline), sceNpTrophy, cellSaveData, cellRtc, cellVoice, cellNetCtl, sys_net, sys_fs, cellGame. 248 of GTA V's 362 imports; the rest are online, keyboard dialogs and shutdown |
| Loader | `pspocket-boot` runs a decrypted EBOOT on the Mac with the game's translated code built in (`-DPSPOCKET_GAME_PPU`, `-DPSPOCKET_GAME_SPU`; used only for the executable it was translated from, D-098). Crash reports show the guest registers and code, the host frames of translated code (D-094) and every thread. `--bdvd IMAGE.iso`, `--flash DIR`, `--frames DIR`, `--trace-frame N`, `--press`, `--spu-dump DIR`, `--rsx-recover` |
| RSX | Portable front end (FIFO methods, state, textures, vertex and fragment programs to MSL with flow control, D-068, D-086, D-089, D-093) and Metal backend (D-032): surfaces and textures over them, GPU conversions of depth buffers and colour targets read as other formats (D-083, D-088, D-097, D-098), frame trace (D-084) |
| iOS app | Boots the game from the ISO in Files (D-071); full screen; DualShock 4/DualSense or on-screen buttons; runs the lifted SPU programs and recovers from FIFO errors (D-084). Not yet: the translated PPU code |

## GTA V so far (docs/runs 004–069)

- Boots from the disc image read in place, through the logos, the Display Calibration screen (a scripted Cross,
  `--press`), "Loading Story Mode" and **into the first mission (the prologue's bank), lit and complete**.
- **Translated ahead of time**: all 85,376 PPU functions of the executable (`ppu-lift`, D-092 to D-096, 133 jump tables
  including the script VM's) and all 88 SPU programs and modules SPURS and the tasks load (`scripts/lift-spu.sh`, D-076,
  D-081, D-085, D-087, D-099). The interpreters only run what the translation has not reached yet, and log it as a seed
  for the next lift.
- Speed on the M4 Mac (run 069): the opening and loading screens and the first mission at 28 to 30 fps, the game's
  cap; "Loading Story Mode" about 20. On the iPhone (before the PPU translation): about 6 fps, pink, no people
  (fixed since on the Mac: D-086, D-088, D-093).
- Graphics (D-078 to D-098): deferred lighting, post-processing, skinned characters, shadow maps; depth buffers and
  colour targets read as other formats convert on the GPU. One 80x45 target is still read back each frame (an NV0039
  copy the game asks for).
- Sound through AVAudioEngine, time-stretched to the game's pace (D-072 to D-075).

## Right now

The Mac build with the game's PPU and SPU code translated runs the first mission at the 30 fps cap with no
interpreted SPU code left but four function-pointer targets of one job binary (re-lifted after run 069). What is
missing, roughly in order:

1. **A playtest on the iPhone with the translated PPU code**: `scripts/ios.sh` builds it in (D-101); the user
   installs it (`scripts/ios.sh install`, phone connected and unlocked) and reports the frame rate and what looks wrong.
2. **One command per game** (D-101): `scripts/translate-game.sh --iso GAME.iso --elf EBOOT.elf [--ios]` translates
   a game's PPU and SPU code and builds it in; the executable is decrypted with the user's own tools. Next: try it on
   a second game.
3. **RSX**: the first mission logs one failure (MSAA rendered with one sample, D-102); shadow lookups (D-100),
   occlusion queries, conditional rendering and 3D textures (D-102) run. Still refused when a game uses them: fragment
   CAL/BEM, vertex BRB/CLB, compressed 3D textures.
4. The per-frame 80x45 readback (make it asynchronous), the FIFO desync seen once in run 055 (the app recovers),
   sound pace on the phone.

## Building and running GTA V on the Mac

For any game, `scripts/translate-game.sh` does all of it (D-101). By hand, for GTA V:

```sh
# the PPU code (rule 1: everything under inventory/ stays out of git)
build/host/recomp/ppu/ppu-lift --elf inventory/gta5/decrypt/EBOOT.elf --out inventory/gta5/ppu-lifted --tu 512 \
    --name game_ppu --prefix game_ppu_ --no-comments
# the SPU programs a run saved with --spu-dump, plus the misses its log lists
scripts/lift-spu.sh inventory/gta5/spu inventory/gta5/spu-lifted inventory/boot-NNN.log
cmake -S . -B build/host -G Ninja -DPSPOCKET_WERROR=ON -DPSPOCKET_GAME_PPU=$PWD/inventory/gta5/ppu-lifted \
    -DPSPOCKET_GAME_SPU=$PWD/inventory/gta5/spu-lifted
ninja -C build/host pspocket-boot
build/host/runtime/loader/pspocket-boot inventory/gta5/decrypt/EBOOT.elf --timeout 200 --rsx-recover \
    --frames inventory/gta5/frames-NNN --frame-every 1500 --hdd0 inventory/gta5/hdd0 --bdvd inventory/gta5/disc.iso \
    --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu \
    --press cross@45 --press cross@55 --press cross@65 --press cross@75 --press cross@90 > inventory/boot-NNN.log 2>&1
```

Debugging translated code: a fault prints host frames for `atos -o pspocket-boot -s SLIDE ADDR...` (D-094);
`PSPOCKET_PPU_ONLY=LO-HI` keeps only some translated PPU functions; `PSPOCKET_SPU_TRACE_RUNS=N` shows why the module
tracker missed code; `--trace-frame N` logs every draw of a frame (D-084). Keep one build directory and delete old
frame dumps: the disk has about 10 GB free.

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
