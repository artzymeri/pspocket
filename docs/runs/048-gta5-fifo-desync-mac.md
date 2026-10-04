# Run 048 · The first mission on the Mac ends on a FIFO desync

2026-10-04 · 574baa0+ · M4 Mac · `pspocket-boot ... --timeout 480 --flash inventory/firmware/dev_flash --spu-dump
inventory/gta5/spu --press cross@45 --press cross@55 --press cross@65 --press cross@75 --press cross@90`
(inventory/boot-048.log). Run notes from this one on carry the number of their log.

The null backend (no frames drawn), the SPUs interpreted, collecting SPU images for D-076. The run reached the first
mission's loading and ended after 775 log lines on `rsx: malformed FIFO command 0x80000000 at 0x001391d0`: the RSX
front end read a command word the game had not finished writing. On ARM, the SPUs' PUTs of command buffers, the
PPU's write of the put pointer and the RSX thread's reads were not ordered (D-077).
