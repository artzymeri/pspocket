# Where pspocket stands

2026-10-06, after run 101 and D-191 (route B: D-147; its stages 1 to 5 done, stage 6 reached on the Mac). Read this
first in a new session, then `CLAUDE.md`,
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
| HLE modules | sysPrxForUser, cellSysmodule, cellSpurs (native, checked against libsre: workloads, tasksets, tasks, job chains, SPURS queues and semaphores, a game's own policy module; the modules programs load themselves, D-079, D-082, D-099, D-136), cellAudio (D-052, D-072 to D-075), cellGcmSys, cellSysutil, sys_io (D-049), sceNp/sceNp2/commerce/SNS (offline, D-139), sceNpTrophy, cellSaveData, cellRtc, cellVoice, cellNetCtl, sys_net (offline loopback, D-141), cellHttp/cellSsl, sys_fs (with AIO), cellGame, cellOskDialog (D-140), cellSync (D-160). All 364 of GTA V's imports and all 140 of Assassin's Creed's (D-160). Every function RPCS3 implements in the libraries pspocket always serves (D-182 to D-189). The PS3's own firmware modules serve libraries the host has nothing of (LLE, D-157), and games load their own PRX modules (sys_prx, D-159) |
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

0. **Done (D-148, D-149):** the memory-spike fix and the regression gate (to confirm on the phone: a long session).
1. **Done (D-150):** the app signs and loads code it made itself (development install).
2. **Done (D-151, D-153):** LLVM in the app compiles the lifters' C on the phone.
3. **Done (D-152):** a game's translation is a library loaded at run time.
4. **Done (D-154, D-155):** "Preparing game…" on the phone, PPU and SPU, with progress and the time left; a
   translator generation makes libraries from before a lifter fix prepare again (D-161).
5. **Done (D-156, D-157, D-159):** firmware from PS3UPDAT.PUP, firmware modules (LLE), run-time PRX loading.
6. **On the Mac:** Assassin's Creed reaches its title screen, its first level and the Abstergo lab (26-34 fps with
   its SPU programs translated, run 099); GTA V passes the gate from a library made with tonight's lifter. **On the
   phone: not yet tried with these fixes** (the phone was locked): install the build, let it prepare both games
   again, then Assassin's Creed to its title screen and GTA V to the bank.

Rendering for any game (D-158): new shaders compile in the background with a 4 ms wait a frame, so a game the phone has
never seen does not stall; more texture formats decode (D-163). Open: the Abstergo lab looks washed out (compare with
footage), multisampled targets drawn with one sample, a GPU shader interpreter for the draws left out while compiling.

**The system software (2026-10-06, D-165 to D-178).** The engine works out per game which system libraries come from
Sony's firmware and which are pspocket's own (D-175, D-177), as a PS3 links them. The firmware is analysed in
docs/firmware.md (D-176: what it holds, the plan for each part). Its 100 modules that are not pspocket's own are
translated once into a system library, at fixed addresses, and checked one by one as they load (D-178). On the phone
that happens once, before the first game after the system software is installed.

**The libraries pspocket always serves itself are complete against RPCS3 (D-182 to D-189).** These are the ones with
no firmware fallback (D-175: liblv2*, libsysutil*, libio, libaudio, libnet, libnetctl, libcamera, libmic, libgem,
libusbd...), where a missing function used to stop an unknown game with ENOSYS. Every function RPCS3 implements there
is now pspocket's own, with RPCS3's checks and answers: the sysutil family, save data, dialogs, game data, input,
audio, devices and the network (D-183); the NP/PSN family offline (D-185); the media library utilities (D-186); AV
chat (D-187); the Japanese IME (D-188); recording, the sub display (D-189). The functions RPCS3 leaves as stubs answer
as RPCS3's do (D-182), and the kernel's gaps are filled (D-184). That is 1,536 host functions in 58 modules, plus 296
stub answers. Left out on purpose:

- raw SPUs and interrupt threads, which few retail games use (D-184);
- cellMicStopEx, fatal in RPCS3 too;
- cellSysmodule's OpenPSID decryption, which needs a key (D-184).

What no device or service backs is answered as the PS3 answers without it: PSN offline, no camera or microphone, an
empty media library, dialogs answered at once.

**The firmware's SPU programs are in the system library too (D-190, D-191, run 101).** The modules the system library
translates carry 171 SPU images (216 programs, 62 of them overlays): video and audio decoders, image decoders, video
post-processing. The programs of the modules a game links are translated into the system library before the game's
first start ("Preparing…"), and stay for every game after, so a new game's video and sound run native from the
first frame instead of on the SPU interpreter. All of them at once would be 19 minutes on the M4 and a 232 MB library,
mostly encoders and Blu-ray codecs, hence per game. On the way: the SPU analyzer no longer copies code for constants
that point into a function (one decoder lifted to 288 MB of C before, D-190); a SPURS task's program is the segment
holding its entry; spu-check checks a prepared library's SPU programs (all 216 agree with the interpreter). Not tried
on the phone yet.

**pspocket's own media libraries, each compared with Sony's (D-179, D-193, D-194, run 102).** A test loads the
firmware's module from the user's installed system software and sends it the same calls as pspocket's library
(tests/hle/firmware_compare.hpp; skipped without the firmware). cellPamf, the reader of movie headers: 207 of 207
calls the same (D-193). cellDmux, the demultiplexer that splits a movie into its video and audio: a demuxer's whole
life on a synthetic stream, every answer, callback and access unit the same, down to its address (D-194). The
comparison found that Sony's own libdmux could not run here (two SPURS queue functions were missing, now added) and
that RPCS3 asks for 0x50 bytes more memory than Sony's libdmux (pspocket follows Sony's). cellVdec, the video
decoder, on FFmpeg (D-195, D-196, run 103): Sony's libvdec now runs in the test too (it needed libsre's overlay tables
and SPURS task exit callbacks, now pspocket's), and pspocket's matches it: Sony's sizes for every codec and level,
pictures one access unit late as Sony's, items and H.264 information in Sony's places read from the stream, frame
buffers in Sony's layout, pictures exact (RGB within one step). VC-1, DivX 3.11 and MVC are not decoded yet.
cellAdec, the audio decoder (D-197, run 104): pspocket's own front end and LPCM decoder, the same as Sony's libadec to
the byte (221 answers and callbacks, 22 PCM outputs, every LPCM packing); every other codec goes to Sony's codec
module through pspocket's front end (the loader now links what a pspocket library takes from the firmware's modules),
also the same as through Sony's libadec. ATRAC3plus, the codec games use most, is now pspocket's own on FFmpeg (D-198,
run 105): on Assassin's Creed's menu music every answer is Sony's and the sound within 0.000001 of Sony's decoder.

The image decoders began with cellPngDec (D-199, run 106): pspocket's own on libpng, the same as Sony's libpngdec in
2,283 answers and 121 decoded images, every colour type, bit depth, output setting and chunk kind (Sony's libpng 1.2.8
quirks reproduced, its gray arithmetic found by fitting the pixels). cellJpgDec (D-200, run 107): pspocket's own
decoder, the same as Sony's libjpgdec sample for sample in 1,848 answers and 149 images, with SPU threads off and on,
progressive and damaged files included. libjpgdec turned out to be libjpeg 6b with Sony's own IDCTs (double float for
quality, 16-bit AltiVec for fast, float for the reduced sizes), AltiVec colour conversion, a 64-bit bit buffer, and its
own mean and memory estimate; all reproduced.

The outside sources the build reads (RPCS3 for the keys and libpng, the LLVM source of the phone's compiler) now live
in `external/` inside the repository, gitignored (D-201).

Next: cellGifDec, measured against Sony's the same way; the other audio codec cores on FFmpeg
(MP3, AC-3, AAC, ATRAC3, MPEG audio) when a game's log shows one; then
docs/firmware.md's iOS frameworks for the heavy media (ImageIO, VideoToolbox, AudioToolbox), and the phone's stage 6
with games nobody has analysed (each run's log names what is still missing).

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
frame dumps: the disk has about 65 GB free (2026-10-06).

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
