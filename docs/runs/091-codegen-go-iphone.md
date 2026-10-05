# Run 091 · Route B's go/no-go on the iPhone: the app writes, signs and runs its own library (D-150)

2026-10-05 19:45-19:55 · working tree on efdfcb2 (runtime/codesign, the codegen.dlopen probe) · iPhone 17 Pro Max,
iOS 26, development install by cable (`scripts/ios.sh install`), Developer Mode on.

`scripts/codegen-probe.sh` then `scripts/codegen-probe.sh --identity` (the user approved macOS's keychain export once):

| Library | Made by | Signed by | Identifier | iOS |
|---|---|---|---|---|
| clang-bundleid.dylib | clang | codesign, on the Mac | the app's bundle ID | signature registered, library validation passed, ps_answer() = 42 |
| clang-filename.dylib | clang | codesign, on the Mac | codesign's default | the same |
| ours-bundleid.dylib | runtime/codesign's Mach-O writer | codesign, on the Mac | the app's bundle ID | the same |
| own library | the app, on the phone (`answer_dylib`, 24,976 bytes) | the app, on the phone (runtime/codesign; team M7XJA39ZV6, chain of 2) | the app's bundle ID | the same |

The self-test passed 9 of 9 probes in 1.05 s; the codegen probe took 29 ms. Route B is a go for development installs
(D-150). The identity lives in the app's Library/Application Support/pspocket-codegen and in inventory/signing on the
Mac (never committed).

Also in this round: the gate's walk failed once on its fps floor (one 5 s window at 19.8 fps at flip 2,400, the GPU in
its slow state, no wait for the GPU, nothing on the walk's path changed) and passed when run again (slowest window 27.2):
noise from the Mac, to be retried once automatically before the gate fails.
