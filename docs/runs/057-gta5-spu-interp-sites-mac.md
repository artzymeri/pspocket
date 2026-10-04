# Run 057 · Where the SPUs' time went: jobs interpreted after their first run

2026-10-04 · 37f47f9+ · M4 Mac · `pspocket-boot ... --timeout 300 --frames inventory/gta5/frames-057 --frame-every 600
--trace-frame 3000 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu --press cross@45 ...
--press cross@90` (inventory/boot-057.log).

- The interpreter's counts by program and address (D-084): 16.6 billion SPU instructions interpreted from job entries
  (local store 0x4C00) with no program attached, and 12 billion more at another job address, in 300 s.
- The cause: a GET into a job binary's range dropped its lifted program, and jobs copy their buffers into their own
  data, so every job after its first run was interpreted. D-085 drops a program only for writes to its instructions.
