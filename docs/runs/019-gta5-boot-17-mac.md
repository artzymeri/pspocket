# Run 019 · GTA V boot, round 17: 60 seconds without a crash, waiting on the SPUs

2026-10-04 · uncommitted tree on ad558c6 (this round's commit) · M4 Mac · `scripts/boot.sh` (inventory/boot-017.log).

| Step | Result |
|---|---|
| Input | `cellKbInit(1)`: no keyboard; `cellPadInit(7)`: the virtual controller on port 0; rumble set to 0 |
| Custom workload | `cellSpursAddWorkload` on the primary SPURS: GTA V's own SPU policy module (0x11B0 bytes at 0x1A6AF00, data at 0x2044400, contention 5..5). Unimplemented; the game goes on |
| Audio | Output checked and configured (2 channels LPCM, 48 kHz), Bink: "Using 2 speakers"; `cellAudioInit` and `cellAudioPortOpen` unimplemented; the game goes on |
| Render targets | Nine tiled regions in local memory (four with compression modes 7 and 10, not emulated, as in RPCS3) and a 1280x768 Z-cull region |
| Frame | `cellSysutilCheckCallback` once, then five `cellSpursRunJobChain` on "GraphicsMain" (workload 6) |
| End | No fatal error: the run ends at the 60 s timeout, 138 of 362 imports implemented |

Every SPU workload the game has started (dependency tasks, zlib inflate, Bink, the EDGE and graphics job chains, its own
policy module) is signalled but never runs. The game has drawn up its first frame and is now waiting for that work.
Next: SPURS running SPU code natively (D-037 stages 3b and 5), and a thread report at the timeout that shows what each
guest thread is waiting for.
