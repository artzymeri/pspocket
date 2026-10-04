# Run 015 · GTA V boot, round 13: the disc image from the iPhone, the game reads its archives

2026-10-04 · build ad558c6 · M4 Mac · `scripts/boot.sh` (inventory/boot-013.log).

| Step | Result |
|---|---|
| Disc image | First copy from the iPhone app (`scripts/ios.sh pull-iso`, devicectl): 18.74 GB in about 8 minutes, then read in place (D-043): BLES01807, volume PS3VOLUME |
| Archives | `common.rpf`, `ps3.rpf`, `audio_rel.rpf`, five `sfx/*.rpf`, then `part0..4.rpf` and `common.rpf` again, all opened from `/dev_bdvd` |
| Install data | `/dev_hdd0/game/BLES01807_install` created through cellGame (ICON0.PNG copied from the disc); the game looks for the `part*.rpf` installed there first, then falls back to the disc |
| SPU | Two "zlibInflater" tasksets, one task each (inflateServer ELF, 50 LS blocks of context); created, not run |
| Gaps | `cellSysCacheMount`, `cellSysutilRegisterCallback`, `cellVideoOutSetGamma`, `cellVideoOutGetDeviceInfo` (logged, the game carries on), then `_cellGcmInitBody`; the game asserts (`tw 31,r1,r1` at 0x00b1cac4) |

Bink's `_cellSpursTaskAttributeInitialize` is still refused: run 013's logging shows r7 = a stack address, r8 = 0
and garbage in r9/r10. libsre explains it: the function takes six arguments, the fifth a pointer to a save-config
structure {context, size, LS pattern}, the sixth the task argument (D-047).

Changed because of it:
- RSX and graphics setup: lv2 `sys_rsx` with the RSX front end and a null backend, and the 20 `cellGcmSys` functions
  (D-045).
- `scripts/ios.sh pull-iso` read the file size from the wrong place in devicectl's listing (it is under `metadata`),
  printed "0 MB" and skipped its free-space check; fixed.
- The firmware reference: libsre decrypted locally (D-046).
