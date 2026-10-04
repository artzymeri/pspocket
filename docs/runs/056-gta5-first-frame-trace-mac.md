# Run 056 · The first frame trace: a deferred renderer reading three inputs wrongly

2026-10-04 · 750963c+ · M4 Mac · `pspocket-boot ... --timeout 280 --frames inventory/gta5/frames-056 --frame-every 600
--trace-frame 3000 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu --press cross@45 ...
--press cross@90` (inventory/boot-056.log), with Time Profiler.

- Frame 3,000 (the first mission), traced draw by draw (D-084): a deferred renderer. Its lighting pass read an
  A8B8G8R8 G-buffer target as A8R8G8B8, a B8 target as B8, and the depth buffer as a colour texture, and got all three
  wrong, so the HDR target stayed black and the frame showed the albedo in the wrong colours (D-083).
- The profile had `GuestMemory::is_committed`'s mutex at 14% of the SPURS workers' time (D-082 makes it lock-free).
