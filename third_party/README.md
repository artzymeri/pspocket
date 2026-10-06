# Third-party code

Pinned, vendored dependencies. Keep this list short (plan section 11).

| Library | Version | Licence | Used by |
|---|---|---|---|
| [Catch2](https://github.com/catchorg/Catch2) (amalgamated `extras/`) | v3.16.0 | BSL-1.0 (`catch2/LICENSE.txt`) | tests only |
| [FFmpeg](https://ffmpeg.org) (not vendored: `scripts/build-ffmpeg.sh` builds the release tarball, SHA-256 pinned, into `build/ffmpeg/`) | 7.1.1 | LGPL-2.1-or-later (configured `--disable-gpl --disable-nonfree --disable-version3`; decoders, parsers, libswscale, libswresample only) | the media libraries' decoders (D-181) |

Planned when their phase needs them: xxHash (runtime hashing of SPU code and shaders), zlib, metal-cpp.
`std::format` replaces fmt (docs/decisions.md D-005).
