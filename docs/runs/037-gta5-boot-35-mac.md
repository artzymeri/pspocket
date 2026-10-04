# Run 037 · GTA V boot, round 35: a 96 KB buffer never filled

2026-10-04 · 68706ad+ · M4 Mac · `pspocket-boot ... --timeout 120 --frames ... --frame-every 30` (inventory/boot-036.log).

| Step | Result |
|---|---|
| Memory dump | `--dump-on-fault` (D-067): the module's address is one of the job's parameters, a 96 KB heap block that is all zeros, recorded in a global by the audio setup code |
