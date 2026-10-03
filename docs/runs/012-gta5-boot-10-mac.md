# Run 012 · GTA V boot, round 10: SPU task creation begins

2026-10-03 · build 1c8ec8f · M4 Mac · `scripts/boot.sh` (inventory/boot-010.log).

| Step | Result |
|---|---|
| Event flag | `cellSpursEventFlagAttachLv2EventQueue`: flag 0x1da7380 of "dependency_spu" gets queue 0x8d00de00 on SPU port 0x10 |
| Tasks | `cellSpursTaskGetLoadableSegmentPattern` on the task ELF at 0x180a500 (unimplemented); the game asserts (`tw 31,r1,r1` at 0x00a372f0) |

Changed because of it (D-041): the four LS pattern helpers, `_cellSpursTaskAttributeInitialize`,
`cellSpursCreateTask`, `cellSpursCreateTaskWithAttribute` and `cellSpursWakeUp`. Tasks are created and marked ready;
running them needs the native taskset policy (D-037 stage 3b), which comes next. Bink's tasksets, which shut down when
task attribute initialize failed (run 009), now get their tasks too.

Also fixed: an exiting lv2 thread woke its joiner before releasing its stack and ID, so for a moment after
`sys_ppu_thread_join` returned the thread could still be found (`test_lv2` failed about half the time under parallel
load). The joiner is now woken last.
