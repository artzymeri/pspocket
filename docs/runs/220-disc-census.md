# Run 220 · The discs' own executable files (D-239)

2026-10-08 · ps3disc --modules · M4 Mac · GTA V (BLES01807) and Assassin's Creed (BLES00158) disc images (inventory/,
kept out of the repository).

| | files named as executables in USRDIR (besides EBOOT.BIN) | PRX modules | SPU ELFs | need a licence | seconds |
|---|---|---|---|---|---|
| GTA V | 0 | 0 | 0 | 0 | 0.36 |
| Assassin's Creed | 1 (data: no SELF or ELF header) | 0 | 0 | 0 | 0.01 |

Neither game ships a module or an SPU program of its own outside its executable: GTA V's code is all in EBOOT.BIN (its
SPU programs in its segments, D-226; its jobs in data archives), Assassin's Creed's one .bin file is data. The game
module pipeline (D-238) and the disc scan (D-239) are covered by synthetic tests until a local game uses them; their
preparation and the gate are unchanged.
