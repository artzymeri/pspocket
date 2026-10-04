#!/usr/bin/env bash
# Lifts the SPU programs a run saved (pspocket-boot --spu-dump DUMP) into C for -DPSPOCKET_GAME_SPU=OUT (D-076).
#
#   scripts/lift-spu.sh DUMP OUT [LOG...]
#
# DUMP holds spu_<hash>_<lsaddr>.bin (the code, from the user's game) and .txt (kind, guest address, LS base, size,
# entry, and for modules "seed" lines: addresses execution entered them at). Every image is lifted at its LS base with
# its entry and seeds; the logs of earlier runs with lifted programs add the addresses where execution left lifted code
# ("no lifted code at ...") as seeds too, so the next lift covers them. OUT gets
# one directory per program and game_spu.c, whose pspocket_game_spu_register() registers them all. DUMP and OUT hold
# code derived from the game: keep them in the gitignored inventory/ or build/ (CLAUDE.md rule 1).
#
# Works with macOS's bash 3.2.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
LIFT="${PSPOCKET_SPU_LIFT:-$ROOT/build/host/recomp/spu/spu-lift}"

[ $# -ge 2 ] || { echo "usage: scripts/lift-spu.sh DUMP OUT [LOG...]" >&2; exit 64; }
DUMP="$1"; OUT="$2"; shift 2
[ -x "$LIFT" ] || { echo "error: no spu-lift at $LIFT (ninja -C build/host spu-lift, or set PSPOCKET_SPU_LIFT)" >&2; exit 1; }
[ -d "$DUMP" ] || { echo "error: no dump directory $DUMP" >&2; exit 1; }
mkdir -p "$OUT"

# Seeds from the logs: "no lifted code at 0xADDR in program NAME".
SEEDS="$(mktemp)"
trap 'rm -f "$SEEDS"' EXIT
if [ $# -gt 0 ]; then
    sed -n 's/.*no lifted code at \(0x[0-9a-fA-F]*\) in program \(spu_[0-9a-f]*_[0-9a-f]*\).*/\2 \1/p' "$@" \
        | sort -u > "$SEEDS" || true
fi

value() { sed -n "s/^$1 //p" "$2"; }

count=0; failed=0
REG="$OUT/game_spu.c.tmp"
{
    echo "/* The game's lifted SPU programs (scripts/lift-spu.sh, D-076). Generated: do not edit or commit. */"
    echo
} > "$REG"
DECLS=""; CALLS=""
for bin in "$DUMP"/spu_*.bin; do
    [ -e "$bin" ] || continue
    name="$(basename "$bin" .bin)"
    meta="$DUMP/$name.txt"
    [ -f "$meta" ] || { echo "warning: $name has no $name.txt, skipped" >&2; continue; }
    base="$(value base "$meta")"; entry="$(value entry "$meta" | head -1)"; size="$(value size "$meta")"
    args=(--image "$bin" --base "$base" --entry "$entry" --name "$name" --out "$OUT/$name" --quiet)
    # Seeds inside the image's code only: execution also leaves lifted code for code outside it (the SPURS syscall
    # stubs, modules a program loads), which spu-lift refuses as seeds.
    add_seed() { if [ $(($1)) -ge $((base)) ] && [ $(($1)) -lt $((base + size)) ]; then args+=(--seed "$1"); fi; }
    while read -r prog seed; do
        [ "$prog" = "$name" ] && add_seed "$seed"
    done < "$SEEDS"
    # Seeds the run saved with the image (modules, D-079: addresses execution entered it at).
    for seed in $(value seed "$meta" | sort -u); do add_seed "$seed"; done
    if "$LIFT" "${args[@]}" > "$OUT/$name.log" 2>&1; then
        DECLS="${DECLS}void ${name}_register(void);"$'\n'
        CALLS="${CALLS}    ${name}_register();"$'\n'
        count=$((count + 1))
    else
        echo "warning: spu-lift failed for $name (see $OUT/$name.log); it stays interpreted" >&2
        rm -rf "${OUT:?}/$name"
        failed=$((failed + 1))
    fi
done
{
    printf '%s\n' "$DECLS"
    echo "void pspocket_game_spu_register(void);"
    echo "void pspocket_game_spu_register(void)"
    echo "{"
    printf '%s' "$CALLS"
    echo "}"
} >> "$REG"
mv "$REG" "$OUT/game_spu.c"
echo "lifted $count SPU programs into $OUT ($failed failed); seeds: $(wc -l < "$SEEDS" | tr -d ' ')"
