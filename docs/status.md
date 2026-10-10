# Where pspocket stands

2026-10-07, after runs 192-207 and D-226 (route B: D-147; its stages 1 to 5 done, stage 6 reached on the Mac). **A new
session: read `docs/handoff.md` first** (what was in flight, the user's rules, commands), then this file, `CLAUDE.md`,
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
| Toolchain | `ps3inspect` (inventory), ELF/SELF/PRX loader, NID database from RPCS3 (D-019), ISO 9660 disc reader. PSN games (D-580 to D-583): their packages installed into dev_hdd0, the player's .rap licence, EDATA files and NPDRM executables decrypted with it (host tools; the app's screens for it to come). PPU: interpreter (the oracle, with a decode cache, D-091), analyzer and lifter with differential fuzzing (D-023, D-028), whole-game translation built into the host (D-092 to D-096, D-098), `ppu-dis`. SPU: interpreter, runtime and lifter (D-029), relocatable programs (D-081), `scripts/lift-spu.sh` (D-076, D-099). One PPU/SPU reservation system (D-030, D-066) |
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
never seen does not stall; more texture formats decode (D-163). Open (the lab's washed-out look was mostly missing shadows, D-214), multisampled targets drawn with one sample, a GPU shader interpreter for the draws left out while compiling.

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

- (raw SPUs, isolated SPUs and interrupt threads came later: D-528, through guest MMIO faults);
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
buffers in Sony's layout, pictures exact (RGB within one step). DivX 3.11, VC-1's advanced profile and MVC's base
view decode on FFmpeg too (D-588 to D-590), not compared with Sony's yet; VC-1's simple and main profiles are
refused (how Sony's decoder gets their sequence header is not known) and MVC's second view is not decoded.
cellAdec, the audio decoder (D-197, run 104): pspocket's own front end and LPCM decoder, the same as Sony's libadec to
the byte (221 answers and callbacks, 22 PCM outputs, every LPCM packing); every other codec goes to Sony's codec
module through pspocket's front end (the loader now links what a pspocket library takes from the firmware's modules),
also the same as through Sony's libadec. ATRAC3plus, the codec games use most, is now pspocket's own on FFmpeg (D-198,
run 105): on Assassin's Creed's menu music every answer is Sony's and the sound within 0.000001 of Sony's decoder.
MP3, MPEG audio layers I and II, AAC, AC-3 and ATRAC3 have pspocket's own decoders on FFmpeg too (D-577 to D-579),
firmware first: Sony's codec module serves where the system software is installed, pspocket's where it is not, until
each is compared with Sony's on the Mac. FFmpeg also builds on Linux now (D-576), so the media tests run in the cloud.

The image decoders began with cellPngDec (D-199, run 106): pspocket's own on libpng, the same as Sony's libpngdec in
2,283 answers and 121 decoded images, every colour type, bit depth, output setting and chunk kind (Sony's libpng 1.2.8
quirks reproduced, its gray arithmetic found by fitting the pixels). cellJpgDec (D-200, run 107): pspocket's own
decoder, the same as Sony's libjpgdec sample for sample in 1,848 answers and 149 images, with SPU threads off and on,
progressive and damaged files included. libjpgdec turned out to be libjpeg 6b with Sony's own IDCTs (double float for
quality, 16-bit AltiVec for fast, float for the reduced sizes), AltiVec colour conversion, a 64-bit bit buffer, and its
own mean and memory estimate; all reproduced.

The outside sources the build reads (RPCS3 for the keys and libpng, the LLVM source of the phone's compiler) now live
in `external/` inside the repository, gitignored (D-201).

cellGifDec (D-202, run 108): pspocket's own, the same as Sony's libgifdec in 1,131 answers and 85 images, with SPU
threads off and on (libgifdec is giflib; each DecodeData returns one record, an image being the whole logical screen).
With it, step 3's image decoders are pspocket's (their Ext functions are still Sony's).

**Assassin's Creed in the Abstergo lab, what still ran interpreted (runs 109 to 117, 2026-10-07).** A new game starts
in the lab, which renders right. Three gaps the logs named are closed, all general:
- a GCC switch whose bounds check branches into the table code and whose cases tail-call went through the PPU
  interpreter 100 times a session (D-203: 21 more tables found and 80 more bounded in the game);
- its busiest SPU job chain ran interpreted, 64 billion instructions in six minutes: prepare had promoted guessed
  code (data decoded as instructions) to real code by carrying every label over as a seed, and a job's own writes to
  its data then dropped its translation (D-205; D-204 and D-206 keep modules lifted when a copy cuts them or their
  guest buffer is reused). After: 1.2 CPU cores busy instead of 3.6, 60 fps without drawing (run 116);
- one module loaded twice a second differs in 18 data words each time (398 whole images a session): SPU programs are
  now also known by their code words alone (D-208), at run time and when preparing;
- the translator generation is 3, so the phone prepares its games again with these fixes.
With the GPU drawing, the walk's median went from 29 to 51 fps, the lab from 28-30 to 49-60 (run 117). The PPU's
interpreted share is now counted (D-207): in Assassin's Creed only its fiber resumes, about 14 instructions each, under
1% of a core (run 118). "Time left" no longer counts the files a preparation keeps (D-209). Open: one GPU command
reader desync (run 113, not seen since).

**Assassin's Creed in play (runs 120-125).** A walk that picks a memory in the Animus DNA timeline reaches the training:
Altaïr walks, the pause menu works, the "Gentle Push" lesson fills the Animus with a crowd (2,000 draws a frame). Profiled
there with macOS `sample`: the RSX thread re-decoded shader programs the game uploads again before every draw (D-210),
copied interleaved vertex data once per attribute and took a write fault per draw on fragment program constants
(D-211). Now about 11.3 µs per draw (13.0 before), the crowd at 30-32 fps (25-29), no write faults (GTA V's 40 a frame
to 0 too; the gate passes). Its fiber switches (12,000 a second) each threw a C++ exception, and the unwinder took
most of two threads: D-213 goes back by _longjmp, a quarter less CPU for the whole process (run 129). Its shadow
cascades are four depth buffers side by side sampled as one texture, and only the first was laid over the decode: the
rest came from stale memory, so the lab had almost no sun shadows. D-214 lays every target in the texture's rows over
it (and copies nothing when they cover it): the shadows are there now, and the lab walk's median is 57.7 fps (run 133).

**GTA V's SPU time (runs 134-137).** Profiled the same way: calls and function entry/exit were over a third of the SPU
samples, cflts 8.5%, byte rotates 5%. D-127's three changes, withdrawn on 2026-10-05 for stops seen later without them,
are back one by one, each checked with spu-check over all 150 of GTA V's programs (no difference) and the gate:
cflts in NEON (D-215), calls saving only what the callee touches (D-216), byte rotates in NEON (D-217). The main SPU
threads 12% less CPU, the audio SPU 15% less. Then each function's own register loads and stores (D-219: the main
SPU threads -6%, the audio SPU -11%; Assassin's Creed's crowd -7.7%) and the module tracker by line owners (D-220:
Assassin's Creed's crowd SPU threads -12%, runs 142-149).

**GTA V's EDGE/GFX job chain stop, found (runs 150-191, D-222 to D-225).** The stop that had halted GTA V's geometry
now and then since 2026-10-05 (the iPhone after 110 s in run 100; the Mac about once in ten walks under load) was an
8-byte job chain command written by an SPU job's PUT word by word (D-124's first word last): the chain read a
jump-to-self's low word alone, a light sync, and ran the previous lap's commands. A PUT's first eight bytes now land in
one store: 0 stops in 11 loaded walks, 3 in 10 before. Also: the module tracker's scan in NEON (D-223).

**A game's first session (D-226, docs/plan-game-modules.md).** Preparing a game now also translates the SPU programs
its executable carries (found by their ELF headers, as the system library's), so a game met for the first time runs
its SPURS tasks translated at once.

**SPU programs translated during the session (D-230 to D-236, docs/plan-background-spu.md, runs 217-219).** The SPU
programs a game meets that its library lacks (the modules and jobs its tasks copy in from data) are lifted, compiled,
signed and loaded on one background core while it plays; the SPUs switch to them at their next load or taken branch.
Assassin's Creed's first 150 s: 22.06 to 0.94 billion SPU instructions interpreted, the training 25-33 to 60 fps.
pspocket-boot --spu-translate on the Mac; the app starts it after the game library loads (not tried on the phone yet).
Also: the RSX reading a packet's arguments in one run was measured and not kept (D-229).

**A game's own PRX modules translated (D-237 to D-239).** The modules a game ships besides its executable are found on
its disc (or saved the first time it loads them), translated at fixed slots at the top of the PRX region into the
game's "modules library", and run translated once their code is checked; synthetic tests only, as neither local game
has one (run 220). Next: speed for the iPhone's heavy scenes (a fresh GTA V profile under load).

**GTA V's frame, profiled again (runs 221-245, 2026-10-08).** The RSX thread is the busiest single thread; fragment
programs found by content (D-240), the host clock in one multiply and idle sleeps by a Mach trap (D-241), texture
layouts kept and the FIFO translation inline (D-242), write notifications only for watched lines (D-243): the RSX
thread 1.69 to 1.55 CPU s per 5 s, the main SPU threads 8.8 to 8.3 under load. The kernel calls the firmware's own
modules make were counted (scripts/firmware-syscalls.py): the six a game can reach through them answered (D-244). Two
SPU-side tries measured and not kept (D-245 waiters on the line's word, D-247 stops as ways out). Where the SPU time
goes now: the EDGE geometry job is 35% of the translated SPU time (shufb, quadword loads and fma inside it); calls
and function entry and exit 18% across all programs, which only registers passed in host registers would cut.
Then (D-246 to D-254): primitive restart in indexed draws (D-249), multisampled targets drawn one texel per sample
(D-250: the gate's last RSX failure gone, its limit now 0), PPU units that declare only the functions they name
(D-251: GTA V's PPU code compiles 6-7% faster), input settled as a controller or the on-screen controls (D-252),
spu-check in every full gate (D-253, after Apple clang built one SPU program wrong with a tried change), and H.264 on
VideoToolbox (D-254: the same pictures, 77% less CPU). Measured and not kept: event flag sets without allocations
(D-246), corner copies at the end of a pass (D-248), a one-instruction byte reversal (D-253). A booted iOS Simulator
had been skewing GPU and fps numbers; shut down.

**No PPU code interpreted, and the RSX state games set now used (D-255 to D-267, runs 259-268).** A non-local jump
(Assassin's Creed's fiber switches) lands in translated code: the interpreter notes the landings and the return
sites above them, the next preparation translates them as continuations, and Assassin's Creed's walk interprets
nothing (7 continuations, from 1.5 million instructions a walk). A census of the registers the RSX stored without a
use found 7 that change what a game sees; all done: the fog factor in FOGC (D-256), two-sided lighting (D-257), the
clip range and depth clamp (D-258), user clip planes (D-259), constants and vertex bases written inside a draw
(D-260), the depth bounds test (D-261); then point sprites, polygon stipple, the alpha reference on float targets,
the window offset and the vertex output mask (D-262 to D-267). kTranslationVersion 4: shader caches are made again
once.

**Where GTA V's GPU time goes (D-268 to D-272, runs 269-303).** Each render pass timed, vertex and fragment apart
(PSPOCKET_GPU_PASS_TIMES): the geometry passes' vertex stages were the biggest part. Vertex programs put out only what
the fragment program reads (D-269, -2%) and indexed draws run by Metal over the game's own indices, so a shared vertex
runs once (D-271: -8 to -15% GPU time a frame in play). GPU changes are measured within one run (PSPOCKET_GPU_AB), and
the shader cache checks a fingerprint of the translation (D-272).

**Assassin's Creed plays its training clean (D-273 to D-276, runs 304-311).** Vertex textures of every kind (D-274),
points and WPOS on multisampled targets in pixels (D-275, D-276): a 600 s training walk with no PPU code interpreted
and no RSX failure (38 continuations). Its last minute is its heaviest, 31-43 flips a second on the M4 with the RSX
thread the busiest at 60% of a core: next to cut. kTranslationVersion 9.

**The RSX thread in that minute (D-277, D-278, runs 312-323).** Its time is spread thin (FIFO reading 19%, draws
47%); uploading only the constants a vertex program reads took 1.2% (an in-run A/B that now sums the RSX thread's CPU
per half). Reading the FIFO in stretches did nothing in GTA V (not kept, as D-229); a FIFO error inside a method's
handler now leaves the reader at put instead of taking every later word for an argument.

**Progress, counted (docs/progress, scripts/progress.py, regenerated before every push).** The README's Status section
shows how far each part is: system library functions pspocket implements itself against every one RPCS3 registers,
kernel calls, PPU and SPU instructions translated and fuzz-checked, a checklist per area (each item with its decision)
and the games against the Flawless bar.

Next: SPU registers passed in host registers between translated functions (docs/improve.md, the biggest cost in
play); vertex textures in 3D and cube; the new audio decoders (MP3, AAC, AC-3, ATRAC3, MPEG audio) compared with
Sony's codec modules on the Mac, then served first; then
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

## System libraries (D-500 on)

Every system library function is to be pspocket's own native code (D-500): the chart in the README at 100%, each
library compared with Sony's own module in tests where the firmware has one. GTA V and Assassin's Creed already need no
firmware module. Done so far (2026-10-10: 5,758 of 6,625, 86.9%):
- **liblv2 (sysPrxForUser).** Its printf family is liblv2's own small formatter (D-501), its heaps and malloc are
  dlmalloc 2.8.3 as liblv2 builds it (D-502), and its process exit list, panic and syscall wrappers are pspocket's
  (D-501); each the same as liblv2 in tests/hle/test_lv2_compare.cpp.
- cellHttpUtil (D-504), cellSpurs' accessors, exception handlers and SPU sharing (D-505, D-506, D-509), cellSheap
  (D-507), cellSpudll (D-508), the C library and C99 maths under sys_libc and sys_libm (D-510 to D-516), cellL10n
  (D-517), sys_fs with the kernel calls behind it (D-518), cellGcmSys compared with libgcm_sys running on pspocket's lv2
  (D-519), liblv2's internal variables (D-520), sys_lv2dbg (D-521), cellUsbd and cellUsbPspcm with lv2's sys_usbd as a
console with no USB device (D-522), lv2's network calls and libnet's own layer over them (D-523, part 1), cellHttp
(D-524: all 123 functions, libhttp's objects where libhttp lays them; past a connection blocked on libnet's resolver),
sysPrxForUser's raw SPU loaders (D-529, which also fixed sys_spu_segment's address offset), cell_FreeType2 (D-525,
FreeType 2.2.1 on the host), cellFontFT (D-526 part 1), cellSync2 (D-531, libsync2's objects bit for bit), cellAtrac (D-532, libatrac3plus over
pspocket's cellAdec), cellSsl (D-530), sys_io's config service (D-533), cellSysutil complete (D-534), cellAtracMulti
(D-535), cellFont (D-526 part 2), cellDaisy (D-538), sys_net part 2 (D-539) and gethostbyaddr (D-569), cellSail parts 1-12 (D-562: everything up
to playing; Start, Stop and the other playback calls are its next part, read from libsail on the Mac).
- **The C++ library, sys_libstdcxx, all 556 functions**: the foundation and the streams (D-560) and the facets (D-561:
  ctype, codecvt, collate, messages, numpunct, num_get/num_put, moneypunct, money_get/money_put, time_get/time_put),
  Dinkumware's of VC8's generation in the "C" locale. No Sony module to compare with (games link it into themselves).
  The C library's thread-local storage (D-570) and setjmp/longjmp (D-572) too.
- **Firmware first (D-563)**: libraries made from RPCS3 without Sony's module at hand are marked so that a game still
  gets Sony's module where the system software is installed, and pspocket's where it is not, until each is compared on
  the Mac: libmixer (the surround mixer over pspocket's cellAudio), cellVpost (D-564: YUV pictures scaled and converted
  to RGBA), cellKey2char (D-565: key codes to characters), cellPngEnc (D-566: PNG files from pictures, on libpng),
  cellFiber (D-568: fibers and contexts, each on a host thread that runs as its PPU thread while it holds the
  thread's baton), the video codec cores' version queries (D-575).
- cellGame's and cellPhotoImportUtil's working structures as their variables under RPCS3's names (D-567).
- The codec cores' operation tables (D-591, D-592): pspocket's own LPCM, ATRAC3plus, MP3, MPEG audio, AAC, AC-3 and
  ATRAC3 cores and its PAMF demuxer core, offered as the firmware modules' tables (and the MPEG audio and AAC PTS
  functions) that Sony's libadec and libdmux call; the FFmpeg cores firmware first. A module's imported variable the
  host exports is the host's (D-593).
- Unverified functions (D-594): functions made from RPCS3 in libraries pspocket compared with Sony's, marked so
  that where the system software has the library's module it still serves the whole library: cellPngDec's and
  cellGifDec's Ext functions (stream input, partial output with display callbacks, D-595), cellVdecOpenExt, SetPts
  and DecodeAuEx2 (D-596).
- On the Mac with the firmware (2026-10-10): libnet's resolver, gethostbyname and gethostbyaddr as libnet's own
  (D-539 part 4: the hosts file, nsdispatch and the DNS query, compared with libnet byte for byte, the queries
  included), so sys_net is complete; cellSpursJq's 63 with libspurs_jq's policy module from the system software
  (D-537, compared with libspurs_jq); liblv2's sysPs2Disc, sysBdMediaId and sysConsoleId (D-544).
- cellRudp's Init, End and event handler, and sys_crashdump's log areas, firmware-first (D-607): the last functions
  RPCS3 has bodies for. A scan of every grey function against RPCS3 found nothing more to make from it: the rest are
  stubs or bare names there.
- The threaded libraries checked for data races in a ThreadSanitizer build (D-571, -DPSPOCKET_TSAN=ON): cellFiber,
  libmixer, the C library's threads, cellPngEnc; then the whole runtime (D-584 to D-587, scripts/tsan.sh: 1,063
  reports to none, the guest code's own accesses suppressed with reasons). Fixed on the way: an SPU's call answered
  after a reply could land, a thread's stack freed after its join returned, RSX labels and FIFO pointers without
  ordering (a FIFO reset could lose a frame's commands), SPURS, libsync2 and the media threads' shared words.
Left (867, 2026-10-10). What each part needs (a session without the firmware could not make these faithfully):
- Sony's module, read or compared on the Mac (firmware_compare.hpp): cellSailRec (58)
  and cellSail's playback (19, libsail), libsnd3 (50) and libsynth2 (17), cellRudp (28, its wire protocol), the codec
  cores' modules (libsvc1d, libsmvd2/4, libsjvtd, libavcdec, libmvcdec past their versions, libdivx*, libvdec's 6, and
  the audio cores' operation tables and PTS functions, about 160 in all), cellJpgEnc (10, its structures), cellCelpEnc and
  cellCelp8Enc (19), cellJpgDec's Ext functions (5), cellNetAoi, libmedi, libad_core/async (32), cellSpurs' 13
  and cellSync's 6 (libsre: Task2, the LF queues' second forms), and the small ones
  (sys_lv2coredump, cellLibprof, cellSysconfPs1emu, cellGamePs1Emu, cellSysmodule's two,
  module management's six).
- The 188 functions answered as RPCS3's stubs, in libraries pspocket always serves: their arguments and answers from
  the libraries' modules.
- sys_libc and sys_libm's internals (260: Dinkumware's formatter, scanner and locale internals, the SPU printf
  helpers' read and write families): no firmware module has them; their arguments are to be read in a game's own copy
  of the C library.
- To compare on the Mac, then drop their firmware-first markers (D-563): libmixer, cellVpost, cellKey2char, cellPngEnc,
  cellFiber, the codec cores' version queries; and cellVdec's DivX 3.11, VC-1 and MVC. The comparisons are written
  (D-597: tests/hle/test_*_compare.cpp over compare_util.hpp, hidden from ctest until each matches): on the Mac,
  `PSPOCKET_TEST_DEV_FLASH=$PWD/inventory/firmware/installed/dev_flash build/host/tests/hle/test_hle "[compare]"`
  prints each library's differences from Sony's module. Writing them found a bug in cellPngEnc: a picture with more
  than one text chunk aborted its stream (D-598, fixed).
Kernel calls (2026-10-09): every named slot of the 634 is bound, 560 as pspocket's own (D-527 parts 1-4: devices,
services, root managers, PRX and overlays through the module loader, sys_rsxaudio; D-528: raw and isolated SPUs and
interrupt threads, their problem-state registers as guest MMIO decoded in the fault handler), the other 74 answered
as RPCS3's null or stub slots (debugger, crypto engine, colored memory: signatures not known).
Next (on the Mac, with the firmware): rerun scripts/build-ffmpeg.sh (macos, macos-tests and ios get the new video
decoders, D-588 to D-590); compare the firmware-first libraries, the new audio decoders (D-577 to D-579) and the new
video formats with Sony's modules and drop their markers; then libsre's last functions (cellSpurs' Task2 and LF queues, cellSync's) and the firmware-backed libraries above
one by one.
