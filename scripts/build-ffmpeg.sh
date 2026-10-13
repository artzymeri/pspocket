#!/bin/bash
# FFmpeg's decoders as static libraries for pspocket's own media libraries (D-181): cellVdec, cellAdec, cellAtrac and
# cellDmux decode as RPCS3's do (rpcs3/Emu/Cell/Modules/cellVdec.cpp, cellAtracXdec.cpp: libavcodec and libswscale),
# so a game needs none of the PS3 firmware's decoder modules.
#
#   scripts/build-ffmpeg.sh [--tarball PATH] [macos|ios|all|linux]
#
# Builds build/ffmpeg/macos (arm64, macOS 13: the Mac's tests and pspocket-boot) and build/ffmpeg/ios (arm64, iOS 17:
# the app), each with include/ and lib/ (libavcodec, libavutil, libswscale, libswresample); "all" is both. On a Linux
# host, "linux" (the default there) builds build/ffmpeg/linux for the host's own architecture with the host's clang
# (D-576: the media tests on a Linux machine), the same decoders without VideoToolbox. The CMake build finds
# build/ffmpeg/macos on the Mac and build/ffmpeg/linux on Linux (or -DPSPOCKET_FFMPEG_DIR=...); scripts/ios.sh links
# build/ffmpeg/ios into the app. JOBS=N sets make's parallel jobs (default: every core).
#
# The hosts that run the tests (macos, linux) also get <target>-tests (D-577): the same decoders and the LGPL encoders
# the tests write their streams with (MPEG audio layer II, AC-3, E-AC-3, AAC, DTS, TrueHD, WMA, MPEG-4 part 2, DivX
# 3.11). Only the test programs link those libraries, in place of the decoders' own (third_party/CMakeLists.txt), so
# nothing the app or pspocket-boot links changes.
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
[ "$(uname -s)" = Linux ] && TARGETS=linux
while [ $# -gt 0 ]; do
    case "$1" in
        --tarball) TARBALL="$2"; shift 2 ;;
        macos|ios|all|linux) TARGETS="$1"; shift ;;
        *) echo "usage: scripts/build-ffmpeg.sh [--tarball PATH] [macos|ios|all|linux]" >&2; exit 64 ;;
    esac
done
say() { echo "==> $*" >&2; }
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)}"
[ -f "$TARBALL" ] || { echo "build-ffmpeg: no $TARBALL (download ffmpeg-$VERSION.tar.xz from ffmpeg.org, or --tarball)" >&2; exit 1; }
# shasum (the Mac's, Perl's) where there is one, else coreutils' sha256sum (a Linux without Perl's tools).
sha256() { if command -v shasum > /dev/null; then shasum -a 256 "$1"; else sha256sum "$1"; fi; }
actual="$(sha256 "$TARBALL" | cut -d' ' -f1)"
[ "$actual" = "$SHA256" ] || { echo "build-ffmpeg: $TARBALL has SHA-256 $actual, not $SHA256" >&2; exit 1; }
mkdir -p "$OUT/src"
[ "$(cd "$(dirname "$TARBALL")" && pwd)/$(basename "$TARBALL")" = "$OUT/src/ffmpeg-$VERSION.tar.xz" ] || cp "$TARBALL" "$OUT/src/"
SRC="$OUT/src/ffmpeg-$VERSION"
if [ ! -f "$SRC/configure" ]; then
    say "extracting ffmpeg-$VERSION"
    tar -xJf "$OUT/src/ffmpeg-$VERSION.tar.xz" -C "$OUT/src"
fi

# What pspocket's media libraries decode: the PS3's video (AVC/H.264, also MVC's base view; MPEG-2; MPEG-4 part 2 as in
# DivX; VC-1; DivX 3.11, which is Microsoft's MPEG-4 version 3, D-588) and audio (ATRAC3, ATRAC3plus and their "AL"
# forms, MP3 and MPEG-1/2 audio, MP3's ADU form for the MP3 core that keeps the bit reservoir itself (D-944), AAC, AC-3
# and E-AC-3, DTS with its HD and LBR extensions, Dolby TrueHD, WMA 1 and 2, WMA Pro and WMA Lossless (D-620), big- and
# little-endian PCM), with the parsers that split their streams. The HEVC
# parser is there only for its SEI code: 7.1.1's H.264 decoder calls ff_aom_uninit_film_grain_params (h2645_sei.c),
# whose aom_film_grain.o only HEVC's SEI selects, so an H.264 build without it does not link (D-195).
DECODERS=h264,mpeg2video,mpeg1video,mpeg4,vc1,msmpeg4v3,atrac3,atrac3al,atrac3p,atrac3pal,mp3,mp3float,mp3adufloat,mp2,mp2float,mp1,mp1float,aac,ac3,eac3,dca,truehd,wmav1,wmav2,wmapro,wmalossless,pcm_s16be,pcm_s16le,pcm_s24be,pcm_f32be
PARSERS=h264,hevc,mpegvideo,mpeg4video,vc1,aac,ac3,mpegaudio,dca,mlp
COMMON=(--disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect
    --disable-gpl --disable-nonfree --disable-version3 --disable-avformat --disable-avdevice --disable-avfilter
    --disable-postproc --enable-avcodec --enable-avutil --enable-swscale --enable-swresample
    --enable-static --disable-shared --enable-pic --disable-debug
    "--enable-decoder=$DECODERS" "--enable-parser=$PARSERS")
# H.264 on the media engine through VideoToolbox (D-254): FFmpeg still parses and orders the pictures, the hardware
# decodes the slices. Apple's frameworks, linked by whatever links these libraries: VideoToolbox, CoreMedia, CoreVideo
# and CoreFoundation. The Apple builds only.
APPLE_HW=(--enable-videotoolbox --enable-hwaccel=h264_videotoolbox)
# The tests' encoders (D-577; MPEG-4 part 2 and msmpeg4v3 for DivX and DivX 3.11, D-588; E-AC-3, DTS, TrueHD and WMA 1
# and 2 for the audio cores of D-620, DTS's and TrueHD's being FFmpeg's experimental ones), for the <target>-tests
# builds only; set per build in EXTRA.
TEST_ENCODERS=mp2,ac3,aac,mpeg4,msmpeg4v3,eac3,dca,truehd,wmav1,wmav2
EXTRA=()

# Configures (again only when the arguments change), builds and installs one target: name, then configure's arguments.
make_target() {
    local name="$1"
    shift
    local prefix="$OUT/$name" work="$OUT/work-$name"
    local stamp="$work/configured-with"
    mkdir -p "$work"
    if [ ! -f "$stamp" ] || [ "$(cat "$stamp")" != "$*" ]; then
        say "configuring FFmpeg for $name"
        (cd "$work" && "$SRC/configure" "$@" > configure.log 2>&1) || { tail -30 "$work/configure.log" >&2; exit 1; }
        echo "$*" > "$stamp"
    fi
    say "building FFmpeg for $name"
    make -C "$work" -j"$JOBS" > "$work/make.log" 2>&1 || { tail -30 "$work/make.log" >&2; exit 1; }
    make -C "$work" install > "$work/install.log" 2>&1
    say "$prefix: $(ls "$prefix"/lib/*.a | xargs -n1 basename | tr '\n' ' ')"
}

build() { # name sdk cflags...
    local name="$1" sdk="$2"
    shift 2
    local prefix="$OUT/$name"
    local sysroot cc
    sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)"
    cc="$(xcrun --sdk "$sdk" -f clang)"
    # The tools configure builds and runs here are the Mac's: its own compiler and SDK.
    local host_sdk
    host_sdk="$(xcrun --sdk macosx --show-sdk-path)"
    local args=("${COMMON[@]}" "${APPLE_HW[@]}" ${EXTRA[@]+"${EXTRA[@]}"} --prefix="$prefix" --cc="$cc" --sysroot="$sysroot"
        --arch=aarch64 --host-cc="$(xcrun --sdk macosx -f clang)" --host-cflags="-isysroot $host_sdk" --host-ldflags="-isysroot $host_sdk"
        --extra-cflags="$*" --extra-ldflags="$*")
    [ "$name" = ios ] && args+=(--enable-cross-compile --target-os=darwin)
    make_target "$name" "${args[@]}"
}

# A Linux host (D-576): its own architecture, its clang (CC overrides), no VideoToolbox. FFmpeg's x86 assembly needs
# nasm; without it the same decoders build from their C code.
build_linux() { # name
    local name="$1"
    local args=("${COMMON[@]}" ${EXTRA[@]+"${EXTRA[@]}"} --prefix="$OUT/$name" --cc="${CC:-clang}")
    case "$(uname -m)" in
        x86_64|i?86) command -v nasm > /dev/null || { say "no nasm: FFmpeg's C code only"; args+=(--disable-x86asm); } ;;
    esac
    make_target "$name" "${args[@]}"
}

# A host's builds: its decoders, then the tests' with the encoders.
build_mac() {
    EXTRA=()
    build macos macosx -arch arm64 -mmacosx-version-min=13.0
    EXTRA=("--enable-encoder=$TEST_ENCODERS")
    build macos-tests macosx -arch arm64 -mmacosx-version-min=13.0
    EXTRA=()
}
build_host_linux() {
    EXTRA=()
    build_linux linux
    EXTRA=("--enable-encoder=$TEST_ENCODERS")
    build_linux linux-tests
    EXTRA=()
}

case "$TARGETS" in
    linux) build_host_linux ;;
    macos) build_mac ;;
    ios) build ios iphoneos -arch arm64 -mios-version-min=17.0 ;;
    all)
        build_mac
        build ios iphoneos -arch arm64 -mios-version-min=17.0
        ;;
esac
