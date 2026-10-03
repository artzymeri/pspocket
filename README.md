# pspocket

A per-game static recompiler that turns a PS3 game's PPU and SPU code into a native, signed iOS app. Game code is
translated to C on the Mac ahead of time and compiled into the app; at runtime the app only reads assets from the
user's own disc dump. No JIT, no interpreter on the hot path. First title: GTA V (BLES01807, D-025).

- Plan: [docs/plan.md](docs/plan.md) · Decisions: [docs/decisions.md](docs/decisions.md) · Rules: [CLAUDE.md](CLAUDE.md)
- Private, personal project. The repo contains no Sony or Rockstar code, keys, firmware or assets, and never will.

## Status

| Phase | Gate | State |
|---|---|---|
| 0 · Inventory and go/no-go | Inventory report, go/no-go | GTA V inventory done, **go** ([docs/phase0-gta5.md](docs/phase0-gta5.md)); runtime trace (job chains) still to do |
| 1 · Toolchain foundation | Samples recompile and match the interpreter; GTA IV ELF lifts | Loader, NID database, manifest done; PPU interpreter and lifter in progress |
| 2 · Runtime core on macOS | Test suite passes; EBOOT boots to its first unimplemented call | Guest memory, logging, timebase done; lv2 kernel and shared reservations in progress (D-018) |
| 3 · SPU | Stress runs, every inventoried SPU program lifts | SPU interpreter, MFC and lifter in progress (D-018) |
| 4 · RSX to Metal | Menu renders on macOS and iOS | RSX FIFO and Metal backend in progress (D-018); iOS test host proven on device ([runs](docs/runs/README.md)) |
| 5–7 · Bring-up, performance, app | | Not started |

## Build

```sh
cmake -S . -B build/host -G Ninja -DPSPOCKET_WERROR=ON
ninja -C build/host
ctest --test-dir build/host --output-on-failure
./build/host/runtime/platform/macos/pspocket-selftest
```

iOS test host on a connected iPhone: `scripts/ios.sh all` (see [app/ios/README.md](app/ios/README.md)).

## Layout

See [CLAUDE.md](CLAUDE.md#layout).
