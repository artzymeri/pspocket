# Run 029 · GTA V boot, round 27: the first picture

2026-10-04 · uncommitted tree on c68e12c (D-057, D-058) · M4 Mac · `scripts/boot.sh --frames inventory/gta5/frames
--frame-every 30` (inventory/boot-028.log; before it inventory/boot-027.log and two experiments).

| Step | Result |
|---|---|
| Metal | The first run through the Metal backend (boot-027) saves 46 frames, all black: the game draws a clear and two empty draws while it waits to load. It ends with a fault in the audio mix thread (a voice object still null) at 49 s; the same fault comes with the null backend at 92 s, so it is the game going wrong while stuck, not Metal |
| Loading | The DVD streamer hands work to its zlib task through a PPU2SPU SPURS event flag; `cellSpursEventFlagSet` refused it (RPCS3's direction check). With libsre's check (D-058) the game loads: its tuning, AI and level data |
| Frames | The logo screen fades in over frames 30 to 150: **GTA V's Rockstar logo**, drawn by the Metal backend |
| Gaps | `sceNpTrophyInit`, `sceNpTrophyTerm`, `cellNetCtlGetInfo` (8 calls), `cellSaveDataAutoLoad2`, `sys_net_free_thread_context`, `cellSpursTasksetAttributeSetTasksetSize` |
| End | The game's own assert (`tw 31,r1,r1` at 0x00BD25FC) after the taskset size call failed |
