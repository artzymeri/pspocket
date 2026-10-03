# runtime/hle — firmware modules on the host, resolved by NID

One file per module (sysPrxForUser, cellGcmSys, cellSysutil, cellFs, cellPad, cellAudio, ...), registered in
`src/modules.cpp`, implementing only the functions the inventory or a runtime log shows GTA V calls (CLAUDE.md
rule 4). Every other import links to a stub that logs `UNIMPLEMENTED module::function (NID)` and returns
CELL_ENOSYS. Linkage, the main-area allocator and TLS live in `src/process.cpp`; see D-033.

Adding a function: write it as `R name(PPUContext&, args...)` with u32/s32/u64/s64 arguments (r3..r10), add
`PS_HLE_FUNC(0xNID, name)` to the module's `register_<module>()`, cite the RPCS3 file its semantics come from, and
run `test_hle` (it checks the NID against the name).
