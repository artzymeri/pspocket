# Run 055 · No module changed in local store; a FIFO desync after 195 s of the mission

2026-10-04 · 750963c+ · M4 Mac · `pspocket-boot ... --timeout 280 --gpu metal --flash inventory/firmware/dev_flash
--spu-dump inventory/gta5/spu --press cross@45 ... --press cross@90` (inventory/boot-055.log).

- Run 054 interpreted 4,970 addresses of the modules GTA V's dependency tasks load. None of their loads had been
  changed in local store before running (count: 0), so D-079's check was not the cause. The module tracker lost them
  instead: data copies pushed their runs out, and task switches moved local store without the MFC (D-082).
- The run ended on `malformed FIFO command 0xb28b0bcf` after 195 s of the first mission, the first since run 050 (none
  in runs 050 to 054: 15, 7, 7 and 10 minutes). D-084 adds the FIFO context to that error and a recovery.
