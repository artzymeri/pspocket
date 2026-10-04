# Run 050 · Fifteen minutes without a desync; where the SPU workers' time goes

2026-10-04 · a532a5f (D-077, D-078) · M4 Mac · `pspocket-boot ... --timeout 900 --flash inventory/firmware/dev_flash
--spu-dump inventory/gta5/spu --frames inventory/gta5/frames-050 --frame-every 150 --press cross@45 ... --press
cross@90` (inventory/boot-050.log), with an Instruments Time Profiler capture (xctrace) of the run.

- No FIFO desync in 15 minutes (runs 048 and 049 ended on one within minutes): D-077 holds.
- The first mission's world draws (D-078), in pink and red: its float render targets were not written back, and
  textures read them in another format (D-080).
- Time Profiler, SPU worker threads: hashing images at every entry (GTA V's own policy module, every job) took about
  a fifth of their time; entering and leaving the SPU FP environment around each run about 3.5% (D-079).
