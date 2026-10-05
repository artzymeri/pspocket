# Device and host runs

One file per device session (or host bring-up run): what was run, on what build, the numbers, and what they mean for the plan. Reports are
the raw text the test host writes (`scripts/ios.sh report`). No game data appears here.

| Run | Date | Build | Device | Result |
|---|---|---|---|---|
| [001](001-first-device-selftest.md) | 2026-10-03 | e557a8a | iPhone 17 Pro Max, iOS 26.6.1 | Self-test 6/6 pass; MetalFX 1152×640 at 30 Hz, 0 dropped |
| [002](002-thermal-10min.md) | 2026-10-03 | e557a8a | iPhone 17 Pro Max, iOS 26.6.1 | 10 min max GPU load: nominal thermals, GPU time flat, 0/18,241 dropped |
| [003](003-ppu-lifted-on-device.md) | 2026-10-03 | 2bef847 | iPhone 17 Pro Max, iOS 26.6.1 | 8/8; PPU interpreter 317 MIPS, lifted 12.0× faster (≈3,690 MIPS) |
| [004](004-gta5-first-boot-mac.md) | 2026-10-03 | a92b4c2 | M4 Mac, pspocket-boot | GTA V EBOOT boots under the interpreter; first gap `sys_lwmutex_create` |
| [005](005-gta5-boot-2-mac.md) | 2026-10-03 | 442ac11 | M4 Mac, pspocket-boot | Past the CRT; next gaps `sys_time_get_system_time`, `cellSysmodule` |
| [006](006-gta5-boot-4-mac.md) | 2026-10-03 | 8cf53e3 | M4 Mac, pspocket-boot | Two SPURS instances up natively; next gap `sys_vm_memory_map` |
| [007](007-gta5-boot-5-mac.md) | 2026-10-03 | db14915 | M4 Mac, pspocket-boot | 60 s of game code without a crash; tasksets, thread creation and PSN status requested |
| [008](008-gta5-boot-6-mac.md) | 2026-10-03 | 246075a | M4 Mac, pspocket-boot | Threads run; Bink tasksets; sheap, network and SPU setup requested |
| [009](009-gta5-boot-7-mac.md) | 2026-10-03 | d749522 | M4 Mac, pspocket-boot | SPU and network setup done; next gap `cellNetCtlInit`; Bink waits on native SPU tasks |
| [010](010-gta5-boot-8-mac.md) | 2026-10-03 | 0d93ebb | M4 Mac, pspocket-boot | Network up; first job chain attribute (14 arguments) |
| [011](011-gta5-boot-9-mac.md) | 2026-10-03 | 4f47fde | M4 Mac, pspocket-boot | Seven job chains and the `dependency_spu` taskset created; next gap `cellSpursEventFlagAttachLv2EventQueue` |
| [012](012-gta5-boot-10-mac.md) | 2026-10-03 | 1c8ec8f | M4 Mac, pspocket-boot | Event flag attached to its lv2 queue; next gap `cellSpursTaskGetLoadableSegmentPattern` (task creation) |
| [013](013-gta5-boot-11-mac.md) | 2026-10-04 | f8c82a4 | M4 Mac, pspocket-boot | Crash in `_cellSpursTaskAttributeInitialize` (a register holding 0x40 read as a pointer); crash reports now show the call site |
| [014](014-gta5-boot-12-mac.md) | 2026-10-04 | a45e338 | M4 Mac, pspocket-boot | Bink fixed; 4 dependency tasks created; first file access needs the disc |
| [015](015-gta5-boot-13-mac.md) | 2026-10-04 | ad558c6 | M4 Mac, pspocket-boot | Disc image copied from the iPhone and read in place; archives open; next gap `_cellGcmInitBody` |
| [016](016-gta5-boot-14-mac.md) | 2026-10-04 | ad558c6+ | M4 Mac, pspocket-boot | The RSX comes up (lv2 sys_rsx, cellGcmSys); next gap `cellVideoOutGetState` |
| [017](017-gta5-boot-15-mac.md) | 2026-10-04 | ad558c6+ | M4 Mac, pspocket-boot | 720p video output configured, Bink tasks created (libsre signature); first job chain "EDGE/GFX"; next gap `cellSpursRunJobChain` |
| [018](018-gta5-boot-16-mac.md) | 2026-10-04 | ad558c6+ | M4 Mac, pspocket-boot | Vblank handler called at 59.94 Hz; next gap `cellKbInit` |
| [019](019-gta5-boot-17-mac.md) | 2026-10-04 | ad558c6+ | M4 Mac, pspocket-boot | 60 s without a crash: input, tiles, first frame's job chain kicks; the game waits on SPU work |
| [020](020-gta5-boot-18-mac.md) | 2026-10-04 | 5cabf48+ | M4 Mac, pspocket-boot | SPU tasks run natively on SPURS workers; job chains and the custom workload don't yet |
| [021](021-gta5-boot-19-mac.md) | 2026-10-04 | 5cabf48+ | M4 Mac, pspocket-boot | GTA V's own SPU module runs, audio starts, no unimplemented calls; the game waits on its job chains |
| [022](022-gta5-boot-20-mac.md) | 2026-10-04 | 437b72e+ | M4 Mac, pspocket-boot | The job chains run; a job sets an lv2 event flag bit from SPU code (unhandled) |
| [023](023-gta5-boot-21-mac.md) | 2026-10-04 | 437b72e+ | M4 Mac, pspocket-boot | The Secondary chain (GTA V's audio mixer) runs 55 commands, then MEMORY_SIZE |
| [024](024-gta5-boot-22-mac.md) | 2026-10-04 | 437b72e+ | M4 Mac, pspocket-boot | MEMORY_SIZE again; first cellAudioAddData |
| [025](025-gta5-boot-23-mac.md) | 2026-10-04 | 437b72e+ | M4 Mac, pspocket-boot | 395 commands; a job fits only when its pages are packed |
| [026](026-gta5-boot-24-mac.md) | 2026-10-04 | 437b72e+ | M4 Mac, pspocket-boot | 60 s, no crash: 2 M RSX words and 1,721 flips (a loading screen, nothing shown yet) |
| [027](027-gta5-boot-25-mac.md) | 2026-10-04 | 437b72e+ | M4 Mac, pspocket-boot | PSN manager callback kept; basic handler next |
| [028](028-gta5-boot-26-mac.md) | 2026-10-04 | 437b72e+ | M4 Mac, pspocket-boot | sceNpLookupInit; the main thread waits on a semaphore from second 4 |
| [029](029-gta5-boot-27-mac.md) | 2026-10-04 | c68e12c+ | M4 Mac, pspocket-boot --frames | First picture: the Rockstar logo screen through the Metal backend, after the event flag direction fix |
| [030](030-gta5-boot-28-mac.md) | 2026-10-04 | c68e12c+ | M4 Mac, pspocket-boot --frames | A CellSpursTaskset2; then `_cellSpursQueueInitialize` (SPURS queues) |
| [031](031-gta5-boot-29-mac.md) | 2026-10-04 | 68706ad | M4 Mac, pspocket-boot --frames | SPURS queues work; the opening animation; the audio engine starves (priorities upside down) |
| [032](032-gta5-boot-30-mac.md) | 2026-10-04 | 68706ad+ | M4 Mac, pspocket-boot --frames | The audio engine runs (D-064); the mixer faults on a job descriptor |
| [033](033-gta5-boot-31-mac.md) | 2026-10-04 | 68706ad+ | M4 Mac, pspocket-boot --frames | Two PPU threads took one job slot: `stwcx.` was not atomic |
| [034](034-gta5-boot-32-mac.md) | 2026-10-04 | 68706ad+ | M4 Mac, pspocket-boot --frames | Atomic reservations (D-066); a fault outside the PPU threads |
| [035](035-gta5-boot-33-mac.md) | 2026-10-04 | 68706ad+ | M4 Mac, pspocket-boot --frames | The fault is the audio mixer's SPU job (a DMA from address 0) |
| [036](036-gta5-boot-34-mac.md) | 2026-10-04 | 68706ad+ | M4 Mac, pspocket-boot --frames | The job loads a code module from a header of zeros |
| [037](037-gta5-boot-35-mac.md) | 2026-10-04 | 68706ad+ | M4 Mac, pspocket-boot --frames | The module's 96 KB buffer was never filled |
| [038](038-gta5-boot-36-mac.md) | 2026-10-04 | 68706ad+ | M4 Mac, pspocket-boot --frames | The MP3 codec comes from /dev_flash, which was missing |
| [039](039-gta5-boot-37-mac.md) | 2026-10-04 | 68706ad+ | M4 Mac, pspocket-boot --frames | No crash for 120 s with /dev_flash (D-067); a grey screen, 110,146 draws skipped |
| [040](040-gta5-boot-38-mac.md) | 2026-10-04 | c6b357d+ | M4 Mac, pspocket-boot --frames | Condition codes (D-068): no failed draws; the legal and Display Calibration screens |
| [041](041-gta5-boot-39-mac.md) | 2026-10-04 | e935dfa | M4 Mac, pspocket-boot --press | Past calibration with a scripted Cross: "Loading Story Mode"; waits on SPURS semaphores |
| [042](042-gta5-first-iphone-run.md) | 2026-10-04 | 0770578+ | iPhone 17 Pro Max, iOS 26.6.1 | GTA V on the phone: ISO picked in Files, calibration with an on-screen Cross, profile saved, "Loading Story Mode" to the semaphore gap |
| [043](043-gta5-sound-mac.md) | 2026-10-04 | 7ea8393+ | M4 Mac, pspocket-boot --audio | Sound blocks played whole (D-074); GTA V's audio runs at 55 to 65% of real time on the loading screens, 20% at its heaviest |
| [044](044-gta5-first-mission-mac.md) | 2026-10-04 | c503afa | M4 Mac, pspocket-boot --frames --press | **The first mission starts** (the prologue's objective and radar); the 3D world is black: vertex condition codes, DIVSQ, vertex TXL, X16 textures |
| [045](045-gta5-time-stretch-mac.md) | 2026-10-04 | c503afa+ | M4 Mac, pspocket-boot --audio | Time stretching follows the game's pace (0.2 to 1.07); 69 speaker underruns in 120 s instead of about 500 |
| [048](048-gta5-fifo-desync-mac.md) | 2026-10-04 | 574baa0+ | M4 Mac, pspocket-boot --spu-dump | The first mission ends on "malformed FIFO command": ARM memory ordering (D-077). From here on a note carries its log's number |
| [049](049-gta5-lifted-spu-mac.md) | 2026-10-04 | 574baa0+ | M4 Mac, pspocket-boot --audio | 102 lifted SPU programs run (D-076); a task that loads modules itself; a second desync |
| [050](050-gta5-profile-mac.md) | 2026-10-04 | a532a5f | M4 Mac, pspocket-boot --frames, Time Profiler | 15 minutes without a desync; the world draws in pink; hashing takes a fifth of the SPU workers (D-079) |
| [051](051-gta5-fps-mac.md) | 2026-10-04 | a532a5f+ | M4 Mac, pspocket-boot --gpu metal | Frame rates: intro 11 to 13, loading 30, "Loading Story Mode" 8 to 10, the mission 4; the SPU interpreter 38% |
| [052](052-gta5-module-dump-mac.md) | 2026-10-04 | 02e8498 | M4 Mac, pspocket-boot --spu-dump | 82 modules at thousands of addresses: 32,570 dumps; relocatable lifting (D-081) |
| [053](053-gta5-relocatable-jobs-mac.md) | 2026-10-04 | 750963c | M4 Mac, pspocket-boot --gpu metal --spu-dump | Relocatable jobs (D-081): the intro at 20 to 28 fps (was 11 to 13); 44 modules saved once each |
| [054](054-gta5-lifted-modules-mac.md) | 2026-10-04 | 750963c+ | M4 Mac, pspocket-boot --frames --spu-dump | Every SPU program lifted: the intro at 30 fps, **the first mission at 9 to 10 fps** (was 4); the world still pink |
| [055](055-gta5-module-count-mac.md) | 2026-10-04 | 750963c+ | M4 Mac, pspocket-boot --spu-dump | No module changed in local store (D-082's cause is the tracker); a FIFO desync after 195 s of the mission |
| [056](056-gta5-first-frame-trace-mac.md) | 2026-10-04 | 750963c+ | M4 Mac, pspocket-boot --trace-frame, Time Profiler | The first frame trace: the lighting pass reads three inputs wrongly (D-083); the commit check's mutex at 14% |
| [057](057-gta5-spu-interp-sites-mac.md) | 2026-10-04 | 37f47f9+ | M4 Mac, pspocket-boot --trace-frame | 16.6 billion SPU instructions interpreted at job entries: jobs lost their lifted code to data GETs (D-085) |
| [058](058-gta5-depth-readbacks-mac.md) | 2026-10-04 | 37f47f9+ | M4 Mac, pspocket-boot --frames | Jobs lifted again; the policy module's stops (D-087); 65,433 depth readbacks, the mission at 6.6 fps |
| [059](059-gta5-stops-lifted-mac.md) | 2026-10-04 | db6b7ad+ | M4 Mac, pspocket-boot --frames | The most interpreted SPU address down to 70 million instructions; the mission at 6 to 7 fps |
| [060](060-gta5-gpu-depth-mac.md) | 2026-10-04 | 66b8b7a | M4 Mac, pspocket-boot --trace-frame | Depth converted on the GPU (D-088): **the first mission at 22 to 25 fps**; IFE and UPB draws skipped |
| [061](061-gta5-fragment-flow-mac.md) | 2026-10-04 | 61be402 | M4 Mac, pspocket-boot --trace-frame | Fragment program flow control (D-089): the mission at 22 to 24 fps |
| [062](062-gta5-decode-cache-mac.md) | 2026-10-04 | 61be402+ | M4 Mac, pspocket-boot --gpu metal | The PPU decode cache (D-091): the mission at 26 to 28 fps |
| [063](063-gta5-skipped-post-mac.md) | 2026-10-04 | 93dd06e+ | M4 Mac, pspocket-boot --trace-frame | Five post-processing draws skipped for vertex texture fetch (D-093) |
| [064](064-gta5-lit-mission-mac.md) | 2026-10-04 | 9cd1f31 | M4 Mac, pspocket-boot --trace-frame | **The first mission lit correctly**, no skipped draws, 26 to 28 fps |
| [065](065-gta5-ppu-aot-fault-mac.md) | 2026-10-04 | 9cd1f31+ | M4 Mac, pspocket-boot, the PPU code translated | The game's PPU code translated ahead of time (D-092) faults within seconds: wrappers entered above their body (D-094) |
| [066](066-gta5-ppu-translated-mac.md) | 2026-10-05 | 9cd1f31+ | M4 Mac, pspocket-boot, the PPU code translated | **The game's PPU code translated: the first mission at 28 to 30 fps** (the cap); an SPU task's modules and the script VM interpreted |
| [067](067-gta5-vm-tables-mac.md) | 2026-10-05 | 9cd1f31+ | M4 Mac, pspocket-boot, the PPU code translated | Modules in shared blocks found, the script VM's tables, DEPTH16 from memory (D-095 to D-097) |
| [068](068-gta5-color-conversion-mac.md) | 2026-10-05 | 9cd1f31+ | M4 Mac, pspocket-boot --spu-dump | Y16_X16 over colour targets converted on the GPU: one readback a frame left (D-098) |
| [069](069-gta5-module-tracker-mac.md) | 2026-10-05 | 9cd1f31+ | M4 Mac, pspocket-boot --spu-dump | The dependency tasks' modules all run lifted (D-099) |
| [070](070-gta5-shadow-lookups-mac.md) | 2026-10-05 | ed07f03- | M4 Mac, pspocket-boot --trace-frame | Shadow lookups compare depth; the cascades are atlas tiles read from stale memory (D-100) |
| [071](071-gta5-atlas-tiles-mac.md) | 2026-10-05 | ed07f03- | M4 Mac, pspocket-boot --trace-frame | Every shadow cascade from its depth buffer (D-100) |
| [072](072-gta5-occlusion-mac.md) | 2026-10-05 | fd3a099+ | M4 Mac, pspocket-boot --frames | Occlusion queries counted on the GPU; 361,371 RSX failures left |
| [073](073-gta5-rsx-complete-mac.md) | 2026-10-05 | fd3a099+ | M4 Mac, pspocket-boot --frames | **One RSX failure in the run**: 3D textures, conditional rendering (D-102) |
| [074](074-gta5-spu-backoff-mac.md) | 2026-10-05 | beddc3e+ | M4 Mac, pspocket-boot, top | SPU workers back off empty policy-module polls: **2.6 cores instead of 4.8** at the same 30 fps (D-103) |
| [075](075-gta5-rsx-thread-mac.md) | 2026-10-05 | 8666ca9+ | M4 Mac, pspocket-boot, sample | The RSX thread's texture hashing and the render thread's yields (D-104): 2.2 cores at 30 fps |
| [077](077-gta5-gpu-passes-mac.md) | 2026-10-05 | 4f08184+ | M4 Mac, pspocket-boot | GPU time per frame and pass endings: **17.8 ms instead of 23.5** (D-105) |
| [079](079-gta5-readbacks-mac.md) | 2026-10-05 | 3b537de+ | M4 Mac, pspocket-boot | The per-frame NV0039 copy on the GPU: **the RSX thread waits 0 ms instead of 9.3** (D-107) |
| [080](080-gta5-mission-mac.md) | 2026-10-05 | 3a3e3fb+ | M4 Mac, pspocket-boot, sticks | Walking, the phone and the charge in the first mission; **no SPU code interpreted** after five lift rounds (D-108) |
| [081](081-gta5-profile-mac.md) | 2026-10-05 | a1f7223+ | M4 Mac, pspocket-boot, sample | CPU per thread in the mission (1.3 cores busy); SPU image identification **363 samples to 30** (D-109) |
| [082](082-gta5-shader-cache-mac.md) | 2026-10-05 | 7ac7e8c+ | M4 Mac, pspocket-boot --shader-cache | Shaders made ahead while the game loads: **no compile stalls in play** instead of 13.6 s of them (D-114) |
