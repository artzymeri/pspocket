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
- **Seen (runs 134-137, GTA V's walk profiled):** one geometry job is 35% of the main SPU threads; summed by SPU
  instruction, calls (brsl) 13.5%, function entry loads and exit stores 22%, cflts 8.5%, shufb 7%, rotqby 5.3%. D-215
  (cflts in NEON), D-216 (D-127's call saves back) and D-217 (byte rotates in NEON), each checked with spu-check over
  all 150 programs: the main SPU threads 12% less CPU, the audio SPU 15% less.
- **Tried (run 138), not kept:** inlining small leaf helpers (up to 32 instructions, 512 a function) into their
  callers: the fuzz tests passed, but GTA V's SPU time did not move (6.98 to 6.97 s per 5 s): since D-216 a call to
  a small leaf stores and loads only a few registers. What is left in the profile (run 137b): untagged code (the C
  compiler's own spills of the SPU registers it cannot keep in 32 NEON registers, 14%), shufb 11%, calls 9.5%, entry
  loads 9.4%, quadword loads 12%.
- **Done (runs 140-141):** shufb in five instructions (D-218, within noise); each function loads at its entry only the
  registers it may read before writing them and stores at each way out only those it may have written (D-219): the
  main SPU threads -6%, the audio SPU -11%. Profile after it: shufb 9.7% of the lifted samples, quadword loads 13.6%,
  untagged lines 15.5% (the C compiler's own spills, the dispatch), calls 6.5%, fma 6%.
- **Try next:**
  - Stores at the ways out barely fell (46.4 to 42.9 a way out): a callee must store whatever it wrote, since its
    caller might read it. Calls from lifted code could tell the callee which of its written registers the caller
    reads (live after the call); the run loop, which knows nothing, keeps the full set. Two entries per function, or
    a mask argument.
  - **Tried (D-247, runs 238-241), not kept:** a stop as a way out instead of a touch of every register: 3% fewer stores
    before calls statically, nothing measurable. Run 237's profile: entry loads 6.9%, calls 6.0%, exit stores 5.3% of
    translated SPU time; 30% of the calls' samples are calls that store 60 registers or more, into functions that use
    that many themselves. What would cut it: registers passed in host registers. A second entry per function taking
    up to eight of the registers it reads at entry as vector arguments (AArch64 passes them in v0-v7), the caller
    passing its locals instead of storing them; the callee stores an argument only when it leaves for the run loop or
    wrote it (its exits know which). Returns could come back in v0-v3 (a four-vector aggregate) the same way.
  - scripts/lift-spu.sh carries every label of the previous lift over as a seed (prepare keeps only the seeds a lift
    used since D-205): code only a constant or discovery reached becomes "real" there and its words count as code.
    Make the script keep NAME.seeds as prepare does.
  - quadword loads and stores reverse the bytes on every access (D-004); a load whose value only goes to a store, or
    the address arithmetic (a lane moved to a general register for each access), may be cheaper in another shape.
    **Tried (D-253):** the reversal as one table lookup instead of rev64 and ext: Apple clang built one program wrong
    with it (spu-check, 3 trials of 102,098; pspocket's own LLVM, -O0 and the sanitizer build it right). Not kept.
- **State:** open (D-215 to D-219 done).

- **Seen (runs 157, 166, GTA V's walk under a load of six spinning threads, `sample`):** the five main SPU threads
  spend about 12% of their busy time yielding (swtch_pri): waiting for the EDGE/GFX chain's lock (552 samples) and
  re-reading contended 128-byte lines in ps_res_acquire (300+, mostly select1: the SPURS kernel's workload selection,
  which every SPU makes after each batch of jobs, on the one SPURS line all of them update). On the Cell each SPU has
  its own core and its atomic unit; here the threads outnumber the free cores on the iPhone.
- **Tried (runs 158-166), not kept:** the chain lock's waiter sleeping on the line (ps_res_wait_change) after 32 looks
  instead of yielding: the chain-lock yields went to 0 but the same threads then piled up on the SPURS line (select1's
  yields 252 to 535); idle Mac unchanged, under load fps 28.2 to 28.75 but SPU CPU +5%. Reverted.
- **Tried (runs 192-205), not kept:** polls deciding on a plain copy of the SPURS line, through the reservation only
  when they store. Idle unchanged (7.21, 7.16 to 7.05, 7.34 s per 5 s); under load too noisy to tell (runs 198 and 203
  fell to 22-25 fps with less CPU, the Mac busy elsewhere); the profile under load (runs 204, 205) shows the select1
  yields not gone (142 to 217 samples): they come from the dispatch's selection and from DMA (ps_res_acquire in
  mfc_command), not from the polls. Reverted. Since D-223/D-225 the yields are smaller anyway: 379 samples of the
  SPU threads' 26,155 in run 204 (1.4%), against 1,256 of 21,145 in run 157.
- **Try:** a wait in ps_res_acquire that spins on the version alone (one load) before re-reading the 128 bytes, and
  only for lines it found changing; measure with profiles, not CPU totals, under load (the totals swing by 20%).
- **Seen (run 157):** ps_res_notify_write, called for every SPU PUT, bumps one version per 128-byte line, each in its own
  host cache line of the 4 MB table (2.4% of the SPU threads' busy time in GTA V): a 16 KB PUT touches 128 scattered
  cache lines. **Done (D-243, runs 230-233):** only slots someone reads a version of are bumped (a 4 KB bitmap, a
  Dekker pairing): the main SPU threads -3 to -8% under load.
- **Seen (run 221):** every store to the SPURS line wakes all the workers parked on it (a mutex and a notify_all in
  the storing thread, about 3% of the SPU threads' busy time), and each woken worker selects again for nothing.
  **Tried (D-245, runs 234-237), not kept:** waiters parked on the slot's version word itself
  (os_sync_wait_on_address, iOS 17.4): the wake's mutex contention went, but the wake system call costs what the
  broadcast did and CPU per frame did not move (0.103 s either way under load). Waking one waiter is not possible: each
  waits on a lost reservation, which every store to the line causes (the Cell sends each SPU its event).
- **Done (D-241):** the clock and idle sleeps of the SPURS workers (about 7% of their busy time): one multiply of
  mach_absolute_time, mach_wait_until.

- **Seen (run 101):** a stop request (thread group stop or terminate, D-031) reaches lifted SPU code at back branches
  and calls, not at an indirect branch to a label of the same function (`bi` into its dispatch switch). A loop that
  goes round only that way cannot be stopped; spu-check met one on a random state (a return address equal to the
  function's own entry). No game is known to loop so.
- **Try:** an attention check where an indirect branch goes back into the dispatch switch (one relaxed load), measured
  against GTA V's walk (rule 8).
- **Since D-219:** an indirect branch goes on inside its function only at the labels the analyzer found such branches
  may reach (jump tables, constants, seeds; 97 of GTA V's 4,056 functions have any); a return to the function's own
  entry or another label now leaves through the run loop, which checks for attention. The loop above is possible only
  through those labels.
- **State:** open, much narrower.

## Audio

- **Seen:** when the SPUs are saturated the sound runs at 0.52 to 0.80 of its pace (17:17:31-46: 15 to 95 periods of
  5 s skipped because the game was late, 400+ waits for the game): AudioEngine is one SPURS workload among the others.
  Earlier (run 087) the shader cache's threads did the same to it.
- **Try:** the native scheduler picks AudioEngine first when it has work (a priority boost for the workload that
  feeds cellAudio); a larger host buffer while the pace falls, so a late block stretches less.
- **Tried (runs 153-156), not kept:** the host threads that feed the sound found by what they write (cellAudio's port
  buffers and the samples cellAudioAdd*Data copies, through the SPU DMA write path; the PPU thread calling
  cellAudioAddData) and raised to cellAudio's own QoS. It found GTA V's two (the mixer's SPU thread writing 0x10c51000,
  the PPU thread adding it), but on the Mac with every core loaded by ten spinning threads of the SPU threads' class
  (which reproduces the phone: sound at 0.8 of its pace, 25 fps) nothing changed: pace 0.81 and 0.85 without, 0.80 and
  0.88 with; the mixer's SPU thread had the CPU it wanted either way (1.9-2.1 s per 5 s). The pace falls with the
  game's frame loop and its main SPU workloads, upstream of the mixer. On the phone the mixer may be held on an
  efficiency core (it was 85-90% busy there): try the raise again in a phone session, with the threads' core types
  in the log. Reverted until then (rule 8).
- **State:** open; making the SPU work faster is what moves it.

## RSX and Metal: the GPU

- **Seen (run 221):** the RSX thread is the busiest single thread of GTA V's frame (42% of a core), 78% of it in draws:
  fragment programs (19%: re-decoding those GTA V writes again into memory it reuses), texture binds (9%), vertex data
  and constants copied into Metal buffers (7.5%, needed: the GPU runs behind the game, which reuses its buffers).
  **Done (D-240, D-242):** programs by content, texture layouts kept, the FIFO translation inline: the RSX thread
  1.69 to 1.55 CPU s per 5 s (idle). **Left:** per-draw allocations (the overlay lists of texture binds, the compile
  key's strings on a program's first options), the dispatch per method.
- **Seen (gate):** the GPU spends 24-26 ms of a frame in play on the M4 with 84 render passes a frame; on the iPhone
  20-27 ms (run 096). **Try:** count what ends each pass (a target change, a copy, a readback, a clear) in a frame and
  merge the ones Metal can keep open (a tile-based GPU pays each pass's loads and stores).
  **Counted (runs 243, 245):** of the passes a target change ends, 84% go to other colour targets, 14% keep the colour
  targets with another depth buffer, 2% are a draw without depth after one with it. Corner copies are all of a target
  the same pass draws to (D-248, not kept). What is left to merge is small; the cost per pass is the lever: load and
  store actions that skip what the next pass does not read (needs knowing the next use, a frame later at the
  earliest), and memoryless depth where a depth buffer is never sampled nor reloaded.

- **Seen (runs 121-125):** in Assassin's Creed's crowd (2,000 draws a frame) the RSX thread is the busiest thread.
  D-210 and D-211 took it from 13.0 to about 11.3 µs per draw (decodes gone, vertex data copied once, no write
  faults on fragment programs). Left: the command reader per word (step_commands, dispatch, io_to_ea: about 15%:
  one atomic counter, a put read and an IO lookup per word), texture uploads (6.7%), texture watches (mprotect, 7%),
  the transform constants copied whole (7.5 KB) after any write.
- **Try:** read the FIFO in runs of words within one IO page (one lookup, one put read a run); the constants copied
  from the lowest written to the highest used register; textures that change every frame hashed instead of watched.
  **Done (D-278, run 323):** only the constants the vertex program reads: 1.2% less RSX thread CPU in the training's
  heaviest minute. **Tried again (D-277, runs 313-322), not kept:** the FIFO in stretches: 4x faster in a benchmark
  against a guest polling get (tests/rsx/bench_fifo.cpp), nothing in GTA V; worth trying again on the iPhone, where the
  get register's cache line may cost more between clusters.
- **Seen (run 312, the training's heaviest minute, 2,000 draws a frame):** the RSX thread 13.5-16 ms of CPU a frame:
  the FIFO reader 19%, draws 47% (texture binds 5.1%, the fragment program's lookup 4.6%, Metal's draw call 4.7%,
  copies 5.1%), no single cost above a few percent. What would cut it as a whole: fewer state checks per draw (most
  draws change one or two registers), encoder state set only when it changed (the viewport, cull and depth bias are
  set on every draw), and the fragment program lookup kept while its registers and bytes are unchanged.

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
  **Done (D-945, part 1):** clears wait instead of ending the pass drawing: a clear of whole targets is the next pass's
  load action, a clear of part of them its first draw, the rest run just before the next pass; a pass does not store
  what the next one clears whole. A synthetic frame shaped like GTA V's (run 324): 57 to 53 passes, 137 to 120 MB
  loaded and 145 to 127 MB stored a frame; GTA V's traced frame by the same rules 85 to 81 passes, about 17 MB less
  each way. **Not measured on GTA V yet** (its disc image is not on the Mac): `PSPOCKET_GPU_AB=30 scripts/gate.sh game`
  gives both halves' passes, MB and GPU time. Copies still end passes: GTA V's need what the pass drew or come with a
  target change (D-248); memoryless depth needs knowing no later pass loads the buffer, and every depth buffer of the
  traced frame is read again. **Left:** D-273's subset draws kept in the larger pass (most of what is left: the
  G-buffer's switches to its first target alone), to try on the iPhone.
- **Multisampling:** done (D-250): multisampled targets are drawn one texel per sample, as the RSX lays them out, and
  the gate's last RSX failure is gone. WPOS there is the pixel's (D-276).
- **Blending:** ADD_SIGNED (0xf006) draws as ADD, as RPCS3 does (VKGSRender.cpp): only the log line says otherwise.
- **Where the GPU time goes (D-270, runs 272-279):** PSPOCKET_GPU_PASS_TIMES=1 times each render pass, vertex and
  fragment apart. GTA V: 17-18 ms in 89 passes; the G-buffer pass 2.7-4.2 ms, mostly its vertex stage; the geometry
  passes' vertex stages about 3.5 ms a frame. Done: vertex outputs only as read (D-269, -2%); indexed draws run by
  Metal over the game's indices, shared vertices once (D-271: -8 to -15% GPU time a frame, the G-buffer pass's vertex
  stage -58%). Tried: whole-word vertex loads (D-268, no gain). Measure GPU changes with PSPOCKET_GPU_AB (D-272):
  between runs the walk's GPU time swings by 2 ms. Next: fans, quads and polygons the same way (their converted
  lists still pull indices), the post-processing chain (0.3 ms a pass on 160x90 to 640x360 targets), the 64x32 pass
  (0.8 ms in the timing mode, 2 microseconds alone: likely the timing mode's), the fragment stage of the G-buffer
  and lighting passes.
- **Render pass traffic (D-273):** GTA V loads about 188 MB and stores 209 MB of attachments a frame (status line).
  Keeping draws to part of the open pass's targets in it cut that by 30% but cost the M4 up to 7% GPU time (not
  kept). Try it on the iPhone when it can be measured; other cuts: a store the next pass's clear makes dead, depth
  buffers no later pass reads (memoryless). **Done (D-945):** clears folded into the next pass and stores the next
  pass's clear makes dead (above); the status line counts the MB not stored.
- **State the RSX keeps that the backend ignored (census, 2026-10-08):** of 32 registers stored without a use, 7 change
  what a game sees. Done: the fog factor in FOGC (D-256), two-sided lighting (D-257), the clip range and depth clamp
  (D-258), user clip planes (D-259), constants and vertex bases written inside a draw (D-260), the depth bounds test
  (D-261); then the rest of the list: point sizes and sprites (D-262), polygon stipple (D-263), the alpha reference on
  float targets (D-264), the window offset (D-265), an exported depth's clamp (D-266: Metal already did it), the vertex
  output mask (D-267), vertex textures of every kind (D-274), points on multisampled targets (D-275). The census's list
  is done.
- **State:** open.

## Shaders

- **Seen:** the cache makes all 13,067 pipelines in 30 s at launch (D-143's order, 3 threads; D-145). In play, a
  pipeline the cache has never seen is compiled on the RSX thread: 11 to 12 at a time at 17:15:58 and 17:16:43 (0.9
  to 1.2 s of compiling in 300 frames), 87 in boot 5 of run 088 (5.4 s).
- **Try:**
  - Bring the phone's pipelines.txt back into the app's seed (scripts/ios.sh can pull the Caches directory), so a new
    install starts with what the phone has seen.
  - ~~Compile a new pipeline off the RSX thread and draw without it for the frames that takes~~: done (D-158,
    run 098), 4 ms of waiting per frame at most, then the draw is skipped. **Done (D-940):** the shader interpreter
    draws those instead, once its own pipeline for the draw's targets is made (the shader cache makes them ahead at
    the next launch). Next, on the phone: how many draws a first session interprets and what they cost the GPU
    (the report line's "drawn by the interpreter"), and the 4 ms wait, which can now be smaller.
  - **Weighed (2026-10-08), not started:** an interpreter only helps if its pipelines exist before the draw, and a
    Metal pipeline is compiled per vertex and fragment function *and* per attachment formats, blending and write masks:
    the interpreter's own pipelines would still be compiled on first sight of each such combination (fewer, but not
    none). A shader is new once per install (the cache keeps it), so the gain is a game's first session. Before
    building it: count on the phone how many draws a first session leaves out, and for how long.
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

- **Seen (runs 126-127, Assassin's Creed's crowd):** the module tracker's GET observer (D-079) is 11-12% of the busy
  SPU time: 1.6 to 3 million GETs a second at peak, 85% of them 256 bytes or less (641 on average), each testing about
  12 runs of the 1 KB blocks it covers and cutting or dropping 1.4 of them, about 240 ns a GET (scattered reads of a
  250 KB per-thread table). A free-slot list and run extents kept in the blocks changed nothing measurable (reverted).
  Small GETs cannot be skipped: GTA V has real code modules of 32 to 176 bytes.
- **Try:** record GETs in a cheap per-SPU ring and build the run holding pc only when the run loop misses lifted code
  (resolves are rare next to GETs), with the resolved runs cached and checked against later GETs; or take a DMA list
  as one event.
- **Done (D-220, runs 142-145):** each 16-byte line of local store knows the run that wrote it last; a run lives while
  it holds a line. The crowd's SPU threads -12% (13.16 to 11.59 s per 5 s). Left: a 16 KB GET walks its 1,024 lines
  twice (release, then the fill), so outside the crowd (bigger GETs) the tracker costs about what the old one did
  (3.4-3.7% of the SPU threads' samples, run 145's profile): scan the lines four at a time (NEON), or skip lines a
  single run holds whole.
- **Seen (runs 109 to 116):** Assassin's Creed's busiest job chain (workload 1, 14 to 15 s of SPU time per 5 s) ran
  interpreted, about 64 billion instructions per six-minute session: the prepared programs counted guessed code
  (shuffle patterns, zeros) as instructions, and a job's own writes there dropped it (D-205). Fixed: 1.5 s of SPU time
  per 5 s, run 116. Still interpreted: LS 0x0e408 and 0x0e740 "in no program" (1.9 and 0.3 billion instructions in
  run 114), code no tracked copy holds.
- **Try:** trace 0x0e408 with PSPOCKET_SPU_TRACE_RUNS (what copies it in), then track that kind of copy.
- **Seen (runs 133-145):** 0x0e408 and 0x0e740 no longer run interpreted; the only SPU code interpreted in a session is
  one module entry, 184 instructions (LS 0x33990, D-208).
- **State:** the job chain fixed (D-204 to D-206); 0x0e408 gone since D-205 to D-208.

## PPU

- **Seen:** no per-thread measurement on the phone yet: the status lines show the SPUs, the GPU and the sound, not
  how busy the PPU threads are. In the 10 to 20 fps stretches neither the GPU nor the SPURS instances were full, so
  a PPU thread or the RSX thread is the likely limit.
- **Try:** read the busiest threads off the new status line (D-146) in the next session, then work on that thread.
- **State:** measurement built (D-146), not yet on the phone. Interpreted code is now counted by owner (pspocket-boot)
  and logged every 5 s by the app (D-228): read both in the next phone session.

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

- **On-screen controls (D-252):** they work (multi-touch buttons, two sticks, hidden while a controller is connected).
  **Try:** a thumb sliding from one button to the next presses the next (one view hit-testing every touch instead of
  a gesture per button), a light haptic tap on press, an opacity setting, L2/R2 as analog triggers.

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
  a C++ throw. Measured (run 118, D-207): Assassin's Creed resumes fibers about 1,000 times a second, each landing at
  its setjmp's return (0xcf6d90) and running about 14 instructions interpreted: 3.1 million in 230 s. In later scenes
  12,000 a second, and the C++ exception per switch was most of the task threads' time (the unwinder); D-213 goes
  back by _longjmp: the process a quarter less CPU in the crowd (run 129). Left: the few instructions after each
  setjmp, interpreted. Better: translate the code after a setjmp's return as a continuation the dispatcher can enter (a
  setjmp return address as a seed), so a jump lands in translated code.
  **Measured (run 258, 600 s training walk, 2026-10-08):** 18.4 million instructions from 3.5 million landings at
  0xcf6d90, about 5 a landing (0.1% of a core). The code there tests a flag, then calls the fiber switch again or
  returns to its caller, which the interpreter runs until its next call. A continuation entry needs, besides the label
  (lifted PPU functions load every register they use at their top, so a `goto` after them works), the stack pointer
  the function's return checks against (the resume point's r1 plus the frame its prologue made: 160 here), and the
  callers' return points as continuations too, or the interpreter takes over again one frame up.
  **Done (D-255, runs 259-268):** landings and the return sites above them are learned by the interpreter (pspocket-boot
  `--ppu-landings`, the app every 60 s) and translated as continuations by the next preparation; Assassin's Creed's
  walk interprets no PPU instruction now (7 continuations). Left: a landing in a function without an ABI frame is
  refused (none so far); on the phone each new set of landings costs one more preparation of the game.
- **Modules (D-159):** a game's own PRX modules ran interpreted. **Done (D-237 to D-239):** they are translated at
  fixed slots at the top of the PRX region, found on the disc before the first session or saved when the game loads
  them, checked before linking, and run translated (synthetic tests; no local game has modules: run 220).
- **Left:**
  - A module whose reference tables patch its code stays interpreted: lift those instructions as loads from the
    patched words (needs a game that has them, rule 4).
  - A newly found module recompiles all of the game's modules: per-module lift folders, as plan-game-modules commit 8
    describes, if a game has many.
  - **PSN games (D-580 to D-583): done on the engine side.** A PSN game's NPDRM EBOOT.BIN, SPRX modules and SPU
    images decrypt with its licence, and its EDATA files open through lv2 with the keys the game shows with
    sceNpDrmIsAvailable, all with RPCS3's checks (tested against RPCS3's own code where the build has its source).
    The player's licence is their <CONTENT_ID>.rap, in /dev_hdd0/home/00000001/exdata (Documents/hdd0/home/00000001/
    exdata on the phone, --hdd0's on the Mac); free content needs none. The game's .pkg files (the game, then its
    updates) install into dev_hdd0/game/<TITLE_ID> as RPCS3 installs them (D-583: on the Mac
    ps3disc --install-pkg, and ps3disc --install-rap for the licence). Left: the app's side, offering
    ps_psn_install_pkg and ps_psn_install_rap for files the player brings, and a library of installed PSN games that
    calls ps_psn_decrypt_executable; scripts/translate-game.sh still takes a disc image only. No PSN game has been
    tried yet.
  - Sniper: Ghost Warrior's engine_jobs.sprx (seen on the phone, D-159) is the first real check, when its disc is
    here.

- **Seen (run 207, D-226):** a game met for the first time ran all its SPU code interpreted until its next launch.
  Now the executable's SPU ELFs are translated when it is prepared (Assassin's Creed: 6, its 4 tasks under the runtime's
  names). Left: the modules and jobs a task copies in from game data (Assassin's Creed's at LS 0x2fba0 and 0x04c00: 21
  billion instructions in a 150 s first session), and a game's own PRX modules (docs/plan-game-modules.md).
- **Done (runs 217-219, D-230 to D-236):** the SPU programs a session meets untranslated are translated while it runs,
  on one core in the background, and loaded as they are ready: Assassin's Creed's first 150 s interpret 0.94 billion
  SPU instructions instead of 22.06 billion, the training at 60 fps instead of 25-33. Left: the first 10 s (before
  the first programs land: 0.75 billion) and the windows where new jobs appear (up to 79 million per 5 s for a few
  seconds); about 0.3 million per 5 s after (entries the lifts had no seed for).
- **Try:**
  - Record where each module a task copies in comes from (guest address, size, the file and offset it was read from);
    when that is the executable or a plain file on the disc, find the same bytes when preparing: the first seconds
    translated too.
  - The phone: compiles run slower and the cores are full with the game; measure the time from first sighting to
    loaded there, the heat, and whether the 60 s limit ever defers a program (the cost model is 8 to 100 times too
    pessimistic for Assassin's Creed's code; the session scales it by its own compiles).
  - A program's later entries (the 0.3 million per 5 s): lift the image again with the misses as seeds and load it over
    the first (a registry "supersede"; tables are never freed, D-231), if the numbers grow in other games.

## Media libraries (D-179, D-193, D-194)

- **Seen:** cellPamf and cellDmux are pspocket's own and the same as Sony's on synthetic streams (run 102). No game on
  the Mac imports them; the phone's L.A. Noire uses cellVdec.
- **Try:** a real movie through cellDmux (a game that plays PAMF, on the phone or the Mac): the time per access unit
  and the callbacks per second. Each unit is one event through the SPU side's queue and one guest callback on the
  demuxer's lv2 thread; the SPU side's host thread sleeps 1.5 ms after each new stream (RPCS3's delay for White Knight
  Chronicles II). The raw elementary stream core (g_cell_dmux_core_ops_raw_es, which other firmware modules use) is
  not pspocket's: only libdmux's PAMF core is.
- **cellVdec (D-196, D-588 to D-590):** Sony's item and information rings are 16 long in pspocket (Sony's length not
  known: decode more than 16 pictures in the probe to find it; the memory size formula, RPCS3's and Sony's, counts 48
  item and information pairs for H.264 and MVC and 32 for the others, a hint); the frame buffers' place and count are
  pspocket's; Sony's RGB rounding (9% of values one step off) could come from libvdec's conversion code; the H.264
  information's fields past RPCS3's structure are inferred from two streams (try a stream with picture order count
  type 0, cropping, no VUI); MPEG-2 needs a synthetic writer to be compared. DivX 3.11 (FFmpeg's msmpeg4v3, its frame
  size from the codec specific information), VC-1's advanced profile (vc1, its sequence header and entry point as the
  decoder's configuration, range mapping applied) and MVC's base view (H.264 without the other views' NAL units)
  decode now, none compared with Sony's yet. **Try (on the Mac, with the firmware; build-ffmpeg.sh run again first,
  for the new decoders):** test_vdec_codecs.cpp's streams (DivX 3.11 and MPEG-4 from FFmpeg's encoders in
  build/ffmpeg/<host>-tests, VC-1 from vc1_writer.hpp, two-view MVC from h264_writer.hpp) through Sony's libvdec as
  test_vdec.cpp's comparison runs: the information blocks (CellVdecDivxInfo's fields, CellVdecVc1Info's layout, which
  is pspocket's own, CellVdecMvcInfo, written as H.264's), where libdivx311dec takes DivX 3.11's frame size from,
  whether libsvc1d applies VC-1's range mapping, what libmvcdec gives of the second view, VC-1 access units before a
  sequence header; then the framing of VC-1's simple and main profiles (refused now). Still not decoded: MVC's second
  view (no MVC decoder in FFmpeg). Speed on a real movie: FFmpeg on one thread, then VideoToolbox for H.264 and MVC's
  base view.
- **cellAdec (D-197, D-198, D-577 to D-579):** the LPCM and ATRAC3plus decoders run on a PPU thread of their own per
  decoder (libadec and libatxdec use SPURS tasks); the LPCM conversion is scalar (RPCS3's is SIMD: measure on a real
  Blu-ray LPCM stream before changing). ATRAC3plus decodes 12.9 s of sound in 0.79 s on the M4 including the test's
  waits (run 105): measure on the phone in a game that plays it. Its errors for bad frames are FFmpeg's generic one
  except a set start bit (Sony's codes per syntax error are not known). MP3, MPEG audio layers I and II, MPEG-4 AAC,
  AC-3 and ATRAC3 have pspocket's own cores on FFmpeg too (adec_ffmpeg.cpp and a file each), which serve only where no
  firmware module gives Sony's core (firmware first, as D-563 for libraries). D-944 compares them with Sony's
  (test_adec_cores.cpp, [.compare-adec], PSPOCKET_TEST_DEV_FLASH set; PSPOCKET_TEST_DUMP, _SHIFT and _LAG print more):
  the sizes, versions and PCM handles are Sony's, and MP3, layers I and II, ATRAC3 and AAC agree in all 45 cases.
  **Next:** AC-3 (D-944 part 3): Sony's first frame is an ERROR 0x80612503 with no PCM; its StartSeq refuses only
  enums out of range, more than 6 channels and channel pointers outside 0 to 5 (libadec's ARG), not the output mode,
  scale factors or two pointers alike; its pointers put each channel in a slot (the later overwriting), slots past
  numberOfChannels dropped; its downmixes take A/52's levels without normalising and auto means Lt/Rt; a scale past 1
  gives silence; its information is 0x50 bytes (a codec word 0x1001, version 0x0499, the bit rate in bit/s, the rate
  as MPEG-4's index, 0xFF or 3 for float or 16-bit, the BSI's bits in 24-bit words). Then drop the firmware-first rule
  codec by codec. A game's stream can be
  given to a probe locally (as SND0.AT3 in run 105). Still Sony's only: MPEG-2 AAC, E-AC-3, MP3 surround, ATRAC3
  multichannel, TrueHD, DTS and WMA (RPCS3's cellAdec.h has no structures for them), and CELP (FFmpeg has no decoder).
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
- **State:** open; the audio cores' comparison with Sony's (above), and the rest when a game's log shows one.

- **Done (D-254):** H.264 decodes on VideoToolbox through FFmpeg's hwaccel: the same pictures as the software decoder,
  the decoding thread 77% less CPU (0.44 instead of 1.95 ms a 720p picture on the M4). **Try:** a game that plays
  PAMF movies (neither local game does: both play Bink on their SPUs) for numbers in play, and the phone.

## The gate (D-149)

- **Seen (2026-10-07 14:14):** test_lv2 failed once in a `ctest -j8` run (LastTestsFailed.log; its output was
  overwritten), then passed 3 suite runs, 30 runs alone and 25 runs under a full-core load. Some test there is timing
  sensitive about 1 in 60 times. **Try:** run it in a loop under load with `--reporter compact` saved per run until
  it fails, then fix the test's timing assumption.
  **Tried (2026-10-08):** 500 runs of test_lv2 under a full-core load and 40 runs of the whole suite with `ctest -j8`:
  no failure. Not reproducible now; the gate keeps `--output-on-failure` logs, so the next one shows its test.
- **Done (D-253):** spu-check over the game's SPU programs is part of `gate.sh all` (about 90 s): a build of translated
  SPU code that the host compiler got wrong showed in no test and no walk, only there.

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
- **Seen (2026-10-07, Assassin's Creed prepared again on the Mac):** "time left" said 3:18 at the start of a
  preparation that took 0:16: 404 of 410 files were kept from the last one, and the estimate counts them as compiled
  again.
- **State:** the build reports the time left of the files it will compile before the first one (D-209).
- **Done (D-227):** objects of earlier preparations nothing links any more are deleted after the link (Assassin's
  Creed: 566 MB freed).
- **Tests:** the timing-sensitive tests are robust now (thread report: CPU time; usleep(0): the backoff state, not
  wall time). Open: the SPU analyzer's other copies (call targets inside another function's code), PGO/LTO clang,
  LLVM IR from the lifters, a scheduler that keeps the biggest files on performance cores.
- **Done (D-251):** per-unit prototypes: GTA V's PPU units compile 6-7% faster (165 to 151-155 s on the M4).
- **Seen (2026-10-08):** GTA V's SPU programs lift 813,469 instructions for 515,520 distinct ones (1.58 times; up to
  4.5 in one program, 2.9 in the busiest module): a function's body takes in the code of others it runs into. Not
  touched: cutting the copies makes each shared path a call between functions, the costliest part of translated SPU
  code at run time, for a one-time saving when preparing.

## Stability: the EDGE/GFX job chain's descriptors written over

- **Seen:** CELL_SPURS_JOB_ERROR_NUM_CACHE or INVALID_BINARY on the EDGE/GFX chain now and then, the chain halts
  (D-130, D-131 on the iPhone, D-138 and run 095 on the Mac): a descriptor slot holds vertex-like data. Run 095's log
  names the writer: a job (binary 0x18fcc80) PUT 0x2000 bytes at 0x10394000, over the descriptor at 0x10395a00, 26 ms
  before the chain read it. The walk passes when run again.
- **Try:** which SPU program binary 0x18fcc80 is and where its output address comes from (its job descriptor's
  output list, the interpreter against the lifted program for that job: spu-check with that image); whether lv2 or
  the game's heap handed out the ring and the job's buffer overlapping (log both allocations).
- **Seen (2026-10-06, inventory/repro-edge run2-3):** caught on the Mac, about one walk in ten under load: the
  descriptor at 0x103b3c00 held the game's own RSX state commands (stencil, polygon mode, cull face, transform
  constants), and no SPU wrote over it among the last 2,048 writes of each thread.
- **Ruled out (D-222, runs 150-152):** the game rewriting a descriptor between our grab and the job's start (we read
  descriptors later than libsre does): 0 changes to what a job uses in 300 s of GTA V. A stop's report now includes the
  descriptor as it was at the grab.
- **Found (D-224, D-225, repro-edge2 run 26):** with the chain's last visits in the report: a job's 8-byte PUT
  turning an END into a jump-to-self was written word by word (D-124's first word last), and the chain read the low
  word alone, 0x12, a light sync, then the previous lap's commands. A PUT now writes its first eight bytes in one store.
- **State:** fixed (D-225): 0 stops in 11 loaded walks where 3 in 10 stopped before. Confirm on the iPhone (run 100's
  stop came after 110 s there).

## Stability: data races (ThreadSanitizer, D-585)

- **Done:** `scripts/tsan.sh` builds build/tsan and runs every threaded test under ThreadSanitizer, a line per group.
  The first sweep found an SPU call's answer overtaken by the PPU's reply (D-584), a thread's stack still committed
  when its join returned (D-586), words pspocket's own threads shared without order (D-587) and tests that read before
  what they waited for had landed. What is left is guest-level and suppressed with its reason (scripts/tsan.supp).
- **Blind spots:** ThreadSanitizer does not model fences, and pspocket orders DMA, lwsync and the reservation system's
  stores with them: a report whose two sides only a fence orders is not a bug (read it through the reservation
  system's version, or a release store, to make sure). The tests interpret: translated code is not covered.
- **Next:** pspocket-boot under ThreadSanitizer for a few minutes of a game, the translated functions' guest accesses
  suppressed as the interpreter's are; the reports left in pspocket's own code are what an hour without a crash needs.
  Run the sweep after any change to threaded code.

## System libraries (D-500)

- **The count.** The chart counts every function RPCS3 registers (6,625). 2,752 of them are sys_libc, sys_libm and
  sys_libstdcxx, which no firmware module has (games link them into themselves); the firmware-backed libraries are
  2,305 functions. Next for the count: the firmware-backed libraries in order of use, then the C and C++ libraries.
- **liblv2's printf (D-501).** Sony's formatter has no floating point; one that games call with %f prints "f". Faithful,
  not an improvement to make.
- **liblv2's heap (D-502).** One host mutex for every heap (liblv2 locks each heap's own); if heap calls ever show in a
  profile, a mutex per heap.
- **sys_fs's kernel calls (D-518).** sys_fs_link, access, ACLs, LSN reads, symbolic links, chown and the mapped
  buffers are pspocket's readings (nothing documents them; libfs only shows the arguments). The LSN calls read a file's
  own sectors and report no CD-DA areas; a game that reads a disc by absolute sector would need the image's extents.
  Names are never converted (fcntl 0xC0000006 answers CELL_ENOTSUP for every mount), as RPCS3: right for host file
  systems, which keep long names.
- **cellGcmSys's flip path (D-519).** pspocket's cellGcmSetFlip writes RPCS3's GCM_FLIP_COMMAND (queue and flip at
  once, buffer id returned); libgcm_sys queues with GCM_DRIVER_QUEUE, flips from its interrupt handler through lv2's
  0x102 and returns the ring slot. Moving to libgcm_sys's path would let the comparison cover cellGcmSetFlip too; it
  needs lv2's queue and flip events (RPCS3 send_event) and a vblank-timed flip.
- **USB (D-522).** No device ever attaches, so cellUsbd's attach, probe and transfer paths and cellUsbPspcm's
  binding and PSP protocol are not written (a dev build stops if one is reached). A device model (an emulated
  Skylanders/Infinity portal, a guitar, a PSP over the network) is where they would come from, each compared with
  libusbd/libusbpspcm running on it. lv2's unnamed USB calls (563 composite drivers, 565 memory from a container,
  567..569 shared memory) answer as RPCS3 (CELL_OK, nothing written): binding them needs names in the syscall table.
- **The network (D-523).** lv2 has no interface but loopback and lists none to libnet (sysctl's routing tables are
  empty); a real console without a cable would still list lo0. libnet's per-thread contexts take 3192 bytes of the
  game's pool each, as on a PS3. gethostbyname gives libnet's answers without its resolver's work yet (part 2).
- **cellHttp past a connection (D-524).** Connecting, sending, receiving and parsing, chunks, gzip, redirects,
  authentication and cookies from responses, pipelining: written once libnet's resolver is pspocket's (D-523 part 2),
  compared against a loopback server. The disk cache's 12 unnamed exports are not registered. HTTPS waits on cellSsl.
- **cellSync2 (D-531).** A PPU thread's receivers are 128 bits of two event flags, as libsync2's: a game with more
  than 128 threads blocked on cellSync2 objects at once gets CELL_SYNC2_ERROR_AGAIN, as on a PS3. The queue's pop
  spins (yielding after 64 looks) while a counted element's pusher has not linked it yet, as libsync2 spins.
- **Kernel calls (D-527, D-528).** Every named slot is bound: 560 own, 74 answered as RPCS3's null or stub slots (the
  debugger's sys_dbg family, the crypto engine, colored memory, whose arguments nothing documents; studying
  liblv2coredump and libatxenc would give their signatures). sys_rsxaudio plays nothing: pspocket's console has no AV
  manager enabling an output (D-527 part 4); a game that feeds audio through it would need the samples routed to the
  host audio unit. Process spawning (21) stays CELL_ENOSYS for a game (RPCS3 too).
- **Raw SPU registers through faults (D-528).** Each register access costs a signal (a few microseconds); a game
  polling SPU_Status in a tight loop would spend its time in the handler. If one shows in a profile: let lifted PPU
  code call the device for the 0xE0040000 window instead of faulting (the lifter knows constant addresses). Exclusive
  and atomic accesses to the window stay faults (fatal), and only Apple AArch64 decodes faults: an x86 or Linux host
  answers sys_raw_spu_create with CELL_EAGAIN. SPU DMA to another raw SPU's local store raises a DMA exception.
