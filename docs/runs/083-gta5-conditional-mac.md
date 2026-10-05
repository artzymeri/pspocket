# Run 083 · Conditional rendering waits for no count

2026-10-05 · eeda058+ (D-115) · M4 Mac · `pspocket-boot ... --timeout 200 --frames DIR --frame-every 1500 --shader-cache
inventory/gta5/shaders`, the intro and the walk of run 080.

| | Draws skipped by conditional rendering | GPU per frame | fps |
|---|---|---|---|
| Before (run 080, 330 s) | 7,736 (each by an older frame's count) | 14 to 16 ms | 30 |
| Pending counts render (D-115, 200 s) | 0 | 8.7 ms | 29.8 to 30 |

- Every conditional test in the run found its query's count still pending: GTA V tests in the frame it queries.
- The frames at 50, 100 and 150 s show the bank, the hostage scene and the corridor as before.
