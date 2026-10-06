#!/bin/bash
# FFmpeg's decoders as static libraries for pspocket's own media libraries (D-181): cellVdec, cellAdec, cellAtrac and
# cellDmux decode as RPCS3's do (rpcs3/Emu/Cell/Modules/cellVdec.cpp, cellAtracXdec.cpp: libavcodec and libswscale),
# so a game needs none of the PS3 firmware's decoder modules.
#
#   scripts/build-ffmpeg.sh [--tarball PATH] [macos|ios|all]
#
# Builds build/ffmpeg/macos (arm64, macOS 13: the Mac's tests and pspocket-boot) and build/ffmpeg/ios (arm64, iOS 17:
# the app), each with include/ and lib/ (libavcodec, libavutil, libswscale, libswresample). The CMake build finds
# build/ffmpeg/macos (or -DPSPOCKET_FFMPEG_DIR=...); scripts/ios.sh links build/ffmpeg/ios into the app.
#
# Licence: configured --disable-gpl --disable-nonfree --disable-version3, then only LGPL-2.1-or-later decoders,
# parsers and libraries, which combine with pspocket's GPLv2 (D-001). No GPL, version3 or nonfree component here.
#
# Source: the unmodified release tarball ffmpeg-7.1.1.tar.xz (https://ffmpeg.org/releases/), checked against the
# SHA-256 below before anything is extracted. It is not kept in the repository: pass --tarball, or put it in
# build/ffmpeg/src/. The build needs no network.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=7.1.1
SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
OUT="$ROOT/build/ffmpeg"
TARBALL="$OUT/src/ffmpeg-$VERSION.tar.xz"
TARGETS=all
while [ $# -gt 0 ]; do
    case "$1" in
        --tarball) TARBALL="$2"; shift 2 ;;
        macos|ios|all) TARGETS="$1"; shift ;;
        *) echo "usage: scripts/build-ffmpeg.sh [--tarball PATH] [macos|ios|all]" >&2; exit 64 ;;
    esac
done
say() { echo "==> $*" >&2; }
[ -f "$TARBALL" ] || { echo "build-ffmpeg: no $TARBALL (download ffmpeg-$VERSION.tar.xz from ffmpeg.org, or --tarball)" >&2; exit 1; }
actual="$(shasum -a 256 "$TARBALL" | cut -d' ' -f1)"
[ "$actual" = "$SHA256" ] || { echo "build-ffmpeg: $TARBALL has SHA-256 $actual, not $SHA256" >&2; exit 1; }
mkdir -p "$OUT/src"
[ "$(cd "$(dirname "$TARBALL")" && pwd)/$(basename "$TARBALL")" = "$OUT/src/ffmpeg-$VERSION.tar.xz" ] || cp "$TARBALL" "$OUT/src/"
SRC="$OUT/src/ffmpeg-$VERSION"
if [ ! -f "$SRC/configure" ]; then
    say "extracting ffmpeg-$VERSION"
    tar -xJf "$OUT/src/ffmpeg-$VERSION.tar.xz" -C "$OUT/src"
fi

# What pspocket's media libraries decode: the PS3's video (AVC/H.264, MPEG-2, MPEG-4 part 2 as in DivX) and audio
# (ATRAC3, ATRAC3plus and their "AL" forms, MP3 and MPEG-1/2 audio, AAC, AC-3 and E-AC-3, big- and little-endian PCM),
# with the parsers that split their streams. The HEVC parser is there only for its SEI code: 7.1.1's H.264 decoder calls
# ff_aom_uninit_film_grain_params (h2645_sei.c), whose aom_film_grain.o only HEVC's SEI selects, so an H.264 build
# without it does not link (D-195).
DECODERS=h264,mpeg2video,mpeg1video,mpeg4,atrac3,atrac3al,atrac3p,atrac3pal,mp3,mp3float,mp2,mp2float,mp1,mp1float,aac,ac3,eac3,pcm_s16be,pcm_s16le,pcm_s24be,pcm_f32be
PARSERS=h264,hevc,mpegvideo,mpeg4video,aac,ac3,mpegaudio
COMMON=(--disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect
    --disable-gpl --disable-nonfree --disable-version3 --disable-avformat --disable-avdevice --disable-avfilter
    --disable-postproc --enable-avcodec --enable-avutil --enable-swscale --enable-swresample
    --enable-static --disable-shared --enable-pic --disable-debug
    "--enable-decoder=$DECODERS" "--enable-parser=$PARSERS")

build() { # name sdk cflags...
    local name="$1" sdk="$2"
    shift 2
    local prefix="$OUT/$name" work="$OUT/work-$name"
    local sysroot cc
    sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)"
    cc="$(xcrun --sdk "$sdk" -f clang)"
    # The tools configure builds and runs here are the Mac's: its own compiler and SDK.
    local host_sdk
    host_sdk="$(xcrun --sdk macosx --show-sdk-path)"
    local args=("${COMMON[@]}" --prefix="$prefix" --cc="$cc" --sysroot="$sysroot" --arch=aarch64
        --host-cc="$(xcrun --sdk macosx -f clang)" --host-cflags="-isysroot $host_sdk" --host-ldflags="-isysroot $host_sdk"
        --extra-cflags="$*" --extra-ldflags="$*")
    [ "$name" = ios ] && args+=(--enable-cross-compile --target-os=darwin)
    local stamp="$work/configured-with"
    mkdir -p "$work"
    if [ ! -f "$stamp" ] || [ "$(cat "$stamp")" != "${args[*]}" ]; then
        say "configuring FFmpeg for $name"
        (cd "$work" && "$SRC/configure" "${args[@]}" > configure.log 2>&1) || { tail -30 "$work/configure.log" >&2; exit 1; }
        echo "${args[*]}" > "$stamp"
    fi
    say "building FFmpeg for $name"
    make -C "$work" -j"$(sysctl -n hw.ncpu)" > "$work/make.log" 2>&1 || { tail -30 "$work/make.log" >&2; exit 1; }
    make -C "$work" install > "$work/install.log" 2>&1
    say "$prefix: $(ls "$prefix"/lib/*.a | xargs -n1 basename | tr '\n' ' ')"
}

case "$TARGETS" in
    macos) build macos macosx -arch arm64 -mmacosx-version-min=13.0 ;;
    ios) build ios iphoneos -arch arm64 -mios-version-min=17.0 ;;
    all)
        build macos macosx -arch arm64 -mmacosx-version-min=13.0
        build ios iphoneos -arch arm64 -mios-version-min=17.0
        ;;
esac
