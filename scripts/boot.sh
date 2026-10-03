#!/usr/bin/env bash
# One bring-up round on the Mac (D-033, D-034): update the checkout, build pspocket-boot, run the game's decrypted
# EBOOT under it, and print the end of the log (the gaps it hit). The full log goes to inventory/boot-NNN.log, which
# is gitignored (it comes from game code). Works with macOS's bash 3.2.
#
#   scripts/boot.sh                    first inventory/*/decrypt/EBOOT.elf, 60 s
#   scripts/boot.sh path/to/EBOOT.elf  [more pspocket-boot options, e.g. --timeout 120 --stop-at-first]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$ROOT"

git pull --ff-only --quiet || echo "boot.sh: git pull failed (local changes?); building what is checked out" >&2
cmake -S . -B build/host -G Ninja -DPSPOCKET_WERROR=ON > /dev/null
ninja -C build/host pspocket-boot | grep -v '^ninja: ' || true

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

n=1
while [ -e "$(printf 'inventory/boot-%03d.log' "$n")" ]; do n=$((n + 1)); done
LOG="$(printf 'inventory/boot-%03d.log' "$n")"

echo "boot.sh: $ELF -> $LOG"
status=0
build/host/runtime/loader/pspocket-boot "$ELF" --timeout 60 "$@" > "$LOG" 2>&1 || status=$?
tail -40 "$LOG"
echo
echo "boot.sh: exit status $status; full log in $LOG"
