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
| Grand Theft Auto V | BLES01807 | Playable | Built from the Mac's translation, not prepared on the phone yet (route B). The prologue plays: 30 fps in light scenes, 10 to 20 in heavy ones (CPU-bound, SPURS mostly EDGE); sound slows to 0.5 to 0.8 when the cores are full; pipelines new to the shader cache stall for up to a second; a memory spike ended the app after ten minutes (fixed in D-148, to confirm); multisampled targets drawn with one sample. No flicker since D-144. | run 089, 2026-10-05 |
| Assassin's Creed | BLES00158 | Broken | 40 of its 140 imports are not implemented. First failure: the main thread spins on `cellSyncMutexTryLock` (24 million calls in 90 s on the Mac, 27,843 logged on the phone) and never creates the RSX. | Mac and phone, 2026-10-05 |
| L.A. Noire | BLES00933 | Broken | About 77 unimplemented imports (cellSync mutexes, cellGcmSys flip and system mode, cellSpurs workloads, sceNpInit, ...). | phone, run 087 |
