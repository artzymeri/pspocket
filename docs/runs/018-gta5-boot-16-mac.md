# Run 018 · GTA V boot, round 16: the first graphics job chain runs, shaders load

2026-10-04 · uncommitted tree on ad558c6 (D-045, D-047, D-048, `cellSpursRunJobChain`) · M4 Mac · `scripts/boot.sh`
(inventory/boot-016.log).

| Step | Result |
|---|---|
| Job chain | `cellSpursRunJobChain` on "EDGE/GFX" (workload 11): signalled, as libsre does; no SPU runs its jobs yet |
| Vblank | `cellGcmSetVBlankHandler`; the interrupt thread calls the handler at 59.94 Hz |
| Loading | The game reads on through its archives and looks for loose shader files (`common/shaders/psn_final/*.cgx`, not on the disc: they live inside the archives) |
| Next gap | `cellKbInit` (sys_io, unimplemented); the game asserts (`tw 31,r1,r1` at 0x00b5684c) |

Changed because of it: sys_io, with one virtual controller and no keyboard (D-049).
