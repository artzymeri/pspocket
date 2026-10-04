# Run 027 · GTA V boot, round 25: PSN handlers

2026-10-04 · uncommitted tree on 437b72e · M4 Mac · `scripts/boot.sh` (inventory/boot-025.log).

| Step | Result |
|---|---|
| PSN | The manager callback is kept (offline, never called) |
| Gaps | `sceNpBasicRegisterContextSensitiveHandler`, `sceNpBasicUnregisterHandler` |
| End | 60 s timeout, no crash; 1,722 flips |
