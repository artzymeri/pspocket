# Run 038 · GTA V boot, round 36: the MP3 codec from /dev_flash

2026-10-04 · 68706ad+ · M4 Mac · `pspocket-boot ... --timeout 120 --frames ... --frame-every 30` (inventory/boot-037.log).

| Step | Result |
|---|---|
| Local store dump | The job keeps the module address at LS 0x10B60: the 96 KB block. GTA V's statically linked MultiStream library fills it from `/dev_flash/sys/external/flashMP3.pic`, Sony's MP3 codec, and pspocket had no /dev_flash (`CELL_ENOTMOUNTED`): the load failed quietly ("cellMSMP3Decoder: Error loading MP3 codec") |
| Fix | /dev_flash from the user's own firmware, as RPCS3 (D-067) |
