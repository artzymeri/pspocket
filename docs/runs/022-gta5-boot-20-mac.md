# Run 022 · GTA V boot, round 20: the job chains run

2026-10-04 · uncommitted tree on 437b72e (D-054, D-055) · M4 Mac · `scripts/boot.sh` (inventory/boot-020.log).

| Step | Result |
|---|---|
| Job chains | With libsre's job chain layout and the native job chain policy, the Secondary chain (SPURS 0x10016000, one SPU) runs its first job: GTA V's 0xBFA0-byte binary, entered as libsre enters it |
| End | That job writes interrupt mailbox code 0xC0 (`sys_event_flag_set_bit_impatient`) at LS 0x0FE78, which the workers did not handle: fatal |

Changed because of it: SPU code sets lv2 event flag bits (D-056).
