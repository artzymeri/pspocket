# Run 028 · GTA V boot, round 26: NP lookup

2026-10-04 · uncommitted tree on 437b72e · M4 Mac · `scripts/boot.sh` (inventory/boot-026.log).

| Step | Result |
|---|---|
| PSN | The basic event handler is kept (offline, never called) |
| Gaps | `sceNpLookupInit` |
| End | 60 s timeout, no crash; 1,720 flips; the RSX idle at the end. The main thread waits on semaphore 0x96015700 from second 4, the DVD streamer on its zlib taskset's SPURS event flag |

GTA V imports 46 more sceNp functions; the ones with a simple offline behaviour in RPCS3 are implemented together next.
