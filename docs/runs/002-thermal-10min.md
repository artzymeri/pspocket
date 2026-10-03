# Run 002 · 10-minute thermal check

2026-10-03 15:15–15:26 · build e557a8a (Release) · iPhone 17 Pro Max (A19 Pro), iOS 26.6.1 · phone untouched, screen
on (the app disables the idle timer) · no debugger.

Command: `devicectl ... launch --console -- com.artzymeri.pspocket.testhost -tab metal -passes 64` for 610 s, then
`scripts/ios.sh run --autorun --exit` and `report` on the warm phone.

## Load

The test host's Metal tab at its maximum synthetic load: 1 scene pass + 64 fullscreen passes at 1152×640, upscaled with
MetalFX spatial to 1320×733, paced at 30 Hz. GPU only — the CPU is nearly idle (1–2.4 ms per frame), so this is a
lower bound on the heat a running game produces with its PPU and SPU threads busy.

## Results (120 samples, one every 5 s)

| Measure | Value |
|---|---|
| Thermal state | nominal in all 120 samples |
| GPU time per frame | 15.72 ms average from 1:00 to 10:08 (15.71–15.79), max 16.06 ms; 7.2 ms in the first 5 s while the slider ramped |
| CPU time per frame | 1.1–2.4 ms |
| Frame interval | 33.34 ms; worst frame in every 5-second window 33.34 ms (no late presents at all) |
| Dropped frames | 0 of 18,241 |
| Memory footprint | 102 MB, flat |

GPU time didn't drift over ten minutes, so the GPU never clocked down: at ~47% GPU occupancy the A19 Pro holds 30 Hz
without throttling.

## Self-test on the warm phone (right after)

6/6 pass. BE guest access 15.2 GB/s read, 14.9 GB/s write; fault round trip 2.64 µs (3.67 µs cold in run 001);
QoS workload 72–79 ms per class; thermal still nominal at the end.

## What it means

- The output path of plan section 7 (native 1152×640, MetalFX, 30 Hz pacing) is sound on the target device.
- Thermals with a real CPU load remain open. The first runtime that executes real guest work (PPU threads + SPU
  threads) should repeat this with the CPU busy, against the plan's 22 ms worst-case frame budget.
