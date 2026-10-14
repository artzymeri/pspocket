# Runs 324–326 · Render passes of a frame shaped like GTA V's, with and without D-945

2026-10-14 · 676e2f7 + D-945 · M4 Mac (busy: other agents' builds, two compilers at about 70% each) ·
`build/host/tests/rsx/test_rsx_metal "[.bench]"`: the synthetic frame in tests/rsx/test_metal.mm, its passes, sizes
and clears in the order of GTA V's traced frame (run 291), every draw one triangle; 600 frames each way, the numbers
of the last 300.

GTA V's prologue walk was not run: its disc image is not on this Mac, and no iPhone was connected to copy it from.

## Separate runs (run 324)

| | render passes a frame | attachments loaded | stored | not stored | clears done by a later pass |
|---|---|---|---|---|---|
| merging off | 57.0 (targets 53, flip 1, 3 clear only) | 137.3 MB | 144.7 MB | 0 | 0 |
| D-945 | 53.0 (targets 50, flip 1, 2 clears in passes of their own) | 119.8 MB | 127.1 MB | 0 | 6.0 |

GPU time a frame, alternating off/on, four rounds: 3.90/3.19, 3.47/3.33, 3.26/3.37, 3.99/3.10 ms (separate backends:
it swings with the other builds' load).

## In-run A/B (run 325, PSPOCKET_GPU_AB=10, 1,200 frames)

| window | GPU ms off | on | RSX thread CPU ms off | on | passes off | on | MB loaded off | on | MB stored off | on |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 4.488 | 4.211 | 1.149 | 1.104 | 57.0 | 53.0 | 137.1 | 119.8 | 144.7 | 127.1 |
| 2 | 4.149 | 4.216 | 1.093 | 1.044 | 57.0 | 53.0 | 137.3 | 119.8 | 144.7 | 127.1 |
| 3 | 4.963 | 4.006 | 1.675 | 1.627 | 57.0 | 53.0 | 137.3 | 119.8 | 144.7 | 127.1 |
| 4 | 3.529 | 3.268 | 1.193 | 1.145 | 57.0 | 53.0 | 137.3 | 119.8 | 144.7 | 127.1 |
| 5 | 3.219 | 3.277 | 1.405 | 1.383 | 57.0 | 53.0 | 137.3 | 119.8 | 144.7 | 127.1 |
| 6 | 3.164 | 3.042 | 1.468 | 1.445 | 57.0 | 53.0 | 137.3 | 119.8 | 144.7 | 127.1 |

Means: GPU 3.92 to 3.67 ms (lower in 4 of 6 windows), RSX thread CPU 1.33 to 1.29 ms (lower in all 6).

## Tests (run 326)

test_rsx 1.9 s, test_rsx_metal and test_rsx_metal_interpreted pass (107 cases), the D-945 cases among them.
test_hle: one failure, the same as on main at 676e2f7 (the MPEG layer 2 fuzz, "the sample as it is").
