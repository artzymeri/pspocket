# Run 010 · GTA V boot, round 8: network up, first job chain

2026-10-03 · build 0d93ebb · M4 Mac · `scripts/boot.sh` (inventory/boot-008.log).

| Step | Result |
|---|---|
| Network | `cellNetCtlInit` succeeds (disconnected), then `sys_net::sys_net_show_ifconfig` |
| SPURS | `_cellSpursJobChainAttributeInitialize(jm 3, SDK 3.30, ...)`: GTA V's first job chain; then the game's assertion |

Changed because of it: the HLE binder takes up to 16 arguments (the ninth and later from the caller's stack, as the
PPC64 ABI and RPCS3 do) and `bool` (b8) arguments; job chain attribute initialize / set name / set halt-on-error and
create-with-attribute (workload marker 0x300: RPCS3 registers job chains without a policy module, so they never run
there); `sys_net_show_ifconfig`. Jobs do not run until the native job chain policy exists, which needs Sony's libsre
as the reference (D-037).
