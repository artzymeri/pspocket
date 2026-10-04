# Run 020 · GTA V boot, round 18: SPU tasks run natively

2026-10-04 · uncommitted tree on 5cabf48 (D-050, D-052, D-053 in progress) · M4 Mac · `scripts/boot.sh`
(inventory/boot-018.log).

| Step | Result |
|---|---|
| SPURS | The native scheduler starts 5 workers for "Primary" and 1 for "Secondary" |
| SPU tasks | Run on the workers through the native taskset policy: Bink's two tasks, the four dependency tasks, the two zlib inflate tasks; they start, then wait for work on their event flags |
| Job chains | EDGE/GFX, Secondary and GraphicsMain get work, but have no native policy yet: logged once each, not run |
| Gaps | `cellSpursAddWorkload` (GTA V's own SPU policy module), `cellAudioInit`, `cellAudioPortOpen` |
| End | No crash; at the 60 s timeout the new thread report (D-053) shows the main thread blocked on a semaphore for 57 s and the render thread polling with `sys_timer_usleep`; the RSX is idle with no flip |

Changed because of it: custom workloads with `cellSpursReadyCountStore` and the workload flag (D-051), cellAudio
(D-052).
