# Run 077 · GPU time per frame and why render passes end

2026-10-05 · 4f08184+ (D-105) · M4 Mac · `pspocket-boot ... --timeout 120 --frames DIR --frame-every 1500 --rsx-recover
--flash inventory/firmware/dev_flash --press cross@45 ... --press cross@90`, the backend's report every 300 flips, in
the first mission.

| | GPU per frame | Draws | Passes | Ended by targets / depth conversion / target copy / colour conversion |
|---|---|---|---|---|
| Before (D-104) | 23.5 ms | 1,041 | 113 | 67 / 30 / 8 / 2 |
| Writes count, not attachments | 23.0 ms | 1,041 | 111 | 68 / 27 / 8 / 2 |
| Eager conversions at pass end (dropped) | 27.2 ms | 1,041 | 111 | 71 / 24 / 8 / 2 |
| Stencil apart, feedback reads once (D-105) | 17.8 ms | 1,041 | 101 | 69 / 16 / 8 / 2 |

- 30 fps throughout; frame 3,000 as in run 071.
- Next for the GPU: the 16 remaining conversion splits and 8 target copies (a depth or colour target read by a pass
  that draws elsewhere could be sampled without converting, as RPCS3 samples the depth image directly).
