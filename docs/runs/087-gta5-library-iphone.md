# Run 087 · The library on the iPhone: the start-of-play shader stalls found and cut; colour flicker reported

2026-10-05 15:47-16:15 · library build (D-142) and D-143's first version · iPhone 17 Pro Max, by cable
(scripts/ios.sh build + install), log streamed with idevicesyslog.

The library booted every game picked from it (GTA V twice, L.A. Noire, Assassin's Creed).

**GTA V, the first two boots (shader cache as D-114):** 30 fps on the loading screens, then 4 to 10 fps for the first
minutes of play, with the sound skipping. The SPUs were mostly idle; the RSX thread compiled 821 pipelines and 161
programs in the first 300 frames of play (6.5 s of compiling). The cache was making 7,526 programs and 11,572
pipelines on 2 threads at utility QoS, all the programs before the first pipeline: after more than 80 s it had not
logged done, and no pipeline the game needed was ready.

**GTA V, third boot (D-143: pipelines first in first-use order, 3 threads at user-initiated QoS, GPU binary
archives):**

| Time after boot | Pipelines made ahead | Programs |
|---|---|---|
| 23 s | 5,831 of 11,634 | 4,205 of 7,560 |
| 33 s | 6,983 | 4,876 |
| 63 s (play has begun) | 9,010 | 5,981 |
| 84 s | 10,247 | 6,612 |

The RSX thread compiled 45 programs and pipelines in the first 300 frames of play (2.3 s), then 154 (4.5 s): 17 to
23 fps. While the cache's threads competed with the game the sound ran at 0.54 to 0.81 of its pace (466 blocks
skipped in 30 s). Saving the archives failed: "expecting 'fragment' stage in pipeline": a pipeline that writes no
colour loses its fragment stage, and an archive holding one cannot be written. After this run: no colourless
pipelines in the archives; the cache's threads drop to utility QoS after 45 s; the next boot should find the
pipelines in the archives.

**Reported by the user:** "the graphics flicker and change colours" in the third boot. Suspects: D-143 had let a
pipeline the cache made join under the backend's own programs for the same key (reverted after this run: a pipeline
joins only with the program objects it was made with, as D-114), D-137's overlay (active: the 640x360
F_W16Z16Y16X16 target read as 1280x720 A8R8G8B8) and D-137's G8B8 (G8B8 over a G8B8 target now draws; those draws
were skipped before). PSPOCKET_RSX_WRITE_BACK_TARGETS=1 turns the overlay off for an A/B.

**Also seen:** the library's layout "in its first second" with the covers too high and the scrims about 60 pt short
(user's screenshot); the logged layout at launch was 956x440. L.A. Noire (BLES00933) boots and stops on about 77
unimplemented imports (cellSync mutexes, cellGcmSys flip and system mode, cellSpurs workloads, sceNpInit, ...).
