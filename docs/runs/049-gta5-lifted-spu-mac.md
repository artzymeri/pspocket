# Run 049 · Lifted SPU programs; a second desync

2026-10-04 · 574baa0+ (D-076) · M4 Mac · `pspocket-boot ... --timeout 240 --flash inventory/firmware/dev_flash --audio
--spu-dump inventory/gta5/spu --press cross@45 ... --press cross@90` (inventory/boot-049.log).

The first run with the game's SPU programs lifted (102 programs: tasks, jobs and GTA V's policy module, D-076). They
ran without a crash, at the interpreted runs' pace (D-079 found why). 6,982 addresses had no lifted code; 6,105 of them
were in one task, `dependency_spu`, which copies in the code of the jobs it runs itself. The run ended on
`malformed FIFO command 0x00000207 at 0x002c0544` (after an invalid primitive 80): the second half of D-077, acquire
loads of the FIFO words and the semaphore the game releases commands with.
