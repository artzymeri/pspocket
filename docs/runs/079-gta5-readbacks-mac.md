# Run 079 · The per-frame NV0039 copy without waiting for the GPU

2026-10-05 · 3b537de+ (D-107) · M4 Mac · `pspocket-boot ... --timeout 130 --frames DIR --frame-every 1500
--rsx-recover --flash inventory/firmware/dev_flash --press cross@45 ... --press cross@90`, the backend's report every
300 flips, in the first mission.

| | Readbacks a frame | Accesses that waited | RSX thread waiting for the GPU | GPU per frame |
|---|---|---|---|---|
| Before (D-106) | 1 | - | 9.3 ms | 17.0 ms |
| Deferred, waiting whenever the FIFO went idle | 1 | - | 13.1 ms | 16.8 ms |
| Deferred, polled (no hazard checks: wrong) | 1 | - | 0.00 ms | 17.3 ms |
| Deferred, with hazard checks | 1 | 1.75 | 8.6 to 9.1 ms | 16.9 ms |
| Copied on the GPU, own semaphores forwarded (D-107) | 1 | 0 | **0.00 ms** | 16.4 to 17.4 ms |

- The copy: NV0039 from 0xcd5b2000 (a 320-wide W16Z16Y16X16 float target, pitch 2,560) to 0xc2704600 (80x45, pitch
  640, local memory), sampled by the next draws. The hazard checks logged the texture read of 0xc2704600 (+0x7080)
  waiting for it. With the copy on the GPU, the semaphore at 0x40300490 the RSX releases after it and acquires itself
  waited instead, until acquires saw the RSX's own queued releases.
- 30 fps throughout; frame 3,000 as in runs 071 and 077.
- On the Mac the game already ran at its 30 fps cap, so this frees the RSX thread for slower devices (the iPhone).
