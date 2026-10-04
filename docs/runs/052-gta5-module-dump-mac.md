# Run 052 · Collecting the modules: 32,570 images

2026-10-04 · 02e8498 (D-080) · M4 Mac · `pspocket-boot ... --timeout 420 --flash inventory/firmware/dev_flash --spu-dump
inventory/gta5/spu --press cross@45 ... --press cross@90` (inventory/boot-052.log), stopped by hand.

With the module resolver of D-079 saving every module without a lifted program, the dump grew to 32,570 images
(533 MB) within minutes: 82 distinct modules, each loaded at thousands of LS addresses, and a lifted program was then
keyed by its LS address. The run was stopped and the module dumps deleted; lifted code that runs wherever the image
sits is D-081. The frame rates matched run 051 until saving slowed the workers ("Loading Story Mode" 7 to 8).
