# Where pspocket stands

2026-10-05, after run 089 and D-149 (route B decided: D-147). Read this first in a new session, then `CLAUDE.md`,
`docs/decisions.md` (D-076 onwards matter most now; D-147 sets the current goal) and the latest `docs/runs/` entries.
Keep this file current when the picture changes. Where each part can improve, with the numbers behind it, is
`docs/improve.md`; how each game stands against the "Flawless" bar is `docs/compatibility.md` (both updated after every
run).

## The project

A static recompiler: a PS3 game's PPU and SPU code is translated to ARM64 ahead of time and its graphics go to Metal;
it is not an emulator, and the interpreters are the oracle and the fallback. **The goal since 2026-10-05 (D-147,
route B): any PS3 disc image picked on the iPhone plays, with no Mac and no rebuild per game.** The phone translates
each game once ("Preparing game…"), signs the result with the user's development certificate and loads it as a library;
the user's own firmware file (PS3UPDAT.PUP) supplies the modules our HLE does not cover. Until then the translation ran
on the Mac and was compiled into the app (D-036, D-090): that path stays as the developer's tool, and GTA V's
Mac-translated build keeps working throughout. GTA V (BLES01807) is the driving title (D-025); Assassin's Creed
(BLES00158) is the second (route B's acceptance). SPURS is our own native C++ implementation (D-037).

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
| HLE modules | sysPrxForUser, cellSysmodule, cellSpurs (native, checked against libsre: workloads, tasksets, tasks, job chains, SPURS queues and semaphores, a game's own policy module; the modules programs load themselves, D-079, D-082, D-099, D-136), cellAudio (D-052, D-072 to D-075), cellGcmSys, cellSysutil, sys_io (D-049), sceNp/sceNp2/commerce/SNS (offline, D-139), sceNpTrophy, cellSaveData, cellRtc, cellVoice, cellNetCtl, sys_net (offline loopback, D-141), cellHttp/cellSsl, sys_fs, cellGame, cellOskDialog (D-140). All 364 of GTA V's imports; 100 of Assassin's Creed's 140 (cellSync, AIO, spinlocks, mmapper and a few more missing). No PRX loading at run time yet |
| Loader | `pspocket-boot` runs a decrypted EBOOT on the Mac with the game's translated code built in (`-DPSPOCKET_GAME_PPU`, `-DPSPOCKET_GAME_SPU`; used only for the executable it was translated from, D-098). Crash reports show the guest registers and code, the host frames of translated code (D-094) and every thread. `--bdvd IMAGE.iso`, `--flash DIR`, `--frames DIR`, `--trace-frame N`, `--press`/`--press-file`, `--spu-dump DIR`, `--rsx-recover`, `--frame-jumps`, `--summary FILE.json` (D-149) |
| Gate | `scripts/gate.sh quick\|game\|spu` (D-149): ctest, then GTA V's prologue walk (`games/gta5/walk-prologue.txt`) checked against `games/gta5/gate.json` (fps, frame jumps, render passes, memory, compiles, new messages); `scripts/ios.sh build` and `testflight` run it first. `scripts/ios.sh session` summarises a phone log, `pull-log` fetches the game's |
| RSX | Portable front end (FIFO methods, state, textures, vertex and fragment programs to MSL with flow control, D-068, D-086, D-089, D-093) and Metal backend (D-032): surfaces and textures over them, GPU conversions of depth buffers and colour targets read as other formats (D-083, D-088, D-097, D-098), frame trace (D-084) |
| iOS app | A landscape game library over Documents/Games (D-142); decrypts each disc's EBOOT on the phone (D-121); GTA V's translated PPU and SPU code built in (route B replaces this with a library per game); the shader cache made ahead (D-143, D-145); memory, heat and the busiest threads in the log every 5 s (D-145, D-146); frees GPU caches when iOS warns about memory (D-148) |

## GTA V so far (docs/runs 004–083)

- Boots from the disc image read in place, through the logos, the Display Calibration screen (a scripted Cross,
  `--press`), "Loading Story Mode" and **into the first mission (the prologue's bank), lit and complete**. With the
  sticks scripted too (D-108), Michael walks through the bank, sets off the charge with the phone and reaches the vault
  corridor at 30 fps, with no SPU code interpreted (run 080).
- **Translated ahead of time**: all 85,376 PPU functions of the executable (`ppu-lift`, D-092 to D-096, 133 jump tables
  including the script VM's) and all 88 SPU programs and modules SPURS and the tasks load (`scripts/lift-spu.sh`, D-076,
  D-081, D-085, D-087, D-099). The interpreters only run what the translation has not reached yet, and log it as a seed
  for the next lift.
- Speed on the M4 Mac (run 069): the opening and loading screens and the first mission at 28 to 30 fps, the game's
  cap; "Loading Story Mode" about 20. On the iPhone (before the PPU translation): about 6 fps, pink, no people
  (fixed since on the Mac: D-086, D-088, D-093).
- Graphics (D-078 to D-098): deferred lighting, post-processing, skinned characters, shadow maps; depth buffers and
  colour targets read as other formats convert on the GPU. The 80x45 corner the game copies out of a target each frame
  is copied on the GPU, and guest memory gets it without the RSX waiting (D-107). GPU time 16 to 17 ms a frame.
- Sound through AVAudioEngine, time-stretched to the game's pace (D-072 to D-075).

## GTA V on the iPhone (runs 087 to 089)

Plays from the library at 30 fps in light scenes and 10 to 20 in heavy ones (CPU-bound: the SPURS workers, mostly
EDGE geometry jobs); the colour flicker is gone (D-144, confirmed by the user); the sound slows to 0.5 to 0.8 of its
pace when the cores are full (the one-SPU mixer instance is busy 85 to 90%); pipelines new to the shader cache stall
for up to a second; a feedback-copy memory spike stopped the app after ten minutes (fixed in D-148, to confirm on the
phone).

## Right now: route B, stage by stage (D-147)

0. **Done (D-148, D-149):** the memory-spike fix and the regression gate. To confirm on the phone: a long session in
   the heavy area of run 089 without memory warnings.
1. **Go/no-go:** the app writes a tiny ARM64 library itself, signs it on the phone and loads it (development install,
   then TestFlight). If iOS refuses both, route A.
2. Code generation on the phone, chosen by measurement (embedded LLVM compiling the lifters' C is the front runner:
   GTA V's whole translation is about 20 CPU-minutes on the M4, an estimated 6 to 12 minutes on the phone).
3. The translation as a per-game library loaded at run time, on the Mac first, with GTA V.
4. The on-phone pipeline ("Preparing game…").
5. Firmware from PS3UPDAT.PUP and the real firmware modules (PRX loading).
6. Acceptance: Assassin's Creed to its title screen and GTA V to the prologue's bank, both prepared on the phone.

In parallel, on the Mac: Assassin's Creed's runtime gaps, starting with cellSync (its main thread spins on
`cellSyncMutexTryLock`, 24 million calls in 90 s, and never creates the RSX).

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
