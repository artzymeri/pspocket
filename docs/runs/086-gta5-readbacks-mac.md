# Run 086 · The walk without readbacks, and a job chain stopped by an overwritten descriptor

2026-10-05 · 6a3aa8f, 453f9ba · M4 Mac, `pspocket-boot --gpu metal`, the walk of run 080 (`--press cross@45 ...
--press ls-up@160:20`, 170 s), seven times; the last with all 364 imports implemented (D-136 to D-141).

Readbacks the RSX thread waited for, per walk (D-137):

| | Readbacks (synchronous) | Their wait per frame | RSX failures | "G8B8 is not decoded yet" |
|---|---|---|---|---|
| Before (run of D-134, protect2) | 42 | 0.09 to 0.16 ms | 18 | yes |
| After (seven walks) | 0 | 0 | 1 (the 4x multisampled target) | no |

The texture behind most of them is GTA V's 640x360 F_W16Z16Y16X16 target at 0xcd710000 read as a 1280x720 A8R8G8B8
texture; it is now laid over memory on the GPU ("0.01 to 0.06 targets laid over memory on the GPU" a frame). On the
iPhone the same texture cost 270 readbacks in one session, in the 2 to 4 fps phase; the G8B8 target read as G8B8 cost
325 to 463.

The first walk stopped at 95 s: the EDGE/GFX job chain (workload 11) found CELL_SPURS_JOB_ERROR_INVALID_BINARY in
descriptor 0x102d1400, whose first 16 bytes were `201d2000 00000000 00000000 00000000` in memory, the rest a
descriptor (its DMA list, and 0x180 bytes on another descriptor's valid header). The iPhone's NUM_CACHE stop of D-131
had a descriptor overwritten with 16-bit pairs. This build had no D-127 code: something writes over the chain's
descriptors. D-138 logs the SPU DMA writes per thread so the next stop names the writer; the six walks since did not
stop. The last walk, with every import implemented, flipped 3,783 frames (3,157 in protect2's), called no unimplemented
function, and reached only sys_net_initialize_network_ex and sceNp2Init of the new ones.
