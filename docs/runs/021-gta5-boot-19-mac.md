# Run 021 · GTA V boot, round 19: GTA V's own SPU module runs, audio starts, no gaps left in what it calls

2026-10-04 · uncommitted tree on 5cabf48 (this round's commits) · M4 Mac · `scripts/boot.sh` (inventory/boot-019.log).

| Step | Result |
|---|---|
| SPU tasks | As in run 020: 8 tasks start on the workers |
| Custom workload | `cellSpursAddWorkload`: workload 12, GTA V's policy module (0x11B0 bytes, priority 12 on SPUs 0-3); `_cellSpursWorkloadFlagReceiver` makes it the flag receiver. Four workers load it and run it (D-051) |
| Audio | `cellAudioInit`; port 0 (8 channels, 8 blocks) and port 1 (2 channels, 16 blocks); a notify event queue gets an event every 5.33 ms (D-052) |
| Imports | 153 of 362 implemented; **no unimplemented function or syscall called** |
| End | No crash; 60 s timeout. Thread report: the main thread waits on semaphore 0x9600ed00, which nobody posts; the render thread polls; the RSX is idle (1025 words processed, no flip) |

What is left on the critical path is the three job chains (EDGE/GFX, GraphicsMain, Secondary): they get work but no
native job chain policy runs them (D-037 stage 5), and the render thread is presumably waiting for their results.
