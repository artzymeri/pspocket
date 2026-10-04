#!/usr/bin/env bash
# One bring-up round on the Mac (D-033, D-034, D-043): update the checkout, build pspocket-boot, run the game's
# decrypted EBOOT under it with its disc image as /dev_bdvd, and print the end of the log (the gaps it hit). The full
# log goes to inventory/boot-NNN.log, which is gitignored (it comes from game code). Works with macOS's bash 3.2.
#
#   scripts/boot.sh                    first inventory/*/decrypt/EBOOT.elf, 60 s
#   scripts/boot.sh path/to/EBOOT.elf  [more pspocket-boot options, e.g. --timeout 120 --stop-at-first]
#
# The disc image, read in place: $PSPOCKET_ISO, else a PS3 game .iso in the game's inventory folder
# (inventory/<game>/), on the Desktop or in Downloads, else the one the app holds on the connected iPhone, copied once
# to inventory/<game>/disc.iso (scripts/ios.sh pull-iso). The game's /dev_hdd0 (game data, saves) is
# inventory/<game>/hdd0. /dev_flash is inventory/firmware/dev_flash when it exists (the user's own firmware, D-067).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$ROOT"

git pull --ff-only --quiet || echo "boot.sh: git pull failed (local changes?); building what is checked out" >&2
cmake -S . -B build/host -G Ninja -DPSPOCKET_WERROR=ON > /dev/null
ninja -C build/host pspocket-boot | grep -v '^ninja: ' || true
BOOT=build/host/runtime/loader/pspocket-boot

ELF=""
if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then
    ELF="$1"
    shift
else
    ELF="$(find inventory -name EBOOT.elf -path '*/decrypt/*' 2>/dev/null | head -1)"
fi
if [ -z "$ELF" ] || [ ! -f "$ELF" ]; then
    echo "boot.sh: no decrypted EBOOT.elf found under inventory/*/decrypt/ (pass its path)" >&2
    exit 66
fi
GAME_DIR="$(dirname "$(dirname "$ELF")")" # inventory/gta5 for inventory/gta5/decrypt/EBOOT.elf

# The first candidate that reads as a PS3 game disc.
find_iso() {
    local f
    if [ -n "${PSPOCKET_ISO:-}" ]; then
        echo "$PSPOCKET_ISO"
        return
    fi
    {
        find "$GAME_DIR" -maxdepth 2 -type f -iname '*.iso' 2>/dev/null || true
        find "$HOME/Desktop" "$HOME/Downloads" -maxdepth 3 -type f -iname '*.iso' -size +1G 2>/dev/null || true
    } | while IFS= read -r f; do
        if "$BOOT" --probe-disc "$f" > /dev/null 2>&1; then
            echo "$f"
            break
        fi
    done
}

ISO="$(find_iso || true)" # the search stops at the first match, which can end it with SIGPIPE
if [ -z "$ISO" ]; then
    echo "boot.sh: no disc image on this Mac; copying the one in the PSPocket app on your iPhone (first time only)" >&2
    if scripts/ios.sh pull-iso --to "$GAME_DIR"; then
        ISO="$GAME_DIR/disc.iso"
    else
        echo "boot.sh: no disc image, so the game can't read its files. Connect and unlock the iPhone (with the" >&2
        echo "         .iso in Files > On My iPhone > PSPocket), or run: PSPOCKET_ISO=/path/to/game.iso scripts/boot.sh" >&2
    fi
fi
DISC=()
if [ -n "$ISO" ]; then
    echo "boot.sh: disc image $ISO ($("$BOOT" --probe-disc "$ISO" 2>&1 || true))"
    DISC=(--bdvd "$ISO")
fi
mkdir -p "$GAME_DIR/hdd0/game"
# /dev_flash: the system files a game reads itself, from the user's own firmware (D-067), when they have been taken
# out of it: inventory/firmware/tools/fw_unself.py --raw flashMP3.pic writes inventory/firmware/dev_flash.
FLASH=()
if [ -d inventory/firmware/dev_flash ]; then
    FLASH=(--flash inventory/firmware/dev_flash)
fi

n=1
while [ -e "$(printf 'inventory/boot-%03d.log' "$n")" ]; do n=$((n + 1)); done
LOG="$(printf 'inventory/boot-%03d.log' "$n")"

echo "boot.sh: $ELF -> $LOG"
status=0
"$BOOT" "$ELF" --timeout 60 --hdd0 "$GAME_DIR/hdd0" ${DISC[@]+"${DISC[@]}"} ${FLASH[@]+"${FLASH[@]}"} "$@" > "$LOG" 2>&1 ||
    status=$?
# The gaps and the end of the run; with the thread report (D-053) when there is one, which is longer than a screen.
if grep -q '^==== thread report' "$LOG"; then
    grep -E 'UNIMPLEMENTED|\[fatal\]' "$LOG" | tail -10 || true
    sed -n '/^==== thread report/,/^==== end of thread report/p' "$LOG"
    sed -n '/^==== end of thread report/,$p' "$LOG" | tail -n +2
else
    tail -40 "$LOG"
fi
echo
echo "boot.sh: exit status $status; full log in $LOG"
