# Where to improve, part by part

A living list (the user's ask, 2026-10-05: "keep notes where to improve every part"): for each part, what the runs
show, what to try, and its state. Numbers come from the runs in docs/runs; an item that lands points to its D-number
and is struck through. Rule 8 holds: an optimization starts from a measurement and ends with one from the same scene.

Main source so far: the iPhone 17 Pro Max session of 2026-10-05 17:13-17:18 (GTA V, D-144 and D-145 installed;
2 performance and 4 efficiency cores, a GPU budget of 33 ms a frame at 30 fps).

## SPU and SPURS: the biggest cost in play

- **Seen:** in the heavier stretches of play (17:17:30-50) the main SPURS instance (5 SPUs) was 70 to 85% busy: EDGE/GFX
  7.6 to 9.4 s of SPU time per 5 s, dependency_spu 2.2 to 4.0 s, GraphicsMain 2.8 to 3.5 s, AudioEngine 1.3 to 2.2 s.
  The second instance (1 SPU, "Secondary") was 85 to 90% busy. fps fell to 14-28 there; the GPU had room.
- **Try:**
  - Profile EDGE/GFX's translated code on the Mac (Instruments, run 085's method) and make its hottest loops faster in
    the SPU lifter: shuffles and float work as NEON (D-127 cut SPU code by 14% this way).
  - EDGE geometry (culling, skinning) is a Sony library many PS3 games ship: a native NEON implementation of its jobs,
    recognised by hash, would serve every such game (goal: any ISO). Large; the payoff is the biggest cost in play.
  - Six SPU threads, the PPU threads and the RSX thread share six cores, four of them efficiency cores: measure the
    context switches and try fewer host threads for the 5-SPU instance (the native scheduler runs any workload on
    any thread, D-037), with QoS that keeps the SPU threads on the performance cores.
- **State:** open.

- **Seen (run 101):** a stop request (thread group stop or terminate, D-031) reaches lifted SPU code at back branches
  and calls, not at an indirect branch to a label of the same function (`bi` into its dispatch switch). A loop that
  goes round only that way cannot be stopped; spu-check met one on a random state (a return address equal to the
  function's own entry). No game is known to loop so.
- **Try:** an attention check where an indirect branch goes back into the dispatch switch (one relaxed load), measured
  against GTA V's walk (rule 8).
- **State:** open.

## Audio

- **Seen:** when the SPUs are saturated the sound runs at 0.52 to 0.80 of its pace (17:17:31-46: 15 to 95 periods of
  5 s skipped because the game was late, 400+ waits for the game): AudioEngine is one SPURS workload among the others.
  Earlier (run 087) the shader cache's threads did the same to it.
- **Try:** the native scheduler picks AudioEngine first when it has work (a priority boost for the workload that
  feeds cellAudio); a larger host buffer while the pace falls, so a late block stretches less.
- **State:** open.

## RSX and Metal: the GPU

- **Seen:** 25 ms of GPU time per frame at 1,504 draws and 83 render passes (55 ended by a target change, 19 by a
  target copy) in the heaviest stretch; 17 to 21 ms in most of play. The GPU is close to its budget too.
- **Seen later in the session (17:19-17:23):** the work per frame follows the scene: 800 to 1,900 draws, 55 to 96
  render passes, 400 to 1,130 texture checks. At 10 to 14 fps (17:22) the GPU took 26 ms of the 70 to 100 ms a frame
  lasted: the CPU side was the limit there. A 10 s dip to 5 to 7 fps (17:23:02) had 96 render passes a frame, 61 of
  them ended by target copies (12 to 23 elsewhere), and 33.6 ms of GPU time a frame.
- **Try:** fewer render passes (a target copy that does not need to end the pass; passes merged when the targets
  repeat); on a tile GPU every pass end writes its targets to memory: load and store actions that skip what the next
  pass does not read, memoryless depth where it is never sampled. Metal System Trace on the phone (needs the user)
  to see where the 25 ms go.
- **Multisampling:** surfaces with AA modes 3 and 5 render with one sample (logged): edges alias. Metal has
  multisample textures; resolve where the game reads them.
- **Blending:** ADD_SIGNED (0xf006) draws as ADD, as RPCS3 does (VKGSRender.cpp): only the log line says otherwise.
- **State:** open.

## Shaders

- **Seen:** the cache makes all 13,067 pipelines in 30 s at launch (D-143's order, 3 threads; D-145). In play, a
  pipeline the cache has never seen is compiled on the RSX thread: 11 to 12 at a time at 17:15:58 and 17:16:43 (0.9
  to 1.2 s of compiling in 300 frames), 87 in boot 5 of run 088 (5.4 s).
- **Try:**
  - Bring the phone's pipelines.txt back into the app's seed (scripts/ios.sh can pull the Caches directory), so a new
    install starts with what the phone has seen.
  - ~~Compile a new pipeline off the RSX thread and draw without it for the frames that takes~~: done (D-158,
    run 098), 4 ms of waiting per frame at most, then the draw is skipped. Next: a shader interpreter (one MSL
    program that runs RSX microcode, RPCS3's "async with shader interpreter") would draw those instead of leaving
    them out; tune the 4 ms on the phone.
- **Archives:** off on the phone (D-145): iOS keeps compiled pipelines between launches itself, and the archives cost
  gigabytes. After an iOS update its cache is empty and the first launch compiles everything (84 s and more, run 087).
- **State:** open.

## Write tracking (textures)

- **Seen:** 37 to 56 write faults per frame, 0.02 MB hashed; no stale textures since D-144 (the user: flicker gone).
- **Try:** nothing needed now. A guest page freed and committed again while watched keeps its tracker state (a
  texture there could go stale): drop the watches of a page the guest frees.
- **State:** fine.

## Memory

- **Seen:** 0.86 GB at the loading screens, 1.6 to 1.7 GB in play, of about 6.4 GB iOS allows (D-145's status line).
- **Try:** nothing needed now; keep reading the status line in every session.
- **State:** fine.

## PPU

- **Seen:** no per-thread measurement on the phone yet: the status lines show the SPUs, the GPU and the sound, not
  how busy the PPU threads are. In the 10 to 20 fps stretches neither the GPU nor the SPURS instances were full, so
  a PPU thread or the RSX thread is the likely limit.
- **Try:** read the busiest threads off the new status line (D-146) in the next session, then work on that thread.
- **State:** measurement built (D-146), not yet on the phone.

## Heat

- **Seen:** nominal in this session's first five minutes; "fair" after a minute of play in run 088.
- **Try:** keep the heat level in every session's notes; lower work (fewer threads, a frame cap) only if it reaches
  "serious".
- **State:** watch.

## The iOS app (library)

- **Seen:** the layout "in the first second" with the covers too high (run 087), not seen since; the layout log
  records the size at launch and on every change.
- **Try:** catch it in the log and fix it.
- **State:** open.

## Other games

- **Seen:** L.A. Noire (BLES00933), prepared on the phone (2026-10-06), stopped before its first frame on
  cellFsSdataOpen (its shaders are an encrypted SDATA file), cellSpursAddWorkloadWithAttribute and 12 more calls: all
  in D-165 now. Its SPU code is in its data files, so its first session runs it interpreted. Assassin's Creed (run
  099): all imports now there (D-160); title screen after D-161, its first level after D-162 (Joliet names).
- **Try:** L.A. Noire on the phone with the D-165 build: its next stops. Engine-wide (the user, 2026-10-06): system
  functions resolved by the engine like a PS3 links them (the firmware's modules wherever they can run, the native
  ones complete as RPCS3 has them), SPU thread groups, the whole RSX feature set, instead of game by game. Assassin's Creed in its first level runs
  at 3 to 4 fps on the Mac: its SPU programs interpreted (16.5 billion instructions in 120 s; the phone's preparation
  lifts them, D-155, so try it there or run pspocket-prepare with --spu-images from a --spu-dump), the job loop
  interpreted after fiber switches (D-161), syscall 327 (page-fault notification) still missing.
- **Fibers (D-161):** after a longjmp in translated code the frames above the jump target run interpreted until they
  return; a fiber-based job loop never does, so its loop stays interpreted (its jobs are translated). Each switch costs
  a C++ throw. Better: translate the code after a setjmp's return as a continuation the dispatcher can enter (a
  setjmp return address as a seed), so a jump lands in translated code.
- **Modules (D-159):** a game's own PRX modules run interpreted; the preparation could translate the PRX files on the
  disc too. NPDRM modules need the game's klicensee.
- **State:** open.

## Media libraries (D-179, D-193, D-194)

- **Seen:** cellPamf and cellDmux are pspocket's own and the same as Sony's on synthetic streams (run 102). No game on
  the Mac imports them; the phone's L.A. Noire uses cellVdec.
- **Try:** a real movie through cellDmux (a game that plays PAMF, on the phone or the Mac): the time per access unit
  and the callbacks per second. Each unit is one event through the SPU side's queue and one guest callback on the
  demuxer's lv2 thread; the SPU side's host thread sleeps 1.5 ms after each new stream (RPCS3's delay for White Knight
  Chronicles II). The raw elementary stream core (g_cell_dmux_core_ops_raw_es, which other firmware modules use) is
  not pspocket's: only libdmux's PAMF core is.
- **cellVdec (D-196):** Sony's item and information rings are 16 long in pspocket (Sony's length not known: decode
  more than 16 pictures in the probe to find it); the frame buffers' place and count are pspocket's; Sony's RGB rounding
  (9% of values one step off) could come from libvdec's conversion code; the H.264 information's fields past RPCS3's
  structure are inferred from two streams (try a stream with picture order count type 0, cropping, no VUI); VC-1,
  DivX 3.11 and MVC need FFmpeg decoders (vc1, msmpeg4v3: LGPL) and a comparison; MPEG-2 needs a synthetic writer to be
  compared. Speed on a real movie: FFmpeg on one thread, then VideoToolbox for H.264.
- **cellAdec (D-197, D-198):** the LPCM and ATRAC3plus decoders run on a PPU thread of their own per decoder (libadec
  and libatxdec use SPURS tasks); the LPCM conversion is scalar (RPCS3's is SIMD: measure on a real Blu-ray LPCM
  stream before changing). ATRAC3plus decodes 12.9 s of sound in 0.79 s on the M4 including the test's waits (run
  105): measure on the phone in a game that plays it. Its errors for bad frames are FFmpeg's generic one except a set
  start bit (Sony's codes per syntax error are not known). The other codecs are still Sony's cores (translated, SPU
  code on pspocket's SPURS): MP3, AC-3, AAC, ATRAC3 and MPEG audio next on FFmpeg, each compared with Sony's core. No
  synthetic writer for those yet: MP2, AC-3 and AAC could use FFmpeg's encoders in a test-only build; layer I frames
  are written in the test; a game's stream can be given to a probe locally (as SND0.AT3 in run 105).
- **cellPngDec (D-199):** decodes the whole image into a host buffer, then copies rows (and converts colour to gray)
  into the game's: one extra copy per image; measure on a game's loading screens before changing. The Ext functions
  (partial decoding with display callbacks, stream callbacks) are Sony's still: write them when a game imports one.
  libpng's NEON filters are off (PNG_ARM_NEON_OPT=0): measure a large PNG with them on.
- **cellJpgDec (D-200):** all scalar: the IDCTs that are AltiVec in libjpgdec (the fast one, the 1/2 one, the colour
  conversion) are written lane by lane in C++, and the whole image goes to a host buffer before rows are copied. Speed
  is not measured: time a game's large JPEG (a loading screen, a photo) on the phone before changing anything; NEON
  versions must keep the same rounding (the digest test and the comparison show it). The quality IDCT runs in double
  as Sony's does: keep it so. The Ext functions are Sony's still: write them when a game imports one. The SPU
  program's IDCTs (jpeg_dec_spu) were never used in the measured runs; if a game ever shows otherwise, compare again.
- **cellGifDec (D-202):** each image record decodes into a host buffer, then fills the whole screen into the game's
  (as libgifdec's output is). The LZW decoder follows giflib's line by line: fine for a game's few small GIFs; measure
  before changing. Not reproduced: the bytes past an extension in libgifdec's buffer (its own state) and a background
  index past the table. Ext functions still Sony's.
- **State:** open; the other audio cores when a game's log shows one.

## The gate (D-149)

- **Seen:** one walk failed on the fps floor (a single 5 s window at 19.8 fps at flip 2,400 while the Mac's GPU was in
  its slow state, no wait for the GPU, nothing on the walk's path changed) and passed when run again (run 091).
- **Try:** a lone fps-floor failure is run again once automatically; two in a row fail the gate. Keep a walk per
  Flawless game as games get there (docs/compatibility.md).
- **State:** open.

## Preparing a game on the phone (route B, D-151)

- **Seen (run 092):** GTA V's code takes about 14 minutes to compile on the iPhone at today's settings (8 on the M4's
  ten cores). Two files cost minutes each: an SPU program with 22 near-copies of one big function, a PPU unit whose
  one function is a few huge blocks. Big SPU programs peak at 0.5-0.9 GB each.
- **Try, each measured against the game's speed through the gate:** -fno-slp-vectorize for PPU code (that unit 35 s
  to 3.3 s, others 17% less); `-grow-region-complexity-budget=1000` for SPU code (2.3x faster, 30% less code); the SPU
  analyzer stops at other functions' entries instead of copying them (block starts repeat up to 3.7 times); per-unit
  prototypes instead of the 5.7 MB shared header (12%); clang built with PGO and LTO (Apple's is 1.4x faster than
  ours on the same file); LLVM IR from the lifters instead of C (no front end). A scheduler that starts compiles by
  predicted memory and keeps the biggest files on the performance cores; an ETA from predicted cost per file,
  corrected live (the user asked for an accurate one).
- **Seen (run 099):** Assassin's Creed's 22 SPU programs compiled 6 times faster than the cost model predicts
  (rate 0.16): the time left said 9:36 for a 0:41 job at 10%. Its programs are small next to GTA V's, which the model
  was fitted on.
- **State:** -fno-slp-vectorize (PPU) and the allocator budget adopted (D-153: GTA V 8:21 to 6:47 on the M4, the
  gate the same); the time left within 15% throughout (run 094) for GTA V and within 8-14% for GTA V again with
  the D-161 lifters (run 099). The SPU model cannot be refitted from the text (D-164: per-game speeds differ 6 times);
  the starting speeds now come from the game's own last preparation, else the device's PPU speed. Next: learn faster
  within a preparation (the first SPU files to finish set the rate; largest-first means they are the slowest).
- **Seen (run 101):** the firmware's SPU programs: one 57 KB decoder lifted to 288 MB of C, its constants each
  starting a copy of the code after them; 1.3 GB for all 216. D-190 makes such constants labels: 421 MB, still 19
  minutes on the M4 and a 232 MB library, so D-191 adds a module's programs only when a game links it. 28 bytes of
  C per byte of SPU code is the norm now (GTA V's 36): what is left is the lifter's own C per instruction.
- **Tests:** the timing-sensitive tests are robust now (thread report: CPU time; usleep(0): the backoff state, not
  wall time). Open: the SPU analyzer's other copies (call targets inside another function's code), per-unit
  prototypes, PGO/LTO clang, LLVM IR from the lifters, a scheduler that keeps the biggest files on performance cores.

## Stability: the EDGE/GFX job chain's descriptors written over

- **Seen:** CELL_SPURS_JOB_ERROR_NUM_CACHE or INVALID_BINARY on the EDGE/GFX chain now and then, the chain halts
  (D-130, D-131 on the iPhone, D-138 and run 095 on the Mac): a descriptor slot holds vertex-like data. Run 095's log
  names the writer: a job (binary 0x18fcc80) PUT 0x2000 bytes at 0x10394000, over the descriptor at 0x10395a00, 26 ms
  before the chain read it. The walk passes when run again.
- **Try:** which SPU program binary 0x18fcc80 is and where its output address comes from (its job descriptor's
  output list, the interpreter against the lifted program for that job: spu-check with that image); whether lv2 or
  the game's heap handed out the ring and the job's buffer overlapping (log both allocations).
- **State:** open; it stands between GTA V and an hour without a stop (the Flawless bar).
