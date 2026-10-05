# Run 089 · iPhone: no flicker, no archive memory; a feedback-copy memory spike stops the app after ten minutes

2026-10-05 17:05-17:24 · builds de54399 (D-144), then ed5005f (D-145) · iPhone 17 Pro Max by cable, log streamed
with idevicesyslog (inventory/phone/live/syslog-12.log) and the game's own log pulled with devicectl.

**Boots 1 and 2 (17:05, 17:09; D-144 with nine GPU binary archives):** iOS stopped the app at the loading screens,
17:06:02 and 17:10:18, 0.3 s and 30 s after the shader cache started. No crash report, no fatal line: "Received memory
warning" 1 and 5 times before. Archives off and deleted: D-145.

**Boot 3 (17:13, D-145):** no memory warnings at boot; the cache made all 13,067 pipelines in 30.1 s (41 to 43 s with
the archives: iOS keeps compiled pipelines itself). The user: the flicker is gone (D-144).

| Time | fps | Sound pace (min) | Memory in use (left) | Heat | Notes |
|---|---|---|---|---|---|
| 17:14-17:16 | 29 to 30 | 1.03 | 0.86 to 1.62 GB (5.58 to 4.82) | nominal | loading, then the first scene; 371 to 948 draws |
| 17:16-17:17 | 23 to 30 | 0.65 to 0.68 | 1.58 to 1.63 GB | nominal | 11 to 12 new pipelines at a time (0.9 to 1.2 s) |
| 17:17-17:19 | 14 to 22 | 0.52 to 0.79 | 1.65 to 1.74 GB | nominal, then fair | SPURS 5-SPU instance 70 to 85% busy (EDGE/GFX 7.6 to 9.4 s per 5 s); the 1-SPU mixer 85 to 90% |
| 17:19-17:22 | 10 to 19 | 0.54 to 0.87 | 1.74 to 1.81 GB | fair | 800 to 1,900 draws, 55 to 96 passes; GPU 20 to 27 ms of a 50 to 100 ms frame |
| 17:23:02-12 | 5 to 7 | 0.54 | 1.86 GB, then **4.33 GB**, then 1.78 | fair | 61 of 96 passes a frame ended by target copies; GPU 33.6 ms |
| 17:23:45-53 | 18 | 0.93 | 1.83 GB, then **4.74 GB (1.70 left)** | fair | memory warnings 9 times in 2.4 s; iOS stops the app |

**The spike, from the code (no run yet):** a draw that samples a render target it also renders to gets a new
full-size copy of the target, every time (texture_cache.mm:490, copy_texture at surface_cache.mm:339): 3.7 to 14.7 MB
each, alive until the frame's command buffer completes, and frames in flight are not capped. The traced draws of this
kind (flicker-trace.log) write mask 0 to the sampled target: the copies are not needed at all. About 200 copies a frame
in the dip makes 0.7 to 1.5 GB per frame in flight. Plan: one copy per surface and generation, frames in flight capped,
a memory trim on iOS warnings (plan "only forward", Phase 1).

**Also seen:** "blend equation 0xf006 (signed)" (drawn as ADD, as RPCS3), multisampled targets drawn with one sample;
the 1-SPU "Secondary" SPURS instance is GTA V's audio mixer (runs 022/024), the sound's bottleneck when the cores are
full.
