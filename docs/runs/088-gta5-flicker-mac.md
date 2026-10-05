# Run 088 · The colour flicker found on the Mac and fixed (D-144); the iPhone's GPU archives keep part of what they hold

2026-10-05 16:18-17:00 · library build with D-143 · iPhone 17 Pro Max (boots 4 and 5 of run 087's session), then
M4 Mac (seven walks).

**iPhone, boots 4 and 5 (16:18 and 16:20):** 29 to 30 fps on the loading screens while the shader cache ran (the
speaker ran dry once at the start). The cache found 3,623 of 11,756 pipelines in the GPU binary archives in boot 4 and
5,307 of 11,770 in boot 5, and was done in 43 and 41 s (boot 3: not done after 84 s). Every archive save reported
"expecting 'fragment' stage" or "missing 'vertex' stage in pipeline no. N" (N from 217 to 447), yet left a file the
next boot partly used. In play (boot 5, from 16:21:31): 27 down to 14 fps, heat fair, the speaker running dry 1 to 8
times per 5 s; the RSX thread compiled 42 programs and 87 pipelines new to the cache in 300 frames (5.4 s of
compiling). The user: the flicker "again the same", then stopped the game.

**Mac, the flicker.** The 128 s scripted walk of run 086 (the prologue), every second frame from frame 2150 saved;
each frame cut to a 64x36 thumbnail and compared with the one before, a mean difference over 25 counted as a jump:

| Walk | What changed | Frames | Jumps (> 25) | Big (> 60) | fps |
|---|---|---|---|---|---|
| a | as built (D-137, D-143) | 692 | 262 | 108 | 27.2 |
| b | D-137's overlay off (PSPOCKET_RSX_WRITE_BACK_TARGETS=1) | 743 | 215 | 76 | 27.0 |
| c | no shader cache | 172 | 25 | 10 | 5.8 |
| d | G8B8 textures skipped, as before D-137 | 530 | 322 | 148 | 24.0 |
| e | textures hashed instead of page protection (PSPOCKET_RSX_HASH_TEXTURES=1) | 742 | 2 | 0 | 29.8 |
| f | D-144's first fix (the resolving state) | 813 | 2 | 0 | 28.8 |
| g | D-144 | 829 | 2 | 0 | 30.0 |

The jumps were textures drawn with stale contents. Walk e put them on D-134's page protection (13:38 that day; the
user: "the game ran good this morning"); reading the fault path found the race (D-144). The regression test written
for it crashed the first fix (a store faulting while its page was released); D-144 passes it 6 times in 6, and the
D-134 code crashes it 3 times in 3. The two jumps left in walks e to g are camera cuts. Walk g: 37 write faults and
0.01 MB hashed per frame.

**Mac, the archives:** a scratch program on the M4 saved archives holding each suspect kind of pipeline (the same one
twice, blending on and off, no colour written, no colour attachment, discard, depth written, a fragment function
without outputs): all saved. The failure is the iPhone's; it is next.
