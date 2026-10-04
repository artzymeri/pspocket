# Run 014 · GTA V boot, round 12: past SPURS setup, the game wants its files

2026-10-04 · build a45e338 · M4 Mac · `scripts/boot.sh` (inventory/boot-012.log).

| Step | Result |
|---|---|
| Bink | Task attribute initialize no longer crashes (run 013's fix) |
| Dependency tasks | The game combines the patterns as D-041 assumed: loadable (8 blocks) minus read-only (6), plus the top 2 KB block for the stack, so 3 blocks and 0x1c00 bytes of context per task. 4 tasks created on "dependency_spu" |
| NP | `sceNp2Init` (pool 0x20000) |
| Game data | `cellGameBootCheck`, then `cellGameDataCheck` (GAMEDATA) and `cellGameContentPermit`, twice |
| Files | `cellFsStat` on a `/dev_bdvd` path, then `cellFsOpen`; the game asserts (`tw 31,r1,r1` at 0x0043c1dc) when it can't open the file |

Changed because of it (D-043, D-044):
- The disc image is read in place as `/dev_bdvd` and `/app_home`, and `boot.sh` finds it or copies it from the iPhone
  once.
- The 20 `cellFs` functions GTA V imports, with lv2 `sys_fs_fcntl`, `sys_fs_chmod` and `sys_fs_utime`.
- `cellGame` (6 functions) with a PARAM.SFO reader and writer.
- `sceNp2Init` and `sceNp2Term`.
