# Run 017 · GTA V boot, round 15: video output configured, the first graphics job chain

2026-10-04 · uncommitted tree on ad558c6 (D-045, D-047, D-048) · M4 Mac · `scripts/boot.sh` (inventory/boot-015.log).

| Step | Result |
|---|---|
| Bink | Both "RAD Game Tools Task" tasksets get their task now that the task attribute follows libsre (D-047): a save config with a 0x1400-byte context (2 LS blocks) |
| System cache | `cellSysCacheMount("BLES01807")`: a fresh cache, `/dev_hdd1` = `inventory/gta5/hdd1/caches/BLES01807_BLES01807` |
| System callbacks | One registered (slot 0) |
| Video | Gamma 1.0; device info, state and resolution: HDMI 1280x720 progressive at 59.94 Hz; `cellVideoOutConfigure` with 720p, pitch 0x1400 |
| Display buffers | Two, 1280x720 at local memory offsets 0 and 0x384000 |
| Job chain | "EDGE/GFX" (workload 11; jobs of 0x280 bytes, 4 grabbed at a time, up to 5 SPUs): Edge, Sony's SPU geometry library |
| Next gap | `cellSpursRunJobChain` (unimplemented); the game asserts (`tw 31,r1,r1` at 0x00b3613c) |

The graphics path now waits on SPU work: the next pieces are `cellSpursRunJobChain` on the PPU side (from libsre), then
running SPU tasks and jobs natively (D-037 stages 3b and 5).
