# Run 069 · Every module the dependency tasks load runs lifted

2026-10-05 · 9cd1f31+ (D-099) · M4 Mac · `pspocket-boot ... --timeout 150 --frames inventory/gta5/frames-069
--frame-every 1500 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu --press cross@45
... --press cross@90` (inventory/boot-069.log).

- No address of the dependency task interpreted (run 068: 3.9 million instructions); 865,927 module loads found their
  lifted program.
- What the SPUs still interpreted: four addresses of one job binary (spu_012bf13511b9f1b0, 1.5 million instructions
  at its most), reached through function pointers; the next SPU lift takes them as seeds.
- The first mission at 29 to 30 fps.
