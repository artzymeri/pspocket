# runtime/hle — firmware modules resolved by NID (Phase 2+)

One file per module (cellGcmSys, cellSysutil, cellFs, cellPad, cellAudio, cellSpurs, ...), implementing only the
functions the inventory shows GTA IV imports. Everything else resolves to a generated stub that logs
`UNIMPLEMENTED module::function (NID)` and returns an error.
