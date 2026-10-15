#!/usr/bin/env bash
# One command per game (D-090, D-101): the disc image in, the game's PPU and SPU code translated ahead of time and
# built into pspocket-boot (and with --ios into the iOS app) out.
#
#   scripts/translate-game.sh --iso GAME.iso [--elf EBOOT.elf] [--seconds S] [--press BUTTON@S]... [--rounds N] [--ios]
#
# The disc's PS3_GAME/USRDIR/EBOOT.BIN is decrypted with the keys the build carries (ps3disc --decrypt, D-121; read
# from RPCS3's source at build time, never committed: CLAUDE.md rule 1). --elf takes an executable decrypted elsewhere
# instead.
#
# 1. The title ID from the disc image names inventory/<TITLE_ID>/, where everything derived from the game stays
#    (gitignored): the executable, the translated code, the SPU programs a run saved, the logs and /dev_hdd0.
# 2. The PPU code: ppu-lift --elf, every function of the executable (D-092, D-101).
# 3. pspocket-boot built with it, then one run of --seconds (default 300) with --spu-dump: the SPU programs SPURS and
#    the game's tasks load are saved, with the addresses where execution left lifted code.
# 4. The SPU programs: scripts/lift-spu.sh (D-076, D-099), and pspocket-boot rebuilt with them. The runs also record
#    the game's shaders in inventory/<TITLE_ID>/shaders (--shader-cache, D-114), which the app makes while it loads.
# 5. With --ios: scripts/ios.sh build with both (then scripts/ios.sh install with the phone connected; the app
#    decrypts the disc's executable itself, D-121).
#
# --rounds N repeats steps 3 and 4 up to N times (default 1), until a run leaves no SPU code interpreted: each run's
# misses and saved programs seed the next lift (GTA V's first mission took five rounds, run 080). Run the script again
# to cover more of the game (other --press scripts reach other parts); the seeds accumulate. Code the translation has
# not reached runs in the interpreters meanwhile, so the game runs either way.
#
# Works with macOS's bash 3.2.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD="${PSPOCKET_HOST_BUILD:-$ROOT/build/host}"
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

say() { printf '==> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
usage() {
    sed -n '2,/^# Works with/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 64
}

ISO="" ELF="" SECONDS_RUN=300 IOS=0 ROUNDS=1
PRESSES=()
while [ $# -gt 0 ]; do
    case "$1" in
        --iso) ISO="${2:?}"; shift 2 ;;
        --elf) ELF="${2:?}"; shift 2 ;;
        --seconds) SECONDS_RUN="${2:?}"; shift 2 ;;
        --press) PRESSES+=(--press "${2:?}"); shift 2 ;;
        --rounds) ROUNDS="${2:?}"; shift 2 ;;
        --ios) IOS=1; shift ;;
        -h|--help) usage ;;
        *) die "unknown option $1 (see --help)" ;;
    esac
done
[ -n "$ISO" ] || usage
case "$ROUNDS" in ''|*[!0-9]*|0) die "--rounds takes a positive number" ;; esac
[ -f "$ISO" ] || die "no disc image $ISO"
if [ -n "$ELF" ]; then
    [ -f "$ELF" ] || die "no executable $ELF"
    if [ "$(head -c 3 "$ELF")" = "SCE" ]; then
        die "$ELF is still an encrypted SELF: leave out --elf and the disc's EBOOT.BIN is decrypted here (D-121)"
    fi
fi

# The host tools: pspocket-boot reads the disc image, ppu-lift and spu-lift translate.
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DPSPOCKET_WERROR=ON --log-level=WARNING >&2
fi
ninja -C "$BUILD" pspocket-boot ppu-lift spu-lift ps3disc >&2

# 1. The title.
probe="$("$BUILD/runtime/loader/pspocket-boot" --probe-disc "$ISO" 2>/dev/null | tail -1)" ||
    die "$ISO is not a readable PS3 game disc"
TITLE="${probe%% *}"
[ -n "$TITLE" ] || die "no title ID in $ISO"
GAME="$ROOT/inventory/$TITLE"
mkdir -p "$GAME/hdd0"
say "$probe: $GAME"
if [ -z "$ELF" ]; then
    # The executable, decrypted from the disc with the keys the build carries (D-121).
    ELF="$GAME/decrypt/EBOOT.elf"
    "$BUILD/runtime/disc/ps3disc" --decrypt "$ISO" "$ELF" >/dev/null || die "could not decrypt $ISO's EBOOT.BIN"
    say "decrypted the disc's EBOOT.BIN: $ELF"
fi
if [ ! -f "$GAME/EBOOT.elf" ] || ! cmp -s "$ELF" "$GAME/EBOOT.elf"; then cp "$ELF" "$GAME/EBOOT.elf"; fi

# 2. The PPU code.
say "translating the PPU code (ppu-lift --elf)"
"$BUILD/recomp/ppu/ppu-lift" --elf "$GAME/EBOOT.elf" --out "$GAME/ppu-lifted" --tu 512 --name game_ppu \
    --prefix game_ppu_ --no-comments > "$GAME/ppu-lift.log"
head -2 "$GAME/ppu-lift.log" >&2

configure() {
    local spu=()
    if [ -f "$GAME/spu-lifted/game_spu.c" ]; then spu=(-DPSPOCKET_GAME_SPU="$GAME/spu-lifted"); else spu=(-DPSPOCKET_GAME_SPU=); fi
    cmake -S "$ROOT" -B "$BUILD" -DPSPOCKET_GAME_PPU="$GAME/ppu-lifted" "${spu[@]}" --log-level=WARNING >&2
    say "building pspocket-boot with the translated code (a few minutes the first time)"
    ninja -C "$BUILD" pspocket-boot >&2
}

# 3 and 4, once per round: a run that saves the SPU programs, then their translation from every run so far.
configure
# /dev_flash: the installed firmware's whole dev_flash when there is one (a game reads system files of its own: D-998),
# else the files taken out of it.
flash=()
if [ -d "$ROOT/inventory/firmware/installed/dev_flash" ]; then flash=(--flash "$ROOT/inventory/firmware/installed/dev_flash")
elif [ -d "$ROOT/inventory/firmware/dev_flash" ]; then flash=(--flash "$ROOT/inventory/firmware/dev_flash"); fi
round=1
while :; do
    n=1
    while [ -e "$GAME/run-$(printf %03d $n).log" ]; do n=$((n + 1)); done
    LOG="$GAME/run-$(printf %03d $n).log"
    say "round $round of $ROUNDS: running the game for $SECONDS_RUN s to collect its SPU programs: $LOG"
    "$BUILD/runtime/loader/pspocket-boot" "$GAME/EBOOT.elf" --timeout "$SECONDS_RUN" --gpu metal --rsx-recover --bdvd "$ISO" \
        --hdd0 "$GAME/hdd0" ${flash[@]+"${flash[@]}"} --spu-dump "$GAME/spu" --shader-cache "$GAME/shaders" \
        ${PRESSES[@]+"${PRESSES[@]}"} > "$LOG" 2>&1 || true
    grep -E "fps|SPU program loads|unimplemented (syscalls|functions) called" "$LOG" | tail -4 >&2 || true
    # What the translation did not cover: code the SPU interpreter ran, images without a lifted program.
    interpreted="$(grep -c '^SPU interpreted:' "$LOG" || true)"
    unknown="$(sed -n 's/.*interpreted (\([0-9]*\) images without a lifted program).*/\1/p' "$LOG" | tail -1)"
    say "round $round: ${interpreted:-0} places SPU code was interpreted, ${unknown:-0} images without a lifted program"

    # 4. The SPU programs, from this run and the earlier ones.
    if ls "$GAME/spu"/spu_*.bin > /dev/null 2>&1; then
        if [ "${interpreted:-0}" = 0 ] && [ "${unknown:-0}" = 0 ] && [ -f "$GAME/spu-lifted/game_spu.c" ]; then
            say "every SPU program this run loaded ran translated: nothing new to lift"
            break
        fi
        say "translating the SPU programs (scripts/lift-spu.sh)"
        logs=("$GAME"/run-*.log)
        "$ROOT/scripts/lift-spu.sh" "$GAME/spu" "$GAME/spu-lifted" "${logs[@]}" >&2
        configure
    else
        say "the run saved no SPU programs (the game did not start SPURS work in $SECONDS_RUN s)"
        break
    fi
    [ "$round" -lt "$ROUNDS" ] || break
    round=$((round + 1))
done

# 5. The iOS app.
if [ "$IOS" = 1 ]; then
    say "building the iOS app with the translated code"
    PSPOCKET_GAME_PPU="$GAME/ppu-lifted" PSPOCKET_GAME_SPU="$GAME/spu-lifted" "$ROOT/scripts/ios.sh" build
fi

say "done: $BUILD/runtime/loader/pspocket-boot $GAME/EBOOT.elf --bdvd $ISO --hdd0 $GAME/hdd0 runs $TITLE translated"
