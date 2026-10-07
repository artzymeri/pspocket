# Compatibility

Every game tried, against the "Flawless" bar (D-147). Updated after every session that tries a game; a fix is general
(no title checks, CLAUDE.md rule 6) and gets a regression test where it can (rule 7). GTA V and every game that has
reached Flawless are re-checked after each change (scripts/gate.sh, D-149), so nothing slides back.

**Flawless** means all of these, on the iPhone:
- boots from the ISO picked on the iPhone, no Mac involved;
- runs at the game's own frame rate (30 or 60 fps) for a full play session, no stutters after preparing;
- graphics match the real PS3 (against RPCS3 or PS3 footage): no flicker, wrong colours or missing objects;
- sound in time, controls responsive, saves load and survive restarting the app;
- no crash in an hour of play; the phone may get warm but must not throttle below the frame rate.

**Playable**: small issues, named. **Boots**: gets past its first screens but is not playable. **Broken**: the first
failure is named.

| Game | Title ID | Rating | Where it stands | Last checked |
|---|---|---|---|---|
| Grand Theft Auto V | BLES01807 | Playable | Prepared on the phone in 7:20 (route B, run 096) and running from that library at 29.5 to 29.9 fps in its opening screens; the prologue plays (with the app's built-in translation, run 089): 30 fps in light scenes, 10 to 20 in heavy ones (CPU-bound, SPURS mostly EDGE); sound slows to 0.5 to 0.8 when the cores are full; pipelines new to the shader cache stalled for up to a second (now compiled in the background, D-158, to confirm on the phone); a memory spike ended the app after ten minutes (fixed in D-148, to confirm); multisampled targets drawn with one sample. No flicker since D-144. **The EDGE/GFX job chain stop (the iPhone's run 100 after 110 s; on the Mac 3 in 10 walks under load) is fixed on the Mac (D-225: 0 in 11 loaded walks)**, to confirm on the phone. | Mac, runs 172-191, 2026-10-07 |
| Assassin's Creed | BLES00158 | Boots | On the Mac (runs 099, 109–117): all 140 imports implemented (D-160), its fiber switches run (D-161), its long file names found (D-162), its depth textures decoded (D-163); the autosave notice, the disclaimer, the intro video, the title screen, a new game and CONTINUE, the Animus loading, the Abstergo lab, the Memory DNA Timeline and **Altaïr in the Animus training, walking, the pause menu, the crowd lesson** (runs 120-125, 58 to 60 fps, 30 to 32 in the crowd since D-210 and D-211): **median 57.7 fps with the GPU drawing, the lab's sun shadows drawn since D-214 (missing before)** (runs 117-133; 29 before D-203 to D-208, which keep its PPU switch, its busiest SPU job chain and a module whose data changes with every load translated). SPU code interpreted: 184 instructions a session. In the crowd 4.2 Mac cores are busy since D-213 (5.6 before), the SPU threads most: CPU time per frame is the work for the iPhone. The crowd's SPU threads since: -7.7% with D-219 (run 142), -12% more with D-220's module tracker (runs 142-145: 13.16 to 11.59 s per 5 s, 33 to 37 fps). Prepared on the phone in 3:37 (run 096), before these fixes: the phone prepares it again on its next start (translator generation 3). | Mac, runs 142-145, 2026-10-07 |
| L.A. Noire | BLES00933 | Broken | Prepared on the phone, then stopped before its first frame on an encrypted SDATA file (its shaders), a SPURS workload attribute and 12 more calls, all done in D-165 (not yet run on the phone). | phone, 2026-10-06 |
