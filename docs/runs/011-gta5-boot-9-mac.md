# Run 011 · GTA V boot, round 9: job chains created, an event flag for SPU dependencies

2026-10-03 · build 4f47fde · M4 Mac · `scripts/boot.sh` (inventory/boot-009.log).

| Step | Result |
|---|---|
| Job chains | Seven created natively: DefaultShort, DefaultLong, ShapeTests, AudioEngine, GraphicsMain, GraphicsOther, Secondary |
| Taskset | "dependency_spu", workload 8 of the primary SPURS instance |
| Event flag | Initialized on that taskset, then `cellSpursEventFlagAttachLv2EventQueue` (unimplemented); the game asserts (`tw 31,r1,r1` at 0x00a37308) |

Changed because of it (D-040): event flag lv2 queue attach and detach, `cellSpursEventFlagSet` and
`cellSpursEventFlagWait` on the PPU, `cellSpursAttachLv2EventQueue` / `cellSpursDetachLv2EventQueue` (all imported by
GTA V) with SPU ports kept in the native SPURS instance, and `cellSpursBarrierInitialize`. The HLE binder takes `u8`
arguments.
