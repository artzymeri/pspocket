# pspocket

A per-game static recompiler that turns a PS3 game's PPU and SPU code into a native, signed iOS app. Game code is
translated to C on the Mac ahead of time and compiled into the app; at runtime the app only reads assets from the
user's own disc dump. No JIT, no interpreter on the hot path. First title: GTA IV (BLES00229, update 1.08).

- Plan: [docs/plan.md](docs/plan.md) · Decisions: [docs/decisions.md](docs/decisions.md) · Rules: [CLAUDE.md](CLAUDE.md)
- Private, personal project. The repo contains no Sony or Rockstar code, keys, firmware or assets, and never will.

## Status

| Phase | Gate | State |
|---|---|---|
| 0 · Inventory and go/no-go | Inventory report, go/no-go | Tools built; waiting on a decrypted EBOOT ([docs/phase0.md](docs/phase0.md)) |
| 1 · Toolchain foundation | Samples recompile and match the interpreter; GTA IV ELF lifts | In progress |
| 2 · Runtime core on macOS | Test suite passes; EBOOT boots to its first unimplemented call | Guest memory, logging, timebase done |
| 3 · SPU | Stress runs, every inventoried SPU program lifts | Not started |
| 4 · RSX to Metal | Menu renders on macOS and iOS | Not started (iOS test host exists early, D-008) |
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
