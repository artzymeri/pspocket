# Run 327 · GTA V on the iPhone 18 Pro Max, watched live

2026-10-14 · c344176 (libraries 6,625 of 6,625) · iPhone 18 Pro Max, iOS 27.0.1, on USB to the Mac (charging) ·
installed with ideviceinstaller (Xcode 26.0.1 has no disk image for iOS 27) · the game's log pulled every 4 s over AFC
and the device log streamed (idevicesyslog), the user playing.

## Before the game ran

- 14:24 the first boot ended at once: SIGBUS, an SPU DMA from guest address 0 (SPURS 0x10016000 "Secondary", SPU pc
  0x4c30, `transfer` in mfc_command). The log said "no /dev_flash": the game's MP3 decoder (flashMP3.pic) and the rest
  of the system files were missing. The firmware's dev_flash (4.92, 1,075 files) went to
  Library/Application Support/pspocket-firmware over AFC, with the signing identity.
- 14:51 the app prepared the system library: 106 files in 35 s. Then GTA V again (the new runtime's ABI): 527 files in
  333 s (compile 326 s), the phone at 93% of a core for minutes (a non-fatal cpu_resource report).
- 14:56:54 the game booted: 362 imports, all pspocket's own, 0 firmware modules run as they are.

## The session (14:56 to 15:20, 22,825 frames)

- The first minutes at 29 to 30 fps, the sound whole. The heat was "serious" from the first report to the last (193
  windows): the preparation had heated the phone, and it was charging.
- From 15:03, in busy scenes 19 to 27 fps with 4.5 to 4.9 cores busy (EDGE/GFX alone 2 SPU threads' worth), and from
  15:06 to 15:20, for minutes at a time, 4 to 12 fps. GPU time a frame against feedback copies a frame (D-148's copies
  of a target a draw samples while drawing to it): 7 to 11 copies at 8 to 16 ms elsewhere; 77 at 45 ms, 124 at 61, 191
  at 81, 231 at 113, 304 at 122, 334 at 155 ms. Up to 411 render passes and 3.3 GB of attachments loaded and stored a
  frame. The copies took MTLDevice.currentAllocatedSize to 8.9 GB and the process to 4.17 GB; iOS warned at 15:07:33 and
  15:13:36 (trim_memory freed up to 6,847 textures). This is D-947.
- Sound: 12.5% of 146,299 periods late (the game's mixing not done in time), up to 52% in the slow stretches, none at
  30 fps. iOS's "quiet time" (audio ducked to -40 dB for 3 s) came twice, after the user's screenshots: not the game's.
- SPU code: programs met during play ran in the interpreter, 844 million SPU instructions in 67 of the 5-s windows (up
  to 20 million in one). In-session translation (D-236) never ran: it waits while the heat is serious and stops after
  a memory warning. At the relaunch (15:20:43) the app updated the library in 13.7 s (12 files): 13 SPU programs where
  there were 8.
- The game saved (15:19:55, BLES01807SGTA50015, 281 KB, read back at once), unlocked "Welcome to Los Santos", and
  found its save at the relaunch.
- Logged once each: blend equation 0xf006/0xf006 (add signed: drawn as add, as RPCS3 does), cellGcmSetTileInfo
  compression modes 7 and 10 (not emulated, as RPCS3), rumble not implemented.

## The second session (15:37, D-947 and D-948 in)

- Installed at 15:36 (6d8f509): no preparation (the runtime's ABI unchanged); the shader cache's recorded programs were
  of another translation (D-948's parameters changed its fingerprint): 6 made ahead, the rest compiled in play, the
  first minute at 2 to 17 fps with up to 160 draws a frame waiting and 60 drawn by the shader interpreter.
- D-947 at work in the scenes that ran at 4 to 12 fps before: 7 to 9 feedback copies a frame, and 30 to 460 draws a
  frame reading a kept copy (the draws behind them: dozens of GTA V's fragment programs reading unit 12, the 1280x720
  F_W16Z16Y16X16 light buffer at 0xcf1c0000, while drawing into it). GPU time 8 to 11 ms a frame, 60 to 89 passes,
  GPU memory 300 to 460 MB, no memory warnings; 15 to 28 fps, now held by the CPU (5 cores busy, EDGE/GFX about two
  SPUs' worth). GTA V invalidates the texture cache 160 to 1,500 times a frame, none of them between two such draws to
  the same targets (no copies "after a write and an invalidation").
- The shadows still came out lit (the user's screenshot: Franklin's and a mailbox post's shadows white on a sunny
  pavement). A frame trace over USB (Documents/pspocket-trace; frame 2816: 3,682 draws, 47 colour targets, 255 PNG
  files) showed no signed blending in that frame, and the sun's shadow resolve (#2769) reading the cascade atlas with a
  depth comparison: the comparison was backwards (D-949).

## The third session and the Mac (16:00 to 18:00)

- The user: "shadow bug is fixed, the main issue is the fps, it should go 30 fps static always". Busy scenes 15 to 28
  fps, 5 of 6 cores busy, GPU 8 to 11 ms a frame, the heat "serious" throughout (charging).
- QoS on the phone (D-950): every SPU worker at interactive, 18.1 fps in the busy windows against 18.2 at
  user-initiated; the sound mixer faster at interactive alone, the sound worse with all of them. The two performance
  cores are full either way.
- The user's screenshot of the fight tutorial: black but a lit top-left rectangle, the characters shiny black. The Mac's
  walk (the disc image back in inventory/gta5) showed it for whole camera shots; off switches and two frame traces
  found it in D-945: a depth buffer cleared alone inside an open pass over a 320x128 target, its corner only (D-951).
  The gate's frame jumps had passed it: it now counts dark frames too.
- Four research tracks on a steady 30 fps (the CPU budget per frame and what could run natively; Apple's platform
  levers; the SPU translation, SPU runtime and RSX thread; the SPU runtime and SPURS overhead): the busy scenes need
  about 277 ms of CPU a frame against 165 to 185 that six cores hold at 30 fps; EDGE/GFX alone is 85 ms (six functions
  84% of it), the sound mixer a whole core at full pace, the RSX thread 33 ms (12 us a draw). The plan from it is in
  docs/improve.md ("A steady 30 fps on the phone").

## What came of it

- D-947: the copy kept as the RSX's texture cache keeps what it read, until the game invalidates the cache or the
  draws go to other targets; a report line names the draws behind the copies.
- D-948: the signed blend equations in the shader (programmable blending).
- D-949: shadow-map lookups compare the texel against the reference, as RPCS3's reversed sampler functions do.
- D-950: the SPU workers' QoS class switchable (and A/B in one session), their time by host CPU; no gain from
  interactive.
- D-951: a clear done in the open pass only when that pass covers it (the black fight scene); the gate counts dark
  frames. D-948 part 2: B8 and G8B8 targets blend the signed equations in the shader too.
- To look at next (docs/improve.md): in-session SPU translation under heat (it never ran in this session); the sound
  when the game is late; the heat at the start, after a long preparation.
